v2.0.4 Beta (23 August 2017)
+ added grouping for rules
- removed listen connections blocking feature
- improved import/export xml database
- fixed open rules editor action in main window
- fixed crash on delete rules
- fixed ui bugs
- fixed bugs

v2.0.3 Beta (17 August 2017)
+ added import/export applications list feature
+ added mode selection into installation dialog
+ added tooltips into the notifications ui
+ added remembering collapsed state for the listview groups
+ increased internal rules loading speed (please update blocklist.xml and rules_system.xml to latest versions)
+ increased rules list icons size
- changed default sorting configuration
- fixed notifications ui logic
- fixed filters installation state flag
- fixed support some domains
- fixed carriage return type for rules_system.xml
- fixed thread spinlock
- updated localization
- updated project sdk
- updated blocklist
- fixed bugs

v2.0.2 Beta (7 August 2017)
- fixed incorrect vector index

v2.0.1 Beta (7 August 2017)
+ added update checking for new beta version
+ added flush dns cache after filters applied
+ set max prefix length for ipv6 addresses to 64
+ new rules editor interface
+ changed minimum width for a main window
- fixed running with "/minimized" argument under uac
- updated localization
- fixed bugs

v2.0 Beta (1 August 2017)
+ new notification ui
+ show notifications only for whitelist mode
+ allow listen connections for all is enabled by default
+ added import custom rules from file feature
+ added support to load large xml files
+ added more information into the main window
+ added apps grouping (allowed/blocked)
+ added custom rules syntax checking
+ added support dns resolution for custom rules
+ added resolving shortcut path
+ added notification display timeout config
+ save internal rules configuration into xml
+ purgen remove only apps with errors
+ minimized dropped packets log size (union remote and local address information)
+ minimized memory usage
- removed "trust no one" mode
- updated system rules
- updated blocklist
- fixed dropped packets logging hibernation (win7 and above)
- fixed remember windows size and position sometimes
- fixed version string trimming
- fixed ui bugs
- fixed memory leaks
- fixed bugs

v1.6.5 (1 June 2017)
+ do not block listen connections on stealth-mode
+ do not block listen connections on boot-time
- fixed dropped events does not shutdown on exit (win7 and above)
- fixed memory leak

v1.6.4 (31 May 2017)
+ added fallback if blocklist and/or system rules not found
+ added more dropped events logging (win7 and above)
- fixed dropped events subscription duplicate (win7 and above)
- fixed run as admin does not work sometimes
- updated blocklist
- fixed bugs

v1.6.3 (27 May 2017)
+ generate unique session key at startup
- fixed custom app rules crash on delete
- fixed lookup account sid length mismatch
- fixed dropped packets logging crash (win7 and above)
- stability improvements
- updated system rules
- fixed bugs

v1.6.2 (24 May 2017)
+ create filter even if file doesn't exists (drive must be mounted)
+ allow inbound & listen traffic only if stealth-mode does not enabled
+ added required rules for the ipv6 stack to work properly
+ stealth-mode marked as experimental
- fixed com library initialization (again!)
- fixed stealth-mode
- fixed bugs

v1.6.1 (23 May 2017)
+ added username and domain information to the window title
+ added configuration refresh on user logon
- removed static wfp session key (request)
- fixed com library initialization
- fixed incorrect return value on file not found error

v1.6 (19 May 2017)
+ added stealth-mode (to prevent udp/tcp port scanning)
+ added acl (access control list) to the engine
+ added gridline for the listview config
+ added item into the custom rules menu for open rules editor
+ added version-independent network events api call (win7 and above)
+ added dropped packets log file size limit to 1mb (win7 and above)
+ reset windows firewall to its initial state when restore it back
+ blocklist marked as experimental
- removed custom rules from package
- fixed dropped packets logging stop sometimes (win7 and above)
- fixed removing custom rules
- fixed classic ui
- fixed bugs
- ui fixes

v1.5.5 (6 May 2017)
+ added installer
+ added static wfp session key
+ copy filter name if description is not available for dropped packets log
- removed "file not found" xml parsing errors
- revert trim rules back (request)
- fixed index flag cannot be set (win8 and above)
- fixed ui bugs

v1.5.4 (30 April 2017)
- trim executable version string
- fixed filters uninstallation
- fixed duplicate update checking
- fixed bugs

v1.5.3 (27 April 2017)
- fixed restoring windows firewall state
- fixed incorrect window size at startup sometimes

v1.5.2 (27 April 2017)
- fixed dropped packets log spinlock cannot be unlocked (critical)
- fixed displaying "file not found" errors
- restore listview selection after application delete
- disable main button on filters installation
- improved shutting down windows firewall feature
- removed unnecessary whitespace trims
- optimized window resizing
- updated to the latest sdk
- fixed bugs

v1.5.1 (17 April 2017)
+ added remember window position and size feature
+ added "enable errors notifications" config
+ added f11 hotkey for maximize window
- disable wow64 filesystem redirection
- changed error log notification text
- fixed possible memory leak

v1.5 (15 April 2017)
+ added index flag to the filters, to help enable faster lookup during classification (win8 and above)
+ added app container loopback traffic permission (win8 and above)
+ added "allow listen connections for all" config
+ added loopback indication for dropped packets log
- copy real path instead display path on copy command in main window listview
- do not show dropped packets notifications when filters are not installed
- if boot-time filters enabled then apply system rules for boot-time too
- custom rules for apps does not saved sometimes
- removed running without admin rights feature
- changed notification about errors logic
- changed alt+f4 behaviour (request)
- cosmetic fixes for tooltips
- fixed disabling windows firewall on some systems
- fixed settings tabstop doesn't work
- fixed incorrect listview icons for some apps
- fixed process list some apps have no icons
- fixed possible duplicate filters
- fixed purge unused apps
- stability improvements
- updated translations
- updated system rules
- updated pugixml
- fixed bugs

v1.4.6 (5 April 2017)
+ added write error logs into a file feature
- fixed process list does not recognize pico applications on win10
- updated translations
- fixed bugs

v1.4.5 (4 April 2017)
+ added pico support (subsystem for unix-based applications) for win10
+ added l2tp/ipsec for system rules
+ added localhost for custom rules
- fixed access denied for some self protected applications (like "ekrn.exe" for nod32)

v1.4.4 (28 March 2017)
+ added ipsec connections monitoring into the log (win8 and above)
- cosmetic fixes about tray notifications
- fixed dropped packets callback crash (critical)
- fixed displaying tray notifications on win10
- fixed possible duplicate filters

v1.4.3 (27 March 2017)
+ added provider information into the log/notifications
+ added displaying error messages on filters configuration via tray popup
- fixed link processing in settings window
- cosmetic changes about debug error messages
- removed "nlatunicast" flag from loopback permissions
- updated blocklist
- code cleanup

v1.4.2 (19 February 2017)
+ added hints to rules pages
- fixed filter creation for nonexistent apps (critical)
- fixed resource definition
- fixed purge unused apps
- cosmetic changes about rules tooltip

v1.4.1 (4 February 2017)
- fixed suspended drop event callback (critical)
- fixed suspended apply filters thread (critical)
- fixed purge unused apps

v1.4.0 (31 January 2017)
* revert original project name
+ added mode changing confirmation
+ added reading information about "System" process
+ added protocol and version (ports only) option into the rules editor
+ added blocklist editor (set it "on" or "off" only)
+ added custom rules applying to the apps feature
+ added "show filenames only" option
+ added icon indication for rules
+ added loopback permission for "trust no one" mode
+ moved system rules into the "rules_system.xml" file
+ clear log even if logging to a file is not enabled
- do not load information about apps from shared resources
- fixed profile not saved if filters is not installed
- removed tray balloon tips on filters changing
- boot-time filters marked as experimental
- improved working under uac (no rights)
- improved system apps detection
- cosmetic fixes in process list
- cosmetic fixes in apps tooltip
- fixed race conditions
- updated translation
- updated blocklist
- updated ui
- fixed bugs

v1.3.7 (21 November 2016)
- fixed special rules crash on apply settings
- fixed special rules reinitialization in main menu/tray menu
- updated translation

v1.3.6 (20 November 2016)
+ added loopback permission for boot-time filters
+ added ip:port syntax for special rules
+ added default values for new special rules
- fixed restore listview selection
- fixed special rules crash on delete item
- fixed log path unexpand environment strings

v1.3.5 (17 November 2016)
- fixed configuration saving (critical)
- fixed xml saving (critical)

v1.3.3 (14 November 2016)
+ added boot-time filters for prevent data leak during system startup, even before "Base Filtering Engine" (BFE) service starts
+ added "purge unused applications" feature
+ added "hide icons" feature
+ added documentation for rules editor and filter page
+ added special rules to main menu/tray menu
- fixed multi-select configuration in listview
- fixed system imagelist destroying
- fixed dropped packets filters decription empty sometimes
- fixed find dialog crash
- removed inbound events logging for some reason
- updated translation
- updated pugixml

v1.3.2 (29 October 2016)
+ added domain\username indication to log events
+ added filter name to log events
+ added ip/port range support for rules
+ combined ip and port rules settings page
+ added save checkbox state on install/delete message
+ added listen loopback permission
+ added inbound events logging
- set xml load encoding to auto
- improved logic for detect incorrect applications
- listview empty text indication doesn't changed at locale change
- changed default log parameters for "system" & "svchost.exe"
- fixed inbound ip doesn't affected in applied rules
- changed default color for silent applications
- removed output log into debug log feature
- improved open/save file dialog flags
- optimized log callback
- updated blocklist
- updated translation
- minor improvements

v1.3.1 (21 October 2016)
+ added option to set application for open log file
+ added option to exclude application from log
+ added option for disable notification sound
+ added middle click on tray icon now open log file
+ added empty listview text indication
+ added rules highlighting
+ added usage examples to rules.xml 
- reorganized log settings
- fixed unit of time for notification timeout had in minutes (instead seconds)
- fixed special rules saved twice
- fixed skip user account control not worked
- fixed special rules saved empty when filters not installed
- fixed system rules cannot be unchecked automatically
- changed auto byte order mask for xml load/save
- replaced QueryFullProcessImageName to NtQueryInformationProcess
- stability improvements
- updated translation
- ui improvements

v1.3 (15 October 2016)
+ disable/enable windows firewall on filters installation/deletion
+ normalize nt paths for dropped packets callback
+ exclude "svchost.exe" & "system" from log configuration
+ abbility to show/clear log
+ resize support as in alpha builds
+ hotkeys for menu items
+ PathUnExpandEnvStrings for log path
+ confirmation settings for exit/deleting application/log clear
- changed message boxes style
- changed default color for invalid applications
- fixed access denied for some processes
- config doesn't saved if user don't trigger apply filters
- improved windows firewall control
- stability improvements
- updated translation

v1.2.118 (5 October 2016)
- fixed crash on filters installation
- save sort order into profile (regression)
- unable to clear log path config
- stability improvements
- updated translation

v1.2.117 (4 October 2016)
* changed ui by IAEA safety standards (issue #2)
+ added logging configuration
- now filters disabled by default
- inbound ports doesn't blocked for special rules
- updated translation
- minor improvements

v1.1.116 (30 September 2016)
+ added "listen" layer blocking
+ added forgotten rules into settings menu
+ added open folder by double click for listview
+ added shared resources highlighting
- dropped packets logging optimizations
- fixed process list menu icons with classic ui
- updated translation
- minor improvements

v1.1.115 (22 September 2016)
+ added more information to dropped packets logging (win7 and above)
- fixed inbound dont't blocking
- cannot add port in rules editor
- updated translation

v1.1.114 (21 September 2016)
- rules in settings dialog cannot be saved

v1.1.113 (20 September 2016)
- fixed cannot delete application from list bug
- fixed default values for settings
- filters for services do not set
- fixed dhcpv6 ports configuration
- fixed windows firewall configuration
- updated translation
- fixed bugs

v1.1.112 (19 September 2016)
* project renamed
+ added "trust no one" mode
+ added dropped packets logging to debugview (win7 and above)
+ added automatic rules applying on insert device
+ added name for filters
+ moved telemetry rules to the resources
- rewritten rules editor
- improved classic ui config
- fixed tray tooltip visibility
- decreased ballon tips

v1.0.101 (7 September 2016)
+ added outbound connections configuration for rules
+ added color configuration
- changed weight for sublayer
- removed startup popup notification
- updated translation

v1.0.100 (30 August 2016)
+ added individual applicaiton rules
+ added highlight for invalid items
+ added icons for process popup menu
- rewritten process retrieving code (fixed many bugs)
- restored "Refresh" listview feature
- prevent for duplicate filters on some configurations
- unlocked application layer configuration in "for all (global)" rules
- loaded default rules on initilize settings
- updated translation
- fixed bugs

v1.0.99 (25 August 2016)
+ added windows update and http protocol allow rule
- blocking list editor context menu does not showed
- blocking list editor first item cannot be edited
- fix rules checkbox displaying in settings window
- rules re-grouped by OSI model
- changed listview custom draw
- updated translation
- updated ui

v1.0.98b (22 August 2016)
+ added ballon tip on remove filters
- changed default rules for all
- changed default rules for allowed

v1.0.96b (21 August 2016)
- settings improvements
- fixed duplicate popup on startup
- cleanup code/resources
- updated translation

v1.0.94b (20 August 2016)
- updated translation
- removed excess tray popups
- filters not applied when firewall started manually
- tray icon does not change on firewall enable
- improved block list editor

v1.0.90b (19 August 2016)
+ now compiled with "treat warnings as error" parameter
+ added global rules configuration
+ added "disable windows firewall" param
+ added balloon tips param
+ started localization
- fixed buffer overflow on remove filters
- finished blocking list editor
- removed duplicate filters
- fixed memory leaks on change window icon
- ui improvements
+ now checking updates worked
- fixed statusbar redraw forgotten
- updated translation
- ui fixes

v1.0.81a (18 August 2016)
+ added indication allowed/blocked item count
+ added edit blocking list support
+ added loopback configuration into profile
- fixed profile mask set to default at startup
- improved profile configuration dialog

v1.0.79a (17 August 2016)
+ added tooltips
- inbound traffic not blocked sometimes

v1.0.77a (16 August 2016)
+ added profiles for applications
+ added more hotkeys
+ find now worked
- increased startup speed (moved filters applying function into another thread)
- fixed ntp setting
- fixed memory leak
- ui improvements

v1.0.52a (13 August 2016)
- fixed main window dpi
- fixed scroll height on change icon size
- removed comments/debug strings
- fixed application state does not saved on check/uncheck
+ added "select all" (ctrl+a) hotkey
+ added "telemetry.xml"
- ip parsing optimization (due ParseNetworkString)
- ui improvements

v1.0.42a (12 August 2016)
+ added outbound/inbound ICMP permission
+ auto apply filters on settings change
- removed "Apply rules" button (do not need it anymore)
- increased startup speed
- removed process monitor
- changed config defaults
- ui improvements
- small bug fixes

v1.0.30a (11 August 2016)
+ added outbound loopback permission
- fixed DHCP/DNS/NTP allowed for all
- fixed memory leak

v1.0.23a (6 August 2016)
+ added support to allow DHCP/DNS/NTP through svchost.exe
- fixed NtOpenProcess access rights

v1.0.21a (4 August 2016)
- fixed uncheck item doesn't saved
- previous build changes extended
- removed duplicate function calling
- menu fixes

v1.0.18a (4 August 2016)
+ added internal telemetry blocking
+ added menu accelerators
+ moved process monitor to low-level
- fixed provider persistency
- re-fuck-to-ring

v1.0.13a (2 August 2016)
+ added search abbility for existing application filters and show them
+ added tray icon indication
+ added find in application list feature
- increased speed (due transactions)
- fixed 32-bit working under 64-bit system
- ui improvements

v1.0.10a (30 July 2016)
- fixed xml saving/parsing bug
- fixed read memory access on filter enumeration

v1.0.7a (29 July 2016)
+ added inbound loopback permission
+ added balloon tips on events
- fixed inbound permission for allowed applications
- fixed critical bug (incorrect memory address access)
- common optimizations
- ui improvements

v1.0.4a (27 July 2016)
+ added icons size configuration
- fixed background thread to catch applications doesn't worked
- fixed v6 callout misstake
- ui improvements

v1.0.3a (26 July 2016)
+ added more settings
- improved ui
- removed service (do not need it anymore)

v1.0.2a (26 July 2016)
- first public version
