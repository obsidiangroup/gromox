#pragma once
#include <cstdint>
#include <cstdlib>
#include <gromox/defs.h>
#include <gromox/common_types.hpp>
#include "store_object.h"
#include "message_object.h"
#define SOCKET_TIMEOUT								60

/* defined by zarafa */
#define MAPI_STORE_PROVIDER							33
#define MAPI_AB										34
#define MAPI_AB_PROVIDER							35
#define MAPI_TRANSPORT_PROVIDER						36
#define MAPI_SPOOLER								37
#define MAPI_PROFILE_PROVIDER						38
#define MAPI_SUBSYSTEM								39
#define MAPI_HOOK_PROVIDER							40

#define STATUS_DEFAULT_OUTBOUND						0x00000001
#define STATUS_DEFAULT_STORE						0x00000002
#define STATUS_PRIMARY_IDENTITY						0x00000004
#define STATUS_SIMPLE_STORE							0x00000008
#define STATUS_XP_PREFER_LAST						0x00000010
#define STATUS_NO_PRIMARY_IDENTITY					0x00000020
#define STATUS_NO_DEFAULT_STORE						0x00000040
#define STATUS_TEMP_SECTION							0x00000080
#define STATUS_OWN_STORE							0x00000100
#define HOOK_INBOUND								0x00000200
#define HOOK_OUTBOUND								0x00000400
#define STATUS_NEED_IPM_TREE						0x00000800
#define STATUS_PRIMARY_STORE						0x00001000
#define STATUS_SECONDARY_STORE						0x00002000

#define SYNC_NEW_MESSAGE							0x800
#define SYNC_SOFT_DELETE							0x01

#define FLAG_SOFT_DELETE            				0x00000002
#define FLAG_ASSOCIATED								0x00000040
#define FLAG_HARD_DELETE							0x00000010
#define FLAG_MOVE		            				0x00000001
#define FLAG_CLEAR_READ								0x00000004
#define FLAG_UNICODE            					0x80000000
#define FLAG_OPEN_IF_EXISTS							0x00000001
#define FLAG_DEL_ASSOCIATED							0x00000008
#define FLAG_COPY_SUBFOLDERS						0x00000010
#define FLAG_CREATE									0x00000002
#define FLAG_CONVENIENT_DEPTH						0x00000001

#define EC_SUPPORTMASK_OWNER	\
	(STORE_ENTRYID_UNIQUE | STORE_SEARCH_OK | STORE_MODIFY_OK | \
	STORE_CREATE_OK | STORE_ATTACH_OK | STORE_OLE_OK | \
	STORE_NOTIFY_OK | STORE_MV_PROPS_OK | STORE_CATEGORIZE_OK | \
	STORE_RTF_OK | STORE_RESTRICTION_OK | STORE_SORT_OK | \
	STORE_UNCOMPRESSED_RTF | \
	STORE_HTML_OK | STORE_UNICODE_OK | STORE_LOCALSTORE | \
	STORE_SUBMIT_OK)
													
#define EC_SUPPORTMASK_OTHER \
	(STORE_ENTRYID_UNIQUE | STORE_SEARCH_OK | STORE_MODIFY_OK | \
	STORE_CREATE_OK | STORE_ATTACH_OK | STORE_OLE_OK | \
	STORE_NOTIFY_OK | STORE_MV_PROPS_OK | STORE_CATEGORIZE_OK | \
	STORE_RTF_OK | STORE_RESTRICTION_OK | STORE_SORT_OK | \
	STORE_UNCOMPRESSED_RTF | STORE_HTML_OK | STORE_UNICODE_OK)

#define EC_SUPPORTMASK_PUBLIC \
	(STORE_ENTRYID_UNIQUE | STORE_SEARCH_OK | STORE_MODIFY_OK | \
	STORE_CREATE_OK | STORE_ATTACH_OK | STORE_OLE_OK | \
	STORE_NOTIFY_OK | STORE_MV_PROPS_OK | STORE_CATEGORIZE_OK | \
	STORE_RTF_OK | STORE_RESTRICTION_OK | STORE_SORT_OK | \
	STORE_UNCOMPRESSED_RTF | \
	STORE_HTML_OK | STORE_UNICODE_OK | STORE_PUBLIC_FOLDERS)

/* end of zarafa defs */

#define NOTIFY_RECEIPT_READ							1
#define NOTIFY_RECEIPT_NON_READ						2

#define LOC_TYPE_PRIVATE_FOLDER						1
#define LOC_TYPE_PUBLIC_FOLDER						2
#define LOC_TYPE_PRIVATE_MESSAGE					3
#define LOC_TYPE_PUBLIC_MESSAGE						4

enum {
	COMMON_UTIL_MAX_RCPT,
	COMMON_UTIL_MAX_MESSAGE,
	COMMON_UTIL_MAX_MAIL_LENGTH,
	COMMON_UTIL_MAX_EXTRULE_LENGTH
};

enum {
	RES_ID_IPM,
	RES_ID_INBOX,
	RES_ID_DRAFT,
	RES_ID_OUTBOX,
	RES_ID_SENT,
	RES_ID_DELETED,
	RES_ID_CONTACTS,
	RES_ID_CALENDAR,
	RES_ID_JOURNAL,
	RES_ID_NOTES,
	RES_ID_TASKS,
	RES_ID_JUNK,
	RES_ID_SYNC,
	RES_ID_CONFLICT,
	RES_ID_LOCAL,
	RES_ID_SERVER,
	RES_TOTAL_NUM
};

extern void common_util_init(const char *org_name, const char *hostname, const char *default_charset, const char *default_zone, int mime_num, int max_rcpt, int max_msg, unsigned int max_mail_len, unsigned int max_rule_len, const char *smtp_ip, uint16_t smtp_port, const char *freebusy_path, const char *submit_cmd);
extern int common_util_run(const char *data_path);
extern void common_util_stop();
extern void common_util_free();
unsigned int common_util_get_param(int param);
extern const char *common_util_get_hostname();
extern const char *common_util_get_freebusy_path();
BOOL common_util_verify_columns_and_sorts(
	const PROPTAG_ARRAY *pcolumns,
	const SORTORDER_SET *psort_criteria);
BOOL common_util_check_message_class(const char *str_class);
extern BOOL common_util_check_delegate(MESSAGE_OBJECT *, char *username, size_t);
BOOL common_util_check_delegate_permission(
	const char *account, const char *maildir);
BOOL common_util_check_delegate_permission_ex(
	const char *account, const char *account_representing);
extern gxerr_t common_util_rectify_message(MESSAGE_OBJECT *, const char *representing_username);
void common_util_set_propvals(TPROPVAL_ARRAY *parray,
	const TAGGED_PROPVAL *ppropval);
void common_util_remove_propvals(
	TPROPVAL_ARRAY *parray, uint32_t proptag);
void* common_util_get_propvals(
	const TPROPVAL_ARRAY *parray, uint32_t proptag);
int common_util_index_proptags(
	const PROPTAG_ARRAY *pproptags, uint32_t proptag);
void common_util_reduce_proptags(PROPTAG_ARRAY *pproptags_minuend,
	const PROPTAG_ARRAY *pproptags_subtractor);
extern BOOL common_util_essdn_to_username(const char *pessdn, char *username, size_t);
BOOL common_util_essdn_to_uid(const char *pessdn, int *puid);
BOOL common_util_essdn_to_ids(const char *pessdn,
	int *pdomain_id, int *puser_id);
extern BOOL common_util_entryid_to_username(const BINARY *, char *username, size_t);
BINARY* common_util_username_to_addressbook_entryid(
	const char *username);
BOOL common_util_essdn_to_entryid(const char *essdn, BINARY *pbin);
BOOL common_util_username_to_essdn(const char *username, char *pessdn, size_t);
BOOL common_util_public_to_essdn(const char *username, char *pessdn, size_t);
void common_util_exmdb_locinfo_to_string(
	uint8_t type, int db_id, uint64_t eid,
	char *loc_string);
BOOL common_util_exmdb_locinfo_from_string(
	const char *loc_string, uint8_t *ptype,
	int *pdb_id, uint64_t *peid);
extern BOOL common_util_build_environment();
extern void common_util_free_environment();
void* common_util_alloc(size_t size);
template<typename T> T *cu_alloc() { return static_cast<T *>(common_util_alloc(sizeof(T))); }
template<typename T> T *cu_alloc(size_t elem) { return static_cast<T *>(common_util_alloc(sizeof(T) * elem)); }
template<typename T> T *me_alloc() { return static_cast<T *>(malloc(sizeof(T))); }
template<typename T> T *me_alloc(size_t elem) { return static_cast<T *>(malloc(sizeof(T) * elem)); }
void common_util_set_clifd(int clifd);
extern int common_util_get_clifd();
char* common_util_dup(const char *pstr);
ZNOTIFICATION* common_util_dup_znotification(
	ZNOTIFICATION *pnotification, BOOL b_temp);
void common_util_free_znotification(ZNOTIFICATION *pnotification);
extern BOOL common_util_addressbook_entryid_to_username(BINARY eid, char *username, size_t);
extern BOOL common_util_parse_addressbook_entryid(BINARY, uint32_t *type, char *essdn, size_t);
uint16_t common_util_get_messaging_entryid_type(BINARY bin);
BOOL common_util_from_folder_entryid(BINARY bin,
	BOOL *pb_private, int *pdb_id, uint64_t *pfolder_id);
BOOL common_util_from_message_entryid(BINARY bin, BOOL *pb_private,
	int *pdb_id, uint64_t *pfolder_id, uint64_t *pmessage_id);
BINARY* common_util_to_store_entryid(STORE_OBJECT *pstore);
BINARY* common_util_to_folder_entryid(
	STORE_OBJECT *pstore, uint64_t folder_id);
BINARY* common_util_calculate_folder_sourcekey(
	STORE_OBJECT *pstore, uint64_t folder_id);
BINARY* common_util_to_message_entryid(STORE_OBJECT *pstore,
	uint64_t folder_id, uint64_t message_id);
BINARY* common_util_calculate_message_sourcekey(
	STORE_OBJECT *pstore, uint64_t message_id);
BINARY* common_util_xid_to_binary(uint8_t size, const XID *pxid);
BOOL common_util_binary_to_xid(const BINARY *pbin, XID *pxid);
BINARY* common_util_guid_to_binary(GUID guid);
BINARY* common_util_pcl_append(const BINARY *pbin_pcl,
	const BINARY *pchange_key);
void common_util_notify_receipt(const char *username,
	int type, MESSAGE_CONTENT *pbrief);
BOOL common_util_convert_from_zrule(TPROPVAL_ARRAY *ppropvals);
BOOL common_util_load_file(const char *path, BINARY *pbin);
BOOL common_util_convert_to_zrule_data(STORE_OBJECT *, TPROPVAL_ARRAY *);
extern gxerr_t common_util_remote_copy_message(STORE_OBJECT *s0, uint64_t message_id, STORE_OBJECT *s1, uint64_t folder_id1);
extern gxerr_t common_util_remote_copy_folder(STORE_OBJECT *s0, uint64_t folder_id, STORE_OBJECT *s1, uint64_t folder_id1, const char *new_name);
extern const uint8_t *common_util_get_muidecsab();
extern const uint8_t *common_util_get_muidzcsab();
uint64_t common_util_convert_notification_folder_id(uint64_t folder_id);
BOOL common_util_send_message(STORE_OBJECT *pstore,
	uint64_t message_id, BOOL b_submit);
BOOL common_util_message_to_rfc822(STORE_OBJECT *pstore,
	uint64_t message_id, BINARY *peml_bin);
MESSAGE_CONTENT* common_util_rfc822_to_message(
	STORE_OBJECT *pstore, const BINARY *peml_bin);
BOOL common_util_message_to_ical(STORE_OBJECT *pstore,
	uint64_t message_id, BINARY *pical_bin);
MESSAGE_CONTENT* common_util_ical_to_message(
	STORE_OBJECT *pstore, const BINARY *pical_bin);
extern BOOL common_util_message_to_vcf(MESSAGE_OBJECT *, BINARY *vcfout);
MESSAGE_CONTENT* common_util_vcf_to_message(
	STORE_OBJECT *pstore, const BINARY *pvcf_bin);
const char* common_util_i18n_to_lang(const char *i18n);
extern const char *common_util_get_default_timezone();
extern const char *common_util_get_submit_command();
void common_util_get_folder_lang(const char *lang, char **ppfolder_lang);
extern const char *zcore_rpc_idtoname(unsigned int i);
