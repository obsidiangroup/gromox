1.2 (2021-09-01)
================

Fixes:

* zcore: repair wrong (parent_)entryid being passed to syncers
* lib: unbreak save/restore of inbox rules from zcore
* http: fix a hang during Outlook's autodiscovery due to incorrect
  HTTP request-body processing
* kdb2mt: implement documented SRCPASS environment variable
* kdb2mt: reduce a false condition in checking for attachment existence
  (reduces PK-1012)
* kdb2mt: recognize fixated namedprops in the range 0x8000..0x84FF
  (appointment data, contact data)

Enhancements:

* kdb2mt: new ``-v`` option to show progress for large folders
* kdb2mt: implement support for embedded messages (reducing PK-1012)
* kdb2mt: support reading gzip attachments
* kdb2mt: make ``--src-mbox`` option more useful by ignoring orphan stores when
  resolving. (Orphan stores can still be extracted with ``--src-guid``.)
* kdb2mt: make ``-s`` (splice) actually effective, by looking for the needed
  PR_IPM_SUBTREE property in the proper MAPI object.
* kdb2mt: added more folder mappings for splice mode (appointments, contacts,
  journal, notes, tasks, drafts) so that e.g. Drafts does get copied to Drafts,
  rather than making a new "Drafts" folder.
* kdb2mt: skip search folders on extraction (these are empty anyway)


1.1 (2021-08-17)
================

Changes:


* mt2exm: wait for pff2mt stream begin before connecting to exmdb
  so as to not run into a timeout
* mysql_adaptor: complain if there is an overlap between user and
  alias table


1.0 (2021-08-06)
================

Enhancements:

* Added an importer for Kopano databases, gromox-kdb2mt.
  This is meant to be used with gromox-mt2exm.
* ldap_adaptor: referrals in search results emitted by MSAD are now ignored.

Changes:

* gromox-pffimport was split into two programs that now need to be
  used as a piped combination, gromox-pff2mt and gromox-mt2exm.
* gromox-smtp has been renamed to gromox-delivery-queue.

Fixes:

* pffimport/pff2mt no longer aborts with assertion PF-1034/1038.


0.27 (2021-07-13)
=================

Fixes:

* oxcical: recognize calendar scale "LunarRokuyou"
* oxcical: fix PidLidIntendedStatus always being olTentative
* pam_gromox: fix NULL deref when the plugin is used
* Avoid double UTF-8 transformation by html_to_plain
* zcore: do not switch to Chinese when desired store language is unavailable

Changes:

* SIGHUP now reloads the exrpc_debug, rop_debug and/or zrpc_debug config
  directives.
* smtp: bump logmsg severity for rejected deliveries so that they become
  default-visible in journalctl.

Enhancements:

* exchange_emsmdb, zcore: store ownership bit (tentatively configured by
  setting owner on Top Of Information Store)
* oxcical: support for the olWorkingElsewhere busy status
* authmgr: implement "allow_all" auth mode
* authmgr: switch default mode to "externid"
* dbop: new db schema n77 to support sync policy of mobile devices


0.26 (2021-07-03)
=================

Fixes:

* exmdb_provider: cure "SELECT count(idx) ..." error messages
* exmdb_provider: fix nonfunctional recursive deletion of folders

Changes:

* config parser: reduce potency of the '#' character in config files /
  '#' only introduces a comment if it is at the start of line now.
  This allows for using '#' in the credentials for MySQL/LDAP.

Enhancements:

* pffimport: skip over broken attachments rather than abort
* pffimport: new -p option to dump properties in more detail
* pffimport: translation of named properties is now implemented
* pffimport: contacts, notes, tasks are now handled
* pffimport: new -s option to splice PFF folders into existing mailbox
* exmdb_provider: requests to set the read flag are now honored
* authmgr, ldap_adaptor, mysql_adaptor: config is now reloaded on SIGHUP


0.25 (2021-05-09)
=================

Fixes:

* http: fix a garbage return code in the emsmdb logon procedure
* zcore: fix a use-after-free crash when opening the addressbook
* event: speedier shutdown of service


0.24 (2021-05-31)
=================

Enhancements:

* zcore: new config directive "zrpc_debug"
* exchamge_emsmdb: new config directory "exrpc_debug"

Fixes:

* imap: fix standard folders' garbled name display (Sent Items, Junk, etc.)
* authmgr: quench stray password resets to the SQL DB
* pffimport: skip over nonsensical bytes in Unicode string properties
* pffimport: skip over unknown nodes when doing tree-analyze (-t)
* tools: fix crash when /etc/gromox is unreadable
* Overquota events are now signalled with better error message
  in grommunio-web (MAPI_E_STORE_FULL rather than MAPI_E_DISK_FULL).

Changes:

* mysql_adaptor: first-time password functionality is now disabled by default;
  new config directive "enable_firsttimepw".
* mysql_adaptor: SHA512-crypt is now used.


0.23 (2021-05-13)
=================

Enhancements:

* New utility ``gromox-pffimport`` for importing PFF/PST/OST

Fixes:

* exchange_emsmdb: fix a crash upon retrieval of calculated properties
* lib: fix crash when zcore uses a zero-length name during
  zcore_callid::COPYFOLDER


0.22 (2021-05-03)
=================

Enhancements:

* exch: implement send quota
* logthru: add logfile support, add an close-open-cycle on SIGHUP
  to facilitate external log rotation

Changes:

* mysql_adaptor: change default schema_upgrades action to "skip"
* exch: remove log_plugin service plugin
* exch: remove mod_proxy plugin

Fixes:

* midb: fix leftover debugging breakpoint infinite loop
* ldap_adaptor: fix null deref when LDAP server is away
* exmdb_provider: fix double-free on shutdown
* delivery: replace pthread_cancel calls, fixing a crash on shutdown


0.21 (2021-04-20)
=================

Enhancements:

* exmdb_provider, midb: emit log message when and which sqlite
  DB cannot be opened

Fixes:

* imap: do not advertise RFC2971 commands when so disabled
* imap: fix misparsing of {octet}-prefixed literals
* imap: quote folder names in LIST, LSUB, XLIST, STATUS results
* exmdb_provider: add a missing iterator advancements in message_rectify_message
* timer: avoid crash on shutdown
* midb: fix concurrent use of sqlite data structure
* midb_agent: speed up termination during midb connection trying


0.20 (2021-04-14)
=================

Enhancements:

* daemons: SIGHUP support / `systemctl reload` is now possible for
  a general reload action
* http: much speedier shutdown, and hence `systemctl restart`
* exchange_nsp: reload now causes the Outlook-facing AB cache to empty
* domain_list: reload now causes rereading of domain_list.txt
* alias_translator: reload now causes rereading of alias_addresses.txt
* adaptor: reload now causes immediate regeneration of all txt files
  that adaptor would normally generate only periodocally
* mysql_adaptor: the "schema_upgrades" config gained an option for "host:"

Fixes:

* exmdb_provider: the wrong store quota property was evaluated when
  copying/moving messages
* exmdb_provider: fix a mutex double unlock
* exchange_emsmdb: fix a crash during rop_getpropertiesall
* mod_proxy: fix an out-of-bounds access while parsing proxy.txt
* imap: fix a double-free that occurred during shutdown
* lib: fix use-after-destruction near ext_buffer_push_release


0.19 (2021-03-30)
=================

Changes:

* exmdb_provider: allow reduction of cache_interval down to 1s

Fixes:

* dbop: classes.filter column was not created on dbop -C
* exchange_emsmdb: fix integer multiplication overflow during quota check
* exchange_emsmdb: fix ftstream_parser_create running into EISDIR error
* exchange_emsmdb: fix read from uninitialized variable
* php-ews: send error messages to error log rather than stdout


0.18 (2021-03-26)
=================

Changes:

* http: Split some unspecific HTTP 500 errors into 500, 502, 503, 504.
* http: Incomplete RTF documents are now decoded rather than "Not Found"
  being emitted.
* mod_cache: added the /web path to the built-in defaults
* mod_fastcgi: fix php-fpm yielding Not Found for /web
* mod_fastcgi: changed the underlying path of the built-in rule for
  /web to /usr/share/grommunio-web
* The systemd .target units were removed

Fixes:

* zcore: moving mails between two stores had erroneously used the
  old mail folder's id for deletion and failed.
* daemons: Fix a crash when programs shut down before entering the mainloop.


0.17 (2021-03-06)
=================

Enhancements:

* http: Raise max_router_connections & max_rpc_stub_threads limits
  to cope with reconnect storms from midb and zcore.
* doc: manpage for autodiscover

Changes:

* all daemons: Disabled the ip6_container and ip6_filter plugins
  for now; the default settings block too many connections.

Fixes:

* http: fix crash when user_default_lang is unset
* imap: advertise STARTTLS when indeed supported
* all daemons: avoid rejecting IPv6 connections from long addresses


0.16
====
* Configuration that lived in ${datadir} was moved to /etc/gromox:
  cache.txt, exmdb_list.txt, event_acl.txt, fastcgi.txt, midb_acl.txt,
  proxy.txt, rewrite.txt, timer_acl.txt. Their presence is also
  optional now; built-in defaults will be used if absent.
* domain_list.cfg and user_filter.cfg are now optional


0.15
====
* ldap_adaptor: new "ldap_start_tls" config directive to control STARTTLS.
* exchange_nsp: deliver PT_MV_UNICODE properties to clients
* authmgr: new config directive "auth_backend_selection"
* oxcical: escaped commas in values were misparsed, now fixed
  ("TZID:Amsterdam\, Berlin\, etc.")
