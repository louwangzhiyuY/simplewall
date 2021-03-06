// simplewall
// Copyright (c) 2016, 2017 Henry++

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windns.h>
#include <mstcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <subauth.h>
#include <fwpmu.h>
#include <dbt.h>
#include <aclapi.h>
#include <wtsapi32.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <sddl.h>
#include <ws2tcpip.h>

#include "main.hpp"
#include "rapp.hpp"
#include "routine.hpp"

#include "pugixml\pugixml.hpp"

#include "resource.hpp"

CONST UINT WM_FINDMSGSTRING = RegisterWindowMessage (FINDMSGSTRING);

rapp app (APP_NAME, APP_NAME_SHORT, APP_VERSION, APP_COPYRIGHT);

std::unordered_map<size_t, ITEM_APPLICATION> apps;
std::unordered_map<size_t, std::vector<size_t>> apps_rules;

std::unordered_map<rstring, bool, rstring::hash, rstring::is_equal> rules_config;

std::vector<ITEM_COLOR> colors;
std::vector<ITEM_PROCESS> processes;
std::vector<ITEM_PROTOCOL> protocols;

std::vector<ITEM_RULE*> rules_blocklist;
std::vector<ITEM_RULE*> rules_system;
std::vector<ITEM_RULE*> rules_custom;

std::vector<ITEM_LOG*> notifications;

STATIC_DATA config;

FWPM_SESSION session;

FWPM_NET_EVENT_SUBSCRIPTION subscription;
FWPM_NET_EVENT_ENUM_TEMPLATE enum_template;

EXTERN_C const IID IID_IImageList;

BOOL _wfp_initialize (BOOL is_full);
VOID _wfp_uninitialize ();

BOOL initializer_callback (HWND hwnd, DWORD msg, LPVOID, LPVOID);
LRESULT CALLBACK NotificationProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

#define LANG_MENU 5

VOID _app_logerror (LPCWSTR fn, DWORD result, LPCWSTR desc, BOOL is_nopopups = FALSE)
{
	_r_dbg_write (APP_NAME_SHORT, APP_VERSION, fn, result, desc);

	if (!is_nopopups && app.ConfigGet (L"IsErrorNotificationsEnabled", TRUE).AsBool ()) // check for timeout (sec.)
	{
		config.is_popuperrors = TRUE;

		app.TrayPopup (NIIF_ERROR | (app.ConfigGet (L"IsNotificationsSilent", FALSE).AsBool () ? NIIF_NOSOUND : 0), APP_NAME, I18N (&app, IDS_STATUS_ERROR, 0));
	}
}

DWORD _mps_changeconfig (BOOL is_stop)
{
	DWORD result = 0;
	BOOL is_started = FALSE;

	SC_HANDLE scm = OpenSCManager (nullptr, nullptr, SC_MANAGER_ALL_ACCESS);

	if (!scm)
	{
		_app_logerror (L"OpenSCManager", GetLastError (), nullptr);
	}
	else
	{
		LPCWSTR arr[] = {L"mpssvc", L"mpsdrv"};

		for (INT i = 0; i < _countof (arr); i++)
		{
			SC_HANDLE sc = OpenService (scm, arr[i], SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP);

			if (!sc)
			{
				_app_logerror (L"OpenService", GetLastError (), arr[i]);
			}
			else
			{
				if (!is_started)
				{
					SERVICE_STATUS status;

					if (QueryServiceStatus (sc, &status))
					{
						is_started = (status.dwCurrentState == SERVICE_RUNNING);
					}
				}

				if (!ChangeServiceConfig (sc, SERVICE_NO_CHANGE, is_stop ? SERVICE_DISABLED : SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
				{
					_app_logerror (L"ChangeServiceConfig", GetLastError (), arr[i]);
				}

				CloseServiceHandle (sc);
			}
		}

		// start services
		if (is_stop)
		{
			if (is_started)
				_r_run (nullptr, L"netsh advfirewall set allprofiles state off", nullptr, SW_HIDE);
		}
		else
		{
			for (INT i = 0; i < _countof (arr); i++)
			{
				SC_HANDLE sc = OpenService (scm, arr[i], SERVICE_QUERY_STATUS | SERVICE_START);

				if (!sc)
				{
					_app_logerror (L"OpenService", GetLastError (), arr[i]);
				}
				else
				{
					DWORD dwBytesNeeded = 0;
					SERVICE_STATUS_PROCESS ssp = {0};

					if (!QueryServiceStatusEx (sc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof (ssp), &dwBytesNeeded))
					{
						_app_logerror (L"QueryServiceStatusEx", GetLastError (), arr[i]);
					}
					else
					{
						if (ssp.dwCurrentState != SERVICE_RUNNING)
						{
							if (!StartService (sc, 0, nullptr))
							{
								_app_logerror (L"StartService", GetLastError (), arr[i]);
							}
						}

						CloseServiceHandle (sc);
					}
				}
			}

			_r_sleep (250);

			_r_run (nullptr, L"cmd /c netsh advfirewall set allprofiles state on & netsh advfirewall set allprofiles firewallpolicy blockinbound,allowoutbound", nullptr, SW_HIDE);
		}

		CloseServiceHandle (scm);
	}

	return result;
}

VOID _app_listviewresize (HWND hwnd, INT width)
{
	if (!app.ConfigGet (L"AutoSizeColumns", TRUE).AsBool ())
		return;

	if (!width)
	{
		RECT rc = {0};
		GetClientRect (GetDlgItem (hwnd, IDC_LISTVIEW), &rc);

		width = rc.right;
	}

	INT cx1 = _R_PERCENT_VAL (74, width);
	INT cx2 = _R_PERCENT_VAL (26, width);

	_r_listview_setcolumn (hwnd, IDC_LISTVIEW, 0, nullptr, cx1);
	_r_listview_setcolumn (hwnd, IDC_LISTVIEW, 1, nullptr, cx2);
}

VOID _app_seticonsize (HWND hwnd)
{
	HIMAGELIST h = nullptr;

	const BOOL is_large = app.ConfigGet (L"IsLargeIcons", FALSE).AsBool ();
	const BOOL is_iconshidden = app.ConfigGet (L"IsIconsHidden", FALSE).AsBool ();

	SHGetImageList (is_large ? SHIL_LARGE : SHIL_SMALL, IID_IImageList, (LPVOID*)&h);

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)h);
	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)h);

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SCROLL, 0, GetScrollPos (GetDlgItem (hwnd, IDC_LISTVIEW), SB_VERT)); // scroll-resize-HACK!!!

	CheckMenuRadioItem (GetMenu (hwnd), IDM_ICONSSMALL, IDM_ICONSLARGE, (is_large ? IDM_ICONSLARGE : IDM_ICONSSMALL), MF_BYCOMMAND);
	CheckMenuItem (GetMenu (hwnd), IDM_ICONSISHIDDEN, MF_BYCOMMAND | (is_iconshidden ? MF_CHECKED : MF_UNCHECKED));

	_app_listviewresize (hwnd, 0);
}

VOID _app_setfont (HWND)
{
	//LOGFONT lf = {0};
	//HFONT hFont = (HFONT)GetStockObject (DEFAULT_GUI_FONT);// (HFONT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, WM_GETFONT, 0, 0);
	//GetObject (hFont, sizeof (lf), &lf);
	//lf.lfHeight = 14;

	//hFont = CreateFontIndirect (&lf);
	//if (hFont != NULL)
	//	SendDlgItemMessage (hwnd, IDC_LISTVIEW, WM_SETFONT, (WPARAM)hFont, TRUE);
}

VOID ShowItem (HWND hwnd, UINT ctrl_id, size_t item, INT scroll_pos)
{
	if (item != LAST_VALUE)
	{
		ListView_SetItemState (GetDlgItem (hwnd, ctrl_id), -1, 0, LVIS_SELECTED); // deselect all
		ListView_SetItemState (GetDlgItem (hwnd, ctrl_id), item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); // select item

		if (scroll_pos == -1)
			SendDlgItemMessage (hwnd, ctrl_id, LVM_ENSUREVISIBLE, item, TRUE); // ensure item visible
	}

	if (scroll_pos != -1)
		SendDlgItemMessage (hwnd, ctrl_id, LVM_SCROLL, 0, scroll_pos); // restore vscroll position
}

VOID _app_refreshstatus (HWND hwnd, BOOL first_part, BOOL second_part)
{
	if (first_part)
		_r_status_settext (hwnd, IDC_STATUSBAR, 0, _r_fmt (I18N (&app, IDS_STATUS_TOTAL, 0), apps.size ()));

	if (second_part)
	{
		switch (app.ConfigGet (L"Mode", ModeWhitelist).AsUint ())
		{
			case ModeWhitelist:
			{
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 0, I18N (&app, IDS_GROUP_ALLOWED, 0), nullptr);
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 1, I18N (&app, IDS_GROUP_BLOCKED, 0), nullptr);

				break;
			}

			case ModeBlacklist:
			{
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 0, I18N (&app, IDS_GROUP_BLOCKED, 0), nullptr);
				_r_listview_setgroup (hwnd, IDC_LISTVIEW, 1, I18N (&app, IDS_GROUP_ALLOWED, 0), nullptr);

				break;
			}
		}
	}
}

rstring _app_getversion (LPCWSTR path)
{
	rstring result;

	HINSTANCE h = LoadLibraryEx (path, nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);

	if (h)
	{
		HRSRC hv = FindResource (h, MAKEINTRESOURCE (VS_VERSION_INFO), RT_VERSION);

		if (hv)
		{
			HGLOBAL hg = LoadResource (h, hv);

			if (hg)
			{
				LPVOID versionInfo = LockResource (hg);

				if (versionInfo)
				{
					UINT vLen = 0, langD = 0;
					LPVOID retbuf = nullptr;

					WCHAR author_entry[128] = {0};
					WCHAR description_entry[128] = {0};
					WCHAR version_entry[128] = {0};

					if (VerQueryValue (versionInfo, L"\\VarFileInfo\\Translation", &retbuf, &vLen) && vLen == 4)
					{
						memcpy_s (&langD, 4, retbuf, 4);
						StringCchPrintf (author_entry, _countof (author_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\CompanyName", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
						StringCchPrintf (description_entry, _countof (description_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\FileDescription", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
						StringCchPrintf (version_entry, _countof (version_entry), L"\\StringFileInfo\\%02X%02X%02X%02X\\FileVersion", (langD & 0xff00) >> 8, langD & 0xff, (langD & 0xff000000) >> 24, (langD & 0xff0000) >> 16);
					}
					else
					{
						StringCchPrintf (author_entry, _countof (author_entry), L"\\StringFileInfo\\%04X04B0\\CompanyName", GetUserDefaultLangID ());
						StringCchPrintf (description_entry, _countof (description_entry), L"\\StringFileInfo\\%04X04B0\\FileDescription", GetUserDefaultLangID ());
						StringCchPrintf (version_entry, _countof (version_entry), L"\\StringFileInfo\\%04X04B0\\FileVersion", GetUserDefaultLangID ());
					}

					if (VerQueryValue (versionInfo, description_entry, &retbuf, &vLen))
					{
						result.Append (TAB_SPACE);
						result.Append (static_cast<LPCWSTR>(retbuf));

						UINT length = 0;
						VS_FIXEDFILEINFO* verInfo = nullptr;

						if (VerQueryValue (versionInfo, L"\\", reinterpret_cast<LPVOID*>(&verInfo), &length))
						{
							result.Append (_r_fmt (L" %d.%d", HIWORD (verInfo->dwProductVersionMS), LOWORD (verInfo->dwProductVersionMS)));

							if (HIWORD (verInfo->dwProductVersionLS) || LOWORD (verInfo->dwProductVersionLS))
							{
								result.Append (_r_fmt (L".%d", HIWORD (verInfo->dwProductVersionLS)));

								if (LOWORD (verInfo->dwProductVersionLS))
									result.Append (_r_fmt (L".%d", LOWORD (verInfo->dwProductVersionLS)));
							}
						}

						result.Append (L"\r\n");
					}

					if (VerQueryValue (versionInfo, author_entry, &retbuf, &vLen))
					{
						result.Append (TAB_SPACE);
						result.Append (static_cast<LPCWSTR>(retbuf));
						result.Append (L"\r\n");
					}

					result.Trim (L"\r\n ");
				}
			}

			UnlockResource (hg);
			FreeResource (hg);
		}

		FreeLibrary (h);
	}

	return result;
}

size_t _app_geticonid (LPCWSTR path)
{
	size_t result = config.def_icon_id;

	if (!app.ConfigGet (L"IsIconsHidden", FALSE).AsBool ())
	{
		SHFILEINFO shfi = {0};

		CoInitialize (nullptr);

		if (SHGetFileInfo (path, 0, &shfi, sizeof (shfi), SHGFI_SYSICONINDEX))
		{
			result = shfi.iIcon;
		}
		else
		{
			result = config.def_icon_id;
		}

		CoUninitialize ();
	}

	return result;
}

size_t _app_getposition (HWND hwnd, size_t hash)
{
	for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_LISTVIEW); i++)
	{
		if ((size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i) == hash)
			return i;
	}

	return LAST_VALUE;
}

rstring _app_getshortcutpath (HWND hwnd, LPCWSTR path)
{
	rstring result;

	IShellLink* psl = nullptr;

	CoInitializeEx (nullptr, COINIT_MULTITHREADED);
	CoInitializeSecurity (nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, 0, nullptr);

	if (SUCCEEDED (CoCreateInstance (CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl)))
	{
		IPersistFile* ppf = nullptr;

		if (SUCCEEDED (psl->QueryInterface (IID_IPersistFile, (LPVOID*)&ppf)))
		{
			if (SUCCEEDED (ppf->Load (path, STGM_READ)))
			{
				if (SUCCEEDED (psl->Resolve (hwnd, 0)))
				{
					WIN32_FIND_DATA wfd = {0};
					WCHAR buffer[MAX_PATH] = {0};

					if (SUCCEEDED (psl->GetPath (buffer, _countof (buffer), (WIN32_FIND_DATA*)&wfd, SLGP_RAWPATH)))
					{
						result = buffer;
					}
				}
			}

			ppf->Release ();
		}

		psl->Release ();
	}

	CoUninitialize ();

	return result;
}

size_t _app_addapplication (HWND hwnd, LPCWSTR path, __time64_t timestamp, bool is_silent, bool is_checked)
{
	if (!path)
		return 0;

	// if file is shortcut - get location
	{
		const size_t len = wcslen (path);

		if (len > 4 && _wcsnicmp (path + (len - 4), L".lnk", 4) == 0)
		{
			path = _app_getshortcutpath (hwnd, path);
		}
	}

	const size_t hash = _r_str_hash (path);

	if (apps.find (hash) != apps.end ())
		return 0; // already exists

	ITEM_APPLICATION* ptr = &apps[hash]; // application pointer;

	const BOOL is_ntoskrnl = (hash == config.ntoskrnl_hash);

	StringCchCopy (ptr->display_path, _countof (ptr->display_path), path);
	StringCchCopy (ptr->real_path, _countof (ptr->real_path), is_ntoskrnl ? _r_path_expand (PATH_NTOSKRNL) : path);
	StringCchCopy (ptr->file_dir, _countof (ptr->file_dir), path);
	StringCchCopy (ptr->file_name, _countof (ptr->file_name), _r_path_extractfile (path));

	PathRemoveFileSpec (ptr->file_dir);

	const DWORD dwAttr = GetFileAttributes (ptr->real_path);

	ptr->is_enabled = is_checked;
	ptr->is_silent = is_silent;
	ptr->is_system = is_ntoskrnl || (((dwAttr != INVALID_FILE_ATTRIBUTES && dwAttr & FILE_ATTRIBUTE_SYSTEM) != 0)) || (_wcsnicmp (ptr->real_path, config.windows_dir, config.wd_length) == 0);
	ptr->is_picoapp = (wcsstr (ptr->real_path, L"\\") == nullptr);

	if (!ptr->is_picoapp)
		ptr->is_network = PathIsNetworkPath (ptr->file_dir) ? true : false;

	ptr->timestamp = timestamp ? timestamp : _r_unixtime_now ();

	if (ptr->is_network || ptr->is_picoapp)
	{
		ptr->icon_id = config.def_icon_id;
	}
	else
	{
		ptr->icon_id = _app_geticonid (ptr->real_path);
	}

	const size_t item = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);

	config.is_firstapply = FALSE; // lock checkbox notifications

	_r_listview_additem (hwnd, IDC_LISTVIEW, app.ConfigGet (L"ShowFilenames", TRUE).AsBool () ? ptr->file_name : path, item, 0, ptr->icon_id, ptr->is_enabled ? 0 : 1, hash);
	_r_listview_additem (hwnd, IDC_LISTVIEW, _r_fmt_date (ptr->timestamp, FDTF_SHORTDATE | FDTF_SHORTTIME), item, 1);

	_r_listview_setitemcheck (hwnd, IDC_LISTVIEW, item, is_checked);

	config.is_firstapply = TRUE; // unlock checkbox notifications

	return hash;
}

BOOL _app_freeapplication (ITEM_APPLICATION *ptr, size_t hash)
{
	BOOL is_enabled = FALSE;

	if (ptr)
	{
		is_enabled = ptr->is_enabled;

		if (ptr->description)
		{
			SecureZeroMemory (ptr->description, (wcslen (ptr->description) + 1) * sizeof (WCHAR));
			free (ptr->description);
			ptr->description = nullptr;
		}
	}

	if (hash)
	{
		if (apps_rules.find (hash) != apps_rules.end ())
		{
			apps_rules.erase (hash);
			is_enabled = TRUE;
		}

		apps.erase (hash);
	}

	return is_enabled;
}

void _app_freerule (ITEM_RULE** ptr)
{
	if (ptr && *ptr)
	{
		ITEM_RULE* rule = *ptr;

		if (rule->name)
		{
			SecureZeroMemory (rule->name, (wcslen (rule->name) + 1) * sizeof (WCHAR));
			free (rule->name);

			rule->name = nullptr;
		}

		if (rule->rule)
		{
			SecureZeroMemory (rule->rule, (wcslen (rule->rule) + 1) * sizeof (WCHAR));
			free (rule->rule);

			rule->rule = nullptr;
		}

		if (rule->apps)
		{
			SecureZeroMemory (rule->apps, (wcslen (rule->apps) + 1) * sizeof (WCHAR));
			free (rule->apps);

			rule->apps = nullptr;
		}

		SecureZeroMemory (rule, sizeof (ITEM_RULE));
		free (rule);

		rule = nullptr;
		*ptr = nullptr;
	}
}

UINT _wfp_destroyfilters (BOOL is_forced)
{
	UINT error_count = 0;

	_R_SPINLOCK (config.lock_access);

	for (auto &p : apps)
		p.second.error_count = 0;

	_R_SPINUNLOCK (config.lock_access);

	DWORD result = FwpmTransactionBegin (config.hengine, 0);

	if (result != ERROR_SUCCESS)
	{
		_app_logerror (L"FwpmTransactionBegin", result, nullptr);
		++error_count;
	}
	else
	{
		HANDLE henum = nullptr;
		result = FwpmFilterCreateEnumHandle (config.hengine, nullptr, &henum);

		if (result != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmFilterCreateEnumHandle", result, nullptr);
			++error_count;
		}
		else
		{
			UINT32 count = 0;
			FWPM_FILTER** matchingFwpFilter = nullptr;

			result = FwpmFilterEnum (config.hengine, henum, 0xFFFFFFFF, &matchingFwpFilter, &count);

			if (result != ERROR_SUCCESS)
			{
				_app_logerror (L"FwpmFilterEnum", result, nullptr);
				++error_count;
			}
			else
			{
				if (matchingFwpFilter)
				{
					for (UINT32 i = 0; i < count; i++)
					{
						if (matchingFwpFilter[i]->providerKey && memcmp (matchingFwpFilter[i]->providerKey, &GUID_WfpProvider, sizeof (GUID)) == 0)
						{
							result = FwpmFilterDeleteById (config.hengine, matchingFwpFilter[i]->filterId);

							if (result != ERROR_SUCCESS)
							{
								_app_logerror (L"FwpmFilterDeleteById", result, nullptr);
								++error_count;
							}
						}
					}

					FwpmFreeMemory ((LPVOID*)&matchingFwpFilter);
				}
			}
		}

		if (henum)
			FwpmFilterDestroyEnumHandle (config.hengine, henum);

		// destroy callouts (deprecated)
		{
			const GUID callouts[] = {
				GUID_WfpOutboundCallout4_DEPRECATED,
				GUID_WfpOutboundCallout6_DEPRECATED,
				GUID_WfpInboundCallout4_DEPRECATED,
				GUID_WfpInboundCallout6_DEPRECATED,
				GUID_WfpListenCallout4_DEPRECATED,
				GUID_WfpListenCallout6_DEPRECATED
			};

			for (UINT i = 0; i < _countof (callouts); i++)
				FwpmCalloutDeleteByKey (config.hengine, &callouts[i]);
		}

		// destroy sublayer
		result = FwpmSubLayerDeleteByKey (config.hengine, &GUID_WfpSublayer);

		if (result != ERROR_SUCCESS && result != FWP_E_SUBLAYER_NOT_FOUND)
		{
			_app_logerror (L"FwpmSubLayerDeleteByKey", result, nullptr);
			++error_count;
		}

		// destroy provider
		result = FwpmProviderDeleteByKey (config.hengine, &GUID_WfpProvider);

		if (result != ERROR_SUCCESS && result != FWP_E_PROVIDER_NOT_FOUND)
		{
			_app_logerror (L"FwpmProviderDeleteByKey", result, nullptr);
			++error_count;
		}

		FwpmTransactionCommit (config.hengine);
	}

	if (is_forced)
	{
		// set icons
		app.SetIcon (IDI_INACTIVE);
		app.TraySetInfo (_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_INACTIVE), GetSystemMetrics (SM_CXSMICON)), nullptr);

		SetDlgItemText (app.GetHWND (), IDC_START_BTN, I18N (&app, IDS_TRAY_START, 0));

		config.is_filtersinstalled = FALSE;
	}

	return error_count;
}

DWORD _wfp_createfilter (LPCWSTR name, FWPM_FILTER_CONDITION* lpcond, UINT32 const count, UINT8 weight, GUID layer, const GUID* callout, BOOL is_block, bool is_boottime)
{
	FWPM_FILTER filter = {0};

	WCHAR fltr_name[128] = {0};
	StringCchCopy (fltr_name, _countof (fltr_name), name ? name : APP_NAME);

	filter.displayData.name = fltr_name;
	filter.displayData.description = fltr_name;

	if (is_boottime)
		filter.flags = FWPM_FILTER_FLAG_BOOTTIME;
	else
		filter.flags = FWPM_FILTER_FLAG_PERSISTENT;

	// filter is indexed to help enable faster lookup during classification (win8 and above)
	if (!is_boottime && _r_sys_validversion (6, 2))
		filter.flags |= FWPM_FILTER_FLAG_INDEXED;

	filter.providerKey = (LPGUID)&GUID_WfpProvider;
	filter.layerKey = layer;
	filter.subLayerKey = GUID_WfpSublayer;

	filter.numFilterConditions = count;
	filter.filterCondition = lpcond;

	if (is_block == FWP_ACTION_CALLOUT_TERMINATING)
		filter.action.type = FWP_ACTION_CALLOUT_TERMINATING;
	else
		filter.action.type = ((is_block) ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT);

	if (callout)
		memcpy_s (&filter.action.calloutKey, sizeof (GUID), callout, sizeof (GUID));

	filter.weight.type = FWP_UINT8;
	filter.weight.uint8 = weight;

	UINT64 filter_id = 0;

	return FwpmFilterAdd (config.hengine, &filter, nullptr, &filter_id);
}

INT CALLBACK _app_listviewcompare (LPARAM lp1, LPARAM lp2, LPARAM lparam)
{
	const UINT column_id = LOWORD (lparam);
	const BOOL is_descend = HIWORD (lparam);

	const size_t item1 = static_cast<size_t>(lp1);
	const size_t item2 = static_cast<size_t>(lp2);

	INT result = 0;

	if (apps.find (item1) == apps.end () || apps.find (item2) == apps.end ())
		return 0;

	_R_SPINLOCK (config.lock_access);

	ITEM_APPLICATION const* app1 = &apps[item1];
	ITEM_APPLICATION const* app2 = &apps[item2];

	if (app1->is_enabled && !app2->is_enabled)
	{
		result = -1;
	}
	else if (!app1->is_enabled && app2->is_enabled)
	{
		result = 1;
	}
	else
	{
		if (column_id == 0)
		{
			// filename
			if (app.ConfigGet (L"ShowFilenames", TRUE).AsBool ())
				result = _wcsicmp (app1->file_name, app2->file_name);
			else
				result = _wcsicmp (app1->file_dir, app2->file_dir);
		}
		else if (column_id == 1)
		{
			// timestamp
			if (app1->timestamp == app2->timestamp)
			{
				result = 0;
			}
			else if (app1->timestamp < app2->timestamp)
			{
				result = -1;
			}
			else if (app1->timestamp > app2->timestamp)
			{
				result = 1;
			}
		}
	}

	_R_SPINUNLOCK (config.lock_access);

	return is_descend ? -result : result;
}

VOID _app_listviewsort (HWND hwnd, INT subitem, BOOL is_notifycode)
{
	BOOL is_descend = app.ConfigGet (L"IsSortDescending", TRUE).AsBool ();

	if (is_notifycode)
		is_descend = !is_descend;

	if (subitem == -1)
		subitem = app.ConfigGet (L"SortColumn", 1).AsBool ();

	LPARAM lparam = MAKELPARAM (subitem, is_descend);

	if (is_notifycode)
	{
		app.ConfigSet (L"IsSortDescending", is_descend);
		app.ConfigSet (L"SortColumn", subitem);
	}

	_r_listview_setcolumnsortindex (hwnd, IDC_LISTVIEW, 0, 0);
	_r_listview_setcolumnsortindex (hwnd, IDC_LISTVIEW, 1, 0);

	_r_listview_setcolumnsortindex (hwnd, IDC_LISTVIEW, subitem, is_descend ? -1 : 1);

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_SORTITEMS, lparam, (LPARAM)&_app_listviewcompare);

	_app_listviewresize (hwnd, 0);
	_app_refreshstatus (hwnd, TRUE, FALSE);
}

bool _app_ruleisport (LPCWSTR rule)
{
	if (!rule)
		return false;

	const size_t length = wcslen (rule);

	for (size_t i = 0; i < length; i++)
	{
		if (iswdigit (rule[i]) == 0 && rule[i] != L'-')
			return false;
	}

	return true;
}

bool _app_ruleishost (LPCWSTR rule)
{
	if (!rule)
		return false;

	const size_t length = wcslen (rule);

	for (size_t i = 0; i < length; i++)
	{
		const WCHAR ch = towlower (rule[i]);

		if ((ch >= L'g' && ch <= L'z') || ch == L'-')
			return true;
	}

	return false;
}

rstring _app_parsehostaddress (LPCWSTR host, USHORT port)
{
	DNS_RECORD* results = nullptr;

	in_addr dnsServer;
	dnsServer.S_un.S_un_b.s_b1 = 8; // google's dns
	dnsServer.S_un.S_un_b.s_b2 = 8;
	dnsServer.S_un.S_un_b.s_b3 = 8;
	dnsServer.S_un.S_un_b.s_b4 = 8;

	IP4_ARRAY dnsServers;
	dnsServers.AddrCount = 1;
	dnsServers.AddrArray[0] = IP4_ADDRESS (dnsServer.S_un.S_addr); // i'm not sure i'm allocating this correctly

	const DWORD code = DnsQuery (host, DNS_TYPE_ALL, DNS_QUERY_NO_LOCAL_NAME | DNS_QUERY_NO_HOSTS_FILE, &dnsServers, &results, nullptr);

	rstring result;

	if (code == ERROR_SUCCESS)
	{
		for (auto current = results; current != nullptr; current = current->pNext)
		{
			if (current->wType == DNS_TYPE_A)
			{
				// ipv4 address
				WCHAR str[INET_ADDRSTRLEN] = {0};
				InetNtopW (AF_INET, &(current->Data.A.IpAddress), str, _countof (str));

				result.Append (str);

				if (port)
					result.Append (_r_fmt (L":%d", port));

				result.Append (RULE_DELIMETER);
			}
			else if (current->wType == DNS_TYPE_AAAA)
			{
				// ipv6 address
				WCHAR str[INET6_ADDRSTRLEN] = {0};
				InetNtopW (AF_INET6, &(current->Data.AAAA.Ip6Address), str, _countof (str));

				result.Append (str);

				if (port)
					result.Append (_r_fmt (L":%d", port));

				result.Append (RULE_DELIMETER);
			}
			else if (current->wType == DNS_TYPE_CNAME)
			{
				// canonical name
				if (current->Data.CNAME.pNameHost)
					result.Append (_app_parsehostaddress (current->Data.CNAME.pNameHost, port));
			}
		}

		DnsFree (results, DnsFreeRecordList);
	}

	return result.Trim (RULE_DELIMETER);
}

bool _app_parsenetworkstring (LPCWSTR network_string, NET_ADDRESS_FORMAT* format_ptr, USHORT* port_ptr, FWP_V4_ADDR_AND_MASK* paddr4, FWP_V6_ADDR_AND_MASK* paddr6, LPWSTR* paddr_dns)
{
	NET_ADDRESS_INFO ni;
	SecureZeroMemory (&ni, sizeof (ni));

	USHORT port = 0;
	BYTE prefix_length = 0;

	const DWORD errcode = ParseNetworkString (network_string, NET_STRING_ANY_ADDRESS | NET_STRING_ANY_SERVICE | NET_STRING_IP_NETWORK | NET_STRING_ANY_ADDRESS_NO_SCOPE | NET_STRING_ANY_SERVICE_NO_SCOPE, &ni, &port, &prefix_length);

	if (errcode != ERROR_SUCCESS)
	{
		_app_logerror (L"ParseNetworkString", errcode, network_string, TRUE);
		return false;
	}
	else
	{
		if (format_ptr)
			*format_ptr = ni.Format;

		if (port_ptr)
			*port_ptr = port;

		if (ni.Format == NET_ADDRESS_IPV4)
		{
			if (paddr4)
			{
				ULONG mask = 0;
				ConvertLengthToIpv4Mask (prefix_length, &mask);

				paddr4->mask = ntohl (mask);
				paddr4->addr = ntohl (ni.Ipv4Address.sin_addr.S_un.S_addr);
			}
		}
		else if (ni.Format == NET_ADDRESS_IPV6)
		{
			if (paddr6)
			{
				paddr6->prefixLength = min (prefix_length, 64); // the standard subnet size for ipv6 networks is a /64 block
				memcpy_s (paddr6->addr, FWP_V6_ADDR_SIZE, ni.Ipv6Address.sin6_addr.u.Byte, FWP_V6_ADDR_SIZE);
			}
		}
		else if (ni.Format == NET_ADDRESS_DNS_NAME)
		{
			if (paddr_dns)
			{
				const rstring buff = _app_parsehostaddress (ni.NamedAddress.Address, port);

				if (!buff.IsEmpty ())
				{
					const size_t size = (buff.GetLength () + 1) * sizeof (WCHAR);

					*paddr_dns = (LPWSTR)malloc (size);

					if (*paddr_dns)
						memcpy_s (*paddr_dns, size, buff, size);
				}
			}
		}
		else
		{
			return false;
		}
	}

	return true;
}

bool _app_parserulestring (rstring rule, ITEM_ADDRESS* ptr, EnumRuleType* ptype)
{
	rule.Trim (L"\r\n "); // trim whitespace

	if (rule.IsEmpty ())
		return false;

	if (rule.At (0) == L'*')
		return true;

	EnumRuleType type = TypeUnknown;
	size_t range_pos = rule.Find (L'-');

	if (ptype)
		type = *ptype;

	// auto-parse rule type
	if (type == TypeUnknown)
	{
		if (_app_ruleisport (rule))
		{
			type = TypePort;
		}
		else
		{
			const bool is_host = _app_ruleishost (rule);

			if (is_host)
			{
				type = TypeHost;
				range_pos = rstring::npos;
			}
			else
			{
				type = TypeIp;
			}
		}
	}

	if (type == TypeUnknown && ptype)
		return false;

	if (ptr && range_pos != rstring::npos)
		ptr->is_range = true;

	WCHAR range_start[48] = {0};
	WCHAR range_end[48] = {0};

	if (range_pos != rstring::npos)
	{
		StringCchCopy (range_start, _countof (range_start), rule.Midded (0, range_pos));
		StringCchCopy (range_end, _countof (range_end), rule.Midded (range_pos + 1));
	}

	if (type == TypePort)
	{
		if (range_pos == rstring::npos)
		{
			// ...port
			if (ptr)
			{
				ptr->type = TypePort;
				ptr->port = (UINT16)rule.AsUlong ();
			}

			return true;
		}
		else
		{
			// ...port range
			if (ptr)
			{
				ptr->type = TypePort;

				if (ptr->range)
				{
					ptr->range->valueLow.type = FWP_UINT16;
					ptr->range->valueLow.uint16 = (UINT16)wcstoul (range_start, nullptr, 10);

					ptr->range->valueHigh.type = FWP_UINT16;
					ptr->range->valueHigh.uint16 = (UINT16)wcstoul (range_end, nullptr, 10);
				}
			}

			return true;
		}
	}
	else
	{
		NET_ADDRESS_FORMAT format;
		USHORT port = 0;

		FWP_V4_ADDR_AND_MASK addr4 = {0};
		FWP_V6_ADDR_AND_MASK addr6 = {0};

		if (range_pos == rstring::npos)
		{
			LPWSTR addr_dns = nullptr;

			// ...address
			if (_app_parsenetworkstring (rule, &format, &port, &addr4, &addr6, ((ptr && ptr->addr_dns) ? &addr_dns : nullptr)))
			{
				if (ptr)
				{
					ptr->format = format;
					ptr->type = TypeIp;
				}

				if (format == NET_ADDRESS_IPV4)
				{
					if (ptr && ptr->addr4)
					{
						ptr->addr4->mask = addr4.mask;
						ptr->addr4->addr = addr4.addr;
					}
				}
				else if (format == NET_ADDRESS_IPV6)
				{
					if (ptr && ptr->addr6)
					{
						ptr->addr6->prefixLength = addr6.prefixLength;
						memcpy_s (ptr->addr6->addr, FWP_V6_ADDR_SIZE, addr6.addr, FWP_V6_ADDR_SIZE);
					}
				}
				else if (format == NET_ADDRESS_DNS_NAME)
				{
					if (ptr)
					{
						ptr->type = TypeHost;

						if (ptr->addr_dns && addr_dns)
							*ptr->addr_dns = addr_dns;
					}
				}
				else
				{
					return false;
				}

				// set port if available
				if (ptr && port)
					ptr->port = port;

				return true;
			}
			else
			{
				return false;
			}
		}
		else
		{
			// ...address range (start)
			if (_app_parsenetworkstring (range_start, &format, &port, &addr4, &addr6, nullptr))
			{
				if (ptr)
				{
					ptr->format = format;
					ptr->type = TypeIp;
				}

				if (format == NET_ADDRESS_IPV4)
				{
					if (ptr && ptr->range)
					{
						ptr->range->valueLow.type = FWP_UINT32;
						ptr->range->valueLow.uint32 = addr4.addr;
					}
				}
				else if (format == NET_ADDRESS_IPV6)
				{
					if (ptr && ptr->range)
					{
						ptr->range->valueLow.type = FWP_BYTE_ARRAY16_TYPE;
						memcpy_s (ptr->range->valueLow.byteArray16->byteArray16, FWP_V6_ADDR_SIZE, addr6.addr, FWP_V6_ADDR_SIZE);
					}
				}
				else
				{
					return false;
				}
			}
			else
			{
				return false;
			}

			// ...address range (end)
			if (_app_parsenetworkstring (range_end, &format, &port, &addr4, &addr6, nullptr))
			{
				if (format == NET_ADDRESS_IPV4)
				{
					if (ptr && ptr->range)
					{
						ptr->range->valueHigh.type = FWP_UINT32;
						ptr->range->valueHigh.uint32 = addr4.addr;
					}
				}
				else if (format == NET_ADDRESS_IPV6)
				{
					if (ptr && ptr->range)
					{
						ptr->range->valueHigh.type = FWP_BYTE_ARRAY16_TYPE;
						memcpy_s (ptr->range->valueHigh.byteArray16->byteArray16, FWP_V6_ADDR_SIZE, addr6.addr, FWP_V6_ADDR_SIZE);
					}
				}
				else
				{
					return false;
				}
			}
			else
			{
				return false;
			}
		}
	}

	return true;
}
// Author: Elmue
// http://stackoverflow.com/questions/65170/how-to-get-name-associated-with-open-handle/18792477#18792477
//
// returns
// "\Device\HarddiskVolume3"                                (Harddisk Drive)
// "\Device\HarddiskVolume3\Temp"                           (Harddisk Directory)
// "\Device\HarddiskVolume3\Temp\transparent.jpeg"          (Harddisk File)
// "\Device\Harddisk1\DP(1)0-0+6\foto.jpg"                  (USB stick)
// "\Device\TrueCryptVolumeP\Data\Passwords.txt"            (Truecrypt Volume)
// "\Device\Floppy0\Autoexec.bat"                           (Floppy disk)
// "\Device\CdRom1\VIDEO_TS\VTS_01_0.VOB"                   (DVD drive)
// "\Device\Serial1"                                        (real COM port)
// "\Device\USBSER000"                                      (virtual COM port)
// "\Device\Mup\ComputerName\C$\Boot.ini"                   (network drive share,  Windows 7)
// "\Device\LanmanRedirector\ComputerName\C$\Boot.ini"      (network drive share,  Windows XP)
// "\Device\LanmanRedirector\ComputerName\Shares\Dance.m3u" (network folder share, Windows XP)
// "\Device\Afd"                                            (internet socket)
// "\Device\NamedPipe\Pipename"                             (named pipe)
// "\BaseNamedObjects\Objectname"                           (named mutex, named event, named semaphore)
// "\REGISTRY\MACHINE\SOFTWARE\Classes\.txt"                (HKEY_CLASSES_ROOT\.txt)

DWORD _FwpmGetAppIdFromFileName1 (LPCWSTR path, FWP_BYTE_BLOB** ptr)
{
	rstring data = path;

	if (!ptr)
		return ERROR_BAD_ARGUMENTS;

	// check for filename-only apps (Pico / System etc.)
	if (data.Find (L'\\', 0) != rstring::npos)
	{
		// file root
		WCHAR path_root[64] = {0};
		StringCchCopy (path_root, _countof (path_root), path);
		PathStripToRoot (path_root);

		if (!_r_fs_exists (path_root))
			return ERROR_PATH_NOT_FOUND;

		// file path (without root)
		WCHAR path_noroot[MAX_PATH] = {0};
		StringCchCopy (path_noroot, _countof (path_noroot), PathSkipRoot (path));

		HANDLE h = CreateFile (path_root, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

		if (h == INVALID_HANDLE_VALUE)
		{
			return GetLastError ();
		}
		else
		{
			BYTE u8_Buffer[2048] = {0};
			DWORD u32_ReqLength = 0;

			UNICODE_STRING* pk_Info = &((OBJECT_NAME_INFORMATION*)u8_Buffer)->Name;
			pk_Info->Buffer = nullptr;
			pk_Info->Length = 0;

			// IMPORTANT: The return value from NtQueryObject is bullshit! (driver bug?)
			// - The function may return STATUS_NOT_SUPPORTED although it has successfully written to the buffer.
			// - The function returns STATUS_SUCCESS although h_File == 0xFFFFFFFF
			NtQueryObject (h, ObjectNameInformation, u8_Buffer, sizeof (u8_Buffer), &u32_ReqLength);

			// On error pk_Info->Buffer is NULL
			if (pk_Info->Buffer && pk_Info->Length)
			{
				pk_Info->Buffer[pk_Info->Length / 2] = 0; // trim buffer!

				data = pk_Info->Buffer;
				data.Append (path_noroot);

				CharLower (data.GetBuffer ()); // lower is imoprtant!
			}
			else
			{
				CloseHandle (h);

				return ERROR_FILE_NOT_FOUND;
			}

			CloseHandle (h);
		}
	}

	// allocate buffer
	{
		*ptr = (FWP_BYTE_BLOB*)malloc (sizeof (FWP_BYTE_BLOB));

		if (!*ptr)
		{
			return ERROR_OUTOFMEMORY;
		}
		else
		{
			SecureZeroMemory (*ptr, sizeof (FWP_BYTE_BLOB));

			const size_t length = (data.GetLength () + 1) * sizeof (WCHAR);
			const DWORD_PTR tmp_ptr = (DWORD_PTR)malloc (length);

			if (!tmp_ptr)
			{
				free (*ptr);

				return ERROR_OUTOFMEMORY;
			}
			else
			{
				SecureZeroMemory ((UINT8*)tmp_ptr, length);

				(*ptr)->data = (UINT8*)tmp_ptr;
				*((*ptr)->data) = (UINT8)tmp_ptr;

				(*ptr)->size = (UINT32)length;

				memcpy_s ((*ptr)->data, length, data.GetString (), length);
			}
		}
	}

	return ERROR_SUCCESS;
}

VOID _FwpmFreeAppIdFromFileName1 (FWP_BYTE_BLOB** ptr)
{
	if (!ptr)
		return;

	if (ptr && *ptr)
	{
		FWP_BYTE_BLOB* blob = *ptr;

		if (blob->data)
		{
			free (blob->data);
			blob->data = nullptr;
		}

		free (blob);
		blob = nullptr;
		*ptr = nullptr;
	}
}

bool _wfp_createrulefilter (LPCWSTR name, LPCWSTR rule, LPCWSTR path, EnumRuleDirection dir, EnumRuleType* ptype, UINT8 protocol, ADDRESS_FAMILY af, BOOL is_block, UINT8 weight, bool is_boottime)
{
	UINT32 count = 0;
	FWPM_FILTER_CONDITION fwfc[6] = {0};

	// rule without address
	if (rule && rule[0] == L'*')
		rule = nullptr;

	FWP_BYTE_BLOB* blob = nullptr;

	FWP_V4_ADDR_AND_MASK addr4 = {0};
	FWP_V6_ADDR_AND_MASK addr6 = {0};

	FWP_RANGE range;
	SecureZeroMemory (&range, sizeof (range));

	ITEM_ADDRESS addr;
	SecureZeroMemory (&addr, sizeof (addr));

	LPWSTR addr_dns = nullptr;

	addr.addr4 = &addr4;
	addr.addr6 = &addr6;
	addr.range = &range;
	addr.addr_dns = &addr_dns;

	UINT32 ip_idx = UINT32 (-1);
	UINT32 port_idx = UINT32 (-1);

	if (path)
	{
		DWORD retn = _FwpmGetAppIdFromFileName1 (path, &blob);

		if (retn != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmGetAppIdFromFileName", retn, path, ((retn == ERROR_FILE_NOT_FOUND) || (retn == ERROR_PATH_NOT_FOUND)));
			return false;
		}
		else
		{
			fwfc[count].fieldKey = FWPM_CONDITION_ALE_APP_ID;
			fwfc[count].matchType = FWP_MATCH_EQUAL;
			fwfc[count].conditionValue.type = FWP_BYTE_BLOB_TYPE;
			fwfc[count].conditionValue.byteBlob = blob;

			count += 1;
		}
	}

	if (protocol)
	{
		fwfc[count].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
		fwfc[count].matchType = FWP_MATCH_EQUAL;
		fwfc[count].conditionValue.type = FWP_UINT8;
		fwfc[count].conditionValue.uint8 = protocol;

		count += 1;
	}

	if (rule && ptype)
	{
		if (_app_parserulestring (rule, &addr, ptype))
		{
			if (addr.is_range && (addr.type == TypeIp || addr.type == TypePort))
			{
				if (addr.type == TypeIp)
				{
					if (addr.format == NET_ADDRESS_IPV4)
						af = AF_INET;
					else if (addr.format == NET_ADDRESS_IPV6)
						af = AF_INET6;
					else
						return false;
				}

				fwfc[count].matchType = FWP_MATCH_RANGE;
				fwfc[count].conditionValue.type = FWP_RANGE_TYPE;
				fwfc[count].conditionValue.rangeValue = &range;

				if (addr.type == TypePort)
					port_idx = count;
				else
					ip_idx = count;

				count += 1;
			}
			else if (addr.type == TypePort)
			{
				fwfc[count].matchType = FWP_MATCH_EQUAL;
				fwfc[count].conditionValue.type = FWP_UINT16;
				fwfc[count].conditionValue.uint16 = addr.port;

				port_idx = count;
				count += 1;
			}
			else if (addr.type == TypeHost || addr.type == TypeIp)
			{
				if (addr.format == NET_ADDRESS_IPV4)
				{
					af = AF_INET;

					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_V4_ADDR_MASK;
					fwfc[count].conditionValue.v4AddrMask = &addr4;

					ip_idx = count;
					count += 1;
				}
				else if (addr.format == NET_ADDRESS_IPV6)
				{
					af = AF_INET6;

					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_V6_ADDR_MASK;
					fwfc[count].conditionValue.v6AddrMask = &addr6;

					ip_idx = count;
					count += 1;
				}
				else if (addr.format == NET_ADDRESS_DNS_NAME)
				{
					_FwpmFreeAppIdFromFileName1 (&blob);

					if (!addr_dns)
					{
						return false;
					}
					else
					{
						rstring::rvector arr = rstring (addr_dns).AsVector (RULE_DELIMETER);

						if (arr.empty ())
						{
							free (addr_dns);
							return false;
						}
						else
						{
							for (size_t i = 0; i < arr.size (); i++)
							{
								EnumRuleType type = TypeHost;

								if (!_wfp_createrulefilter (name, arr.at (i), path, dir, &type, protocol, af, is_block, weight, is_boottime))
								{
									free (addr_dns);
									return false;
								}
							}
						}

						free (addr_dns);
						return true;
					}
				}
				else
				{
					_FwpmFreeAppIdFromFileName1 (&blob);
					return false;
				}

				// set port if available
				if (addr.port)
				{
					fwfc[count].matchType = FWP_MATCH_EQUAL;
					fwfc[count].conditionValue.type = FWP_UINT16;
					fwfc[count].conditionValue.uint16 = addr.port;

					port_idx = count;
					count += 1;
				}
			}
			else
			{
				_FwpmFreeAppIdFromFileName1 (&blob);
				return false;
			}
		}
		else
		{
			_FwpmFreeAppIdFromFileName1 (&blob);
			return false;
		}
	}

	// create filters
	DWORD result = 0;

	if (dir == DirOutbound || dir == DirBoth)
	{
		if (ip_idx != UINT32 (-1))
			fwfc[ip_idx].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;

		if (port_idx != UINT32 (-1))
			fwfc[port_idx].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;

		if (af == AF_INET || af == AF_UNSPEC)
		{
			result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, is_block, is_boottime);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule);
		}

		if (af == AF_INET6 || af == AF_UNSPEC)
		{
			result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, is_block, is_boottime);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmFilterAdd", result, rule);
		}
	}

	if (dir == DirInbound || dir == DirBoth)
	{
		if (ip_idx != UINT32 (-1))
			fwfc[ip_idx].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;

		if (port_idx != UINT32 (-1))
			fwfc[port_idx].fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;

		// allow inbound connections only if stealth mode does not enabled
		if (!app.ConfigGet (L"UseStealthMode", FALSE).AsBool () || (app.ConfigGet (L"UseStealthMode", FALSE).AsBool () && is_block))
		{
			if (af == AF_INET || af == AF_UNSPEC)
			{
				result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, is_block, is_boottime);

				if (result != ERROR_SUCCESS)
					_app_logerror (L"FwpmFilterAdd", result, rule);
			}

			if (af == AF_INET6 || af == AF_UNSPEC)
			{
				result = _wfp_createfilter (name, fwfc, count, weight, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, is_block, is_boottime);

				if (result != ERROR_SUCCESS)
					_app_logerror (L"FwpmFilterAdd", result, rule);
			}
		}
	}

	_FwpmFreeAppIdFromFileName1 (&blob);

	return true;
}

LPVOID _app_loadresource (UINT rc, PDWORD size)
{
	const HINSTANCE hinst = app.GetHINSTANCE ();

	HRSRC hres = FindResource (hinst, MAKEINTRESOURCE (rc), RT_RCDATA);

	if (hres)
	{
		HGLOBAL hloaded = LoadResource (hinst, hres);

		if (hloaded)
		{
			LPVOID pLockedResource = LockResource (hloaded);

			if (pLockedResource)
			{
				DWORD dwResourceSize = SizeofResource (hinst, hres);

				if (dwResourceSize != 0)
				{
					if (size)
						*size = dwResourceSize;

					return pLockedResource;
				}
			}
		}
	}

	return nullptr;
}

VOID _app_loadrules (HWND hwnd, LPCWSTR path, UINT rc, BOOL is_internal, std::vector<ITEM_RULE*>* ptr)
{
	if (!ptr)
		return;

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file (path, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

	if (!result)
	{
		// show only syntax, memory and i/o errors...
		if (result.status != pugi::status_file_not_found)
			_app_logerror (L"pugi::load_file", 0, _r_fmt (L"status: %d, offset: %d, text: %s, file: %s", result.status, result.offset, rstring (result.description ()), path));

		// if file not found or parsing error, get internal rules (from resources)
		if (rc)
		{
			DWORD size = 0;
			LPVOID buffer = _app_loadresource (rc, &size);

			if (buffer)
				result = doc.load_buffer (buffer, size, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);
		}
	}

	if (result)
	{
		pugi::xml_node root = doc.child (L"root");

		if (root)
		{
			size_t idx = 0;

			for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
			{
				ITEM_RULE* rule_ptr = (ITEM_RULE*)malloc (sizeof (ITEM_RULE));

				if (rule_ptr)
				{
					SecureZeroMemory (rule_ptr, sizeof (ITEM_RULE));

					// allocate required memory
					{
						rstring attr_name = item.attribute (L"name").as_string ();
						rstring attr_rule = item.attribute (L"rule").as_string ();

						const size_t name_length = min (attr_name.GetLength (), RULE_NAME_CCH_MAX) + 1;
						const size_t rule_length = min (attr_rule.GetLength (), RULE_RULE_CCH_MAX) + 1;

						rule_ptr->name = (LPWSTR)malloc ((name_length + 1) * sizeof (WCHAR));
						rule_ptr->rule = (LPWSTR)malloc ((rule_length + 1) * sizeof (WCHAR));

						if (rule_ptr->name)
							StringCchCopy (rule_ptr->name, name_length, attr_name);

						if (rule_ptr->rule)
							StringCchCopy (rule_ptr->rule, rule_length, attr_rule);
					}

					rule_ptr->dir = (EnumRuleDirection)item.attribute (L"dir").as_uint ();

					if (!item.attribute (L"type").empty ())
					{
						rule_ptr->type = (EnumRuleType)item.attribute (L"type").as_uint ();
					}

					rule_ptr->protocol = (UINT8)item.attribute (L"protocol").as_uint ();
					rule_ptr->version = (ADDRESS_FAMILY)item.attribute (L"version").as_uint ();

					if (!item.attribute (L"apps").empty ())
					{
						// allocate required memory
						rstring attr_apps = item.attribute (L"apps").as_string ();
						const size_t apps_length = min (attr_apps.GetLength (), RULE_APPS_CCH_MAX) + 1;

						rule_ptr->apps = (LPWSTR)malloc ((apps_length + 1) * sizeof (WCHAR));

						if (rule_ptr->apps)
							StringCchCopy (rule_ptr->apps, apps_length, attr_apps);
					}

					rule_ptr->is_block = item.attribute (L"is_block").as_bool ();
					rule_ptr->is_enabled = item.attribute (L"is_enabled").as_bool ();

					if (is_internal)
					{
						// system rules/blocklist
						if (rules_config.find (rule_ptr->name) != rules_config.end ())
							rule_ptr->is_enabled = rules_config[rule_ptr->name];
						else
							rules_config[rule_ptr->name] = rule_ptr->is_enabled;
					}
					else
					{
						// custom rules (if memory allocated)
						if (!item.attribute (L"apps").empty () && rule_ptr->apps)
						{
							rstring::rvector arr = rstring (rule_ptr->apps).AsVector (RULE_DELIMETER);

							for (size_t i = 0; i < arr.size (); i++)
							{
								const rstring path2 = _r_path_expand (arr.at (i).Trim (L"\r\n "));

								if (!path2.IsEmpty ())
								{
									const size_t hash = path2.Hash ();

									if (hash)
									{
										if (apps.find (hash) == apps.end ())
											_app_addapplication (hwnd, path2, 0, FALSE, FALSE);

										apps_rules[hash].push_back (idx);
									}
								}
							}
						}
					}

					ptr->push_back (rule_ptr);

					idx += 1;
				}
			}
		}
	}
}

VOID _app_profilesave (HWND hwnd, LPCWSTR path_apps = nullptr, LPCWSTR path_rules = nullptr)
{
	// apps rules
	{
		pugi::xml_document doc;
		pugi::xml_node root = doc.append_child (L"root");

		if (root)
		{
			const size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);

			if (count)
			{
				for (size_t i = 0; i < count; i++)
				{
					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i);

					if (hash)
					{
						pugi::xml_node item = root.append_child (L"item");

						if (item)
						{
							ITEM_APPLICATION const* ptr = &apps[hash];

							item.append_attribute (L"path").set_value (ptr->display_path);
							item.append_attribute (L"timestamp").set_value (ptr->timestamp);
							item.append_attribute (L"is_silent").set_value (ptr->is_silent);
							item.append_attribute (L"is_enabled").set_value (ptr->is_enabled);
						}
					}
				}
			}
			else
			{
				for (auto const &p : apps)
				{
					const size_t hash = p.first;

					if (hash)
					{
						pugi::xml_node item = root.append_child (L"item");

						if (item)
						{
							ITEM_APPLICATION const* ptr = &p.second;

							item.append_attribute (L"path").set_value (ptr->display_path);
							item.append_attribute (L"timestamp").set_value (ptr->timestamp);
							item.append_attribute (L"is_silent").set_value (ptr->is_silent);
							item.append_attribute (L"is_enabled").set_value (ptr->is_enabled);
						}
					}
				}
			}

			doc.save_file (path_apps ? path_apps : config.apps_path, L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);
		}
	}

	{
		pugi::xml_document doc;
		pugi::xml_node root = doc.append_child (L"root");

		if (root)
		{
			for (auto const &p : rules_config)
			{
				pugi::xml_node item = root.append_child (L"item");

				if (item)
				{
					item.append_attribute (L"name").set_value (p.first);
					item.append_attribute (L"is_enabled").set_value (p.second);
				}
			}

			doc.save_file (config.rules_config_path, L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);
		}
	}

	// custom rules
	{
		if (rules_custom.empty ())
		{
			if (_r_fs_exists (config.rules_custom_path))
				_r_fs_delete (config.rules_custom_path);
		}
		else
		{
			pugi::xml_document doc;
			pugi::xml_node root = doc.append_child (L"root");

			if (root)
			{
				for (size_t i = 0; i < rules_custom.size (); i++)
				{
					ITEM_RULE const* ptr = rules_custom.at (i);

					if (!ptr)
						continue;

					pugi::xml_node item = root.append_child (L"item");

					if (item)
					{
						item.append_attribute (L"name").set_value (ptr->name);
						item.append_attribute (L"rule").set_value (ptr->rule);
						item.append_attribute (L"dir").set_value (ptr->dir);

						if (ptr->type != TypeUnknown)
							item.append_attribute (L"type").set_value (ptr->type);

						item.append_attribute (L"protocol").set_value (ptr->protocol);
						item.append_attribute (L"version").set_value (ptr->version);

						// add apps attribute
						{
							rstring arr;
							BOOL is_haveapps = FALSE;

							for (auto const &p : apps_rules)
							{
								const size_t hash = p.first;

								if (hash)
								{
									for (size_t j = 0; j < p.second.size (); j++)
									{
										if (p.second.at (j) == i)
										{
											arr.Append (_r_path_unexpand (apps[hash].display_path));
											arr.Append (RULE_DELIMETER);

											is_haveapps = TRUE;

											break;
										}
									}
								}
							}

							if (is_haveapps)
							{
								arr.Trim (RULE_DELIMETER);

								item.append_attribute (L"apps").set_value (arr);
							}
						}

						item.append_attribute (L"is_block").set_value (ptr->is_block);
						item.append_attribute (L"is_enabled").set_value (ptr->is_enabled);
					}
				}

				doc.save_file (path_rules ? path_rules : config.rules_custom_path, L"\t", PUGIXML_SAVE_FLAGS, PUGIXML_SAVE_ENCODING);
			}
		}
	}
}

UINT _wfp_installfilters ()
{
	UINT error_count = 0;

	_wfp_destroyfilters (FALSE); // destroy all installed filters first

	DWORD result = FwpmTransactionBegin (config.hengine, 0);

	if (result != ERROR_SUCCESS)
	{
		_app_logerror (L"FwpmTransactionBegin", result, nullptr);
		++error_count;
	}
	else
	{
		// create provider
		FWPM_PROVIDER provider = {0};

		provider.displayData.name = APP_NAME;
		provider.displayData.description = APP_NAME;

		provider.providerKey = GUID_WfpProvider;
		provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;

		result = FwpmProviderAdd (config.hengine, &provider, nullptr);

		if (result != ERROR_SUCCESS && result != FWP_E_ALREADY_EXISTS)
		{
			_app_logerror (L"FwpmProviderAdd", result, nullptr);
			FwpmTransactionAbort (config.hengine);

			++error_count;
		}
		else
		{
			FWPM_SUBLAYER sublayer = {0};

			sublayer.displayData.name = APP_NAME;
			sublayer.displayData.description = APP_NAME;

			sublayer.providerKey = (LPGUID)&GUID_WfpProvider;
			sublayer.subLayerKey = GUID_WfpSublayer;
			sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
			sublayer.weight = (UINT16)app.ConfigGet (L"SublayerWeight", 0x0000ffff).AsUint (); // highest weight "65535"

			result = FwpmSubLayerAdd (config.hengine, &sublayer, nullptr);

			if (result != ERROR_SUCCESS && result != FWP_E_ALREADY_EXISTS)
			{
				_app_logerror (L"FwpmSubLayerAdd", result, nullptr);
				FwpmTransactionAbort (config.hengine);

				++error_count;
			}
			else
			{
				const EnumMode mode = (EnumMode)app.ConfigGet (L"Mode", ModeWhitelist).AsUint ();

				FWPM_FILTER_CONDITION fwfc[6] = {0};

				// add loopback connections permission
				{
					// match all loopback (localhost) data
					fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
					fwfc[0].matchType = FWP_MATCH_FLAGS_ANY_SET;
					fwfc[0].conditionValue.type = FWP_UINT32;
					fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

					// tests if the network traffic is (non-)app container loopback traffic (win8 and above)
					if (_r_sys_validversion (6, 2))
						fwfc[0].conditionValue.uint32 |= (FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK | FWP_CONDITION_FLAG_IS_NON_APPCONTAINER_LOOPBACK);

					error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, FALSE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, FALSE);

					error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, FALSE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, FALSE);

					// boot-time filters loopback permission
					if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
					{
						error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, TRUE);
						error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, TRUE);

						error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, TRUE);
						error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, TRUE);
					}

					// ipv4/ipv6 loopback
					LPCWSTR ip_list[] = {L"10.0.0.0/8", L"172.16.0.0/12", L"169.254.0.0/16", L"192.168.0.0/16", L"224.0.0.0/24", L"fd00::/8", L"fe80::/10"};

					for (size_t i = 0; i < _countof (ip_list); i++)
					{
						FWP_V4_ADDR_AND_MASK addr4 = {0};
						FWP_V6_ADDR_AND_MASK addr6 = {0};

						ITEM_ADDRESS addr;
						SecureZeroMemory (&addr, sizeof (addr));

						addr.addr4 = &addr4;
						addr.addr6 = &addr6;

						EnumRuleType rule_type = TypeIp;

						if (_app_parserulestring (ip_list[i], &addr, &rule_type))
						{
							//fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
							fwfc[1].matchType = FWP_MATCH_EQUAL;

							if (addr.format == NET_ADDRESS_IPV4)
							{
								fwfc[1].conditionValue.type = FWP_V4_ADDR_MASK;
								fwfc[1].conditionValue.v4AddrMask = &addr4;

								fwfc[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
								error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, FALSE);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, FALSE, TRUE);

								fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
								error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, FALSE);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, TRUE);
							}
							else if (addr.format == NET_ADDRESS_IPV6)
							{
								fwfc[1].conditionValue.type = FWP_V6_ADDR_MASK;
								fwfc[1].conditionValue.v6AddrMask = &addr6;

								fwfc[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
								error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, FALSE);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, FALSE, TRUE);

								fwfc[1].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
								error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, FALSE);

								if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
									error_count += ERROR_SUCCESS != _wfp_createfilter (nullptr, fwfc, 2, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, TRUE);
							}
						}
					}
				}

				// apply apps rules
				{
					const bool is_block = (mode == ModeBlacklist) ? true : false;

					for (auto& p : apps)
					{
						ITEM_APPLICATION* ptr = &p.second;

						if (!ptr->is_enabled)
							continue;

						ptr->error_count = !_wfp_createrulefilter (ptr->file_name, nullptr, ptr->display_path, DirBoth, nullptr, 0, AF_UNSPEC, is_block, FILTER_WEIGHT_APPLICATION, false);

						error_count += ptr->error_count;
					}

					// unlock this app
					error_count += !_wfp_createrulefilter (nullptr, nullptr, app.GetBinaryPath (), DirBoth, nullptr, 0, AF_UNSPEC, false, FILTER_WEIGHT_APPLICATION, false);
				}

				// apply blocklist rules
				if (app.ConfigGet (L"UseBlocklist2", FALSE).AsBool ())
				{
					for (size_t i = 0; i < rules_blocklist.size (); i++)
					{
						ITEM_RULE* ptr = rules_blocklist.at (i);

						if (!ptr || !ptr->is_enabled)
							continue;

						rstring::rvector arr = rstring (ptr->rule).AsVector (RULE_DELIMETER);

						for (size_t j = 0; j < arr.size (); j++)
						{
							const rstring rule = arr.at (j).Trim (L"\r\n ");

							if (!rule.IsEmpty ())
								error_count += !_wfp_createrulefilter (ptr->name, rule, nullptr, ptr->dir, &ptr->type, ptr->protocol, ptr->version, ptr->is_block, FILTER_WEIGHT_BLOCKLIST, false);
						}
					}
				}

				// apply system rules
				{
					for (size_t i = 0; i < rules_system.size (); i++)
					{
						ITEM_RULE* ptr = rules_system.at (i);

						if (!ptr || !ptr->is_enabled)
							continue;

						rstring::rvector arr = rstring (ptr->rule).AsVector (RULE_DELIMETER);

						for (size_t j = 0; j < arr.size (); j++)
						{
							const rstring rule = arr.at (j).Trim (L"\r\n ");

							if (rule.IsEmpty ())
								continue;

							if (ptr->apps)
							{
								// apply rules for specified apps
								rstring::rvector arr2 = rstring (ptr->apps).AsVector (RULE_DELIMETER);

								for (size_t k = 0; k < arr2.size (); k++)
								{
									const rstring path = _r_path_expand (arr2.at (k)).Trim (L"\r\n ");

									if (!path.IsEmpty ())
										error_count += !_wfp_createrulefilter (ptr->name, rule, path, ptr->dir, &ptr->type, ptr->protocol, ptr->version, ptr->is_block, FILTER_WEIGHT_CUSTOM, FALSE);
								}
							}
							else
							{
								// apply rules for all apps
								error_count += !_wfp_createrulefilter (ptr->name, rule, nullptr, ptr->dir, &ptr->type, ptr->protocol, ptr->version, ptr->is_block, FILTER_WEIGHT_CUSTOM, FALSE);
							}
						}
					}
				}

				// apply custom rules for apps
				{
					for (auto const &p : apps_rules)
					{
						const size_t hash = p.first;

						if (hash && apps.find (hash) != apps.end ())
						{
							const rstring path = apps[hash].display_path;

							for (size_t i = 0; i < p.second.size (); i++)
							{
								ITEM_RULE* ptr = rules_custom.at (i);

								// prevent filters duplicate, if rule is checked: guilty! next case.
								if (!ptr || ptr->is_enabled)
									continue;

								const rstring arr = ptr->rule;
								rstring::rvector vc = arr.AsVector (RULE_DELIMETER);

								for (size_t j = 0; j < vc.size (); j++)
								{
									const rstring rule = vc.at (j).Trim (L"\r\n ");

									if (!rule.IsEmpty ())
										error_count += !_wfp_createrulefilter (ptr->name, rule, path, ptr->dir, &ptr->type, ptr->protocol, ptr->version, ptr->is_block, FILTER_WEIGHT_CUSTOM, FALSE);
								}
							}
						}
					}
				}

				// apply custom rules for all
				{
					for (size_t i = 0; i < rules_custom.size (); i++)
					{
						ITEM_RULE* ptr = rules_custom.at (i);

						// only if enabled
						if (!ptr || !ptr->is_enabled)
							continue;

						const rstring arr = ptr->rule;
						rstring::rvector vc = arr.AsVector (RULE_DELIMETER);

						for (size_t j = 0; j < vc.size (); j++)
						{
							const rstring rule = vc.at (j).Trim (L"\r\n ");

							if (!rule.IsEmpty ())
								error_count += !_wfp_createrulefilter (ptr->name, rule, nullptr, ptr->dir, &ptr->type, ptr->protocol, ptr->version, ptr->is_block, FILTER_WEIGHT_CUSTOM, FALSE);
						}
					}
				}

				// allow ipv6 required rules
				{
					// allows 6to4 tunneling, which enables ipv6 to run over an ipv4 network
					fwfc[0].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
					fwfc[0].matchType = FWP_MATCH_EQUAL;
					fwfc[0].conditionValue.type = FWP_UINT8;
					fwfc[0].conditionValue.uint8 = IPPROTO_IPV6; // ipv6 header

					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Allow6to4", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, FALSE, FALSE);

					// allows icmpv6 router solicitation messages, which are required for the ipv6 stack to work properly
					fwfc[0].fieldKey = FWPM_CONDITION_ICMP_TYPE;
					fwfc[0].matchType = FWP_MATCH_EQUAL;
					fwfc[0].conditionValue.type = FWP_UINT16;
					fwfc[0].conditionValue.uint16 = 0x85;

					error_count += ERROR_SUCCESS != _wfp_createfilter (L"AllowIcmpV6Type133", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, FALSE);

					// allows icmpv6 router advertise messages, which are required for the ipv6 stack to work properly
					fwfc[0].conditionValue.uint16 = 0x86;
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"AllowIcmpV6Type134", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, FALSE);

					// allows icmpv6 neighbor solicitation messages, which are required for the ipv6 stack to work properly
					fwfc[0].conditionValue.uint16 = 0x87;
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"AllowIcmpV6Type135", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, FALSE);

					// allows icmpv6 neighbor advertise messages, which are required for the ipv6 stack to work properly
					fwfc[0].conditionValue.uint16 = 0x88;
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"AllowIcmpV6Type136", fwfc, 1, FILTER_WEIGHT_HIGHEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, FALSE, FALSE);
				}

				// prevent port scanning using stealth discards and silent drops
				if (app.ConfigGet (L"UseStealthMode", FALSE).AsBool ())
				{
					// blocks udp port scanners
					fwfc[0].fieldKey = FWPM_CONDITION_ICMP_TYPE;
					fwfc[0].matchType = FWP_MATCH_EQUAL;
					fwfc[0].conditionValue.type = FWP_UINT16;
					fwfc[0].conditionValue.uint16 = 0x03; // destination unreachable

					error_count += ERROR_SUCCESS != _wfp_createfilter (L"BlockIcmpErrorV4", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_OUTBOUND_ICMP_ERROR_V4, nullptr, TRUE, FALSE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"BlockIcmpErrorV6", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_OUTBOUND_ICMP_ERROR_V6, nullptr, TRUE, FALSE);

					// blocks tcp port scanners
					fwfc[0].fieldKey = FWPM_CONDITION_FLAGS;
					fwfc[0].matchType = FWP_MATCH_FLAGS_NONE_SET;
					fwfc[0].conditionValue.type = FWP_UINT32;
					fwfc[0].conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

					// tests if the network traffic is (non-)app container loopback traffic (win8 and above)
					if (_r_sys_validversion (6, 2))
						fwfc[0].conditionValue.uint32 |= (FWP_CONDITION_FLAG_IS_APPCONTAINER_LOOPBACK | FWP_CONDITION_FLAG_IS_NON_APPCONTAINER_LOOPBACK);

					error_count += ERROR_SUCCESS != _wfp_createfilter (L"BlockTcpRstOnCloseV4", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_INBOUND_TRANSPORT_V4_DISCARD, &FWPM_CALLOUT_WFP_TRANSPORT_LAYER_V4_SILENT_DROP, FWP_ACTION_CALLOUT_TERMINATING, FALSE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"BlockTcpRstOnCloseV6", fwfc, 1, FILTER_WEIGHT_HIGHEST_IMPORTANT, FWPM_LAYER_INBOUND_TRANSPORT_V6_DISCARD, &FWPM_CALLOUT_WFP_TRANSPORT_LAYER_V6_SILENT_DROP, FWP_ACTION_CALLOUT_TERMINATING, FALSE);
				}

				// block all outbound traffic (only on "whitelist" mode)
				if (mode == ModeWhitelist)
				{
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all outbound connections", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, TRUE, FALSE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all outbound connections", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, TRUE, FALSE);
				}

				// block all inbound traffic (only on"stealth" mode)
				if (app.ConfigGet (L"UseStealthMode", FALSE).AsBool () || app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () == FALSE)
				{
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all inbound connections", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, TRUE, FALSE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all inbound connections", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, TRUE, FALSE);
				}

				// install boot-time filters (enforced at boot-time, even before "base filtering engine" service starts)
				if (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ())
				{
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all outbound connections (boot-time)", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V4, nullptr, TRUE, TRUE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all outbound connections (boot-time)", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_CONNECT_V6, nullptr, TRUE, TRUE);

					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all inbound connections (boot-time)", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4, nullptr, TRUE, TRUE);
					error_count += ERROR_SUCCESS != _wfp_createfilter (L"Block all inbound connections (boot-time)", nullptr, 0, FILTER_WEIGHT_LOWEST, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6, nullptr, TRUE, TRUE);
				}

				FwpmTransactionCommit (config.hengine);

				// set icons
				app.SetIcon (IDI_MAIN);
				app.TraySetInfo (_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_MAIN), GetSystemMetrics (SM_CXSMICON)), nullptr);

				SetDlgItemText (app.GetHWND (), IDC_START_BTN, I18N (&app, IDS_TRAY_STOP, 0));

				config.is_filtersinstalled = TRUE;
			}
		}
	}

	return error_count;
}

BOOL _app_logchecklimit ()
{
	const size_t limit = app.ConfigGet (L"LogSizeLimit", 1).AsUint ();

	if (!limit || !config.hlog || config.hlog == INVALID_HANDLE_VALUE)
		return FALSE;

	if (_r_fs_size (config.hlog) >= (limit * _R_BYTESIZE_MB))
	{
		// make backup before log truncate
		if (app.ConfigGet (L"IsLogBackup", TRUE).AsBool ())
		{
			const rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

			_r_fs_delete (path + L".bak");
			_r_fs_copy (path, path + L".bak");
		}

		SetFilePointer (config.hlog, 2, nullptr, FILE_BEGIN);
		SetEndOfFile (config.hlog);

		return TRUE;
	}

	return FALSE;
}

BOOL _app_loginit (BOOL is_install)
{
	// win7 and above
	if (!_r_sys_validversion (6, 1))
		return FALSE;

	// reset all handles
	_R_SPINLOCK (config.lock_writelog);

	if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
	{
		CloseHandle (config.hlog);
		config.hlog = nullptr;
	}

	_R_SPINUNLOCK (config.lock_writelog);

	if (!is_install)
		return TRUE; // already closed

	// check if log enabled
	if (!app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () || !app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ())
		return FALSE;

	BOOL result = FALSE;

	if (is_install)
	{
		const rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

		_R_SPINLOCK (config.lock_writelog);

		config.hlog = CreateFile (path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

		if (config.hlog == INVALID_HANDLE_VALUE)
		{
			_app_logerror (L"CreateFile", GetLastError (), path);
		}
		else
		{
			if (GetLastError () != ERROR_ALREADY_EXISTS)
			{
				DWORD written = 0;
				static const BYTE bom[] = {0xFF, 0xFE};

				WriteFile (config.hlog, bom, sizeof (bom), &written, nullptr); // write utf-16 le byte order mask
			}
			else
			{
				_app_logchecklimit ();

				SetFilePointer (config.hlog, 0, nullptr, FILE_END);
			}

			result = TRUE;
		}

		_R_SPINUNLOCK (config.lock_writelog);
	}

	return result;
}

VOID _app_logwrite (ITEM_LOG const* ptr)
{
	if (!ptr)
		return;

	if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
	{
		_R_SPINLOCK (config.lock_writelog);

		_app_logchecklimit ();

		rstring addr;
		addr.Format (L"%s:%s", ptr->protocol, ((ptr->direction == FWP_DIRECTION_IN) ? ptr->local_addr : ptr->remote_addr));

		if (ptr->direction == FWP_DIRECTION_IN)
		{
			if (ptr->local_port)
				addr.Append (_r_fmt (L":%d", ptr->local_port));

			if ((ptr->flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
				addr.Append (_r_fmt (L" (from %s)", ptr->remote_port ? _r_fmt (L"%s:%d", ptr->remote_addr, ptr->remote_port) : ptr->remote_addr));
		}
		else if (ptr->direction == FWP_DIRECTION_OUT && ptr->remote_port)
		{
			if (ptr->remote_port)
				addr.Append (_r_fmt (L":%d", ptr->remote_port));

			if ((ptr->flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0)
				addr.Append (_r_fmt (L" (from %s)", ptr->local_port ? _r_fmt (L"%s:%d", ptr->local_addr, ptr->local_port) : ptr->local_addr));
		}

		rstring filter;

		if (ptr->provider_name[0])
			filter.Format (L"%s\\%s", ptr->provider_name, ptr->filter_name);
		else
			filter = ptr->filter_name;

		rstring buffer;
		buffer.Format (L"[%s] %s (%s) [%s] %s [%s]\r\n", ptr->date, ptr->full_path, ptr->username, addr, filter, ((ptr->direction == FWP_DIRECTION_IN) ? L"In" : L"Out"));

		DWORD written = 0;
		WriteFile (config.hlog, buffer.GetString (), DWORD (buffer.GetLength () * sizeof (WCHAR)), &written, nullptr);

		_R_SPINUNLOCK (config.lock_writelog);
	}
}

VOID _app_notifysetpos (HWND hwnd, POINT* src_point)
{
	RECT windowRect = {0};

	GetWindowRect (hwnd, &windowRect);

	RECT rc = {0};
	SystemParametersInfo (SPI_GETWORKAREA, 0, &rc, 0);

	if (src_point)
	{
		windowRect.left = src_point->x + (0 - (windowRect.right - windowRect.left)) / 2;
	}
	else
	{
		windowRect.left = rc.right - (windowRect.right - windowRect.left);
	}

	windowRect.top = rc.bottom - (windowRect.bottom - windowRect.top);

	SetWindowPos (hwnd, nullptr, windowRect.left, windowRect.top, 0, 0, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
}

VOID _app_notifycreatewindow ()
{
	WNDCLASSEX wcex = {0};

	wcex.cbSize = sizeof (wcex);
	wcex.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
	wcex.lpfnWndProc = &NotificationProc;
	wcex.hInstance = app.GetHINSTANCE ();
	wcex.hCursor = LoadCursor (nullptr, IDC_ARROW);
	wcex.lpszClassName = NOTIFY_CLASS_DLG;
	wcex.hbrBackground = GetSysColorBrush (COLOR_WINDOW);

	if (!RegisterClassEx (&wcex))
	{
		_app_logerror (L"RegisterClassEx", GetLastError (), nullptr);
	}
	else
	{
		RECT rc = {0};
		SystemParametersInfo (SPI_GETWORKAREA, 0, &rc, 0);

		const UINT width = app.GetDPI (NOTIFY_WIDTH);
		const UINT height = app.GetDPI (NOTIFY_HEIGHT);
		const UINT spacer = app.GetDPI (NOTIFY_SPACER);

		config.hnotification = CreateWindowEx (WS_EX_TOOLWINDOW | WS_EX_TOPMOST, NOTIFY_CLASS_DLG, nullptr, WS_POPUP | WS_BORDER, rc.right - width, rc.bottom - height, width, height, nullptr, nullptr, wcex.hInstance, nullptr);

		if (!config.hnotification)
		{
			_app_logerror (L"CreateWindowEx", GetLastError (), nullptr);
		}
		else
		{
			HWND h = CreateWindowEx (0, WC_STATIC, APP_NAME, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS | SS_CENTERIMAGE, app.GetDPI (10), app.GetDPI (8), app.GetDPI (66), app.GetDPI (20), config.hnotification, (HMENU)IDC_TITLE_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WC_BUTTON, L"x", WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, width - app.GetDPI (20) - (spacer * 2), app.GetDPI (8), app.GetDPI (20), app.GetDPI (20), config.hnotification, (HMENU)IDC_CLOSE_BTN, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER | SS_ICON, app.GetDPI (10), app.GetDPI (48), app.GetDPI (36), app.GetDPI (36), config.hnotification, (HMENU)IDC_ICON_ID, nullptr, nullptr);

			h = CreateWindowEx (0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, app.GetDPI (56), app.GetDPI (48), width - app.GetDPI (66), app.GetDPI (16), config.hnotification, (HMENU)IDC_FILE_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, app.GetDPI (56), app.GetDPI (66), width - app.GetDPI (66), app.GetDPI (16), config.hnotification, (HMENU)IDC_ADDRESS_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, app.GetDPI (56), app.GetDPI (84), width - app.GetDPI (66), app.GetDPI (16), config.hnotification, (HMENU)IDC_FILTER_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, app.GetDPI (56), app.GetDPI (102), width - app.GetDPI (66), app.GetDPI (16), config.hnotification, (HMENU)IDC_DATE_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_CHECKBOX, app.GetDPI (10), app.GetDPI (128), width - app.GetDPI (20), app.GetDPI (16), config.hnotification, (HMENU)IDC_CREATERULE_ADDR_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_CHECKBOX, app.GetDPI (10), app.GetDPI (146), width - app.GetDPI (20), app.GetDPI (16), config.hnotification, (HMENU)IDC_CREATERULE_PORT_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_CHECKBOX, app.GetDPI (10), app.GetDPI (164), width - app.GetDPI (64) - app.GetDPI (4), app.GetDPI (16), config.hnotification, (HMENU)IDC_DISABLENOTIFY_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WC_BUTTON, L"<", WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, app.GetDPI (10), height - app.GetDPI (38), app.GetDPI (24), app.GetDPI (24), config.hnotification, (HMENU)IDC_PREV_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WC_BUTTON, L">", WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, app.GetDPI (10 + 24 + 2), height - app.GetDPI (38), app.GetDPI (24), app.GetDPI (24), config.hnotification, (HMENU)IDC_NEXT_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (0, WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS | SS_CENTERIMAGE, app.GetDPI (10 + 24 + 2 + 24 + 6), height - app.GetDPI (38), app.GetDPI (44), app.GetDPI (24), config.hnotification, (HMENU)IDC_IDX_ID, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);

			h = CreateWindowEx (app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, width - ((app.GetDPI (100) + spacer + app.GetDPI (2)) * 2), height - app.GetDPI (38), app.GetDPI (100), app.GetDPI (24), config.hnotification, (HMENU)IDC_ALLOW_BTN, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);
			SendMessage (h, BM_SETIMAGE, IMAGE_ICON, (WPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_ALLOW), GetSystemMetrics (SM_CXSMICON)));

			h = CreateWindowEx (app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WC_BUTTON, nullptr, WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, width - app.GetDPI (100) - (spacer * 2), height - app.GetDPI (38), app.GetDPI (100), app.GetDPI (24), config.hnotification, (HMENU)IDC_BLOCK_BTN, nullptr, nullptr);
			SendMessage (h, WM_SETFONT, (LPARAM)GetStockObject (DEFAULT_GUI_FONT), true);
			SendMessage (h, BM_SETIMAGE, IMAGE_ICON, (WPARAM)_r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_BLOCK), GetSystemMetrics (SM_CXSMICON)));

			_r_ctrl_settip (config.hnotification, IDC_FILE_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_CREATERULE_ADDR_ID, LPSTR_TEXTCALLBACK);
			_r_ctrl_settip (config.hnotification, IDC_CREATERULE_PORT_ID, LPSTR_TEXTCALLBACK);
		}
	}
}

size_t _app_notifygetcurrent ()
{
	const size_t count = notifications.size ();

	if (count)
	{
		if (count == 1)
			return 0;

		const size_t idx = (size_t)GetWindowLongPtr (config.hnotification, GWLP_USERDATA);
		const size_t new_idx = max (0, min (idx, count - 1));

		SetWindowLongPtr (config.hnotification, GWLP_USERDATA, new_idx);

		return new_idx;
	}

	return LAST_VALUE;
}

bool _app_notifyremove (const size_t idx)
{
	const size_t count = notifications.size ();

	if (!count)
		return false;

	if (idx > (count - 1))
		return false;

	ITEM_LOG* ptr = notifications.at (idx);

	if (ptr)
	{
		if (ptr->hicon)
		{
			DestroyIcon (ptr->hicon);
			ptr->hicon = nullptr;
		}

		SecureZeroMemory (ptr, sizeof (ITEM_LOG));
		free (ptr);
		ptr = nullptr;
	}

	notifications.erase (notifications.begin () + idx);

	_app_notifygetcurrent (); // refresh current id value

	return true;
}

VOID _app_notifycommand (HWND hwnd, bool is_block)
{
	const size_t idx = _app_notifygetcurrent ();

	if (idx == LAST_VALUE)
		return;

	ITEM_LOG* ptr = notifications.at (idx);

	if (!ptr)
		return;

	ITEM_APPLICATION* ptr_app = nullptr;

	const size_t hash = ptr->hash;

	if (hash && apps.find (hash) != apps.end ())
		ptr_app = &apps[hash];

	if (ptr_app && IsDlgButtonChecked (hwnd, IDC_DISABLENOTIFY_ID))
		ptr_app->is_silent = true;

	const bool is_createaddrrule = IsWindowEnabled (GetDlgItem (hwnd, IDC_CREATERULE_ADDR_ID)) && IsDlgButtonChecked (hwnd, IDC_CREATERULE_ADDR_ID);
	const bool is_createportrule = IsWindowEnabled (GetDlgItem (hwnd, IDC_CREATERULE_PORT_ID)) && IsDlgButtonChecked (hwnd, IDC_CREATERULE_PORT_ID);

	if (is_createaddrrule || is_createportrule)
	{
		WCHAR rule[128] = {0};

		if (is_createaddrrule)
			StringCchCopy (rule, _countof (rule), ptr->remote_addr);

		if (is_createportrule)
		{
			if (is_createaddrrule)
				StringCchCat (rule, _countof (rule), L":");

			StringCchCat (rule, _countof (rule), _r_fmt (L"%d", ptr->remote_port));
		}

		size_t rule_id = LAST_VALUE;

		const size_t rule_length = min (wcslen (rule), RULE_RULE_CCH_MAX) + 1;

		for (size_t i = 0; i < rules_custom.size (); i++)
		{
			ITEM_RULE const* rule_ptr = rules_custom.at (i);

			if (rule_ptr)
			{
				if ((rule_ptr->is_block == is_block) && rule_ptr->rule && _wcsnicmp (rule_ptr->rule, rule, rule_length) == 0)
				{
					rule_id = i;
					break;
				}
			}
		}

		if (rule_id == LAST_VALUE)
		{
			// create rule
			ITEM_RULE* rule_ptr = (ITEM_RULE*)malloc (sizeof (ITEM_RULE));

			if (rule_ptr)
			{
				SecureZeroMemory (rule_ptr, sizeof (ITEM_RULE));

				const size_t name_length = min (wcslen (rule), RULE_NAME_CCH_MAX) + 1;
				const size_t path_length = min (wcslen (ptr->full_path), RULE_APPS_CCH_MAX) + 1;

				rule_ptr->name = (LPWSTR)malloc ((name_length + 1) * sizeof (WCHAR));
				rule_ptr->rule = (LPWSTR)malloc ((rule_length + 1) * sizeof (WCHAR));
				rule_ptr->apps = (LPWSTR)malloc ((path_length + 1) * sizeof (WCHAR));

				StringCchCopy (rule_ptr->name, name_length + 1, rule);
				StringCchCopy (rule_ptr->rule, rule_length + 1, rule);
				StringCchCopy (rule_ptr->apps, path_length + 1, ptr->full_path);

				//rule_ptr->protocol = ptr->protocol8;
				rule_ptr->dir = (ptr->direction == FWP_DIRECTION_IN) ? DirInbound : DirOutbound;
				rule_ptr->is_block = is_block ? true : false;
				rule_ptr->is_enabled = false;

				rule_id = rules_custom.size ();
				rules_custom.push_back (rule_ptr);
			}
		}
		else
		{
			// modify rule
		}

		if (hash && (rule_id != LAST_VALUE))
			apps_rules[hash].push_back (rule_id);
	}
	else
	{
		if (!is_block)
		{
			// allow entire app
			if (ptr_app)
			{
				ptr_app->is_enabled = true;

				if (hash)
					_r_listview_setitemcheck (app.GetHWND (), IDC_LISTVIEW, _app_getposition (app.GetHWND (), hash), TRUE);
			}
		}
		else
		{
			// block entire app (do not block entire app, it may be enabled but user was create custom rule affected on app.)

			//if (ptr_app)
			//{
			//	ptr_app->is_enabled = false;

			//	if (hash)
			//		_r_listview_setitemcheck (app.GetHWND (), IDC_LISTVIEW, _app_getposition (app.GetHWND (), hash), FALSE);
			//}
		}
	}

	// clear same notifications (only for "full-enabled")
	if (hash && (!is_createaddrrule && !is_createportrule))
	{
		std::vector<size_t> idx_array;

		for (size_t i = 0; i < notifications.size (); i++)
		{
			ITEM_LOG const* p = notifications.at (i);

			if (i == idx)
				continue;

			if (p && p->hash == hash)
			{
				idx_array.push_back (i);
			}
		}

		for (size_t i = (idx_array.size () - 1); i != LAST_VALUE; i--)
			_app_notifyremove (i);
	}

	_app_notifyremove (idx);

	if (_app_notifygetcurrent () != LAST_VALUE)
	{
		SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_PREV_ID, 0), 0);
	}
	else
	{
		ShowWindow (hwnd, SW_HIDE);
	}

	// apply rules
	{
		SetEvent (config.install_evt);

		_app_profilesave (app.GetHWND ());

		SendDlgItemMessage (app.GetHWND (), IDC_LISTVIEW, LVM_REDRAWITEMS, 0, apps.size ()); // redraw (required!)
	}
}

VOID _app_notifysettimeout (HWND hwnd, BOOL is_create, UINT timeout)
{
	if (is_create)
	{
		SetTimer (hwnd, NOTIFY_TIMER_ID, timeout, nullptr);
	}
	else
	{
		KillTimer (hwnd, NOTIFY_TIMER_ID);
	}
}

VOID _app_notifyshow (size_t idx, POINT* pt)
{
	const size_t total_size = notifications.size ();

	if (!total_size)
		return;

	if (total_size == 1)
		idx = 0;
	else if (idx == LAST_VALUE)
		idx = total_size - 1;

	ITEM_LOG const* ptr = notifications.at (max (0, min (idx, total_size - 1)));

	if (ptr)
	{
		_r_ctrl_settext (config.hnotification, IDC_FILE_ID, L"%s: %s", I18N (&app, IDS_FILE, 0), _r_path_extractfile (ptr->full_path));
		_r_ctrl_settext (config.hnotification, IDC_ADDRESS_ID, L"%s: %s%s [%s]", I18N (&app, IDS_ADDRESS, 0), ptr->remote_addr, (ptr->remote_port ? _r_fmt (L":%d", ptr->remote_port) : L""), I18N (&app, IDS_DIRECTION_1 + ((ptr->direction == FWP_DIRECTION_IN) ? 1 : 0), _r_fmt (L"IDS_DIRECTION_%d", (ptr->direction == FWP_DIRECTION_IN) ? 2 : 1)));
		_r_ctrl_settext (config.hnotification, IDC_DATE_ID, L"%s: %s", I18N (&app, IDS_DATE, 0), ptr->date);
		_r_ctrl_settext (config.hnotification, IDC_FILTER_ID, L"%s: %s", I18N (&app, IDS_FILTER, 0), ptr->filter_name);

		_r_ctrl_settext (config.hnotification, IDC_CREATERULE_ADDR_ID, I18N (&app, IDS_NOTIFY_CREATERULE_ADDRESS, 0), ptr->remote_addr);
		_r_ctrl_settext (config.hnotification, IDC_CREATERULE_PORT_ID, I18N (&app, IDS_NOTIFY_CREATERULE_PORT, 0), ptr->remote_port);

		_r_ctrl_settext (config.hnotification, IDC_DISABLENOTIFY_ID, I18N (&app, IDS_NOTIFY_DISABLENOTIFICATIONS, 0), _r_path_extractfile (ptr->full_path));

		_r_ctrl_settext (config.hnotification, IDC_IDX_ID, L"%d/%d", idx + 1, total_size);

		_r_ctrl_enable (config.hnotification, IDC_CREATERULE_PORT_ID, ptr->remote_port != 0);

		_r_ctrl_settext (config.hnotification, IDC_ALLOW_BTN, I18N (&app, IDS_ACTION_1, 0));
		_r_ctrl_settext (config.hnotification, IDC_BLOCK_BTN, I18N (&app, IDS_ACTION_2, 0));

		SendDlgItemMessage (config.hnotification, IDC_ICON_ID, STM_SETIMAGE, IMAGE_ICON, (WPARAM)(ptr->hicon ? ptr->hicon : config.def_hicon));

		CheckDlgButton (config.hnotification, IDC_CREATERULE_ADDR_ID, BST_UNCHECKED);
		CheckDlgButton (config.hnotification, IDC_CREATERULE_PORT_ID, BST_UNCHECKED);
		CheckDlgButton (config.hnotification, IDC_DISABLENOTIFY_ID, BST_UNCHECKED);

		SetWindowLongPtr (config.hnotification, GWLP_USERDATA, idx);

		_app_notifysetpos (config.hnotification, pt);

		ShowWindow (config.hnotification, SW_SHOW);
	}
}

VOID _app_notifyadd (ITEM_LOG const* ptr)
{
	// do not show notifications if file is not present
	if (!ptr || !ptr->hash)
		return;

	ITEM_APPLICATION* ptr2 = nullptr;

	if (apps.find (ptr->hash) != apps.end ())
		ptr2 = &apps[ptr->hash];

	if (ptr2 && ptr2->is_silent)
		return;

	// check for last display time
	if (ptr2)
	{
		const UINT notification_timeout = app.ConfigGet (L"NotificationsTimeout", NOTIFY_TIMEOUT).AsUint ();

		if (notification_timeout)
		{
			if ((_r_unixtime_now () - ptr2->lastnotified) <= notification_timeout)
				return;
		}
	}

	// notifications limit
	if (notifications.size () >= 10)
		_app_notifyremove (0);

	ITEM_LOG* ptr3 = (ITEM_LOG*)malloc (sizeof (ITEM_LOG));

	if (ptr3)
	{
		memcpy_s (ptr3, sizeof (ITEM_LOG), ptr, sizeof (ITEM_LOG));

		notifications.push_back (ptr3);

		const size_t total_size = notifications.size ();
		const size_t idx = total_size - 1;

		if (total_size > 1)
		{
			_r_ctrl_enable (config.hnotification, IDC_NEXT_ID, TRUE);
			_r_ctrl_enable (config.hnotification, IDC_PREV_ID, TRUE);
		}
		else
		{
			_r_ctrl_enable (config.hnotification, IDC_NEXT_ID, FALSE);
			_r_ctrl_enable (config.hnotification, IDC_PREV_ID, FALSE);
		}

		if (!app.ConfigGet (L"IsNotificationsSilent", FALSE).AsBool ())
			PlaySound (MAKEINTRESOURCE (SND_ALIAS_SYSTEMASTERISK), nullptr, SND_ALIAS_ID | SND_ASYNC);

		_app_notifyshow (idx, nullptr);

		const UINT display_timeout = app.ConfigGet (L"NotificationsDisplayTimeout", NOTIFY_TIMER_DEFAULT).AsUint ();

		if (display_timeout)
			_app_notifysettimeout (config.hnotification, TRUE, (display_timeout * _R_SECONDSCLOCK_MSEC));
	}

	if (ptr2)
		ptr2->lastnotified = _r_unixtime_now ();
}

VOID CALLBACK _app_logcallback (const FILETIME* pft, const UINT8* app_id, SID* user_id, UINT64 filter_id, UINT32 flags, UINT8 proto, FWP_IP_VERSION ipver, BOOL is_loopback, FWP_BYTE_ARRAY16 const* remoteaddr, UINT16 remoteport, FWP_BYTE_ARRAY16 const* localaddr, UINT16 localport, UINT32 direction)
{
	if (!config.is_filtersinstalled)
		return;

	const BOOL is_logenabled = app.ConfigGet (L"IsLogEnabled", FALSE).AsBool ();
	const BOOL is_notificationenabled = app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool ();
	BOOL is_myprovider = FALSE;

	ITEM_LOG log;
	SecureZeroMemory (&log, sizeof (log));

	// copy date and time
	if (pft)
		StringCchCopy (log.date, _countof (log.date), _r_fmt_date (_r_unixtime_from_filetime (pft), FDTF_SHORTDATE | FDTF_LONGTIME));

	// copy converted nt device path into win32
	if (app_id)
	{
		rstring path = _r_path_dospathfromnt (LPCWSTR (app_id));

		log.hash = path.Hash ();
		StringCchCopy (log.full_path, _countof (log.full_path), path);
	}
	else
	{
		log.hash = 0;
		StringCchCopy (log.full_path, _countof (log.full_path), NA_TEXT);
	}

	// apps collector
	if (app_id && log.hash && apps.find (log.hash) == apps.end ())
	{
		_app_addapplication (app.GetHWND (), log.full_path, 0, FALSE, FALSE);

		_app_listviewsort (app.GetHWND (), -1, FALSE);
		_app_profilesave (app.GetHWND ());
	}

	if (!is_logenabled && !is_notificationenabled)
		return;

	// get username (only if log to file enabled)
	if (is_logenabled)
	{
		if ((flags & FWPM_NET_EVENT_FLAG_USER_ID_SET) != 0 && user_id)
		{
			SID_NAME_USE sid_type;

			WCHAR username[MAX_PATH] = {0};
			WCHAR domain[MAX_PATH] = {0};

			DWORD length1 = _countof (username);
			DWORD length2 = _countof (domain);

			if (LookupAccountSid (nullptr, user_id, username, &length1, domain, &length2, &sid_type) && length1 && length2)
			{
				StringCchPrintf (log.username, _countof (log.username), L"%s\\%s", domain, username);
			}
		}

		if (!log.username[0])
			StringCchCopy (log.username, _countof (log.username), NA_TEXT);
	}

	// read filter information
	if (is_logenabled || is_notificationenabled)
	{
		if (filter_id)
		{
			FWPM_FILTER* filter = nullptr;
			FWPM_PROVIDER* provider = nullptr;

			if (FwpmFilterGetById (config.hengine, filter_id, &filter) == ERROR_SUCCESS)
			{
				StringCchCopy (log.filter_name, _countof (log.filter_name), (filter->displayData.description ? filter->displayData.description : filter->displayData.name));

				if (filter->providerKey)
				{
					if (memcmp (filter->providerKey, &GUID_WfpProvider, sizeof (GUID)) == 0)
						is_myprovider = true;

					if (FwpmProviderGetByKey (config.hengine, filter->providerKey, &provider) == ERROR_SUCCESS)
						StringCchCopy (log.provider_name, _countof (log.provider_name), (provider->displayData.description ? provider->displayData.description : provider->displayData.name));
				}
			}

			if (filter)
				FwpmFreeMemory ((LPVOID*)&filter);

			if (provider)
				FwpmFreeMemory ((LPVOID*)&provider);

			if (!log.filter_name[0])
				StringCchPrintf (log.filter_name, _countof (log.filter_name), L"#%d", filter_id);
		}

		if (!log.filter_name[0])
			StringCchCopy (log.filter_name, _countof (log.filter_name), NA_TEXT);
	}

	// protocol
	{
		if (proto && (flags & FWPM_NET_EVENT_FLAG_IP_PROTOCOL_SET) != 0)
		{
			for (size_t i = 0; i < protocols.size (); i++)
			{
				if (protocols.at (i).v == proto)
					StringCchCopy (log.protocol, _countof (log.protocol), protocols.at (i).t);
			}

			log.protocol8 = proto;
		}

		if (!log.protocol[0])
			StringCchCopy (log.protocol, _countof (log.protocol), NA_TEXT);
	}

	if (is_logenabled || is_notificationenabled)
	{
		// ipv4 address
		if (ipver == FWP_IP_VERSION_V4)
		{
			if ((flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0 || (flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
			{
				if (remoteaddr && (flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
				{
					StringCchPrintf (log.remote_addr, _countof (log.remote_addr), L"%d.%d.%d.%d",
						remoteaddr->byteArray16[3],
						remoteaddr->byteArray16[2],
						remoteaddr->byteArray16[1],
						remoteaddr->byteArray16[0]
					);

					if (remoteport && (flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0)
						log.remote_port = remoteport;
				}

				if (localaddr && (flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0)
				{
					StringCchPrintf (log.local_addr, _countof (log.local_addr), L"%d.%d.%d.%d",
						localaddr->byteArray16[3],
						localaddr->byteArray16[2],
						localaddr->byteArray16[1],
						localaddr->byteArray16[0]
					);

					if (localport && (flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0)
						log.local_port = localport;
				}
			}
		}
		else if (ipver == FWP_IP_VERSION_V6)
		{
			if ((flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0 || (flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
			{
				if (remoteaddr && (flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
				{
					StringCchPrintf (log.remote_addr, _countof (log.remote_addr), L"%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
						remoteaddr->byteArray16[0],
						remoteaddr->byteArray16[1],
						remoteaddr->byteArray16[2],
						remoteaddr->byteArray16[3],
						remoteaddr->byteArray16[4],
						remoteaddr->byteArray16[5],
						remoteaddr->byteArray16[6],
						remoteaddr->byteArray16[7],
						remoteaddr->byteArray16[8],
						remoteaddr->byteArray16[9],
						remoteaddr->byteArray16[10],
						remoteaddr->byteArray16[11],
						remoteaddr->byteArray16[12],
						remoteaddr->byteArray16[13],
						remoteaddr->byteArray16[14],
						remoteaddr->byteArray16[15]
					);

					if (remoteport && (flags & FWPM_NET_EVENT_FLAG_REMOTE_PORT_SET) != 0)
						log.remote_port = remoteport;
				}

				if (localaddr && (flags & FWPM_NET_EVENT_FLAG_LOCAL_ADDR_SET) != 0)
				{
					StringCchPrintf (log.local_addr, _countof (log.local_addr), L"%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
						localaddr->byteArray16[0],
						localaddr->byteArray16[1],
						localaddr->byteArray16[2],
						localaddr->byteArray16[3],
						localaddr->byteArray16[4],
						localaddr->byteArray16[5],
						localaddr->byteArray16[6],
						localaddr->byteArray16[7],
						localaddr->byteArray16[8],
						localaddr->byteArray16[9],
						localaddr->byteArray16[10],
						localaddr->byteArray16[11],
						localaddr->byteArray16[12],
						localaddr->byteArray16[13],
						localaddr->byteArray16[14],
						localaddr->byteArray16[15]
					);

					if (localport && (flags & FWPM_NET_EVENT_FLAG_LOCAL_PORT_SET) != 0)
						log.local_port = localport;
				}
			}
		}
	}

	// store flags
	log.flags = flags;

	// indicates whether the packet originated from (or was heading to) the loopback adapter
	log.is_loopback = is_loopback ? true : false;

	// indicates the direction of the packet transmission
	log.direction = direction;

	// write log to the file
	if (is_logenabled)
		_app_logwrite (&log);

	// show notification only for whitelist mode
	if ((app.ConfigGet (L"Mode", ModeWhitelist).AsUint () == ModeWhitelist))
	{
		// show notification (only for my own provider and file is present)
		if (is_notificationenabled && is_myprovider && log.hash && (flags & FWPM_NET_EVENT_FLAG_REMOTE_ADDR_SET) != 0)
		{
			SHFILEINFO shfi = {0};

			CoInitialize (nullptr);

			if (SHGetFileInfo (log.full_path, 0, &shfi, sizeof (shfi), SHGFI_ICON))
				log.hicon = shfi.hIcon;

			CoUninitialize ();

			_app_notifyadd (&log);
		}
	}
}

// win7 callback
VOID CALLBACK _app_logcallback0 (LPVOID, const FWPM_NET_EVENT1 *pEvent)
{
	if (pEvent && pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
	{
		_app_logcallback (&pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.userId, pEvent->classifyDrop->filterId, pEvent->header.flags, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->classifyDrop->isLoopback, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, &pEvent->header.localAddrV6, pEvent->header.localPort, pEvent->classifyDrop->msFwpDirection);
	}
}

// win8 callback
VOID CALLBACK _app_logcallback1 (LPVOID, const FWPM_NET_EVENT2 *pEvent)
{
	if (pEvent && pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
	{
		_app_logcallback (&pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.userId, pEvent->classifyDrop->filterId, pEvent->header.flags, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->classifyDrop->isLoopback, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, &pEvent->header.localAddrV6, pEvent->header.localPort, pEvent->classifyDrop->msFwpDirection);
	}
}

// win10 callback
VOID CALLBACK _app_logcallback2 (LPVOID, const FWPM_NET_EVENT3 *pEvent)
{
	if (pEvent && pEvent->type == FWPM_NET_EVENT_TYPE_CLASSIFY_DROP && pEvent->classifyDrop)
	{
		_app_logcallback (&pEvent->header.timeStamp, pEvent->header.appId.data, pEvent->header.userId, pEvent->classifyDrop->filterId, pEvent->header.flags, pEvent->header.ipProtocol, pEvent->header.ipVersion, pEvent->classifyDrop->isLoopback, &pEvent->header.remoteAddrV6, pEvent->header.remotePort, &pEvent->header.localAddrV6, pEvent->header.localPort, pEvent->classifyDrop->msFwpDirection);
	}
}

VOID _app_flushdnscache ()
{
	HINSTANCE h = LoadLibrary (L"dnsapi.dll");

	if (h)
	{
		static BOOL (WINAPI *DoDnsFlushResolverCache)();

		*(FARPROC *)&DoDnsFlushResolverCache = GetProcAddress (h, "DnsFlushResolverCache");

		if (DoDnsFlushResolverCache)
			DoDnsFlushResolverCache ();

		FreeLibrary (h);
	}
}

UINT WINAPI ApplyThread (LPVOID)
{
	const HANDLE evts[] = {config.stop_evt, config.install_evt, config.destroy_evt};

#ifndef _WIN64
	if (_r_sys_iswow64 ())
		Wow64EnableWow64FsRedirection (FALSE);
#endif

	while (TRUE)
	{
		const DWORD state = WaitForMultipleObjectsEx (_countof (evts), evts, FALSE, INFINITE, FALSE);

		if (state == WAIT_OBJECT_0) // stop event
		{
			SetEvent (config.finish_evt);
			break;
		}
		else if (state == (WAIT_OBJECT_0 + 1)) // install filters event
		{
			_R_SPINLOCK (config.lock_apply);

			_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, FALSE);

			if (app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ())
			{
				if (_wfp_initialize (TRUE))
					_wfp_installfilters ();
			}

			_app_listviewsort (app.GetHWND (), -1, FALSE);
			_app_profilesave (app.GetHWND ());

			_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, TRUE);

			_app_flushdnscache ();

			SendDlgItemMessage (app.GetHWND (), IDC_LISTVIEW, LVM_REDRAWITEMS, 0, apps.size ()); // redraw (required!)

			_R_SPINUNLOCK (config.lock_apply);
		}
		else if (state == (WAIT_OBJECT_0 + 2)) // destroy filters event
		{
			_R_SPINLOCK (config.lock_apply);

			_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, FALSE);

			if (_wfp_initialize (FALSE))
				_wfp_destroyfilters (TRUE);

			_wfp_uninitialize ();

			_r_ctrl_enable (app.GetHWND (), IDC_START_BTN, TRUE);

			_app_flushdnscache ();

			_R_SPINUNLOCK (config.lock_apply);
		}
		else
		{
			break;
		}
	}

	return ERROR_SUCCESS;
}

VOID addcolor (LPCWSTR locale_sid, UINT locale_id, LPCWSTR cfg, bool is_enabled, LPCWSTR config_color, COLORREF default_clr)
{
	ITEM_COLOR color;
	SecureZeroMemory (&color, sizeof (color));

	StringCchCopy (color.config, _countof (color.config), cfg);
	StringCchCopy (color.config_color, _countof (color.config_color), config_color);

	color.is_enabled = is_enabled;
	color.default_clr = default_clr;

	color.locale_id = locale_id;
	StringCchCopy (color.locale_sid, _countof (color.locale_sid), locale_sid);

	colors.push_back (color);
}

VOID addprotocol (LPCWSTR n, UINT8 v)
{
	ITEM_PROTOCOL protocol;
	SecureZeroMemory (&protocol, sizeof (protocol));

	protocol.v = v;
	StringCchCopy (protocol.t, _countof (protocol.t), n);

	protocols.push_back (protocol);
}

HBITMAP _app_ico2bmp (HICON hico)
{
	const INT icon_size = GetSystemMetrics (SM_CXSMICON);

	RECT rc = {0};
	rc.right = icon_size;
	rc.bottom = icon_size;

	HDC hdc = GetDC (nullptr);
	HDC hmemdc = CreateCompatibleDC (hdc);
	HBITMAP hbitmap = CreateCompatibleBitmap (hdc, icon_size, icon_size);
	ReleaseDC (nullptr, hdc);

	HGDIOBJ old_bmp = SelectObject (hmemdc, hbitmap);
	_r_wnd_fillrect (hmemdc, &rc, GetSysColor (COLOR_MENU));
	DrawIconEx (hmemdc, 0, 0, hico, icon_size, icon_size, 0, nullptr, DI_NORMAL);
	SelectObject (hmemdc, old_bmp);

	DeleteDC (hmemdc);

	return hbitmap;
}

VOID _app_getprocesslist (std::vector<ITEM_PROCESS>* pvc)
{
	if (!pvc)
		return;

	// clear previous result
	{
		for (size_t i = 0; i < pvc->size (); i++)
		{
			if (pvc->at (i).hbmp)
				DeleteObject (pvc->at (i).hbmp); // free memory
		}

		pvc->clear ();
	}

	NTSTATUS status = 0;

	ULONG length = 0x4000;
	PVOID buffer = malloc (length);

	while (TRUE)
	{
		status = NtQuerySystemInformation (SystemProcessInformation, buffer, length, &length);

		if (status == 0xC0000023L /*STATUS_BUFFER_TOO_SMALL*/ || status == 0xc0000004 /*STATUS_INFO_LENGTH_MISMATCH*/)
		{
			PVOID buffer_new = realloc (buffer, length);

			if (!buffer_new)
			{
				break;
			}
			else
			{
				buffer = buffer_new;
			}
		}
		else
		{
			break;
		}
	}

	if (NT_SUCCESS (status))
	{
		PSYSTEM_PROCESS_INFORMATION spi = (PSYSTEM_PROCESS_INFORMATION)buffer;

		std::unordered_map<size_t, BOOL> checker;

		do
		{
			const DWORD pid = (DWORD)(DWORD_PTR)spi->UniqueProcessId;

			if (!pid) // skip "system idle process"
				continue;

			const HANDLE hprocess = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

			if (hprocess)
			{
				WCHAR display_path[64] = {0};
				WCHAR real_path[MAX_PATH] = {0};

				size_t hash = 0;

				StringCchPrintf (display_path, _countof (display_path), L"%s (%d)", spi->ImageName.Buffer, pid);

				if (pid == PROC_SYSTEM_PID)
				{
					StringCchCopy (real_path, _countof (real_path), _r_path_expand (PATH_NTOSKRNL));

					hash = _r_str_hash (spi->ImageName.Buffer);
				}
				else
				{
					DWORD size = _countof (real_path) - 1;

					if (QueryFullProcessImageName (hprocess, 0, real_path, &size))
					{
						hash = _r_str_hash (real_path);
					}
					else
					{
						// cannot get file path because it's not filesystem process (Pico maybe?)
						if (GetLastError () == ERROR_GEN_FAILURE)
						{
							StringCchCopy (real_path, _countof (real_path), spi->ImageName.Buffer);
							hash = _r_str_hash (spi->ImageName.Buffer);
						}
						else
						{
							CloseHandle (hprocess);
							continue;
						}
					}
				}

				if (hash && apps.find (hash) == apps.end () && checker.find (hash) == checker.end ())
				{
					checker[hash] = TRUE;

					ITEM_PROCESS item;
					SecureZeroMemory (&item, sizeof (item));

					StringCchCopy (item.display_path, _countof (item.display_path), display_path);
					StringCchCopy (item.real_path, _countof (item.real_path), ((pid == PROC_SYSTEM_PID) ? PROC_SYSTEM_NAME : real_path));

					// get file icon
					{
						SHFILEINFO shfi = {0};

						CoInitialize (nullptr);

						if (SHGetFileInfo (real_path, 0, &shfi, sizeof (shfi), SHGFI_SMALLICON | SHGFI_ICON))
						{
							item.hbmp = _app_ico2bmp (shfi.hIcon);

							DestroyIcon (shfi.hIcon);
						}
						else
						{
							item.hbmp = _app_ico2bmp (config.def_hicon_sm);
						}

						CoUninitialize ();
					}

					pvc->push_back (item);
				}

				CloseHandle (hprocess);
			}
		}
		while ((spi = ((spi->NextEntryOffset ? (PSYSTEM_PROCESS_INFORMATION)((PCHAR)(spi)+(spi)->NextEntryOffset) : nullptr))) != nullptr);
	}

	free (buffer); // free the allocated buffer
}

VOID _app_profileload (HWND hwnd, LPCWSTR path_apps = nullptr, LPCWSTR path_rules = nullptr)
{
	// load applications
	{
		const size_t item_id = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
		const INT scroll_pos = GetScrollPos (GetDlgItem (hwnd, IDC_LISTVIEW), SB_VERT);

		// clear apps
		for (auto &p : apps)
			_app_freeapplication (&p.second, 0);

		for (size_t i = 0; i < rules_blocklist.size (); i++)
			_app_freerule (&rules_blocklist.at (i));

		for (size_t i = 0; i < rules_system.size (); i++)
			_app_freerule (&rules_system.at (i));

		for (size_t i = 0; i < rules_custom.size (); i++)
			_app_freerule (&rules_custom.at (i));

		apps.clear ();
		apps_rules.clear ();
		rules_config.clear ();

		rules_blocklist.clear ();
		rules_system.clear ();
		rules_custom.clear ();

		_r_listview_deleteallitems (hwnd, IDC_LISTVIEW);

		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file (path_apps ? path_apps : config.apps_path, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

		if (!result)
		{
			// show only syntax, memory and i/o errors...
			if (result.status != pugi::status_file_not_found)
				_app_logerror (L"pugi::load_file", 0, _r_fmt (L"status: %d, offset: %d, text: %s, file: %s", result.status, result.offset, rstring (result.description ()), config.apps_path));
		}
		else
		{
			pugi::xml_node root = doc.child (L"root");

			if (root)
			{
				for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
				{
					_app_addapplication (hwnd, item.attribute (L"path").as_string (), item.attribute (L"timestamp").as_ullong (), item.attribute (L"is_silent").as_bool (), item.attribute (L"is_enabled").as_bool ());
				}
			}
		}

		ShowItem (hwnd, IDC_LISTVIEW, item_id, scroll_pos);
	}

	{
		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file (config.rules_config_path, PUGIXML_LOAD_FLAGS, PUGIXML_LOAD_ENCODING);

		if (result)
		{
			pugi::xml_node root = doc.child (L"root");

			if (root)
			{
				for (pugi::xml_node item = root.child (L"item"); item; item = item.next_sibling (L"item"))
				{
					rules_config[item.attribute (L"name").as_string ()] = item.attribute (L"is_enabled").as_bool ();
				}
			}
		}
	}

	// load rules
	_app_loadrules (hwnd, config.blocklist_path, IDR_RULES_BLOCKLIST, TRUE, &rules_blocklist);
	_app_loadrules (hwnd, config.rules_system_path, IDR_RULES_SYSTEM, TRUE, &rules_system);
	_app_loadrules (hwnd, path_rules ? path_rules : config.rules_custom_path, 0, FALSE, &rules_custom);

	_app_listviewresize (hwnd, 0);
	_app_refreshstatus (hwnd, TRUE, FALSE);

	SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_REDRAWITEMS, 0, apps.size ()); // redraw (required!)
}

BOOL _app_installmessage (HWND hwnd, BOOL is_install)
{
	WCHAR text[256] = {0};

	WCHAR mode[64] = {0};
	WCHAR mode1[64] = {0};
	WCHAR mode2[64] = {0};

	WCHAR flag[64] = {0};

	INT result = 0;
	BOOL is_flagchecked = FALSE;
	INT radio_checked = 0;

	TASKDIALOGCONFIG tdc = {0};
	TASKDIALOG_BUTTON tdr[2] = {0};

	tdc.cbSize = sizeof (tdc);
	tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
	tdc.hwndParent = hwnd;
	tdc.pszWindowTitle = APP_NAME;
	tdc.pfCallback = &_r_msg_callback;
	tdc.pszMainIcon = TD_WARNING_ICON;
	tdc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
	tdc.nDefaultButton = IDYES;
	tdc.pszMainInstruction = text;
	tdc.pszVerificationText = flag;

	if (is_install)
	{
		StringCchCopy (text, _countof (text), I18N (&app, IDS_QUESTION_START, 0));
		StringCchCopy (flag, _countof (flag), I18N (&app, IDS_DISABLEWINDOWSFIREWALL_CHK, 0));

		tdc.pszContent = mode;
		tdc.pRadioButtons = tdr;
		tdc.cRadioButtons = _countof (tdr);
		tdc.nDefaultRadioButton = IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint ();

		tdr[0].nButtonID = IDM_TRAY_MODEWHITELIST;
		tdr[0].pszButtonText = mode1;

		tdr[1].nButtonID = IDM_TRAY_MODEBLACKLIST;
		tdr[1].pszButtonText = mode2;

		StringCchCopy (mode, _countof (mode), I18N (&app, IDS_TRAY_MODE, 0) + L":");
		StringCchCopy (mode1, _countof (mode1), I18N (&app, IDS_MODE_WHITELIST, 0));
		StringCchCopy (mode2, _countof (mode2), I18N (&app, IDS_MODE_BLACKLIST, 0));

		if (app.ConfigGet (L"IsDisableWindowsFirewallChecked", TRUE).AsBool ())
			tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
	}
	else
	{
		StringCchCopy (text, _countof (text), I18N (&app, IDS_QUESTION_STOP, 0));
		StringCchCopy (flag, _countof (flag), I18N (&app, IDS_ENABLEWINDOWSFIREWALL_CHK, 0));

		if (app.ConfigGet (L"IsEnableWindowsFirewallChecked", TRUE).AsBool ())
			tdc.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
	}

	TaskDialogIndirect (&tdc, &result, &radio_checked, &is_flagchecked);

	if (result == IDYES)
	{
		if (is_install)
		{
			app.ConfigSet (L"IsDisableWindowsFirewallChecked", is_flagchecked);
			app.ConfigSet (L"IsFiltersEnabled", TRUE);

			app.ConfigSet (L"Mode", ((radio_checked == IDM_TRAY_MODEWHITELIST) ? ModeWhitelist : ModeBlacklist));
			CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint (), MF_BYCOMMAND);

			if (is_flagchecked)
				_mps_changeconfig (TRUE);
		}
		else
		{
			app.ConfigSet (L"IsEnableWindowsFirewallChecked", is_flagchecked);
			app.ConfigSet (L"IsFiltersEnabled", (LONGLONG)FALSE);

			if (is_flagchecked)
				_mps_changeconfig (FALSE);
		}

		return TRUE;
	}

	return FALSE;
}

BOOL initializer_callback (HWND hwnd, DWORD msg, LPVOID, LPVOID)
{
	switch (msg)
	{
		case _RM_ARGUMENTS:
		{
			if (wcsstr (GetCommandLine (), L"/uninstall"))
			{
				const BOOL is_enabled = app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ();

				if (is_enabled)
				{
					if (_app_installmessage (hwnd, FALSE))
					{
						if (_wfp_initialize (FALSE))
							_wfp_destroyfilters (TRUE);

						_wfp_uninitialize ();
					}
				}

				return TRUE;
			}

			break;
		}

		case _RM_INITIALIZE:
		{
			SetWindowText (hwnd, config.title);

			// set icons
			{
				const BOOL state = app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool () && config.is_filtersinstalled;

				app.SetIcon (state ? IDI_MAIN : IDI_INACTIVE);
				app.TrayCreate (hwnd, UID, WM_TRAYICON, _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (state ? IDI_MAIN : IDI_INACTIVE), GetSystemMetrics (SM_CXSMICON)), FALSE);
				SetDlgItemText (hwnd, IDC_START_BTN, I18N (&app, (state ? IDS_TRAY_STOP : IDS_TRAY_START), state ? L"IDS_TRAY_STOP" : L"IDS_TRAY_START"));
			}

			// load profile
			_app_profileload (hwnd);
			_app_listviewsort (hwnd, -1, FALSE);

			if (app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool ())
				SetEvent (config.install_evt);

			_app_loginit (TRUE); // enable dropped packets logging (win7 and above)

			CheckMenuItem (GetMenu (hwnd), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AlwaysOnTop", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_SHOWFILENAMESONLY_CHK, MF_BYCOMMAND | (app.ConfigGet (L"ShowFilenames", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_AUTOSIZECOLUMNS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"AutoSizeColumns", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint (), MF_BYCOMMAND);

			CheckMenuItem (GetMenu (hwnd), IDM_USEBLOCKLIST_CHK, MF_BYCOMMAND | (app.ConfigGet (L"UseBlocklist2", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_STEALTHMODE_CHK, MF_BYCOMMAND | (app.ConfigGet (L"UseStealthMode", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_INSTALLBOOTTIMEFILTERS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			CheckMenuItem (GetMenu (hwnd), IDM_RULE_ALLOWINBOUND, MF_BYCOMMAND | (app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			if (rules_blocklist.empty ())
			{
				EnableMenuItem (GetMenu (hwnd), IDM_USEBLOCKLIST_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
			}

			// append system rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 3);

				// clear menu
				{
					const size_t count = GetMenuItemCount (submenu);

					for (size_t i = count; i != LAST_VALUE; i--)
						DeleteMenu (submenu, UINT (i), MF_BYPOSITION);
				}

				if (rules_system.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM, I18N (&app, IDS_STATUS_EMPTY, 0));
					EnableMenuItem (submenu, IDM_RULES_SYSTEM, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}
				else
				{
					for (size_t i = 0; i < rules_system.size (); i++)
					{
						ITEM_RULE const* ptr = rules_system.at (i);

						if (!ptr)
							continue;

						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM + i, buffer);

						if (rules_config[ptr->name])
							CheckMenuItem (submenu, IDM_RULES_SYSTEM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}
			}

			// append custom rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 4);

				// clear menu
				{
					const size_t count = GetMenuItemCount (submenu);

					for (size_t i = count; i != LAST_VALUE; i--)
						DeleteMenu (submenu, UINT (i), MF_BYPOSITION);
				}

				if (rules_custom.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM, I18N (&app, IDS_STATUS_EMPTY, 0));
					EnableMenuItem (submenu, IDM_RULES_CUSTOM, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}
				else
				{
					for (size_t i = 0; i < rules_custom.size (); i++)
					{
						ITEM_RULE const* ptr = rules_custom.at (i);

						if (!ptr)
							continue;

						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), I18N (&app, IDS_RULE_TITLE_1, 0), ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM + i, buffer);

						if (ptr->is_enabled)
							CheckMenuItem (submenu, IDM_RULES_CUSTOM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}

				AppendMenu (submenu, MF_SEPARATOR, 0, nullptr);
				AppendMenu (submenu, MF_STRING, IDM_OPENRULESEDITOR, I18N (&app, IDS_OPENRULESEDITOR, 0));
			}

			CheckMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
			CheckMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

			// win7 and above
			if (!_r_sys_validversion (6, 1))
			{
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				EnableMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
			}

			break;
		}

		case _RM_LOCALIZE:
		{
			HMENU menu = GetMenu (hwnd);

			app.LocaleMenu (menu, I18N (&app, IDS_FILE, 0), 0, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_ADD_FILE, 0), IDM_ADD_FILE, FALSE);
			app.LocaleMenu (GetSubMenu (menu, 0), I18N (&app, IDS_EXPORT, 0), 2, TRUE);
			app.LocaleMenu (GetSubMenu (menu, 0), I18N (&app, IDS_IMPORT, 0), 3, TRUE);
			app.LocaleMenu (menu, _r_fmt (I18N (&app, IDS_EXPORT_APPS, 0), XML_APPS), IDM_EXPORT_APPS, FALSE);
			app.LocaleMenu (menu, _r_fmt (I18N (&app, IDS_EXPORT_RULES, 0), XML_RULES_CUSTOM), IDM_EXPORT_RULES, FALSE);
			app.LocaleMenu (menu, _r_fmt (I18N (&app, IDS_IMPORT_APPS, 0), XML_APPS), IDM_IMPORT_APPS, FALSE);
			app.LocaleMenu (menu, _r_fmt (I18N (&app, IDS_IMPORT_RULES, 0), XML_RULES_CUSTOM), IDM_IMPORT_RULES, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_EXIT, 0), IDM_EXIT, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_EDIT, 0), 1, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_PURGEN, 0) + L"\tCtrl+Del", IDM_PURGEN, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_FIND, 0) + L"\tCtrl+F", IDM_FIND, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_FINDNEXT, 0) + L"\tF3", IDM_FINDNEXT, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_REFRESH, 0) + L"\tF5", IDM_REFRESH, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_VIEW, 0), 2, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_ALWAYSONTOP_CHK, 0), IDM_ALWAYSONTOP_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_SHOWFILENAMESONLY_CHK, 0), IDM_SHOWFILENAMESONLY_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_AUTOSIZECOLUMNS_CHK, 0), IDM_AUTOSIZECOLUMNS_CHK, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 2), I18N (&app, IDS_ICONS, 0), 4, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_ICONSSMALL, 0), IDM_ICONSSMALL, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ICONSLARGE, 0), IDM_ICONSLARGE, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ICONSISHIDDEN, 0), IDM_ICONSISHIDDEN, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 2), I18N (&app, IDS_LANGUAGE, 0), LANG_MENU, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_SETTINGS, 0), 3, TRUE);

			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_MODE, 0), 0, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_MODE_WHITELIST, 0), IDM_TRAY_MODEWHITELIST, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_MODE_BLACKLIST, 0), IDM_TRAY_MODEBLACKLIST, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_FILTERS, 0), 2, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_USEBLOCKLIST_CHK, 0), IDM_USEBLOCKLIST_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_USESTEALTHMODE_CHK, 0), IDM_STEALTHMODE_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_CHK, 0), IDM_INSTALLBOOTTIMEFILTERS_CHK, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_RULE_ALLOWINBOUND, 0), IDM_RULE_ALLOWINBOUND, FALSE);

			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_SYSTEM_RULES, 0), 3, TRUE);
			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_CUSTOM_RULES, 0), 4, TRUE);
			app.LocaleMenu (GetSubMenu (menu, 3), I18N (&app, IDS_TRAY_LOG, 0), 5, TRUE);

			app.LocaleMenu (menu, I18N (&app, IDS_ENABLELOG_CHK, 0), IDM_ENABLELOG_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ENABLENOTIFICATIONS_CHK, 0), IDM_ENABLENOTIFICATIONS_CHK, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_LOGSHOW, 0) + L"\tCtrl+I", IDM_LOGSHOW, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_LOGCLEAR, 0) + L"\tCtrl+X", IDM_LOGCLEAR, FALSE);

			app.LocaleMenu (menu, I18N (&app, IDS_SETTINGS, 0) + L"\tCtrl+P", IDM_SETTINGS, FALSE);

			// append system rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 3);

				// clear menu
				{
					const size_t count = GetMenuItemCount (submenu);

					for (size_t i = count; i != LAST_VALUE; i--)
						DeleteMenu (submenu, UINT (i), MF_BYPOSITION);
				}

				if (rules_system.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM, I18N (&app, IDS_STATUS_EMPTY, 0));
					EnableMenuItem (submenu, IDM_RULES_SYSTEM, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}
				else
				{
					for (size_t i = 0; i < rules_system.size (); i++)
					{
						ITEM_RULE const* ptr = rules_system.at (i);

						if (!ptr)
							continue;

						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_SYSTEM + i, buffer);

						if (rules_config[ptr->name])
							CheckMenuItem (submenu, IDM_RULES_SYSTEM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}
			}

			// append custom rules
			{
				HMENU submenu = GetSubMenu (GetSubMenu (GetMenu (hwnd), 3), 4);

				// clear menu
				{
					const size_t count = GetMenuItemCount (submenu);

					for (size_t i = count; i != LAST_VALUE; i--)
						DeleteMenu (submenu, UINT (i), MF_BYPOSITION);
				}

				if (rules_custom.empty ())
				{
					AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM, I18N (&app, IDS_STATUS_EMPTY, 0));
					EnableMenuItem (submenu, IDM_RULES_CUSTOM, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}
				else
				{
					for (size_t i = 0; i < rules_custom.size (); i++)
					{
						ITEM_RULE const* ptr = rules_custom.at (i);

						if (!ptr)
							continue;

						WCHAR buffer[128] = {0};
						StringCchPrintf (buffer, _countof (buffer), I18N (&app, IDS_RULE_TITLE_1, 0), ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

						AppendMenu (submenu, MF_STRING, IDM_RULES_CUSTOM + i, buffer);

						if (ptr->is_enabled)
							CheckMenuItem (submenu, IDM_RULES_CUSTOM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
					}
				}

				AppendMenu (submenu, MF_SEPARATOR, 0, nullptr);
				AppendMenu (submenu, MF_STRING, IDM_OPENRULESEDITOR, I18N (&app, IDS_OPENRULESEDITOR, 0));
			}

			app.LocaleMenu (menu, I18N (&app, IDS_HELP, 0), 4, TRUE);
			app.LocaleMenu (menu, I18N (&app, IDS_WEBSITE, 0), IDM_WEBSITE, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_DONATE, 0), IDM_DONATE, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_CHECKUPDATES, 0), IDM_CHECKUPDATES, FALSE);
			app.LocaleMenu (menu, I18N (&app, IDS_ABOUT, 0), IDM_ABOUT, FALSE);

			app.LocaleEnum ((HWND)GetSubMenu (menu, 2), LANG_MENU, TRUE, IDM_LANGUAGE); // enum localizations

			SetDlgItemText (hwnd, IDC_START_BTN, I18N (&app, (app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool () && config.is_filtersinstalled ? IDS_TRAY_STOP : IDS_TRAY_START), app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool () && config.is_filtersinstalled ? L"IDS_TRAY_STOP" : L"IDS_TRAY_START"));
			SetDlgItemText (hwnd, IDC_SETTINGS_BTN, I18N (&app, IDS_SETTINGS, 0));
			SetDlgItemText (hwnd, IDC_EXIT_BTN, I18N (&app, IDS_EXIT, 0));

			_r_wnd_addstyle (hwnd, IDC_START_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_SETTINGS_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_EXIT_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			_r_wnd_addstyle (config.hnotification, IDC_ALLOW_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (config.hnotification, IDC_BLOCK_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (config.hnotification, IDC_CLOSE_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (config.hnotification, IDC_NEXT_ID, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (config.hnotification, IDC_PREV_ID, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			_app_listviewresize (hwnd, 0);

			_app_refreshstatus (hwnd, TRUE, TRUE); // refresh statusbar

			_r_listview_setcolumn (hwnd, IDC_LISTVIEW, 0, I18N (&app, IDS_FILEPATH, 0), 0);
			_r_listview_setcolumn (hwnd, IDC_LISTVIEW, 1, I18N (&app, IDS_ADDED, 0), 0);

			SendDlgItemMessage (hwnd, IDC_LISTVIEW, (LVM_FIRST + 84), 0, 0); // LVM_RESETEMPTYTEXT

			break;
		}

		case _RM_UNINITIALIZE:
		{
			_app_loginit (FALSE); // disable dropped packets logging (win7 and above)
			app.TrayDestroy (UID);

			break;
		}
	}

	return FALSE;
}

INT_PTR CALLBACK EditorProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	static ITEM_RULE* ptr = nullptr;

	switch (msg)
	{
		case WM_INITDIALOG:
		{
			ptr = (ITEM_RULE*)lparam;

			// configure window
			_r_wnd_center (hwnd);

			// localize window
			SetWindowText (hwnd, I18N (&app, IDS_EDITOR, 0));

			SetDlgItemText (hwnd, IDC_NAME, I18N (&app, IDS_NAME, 0) + L":");
			SetDlgItemText (hwnd, IDC_RULES, I18N (&app, IDS_RULE, 0) + L":");
			SetDlgItemText (hwnd, IDC_DIRECTION, I18N (&app, IDS_DIRECTION, 0) + L":");
			SetDlgItemText (hwnd, IDC_PROTOCOL, I18N (&app, IDS_PROTOCOL, 0) + L":");
			SetDlgItemText (hwnd, IDC_IPVERSION, I18N (&app, IDS_IPVERSION, 0) + L":");
			SetDlgItemText (hwnd, IDC_ACTION, I18N (&app, IDS_ACTION, 0) + L":");

			SetDlgItemText (hwnd, IDC_RULES_HELP, I18N (&app, IDS_RULES_HELP, 0));
			SetDlgItemText (hwnd, IDC_ENABLED_CHK, I18N (&app, IDS_ENABLED_CHK, 0));

			SetDlgItemText (hwnd, IDC_APPLY, I18N (&app, IDS_APPLY, 0));
			SetDlgItemText (hwnd, IDC_CLOSE, I18N (&app, IDS_CLOSE, 0));

			_r_wnd_addstyle (hwnd, IDC_APPLY, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
			_r_wnd_addstyle (hwnd, IDC_CLOSE, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

			// set data
			if (ptr->name)
				SetDlgItemText (hwnd, IDC_NAME_EDIT, ptr->name);

			if (ptr->rule)
				SetDlgItemText (hwnd, IDC_RULES_EDIT, ptr->rule);

			// direction
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_DIRECTION_1, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 1, (LPARAM)I18N (&app, IDS_DIRECTION_2, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_INSERTSTRING, 2, (LPARAM)I18N (&app, IDS_DIRECTION_3, 0).GetString ());

			SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_SETCURSEL, (WPARAM)ptr->dir, 0);

			// protocol
			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_ALL, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETCURSEL, 0, 0);

			for (size_t i = 0; i < protocols.size (); i++)
			{
				SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_INSERTSTRING, i + 1, (LPARAM)protocols.at (i).t);
				SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETITEMDATA, i + 1, (LPARAM)protocols.at (i).v);

				if (ptr->protocol == protocols.at (i).v)
					SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_SETCURSEL, (WPARAM)i + 1, 0);
			}

			// address family
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_ALL, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETITEMDATA, 0, (LPARAM)AF_UNSPEC);

			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_INSERTSTRING, 1, (LPARAM)L"IPv4");
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETITEMDATA, 1, (LPARAM)AF_INET);

			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_INSERTSTRING, 2, (LPARAM)L"IPv6");
			SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETITEMDATA, 2, (LPARAM)AF_INET6);

			if (ptr->version == AF_UNSPEC)
				SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETCURSEL, (WPARAM)0, 0);
			else if (ptr->version == AF_INET)
				SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETCURSEL, (WPARAM)1, 0);
			else if (ptr->version == AF_INET6)
				SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_SETCURSEL, (WPARAM)2, 0);

			// action
			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_INSERTSTRING, 0, (LPARAM)I18N (&app, IDS_ACTION_1, 0).GetString ());
			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_INSERTSTRING, 1, (LPARAM)I18N (&app, IDS_ACTION_2, 0).GetString ());

			SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_SETCURSEL, (WPARAM)ptr->is_block, 0);

			// state
			CheckDlgButton (hwnd, IDC_ENABLED_CHK, ptr->is_enabled ? BST_CHECKED : BST_UNCHECKED);

			// set limitation
			SendDlgItemMessage (hwnd, IDC_NAME_EDIT, EM_LIMITTEXT, RULE_NAME_CCH_MAX - 1, 0);
			SendDlgItemMessage (hwnd, IDC_RULES_EDIT, EM_LIMITTEXT, RULE_RULE_CCH_MAX - 1, 0);

			_r_ctrl_enable (hwnd, IDC_APPLY, FALSE); // disable apply button

			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case NM_CLICK:
				case NM_RETURN:
				{
					if (nmlp->idFrom == IDC_RULES_HELP)
					{
						PNMLINK nmlink = (PNMLINK)lparam;

						if (nmlink->item.szUrl)
							ShellExecute (hwnd, nullptr, nmlink->item.szUrl, nullptr, nullptr, SW_SHOWDEFAULT);
					}

					break;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			{
				const BOOL is_button = (GetWindowLongPtr (GetDlgItem (hwnd, LOWORD (wparam)), GWL_STYLE) & (BS_CHECKBOX | BS_RADIOBUTTON)) != 0;

				if ((HIWORD (wparam) == BN_CLICKED && is_button) || HIWORD (wparam) == EN_CHANGE || HIWORD (wparam) == CBN_SELENDOK)
				{
					const BOOL is_enable = (SendDlgItemMessage (hwnd, IDC_NAME_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0) && (SendDlgItemMessage (hwnd, IDC_RULES_EDIT, WM_GETTEXTLENGTH, 0, 0) > 0);

					_r_ctrl_enable (hwnd, IDC_APPLY, is_enable); // enable apply button

					return FALSE;
				}
			}

			switch (LOWORD (wparam))
			{
				case IDOK: // process Enter key
				case IDC_APPLY:
				{
					// if not enabled - nothing is changed!
					if (!IsWindowEnabled (GetDlgItem (hwnd, IDC_APPLY)))
						return FALSE;

					// destination of the rule
					{
						rstring rule = _r_ctrl_gettext (hwnd, IDC_RULES_EDIT);

						if (!rule.IsEmpty ())
						{
							const size_t rule_length = min (rule.GetLength (), RULE_RULE_CCH_MAX) + 1;
							const size_t new_sizeb = (rule_length + 1) * sizeof (WCHAR);

							// here we parse and check rule syntax
							{
								rstring::rvector arr = rule.AsVector (RULE_DELIMETER);

								for (size_t i = 0; i < arr.size (); i++)
								{
									rstring rule_single = arr.at (i).Trim (L"\r\n ");

									if (rule_single.IsEmpty ())
										continue;

									if (!_app_parserulestring (rule_single, nullptr, nullptr))
									{
										_r_ctrl_showtip (hwnd, IDC_RULES_EDIT, APP_NAME, _r_fmt (I18N (&app, IDS_STATUS_SYNTAX_ERROR, 0), rule_single), TTI_ERROR);
										_r_ctrl_enable (hwnd, IDC_APPLY, FALSE);

										return FALSE;
									}
								}
							}

							if (ptr->rule)
							{
								LPVOID tmp_ptr = realloc (ptr->rule, new_sizeb);

								if (tmp_ptr)
								{
									ptr->rule = (LPWSTR)tmp_ptr;
								}
								else
								{
									free (ptr->rule);
									ptr->rule = (LPWSTR)malloc (new_sizeb);
								}
							}
							else
							{
								ptr->rule = (LPWSTR)malloc (new_sizeb);
							}

							StringCchCopy (ptr->rule, rule_length, rule);
						}
					}

					// name of the rule
					{
						rstring name = _r_ctrl_gettext (hwnd, IDC_NAME_EDIT);

						if (!name.IsEmpty ())
						{
							const size_t name_length = min (name.GetLength (), RULE_NAME_CCH_MAX) + 1;
							const size_t new_sizeb = (name_length + 1) * sizeof (WCHAR);

							if (ptr->name)
							{
								LPVOID tmp_ptr = realloc (ptr->name, new_sizeb);

								if (tmp_ptr)
								{
									ptr->name = (LPWSTR)tmp_ptr;
								}
								else
								{
									free (ptr->name);
									ptr->name = (LPWSTR)malloc (new_sizeb);
								}
							}
							else
							{
								ptr->name = (LPWSTR)malloc (new_sizeb);
							}

							StringCchCopy (ptr->name, name_length, name);
						}
					}

					ptr->protocol = (UINT8)SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_GETITEMDATA, SendDlgItemMessage (hwnd, IDC_PROTOCOL_EDIT, CB_GETCURSEL, 0, 0), 0);
					ptr->version = (ADDRESS_FAMILY)SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_GETITEMDATA, SendDlgItemMessage (hwnd, IDC_IPVERSION_EDIT, CB_GETCURSEL, 0, 0), 0);

					ptr->dir = (EnumRuleDirection)SendDlgItemMessage (hwnd, IDC_DIRECTION_EDIT, CB_GETCURSEL, 0, 0);
					ptr->is_block = SendDlgItemMessage (hwnd, IDC_ACTION_EDIT, CB_GETCURSEL, 0, 0) ? true : false;
					ptr->is_enabled = IsDlgButtonChecked (hwnd, IDC_ENABLED_CHK) == BST_CHECKED;

					EndDialog (hwnd, 1);

					break;
				}

				case IDCANCEL: // process Esc key
				case IDC_CLOSE:
				{
					EndDialog (hwnd, 0);
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

BOOL settings_callback (HWND hwnd, DWORD msg, LPVOID lpdata1, LPVOID lpdata2)
{
	PAPP_SETTINGS_PAGE const page = (PAPP_SETTINGS_PAGE)lpdata2;

	switch (msg)
	{
		case _RM_INITIALIZE:
		{
			// localize titles
			SetDlgItemText (hwnd, IDC_TITLE_1, I18N (&app, IDS_TITLE_1, 0));
			SetDlgItemText (hwnd, IDC_TITLE_2, I18N (&app, IDS_TITLE_2, 0));
			SetDlgItemText (hwnd, IDC_TITLE_3, I18N (&app, IDS_TITLE_3, 0));
			SetDlgItemText (hwnd, IDC_TITLE_4, I18N (&app, IDS_TITLE_4, 0));
			SetDlgItemText (hwnd, IDC_TITLE_5, I18N (&app, IDS_TITLE_5, 0));
			SetDlgItemText (hwnd, IDC_TITLE_6, I18N (&app, IDS_TITLE_6, 0));
			SetDlgItemText (hwnd, IDC_TITLE_7, I18N (&app, IDS_TITLE_7, 0));

			switch (page->dlg_id)
			{
				case IDD_SETTINGS_1:
				{
					// localize
					SetDlgItemText (hwnd, IDC_ALWAYSONTOP_CHK, I18N (&app, IDS_ALWAYSONTOP_CHK, 0));
					SetDlgItemText (hwnd, IDC_LOADONSTARTUP_CHK, I18N (&app, IDS_LOADONSTARTUP_CHK, 0));
					SetDlgItemText (hwnd, IDC_SKIPUACWARNING_CHK, I18N (&app, IDS_SKIPUACWARNING_CHK, 0));
					SetDlgItemText (hwnd, IDC_CHECKUPDATES_CHK, I18N (&app, IDS_CHECKUPDATES_CHK, 0));

					SetDlgItemText (hwnd, IDC_LANGUAGE_HINT, I18N (&app, IDS_LANGUAGE_HINT, 0));

					CheckDlgButton (hwnd, IDC_ALWAYSONTOP_CHK, app.ConfigGet (L"AlwaysOnTop", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

#ifdef _APP_HAVE_AUTORUN
					CheckDlgButton (hwnd, IDC_LOADONSTARTUP_CHK, app.AutorunIsEnabled () ? BST_CHECKED : BST_UNCHECKED);
#endif // _APP_HAVE_AUTORUN

#ifdef _APP_HAVE_SKIPUAC
					CheckDlgButton (hwnd, IDC_SKIPUACWARNING_CHK, app.SkipUacIsEnabled () ? BST_CHECKED : BST_UNCHECKED);
#endif // _APP_HAVE_SKIPUAC

					CheckDlgButton (hwnd, IDC_CHECKUPDATES_CHK, app.ConfigGet (L"CheckUpdates", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					app.LocaleEnum (hwnd, IDC_LANGUAGE, FALSE, 0);

					SetWindowLongPtr (hwnd, GWLP_USERDATA, (LONG_PTR)SendDlgItemMessage (hwnd, IDC_LANGUAGE, CB_GETCURSEL, 0, 0)); // check on save

					break;
				}

				case IDD_SETTINGS_2:
				{
					// localize
					SetDlgItemText (hwnd, IDC_USEBLOCKLIST_CHK, I18N (&app, IDS_USEBLOCKLIST_CHK, 0));
					SetDlgItemText (hwnd, IDC_USEBLOCKLIST_HINT, I18N (&app, IDS_USEBLOCKLIST_HINT, 0));

					SetDlgItemText (hwnd, IDC_USESTEALTHMODE_CHK, I18N (&app, IDS_USESTEALTHMODE_CHK, 0));
					SetDlgItemText (hwnd, IDC_USESTEALTHMODE_HINT, I18N (&app, IDS_USESTEALTHMODE_HINT, 0));

					SetDlgItemText (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_CHK, 0));
					SetDlgItemText (hwnd, IDC_INSTALLBOOTTIMEFILTERS_HINT, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_HINT, 0));

					SetDlgItemText (hwnd, IDC_RULE_ALLOWINBOUND, I18N (&app, IDS_RULE_ALLOWINBOUND, 0));
					SetDlgItemText (hwnd, IDC_RULE_ALLOWINBOUND_HINT, I18N (&app, IDS_RULE_ALLOWINBOUND_HINT, 0));

					CheckDlgButton (hwnd, IDC_USEBLOCKLIST_CHK, app.ConfigGet (L"UseBlocklist2", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_USESTEALTHMODE_CHK, app.ConfigGet (L"UseStealthMode", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK, app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					CheckDlgButton (hwnd, IDC_RULE_ALLOWINBOUND, app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					if (rules_blocklist.empty ())
						_r_ctrl_enable (hwnd, IDC_USEBLOCKLIST_CHK, FALSE);

					break;
				}

				case IDD_SETTINGS_3:
				{
					// localize
					SetDlgItemText (hwnd, IDC_CONFIRMEXIT_CHK, I18N (&app, IDS_CONFIRMEXIT_CHK, 0));
					SetDlgItemText (hwnd, IDC_CONFIRMDELETE_CHK, I18N (&app, IDS_CONFIRMDELETE_CHK, 0));
					SetDlgItemText (hwnd, IDC_CONFIRMLOGCLEAR_CHK, I18N (&app, IDS_CONFIRMLOGCLEAR_CHK, 0));

					SetDlgItemText (hwnd, IDC_COLORS_HINT, I18N (&app, IDS_COLORS_HINT, 0));

					CheckDlgButton (hwnd, IDC_CONFIRMEXIT_CHK, app.ConfigGet (L"ConfirmExit", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_CONFIRMDELETE_CHK, app.ConfigGet (L"ConfirmDelete", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_CONFIRMLOGCLEAR_CHK, app.ConfigGet (L"ConfirmLogClear", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					// configure listview
					_r_listview_setstyle (hwnd, IDC_COLORS, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

					_r_listview_deleteallitems (hwnd, IDC_COLORS);
					_r_listview_deleteallcolumns (hwnd, IDC_COLORS);

					_r_listview_addcolumn (hwnd, IDC_COLORS, nullptr, 95, 0, LVCFMT_LEFT);

					for (size_t i = 0; i < colors.size (); i++)
					{
						colors.at (i).clr = app.ConfigGet (colors.at (i).config_color, colors.at (i).default_clr).AsUlong ();

						_r_listview_additem (hwnd, IDC_COLORS, I18N (&app, colors.at (i).locale_id, colors.at (i).locale_sid), i, 0, LAST_VALUE, LAST_VALUE, i);

						if (app.ConfigGet (colors.at (i).config, colors.at (i).is_enabled).AsBool ())
							_r_listview_setitemcheck (hwnd, IDC_COLORS, i, TRUE);
					}

					break;
				}

				case IDD_SETTINGS_4:
				{
					// localize
					SetDlgItemText (hwnd, IDC_ENABLELOG_CHK, I18N (&app, IDS_ENABLELOG_CHK, 0));

					SetDlgItemText (hwnd, IDC_LOGSIZELIMIT_HINT, I18N (&app, IDS_LOGSIZELIMIT_HINT, 0));
					SetDlgItemText (hwnd, IDC_LOGBACKUP_CHK, I18N (&app, IDS_LOGBACKUP_CHK, 0));

					SetDlgItemText (hwnd, IDC_ENABLENOTIFICATIONS_CHK, I18N (&app, IDS_ENABLENOTIFICATIONS_CHK, 0));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONSILENT_CHK, I18N (&app, IDS_NOTIFICATIONSILENT_CHK, 0));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT_HINT, I18N (&app, IDS_NOTIFICATIONDISPLAYTIMEOUT_HINT, 0));
					SetDlgItemText (hwnd, IDC_NOTIFICATIONTIMEOUT_HINT, I18N (&app, IDS_NOTIFICATIONTIMEOUT_HINT, 0));

					// win7 and above
					if (!_r_sys_validversion (6, 1))
					{
						_r_ctrl_enable (hwnd, IDC_ENABLELOG_CHK, FALSE);
						_r_ctrl_enable (hwnd, IDC_ENABLENOTIFICATIONS_CHK, FALSE);
					}

					CheckDlgButton (hwnd, IDC_ENABLELOG_CHK, app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					SetDlgItemText (hwnd, IDC_LOGPATH, app.ConfigGet (L"LogPath", PATH_LOG));

					SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_SETRANGE32, 1, 12);
					SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_SETPOS32, 0, app.ConfigGet (L"LogSizeLimit", 1).AsUint ());

					CheckDlgButton (hwnd, IDC_LOGBACKUP_CHK, app.ConfigGet (L"IsLogBackup", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					CheckDlgButton (hwnd, IDC_ENABLENOTIFICATIONS_CHK, app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool () ? BST_CHECKED : BST_UNCHECKED);
					CheckDlgButton (hwnd, IDC_NOTIFICATIONSILENT_CHK, app.ConfigGet (L"IsNotificationsSilent", FALSE).AsBool () ? BST_CHECKED : BST_UNCHECKED);

					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_SETRANGE32, 0, _R_SECONDSCLOCK_HOUR (1));
					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_SETPOS32, 0, app.ConfigGet (L"NotificationsDisplayTimeout", NOTIFY_TIMER_DEFAULT).AsUint ());

					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_SETRANGE32, 0, _R_SECONDSCLOCK_HOUR (1));
					SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_SETPOS32, 0, app.ConfigGet (L"NotificationsTimeout", NOTIFY_TIMEOUT).AsUint ());

					_r_wnd_addstyle (hwnd, IDC_LOGPATH_BTN, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);
					_r_wnd_addstyle (hwnd, IDC_LOGPATH_BTN2, app.IsClassicUI () ? WS_EX_STATICEDGE : 0, WS_EX_STATICEDGE, GWL_EXSTYLE);

					SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_ENABLELOG_CHK, 0), 0);
					SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDC_ENABLENOTIFICATIONS_CHK, 0), 0);

					break;
				}

				case IDD_SETTINGS_5:
				case IDD_SETTINGS_6:
				case IDD_SETTINGS_7:
				{
					// localize
					SetDlgItemText (hwnd, IDC_RULES_BLOCKLIST_HINT, I18N (&app, IDS_RULES_BLOCKLIST_HINT, 0));
					SetDlgItemText (hwnd, IDC_RULES_SYSTEM_HINT, I18N (&app, IDS_RULES_SYSTEM_HINT, 0));
					SetDlgItemText (hwnd, IDC_RULES_CUSTOM_HINT, I18N (&app, IDS_RULES_CUSTOM_HINT, 0));

					// configure listview
					_r_listview_setstyle (hwnd, IDC_EDITOR, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

					const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)config.himg);
					SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_SETIMAGELIST, LVSIL_NORMAL, (LPARAM)config.himg);

					const BOOL group1_collapsed = (SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETGROUPSTATE, 0, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0;
					const BOOL group2_collapsed = (SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETGROUPSTATE, 1, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0;

					_r_listview_deleteallitems (hwnd, IDC_EDITOR);
					_r_listview_deleteallgroups (hwnd, IDC_EDITOR);
					_r_listview_deleteallcolumns (hwnd, IDC_EDITOR);

					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_NAME, 0), 40, 1, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_DIRECTION, 0), 22, 2, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_PROTOCOL, 0), 20, 3, LVCFMT_LEFT);
					_r_listview_addcolumn (hwnd, IDC_EDITOR, I18N (&app, IDS_IPVERSION, 0), 14, 4, LVCFMT_LEFT);

					_r_listview_addgroup (hwnd, IDC_EDITOR, I18N (&app, IDS_GROUP_ENABLED, 0), 0, 0, LVGS_COLLAPSIBLE | (group1_collapsed ? LVGS_COLLAPSED : 0));
					_r_listview_addgroup (hwnd, IDC_EDITOR, I18N (&app, IDS_GROUP_DISABLED, 0), 1, 0, LVGS_COLLAPSIBLE | (group2_collapsed ? LVGS_COLLAPSED : 0));

					std::vector<ITEM_RULE*> const* ptr = nullptr;

					if (page->dlg_id == IDD_SETTINGS_5)
					{
						ptr = &rules_system;
					}
					else if (page->dlg_id == IDD_SETTINGS_6)
					{
						ptr = &rules_custom;
					}
					else if (page->dlg_id == IDD_SETTINGS_7)
					{
						ptr = &rules_blocklist;
					}

					if (ptr)
					{
						for (size_t i = 0; i < ptr->size (); i++)
						{
							ITEM_RULE const* ptr2 = ptr->at (i);

							if (!ptr2)
								continue;

							rstring dir = I18N (&app, IDS_DIRECTION_1 + ptr2->dir, _r_fmt (L"IDS_DIRECTION_%d", ptr2->dir + 1));
							rstring protocol = I18N (&app, IDS_ALL, 0);
							rstring af = protocol;

							// protocol
							for (size_t j = 0; j < protocols.size (); j++)
							{
								if (ptr2->protocol == protocols.at (j).v)
									protocol = protocols.at (j).t;
							}

							// address family
							if (ptr2->version == AF_INET)
								af = L"IPv4";
							if (ptr2->version == AF_INET6)
								af = L"IPv6";

							_r_listview_additem (hwnd, IDC_EDITOR, ptr2->name, i, 0, ptr2->is_block ? 1 : 0, ptr2->is_enabled ? 0 : 1, i);
							_r_listview_additem (hwnd, IDC_EDITOR, dir, i, 1);
							_r_listview_additem (hwnd, IDC_EDITOR, protocol, i, 2);
							_r_listview_additem (hwnd, IDC_EDITOR, af, i, 3);

							_r_listview_setitemcheck (hwnd, IDC_EDITOR, i, ptr2->is_enabled);
						}

						ShowItem (hwnd, IDC_EDITOR, item, -1);

						SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_REDRAWITEMS, 0, ptr->size ()); // redraw (required!)
					}

					break;
				}
			}

			break;
		}

		case _RM_MESSAGE:
		{
			LPMSG pmsg = (LPMSG)lpdata1;

			switch (pmsg->message)
			{
				case WM_NOTIFY:
				{
					LPNMHDR nmlp = (LPNMHDR)pmsg->lParam;

					switch (nmlp->code)
					{
						case LVN_GETINFOTIP:
						{
							if (nmlp->idFrom != IDC_EDITOR)
								break;

							LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)pmsg->lParam;

							ITEM_RULE const* ptr = nullptr;

							const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, lpnmlv->iItem);

							if (page->dlg_id == IDD_SETTINGS_6)
								ptr = rules_custom[idx];
							else if (page->dlg_id == IDD_SETTINGS_7)
								ptr = rules_blocklist[idx];
							else if (page->dlg_id == IDD_SETTINGS_5)
								ptr = rules_system[idx];

							if (ptr)
							{
								rstring rule = ptr->rule;

								if (rule.IsEmpty ())
									rule = I18N (&app, IDS_STATUS_EMPTY, 0);
								else
									rule.Replace (RULE_DELIMETER, L"\r\n" TAB_SPACE);

								StringCchPrintf (lpnmlv->pszText, lpnmlv->cchTextMax, L"%s\r\n%s:\r\n%s%s", ptr->name, I18N (&app, IDS_RULE, 0), TAB_SPACE, rule);

								if (ptr->apps)
									StringCchCat (lpnmlv->pszText, lpnmlv->cchTextMax, _r_fmt (L"\r\n%s:\r\n%s%s", I18N (&app, IDS_FILEPATH, 0), TAB_SPACE, rstring (ptr->apps).Replaced (RULE_DELIMETER, L"\r\n" TAB_SPACE)));
							}

							break;
						}

						case NM_CUSTOMDRAW:
						{
							LONG result = CDRF_DODEFAULT;
							LPNMLVCUSTOMDRAW lpnmlv = (LPNMLVCUSTOMDRAW)pmsg->lParam;

							if (nmlp->idFrom != IDC_COLORS && nmlp->idFrom != IDC_EDITOR)
								break;

							switch (lpnmlv->nmcd.dwDrawStage)
							{
								case CDDS_PREPAINT:
								{
									result = CDRF_NOTIFYITEMDRAW;
									break;
								}

								case CDDS_ITEMPREPAINT:
								{
									if (nmlp->idFrom == IDC_COLORS)
									{
										lpnmlv->clrTextBk = colors.at (lpnmlv->nmcd.lItemlParam).clr;
										_r_wnd_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, lpnmlv->clrTextBk);

										result = CDRF_NEWFONT;
									}
									else if (nmlp->idFrom == IDC_EDITOR)
									{
										const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, lpnmlv->nmcd.dwItemSpec);

										BOOL is_custom = FALSE;

										if (page->dlg_id == IDD_SETTINGS_5)
											is_custom = rules_system.at (idx) && rules_system.at (idx)->apps ? TRUE : FALSE;
										else if (page->dlg_id == IDD_SETTINGS_6)
											is_custom = rules_custom.at (idx) && rules_custom.at (idx)->apps ? TRUE : FALSE;

										if (is_custom)
										{
											lpnmlv->clrTextBk = app.ConfigGet (L"ColorCustom", LISTVIEW_COLOR_CUSTOM).AsUlong ();
											_r_wnd_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, lpnmlv->clrTextBk);

											result = CDRF_NEWFONT;
										}
									}

									break;
								}
							}

							SetWindowLongPtr (hwnd, DWLP_MSGRESULT, result);
							return TRUE;
						}

						case LVN_GETEMPTYMARKUP:
						{
							NMLVEMPTYMARKUP* lpnmlv = (NMLVEMPTYMARKUP*)pmsg->lParam;

							lpnmlv->dwFlags = EMF_CENTERED;
							StringCchCopy (lpnmlv->szMarkup, _countof (lpnmlv->szMarkup), I18N (&app, IDS_STATUS_EMPTY, 0));

							SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
							return TRUE;
						}

						case NM_DBLCLK:
						{
							LPNMITEMACTIVATE lpnmlv = (LPNMITEMACTIVATE)pmsg->lParam;

							if (lpnmlv->iItem != -1)
							{
								if (nmlp->idFrom == IDC_COLORS)
								{
									const size_t idx = _r_listview_getitemlparam (hwnd, IDC_COLORS, lpnmlv->iItem);

									CHOOSECOLOR cc = {0};
									COLORREF cust[16] = {LISTVIEW_COLOR_CUSTOM, LISTVIEW_COLOR_INVALID, LISTVIEW_COLOR_NETWORK, LISTVIEW_COLOR_SILENT, LISTVIEW_COLOR_SYSTEM};

									cc.lStructSize = sizeof (cc);
									cc.Flags = CC_RGBINIT | CC_FULLOPEN;
									cc.hwndOwner = hwnd;
									cc.lpCustColors = cust;
									cc.rgbResult = colors.at (idx).clr;

									if (ChooseColor (&cc))
									{
										colors.at (idx).clr = cc.rgbResult;

										_r_ctrl_enable (GetParent (hwnd), IDC_APPLY, TRUE); // enable apply button (required!)
									}
								}
								else if (nmlp->idFrom == IDC_EDITOR)
								{
									SendMessage (hwnd, WM_COMMAND, MAKELPARAM (IDM_EDIT, 0), 0);
								}
							}

							break;
						}

						case NM_CLICK:
						case NM_RETURN:
						{
							if (nmlp->idFrom == IDC_RULES_BLOCKLIST_HINT)
							{
								PNMLINK nmlink = (PNMLINK)pmsg->lParam;

								if (nmlink->item.szUrl)
									ShellExecute (hwnd, nullptr, nmlink->item.szUrl, nullptr, nullptr, SW_SHOWDEFAULT);
							}

							break;
						}
					}

					break;
				}

				case WM_CONTEXTMENU:
				{
					UINT ctrl_id = GetDlgCtrlID ((HWND)pmsg->wParam);

					if (ctrl_id == IDC_EDITOR)
					{
						HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_EDITOR)), submenu = GetSubMenu (menu, 0);

						// localize
						app.LocaleMenu (submenu, I18N (&app, IDS_ADD, 0), IDM_ADD, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_EDIT2, 0), IDM_EDIT, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_DELETE, 0), IDM_DELETE, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_CHECKALL, 0), IDM_CHECKALL, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_UNCHECKALL, 0), IDM_UNCHECKALL, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_CHECK, 0), IDM_CHECK, FALSE);
						app.LocaleMenu (submenu, I18N (&app, IDS_UNCHECK, 0), IDM_UNCHECK, FALSE);

						if (!SendDlgItemMessage (hwnd, ctrl_id, LVM_GETITEMCOUNT, 0, 0))
						{
							EnableMenuItem (submenu, IDS_CHECKALL, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
							EnableMenuItem (submenu, IDS_UNCHECKALL, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						}

						if (!SendDlgItemMessage (hwnd, ctrl_id, LVM_GETSELECTEDCOUNT, 0, 0))
						{
							EnableMenuItem (submenu, IDM_EDIT, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
							EnableMenuItem (submenu, IDM_DELETE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
							EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
							EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						}

						if (page->dlg_id != IDD_SETTINGS_6)
						{
							DeleteMenu (submenu, IDM_ADD, MF_BYCOMMAND);
							DeleteMenu (submenu, IDM_EDIT, MF_BYCOMMAND);
							DeleteMenu (submenu, IDM_DELETE, MF_BYCOMMAND);
							DeleteMenu (submenu, 0, MF_BYPOSITION);
						}

						POINT pt = {0};
						GetCursorPos (&pt);

						TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

						DestroyMenu (menu);
						DestroyMenu (submenu);
					}
				}

				case WM_COMMAND:
				{
					switch (LOWORD (pmsg->wParam))
					{
						case IDC_ENABLELOG_CHK:
						{
							const UINT ctrl = LOWORD (pmsg->wParam);

							const BOOL is_enabled = IsWindowEnabled (GetDlgItem (hwnd, ctrl)) && (IsDlgButtonChecked (hwnd, ctrl) == BST_CHECKED);

							_r_ctrl_enable (hwnd, IDC_LOGPATH, is_enabled); // input
							_r_ctrl_enable (hwnd, IDC_LOGPATH_BTN, is_enabled); // button
							_r_ctrl_enable (hwnd, IDC_LOGPATH_BTN2, is_enabled); // button

							EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_GETBUDDY, 0, 0), is_enabled);

							_r_ctrl_enable (hwnd, IDC_LOGBACKUP_CHK, is_enabled); // button

							break;
						}

						case IDC_ENABLENOTIFICATIONS_CHK:
						{
							const UINT ctrl = LOWORD (pmsg->wParam);

							const BOOL is_enabled = IsWindowEnabled (GetDlgItem (hwnd, ctrl)) && (IsDlgButtonChecked (hwnd, ctrl) == BST_CHECKED);

							_r_ctrl_enable (hwnd, IDC_NOTIFICATIONSILENT_CHK, is_enabled);

							EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_GETBUDDY, 0, 0), is_enabled);
							EnableWindow ((HWND)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_GETBUDDY, 0, 0), is_enabled);

							break;
						}

						case IDC_LOGPATH_BTN:
						{
							OPENFILENAME ofn = {0};

							WCHAR path[MAX_PATH] = {0};
							GetDlgItemText (hwnd, IDC_LOGPATH, path, _countof (path));
							StringCchCopy (path, _countof (path), _r_path_expand (path));

							ofn.lStructSize = sizeof (ofn);
							ofn.hwndOwner = hwnd;
							ofn.lpstrFile = path;
							ofn.nMaxFile = _countof (path);
							ofn.lpstrFileTitle = APP_NAME_SHORT;
							ofn.nMaxFile = _countof (path);
							ofn.lpstrFilter = L"*.log\0*.log\0\0";
							ofn.lpstrDefExt = L"log";
							ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;

							if (GetSaveFileName (&ofn))
								SetDlgItemText (hwnd, IDC_LOGPATH, _r_path_unexpand (path));

							break;
						}

						case IDC_LOGPATH_BTN2:
						{
							rstring path = _r_path_expand (_r_ctrl_gettext (hwnd, IDC_LOGPATH));

							if (!_r_fs_exists (path))
								return FALSE;

							_r_run (nullptr, _r_fmt (L"%s \"%s\"", app.ConfigGet (L"LogViewer", L"notepad.exe"), path));

							break;
						}

						case IDM_ADD:
						{
							if (page->dlg_id != IDD_SETTINGS_6)
								break;

							ITEM_RULE* ptr = (ITEM_RULE*)malloc (sizeof (ITEM_RULE));

							if (ptr)
							{
								SecureZeroMemory (ptr, sizeof (ITEM_RULE));

								if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr))
								{
									rules_custom.push_back (ptr);

									settings_callback (page->hwnd, _RM_INITIALIZE, nullptr, page); // reinititalize page
								}
								else
								{
									_app_freerule (&ptr);
								}
							}

							break;
						}

						case IDM_EDIT:
						{
							if (page->dlg_id != IDD_SETTINGS_6)
								break;

							const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

							if (item != LAST_VALUE)
							{
								const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, item);

								ITEM_RULE* ptr = rules_custom[idx];

								if (ptr && DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr))
								{
									settings_callback (page->hwnd, _RM_INITIALIZE, nullptr, page); // re-inititalize page
								}
							}

							break;
						}

						case IDM_DELETE:
						{
							if (page->dlg_id != IDD_SETTINGS_6)
								break;

							if (_r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
								break;

							const size_t count = _r_listview_getitemcount (hwnd, IDC_EDITOR) - 1;

							for (size_t i = count; i != LAST_VALUE; i--)
							{
								if (ListView_GetItemState (GetDlgItem (hwnd, IDC_EDITOR), i, LVNI_SELECTED))
								{
									const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, i);

									for (auto &p : apps_rules)
									{
										const size_t hash = p.first;

										if (hash)
										{
											for (size_t j = 0; j < p.second.size (); j++)
											{
												if (p.second.at (j) == idx)
													p.second.erase (p.second.begin () + j);
											}
										}
									}

									SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_DELETEITEM, i, 0);

									_app_freerule (&rules_custom.at (idx));
								}
							}

							SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_REDRAWITEMS, 0, _r_listview_getitemcount (hwnd, IDC_EDITOR)); // redraw (required!)

							break;
						}

						case IDM_CHECKALL:
						case IDM_UNCHECKALL:
						{
							_r_listview_setitemcheck (hwnd, IDC_EDITOR, LAST_VALUE, (LOWORD (pmsg->wParam) == IDM_CHECKALL) ? TRUE : FALSE);
							break;
						}

						case IDM_CHECK:
						case IDM_UNCHECK:
						{
							INT item = -1;

							while ((item = (INT)SendDlgItemMessage (hwnd, IDC_EDITOR, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
							{
								_r_listview_setitemcheck (hwnd, IDC_EDITOR, item, LOWORD (pmsg->wParam) == IDM_CHECK ? TRUE : FALSE);
							}

							break;
						}
					}

					break;
				}
			}

			break;
		}

		case _RM_SAVE:
		{
			switch (page->dlg_id)
			{
				case IDD_SETTINGS_1:
				{
					app.ConfigSet (L"AlwaysOnTop", DWORD ((IsDlgButtonChecked (hwnd, IDC_ALWAYSONTOP_CHK) == BST_CHECKED) ? TRUE : FALSE));

#ifdef _APP_HAVE_AUTORUN
					app.AutorunEnable (IsDlgButtonChecked (hwnd, IDC_LOADONSTARTUP_CHK) == BST_CHECKED);
#endif // _APP_HAVE_AUTORUN

#ifdef _APP_HAVE_SKIPUAC
					app.SkipUacEnable (IsDlgButtonChecked (hwnd, IDC_SKIPUACWARNING_CHK) == BST_CHECKED);
#endif // _APP_HAVE_SKIPUAC

					app.ConfigSet (L"CheckUpdates", ((IsDlgButtonChecked (hwnd, IDC_CHECKUPDATES_CHK) == BST_CHECKED) ? TRUE : FALSE));

					// set language
					rstring buffer;

					if (SendDlgItemMessage (hwnd, IDC_LANGUAGE, CB_GETCURSEL, 0, 0) >= 1)
						buffer = _r_ctrl_gettext (hwnd, IDC_LANGUAGE);

					app.ConfigSet (L"Language", buffer);

					if (GetWindowLongPtr (hwnd, GWLP_USERDATA) != (INT)SendDlgItemMessage (hwnd, IDC_LANGUAGE, CB_GETCURSEL, 0, 0))
						return TRUE; // for restart

					break;
				}

				case IDD_SETTINGS_2:
				{
					app.ConfigSet (L"UseBlocklist2", DWORD ((IsDlgButtonChecked (hwnd, IDC_USEBLOCKLIST_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"UseStealthMode", DWORD ((IsDlgButtonChecked (hwnd, IDC_USESTEALTHMODE_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"InstallBoottimeFilters", DWORD ((IsDlgButtonChecked (hwnd, IDC_INSTALLBOOTTIMEFILTERS_CHK) == BST_CHECKED) ? TRUE : FALSE));

					app.ConfigSet (L"AllowInboundConnections", DWORD ((IsDlgButtonChecked (hwnd, IDC_RULE_ALLOWINBOUND) == BST_CHECKED) ? TRUE : FALSE));

					break;
				}

				case IDD_SETTINGS_3:
				{
					app.ConfigSet (L"ConfirmExit", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMEXIT_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"ConfirmDelete", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMDELETE_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"ConfirmLogClear", DWORD ((IsDlgButtonChecked (hwnd, IDC_CONFIRMLOGCLEAR_CHK) == BST_CHECKED) ? TRUE : FALSE));

					for (size_t i = 0; i < colors.size (); i++)
					{
						app.ConfigSet (colors.at (i).config, _r_listview_getcheckstate (hwnd, IDC_COLORS, i));
						app.ConfigSet (colors.at (i).config_color, colors.at (i).clr);
					}

					break;
				}

				case IDD_SETTINGS_4:
				{
					app.ConfigSet (L"IsLogEnabled", DWORD ((IsDlgButtonChecked (hwnd, IDC_ENABLELOG_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"LogPath", _r_ctrl_gettext (hwnd, IDC_LOGPATH));
					app.ConfigSet (L"LogSizeLimit", (DWORD)SendDlgItemMessage (hwnd, IDC_LOGSIZELIMIT, UDM_GETPOS32, 0, 0));
					app.ConfigSet (L"IsLogBackup", DWORD ((IsDlgButtonChecked (hwnd, IDC_LOGBACKUP_CHK) == BST_CHECKED) ? TRUE : FALSE));

					app.ConfigSet (L"IsNotificationsEnabled", DWORD ((IsDlgButtonChecked (hwnd, IDC_ENABLENOTIFICATIONS_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"IsNotificationsSilent", DWORD ((IsDlgButtonChecked (hwnd, IDC_NOTIFICATIONSILENT_CHK) == BST_CHECKED) ? TRUE : FALSE));
					app.ConfigSet (L"NotificationsDisplayTimeout", (DWORD)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONDISPLAYTIMEOUT, UDM_GETPOS32, 0, 0));
					app.ConfigSet (L"NotificationsTimeout", (DWORD)SendDlgItemMessage (hwnd, IDC_NOTIFICATIONTIMEOUT, UDM_GETPOS32, 0, 0));

					break;
				}

				case IDD_SETTINGS_7:
				{
					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_EDITOR); i++)
					{
						const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, i);

						ITEM_RULE* ptr = rules_blocklist.at (idx);

						if (ptr)
						{
							ptr->is_enabled = _r_listview_getcheckstate (hwnd, IDC_EDITOR, i) ? true : false;
							rules_config[ptr->name] = ptr->is_enabled;
						}
					}

					break;
				}

				case IDD_SETTINGS_5:
				{
					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_EDITOR); i++)
					{
						const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, i);

						ITEM_RULE* ptr = rules_system.at (idx);

						if (ptr)
						{
							ptr->is_enabled = _r_listview_getcheckstate (hwnd, IDC_EDITOR, i) ? true : false;
							rules_config[ptr->name] = ptr->is_enabled;
						}
					}

					break;
				}

				case IDD_SETTINGS_6:
				{
					for (size_t i = 0; i < _r_listview_getitemcount (hwnd, IDC_EDITOR); i++)
					{
						const size_t idx = _r_listview_getitemlparam (hwnd, IDC_EDITOR, i);

						ITEM_RULE* ptr = rules_custom.at (idx);

						if (ptr)
						{
							ptr->is_enabled = _r_listview_getcheckstate (hwnd, IDC_EDITOR, i) ? true : false;
						}
					}

					_app_profilesave (app.GetHWND ()); // save profile
					_app_profileload (app.GetHWND ()); // load profile

					break;
				}
			}

			break;
		}

		case _RM_UNINITIALIZE:
		{
			_app_profileload (app.GetHWND ());
			break;
		}
	}

	return FALSE;
}

VOID ResizeWindow (HWND hwnd, INT width, INT height)
{
	RECT rc = {0};

	GetClientRect (GetDlgItem (hwnd, IDC_EXIT_BTN), &rc);
	INT button_width = rc.right;

	GetClientRect (GetDlgItem (hwnd, IDC_STATUSBAR), &rc);
	INT statusbar_height = rc.bottom;

	INT button_top = height - statusbar_height - app.GetDPI (1 + 34);

	SetWindowPos (GetDlgItem (hwnd, IDC_LISTVIEW), nullptr, 0, 0, width, height - statusbar_height - app.GetDPI (1 + 46), SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);

	SetWindowPos (GetDlgItem (hwnd, IDC_START_BTN), nullptr, app.GetDPI (10), button_top, 0, 0, SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	SetWindowPos (GetDlgItem (hwnd, IDC_SETTINGS_BTN), nullptr, width - app.GetDPI (10) - button_width - button_width - app.GetDPI (6), button_top, 0, 0, SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
	SetWindowPos (GetDlgItem (hwnd, IDC_EXIT_BTN), nullptr, width - app.GetDPI (10) - button_width, button_top, 0, 0, SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSIZE);

	// resize columns
	_app_listviewresize (hwnd, 0);

	// resize statusbar
	SendDlgItemMessage (hwnd, IDC_STATUSBAR, WM_SIZE, 0, 0);
}

BOOL _wfp_initialize (BOOL is_full)
{
	BOOL result = TRUE;
	DWORD rc = 0;

	if (!config.hengine)
	{
		// generate unique session key
		if (!config.psession)
		{
			config.psession = (GUID*)malloc (sizeof (GUID));

			if (config.psession)
			{
				if (CoCreateGuid (config.psession) != S_OK)
				{
					SecureZeroMemory (config.psession, sizeof (GUID));
					free (config.psession);

					config.psession = nullptr;
				}
			}
		}

		SecureZeroMemory (&session, sizeof (session));

		session.displayData.name = APP_NAME;
		session.displayData.description = APP_NAME;

		if (config.psession)
			memcpy_s (&session.sessionKey, sizeof (GUID), config.psession, sizeof (GUID));

		rc = FwpmEngineOpen (nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &config.hengine);

		if (rc != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmEngineOpen", rc, nullptr);
			config.hengine = nullptr;
			result = FALSE;
		}
	}

	// set security info
	if (is_full && !config.is_securityinfoset)
	{
		if (config.hengine && config.psid)
		{
			// Add DACL for given user
			PACL pDacl = nullptr;
			PSECURITY_DESCRIPTOR securityDescriptor = nullptr;

			rc = FwpmEngineGetSecurityInfo (config.hengine, DACL_SECURITY_INFORMATION, nullptr, nullptr, &pDacl, nullptr, &securityDescriptor);

			if (rc != ERROR_SUCCESS)
			{
				_app_logerror (L"FwpmEngineGetSecurityInfo", rc, nullptr);
			}
			else
			{
				BOOL bExists = false;

				// Loop through the ACEs and search for user SID.
				for (WORD cAce = 0; !bExists && cAce < pDacl->AceCount; cAce++)
				{
					ACCESS_ALLOWED_ACE* pAce = nullptr;

					// Get ACE info
					if (GetAce (pDacl, cAce, (LPVOID*)&pAce) == FALSE)
					{
						_app_logerror (L"GetAce", GetLastError (), nullptr);
						continue;
					}

					if (pAce->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
						continue;

					bExists = EqualSid (&pAce->SidStart, config.psid);
				}

				if (!bExists)
				{
					PACL pNewDacl = nullptr;
					EXPLICIT_ACCESS ea = {0};

					// Initialize an EXPLICIT_ACCESS structure for the new ACE.
					SecureZeroMemory (&ea, sizeof (EXPLICIT_ACCESS));

					ea.grfAccessPermissions = FWPM_GENERIC_WRITE | FWPM_GENERIC_EXECUTE | FWPM_GENERIC_READ | DELETE | WRITE_DAC | WRITE_OWNER;
					ea.grfAccessMode = GRANT_ACCESS;
					ea.grfInheritance = NO_INHERITANCE;
					ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
					ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
					ea.Trustee.ptstrName = (LPTSTR)config.psid;
					//ea.Trustee.ptstrName = user;

					// Create a new ACL that merges the new ACE
					// into the existing DACL.
					rc = SetEntriesInAcl (1, &ea, pDacl, &pNewDacl);

					if (rc != ERROR_SUCCESS)
					{
						_app_logerror (L"SetEntriesInAcl", rc, nullptr);
					}
					else
					{
						rc = FwpmEngineSetSecurityInfo (config.hengine, DACL_SECURITY_INFORMATION, nullptr, nullptr, pNewDacl, nullptr);

						if (rc != ERROR_SUCCESS)
						{
							_app_logerror (L"FwpmEngineSetSecurityInfo", rc, nullptr);
						}
						else
						{
							config.is_securityinfoset = TRUE;
						}
					}

					if (pNewDacl)
						LocalFree (pNewDacl);
				}
			}
		}
	}

	// net events subscribe (win7 and above)
	if (is_full && config.hengine && !config.hevent && _r_sys_validversion (6, 1))
	{
		FWP_VALUE val;
		SecureZeroMemory (&val, sizeof (val));

		val.type = FWP_UINT32;
		val.uint32 = 1;
		rc = FwpmEngineSetOption (config.hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

		if (rc != ERROR_SUCCESS)
		{
			_app_logerror (L"FwpmEngineSetOption", rc, L"FWPM_ENGINE_COLLECT_NET_EVENTS");
		}
		else
		{
			HINSTANCE hmodule = GetModuleHandle (L"fwpuclnt.dll");

			if (hmodule)
			{
				FWPMNES2 _FwpmNetEventSubscribe2 = nullptr;
				FWPMNES1 _FwpmNetEventSubscribe1 = nullptr;
				FWPMNES0 _FwpmNetEventSubscribe0 = nullptr;

				_FwpmNetEventSubscribe2 = (FWPMNES2)GetProcAddress (hmodule, "FwpmNetEventSubscribe2"); // win10

				if (!_FwpmNetEventSubscribe2)
				{
					_FwpmNetEventSubscribe1 = (FWPMNES1)GetProcAddress (hmodule, "FwpmNetEventSubscribe1"); // win8

					if (!_FwpmNetEventSubscribe1)
						_FwpmNetEventSubscribe0 = (FWPMNES0)GetProcAddress (hmodule, "FwpmNetEventSubscribe0"); // win7
				}

				if (!_FwpmNetEventSubscribe2 && !_FwpmNetEventSubscribe1 && !_FwpmNetEventSubscribe0)
				{
					_app_logerror (L"GetProcAddress", GetLastError (), L"FwpmNetEventSubscribe");
				}
				else
				{
					SecureZeroMemory (&subscription, sizeof (subscription));
					SecureZeroMemory (&enum_template, sizeof (enum_template));

					if (config.psession)
						memcpy_s (&subscription.sessionKey, sizeof (GUID), config.psession, sizeof (GUID));

					subscription.enumTemplate = &enum_template;

					if (_FwpmNetEventSubscribe2)
						rc = _FwpmNetEventSubscribe2 (config.hengine, &subscription, _app_logcallback2, nullptr, &config.hevent);
					else if (_FwpmNetEventSubscribe1)
						rc = _FwpmNetEventSubscribe1 (config.hengine, &subscription, _app_logcallback1, nullptr, &config.hevent);
					else if (_FwpmNetEventSubscribe0)
						rc = _FwpmNetEventSubscribe0 (config.hengine, &subscription, _app_logcallback0, nullptr, &config.hevent);

					if (rc != ERROR_SUCCESS)
					{
						_app_logerror (L"FwpmNetEventSubscribe", rc, nullptr);
					}
					else
					{
						_app_loginit (TRUE); // create log file
					}
				}
			}
		}
	}

	return result;
}

VOID _wfp_uninitialize ()
{
	DWORD result = 0;

	_app_loginit (FALSE); // destroy log file handle if present

	if (config.hengine)
	{
		// net events unsubscribe (win7 and above)
		if (_r_sys_validversion (6, 1))
		{
			if (config.hevent)
			{
				FWPMNEU _FwpmNetEventUnsubscribe = (FWPMNEU)GetProcAddress (GetModuleHandle (L"fwpuclnt.dll"), "FwpmNetEventUnsubscribe0");

				if (_FwpmNetEventUnsubscribe)
				{
					result = _FwpmNetEventUnsubscribe (config.hengine, config.hevent);

					if (result == ERROR_SUCCESS)
						config.hevent = nullptr;
				}
			}

			FWP_VALUE val;
			SecureZeroMemory (&val, sizeof (val));

			val.type = FWP_UINT32;
			val.uint32 = 0;

			result = FwpmEngineSetOption (config.hengine, FWPM_ENGINE_COLLECT_NET_EVENTS, &val);

			if (result != ERROR_SUCCESS)
				_app_logerror (L"FwpmEngineSetOption", result, L"FWPM_ENGINE_COLLECT_NET_EVENTS");
		}

		FwpmEngineClose (config.hengine);
		config.hengine = nullptr;
	}

	if (config.psession)
	{
		SecureZeroMemory (config.psession, sizeof (GUID));
		free (config.psession);

		config.psession = nullptr;
	}

	config.is_securityinfoset = FALSE;
}

LRESULT CALLBACK NotificationProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
		case WM_LBUTTONDBLCLK:
		{
			const size_t idx = _app_notifygetcurrent ();

			if (idx != LAST_VALUE)
			{
				ShowItem (app.GetHWND (), IDC_LISTVIEW, _app_getposition (app.GetHWND (), notifications.at (idx)->hash), -1);

				_r_wnd_toggle (app.GetHWND (), TRUE);
			}

			ShowWindow (hwnd, SW_HIDE);

			break;
		}

		case WM_MOUSEMOVE:
		case WM_NCMOUSEMOVE:
		{
			if (IsWindowVisible (hwnd))
			{
				if (msg == WM_NCMOUSEMOVE)
				{
					TRACKMOUSEEVENT tme = {0};

					tme.cbSize = sizeof (tme);
					tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
					tme.hwndTrack = hwnd;
					tme.dwHoverTime = 0;

					TrackMouseEvent (&tme);

					config.htracked_window = hwnd;
				}

				_app_notifysettimeout (hwnd, FALSE, 0);
			}

			break;
		}

		case WM_NCMOUSELEAVE:
		{
			if (hwnd == config.htracked_window)
				_app_notifysettimeout (hwnd, TRUE, NOTIFY_TIMER_MOUSE);

			break;
		}

		case WM_ACTIVATE:
		{
			switch (LOWORD (wparam))
			{
				case WA_ACTIVE:
				case WA_CLICKACTIVE:
				{
					_app_notifysettimeout (hwnd, FALSE, 0);
					break;
				}
				case WA_INACTIVE:
				{
					_app_notifysettimeout (hwnd, TRUE, 0);
					break;
				}
			}

			break;
		}

		case WM_SIZE:
		{
			InvalidateRect (hwnd, nullptr, FALSE);
			break;
		}

		case WM_TIMER:
		{
			if (wparam == NOTIFY_TIMER_ID)
			{
				_app_notifysettimeout (hwnd, FALSE, 0);

				ShowWindow (hwnd, SW_HIDE);
			}

			break;
		}

		case WM_CLOSE:
		{
			ShowWindow (hwnd, SW_HIDE);
			SetWindowLongPtr (hwnd, DWLP_MSGRESULT, 0);

			return TRUE;
		}

		case WM_KEYDOWN:
		{
			switch (wparam)
			{
				case VK_ESCAPE:
				{
					ShowWindow (hwnd, SW_HIDE);
					break;
				}
			}

			break;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps = {0};
			HDC dc = BeginPaint (hwnd, &ps);

			RECT rc = {0};
			GetClientRect (hwnd, &rc);

			static INT top = app.GetDPI (36);
			static INT bottom = app.GetDPI (48);

			const INT height = rc.bottom;

			rc.bottom = top;

			_r_wnd_fillrect (dc, &rc, GetSysColor (COLOR_BTNFACE));

			for (INT i = 0; i < rc.right; i++)
				SetPixel (dc, i, top, GetSysColor (COLOR_APPWORKSPACE));

			rc.top = height - bottom;
			rc.bottom = rc.top + bottom;

			_r_wnd_fillrect (dc, &rc, GetSysColor (COLOR_BTNFACE));

			for (INT i = 0; i < rc.right; i++)
				SetPixel (dc, i, rc.top, GetSysColor (COLOR_APPWORKSPACE));

			EndPaint (hwnd, &ps);

			break;
		}

		case WM_CTLCOLORSTATIC:
		{
			if (GetDlgCtrlID ((HWND)lparam) == IDC_TITLE_ID || GetDlgCtrlID ((HWND)lparam) == IDC_IDX_ID)
				break;

			return (INT_PTR)GetSysColorBrush (COLOR_WINDOW);
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case TTN_GETDISPINFO:
				{
					LPNMTTDISPINFO lpnmdi = (LPNMTTDISPINFO)lparam;

					if ((lpnmdi->uFlags & TTF_IDISHWND) != 0)
					{
						WCHAR buffer[1024] = {0};
						const UINT ctrl_id = GetDlgCtrlID ((HWND)lpnmdi->hdr.idFrom);

						if (ctrl_id == IDC_FILE_ID)
						{
							const size_t idx = _app_notifygetcurrent ();

							if (idx != LAST_VALUE)
							{
								StringCchCopy (buffer, _countof (buffer), notifications.at (idx)->full_path);

								lpnmdi->lpszText = buffer;
							}
						}
						else if (ctrl_id == IDC_CREATERULE_ADDR_ID || ctrl_id == IDC_CREATERULE_PORT_ID)
						{
							StringCchCopy (buffer, _countof (buffer), I18N (&app, IDS_NOTIFY_TOOLTIP, 0));

							lpnmdi->lpszText = buffer;
						}
					}

					break;
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			switch (LOWORD (wparam))
			{
				case IDC_PREV_ID:
				{
					const size_t current_idx = _app_notifygetcurrent ();

					if (current_idx != LAST_VALUE)
					{
						const size_t count = notifications.size ();

						if (!current_idx)
						{
							_app_notifyshow (count - 1, nullptr);
						}
						else
						{
							const size_t idx = (size_t)current_idx - 1;

							if (idx >= 0 && idx <= (count - 1))
								_app_notifyshow (idx, nullptr);
						}
					}

					break;
				}

				case IDC_NEXT_ID:
				{
					const size_t current_idx = _app_notifygetcurrent ();

					if (current_idx != LAST_VALUE)
					{
						const size_t count = notifications.size ();

						if (current_idx >= count - 1)
						{
							_app_notifyshow (0, nullptr);
						}
						else
						{
							const size_t idx = (size_t)current_idx + 1;

							if (idx >= 0 && idx <= (count - 1))
								_app_notifyshow (idx, nullptr);
						}
					}

					break;
				}

				case IDC_ALLOW_BTN:
				{
					_app_notifycommand (hwnd, FALSE);
					break;
				}

				case IDC_BLOCK_BTN:
				{
					_app_notifycommand (hwnd, TRUE);
					break;
				}

				case IDC_CLOSE_BTN:
				{
					ShowWindow (hwnd, SW_HIDE);
					break;
				}
			}

			break;
		}
	}

	return DefWindowProc (hwnd, msg, wparam, lparam);
}

INT_PTR CALLBACK DlgProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_FINDMSGSTRING)
	{
		LPFINDREPLACE const lpfr = (LPFINDREPLACE)lparam;

		if ((lpfr->Flags & FR_DIALOGTERM) != 0)
		{
			config.hfind = nullptr;
		}
		else if ((lpfr->Flags & FR_FINDNEXT) != 0)
		{
			const size_t total = _r_listview_getitemcount (hwnd, IDC_LISTVIEW);
			const INT start = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)total - 1, LVNI_SELECTED | LVNI_DIRECTIONMASK | LVNI_BELOW) + 1;

			for (size_t i = start; i < total; i++)
			{
				const rstring text = _r_listview_getitemtext (hwnd, IDC_LISTVIEW, i, 0);

				if (StrStrI (text, lpfr->lpstrFindWhat) != nullptr)
				{
					ShowItem (hwnd, IDC_LISTVIEW, i, 0);
					SetFocus (hwnd);
					break;
				}
			}
		}

		return FALSE;
	}

	switch (msg)
	{
		case WM_INITDIALOG:
		{

#ifndef _WIN64
			if (_r_sys_iswow64 ())
				Wow64EnableWow64FsRedirection (FALSE);
#endif

			// static initializer
			config.wd_length = GetWindowsDirectory (config.windows_dir, _countof (config.windows_dir));
			StringCchPrintf (config.apps_path, _countof (config.apps_path), L"%s\\" XML_APPS, app.GetProfileDirectory ());
			StringCchPrintf (config.blocklist_path, _countof (config.blocklist_path), L"%s\\" XML_BLOCKLIST, app.GetProfileDirectory ());
			StringCchPrintf (config.rules_config_path, _countof (config.rules_config_path), L"%s\\" XML_RULES_CONFIG, app.GetProfileDirectory ());
			StringCchPrintf (config.rules_custom_path, _countof (config.rules_custom_path), L"%s\\" XML_RULES_CUSTOM, app.GetProfileDirectory ());
			StringCchPrintf (config.rules_system_path, _countof (config.rules_system_path), L"%s\\" XML_RULES_SYSTEM, app.GetProfileDirectory ());

			config.ntoskrnl_hash = _r_str_hash (PROC_SYSTEM_NAME);

			// set privileges
			_r_sys_setprivilege (SE_BACKUP_NAME, TRUE);
			_r_sys_setprivilege (SE_DEBUG_NAME, TRUE);
			_r_sys_setprivilege (SE_SECURITY_NAME, TRUE);
			_r_sys_setprivilege (SE_TAKE_OWNERSHIP_NAME, TRUE);

			// get current user security identifier
			if (!config.psid)
			{
				// get user sid
				HANDLE token = nullptr;
				DWORD token_length = 0;
				PTOKEN_USER token_user = nullptr;

				if (OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &token))
				{
					GetTokenInformation (token, TokenUser, nullptr, token_length, &token_length);

					if (GetLastError () == ERROR_INSUFFICIENT_BUFFER)
					{
						token_user = (PTOKEN_USER)malloc (token_length);

						if (token_user)
						{
							if (GetTokenInformation (token, TokenUser, token_user, token_length, &token_length))
							{
								SID_NAME_USE sid_type;

								WCHAR username[MAX_PATH] = {0};
								WCHAR domain[MAX_PATH] = {0};

								DWORD length1 = _countof (username);
								DWORD length2 = _countof (domain);

								if (LookupAccountSid (nullptr, token_user->User.Sid, username, &length1, domain, &length2, &sid_type))
									StringCchPrintf (config.title, _countof (config.title), L"%s [%s\\%s]", APP_NAME, domain, username);

								config.psid = (SID*)malloc (SECURITY_MAX_SID_SIZE);

								if (config.psid)
								{
									SecureZeroMemory (config.psid, SECURITY_MAX_SID_SIZE);

									memcpy_s (config.psid, SECURITY_MAX_SID_SIZE, token_user->User.Sid, SECURITY_MAX_SID_SIZE);
								}
							}
						}

						free (token_user);
					}

					CloseHandle (token);
				}

				if (!config.title)
					StringCchCopy (config.title, _countof (config.title), APP_NAME); // fallback
			}

			// configure listview
			_r_listview_setstyle (hwnd, IDC_LISTVIEW, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

			_r_listview_addcolumn (hwnd, IDC_LISTVIEW, I18N (&app, IDS_FILEPATH, 0), 70, 0, LVCFMT_LEFT);
			_r_listview_addcolumn (hwnd, IDC_LISTVIEW, I18N (&app, IDS_ADDED, 0), 26, 1, LVCFMT_LEFT);

			_r_listview_addgroup (hwnd, IDC_LISTVIEW, I18N (&app, IDS_GROUP_ALLOWED, 0), 0, 0, LVGS_COLLAPSIBLE | (app.ConfigGet (L"Group1IsCollaped", FALSE).AsBool () ? LVGS_COLLAPSED : 0));
			_r_listview_addgroup (hwnd, IDC_LISTVIEW, I18N (&app, IDS_GROUP_BLOCKED, 0), 1, 0, LVGS_COLLAPSIBLE | (app.ConfigGet (L"Group2IsCollaped", FALSE).AsBool () ? LVGS_COLLAPSED : 0));

			_app_seticonsize (hwnd);
			_app_setfont (hwnd);

			// load settings imagelist
			{
				//const INT cx = app.GetDPI (18);
				const INT cx = GetSystemMetrics (SM_CXSMICON);

				config.himg = ImageList_Create (cx, cx, ILC_COLOR32 | ILC_MASK, 0, 5);

				HICON hico = _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_ALLOW), cx);
				ImageList_ReplaceIcon (config.himg, -1, hico);
				DestroyIcon (hico);

				hico = _r_loadicon (app.GetHINSTANCE (), MAKEINTRESOURCE (IDI_BLOCK), cx);
				ImageList_ReplaceIcon (config.himg, -1, hico);
				DestroyIcon (hico);
			}

			// get default icon for executable
			{
				SHFILEINFO shfi = {0};

				CoInitialize (nullptr);

				if (SHGetFileInfo (_r_path_expand (PATH_NTOSKRNL), 0, &shfi, sizeof (shfi), SHGFI_SYSICONINDEX | SHGFI_ICON))
				{
					config.def_icon_id = shfi.iIcon;
					config.def_hicon = CopyIcon (shfi.hIcon);

					DestroyIcon (shfi.hIcon);
				}

				if (SHGetFileInfo (_r_path_expand (PATH_NTOSKRNL), 0, &shfi, sizeof (shfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_ICON))
				{
					config.def_icon_id = shfi.iIcon;
					config.def_hicon_sm = CopyIcon (shfi.hIcon);

					DestroyIcon (shfi.hIcon);
				}

				CoUninitialize ();
			}

			// drag & drop support
			DragAcceptFiles (hwnd, TRUE);

			// settings
			app.AddSettingsPage (nullptr, IDD_SETTINGS_1, IDS_SETTINGS_1, L"IDS_SETTINGS_1", &settings_callback);
			app.AddSettingsPage (nullptr, IDD_SETTINGS_3, IDS_SETTINGS_3, L"IDS_SETTINGS_3", &settings_callback);

			{
				const size_t page_id = app.AddSettingsPage (nullptr, IDD_SETTINGS_2, IDS_TRAY_FILTERS, L"IDS_TRAY_FILTERS", &settings_callback);

				app.AddSettingsPage (nullptr, IDD_SETTINGS_7, IDS_TRAY_BLOCKLIST_RULES, L"IDS_TRAY_BLOCKLIST_RULES", &settings_callback, page_id);
				app.AddSettingsPage (nullptr, IDD_SETTINGS_5, IDS_TRAY_SYSTEM_RULES, L"IDS_TRAY_SYSTEM_RULES", &settings_callback, page_id);
				app.AddSettingsPage (nullptr, IDD_SETTINGS_6, IDS_TRAY_CUSTOM_RULES, L"IDS_TRAY_CUSTOM_RULES", &settings_callback, page_id);
			}

			app.AddSettingsPage (nullptr, IDD_SETTINGS_4, IDS_TRAY_LOG, L"IDS_TRAY_LOG", &settings_callback);

			// load colors
			{
				colors.clear ();

				addcolor (L"IDS_HIGHLIGHT_CUSTOM", IDS_HIGHLIGHT_CUSTOM, L"IsHighlightCustom", TRUE, L"ColorCustom", LISTVIEW_COLOR_CUSTOM);
				addcolor (L"IDS_HIGHLIGHT_INVALID", IDS_HIGHLIGHT_INVALID, L"IsHighlightInvalid", TRUE, L"ColorInvalid", LISTVIEW_COLOR_INVALID);
				addcolor (L"IDS_HIGHLIGHT_NETWORK", IDS_HIGHLIGHT_NETWORK, L"IsHighlightNetwork", TRUE, L"ColorNetwork", LISTVIEW_COLOR_NETWORK);
				addcolor (L"IDS_HIGHLIGHT_SILENT", IDS_HIGHLIGHT_SILENT, L"IsHighlightSilent", TRUE, L"ColorSilent", LISTVIEW_COLOR_SILENT);
				addcolor (L"IDS_HIGHLIGHT_SYSTEM", IDS_HIGHLIGHT_SYSTEM, L"IsHighlightSystem", TRUE, L"ColorSystem", LISTVIEW_COLOR_SYSTEM);
			}

			// load protocols
			{
				protocols.clear ();

				addprotocol (L"ICMP", IPPROTO_ICMP);
				addprotocol (L"ICMPv6", IPPROTO_ICMPV6);
				addprotocol (L"IGMP", IPPROTO_IGMP);
				addprotocol (L"IPv4", IPPROTO_IPV4);
				addprotocol (L"IPv6", IPPROTO_IPV6);
				addprotocol (L"L2TP", IPPROTO_L2TP);
				addprotocol (L"RAW", IPPROTO_RAW);
				addprotocol (L"RDP", IPPROTO_RDP);
				addprotocol (L"SCTP", IPPROTO_SCTP);
				addprotocol (L"TCP", IPPROTO_TCP);
				addprotocol (L"UDP", IPPROTO_UDP);
			}

			// initialize thread objects
			if (!config.install_evt)
				config.install_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

			if (!config.destroy_evt)
				config.destroy_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

			if (!config.stop_evt)
				config.stop_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

			if (!config.finish_evt)
				config.finish_evt = CreateEvent (nullptr, FALSE, FALSE, nullptr);

			if (!config.hthread)
				config.hthread = (HANDLE)_beginthreadex (nullptr, 0, &ApplyThread, nullptr, 0, nullptr);

			WTSRegisterSessionNotification (hwnd, NOTIFY_FOR_THIS_SESSION);

			_app_notifycreatewindow ();

			break;
		}

		case WM_DROPFILES:
		{
			UINT numfiles = DragQueryFile ((HDROP)wparam, 0xFFFFFFFF, nullptr, 0);
			size_t item = 0;

			for (UINT i = 0; i < numfiles; i++)
			{
				const UINT length = DragQueryFile ((HDROP)wparam, i, nullptr, 0);

				LPWSTR file = (LPWSTR)malloc ((length + 1) * sizeof (WCHAR));

				if (file)
				{
					DragQueryFile ((HDROP)wparam, i, file, length + 1);

					item = _app_addapplication (hwnd, file, 0, FALSE, FALSE);

					free (file);
				}
			}

			_app_listviewsort (hwnd, -1, FALSE);
			_app_profilesave (hwnd);

			ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, item), -1);

			DragFinish ((HDROP)wparam);

			break;
		}

		case WM_CLOSE:
		{
			if (app.ConfigGet (L"ConfirmExit", TRUE).AsBool ())
			{
				WCHAR flag[64] = {0};
				WCHAR text[128] = {0};

				INT result = 0;
				BOOL is_flagchecked = 0;

				TASKDIALOGCONFIG tdc = {0};

				tdc.cbSize = sizeof (tdc);
				tdc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_VERIFICATION_FLAG_CHECKED;
				tdc.hwndParent = hwnd;
				tdc.pszWindowTitle = APP_NAME;
				tdc.pfCallback = &_r_msg_callback;
				tdc.pszMainIcon = TD_INFORMATION_ICON;
				tdc.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
				tdc.pszMainInstruction = text;
				tdc.pszVerificationText = flag;

				StringCchCopy (text, _countof (text), I18N (&app, IDS_QUESTION_EXIT, 0));
				StringCchCopy (flag, _countof (flag), I18N (&app, IDS_ALWAYSPERFORMTHISCHECK_CHK, 0));

				TaskDialogIndirect (&tdc, &result, nullptr, &is_flagchecked);

				if (result != IDYES)
					return TRUE;

				app.ConfigSet (L"ConfirmExit", is_flagchecked);
			}

			DestroyWindow (hwnd);

			break;
		}

		case WM_DESTROY:
		{
			app.TrayDestroy (UID);

			DestroyWindow (config.hnotification);
			UnregisterClass (NOTIFY_CLASS_DLG, app.GetHINSTANCE ());

			ImageList_Destroy (config.himg);

			app.ConfigSet (L"Group1IsCollaped", (SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETGROUPSTATE, 0, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0);
			app.ConfigSet (L"Group2IsCollaped", (SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETGROUPSTATE, 1, LVGS_COLLAPSED) & LVGS_COLLAPSED) != 0);

			_R_SPINLOCK (config.lock_apply);

			if (config.stop_evt)
			{
				SetEvent (config.stop_evt); // exit thread

				if (config.finish_evt)
				{
					WaitForSingleObjectEx (config.finish_evt, 30 * _R_SECONDSCLOCK_MSEC, FALSE);
				}
				else
				{
					_r_sleep (1000);
				}

				if (config.hthread)
				{
					CloseHandle (config.hthread);
					config.hthread = nullptr;
				}
			}

			_wfp_uninitialize ();

			_R_SPINUNLOCK (config.lock_apply);

			PostQuitMessage (0);

			break;
		}

		case WM_PAINT:
		{
			PAINTSTRUCT ps = {0};
			HDC dc = BeginPaint (hwnd, &ps);

			RECT rc = {0};
			GetWindowRect (GetDlgItem (hwnd, IDC_LISTVIEW), &rc);

			for (INT i = 0; i < rc.right; i++)
				SetPixel (dc, i, rc.bottom - rc.top, GetSysColor (COLOR_APPWORKSPACE));

			EndPaint (hwnd, &ps);

			break;
		}

		case WM_NOTIFY:
		{
			LPNMHDR nmlp = (LPNMHDR)lparam;

			switch (nmlp->code)
			{
				case NM_CUSTOMDRAW:
				{
					if (nmlp->idFrom != IDC_LISTVIEW)
						break;

					LONG result = CDRF_DODEFAULT;
					LPNMLVCUSTOMDRAW lpnmlv = (LPNMLVCUSTOMDRAW)lparam;

					switch (lpnmlv->nmcd.dwDrawStage)
					{
						case CDDS_PREPAINT:
						{
							result = CDRF_NOTIFYITEMDRAW;
							break;
						}

						case CDDS_ITEMPREPAINT:
						{
							const size_t hash = lpnmlv->nmcd.lItemlParam;

							if (hash)
							{
								ITEM_APPLICATION const* ptr = &apps[hash];

								COLORREF new_clr = 0;

								if (app.ConfigGet (L"IsHighlightInvalid", TRUE).AsBool () && ((ptr->is_enabled && ptr->error_count) || (!ptr->is_enabled && (!ptr->is_picoapp && !_r_fs_exists (ptr->real_path)))))
								{
									new_clr = app.ConfigGet (L"ColorInvalid", LISTVIEW_COLOR_INVALID).AsUlong ();
								}
								else if (app.ConfigGet (L"IsHighlightCustom", TRUE).AsBool () && (apps_rules.find (hash) != apps_rules.end () && !apps_rules[hash].empty ()))
								{
									new_clr = app.ConfigGet (L"ColorCustom", LISTVIEW_COLOR_CUSTOM).AsUlong ();
								}
								else if (ptr->is_silent && app.ConfigGet (L"IsHighlightSilent", TRUE).AsBool ())
								{
									new_clr = app.ConfigGet (L"ColorSilent", LISTVIEW_COLOR_SILENT).AsUlong ();
								}
								else if (ptr->is_network && app.ConfigGet (L"IsHighlightNetwork", TRUE).AsBool ())
								{
									new_clr = app.ConfigGet (L"ColorNetwork", LISTVIEW_COLOR_NETWORK).AsUlong ();
								}
								else if (ptr->is_system && app.ConfigGet (L"IsHighlightSystem", TRUE).AsBool ())
								{
									new_clr = app.ConfigGet (L"ColorSystem", LISTVIEW_COLOR_SYSTEM).AsUlong ();
								}

								if (new_clr)
								{
									_r_wnd_fillrect (lpnmlv->nmcd.hdc, &lpnmlv->nmcd.rc, new_clr);
									lpnmlv->clrTextBk = new_clr;

									result = CDRF_NEWFONT;
								}
							}

							break;
						}
					}

					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, result);
					return TRUE;
				}

				case LVN_COLUMNCLICK:
				{
					LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lparam;

					_app_listviewsort (hwnd, pnmv->iSubItem, TRUE);

					break;
				}

				case LVN_GETINFOTIP:
				{
					LPNMLVGETINFOTIP lpnmlv = (LPNMLVGETINFOTIP)lparam;

					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, lpnmlv->iItem);

					if (hash)
					{
						ITEM_APPLICATION* ptr = &apps[hash];

						rstring buffer;

						if (ptr->description)
						{
							buffer = ptr->description;
						}
						else
						{
							buffer = _app_getversion (ptr->real_path);

							if (!buffer.IsEmpty ())
							{
								ptr->description = (LPWSTR)malloc ((buffer.GetLength () + 1) * sizeof (WCHAR));

								if (ptr->description)
								{
									StringCchCopy (ptr->description, buffer.GetLength () + 1, buffer);
								}
							}
						}

						StringCchCopy (lpnmlv->pszText, lpnmlv->cchTextMax, ptr->real_path);

						if (!buffer.IsEmpty ())
						{
							buffer.Insert (L"\r\n" + I18N (&app, IDS_FILEPATH, 0) + L":\r\n" TAB_SPACE, 0);
							StringCchCat (lpnmlv->pszText, lpnmlv->cchTextMax, buffer);

							buffer.Clear ();
						}

						if ((apps_rules.find (hash) != apps_rules.end () && !apps_rules[hash].empty ()))
							buffer.Append (TAB_SPACE + I18N (&app, IDS_HIGHLIGHT_CUSTOM, 0) + L"\r\n");

						if (ptr->error_count || (!ptr->is_picoapp && !_r_fs_exists (ptr->real_path)))
							buffer.Append (TAB_SPACE + I18N (&app, IDS_HIGHLIGHT_INVALID, 0) + L"\r\n");

						if (ptr->is_network)
							buffer.Append (TAB_SPACE + I18N (&app, IDS_HIGHLIGHT_NETWORK, 0) + L"\r\n");

						if (ptr->is_picoapp)
							buffer.Append (TAB_SPACE + I18N (&app, IDS_HIGHLIGHT_PICO, 0) + L"\r\n");

						if (ptr->is_silent)
							buffer.Append (TAB_SPACE + I18N (&app, IDS_HIGHLIGHT_SILENT, 0) + L"\r\n");

						if (ptr->is_system)
							buffer.Append (TAB_SPACE + I18N (&app, IDS_HIGHLIGHT_SYSTEM, 0) + L"\r\n");

						if (!buffer.IsEmpty ())
						{
							buffer.Insert (L"\r\n" + I18N (&app, IDS_NOTES, 0) + L":\r\n", 0);
							StringCchCat (lpnmlv->pszText, lpnmlv->cchTextMax, buffer);
						}
					}

					break;
				}

				case LVN_ITEMCHANGED:
				{
					LPNMLISTVIEW lpnmlv = (LPNMLISTVIEW)lparam;

					if (lpnmlv->uNewState == 8192 || lpnmlv->uNewState == 4096)
					{
						const size_t hash = lpnmlv->lParam;

						if (!hash || !config.is_firstapply || apps.find (hash) == apps.end ())
							return FALSE;

						ITEM_APPLICATION* ptr = &apps[hash];

						ptr->is_enabled = (lpnmlv->uNewState == 8192) ? true : false;

						_r_listview_setitemgroup (hwnd, IDC_LISTVIEW, lpnmlv->iItem, ptr->is_enabled ? 0 : 1);
						_app_listviewsort (hwnd, -1, FALSE);

						SetEvent (config.install_evt);
					}

					break;
				}

				case LVN_GETEMPTYMARKUP:
				{
					NMLVEMPTYMARKUP* lpnmlv = (NMLVEMPTYMARKUP*)lparam;

					lpnmlv->dwFlags = EMF_CENTERED;
					StringCchCopy (lpnmlv->szMarkup, _countof (lpnmlv->szMarkup), I18N (&app, IDS_STATUS_EMPTY, 0));

					SetWindowLongPtr (hwnd, DWLP_MSGRESULT, TRUE);
					return TRUE;
				}

				case NM_DBLCLK:
				{
					LPNMITEMACTIVATE lpnmlv = (LPNMITEMACTIVATE)lparam;

					if (lpnmlv->iItem != -1)
						SendMessage (hwnd, WM_COMMAND, MAKELPARAM (IDM_EXPLORE, 0), 0);

					break;
				}
			}

			break;
		}

		case WM_CONTEXTMENU:
		{
			if (GetDlgCtrlID ((HWND)wparam) == IDC_LISTVIEW)
			{
				const HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_LISTVIEW)), submenu = GetSubMenu (menu, 0);
				const HMENU submenu1 = GetSubMenu (submenu, 1);
				const HMENU submenu3 = GetSubMenu (submenu, 3);

				// localize
				app.LocaleMenu (submenu, I18N (&app, IDS_ADD, 0), 0, TRUE);
				app.LocaleMenu (submenu, I18N (&app, IDS_ADD_FILE, 0), IDM_ADD_FILE, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_ADD_PROCESS, 0), 1, TRUE);
				app.LocaleMenu (submenu, I18N (&app, IDS_SETTINGS, 0), 3, TRUE);
				app.LocaleMenu (submenu, I18N (&app, IDS_DISABLENOTIFICATIONS, 0), IDM_DISABLENOTIFICATIONS, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_ALL, 0), IDM_ALL, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_REFRESH, 0) + L"\tF5", IDM_REFRESH2, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_EXPLORE, 0), IDM_EXPLORE, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_COPY, 0) + L"\tCtrl+C", IDM_COPY, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_DELETE, 0) + L"\tDel", IDM_DELETE, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_CHECK, 0), IDM_CHECK, FALSE);
				app.LocaleMenu (submenu, I18N (&app, IDS_UNCHECK, 0), IDM_UNCHECK, FALSE);

				if (!SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0))
				{
					EnableMenuItem (submenu, 3, MF_BYPOSITION | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_EXPLORE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_COPY, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_DELETE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_CHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					EnableMenuItem (submenu, IDM_UNCHECK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
				}

				// generate processes popup menu
				{
					_app_getprocesslist (&processes);

					if (processes.empty ())
					{
						MENUITEMINFO mii = {0};

						rstring buffer = I18N (&app, IDS_STATUS_EMPTY, 0);

						mii.cbSize = sizeof (mii);
						mii.fMask = MIIM_STATE | MIIM_STRING;
						mii.dwTypeData = buffer.GetBuffer ();
						mii.fState = MF_DISABLED | MF_GRAYED;

						SetMenuItemInfo (submenu1, IDM_ALL, FALSE, &mii);

						buffer.Clear ();
					}
					else
					{
						AppendMenu (submenu1, MF_SEPARATOR, 0, nullptr);

						for (size_t i = 0; i < processes.size (); i++)
						{
							MENUITEMINFO mii = {0};

							mii.cbSize = sizeof (mii);
							mii.fMask = MIIM_ID | MIIM_CHECKMARKS | MIIM_STRING;
							mii.fType = MFT_STRING;
							mii.fState = MFS_DEFAULT;
							mii.dwTypeData = processes.at (i).display_path;
							mii.hbmpChecked = processes.at (i).hbmp;
							mii.hbmpUnchecked = processes.at (i).hbmp;
							mii.wID = IDM_PROCESS + UINT (i);

							InsertMenuItem (submenu1, IDM_PROCESS + UINT (i), FALSE, &mii);
						}
					}
				}

				// show configuration
				{
					const size_t item = (size_t)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); // get first item
					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

					if (hash)
					{
						{
							ITEM_APPLICATION const* ptr = &apps[hash];

							CheckMenuItem (submenu, IDM_DISABLENOTIFICATIONS, MF_BYCOMMAND | (ptr->is_silent ? MF_CHECKED : MF_UNCHECKED));
						}

						AppendMenu (submenu3, MF_SEPARATOR, 0, nullptr);

						if (rules_custom.empty ())
						{
							AppendMenu (submenu3, MF_STRING, IDM_RULES_APPS, I18N (&app, IDS_STATUS_EMPTY, 0));
							EnableMenuItem (submenu3, IDM_RULES_APPS, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						}
						else
						{
							for (size_t i = 0; i < rules_custom.size (); i++)
							{
								ITEM_RULE const* ptr = rules_custom.at (i);

								if (!ptr)
									continue;

								MENUITEMINFO mii = {0};

								BOOL is_checked = FALSE;

								if ((apps_rules.find (hash) != apps_rules.end ()) && !apps_rules[hash].empty ())
								{
									for (size_t j = 0; j < apps_rules[hash].size (); j++)
									{
										if (apps_rules[hash].at (j) == i)
										{
											is_checked = TRUE;
											break;
										}
									}
								}

								WCHAR buffer[128] = {0};
								StringCchPrintf (buffer, _countof (buffer), I18N (&app, IDS_RULE_TITLE_2, 0), ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

								mii.cbSize = sizeof (mii);
								mii.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
								mii.fType = MFT_STRING;
								mii.fState = MFS_DEFAULT;
								mii.dwTypeData = buffer;
								mii.fState = is_checked ? MF_CHECKED : MF_UNCHECKED;
								mii.wID = IDM_RULES_APPS + UINT (i);

								InsertMenuItem (submenu3, IDM_RULES_APPS + UINT (i), FALSE, &mii);
							}
						}

						AppendMenu (submenu3, MF_SEPARATOR, 0, nullptr);
						AppendMenu (submenu3, MF_STRING, IDM_OPENRULESEDITOR, I18N (&app, IDS_OPENRULESEDITOR, 0));
					}
				}

				POINT pt = {0};
				GetCursorPos (&pt);

				TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

				DestroyMenu (menu);
				DestroyMenu (submenu);
			}

			break;
		}

		case WM_SIZE:
		{
			ResizeWindow (hwnd, LOWORD (lparam), HIWORD (lparam));
			RedrawWindow (hwnd, nullptr, nullptr, RDW_ALLCHILDREN | RDW_ERASE | RDW_INVALIDATE);

			break;
		}

		case WM_TRAYICON:
		{
			switch (LOWORD (lparam))
			{
				case NIN_POPUPOPEN:
				{
					//POINT location;
					//GetCursorPos (&location);

					_app_notifyshow (LAST_VALUE, nullptr);
					_app_notifysettimeout (config.hnotification, FALSE, 0);

					break;
				}

				case NIN_POPUPCLOSE:

					_app_notifysettimeout (config.hnotification, TRUE, NOTIFY_TIMER_POPUP);
					break;


				case NIN_BALLOONUSERCLICK:
				{
					if (config.is_popuperrors)
					{
						ShellExecute (hwnd, nullptr, _r_dbg_getpath (APP_NAME_SHORT), nullptr, nullptr, SW_SHOWDEFAULT);

						config.is_popuperrors = FALSE;
					}

					break;
				}

				case NIN_BALLOONHIDE:
				case NIN_BALLOONTIMEOUT:
				{
					config.is_popuperrors = FALSE;

					break;
				}

				case WM_MBUTTONDOWN:
				{
					SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_LOGSHOW, 0), 0);
					break;
				}

				case WM_LBUTTONUP:
				{
					SetForegroundWindow (hwnd);
					break;
				}

				case WM_LBUTTONDBLCLK:
				{
					_r_wnd_toggle (hwnd, FALSE);
					break;
				}

				case WM_RBUTTONUP:
				{
					SetForegroundWindow (hwnd); // don't touch

					HMENU menu = LoadMenu (nullptr, MAKEINTRESOURCE (IDM_TRAY)), submenu = GetSubMenu (menu, 0);

					// localize
					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_SHOW, 0), IDM_TRAY_SHOW, FALSE);
					app.LocaleMenu (submenu, I18N (&app, (app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool () && config.is_filtersinstalled ? IDS_TRAY_STOP : IDS_TRAY_START), app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool () && config.is_filtersinstalled ? L"IDS_TRAY_STOP" : L"IDS_TRAY_START"), IDM_TRAY_START, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_MODE, 0), 3, TRUE);
					app.LocaleMenu (submenu, I18N (&app, IDS_MODE_WHITELIST, 0), IDM_TRAY_MODEWHITELIST, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_MODE_BLACKLIST, 0), IDM_TRAY_MODEBLACKLIST, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_FILTERS, 0), 5, TRUE);

					app.LocaleMenu (submenu, I18N (&app, IDS_USEBLOCKLIST_CHK, 0), IDM_TRAY_USEBLOCKLIST_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_USESTEALTHMODE_CHK, 0), IDM_TRAY_STEALTHMODE_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_INSTALLBOOTTIMEFILTERS_CHK, 0), IDM_TRAY_INSTALLBOOTTIMEFILTERS_CHK, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_RULE_ALLOWINBOUND, 0), IDM_TRAY_RULE_ALLOWINBOUND, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_SYSTEM_RULES, 0), 6, TRUE);
					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_CUSTOM_RULES, 0), 7, TRUE);

					app.LocaleMenu (submenu, I18N (&app, IDS_TRAY_LOG, 0), 8, TRUE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ENABLELOG_CHK, 0), IDM_TRAY_ENABLELOG_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ENABLENOTIFICATIONS_CHK, 0), IDM_TRAY_ENABLENOTIFICATIONS_CHK, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_LOGSHOW, 0), IDM_TRAY_LOGSHOW, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_LOGCLEAR, 0), IDM_TRAY_LOGCLEAR, FALSE);

					app.LocaleMenu (submenu, I18N (&app, IDS_SETTINGS, 0), IDM_TRAY_SETTINGS, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_WEBSITE, 0), IDM_TRAY_WEBSITE, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_ABOUT, 0), IDM_TRAY_ABOUT, FALSE);
					app.LocaleMenu (submenu, I18N (&app, IDS_EXIT, 0), IDM_TRAY_EXIT, FALSE);

					CheckMenuItem (submenu, IDM_TRAY_USEBLOCKLIST_CHK, MF_BYCOMMAND | (app.ConfigGet (L"UseBlocklist2", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_STEALTHMODE_CHK, MF_BYCOMMAND | (app.ConfigGet (L"UseStealthMode", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_INSTALLBOOTTIMEFILTERS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					CheckMenuItem (submenu, IDM_TRAY_RULE_ALLOWINBOUND, MF_BYCOMMAND | (app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					CheckMenuItem (submenu, IDM_TRAY_ENABLELOG_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsLogEnabled", FALSE).AsBool () ? MF_CHECKED : MF_UNCHECKED));
					CheckMenuItem (submenu, IDM_TRAY_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool () ? MF_CHECKED : MF_UNCHECKED));

					if (_r_spinislocked (&config.lock_apply))
					{
						EnableMenuItem (submenu, IDM_TRAY_START, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					}

					if (rules_blocklist.empty ())
					{
						EnableMenuItem (submenu, IDM_TRAY_USEBLOCKLIST_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					}

					// win7 and above
					if (!_r_sys_validversion (6, 1))
					{
						EnableMenuItem (submenu, IDM_TRAY_ENABLELOG_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						EnableMenuItem (submenu, IDM_TRAY_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
					}

					CheckMenuRadioItem (submenu, IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + app.ConfigGet (L"Mode", ModeWhitelist).AsUint (), MF_BYCOMMAND);

					// append system rules
					{
						const HMENU submenu_sub = GetSubMenu (submenu, 6);

						DeleteMenu (submenu_sub, 0, MF_BYPOSITION);

						if (rules_system.empty ())
						{
							AppendMenu (submenu_sub, MF_STRING, IDM_RULES_SYSTEM, I18N (&app, IDS_STATUS_EMPTY, 0));
							EnableMenuItem (submenu_sub, IDM_RULES_SYSTEM, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						}
						else
						{
							for (size_t i = 0; i < rules_system.size (); i++)
							{
								ITEM_RULE const *ptr = rules_system.at (i);

								if (!ptr)
									continue;

								WCHAR buffer[128] = {0};
								StringCchPrintf (buffer, _countof (buffer), L"[%s] %s", ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

								AppendMenu (submenu_sub, MF_STRING, IDM_RULES_SYSTEM + i, buffer);

								if (rules_config[ptr->name])
									CheckMenuItem (submenu_sub, IDM_RULES_SYSTEM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
							}
						}
					}

					// append custom rules
					{
						const HMENU submenu_sub = GetSubMenu (submenu, 7);

						DeleteMenu (submenu_sub, 0, MF_BYPOSITION);

						if (rules_custom.empty ())
						{
							AppendMenu (submenu_sub, MF_STRING, IDM_RULES_CUSTOM, I18N (&app, IDS_STATUS_EMPTY, 0));
							EnableMenuItem (submenu_sub, IDM_RULES_CUSTOM, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
						}
						else
						{
							for (size_t i = 0; i < rules_custom.size (); i++)
							{
								ITEM_RULE const *ptr = rules_custom.at (i);

								if (!ptr)
									continue;

								WCHAR buffer[128] = {0};
								StringCchPrintf (buffer, _countof (buffer), I18N (&app, IDS_RULE_TITLE_1, 0), ptr->is_block ? I18N (&app, IDS_ACTION_2, 0) : I18N (&app, IDS_ACTION_1, 0), ptr->name);

								AppendMenu (submenu_sub, MF_STRING, IDM_RULES_CUSTOM + i, buffer);

								if (ptr->is_enabled)
									CheckMenuItem (submenu_sub, IDM_RULES_CUSTOM + UINT (i), MF_BYCOMMAND | MF_CHECKED);
							}
						}

						AppendMenu (submenu_sub, MF_SEPARATOR, 0, nullptr);
						AppendMenu (submenu_sub, MF_STRING, IDM_OPENRULESEDITOR, I18N (&app, IDS_OPENRULESEDITOR, 0));
					}

					POINT pt = {0};
					GetCursorPos (&pt);

					TrackPopupMenuEx (submenu, TPM_RIGHTBUTTON | TPM_LEFTBUTTON, pt.x, pt.y, hwnd, nullptr);

					DestroyMenu (submenu);
					DestroyMenu (menu);

					break;
				}
			}

			break;
		}

		case WM_WTSSESSION_CHANGE:
		{
			// check for session id
			DWORD session_id = 0;

			ProcessIdToSessionId (GetCurrentProcessId (), &session_id);

			if (session_id != (DWORD)lparam)
				break;

			switch (wparam)
			{
				case WTS_SESSION_UNLOCK:
				case WTS_SESSION_LOGON:
				{
					app.ConfigInit ();

					_app_profileload (hwnd);

					_wfp_initialize (TRUE);

					_app_loginit (TRUE);

					SetEvent (config.install_evt); // apply filters

					break;
				}

				case WTS_SESSION_LOCK:
				case WTS_SESSION_LOGOFF:
				{
					_app_profilesave (hwnd);

					_wfp_uninitialize ();

					_app_loginit (FALSE);

					break;
				}
			}

			break;
		}

		case WM_DEVICECHANGE:
		{
			if (wparam == DBT_DEVICEARRIVAL)
			{
				const PDEV_BROADCAST_HDR lbhdr = (PDEV_BROADCAST_HDR)lparam;

				if (lbhdr->dbch_devicetype == DBT_DEVTYP_VOLUME)
				{
					_app_profileload (hwnd);

					SetEvent (config.install_evt); // apply filters
				}
			}

			break;
		}

		case WM_COMMAND:
		{
			if (HIWORD (wparam) == 0 && LOWORD (wparam) >= IDM_LANGUAGE && LOWORD (wparam) <= IDM_LANGUAGE + app.LocaleGetCount ())
			{
				app.LocaleApplyFromMenu (GetSubMenu (GetSubMenu (GetMenu (hwnd), 2), LANG_MENU), LOWORD (wparam), IDM_LANGUAGE);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_PROCESS && LOWORD (wparam) <= IDM_PROCESS + processes.size ()))
			{
				ITEM_PROCESS const * ptr = &processes.at (LOWORD (wparam) - IDM_PROCESS);

				const size_t hash = _app_addapplication (hwnd, ptr->real_path, 0, FALSE, FALSE);

				_app_listviewsort (hwnd, -1, FALSE);
				_app_profilesave (hwnd);

				ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, hash), -1);

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_RULES_SYSTEM && LOWORD (wparam) <= IDM_RULES_SYSTEM + rules_system.size ()))
			{
				ITEM_RULE* ptr = rules_system.at (LOWORD (wparam) - IDM_RULES_SYSTEM);

				if (ptr)
				{
					const bool new_val = !rules_config[ptr->name];
					ptr->is_enabled = new_val;

					rules_config[ptr->name] = new_val;

					CheckMenuItem (GetMenu (hwnd), IDM_RULES_SYSTEM + (LOWORD (wparam) - IDM_RULES_SYSTEM), MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));

					SetEvent (config.install_evt); // apply filters
				}

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_RULES_CUSTOM && LOWORD (wparam) <= IDM_RULES_CUSTOM + rules_custom.size ()))
			{
				ITEM_RULE* ptr = rules_custom.at (LOWORD (wparam) - IDM_RULES_CUSTOM);

				if (ptr)
				{
					ptr->is_enabled = !ptr->is_enabled;

					CheckMenuItem (GetMenu (hwnd), IDM_RULES_CUSTOM + (LOWORD (wparam) - IDM_RULES_CUSTOM), MF_BYCOMMAND | (ptr->is_enabled ? MF_CHECKED : MF_UNCHECKED));

					_app_profilesave (hwnd);

					SetEvent (config.install_evt); // apply filters
				}

				return FALSE;
			}
			else if ((LOWORD (wparam) >= IDM_RULES_APPS && LOWORD (wparam) <= IDM_RULES_APPS + rules_custom.size ()))
			{
				const size_t idx = LOWORD (wparam) - IDM_RULES_APPS;

				INT item = -1;
				BOOL is_remove = BOOL (-1);

				while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
				{
					const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

					if (hash)
					{
						if (is_remove == BOOL (-1))
						{
							is_remove = FALSE;

							if (apps_rules.find (hash) != apps_rules.end () && !apps_rules[hash].empty ())
							{
								for (size_t j = 0; j < apps_rules[hash].size (); j++)
								{
									if (apps_rules[hash].at (j) == idx)
									{
										is_remove = TRUE;
										break;
									}
								}
							}
						}

						if (is_remove)
						{
							for (size_t j = 0; j < apps_rules[hash].size (); j++)
							{
								if (apps_rules[hash].at (j) == idx)
								{
									apps_rules[hash].erase (apps_rules[hash].begin () + j);
								}
							}
						}
						else
						{
							apps_rules[hash].push_back (idx);
						}
					}
				}

				_app_profilesave (hwnd);
				_app_profileload (hwnd);

				SetEvent (config.install_evt); // apply filters

				return FALSE;
			}

			switch (LOWORD (wparam))
			{
				case IDCANCEL: // process Esc key
				case IDM_TRAY_SHOW:
				{
					_r_wnd_toggle (hwnd, FALSE);
					break;
				}

				case IDM_SETTINGS:
				case IDM_TRAY_SETTINGS:
				case IDC_SETTINGS_BTN:
				{
					app.CreateSettingsWindow ();
					break;
				}

				case IDM_EXIT:
				case IDM_TRAY_EXIT:
				case IDC_EXIT_BTN:
				{
					SendMessage (hwnd, WM_CLOSE, 0, 0);
					break;
				}

				case IDM_WEBSITE:
				case IDM_TRAY_WEBSITE:
				{
					ShellExecute (hwnd, nullptr, _APP_WEBSITE_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_DONATE:
				{
					ShellExecute (hwnd, nullptr, _APP_DONATION_URL, nullptr, nullptr, SW_SHOWDEFAULT);
					break;
				}

				case IDM_CHECKUPDATES:
				{
					app.CheckForUpdates (FALSE);
					break;
				}

				case IDM_ABOUT:
				case IDM_TRAY_ABOUT:
				{
					app.CreateAboutWindow ();
					break;
				}

				case IDM_EXPORT_APPS:
				case IDM_EXPORT_RULES:
				{
					WCHAR path[MAX_PATH] = {0};
					StringCchCopy (path, _countof (path), ((LOWORD (wparam) == IDM_EXPORT_APPS) ? XML_APPS : XML_RULES_CUSTOM));

					OPENFILENAME ofn = {0};

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = path;
					ofn.nMaxFile = _countof (path);
					ofn.lpstrFilter = L"*.xml\0*.xml\0\0";
					ofn.lpstrDefExt = L"xml";
					ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;

					if (GetSaveFileName (&ofn))
					{
						_app_profilesave (hwnd, ((LOWORD (wparam) == IDM_EXPORT_APPS) ? path : nullptr), ((LOWORD (wparam) == IDM_EXPORT_RULES) ? path : nullptr));
					}

					break;
				}

				case IDM_IMPORT_APPS:
				case IDM_IMPORT_RULES:
				{
					WCHAR path[MAX_PATH] = {0};
					StringCchCopy (path, _countof (path), ((LOWORD (wparam) == IDM_IMPORT_APPS) ? XML_APPS : XML_RULES_CUSTOM));

					OPENFILENAME ofn = {0};

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = path;
					ofn.nMaxFile = _countof (path);
					ofn.lpstrFilter = L"*.xml\0*.xml\0\0";
					ofn.lpstrDefExt = L"xml";
					ofn.Flags = OFN_EXPLORER | OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_FORCESHOWHIDDEN;

					if (GetOpenFileName (&ofn))
					{
						_app_profileload (hwnd, ((LOWORD (wparam) == IDM_IMPORT_APPS) ? path : nullptr), ((LOWORD (wparam) == IDM_IMPORT_RULES) ? path : nullptr));
						_app_profilesave (hwnd);

						SetEvent (config.install_evt);
					}

					break;
				}

				case IDM_ALWAYSONTOP_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"AlwaysOnTop", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ALWAYSONTOP_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AlwaysOnTop", new_val);

					_r_wnd_top (hwnd, new_val);

					break;
				}

				case IDM_SHOWFILENAMESONLY_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"ShowFilenames", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_SHOWFILENAMESONLY_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"ShowFilenames", new_val);

					_app_profileload (hwnd);
					_app_listviewsort (hwnd, -1, FALSE);

					break;
				}

				case IDM_AUTOSIZECOLUMNS_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"AutoSizeColumns", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_AUTOSIZECOLUMNS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AutoSizeColumns", new_val);

					_app_listviewresize (hwnd, 0);

					break;
				}

				case IDM_ICONSSMALL:
				case IDM_ICONSLARGE:
				{
					app.ConfigSet (L"IsLargeIcons", LOWORD (wparam) == IDM_ICONSLARGE);

					_app_seticonsize (hwnd);

					break;
				}

				case IDM_ICONSISHIDDEN:
				{
					app.ConfigSet (L"IsIconsHidden", !app.ConfigGet (L"IsIconsHidden", FALSE).AsBool ());

					_app_seticonsize (hwnd);

					_app_profileload (hwnd);

					break;
				}

				case IDM_TRAY_MODEWHITELIST:
				case IDM_TRAY_MODEBLACKLIST:
				{
					if (_r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
						break;

					EnumMode curr = ModeWhitelist;

					if (LOWORD (wparam) == IDM_TRAY_MODEBLACKLIST)
						curr = ModeBlacklist;

					app.ConfigSet (L"Mode", curr);

					_app_refreshstatus (hwnd, FALSE, TRUE);

					CheckMenuRadioItem (GetMenu (hwnd), IDM_TRAY_MODEWHITELIST, IDM_TRAY_MODEBLACKLIST, IDM_TRAY_MODEWHITELIST + curr, MF_BYCOMMAND);

					SetEvent (config.install_evt); // apply filters

					break;
				}

				case IDM_FIND:
				{
					if (!config.hfind)
					{
						static FINDREPLACE fr = {0}; // "static" is required for WM_FINDMSGSTRING

						fr.lStructSize = sizeof (fr);
						fr.hwndOwner = hwnd;
						fr.lpstrFindWhat = config.search_string;
						fr.wFindWhatLen = _countof (config.search_string) - 1;
						fr.Flags = FR_HIDEWHOLEWORD | FR_HIDEMATCHCASE | FR_HIDEUPDOWN | FR_FINDNEXT;

						config.hfind = FindText (&fr);
					}
					else
					{
						SetFocus (config.hfind);
					}

					break;
				}

				case IDM_FINDNEXT:
				{
					if (!config.search_string[0])
					{
						SendMessage (hwnd, WM_COMMAND, MAKEWPARAM (IDM_FIND, 0), 0);
					}
					else
					{
						FINDREPLACE fr = {0};

						fr.Flags = FR_FINDNEXT;
						fr.lpstrFindWhat = config.search_string;

						SendMessage (hwnd, WM_FINDMSGSTRING, 0, (LPARAM)&fr);
					}

					break;
				}

				case IDM_REFRESH:
				case IDM_REFRESH2:
				{
					_app_profileload (hwnd);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_USEBLOCKLIST_CHK:
				case IDM_TRAY_USEBLOCKLIST_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"UseBlocklist2", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_USEBLOCKLIST_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"UseBlocklist2", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_INSTALLBOOTTIMEFILTERS_CHK:
				case IDM_TRAY_INSTALLBOOTTIMEFILTERS_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"InstallBoottimeFilters", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_INSTALLBOOTTIMEFILTERS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"InstallBoottimeFilters", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_STEALTHMODE_CHK:
				case IDM_TRAY_STEALTHMODE_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"UseStealthMode", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_STEALTHMODE_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"UseStealthMode", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_RULE_ALLOWINBOUND:
				case IDM_TRAY_RULE_ALLOWINBOUND:
				{
					BOOL new_val = !app.ConfigGet (L"AllowInboundConnections", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_RULE_ALLOWINBOUND, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"AllowInboundConnections", new_val);

					SetEvent (config.install_evt);

					break;
				}

				case IDM_ENABLELOG_CHK:
				case IDM_TRAY_ENABLELOG_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"IsLogEnabled", FALSE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLELOG_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsLogEnabled", new_val);

					_app_loginit (new_val);

					break;
				}

				case IDM_ENABLENOTIFICATIONS_CHK:
				case IDM_TRAY_ENABLENOTIFICATIONS_CHK:
				{
					BOOL new_val = !app.ConfigGet (L"IsNotificationsEnabled", TRUE).AsBool ();

					CheckMenuItem (GetMenu (hwnd), IDM_ENABLENOTIFICATIONS_CHK, MF_BYCOMMAND | (new_val ? MF_CHECKED : MF_UNCHECKED));
					app.ConfigSet (L"IsNotificationsEnabled", new_val);

					break;
				}

				case IDM_LOGSHOW:
				case IDM_TRAY_LOGSHOW:
				{
					rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

					if (!_r_fs_exists (path))
						return FALSE;

					_r_run (nullptr, _r_fmt (L"%s \"%s\"", app.ConfigGet (L"LogViewer", L"notepad.exe"), path));

					break;
				}

				case IDM_LOGCLEAR:
				case IDM_TRAY_LOGCLEAR:
				{
					rstring path = _r_path_expand (app.ConfigGet (L"LogPath", PATH_LOG));

					if ((config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE) || _r_fs_exists (path))
					{
						if (app.ConfigGet (L"ConfirmLogClear", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES)
							break;

						if (config.hlog != nullptr && config.hlog != INVALID_HANDLE_VALUE)
						{
							_R_SPINLOCK (config.lock_writelog);

							SetFilePointer (config.hlog, 2, nullptr, FILE_BEGIN);
							SetEndOfFile (config.hlog);

							_R_SPINUNLOCK (config.lock_writelog);
						}
						else
						{
							_r_fs_delete (path);
						}

						_r_fs_delete (path + L".bak");
					}

					break;
				}

				case IDM_TRAY_START:
				case IDC_START_BTN:
				{
					const BOOL state = app.ConfigGet (L"IsFiltersEnabled", FALSE).AsBool () && config.is_filtersinstalled;

					if (_app_installmessage (hwnd, !state))
					{
						if (!state)
							SetEvent (config.install_evt); // install
						else
							SetEvent (config.destroy_evt); // uninstall
					}

					break;
				}

				case IDM_ADD_FILE:
				{
					WCHAR files[_R_BUFFER_LENGTH] = {0};
					OPENFILENAME ofn = {0};

					size_t item = 0;

					ofn.lStructSize = sizeof (ofn);
					ofn.hwndOwner = hwnd;
					ofn.lpstrFile = files;
					ofn.nMaxFile = _countof (files);
					ofn.lpstrFilter = L"*.exe\0*.exe\0*.*\0*.*\0\0";
					ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_ENABLESIZING | OFN_PATHMUSTEXIST | OFN_FORCESHOWHIDDEN;

					if (GetOpenFileName (&ofn))
					{
						if (files[ofn.nFileOffset - 1] != 0)
						{
							item = _app_addapplication (hwnd, files, 0, FALSE, FALSE);
						}
						else
						{
							LPWSTR p = files;
							WCHAR dir[MAX_PATH] = {0};
							GetCurrentDirectory (_countof (dir), dir);

							while (*p)
							{
								p += wcslen (p) + 1;

								if (*p)
									item = _app_addapplication (hwnd, _r_fmt (L"%s\\%s", dir, p), 0, FALSE, FALSE);
							}
						}

						_app_listviewsort (hwnd, -1, FALSE);
						_app_profilesave (hwnd);

						ShowItem (hwnd, IDC_LISTVIEW, _app_getposition (hwnd, item), -1);
					}

					break;
				}

				case IDM_ALL:
				{
					_app_getprocesslist (&processes);

					for (size_t i = 0; i < processes.size (); i++)
						_app_addapplication (hwnd, processes.at (i).real_path, 0, FALSE, FALSE);

					_app_listviewsort (hwnd, -1, FALSE);
					_app_profilesave (hwnd);

					break;
				}

				case IDM_DISABLENOTIFICATIONS:
				case IDM_EXPLORE:
				case IDM_COPY:
				case IDM_CHECK:
				case IDM_UNCHECK:
				{
					INT item = -1;
					BOOL new_val = BOOL (-1);

					rstring buffer;

					while ((item = (INT)SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETNEXTITEM, item, LVNI_SELECTED)) != -1)
					{
						const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, item);

						if (hash)
						{
							ITEM_APPLICATION* ptr = &apps[hash];

							if (LOWORD (wparam) == IDM_EXPLORE)
							{
								if (!ptr->is_picoapp)
								{
									if (_r_fs_exists (ptr->real_path))
										_r_run (nullptr, _r_fmt (L"\"explorer.exe\" /select,\"%s\"", ptr->real_path));
									else if (_r_fs_exists (ptr->file_dir))
										ShellExecute (hwnd, nullptr, ptr->file_dir, nullptr, nullptr, SW_SHOWDEFAULT);
								}
							}
							else if (LOWORD (wparam) == IDM_COPY)
							{
								buffer.Append (ptr->real_path).Append (L"\r\n");
							}
							else if (LOWORD (wparam) == IDM_DISABLENOTIFICATIONS)
							{
								if (new_val == BOOL (-1))
									new_val = !ptr->is_silent;

								ptr->is_silent = new_val ? true : false;
							}
							else if (LOWORD (wparam) == IDM_CHECK || LOWORD (wparam) == IDM_UNCHECK)
							{
								ptr->is_enabled = (LOWORD (wparam) == IDM_CHECK) ? true : false;
								_r_listview_setitemcheck (hwnd, IDC_LISTVIEW, item, LOWORD (wparam) == IDM_CHECK ? TRUE : FALSE);
							}
						}
					}

					if (LOWORD (wparam) == IDM_DISABLENOTIFICATIONS)
					{
						_app_profilesave (hwnd);

						SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_REDRAWITEMS, 0, apps.size ()); // redraw (required!)
					}
					else if (LOWORD (wparam) == IDM_COPY)
					{
						buffer.Trim (L"\r\n");
						_r_clipboard_set (hwnd, buffer, buffer.GetLength ());
					}

					break;
				}

				case IDM_OPENRULESEDITOR:
				{
					ITEM_RULE* ptr = (ITEM_RULE*)malloc (sizeof (ITEM_RULE));

					if (ptr)
					{
						SecureZeroMemory (ptr, sizeof (ITEM_RULE));

						if (DialogBoxParam (nullptr, MAKEINTRESOURCE (IDD_EDITOR), hwnd, &EditorProc, (LPARAM)ptr))
						{
							rules_custom.push_back (ptr);

							_app_profilesave (hwnd);
							_app_profileload (hwnd);

							SetEvent (config.install_evt);
						}
						else
						{
							_app_freerule (&ptr);
						}
					}

					break;
				}

				case IDM_DELETE:
				{
					if (!SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_GETSELECTEDCOUNT, 0, 0) || (app.ConfigGet (L"ConfirmDelete", TRUE).AsBool () && _r_msg (hwnd, MB_YESNO | MB_ICONQUESTION, APP_NAME, nullptr, I18N (&app, IDS_QUESTION, 0)) != IDYES))
						break;

					const size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1;

					BOOL is_checked = FALSE;

					_R_SPINLOCK (config.lock_access);

					size_t item = LAST_VALUE;

					for (size_t i = count; i != LAST_VALUE; i--)
					{
						if (ListView_GetItemState (GetDlgItem (hwnd, IDC_LISTVIEW), i, LVNI_SELECTED))
						{
							const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i);

							if (hash)
							{
								SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_DELETEITEM, i, 0);

								is_checked = _app_freeapplication (&apps[hash], hash);

								item = i;
							}
						}
					}

					_R_SPINUNLOCK (config.lock_access);

					if (item != LAST_VALUE)
						ShowItem (hwnd, IDC_LISTVIEW, min (item, _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1), -1);

					_app_profilesave (hwnd);
					_app_profileload (hwnd);

					if (is_checked)
						SetEvent (config.install_evt);

					break;
				}

				case IDM_PURGEN:
				{
					BOOL is_deleted = FALSE;
					BOOL is_checked = FALSE;

					const size_t count = _r_listview_getitemcount (hwnd, IDC_LISTVIEW) - 1;

					_R_SPINLOCK (config.lock_access);

					for (size_t i = count; i != LAST_VALUE; i--)
					{
						const size_t hash = (size_t)_r_listview_getitemlparam (hwnd, IDC_LISTVIEW, i);

						if (hash)
						{
							ITEM_APPLICATION* ptr = &apps[hash];

							if (ptr->is_enabled && ptr->error_count || (!_r_fs_exists (ptr->real_path) && !ptr->is_picoapp))
							{
								SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_DELETEITEM, i, 0);

								is_deleted = TRUE;

								if (_app_freeapplication (ptr, hash))
									is_checked = TRUE;
							}
						}
					}

					_R_SPINUNLOCK (config.lock_access);

					if (is_deleted)
					{
						_app_profilesave (hwnd);
						_app_profileload (hwnd);

						if (is_checked)
							SetEvent (config.install_evt);
						else
							SendDlgItemMessage (hwnd, IDC_LISTVIEW, LVM_REDRAWITEMS, 0, apps.size ()); // redraw (required!)
					}

					break;
				}

				case IDM_SELECT_ALL:
				{
					ListView_SetItemState (GetDlgItem (hwnd, IDC_LISTVIEW), -1, LVIS_SELECTED, LVIS_SELECTED);
					break;
				}

				case IDM_ZOOM:
				{
					ShowWindow (hwnd, IsZoomed (hwnd) ? SW_RESTORE : SW_MAXIMIZE);
					break;
				}
			}

			break;
		}
	}

	return FALSE;
}

INT APIENTRY wWinMain (HINSTANCE, HINSTANCE, LPWSTR, INT)
{
	if (app.CreateMainWindow (&DlgProc, &initializer_callback))
	{
		MSG msg = {0};

		HACCEL haccel = LoadAccelerators (app.GetHINSTANCE (), MAKEINTRESOURCE (IDA_MAIN));

		while (GetMessage (&msg, nullptr, 0, 0) > 0)
		{
			if (haccel)
				TranslateAccelerator (app.GetHWND (), haccel, &msg);

			if (!IsDialogMessage (app.GetHWND (), &msg))
			{
				TranslateMessage (&msg);
				DispatchMessage (&msg);
			}
		}

		if (haccel)
			DestroyAcceleratorTable (haccel);
	}

	return ERROR_SUCCESS;
}
