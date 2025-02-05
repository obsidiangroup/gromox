// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <cerrno>
#include <cstdint>
#include <string>
#include <unistd.h>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mapidefs.h>
#include <gromox/socket.h>
#include <gromox/pcl.hpp>
#include <gromox/ical.hpp>
#include <gromox/util.hpp>
#include <gromox/vcard.hpp>
#include <gromox/oxvcard.hpp>
#include <gromox/oxcical.hpp>
#include <gromox/oxcmail.hpp>
#include <gromox/propval.hpp>
#include <gromox/timezone.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/mime_pool.hpp>
#include <gromox/list_file.hpp>
#include <gromox/ext_buffer.hpp>
#include "common_util.h"
#include "exmdb_client.h"
#include "message_object.h"
#include <gromox/proptag_array.hpp>
#include "zarafa_server.h"
#include <gromox/alloc_context.hpp>
#include "bounce_producer.h"
#include "system_services.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <fcntl.h>
#include <cstdio>

using namespace std::string_literals;
using namespace gromox;

enum {
	SMTP_SEND_OK = 0,
	SMTP_CANNOT_CONNECT,
	SMTP_CONNECT_ERROR,
	SMTP_TIME_OUT,
	SMTP_TEMP_ERROR,
	SMTP_UNKOWN_RESPONSE,
	SMTP_PERMANENT_ERROR
};

namespace {

struct ENVIRONMENT_CONTEXT {
	ALLOC_CONTEXT allocator;
	int clifd;
};

struct LANGMAP_ITEM {
	char lang[32];
	char i18n[32];
};

}

static int g_max_rcpt;
static int g_mime_num;
static uint16_t g_smtp_port;
static int g_max_message;
static char g_smtp_ip[40];
static char g_org_name[256];
static char g_hostname[256];
static MIME_POOL *g_mime_pool;
static pthread_key_t g_dir_key;
static pthread_key_t g_env_key;
static char g_default_zone[64];
static char g_freebusy_path[256];
static char g_default_charset[32];
static char g_submit_command[1024];
static unsigned int g_max_mail_len;
static unsigned int g_max_rule_len;
static std::unique_ptr<LIST_FILE> g_langmap_list, g_folderlang_list;

BOOL common_util_verify_columns_and_sorts(
	const PROPTAG_ARRAY *pcolumns,
	const SORTORDER_SET *psort_criteria)
{
	int i;
	uint32_t proptag;
	
	proptag = 0;
	for (i=0; i<psort_criteria->count; i++) {
		if (!(psort_criteria->psort[i].type & MV_INSTANCE))
			continue;
		if (!(psort_criteria->psort[i].type & MV_FLAG))
			return FALSE;
		proptag = PROP_TAG(psort_criteria->psort[i].type, psort_criteria->psort[i].propid);
		break;
	}
	for (i=0; i<pcolumns->count; i++) {
		if (pcolumns->pproptag[i] & MV_INSTANCE) {
			if (proptag != pcolumns->pproptag[i]) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

BOOL common_util_check_message_class(const char *str_class)
{
	int i;
	int len;
	
	len = strlen(str_class);
	if (len + 1 > 255) {
		return FALSE;
	}
	for (i=0; i<len; i++) {
		if (str_class[i] < 32 || str_class[i] > 126) {
			return FALSE;
		}
		if ('.' == str_class[i] && '.' == str_class[i + 1]) {
			return FALSE;
		}
	}
	if ('.' == str_class[0] || '.' == str_class[len - 1]) {
		return FALSE;
	}
	return TRUE;
}

BOOL common_util_check_delegate(MESSAGE_OBJECT *pmessage, char *username, size_t ulen)
{
	void *pvalue;
	uint32_t proptag_buff[4];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	tmp_proptags.count = 4;
	tmp_proptags.pproptag = proptag_buff;
	proptag_buff[0] = PROP_TAG_SENTREPRESENTINGADDRESSTYPE;
	proptag_buff[1] = PROP_TAG_SENTREPRESENTINGEMAILADDRESS;
	proptag_buff[2] = PROP_TAG_SENTREPRESENTINGSMTPADDRESS;
	proptag_buff[3] = PROP_TAG_SENTREPRESENTINGENTRYID;
	if (!pmessage->get_properties(&tmp_proptags, &tmp_propvals))
		return FALSE;	
	if (0 == tmp_propvals.count) {
		username[0] = '\0';
		return TRUE;
	}
	pvalue = common_util_get_propvals(&tmp_propvals,
				PROP_TAG_SENTREPRESENTINGADDRESSTYPE);
	if (NULL != pvalue) {
		if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
			pvalue = common_util_get_propvals(&tmp_propvals,
						PROP_TAG_SENTREPRESENTINGEMAILADDRESS);
			if (NULL != pvalue) {
				return common_util_essdn_to_username(static_cast<char *>(pvalue),
				       username, ulen);
			}
		} else if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
			pvalue = common_util_get_propvals(&tmp_propvals,
					PROP_TAG_SENTREPRESENTINGEMAILADDRESS);
			if (NULL != pvalue) {
				gx_strlcpy(username, static_cast<char *>(pvalue), ulen);
				return TRUE;
			}
		}
	}
	pvalue = common_util_get_propvals(&tmp_propvals,
				PROP_TAG_SENTREPRESENTINGSMTPADDRESS);
	if (NULL != pvalue) {
		gx_strlcpy(username, static_cast<char *>(pvalue), ulen);
		return TRUE;
	}
	pvalue = common_util_get_propvals(&tmp_propvals,
					PROP_TAG_SENTREPRESENTINGENTRYID);
	if (NULL != pvalue) {
		return common_util_entryid_to_username(static_cast<BINARY *>(pvalue),
		       username, ulen);
	}
	username[0] = '\0';
	return TRUE;
}

BOOL common_util_check_delegate_permission(
	const char *account, const char *maildir)
{
	struct srcitem { char user[324]; };
	char temp_path[256];
	
	sprintf(temp_path, "%s/config/delegates.txt", maildir);
	auto pfile = list_file_initd(temp_path, nullptr, "%s:324");
	if (NULL == pfile) {
		return FALSE;
	}
	auto item_num = pfile->get_size();
	auto pitem = static_cast<const srcitem *>(pfile->get_list());
	for (decltype(item_num) i = 0; i < item_num; ++i) {
		if (strcasecmp(pitem[i].user, account) == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

BOOL common_util_check_delegate_permission_ex(
	const char *account, const char *account_representing)
{
	char maildir[256];	
	
	if (0 == strcasecmp(account, account_representing)) {
		return TRUE;
	}
	if (FALSE == system_services_get_maildir(
		account_representing, maildir)) {
		return FALSE;
	}
	return common_util_check_delegate_permission(account, maildir);
}

gxerr_t common_util_rectify_message(MESSAGE_OBJECT *pmessage,
    const char *representing_username)
{
	BINARY *pentryid;
	uint64_t nt_time;
	uint8_t tmp_byte;
	int32_t tmp_level;
	BINARY search_bin;
	BINARY search_bin1;
	char essdn_buff[1024];
	char tmp_display[256];
	char essdn_buff1[1024];
	char tmp_display1[256];
	char search_buff[1024];
	char search_buff1[1024];
	TPROPVAL_ARRAY tmp_propvals;
	TAGGED_PROPVAL propval_buff[20];
	
	auto account = pmessage->pstore->get_account();
	tmp_propvals.count = 15;
	tmp_propvals.ppropval = propval_buff;
	propval_buff[0].proptag = PR_READ;
	propval_buff[0].pvalue = &tmp_byte;
	tmp_byte = 1;
	propval_buff[1].proptag = PROP_TAG_CLIENTSUBMITTIME;
	propval_buff[1].pvalue = &nt_time;
	nt_time = rop_util_current_nttime();
	propval_buff[2].proptag = PROP_TAG_MESSAGEDELIVERYTIME;
	propval_buff[2].pvalue = &nt_time;
	propval_buff[3].proptag = PROP_TAG_CONTENTFILTERSPAMCONFIDENCELEVEL;
	propval_buff[3].pvalue = &tmp_level;
	tmp_level = -1;
	propval_buff[4].proptag = PROP_TAG_SENDERSMTPADDRESS;
	propval_buff[4].pvalue = deconst(account);
	propval_buff[5].proptag = PROP_TAG_SENDERADDRESSTYPE;
	propval_buff[5].pvalue  = deconst("EX");
	if (!common_util_username_to_essdn(account, essdn_buff, GX_ARRAY_SIZE(essdn_buff)))
		return GXERR_CALL_FAILED;
	if (FALSE == system_services_get_user_displayname(
		account, tmp_display)) {
		return GXERR_CALL_FAILED;
	}
	pentryid = common_util_username_to_addressbook_entryid(account);
	if (NULL == pentryid) {
		return GXERR_CALL_FAILED;
	}
	search_bin.cb = gx_snprintf(search_buff, GX_ARRAY_SIZE(search_buff), "EX:%s", essdn_buff) + 1;
	search_bin.pv = search_buff;
	propval_buff[6].proptag = PROP_TAG_SENDEREMAILADDRESS;
	propval_buff[6].pvalue = essdn_buff;
	propval_buff[7].proptag = PROP_TAG_SENDERNAME;
	propval_buff[7].pvalue = tmp_display;
	propval_buff[8].proptag = PROP_TAG_SENDERENTRYID;
	propval_buff[8].pvalue = pentryid;
	propval_buff[9].proptag = PROP_TAG_SENDERSEARCHKEY;
	propval_buff[9].pvalue = &search_bin;
	if (0 != strcasecmp(account, representing_username)) {
		if (!common_util_username_to_essdn(representing_username,
		    essdn_buff1, GX_ARRAY_SIZE(essdn_buff1)))
			return GXERR_CALL_FAILED;
		if (FALSE == system_services_get_user_displayname(
			representing_username, tmp_display1)) {
			return GXERR_CALL_FAILED;
		}
		pentryid = common_util_username_to_addressbook_entryid(
										representing_username);
		if (NULL == pentryid) {
			return GXERR_CALL_FAILED;
		}
	} else {
		strcpy(essdn_buff1, essdn_buff);
		strcpy(tmp_display1, tmp_display);
	}
	search_bin1.cb = gx_snprintf(search_buff1, GX_ARRAY_SIZE(search_buff1), "EX:%s", essdn_buff1) + 1;
	search_bin1.pv = search_buff1;
	propval_buff[10].proptag = PROP_TAG_SENTREPRESENTINGSMTPADDRESS;
	propval_buff[10].pvalue = deconst(representing_username);
	propval_buff[11].proptag = PROP_TAG_SENTREPRESENTINGADDRESSTYPE;
	propval_buff[11].pvalue  = deconst("EX");
	propval_buff[12].proptag = PROP_TAG_SENTREPRESENTINGEMAILADDRESS;
	propval_buff[12].pvalue = essdn_buff1;
	propval_buff[13].proptag = PROP_TAG_SENTREPRESENTINGNAME;
	propval_buff[13].pvalue = tmp_display1;
	propval_buff[14].proptag = PROP_TAG_SENTREPRESENTINGENTRYID;
	propval_buff[14].pvalue = pentryid;
	propval_buff[15].proptag = PROP_TAG_SENTREPRESENTINGSEARCHKEY;
	propval_buff[15].pvalue = &search_bin1;
	if (!pmessage->set_properties(&tmp_propvals))
		return GXERR_CALL_FAILED;
	return pmessage->save();
}

void common_util_set_propvals(TPROPVAL_ARRAY *parray,
	const TAGGED_PROPVAL *ppropval)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (ppropval->proptag == parray->ppropval[i].proptag) {
			parray->ppropval[i].pvalue = ppropval->pvalue;
			return;
		}
	}
	parray->ppropval[parray->count] = *ppropval;
	parray->count ++;
}

void common_util_remove_propvals(
	TPROPVAL_ARRAY *parray, uint32_t proptag)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (proptag == parray->ppropval[i].proptag) {
			parray->count --;
			if (i < parray->count) {
				memmove(parray->ppropval + i, parray->ppropval + i + 1,
					(parray->count - i) * sizeof(TAGGED_PROPVAL));
			}
			return;
		}
	}
}

void* common_util_get_propvals(
	const TPROPVAL_ARRAY *parray, uint32_t proptag)
{
	int i;
	
	for (i=0; i<parray->count; i++) {
		if (proptag == parray->ppropval[i].proptag) {
			return (void*)parray->ppropval[i].pvalue;
		}
	}
	return NULL;
}

int common_util_index_proptags(
	const PROPTAG_ARRAY *pproptags, uint32_t proptag)
{
	int i;
	
	for (i=0; i<pproptags->count; i++) {
		if (proptag == pproptags->pproptag[i]) {
			return i;
		}
	}
	return -1;
}

void common_util_reduce_proptags(PROPTAG_ARRAY *pproptags_minuend,
	const PROPTAG_ARRAY *pproptags_subtractor)
{
	int i, j;
	
	for (j=0; j<pproptags_subtractor->count; j++) {
		for (i=0; i<pproptags_minuend->count; i++) {
			if (pproptags_subtractor->pproptag[j] ==
				pproptags_minuend->pproptag[i]) {
				pproptags_minuend->count --;
				if (i < pproptags_minuend->count) {
					memmove(pproptags_minuend->pproptag + i,
						pproptags_minuend->pproptag + i + 1,
						(pproptags_minuend->count - i) *
						sizeof(uint32_t));
				}
				break;
			}
		}
	}
}

BOOL common_util_essdn_to_username(const char *pessdn,
    char *username, size_t ulen)
{
	char *pat;
	int tmp_len;
	int user_id;
	const char *plocal;
	char tmp_essdn[1024];
	
	tmp_len = sprintf(tmp_essdn,
			"/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=",
			g_org_name);
	if (0 != strncasecmp(pessdn, tmp_essdn, tmp_len)) {
		return FALSE;
	}
	if ('-' != pessdn[tmp_len + 16]) {
		return FALSE;
	}
	plocal = pessdn + tmp_len + 17;
	user_id = decode_hex_int(pessdn + tmp_len + 8);
	if (!system_services_get_username_from_id(user_id, username, ulen))
		return FALSE;
	pat = strchr(username, '@');
	if (NULL == pat) {
		return FALSE;
	}
	return strncasecmp(username, plocal, pat - username) == 0 ? TRUE : false;
}

BOOL common_util_essdn_to_uid(const char *pessdn, int *puid)
{
	int tmp_len;
	char tmp_essdn[1024];
	
	tmp_len = sprintf(tmp_essdn,
			"/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=",
			g_org_name);
	if (0 != strncasecmp(pessdn, tmp_essdn, tmp_len)) {
		return FALSE;
	}
	if ('-' != pessdn[tmp_len + 16]) {
		return FALSE;
	}
	*puid = decode_hex_int(pessdn + tmp_len + 8);
	return TRUE;
}

BOOL common_util_essdn_to_ids(const char *pessdn,
	int *pdomain_id, int *puser_id)
{
	int tmp_len;
	char tmp_essdn[1024];
	
	tmp_len = sprintf(tmp_essdn,
			"/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=",
			g_org_name);
	if (0 != strncasecmp(pessdn, tmp_essdn, tmp_len)) {
		return FALSE;
	}
	if ('-' != pessdn[tmp_len + 16]) {
		return FALSE;
	}
	*pdomain_id = decode_hex_int(pessdn + tmp_len);
	*puser_id = decode_hex_int(pessdn + tmp_len + 8);
	return TRUE;	
}

BOOL common_util_username_to_essdn(const char *username, char *pessdn, size_t dnmax)
{
	int user_id;
	int domain_id;
	char *pdomain;
	char tmp_name[UADDR_SIZE];
	char hex_string[16];
	char hex_string2[16];
	
	gx_strlcpy(tmp_name, username, GX_ARRAY_SIZE(tmp_name));
	pdomain = strchr(tmp_name, '@');
	if (NULL == pdomain) {
		return FALSE;
	}
	*pdomain = '\0';
	pdomain ++;
	if (!system_services_get_user_ids(username, &user_id, &domain_id, nullptr))
		return FALSE;
	encode_hex_int(user_id, hex_string);
	encode_hex_int(domain_id, hex_string2);
	snprintf(pessdn, dnmax, "/o=%s/ou=Exchange Administrative Group "
			"(FYDIBOHF23SPDLT)/cn=Recipients/cn=%s%s-%s",
			g_org_name, hex_string2, hex_string, tmp_name);
	HX_strupper(pessdn);
	return TRUE;
}

BOOL common_util_public_to_essdn(const char *username, char *pessdn, size_t dnmax)
{
	//TODO
	return FALSE;
}

void common_util_exmdb_locinfo_to_string(
	uint8_t type, int db_id, uint64_t eid,
	char *loc_string)
{
	
	sprintf(loc_string, "%d:%d:%llx", (int)type,
	        db_id, static_cast<unsigned long long>(rop_util_get_gc_value(eid)));
}

BOOL common_util_exmdb_locinfo_from_string(
	const char *loc_string, uint8_t *ptype,
	int *pdb_id, uint64_t *peid)
{
	int tmp_len;
	uint64_t tmp_val;
	char tmp_buff[16];
	
	if (0 == strncmp(loc_string, "1:", 2)) {
		*ptype = LOC_TYPE_PRIVATE_FOLDER;
	} else if (0 == strncmp(loc_string, "2:", 2)) {
		*ptype = LOC_TYPE_PUBLIC_FOLDER;
	} else if (0 == strncmp(loc_string, "3:", 2)) {
		*ptype = LOC_TYPE_PRIVATE_MESSAGE;
	} else if (0 == strncmp(loc_string, "4:", 2)) {
		*ptype = LOC_TYPE_PUBLIC_MESSAGE;
	} else {
		return FALSE;
	}
	auto ptoken = strchr(loc_string + 2, ':');
	if (NULL == ptoken) {
		return FALSE;
	}
	tmp_len = ptoken - (loc_string + 2);
	if (tmp_len > 12) {
		return FALSE;
	}
	memcpy(tmp_buff, loc_string + 2, tmp_len);
	tmp_buff[tmp_len] = '\0';
	*pdb_id = atoi(tmp_buff);
	if (0 == *pdb_id) {
		return FALSE;
	}
	tmp_val = strtoll(ptoken + 1, NULL, 16);
	if (0 == tmp_val) {
		return FALSE;
	}
	*peid = rop_util_make_eid_ex(1, tmp_val);
	return TRUE;
}

void common_util_init(const char *org_name, const char *hostname,
	const char *default_charset, const char *default_zone, int mime_num,
	int max_rcpt, int max_message, unsigned int max_mail_len,
    unsigned int max_rule_len, const char *smtp_ip, uint16_t smtp_port,
	const char *freebusy_path, const char *submit_command)
{
	gx_strlcpy(g_org_name, org_name, GX_ARRAY_SIZE(g_org_name));
	gx_strlcpy(g_hostname, hostname, GX_ARRAY_SIZE(g_hostname));
	gx_strlcpy(g_default_charset, default_charset, GX_ARRAY_SIZE(g_default_charset));
	gx_strlcpy(g_default_zone, default_zone, GX_ARRAY_SIZE(g_default_zone));
	g_mime_num = mime_num;
	g_max_rcpt = max_rcpt;
	g_max_message = max_message;
	g_max_mail_len = max_mail_len;
	g_max_rule_len = max_rule_len;
	gx_strlcpy(g_smtp_ip, smtp_ip, GX_ARRAY_SIZE(g_smtp_ip));
	g_smtp_port = smtp_port;
	gx_strlcpy(g_freebusy_path, freebusy_path, GX_ARRAY_SIZE(g_freebusy_path));
	gx_strlcpy(g_submit_command, submit_command, GX_ARRAY_SIZE(g_submit_command));
	pthread_key_create(&g_dir_key, NULL);
	pthread_key_create(&g_env_key, NULL);
}

int common_util_run(const char *data_path)
{
	g_mime_pool = mime_pool_init(g_mime_num, 16, TRUE);
	if (NULL == g_mime_pool) {
		printf("[common_util]: Failed to init MIME pool\n");
		return -1;
	}
	if (FALSE == oxcmail_init_library(
		g_org_name, system_services_get_user_ids,
		system_services_get_username_from_id,
		system_services_ltag_to_lcid,
		system_services_lcid_to_ltag,
		system_services_charset_to_cpid,
		system_services_cpid_to_charset,
		system_services_mime_to_extension,
		system_services_extension_to_mime)) {
		printf("[common_util]: Failed to init oxcmail library\n");
		return -2;
	}
	g_langmap_list = list_file_initd("langmap.txt", data_path, "%s:32%s:32");
	if (g_langmap_list == nullptr || g_langmap_list->get_size() == 0) {
		printf("[common_util]: list_file_initd langmap.txt: %s\n", strerror(errno));
		return -3;
	}
	
	g_folderlang_list = list_file_initd("folder_lang.txt", data_path,
		"%s:64%s:64%s:64%s:64%s:64%s:64%s:64%s:64%s"
		":64%s:64%s:64%s:64%s:64%s:64%s:64%s:64%s:64");
	if (NULL == g_folderlang_list) {
		printf("[common_util]: list_file_initd folder_lang.txt: %s\n", strerror(errno));
		return -4;
	}
	return 0;
}

void common_util_stop()
{
	g_langmap_list.reset();
	g_folderlang_list.reset();
}

void common_util_free()
{
	pthread_key_delete(g_dir_key);
	pthread_key_delete(g_env_key);
}

unsigned int common_util_get_param(int param)
{
	switch (param) {
	case COMMON_UTIL_MAX_RCPT:
		return g_max_rcpt;
	case COMMON_UTIL_MAX_MESSAGE:
		return g_max_message;
	case COMMON_UTIL_MAX_MAIL_LENGTH:
		return g_max_mail_len;
	case COMMON_UTIL_MAX_EXTRULE_LENGTH:
		return g_max_rule_len;
	}
	return 0;
}

const char* common_util_get_hostname()
{
	return g_hostname;
}

const char* common_util_get_freebusy_path()
{
	return g_freebusy_path;
}

BOOL common_util_build_environment()
{
	auto pctx = me_alloc<ENVIRONMENT_CONTEXT>();
	if (NULL == pctx) {
		return FALSE;
	}
	alloc_context_init(&pctx->allocator);
	pctx->clifd = -1;
	pthread_setspecific(g_env_key, pctx);
	return TRUE;
}

void common_util_free_environment()
{
	auto pctx = static_cast<ENVIRONMENT_CONTEXT *>(pthread_getspecific(g_env_key));
	pthread_setspecific(g_env_key, NULL);
	if (pctx == nullptr)
		return;
	alloc_context_free(&pctx->allocator);
	free(pctx);
}

void* common_util_alloc(size_t size)
{
	auto pctx = static_cast<ENVIRONMENT_CONTEXT *>(pthread_getspecific(g_env_key));
	return alloc_context_alloc(&pctx->allocator, size);
}

void common_util_set_clifd(int clifd)
{
	auto pctx = static_cast<ENVIRONMENT_CONTEXT *>(pthread_getspecific(g_env_key));
	if (NULL != pctx) {
		pctx->clifd = clifd;
	}
}

int common_util_get_clifd()
{
	auto pctx = static_cast<ENVIRONMENT_CONTEXT *>(pthread_getspecific(g_env_key));
	if (NULL == pctx) {
		return -1;
	}
	return pctx->clifd;
}

char* common_util_dup(const char *pstr)
{
	int len;
	
	len = strlen(pstr) + 1;
	auto pstr1 = cu_alloc<char>(len);
	if (NULL == pstr1) {
		return NULL;
	}
	memcpy(pstr1, pstr, len);
	return pstr1;
}

static BINARY* common_util_dup_binary(const BINARY *pbin)
{
	auto pbin1 = cu_alloc<BINARY>();
	if (NULL == pbin1) {
		return NULL;
	}
	pbin1->cb = pbin->cb;
	if (0 == pbin->cb) {
		pbin1->pb = NULL;
		return pbin1;
	}
	pbin1->pv = common_util_alloc(pbin->cb);
	if (pbin1->pv == nullptr)
		return NULL;
	memcpy(pbin1->pv, pbin->pv, pbin->cb);
	return pbin1;
}

ZNOTIFICATION* common_util_dup_znotification(
	ZNOTIFICATION *pnotification, BOOL b_temp)
{
	OBJECT_ZNOTIFICATION *pobj_notify;
	OBJECT_ZNOTIFICATION *pobj_notify1;
	NEWMAIL_ZNOTIFICATION *pnew_notify;
	NEWMAIL_ZNOTIFICATION *pnew_notify1;
	ZNOTIFICATION *pnotification1 = !b_temp ? me_alloc<ZNOTIFICATION>() : cu_alloc<ZNOTIFICATION>();
	
	if (pnotification1 == nullptr)
		return NULL;
	pnotification1->event_type = pnotification->event_type;
	if (EVENT_TYPE_NEWMAIL == pnotification->event_type) {
		pnew_notify1 = (NEWMAIL_ZNOTIFICATION*)
			pnotification->pnotification_data;
		if (FALSE == b_temp) {
			pnew_notify = me_alloc<NEWMAIL_ZNOTIFICATION>();
			if (NULL == pnew_notify) {
				free(pnotification1);
				return NULL;
			}
		} else {
			pnew_notify = cu_alloc<NEWMAIL_ZNOTIFICATION>();
			if (NULL == pnew_notify) {
				return NULL;
			}
		}
		memset(pnew_notify, 0, sizeof(NEWMAIL_ZNOTIFICATION));
		pnotification1->pnotification_data = pnew_notify;
		pnew_notify->entryid.cb = pnew_notify1->entryid.cb;
		if (FALSE == b_temp) {
			pnew_notify->entryid.pv = malloc(pnew_notify->entryid.cb);
			if (pnew_notify->entryid.pv == nullptr) {
				pnew_notify->entryid.cb = 0;
				common_util_free_znotification(pnotification1);
				return NULL;
			}
		} else {
			pnew_notify->entryid.pv = common_util_alloc(pnew_notify->entryid.cb);
			if (pnew_notify->entryid.pv == nullptr) {
				pnew_notify->entryid.cb = 0;
				return NULL;
			}
		}
		memcpy(pnew_notify->entryid.pv, pnew_notify1->entryid.pv,
			pnew_notify->entryid.cb);
		pnew_notify->parentid.cb = pnew_notify1->parentid.cb;
		if (FALSE == b_temp) {
			pnew_notify->parentid.pv = malloc(pnew_notify->parentid.cb);
			if (pnew_notify->parentid.pv == nullptr) {
				pnew_notify->parentid.cb = 0;
				common_util_free_znotification(pnotification1);
				return NULL;
			}
		} else {
			pnew_notify->parentid.pv = common_util_alloc(pnew_notify->parentid.cb);
			if (pnew_notify->parentid.pv == nullptr) {
				pnew_notify->parentid.cb = 0;
				return NULL;
			}
		}
		memcpy(pnew_notify->parentid.pv, pnew_notify1->parentid.pv,
			pnew_notify->parentid.cb);
		pnew_notify->flags = pnew_notify1->flags;
		if (FALSE == b_temp) {
			pnew_notify->message_class = strdup(pnew_notify1->message_class);
			if (NULL == pnew_notify->message_class) {
				common_util_free_znotification(pnotification1);
				return NULL;
			}
		} else {
			pnew_notify->message_class = common_util_dup(
							pnew_notify1->message_class);
			if (NULL == pnew_notify->message_class) {
				return NULL;
			}
		}
		pnew_notify->message_flags = pnew_notify1->message_flags;
	} else {
		pobj_notify1 = (OBJECT_ZNOTIFICATION*)
			pnotification->pnotification_data;
		if (FALSE == b_temp) {
			pobj_notify = me_alloc<OBJECT_ZNOTIFICATION>();
			if (NULL == pobj_notify) {
				free(pnotification1);
				return NULL;
			}
		} else {
			pobj_notify = cu_alloc<OBJECT_ZNOTIFICATION>();
			if (NULL == pobj_notify) {
				return NULL;
			}
		}
		memset(pobj_notify, 0, sizeof(OBJECT_ZNOTIFICATION));
		pnotification1->pnotification_data = pobj_notify;
		pobj_notify->object_type = pobj_notify1->object_type;
		if (NULL != pobj_notify1->pentryid) {
			if (FALSE == b_temp) {
				pobj_notify->pentryid = static_cast<BINARY *>(propval_dup(PT_BINARY,
				                        pobj_notify1->pentryid));
				if (NULL == pobj_notify->pentryid) {
					common_util_free_znotification(pnotification1);
					return NULL;
				}
			} else {
				pobj_notify->pentryid = common_util_dup_binary(
										pobj_notify1->pentryid);
				if (NULL == pobj_notify->pentryid) {
					return NULL;
				}
			}
		}
		if (NULL != pobj_notify1->pparentid) {
			if (FALSE == b_temp) {
				pobj_notify->pparentid = static_cast<BINARY *>(propval_dup(PT_BINARY,
				                         pobj_notify1->pparentid));
				if (NULL == pobj_notify->pparentid) {
					common_util_free_znotification(pnotification1);
					return NULL;
				}
			} else {
				pobj_notify->pparentid = common_util_dup_binary(
										pobj_notify1->pparentid);
				if (NULL == pobj_notify->pparentid) {
					return NULL;
				}
			}
		}
		if (NULL != pobj_notify1->pold_entryid) {
			if (FALSE == b_temp) {
				pobj_notify->pold_entryid = static_cast<BINARY *>(propval_dup(PT_BINARY,
				                            pobj_notify1->pold_entryid));
				if (NULL == pobj_notify->pold_entryid) {
					common_util_free_znotification(pnotification1);
					return NULL;
				}
			} else {
				pobj_notify->pold_entryid = common_util_dup_binary(
										pobj_notify1->pold_entryid);
				if (NULL == pobj_notify->pold_entryid) {
					return NULL;
				}
			}
		}
		if (NULL != pobj_notify->pold_parentid) {
			if (FALSE == b_temp) {
				pobj_notify->pold_parentid = static_cast<BINARY *>(propval_dup(PT_BINARY,
				                             pobj_notify1->pold_parentid));
				if (NULL == pobj_notify->pold_parentid) {
					common_util_free_znotification(pnotification1);
					return NULL;
				}
			} else {
				pobj_notify->pold_parentid = common_util_dup_binary(
										pobj_notify1->pold_parentid);
				if (NULL == pobj_notify->pold_parentid) {
					return NULL;
				}
			}
		}
		if (NULL != pobj_notify->pproptags) {
			if (FALSE == b_temp) {
				pobj_notify1->pproptags = proptag_array_dup(
									pobj_notify->pproptags);
				if (NULL == pobj_notify1->pproptags) {
					common_util_free_znotification(pnotification1);
					return NULL;
				}
			} else {
				pobj_notify1->pproptags = cu_alloc<PROPTAG_ARRAY>();
				if (NULL == pobj_notify1->pproptags) {
					return NULL;
				}
				pobj_notify1->pproptags->count =
					pobj_notify->pproptags->count;
				pobj_notify1->pproptags->pproptag = cu_alloc<uint32_t>(pobj_notify->pproptags->count);
				if (NULL == pobj_notify1->pproptags->pproptag) {
					return NULL;
				}
				memcpy(pobj_notify1->pproptags->pproptag,
					pobj_notify->pproptags->pproptag, sizeof(
					uint32_t)*pobj_notify->pproptags->count);
			}
		}
	}
	return pnotification1;
}

void common_util_free_znotification(ZNOTIFICATION *pnotification)
{
	OBJECT_ZNOTIFICATION *pobj_notify;
	NEWMAIL_ZNOTIFICATION *pnew_notify;
	
	if (EVENT_TYPE_NEWMAIL == pnotification->event_type) {
		pnew_notify = (NEWMAIL_ZNOTIFICATION*)
			pnotification->pnotification_data;
		if (NULL != pnew_notify->entryid.pb) {
			free(pnew_notify->entryid.pb);
		}
		if (NULL != pnew_notify->parentid.pb) {
			free(pnew_notify->parentid.pb);
		}
		if (NULL != pnew_notify->message_class) {
			free(pnew_notify->message_class);
		}
		free(pnew_notify);
	} else {
		pobj_notify = (OBJECT_ZNOTIFICATION*)
			pnotification->pnotification_data;
		if (NULL != pobj_notify->pentryid) {
			rop_util_free_binary(pobj_notify->pentryid);
		}
		if (NULL != pobj_notify->pparentid) {
			rop_util_free_binary(pobj_notify->pparentid);
		}
		if (NULL != pobj_notify->pold_entryid) {
			rop_util_free_binary(pobj_notify->pold_entryid);
		}
		if (NULL != pobj_notify->pold_parentid) {
			rop_util_free_binary(pobj_notify->pold_parentid);
		}
		if (NULL != pobj_notify->pproptags) {
			proptag_array_free(pobj_notify->pproptags);
		}
	}
	free(pnotification);
}

BOOL common_util_addressbook_entryid_to_username(BINARY entryid_bin,
    char *username, size_t ulen)
{
	EXT_PULL ext_pull;
	ADDRESSBOOK_ENTRYID tmp_entryid;

	ext_pull.init(entryid_bin.pb, entryid_bin.cb, common_util_alloc, EXT_FLAG_UTF16);
	if (ext_pull.g_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return FALSE;
	return common_util_essdn_to_username(tmp_entryid.px500dn, username, ulen);
}

BOOL common_util_parse_addressbook_entryid(BINARY entryid_bin, uint32_t *ptype,
    char *pessdn, size_t dsize)
{
	EXT_PULL ext_pull;
	ADDRESSBOOK_ENTRYID tmp_entryid;

	ext_pull.init(entryid_bin.pb, entryid_bin.cb, common_util_alloc, EXT_FLAG_UTF16);
	if (ext_pull.g_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return FALSE;
	*ptype = tmp_entryid.type;
	gx_strlcpy(pessdn, tmp_entryid.px500dn, dsize);
	return TRUE;
}

static BOOL common_util_entryid_to_username_internal(const BINARY *pbin,
    EXT_BUFFER_ALLOC alloc, char *username, size_t ulen)
{
	uint32_t flags;
	EXT_PULL ext_pull;
	uint8_t tmp_uid[16];
	uint8_t provider_uid[16];
	ONEOFF_ENTRYID oneoff_entry;
	ADDRESSBOOK_ENTRYID ab_entryid;
	
	if (pbin->cb < 20) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, 20, alloc, 0);
	if (ext_pull.g_uint32(&flags) != EXT_ERR_SUCCESS || flags != 0 ||
	    ext_pull.g_bytes(provider_uid, arsizeof(provider_uid)) != EXT_ERR_SUCCESS)
		return FALSE;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK, tmp_uid);
	if (0 == memcmp(tmp_uid, provider_uid, 16)) {
		ext_pull.init(pbin->pb, pbin->cb, alloc, EXT_FLAG_UTF16);
		if (ext_pull.g_abk_eid(&ab_entryid) != EXT_ERR_SUCCESS)
			return FALSE;
		if (ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER != ab_entryid.type) {
			return FALSE;
		}
		return common_util_essdn_to_username(ab_entryid.px500dn,
		       username, ulen);
	}
	rop_util_get_provider_uid(PROVIDER_UID_ONE_OFF, tmp_uid);
	if (0 == memcmp(tmp_uid, provider_uid, 16)) {
		ext_pull.init(pbin->pb, pbin->cb, alloc, EXT_FLAG_UTF16);
		if (ext_pull.g_oneoff_eid(&oneoff_entry) != EXT_ERR_SUCCESS)
			return FALSE;
		if (0 != strcasecmp(oneoff_entry.paddress_type, "SMTP")) {
			return FALSE;
		}
		gx_strlcpy(username, oneoff_entry.pmail_address, ulen);
		return TRUE;
	}
	return FALSE;
}

BOOL common_util_entryid_to_username(const BINARY *pbin,
    char *username, size_t ulen)
{
	return common_util_entryid_to_username_internal(pbin,
	       common_util_alloc, username, ulen);
}

BINARY* common_util_username_to_addressbook_entryid(
	const char *username)
{
	char x500dn[1024];
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	
	if (!common_util_username_to_essdn(username, x500dn, GX_ARRAY_SIZE(x500dn)))
		return NULL;
	tmp_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
							tmp_entryid.provider_uid);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER;
	tmp_entryid.px500dn = x500dn;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(1280);
	if (pbin->pv == nullptr ||
	    !ext_push.init(pbin->pv, 1280, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BOOL common_util_essdn_to_entryid(const char *essdn, BINARY *pbin)
{
	EXT_PUSH ext_push;
	ADDRESSBOOK_ENTRYID tmp_entryid;
	
	pbin->pv = common_util_alloc(1280);
	if (pbin->pv == nullptr)
		return FALSE;
	tmp_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ADDRESS_BOOK,
							tmp_entryid.provider_uid);
	tmp_entryid.version = 1;
	tmp_entryid.type = ADDRESSBOOK_ENTRYID_TYPE_LOCAL_USER;
	tmp_entryid.px500dn = deconst(essdn);
	if (!ext_push.init(pbin->pv, 1280, EXT_FLAG_UTF16) ||
	    ext_push.p_abk_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return false;
	pbin->cb = ext_push.m_offset;
	return TRUE;
}

static BOOL common_util_username_to_entryid(const char *username,
    const char *pdisplay_name, BINARY *pbin, enum display_type *dtpp)
{
	int status;
	int user_id;
	int domain_id;
	char *pdomain;
	char x500dn[1024];
	EXT_PUSH ext_push;
	char tmp_name[UADDR_SIZE];
	char hex_string[16];
	char hex_string2[16];
	ONEOFF_ENTRYID oneoff_entry;
	auto dtypx = DT_MAILUSER;
	
	if (system_services_get_user_ids(username, &user_id, &domain_id, &dtypx)) {
		gx_strlcpy(tmp_name, username, GX_ARRAY_SIZE(tmp_name));
		pdomain = strchr(tmp_name, '@');
		if (NULL == pdomain) {
			return FALSE;
		}
		*pdomain = '\0';
		encode_hex_int(user_id, hex_string);
		encode_hex_int(domain_id, hex_string2);
		snprintf(x500dn, 1024, "/o=%s/ou=Exchange Administrative "
				"Group (FYDIBOHF23SPDLT)/cn=Recipients/cn=%s%s-%s",
				g_org_name, hex_string2, hex_string, tmp_name);
		HX_strupper(x500dn);
		if (FALSE == common_util_essdn_to_entryid(x500dn, pbin)) {
			return FALSE;
		}
		if (dtpp != nullptr)
			*dtpp = dtypx;
		return TRUE;
	}
	pbin->pv = common_util_alloc(1280);
	if (pbin->pv == nullptr)
		return FALSE;
	oneoff_entry.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_ONE_OFF,
						oneoff_entry.provider_uid);
	oneoff_entry.version = 0;
	oneoff_entry.ctrl_flags = CTRL_FLAG_NORICH | CTRL_FLAG_UNICODE;
	oneoff_entry.pdisplay_name = pdisplay_name != nullptr && *pdisplay_name != '\0' ?
	                             deconst(pdisplay_name) : deconst(username);
	oneoff_entry.paddress_type = deconst("SMTP");
	oneoff_entry.pmail_address = (char*)username;
	if (!ext_push.init(pbin->pv, 1280, EXT_FLAG_UTF16))
		return false;
	status = ext_push.p_oneoff_eid(&oneoff_entry);
	if (EXT_ERR_CHARCNV == status) {
		oneoff_entry.ctrl_flags = CTRL_FLAG_NORICH;
		status = ext_push.p_oneoff_eid(&oneoff_entry);
	}
	if (EXT_ERR_SUCCESS != status) {
		return FALSE;
	}
	pbin->cb = ext_push.m_offset;
	if (dtpp != nullptr)
		*dtpp = DT_MAILUSER;
	return TRUE;
}

uint16_t common_util_get_messaging_entryid_type(BINARY bin)
{
	uint32_t flags;
	EXT_PULL ext_pull;
	uint16_t folder_type;
	char provider_uid[16];
	
	ext_pull.init(bin.pb, bin.cb, common_util_alloc, 0);
	if (ext_pull.g_uint32(&flags) != EXT_ERR_SUCCESS ||
	    ext_pull.g_bytes(provider_uid, arsizeof(provider_uid)) != EXT_ERR_SUCCESS ||
	    ext_pull.g_uint16(&folder_type) != EXT_ERR_SUCCESS)
		return 0;
	return folder_type;
}

BOOL common_util_from_folder_entryid(BINARY bin,
	BOOL *pb_private, int *pdb_id, uint64_t *pfolder_id)
{
	BOOL b_found;
	uint16_t replid;
	EXT_PULL ext_pull;
	FOLDER_ENTRYID tmp_entryid;
	
	ext_pull.init(bin.pb, bin.cb, common_util_alloc, 0);
	if (ext_pull.g_folder_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return FALSE;	
	switch (tmp_entryid.folder_type) {
	case EITLT_PRIVATE_FOLDER:
		*pb_private = TRUE;
		*pdb_id = rop_util_make_user_id(
			tmp_entryid.database_guid);
		if (-1 == *pdb_id) {
			return FALSE;
		}
		*pfolder_id = rop_util_make_eid(1,
				tmp_entryid.global_counter);
		return TRUE;
	case EITLT_PUBLIC_FOLDER: {
		*pb_private = FALSE;
		*pdb_id = rop_util_make_domain_id(
				tmp_entryid.database_guid);
		if (*pdb_id > 0) {
			*pfolder_id = rop_util_make_eid(1,
					tmp_entryid.global_counter);
			return TRUE;
		}
		auto pinfo = zarafa_server_get_info();
		if (NULL == pinfo || *pdb_id != pinfo->domain_id) {
			return FALSE;
		}
		if (!exmdb_client::get_mapping_replid(pinfo->get_homedir(),
		    tmp_entryid.database_guid, &b_found, &replid) || !b_found)
			return FALSE;
		*pfolder_id = rop_util_make_eid(replid,
					tmp_entryid.global_counter);
		return TRUE;
	}
	default:
		return FALSE;
	}
}

BOOL common_util_from_message_entryid(BINARY bin, BOOL *pb_private,
	int *pdb_id, uint64_t *pfolder_id, uint64_t *pmessage_id)
{
	BOOL b_found;
	uint16_t replid;
	EXT_PULL ext_pull;
	MESSAGE_ENTRYID tmp_entryid;
	
	ext_pull.init(bin.pb, bin.cb, common_util_alloc, 0);
	if (ext_pull.g_msg_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return FALSE;	
	if (0 != memcmp(&tmp_entryid.folder_database_guid,
		&tmp_entryid.message_database_guid, sizeof(GUID))) {
		return FALSE;
	}
	switch (tmp_entryid.message_type) {
	case EITLT_PRIVATE_MESSAGE:
		*pb_private = TRUE;
		*pdb_id = rop_util_make_user_id(
			tmp_entryid.folder_database_guid);
		if (-1 == *pdb_id) {
			return FALSE;
		}
		*pfolder_id = rop_util_make_eid(1,
			tmp_entryid.folder_global_counter);
		*pmessage_id = rop_util_make_eid(1,
			tmp_entryid.message_global_counter);
		return TRUE;
	case EITLT_PUBLIC_MESSAGE: {
		*pb_private = FALSE;
		*pdb_id = rop_util_make_domain_id(
			tmp_entryid.folder_database_guid);
		if (*pdb_id > 0) {
			*pfolder_id = rop_util_make_eid(1,
				tmp_entryid.folder_global_counter);
			*pmessage_id = rop_util_make_eid(1,
				tmp_entryid.message_global_counter);
			return TRUE;
		}
		auto pinfo = zarafa_server_get_info();
		if (NULL == pinfo || *pdb_id != pinfo->domain_id) {
			return FALSE;
		}
		if (!exmdb_client::get_mapping_replid(pinfo->get_homedir(),
		    tmp_entryid.folder_database_guid, &b_found, &replid) ||
		    !b_found)
			return FALSE;
		*pfolder_id = rop_util_make_eid(replid,
			tmp_entryid.folder_global_counter);
		*pmessage_id = rop_util_make_eid(replid,
			tmp_entryid.message_global_counter);
		return TRUE;
	}
	default:
		return FALSE;
	}
}

BINARY* common_util_to_folder_entryid(
	STORE_OBJECT *pstore, uint64_t folder_id)
{
	BOOL b_found;
	BINARY tmp_bin;
	uint16_t replid;
	EXT_PUSH ext_push;
	FOLDER_ENTRYID tmp_entryid;
	
	tmp_entryid.flags = 0;
	if (pstore->b_private) {
		tmp_bin.cb = 0;
		tmp_bin.pb = tmp_entryid.provider_uid;
		rop_util_guid_to_binary(pstore->mailbox_guid, &tmp_bin);
		tmp_entryid.database_guid = rop_util_make_user_guid(pstore->account_id);
		tmp_entryid.folder_type = EITLT_PRIVATE_FOLDER;
	} else {
		rop_util_get_provider_uid(PROVIDER_UID_PUBLIC,
							tmp_entryid.provider_uid);
		replid = rop_util_get_replid(folder_id);
		if (1 != replid) {
			if (!exmdb_client::get_mapping_guid(pstore->get_dir(),
			    replid, &b_found, &tmp_entryid.database_guid))
				return NULL;	
			if (!b_found)
				return NULL;
		} else {
			tmp_entryid.database_guid = rop_util_make_domain_guid(pstore->account_id);
		}
		tmp_entryid.folder_type = EITLT_PUBLIC_FOLDER;
	}
	rop_util_get_gc_array(folder_id, tmp_entryid.global_counter);
	tmp_entryid.pad[0] = 0;
	tmp_entryid.pad[1] = 0;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(256);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 256, 0) ||
	    ext_push.p_folder_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BINARY* common_util_calculate_folder_sourcekey(
	STORE_OBJECT *pstore, uint64_t folder_id)
{
	BOOL b_found;
	uint16_t replid;
	EXT_PUSH ext_push;
	LONG_TERM_ID longid;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->cb = 22;
	pbin->pv = common_util_alloc(22);
	if (pbin->pv == nullptr)
		return NULL;
	if (pstore->b_private) {
		longid.guid = rop_util_make_user_guid(pstore->account_id);
	} else {
		replid = rop_util_get_replid(folder_id);
		if (1 == replid) {
			longid.guid = rop_util_make_domain_guid(pstore->account_id);
		} else {
			if (!exmdb_client::get_mapping_guid(pstore->get_dir(),
			    replid, &b_found, &longid.guid))
				return NULL;	
			if (!b_found)
				return NULL;
		}	
	}
	rop_util_get_gc_array(folder_id, longid.global_counter);
	if (!ext_push.init(pbin->pv, 22, 0) ||
	    ext_push.p_guid(&longid.guid) != EXT_ERR_SUCCESS ||
	    ext_push.p_bytes(longid.global_counter, 6) != EXT_ERR_SUCCESS)
		return NULL;
	return pbin;
}

BINARY* common_util_to_message_entryid(STORE_OBJECT *pstore,
	uint64_t folder_id, uint64_t message_id)
{
	BOOL b_found;
	BINARY tmp_bin;
	uint16_t replid;
	EXT_PUSH ext_push;
	MESSAGE_ENTRYID tmp_entryid;
	
	tmp_entryid.flags = 0;
	if (pstore->b_private) {
		tmp_bin.cb = 0;
		tmp_bin.pb = tmp_entryid.provider_uid;
		rop_util_guid_to_binary(pstore->mailbox_guid, &tmp_bin);
		tmp_entryid.folder_database_guid = rop_util_make_user_guid(pstore->account_id);
		tmp_entryid.message_type = EITLT_PRIVATE_MESSAGE;
	} else {
		rop_util_get_provider_uid(PROVIDER_UID_PUBLIC,
							tmp_entryid.provider_uid);
		replid = rop_util_get_replid(folder_id);
		if (1 != replid) {
			if (!exmdb_client::get_mapping_guid(pstore->get_dir(),
			    replid, &b_found, &tmp_entryid.folder_database_guid))
				return NULL;	
			if (!b_found)
				return NULL;
		} else {
			tmp_entryid.folder_database_guid = rop_util_make_domain_guid(pstore->account_id);
		}
		tmp_entryid.message_type = EITLT_PUBLIC_MESSAGE;
	}
	tmp_entryid.message_database_guid = tmp_entryid.folder_database_guid;
	rop_util_get_gc_array(folder_id, tmp_entryid.folder_global_counter);
	rop_util_get_gc_array(message_id, tmp_entryid.message_global_counter);
	tmp_entryid.pad1[0] = 0;
	tmp_entryid.pad1[1] = 0;
	tmp_entryid.pad2[0] = 0;
	tmp_entryid.pad2[1] = 0;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(256);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 256, 0) ||
	    ext_push.p_msg_eid(&tmp_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BINARY* common_util_calculate_message_sourcekey(
	STORE_OBJECT *pstore, uint64_t message_id)
{
	EXT_PUSH ext_push;
	LONG_TERM_ID longid;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->cb = 22;
	pbin->pv = common_util_alloc(22);
	if (pbin->pv == nullptr)
		return NULL;
	longid.guid = pstore->guid();
	rop_util_get_gc_array(message_id, longid.global_counter);
	if (!ext_push.init(pbin->pv, 22, 0) ||
	    ext_push.p_guid(&longid.guid) != EXT_ERR_SUCCESS ||
	    ext_push.p_bytes(longid.global_counter, 6) != EXT_ERR_SUCCESS)
		return NULL;
	return pbin;
}

BINARY* common_util_xid_to_binary(uint8_t size, const XID *pxid)
{
	EXT_PUSH ext_push;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(24);
	if (pbin->pv == nullptr || !ext_push.init(pbin->pv, 24, 0) ||
	    ext_push.p_xid(size, pxid) != EXT_ERR_SUCCESS)
		return NULL;
	pbin->cb = ext_push.m_offset;
	return pbin;
}

BOOL common_util_binary_to_xid(const BINARY *pbin, XID *pxid)
{
	EXT_PULL ext_pull;
	
	if (pbin->cb < 17 || pbin->cb > 24) {
		return FALSE;
	}
	ext_pull.init(pbin->pb, pbin->cb, common_util_alloc, 0);
	return ext_pull.g_xid(pbin->cb, pxid) == EXT_ERR_SUCCESS ? TRUE : false;
}

BINARY* common_util_guid_to_binary(GUID guid)
{
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->cb = 0;
	pbin->pv = common_util_alloc(16);
	if (pbin->pv == nullptr)
		return NULL;
	rop_util_guid_to_binary(guid, pbin);
	return pbin;
}

BINARY* common_util_pcl_append(const BINARY *pbin_pcl,
	const BINARY *pchange_key)
{
	PCL *ppcl;
	SIZED_XID xid;
	BINARY *ptmp_bin;
	
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	ppcl = pcl_init();
	if (NULL == ppcl) {
		return NULL;
	}
	if (NULL != pbin_pcl) {
		if (FALSE == pcl_deserialize(ppcl, pbin_pcl)) {
			pcl_free(ppcl);
			return NULL;
		}
	}
	xid.size = pchange_key->cb;
	if (FALSE == common_util_binary_to_xid(pchange_key, &xid.xid)) {
		pcl_free(ppcl);
		return NULL;
	}
	if (FALSE == pcl_append(ppcl, &xid)) {
		pcl_free(ppcl);
		return NULL;
	}
	ptmp_bin = pcl_serialize(ppcl);
	pcl_free(ppcl);
	if (NULL == ptmp_bin) {
		return NULL;
	}
	pbin->cb = ptmp_bin->cb;
	pbin->pv = common_util_alloc(ptmp_bin->cb);
	if (pbin->pv == nullptr) {
		rop_util_free_binary(ptmp_bin);
		return NULL;
	}
	memcpy(pbin->pv, ptmp_bin->pv, pbin->cb);
	rop_util_free_binary(ptmp_bin);
	return pbin;
}

BOOL common_util_load_file(const char *path, BINARY *pbin)
{
	struct stat node_state;
	
	auto fd = open(path, O_RDONLY);
	if (-1 == fd) {
		return FALSE;
	}
	if (fstat(fd, &node_state) != 0) {
		close(fd);
		return FALSE;
	}
	pbin->cb = node_state.st_size;
	pbin->pv = common_util_alloc(node_state.st_size);
	if (pbin->pv == nullptr ||
	    read(fd, pbin->pv, node_state.st_size) != node_state.st_size) {
		close(fd);
		return FALSE;
	}
	close(fd);
	return TRUE;
}

static BOOL common_util_send_command(int sockd,
	const char *command, int command_len)
{
	int write_len;

	write_len = write(sockd, command, command_len);
    if (write_len != command_len) {
		return FALSE;
	}
	return TRUE;
}

static int common_util_get_response(int sockd,
	char *response, int response_len, BOOL expect_3xx)
{
	int read_len;

	memset(response, 0, response_len);
	read_len = read(sockd, response, response_len);
	if (-1 == read_len || 0 == read_len) {
		return SMTP_TIME_OUT;
	}
	if ('\n' == response[read_len - 1] && '\r' == response[read_len - 2]){
		/* remove /r/n at the end of response */
		read_len -= 2;
	}
	response[read_len] = '\0';
	if (FALSE == expect_3xx && '2' == response[0] &&
	    HX_isdigit(response[1]) && HX_isdigit(response[2])) {
		return SMTP_SEND_OK;
	} else if(TRUE == expect_3xx && '3' == response[0] &&
	    HX_isdigit(response[1]) && HX_isdigit(response[2])) {
		return SMTP_SEND_OK;
	} else {
		if ('4' == response[0]) {
           	return SMTP_TEMP_ERROR;	
		} else if ('5' == response[0]) {
			return SMTP_PERMANENT_ERROR;
		} else {
			return SMTP_UNKOWN_RESPONSE;
		}
	}
}

static void log_err(const char *format, ...)
{
	va_list ap;
	char log_buf[2048];
	
	auto pinfo = zarafa_server_get_info();
	if (NULL == pinfo) {
		return;
	}
	va_start(ap, format);
	vsnprintf(log_buf, sizeof(log_buf) - 1, format, ap);
	va_end(ap);
	log_buf[sizeof(log_buf) - 1] = '\0';
	system_services_log_info(LV_ERR, "user=%s  %s", pinfo->get_username(), log_buf);
}

static BOOL common_util_send_mail(MAIL *pmail,
	const char *sender, DOUBLE_LIST *prcpt_list)
{
	int res_val;
	int command_len;
	DOUBLE_LIST_NODE *pnode;
	char last_command[1024];
	char last_response[1024];
	
	int sockd = gx_inet_connect(g_smtp_ip, g_smtp_port, 0);
	if (sockd < 0) {
		log_err("Cannot connect to SMTP server [%s]:%hu: %s",
			g_smtp_ip, g_smtp_port, strerror(-sockd));
		return FALSE;
	}
	/* read welcome information of MTA */
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Timeout with SMTP server [%s]:%hu",
			g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
        /* send quit command to server */
        common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("Failed to connect to SMTP. "
			"Server response is \"%s\".", last_response);
		return FALSE;
	}

	/* send helo xxx to server */
	snprintf(last_command, 1024, "helo %s\r\n", g_hostname);
	command_len = strlen(last_command);
	if (FALSE == common_util_send_command(
		sockd, last_command, command_len)) {
		close(sockd);
		log_err("Failed to send \"HELO\" command");
		return FALSE;
	}
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Timeout with SMTP server [%s]:%hu", g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		/* send quit command to server */
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("SMTP server responded \"%s\" "
			"after sending \"HELO\" command", last_response);
		return FALSE;
	}

	command_len = sprintf(last_command, "mail from:<%s>\r\n", sender);
	
	if (FALSE == common_util_send_command(
		sockd, last_command, command_len)) {
		close(sockd);
		log_err("Failed to send \"MAIL FROM\" command");
		return FALSE;
	}
	/* read mail from response information */
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Timeout with SMTP server [%s]:%hu", g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
		case SMTP_TEMP_ERROR:
		case SMTP_UNKOWN_RESPONSE:
		/* send quit command to server */
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("SMTP server responded \"%s\" "
			"after sending \"MAIL FROM\" command", last_response);
		return FALSE;
	}

	for (pnode=double_list_get_head(prcpt_list); NULL!=pnode;
		pnode=double_list_get_after(prcpt_list, pnode)) {
		if (strchr(static_cast<char *>(pnode->pdata), '@') == nullptr) {
			command_len = sprintf(last_command,
				"rcpt to:<%s@none>\r\n",
			              static_cast<const char *>(pnode->pdata));
		} else {
			command_len = sprintf(last_command,
				"rcpt to:<%s>\r\n",
			              static_cast<const char *>(pnode->pdata));
		}
		if (FALSE == common_util_send_command(
			sockd, last_command, command_len)) {
			close(sockd);
			log_err("Failed to send \"RCPT TO\" command");
			return FALSE;
		}
		/* read rcpt to response information */
		res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
		switch (res_val) {
		case SMTP_TIME_OUT:
			close(sockd);
			log_err("Timeout with SMTP server [%s]:%hu", g_smtp_ip, g_smtp_port);
			return FALSE;
		case SMTP_PERMANENT_ERROR:
		case SMTP_TEMP_ERROR:
		case SMTP_UNKOWN_RESPONSE:
			common_util_send_command(sockd, "quit\r\n", 6);
			close(sockd);
			log_err("SMTP server responded \"%s\" "
				"after sending \"RCPT TO\" command", last_response);
			return FALSE;
		}						
	}
	/* send data */
	strcpy(last_command, "data\r\n");
	command_len = strlen(last_command);
	if (FALSE == common_util_send_command(
		sockd, last_command, command_len)) {
		close(sockd);
		log_err("Sender %s: failed to send \"DATA\" command", sender);
		return FALSE;
	}

	/* read data response information */
	res_val = common_util_get_response(sockd, last_response, 1024, TRUE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Sender %s: Timeout with SMTP server [%s]:%hu",
			sender, g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("Sender %s: SMTP server responded \"%s\" "
				"after sending \"data\" command", sender, last_response);
		return FALSE;
	}

	mail_set_header(pmail, "X-Mailer", "gromox-zcore " PACKAGE_VERSION);
	if (FALSE == mail_to_file(pmail, sockd) ||
		FALSE == common_util_send_command(sockd, ".\r\n", 3)) {
		close(sockd);
		log_err("Sender %s: Failed to send mail content", sender);
		return FALSE;
	}
	res_val = common_util_get_response(sockd, last_response, 1024, FALSE);
	switch (res_val) {
	case SMTP_TIME_OUT:
		close(sockd);
		log_err("Sender %s: Timeout with SMTP server [%s]:%hu", sender, g_smtp_ip, g_smtp_port);
		return FALSE;
	case SMTP_PERMANENT_ERROR:
	case SMTP_TEMP_ERROR:
	case SMTP_UNKOWN_RESPONSE:	
        common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("Sender %s: SMTP server responded \"%s\" "
					"after sending mail content", sender, last_response);
		return FALSE;
	case SMTP_SEND_OK:
		common_util_send_command(sockd, "quit\r\n", 6);
		close(sockd);
		log_err("SMTP server [%s]:%hu has received message from %s",
			g_smtp_ip, g_smtp_port, sender);
		return TRUE;
	}
	return false;
}

static void common_util_set_dir(const char *dir)
{
	pthread_setspecific(g_dir_key, dir);
}

static const char* common_util_get_dir()
{
	return static_cast<char *>(pthread_getspecific(g_dir_key));
}

static BOOL common_util_get_propids(
	const PROPNAME_ARRAY *ppropnames,
	PROPID_ARRAY *ppropids)
{
	return exmdb_client::get_named_propids(
			common_util_get_dir(), FALSE,
			ppropnames, ppropids);
}

static BOOL common_util_get_propids_create(const PROPNAME_ARRAY *names,
    PROPID_ARRAY *ids)
{
	return exmdb_client::get_named_propids(common_util_get_dir(),
	       TRUE, names, ids);
}

static BOOL common_util_get_propname(
	uint16_t propid, PROPERTY_NAME **pppropname)
{
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	
	propids.count = 1;
	propids.ppropid = &propid;
	if (!exmdb_client::get_named_propnames(
		common_util_get_dir(), &propids, &propnames)) {
		return FALSE;
	}
	*pppropname = propnames.count != 1 ? nullptr : propnames.ppropname;
	return TRUE;
}

BOOL common_util_send_message(STORE_OBJECT *pstore,
	uint64_t message_id, BOOL b_submit)
{
	MAIL imail;
	void *pvalue;
	BOOL b_result;
	BOOL b_delete;
	int body_type;
	EID_ARRAY ids;
	BOOL b_private;
	BOOL b_partial;
	int accound_id;
	uint64_t new_id;
	uint64_t parent_id;
	uint64_t folder_id;
	TARRAY_SET *prcpts;
	DOUBLE_LIST temp_list;
	uint32_t message_flags;
	TAGGED_PROPVAL *ppropval;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pinfo = zarafa_server_get_info();
	uint32_t cpid = pinfo == nullptr ? 1252 : pinfo->cpid;
	if (!exmdb_client_get_message_property(pstore->get_dir(), nullptr, 0,
	    message_id, PROP_TAG_PARENTFOLDERID, &pvalue) || pvalue == nullptr)
		return FALSE;
	parent_id = *(uint64_t*)pvalue;
	if (!exmdb_client::read_message(pstore->get_dir(), nullptr, cpid,
	    message_id, &pmsgctnt) || pmsgctnt == nullptr)
		return FALSE;
	if (common_util_get_propvals(&pmsgctnt->proplist, PR_INTERNET_CPID) == nullptr) {
		ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt->proplist.count + 1);
		if (NULL == ppropval) {
			return FALSE;
		}
		memcpy(ppropval, pmsgctnt->proplist.ppropval,
			sizeof(TAGGED_PROPVAL)*pmsgctnt->proplist.count);
		ppropval[pmsgctnt->proplist.count].proptag = PR_INTERNET_CPID;
		ppropval[pmsgctnt->proplist.count].pvalue = &cpid;
		pmsgctnt->proplist.ppropval = ppropval;
		pmsgctnt->proplist.count ++;
	}
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_MESSAGE_FLAGS);
	if (NULL == pvalue) {
		return FALSE;
	}
	message_flags = *(uint32_t*)pvalue;
	BOOL b_resend = (message_flags & MSGFLAG_RESEND) ? TRUE : false;
	prcpts = pmsgctnt->children.prcpts;
	if (NULL == prcpts) {
		return FALSE;
	}
	double_list_init(&temp_list);
	if (prcpts->count == 0)
		fprintf(stderr, "I-1504: Store %s attempted to send message %llxh to 0 recipients\n",
		        pstore->get_account(), static_cast<unsigned long long>(message_id));
	for (size_t i = 0; i < prcpts->count; ++i) {
		auto pnode = cu_alloc<DOUBLE_LIST_NODE>();
		if (NULL == pnode) {
			return FALSE;
		}
		if (TRUE == b_resend) {
			pvalue = common_util_get_propvals(
				prcpts->pparray[i], PROP_TAG_RECIPIENTTYPE);
			if (NULL == pvalue) {
				return FALSE;
			}
			if (!(*static_cast<uint32_t *>(pvalue) & RECIPIENT_TYPE_NEED_RESEND))
				continue;	
		}
		/*
		if (FALSE == b_submit) {
			pvalue = common_util_get_propvals(
				prcpts->pparray[i], PROP_TAG_RESPONSIBILITY);
			if (NULL == pvalue || 0 != *(uint8_t*)pvalue) {
				continue;
			}
		}
		*/
		pnode->pdata = common_util_get_propvals(prcpts->pparray[i], PR_SMTP_ADDRESS);
		if (NULL != pnode->pdata && '\0' != ((char*)pnode->pdata)[0]) {
			double_list_append_as_tail(&temp_list, pnode);
			continue;
		}
		pvalue = common_util_get_propvals(
			prcpts->pparray[i], PROP_TAG_ADDRESSTYPE);
		if (NULL == pvalue) {
 CONVERT_ENTRYID:
			pvalue = common_util_get_propvals(prcpts->pparray[i], PR_ENTRYID);
			if (NULL == pvalue) {
				return FALSE;
			}
			pnode->pdata = common_util_alloc(UADDR_SIZE);
			if (NULL == pnode->pdata) {
				return FALSE;
			}
			if (!common_util_entryid_to_username(static_cast<BINARY *>(pvalue),
			    static_cast<char *>(pnode->pdata), UADDR_SIZE))
				return FALSE;	
		} else {
			if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
				pnode->pdata = common_util_get_propvals(prcpts->pparray[i], PR_EMAIL_ADDRESS);
				if (NULL == pnode->pdata) {
					return FALSE;
				}
			} else if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
				pvalue = common_util_get_propvals(prcpts->pparray[i], PR_EMAIL_ADDRESS);
				if (NULL == pvalue) {
					goto CONVERT_ENTRYID;
				}
				pnode->pdata = common_util_alloc(UADDR_SIZE);
				if (NULL == pnode->pdata) {
					return FALSE;
				}
				if (!common_util_essdn_to_username(static_cast<char *>(pvalue),
				    static_cast<char *>(pnode->pdata), UADDR_SIZE))
					goto CONVERT_ENTRYID;
			} else {
				goto CONVERT_ENTRYID;
			}
		}
		double_list_append_as_tail(&temp_list, pnode);
	}
	if (double_list_get_nodes_num(&temp_list) > 0) {
		pvalue = common_util_get_propvals(&pmsgctnt->proplist,
						PROP_TAG_INTERNETMAILOVERRIDEFORMAT);
		if (NULL == pvalue) {
			body_type = OXCMAIL_BODY_PLAIN_AND_HTML;
		} else {
			if (*(uint32_t*)pvalue & MESSAGE_FORMAT_PLAIN_AND_HTML) {
				body_type = OXCMAIL_BODY_PLAIN_AND_HTML;
			} else if (*(uint32_t*)pvalue & MESSAGE_FORMAT_HTML_ONLY) {
				body_type = OXCMAIL_BODY_HTML_ONLY;
			} else {
				body_type = OXCMAIL_BODY_PLAIN_ONLY;
			}
		}
		common_util_set_dir(pstore->get_dir());
		/* try to avoid TNEF message */
		if (FALSE == oxcmail_export(pmsgctnt, FALSE,
			body_type, g_mime_pool, &imail, common_util_alloc,
			common_util_get_propids, common_util_get_propname)) {
			return FALSE;	
		}
		if (!common_util_send_mail(&imail, pstore->get_account(), &temp_list)) {
			mail_free(&imail);
			return FALSE;
		}
		mail_free(&imail);
	}
	pvalue = common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_DELETEAFTERSUBMIT);
	b_delete = FALSE;
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		b_delete = TRUE;
	}
	common_util_remove_propvals(&pmsgctnt->proplist,
							PROP_TAG_SENTMAILSVREID);
	auto ptarget = static_cast<BINARY *>(common_util_get_propvals(
	               &pmsgctnt->proplist, PROP_TAG_TARGETENTRYID));
	if (NULL != ptarget) {
		if (FALSE == common_util_from_message_entryid(*ptarget,
			&b_private, &accound_id, &folder_id, &new_id)) {
			return FALSE;	
		}
		if (!exmdb_client::clear_submit(pstore->get_dir(), message_id, false))
			return FALSE;
		if (!exmdb_client::movecopy_message(pstore->get_dir(),
		    pstore->account_id, cpid, message_id, folder_id, new_id,
		    TRUE, &b_result))
			return FALSE;
	} else if (TRUE == b_delete) {
		exmdb_client_delete_message(pstore->get_dir(),
			pstore->account_id, cpid, parent_id, message_id,
			TRUE, &b_result);
	} else {
		if (!exmdb_client::clear_submit(pstore->get_dir(), message_id, false))
			return FALSE;
		ids.count = 1;
		ids.pids = &message_id;
		return exmdb_client::movecopy_messages(pstore->get_dir(),
		       pstore->account_id, cpid, false, nullptr, parent_id,
			rop_util_make_eid_ex(1, PRIVATE_FID_SENT_ITEMS),
			FALSE, &ids, &b_partial);
	}
	return TRUE;
}

void common_util_notify_receipt(const char *username,
	int type, MESSAGE_CONTENT *pbrief)
{
	MAIL imail;
	DOUBLE_LIST_NODE node;
	DOUBLE_LIST rcpt_list;
	
	node.pdata = common_util_get_propvals(&pbrief->proplist,
					PROP_TAG_SENTREPRESENTINGSMTPADDRESS);
	if (NULL == node.pdata) {
		return;
	}
	double_list_init(&rcpt_list);
	double_list_append_as_tail(&rcpt_list, &node);
	mail_init(&imail, g_mime_pool);
	int bounce_type = type == NOTIFY_RECEIPT_READ ? BOUNCE_NOTIFY_READ : BOUNCE_NOTIFY_NON_READ;
	if (FALSE == bounce_producer_make(username,
		pbrief, bounce_type, &imail)) {
		mail_free(&imail);
		return;
	}
	common_util_send_mail(&imail, username, &rcpt_list);
	mail_free(&imail);
}

static MOVECOPY_ACTION* common_util_convert_from_zmovecopy(
	ZMOVECOPY_ACTION *pmovecopy)
{
	int db_id;
	int user_id;
	BOOL b_private;
	SVREID *psvreid;
	EXT_PULL ext_pull;
	
	auto pmovecopy1 = cu_alloc<MOVECOPY_ACTION>();
	if (NULL == pmovecopy1) {
		return NULL;
	}
	auto pstore_entryid = cu_alloc<STORE_ENTRYID>();
	if (NULL == pstore_entryid) {
		return NULL;
	}
	ext_pull.init(pmovecopy->store_eid.pb, pmovecopy->store_eid.cb,
		common_util_alloc, EXT_FLAG_UTF16);
	if (ext_pull.g_store_eid(pstore_entryid) != EXT_ERR_SUCCESS)
		return NULL;
	if (FALSE == common_util_essdn_to_uid(
		pstore_entryid->pmailbox_dn, &user_id)) {
		return NULL;	
	}
	auto pinfo = zarafa_server_get_info();
	if (user_id != pinfo->user_id) {
		pmovecopy1->same_store = 0;
		pmovecopy1->pstore_eid = pstore_entryid;
		pmovecopy1->pfolder_eid = &pmovecopy->folder_eid;
	} else {
		pmovecopy1->same_store = 1;
		pmovecopy1->pstore_eid = NULL;
		psvreid = cu_alloc<SVREID>();
		if (NULL == psvreid) {
			return NULL;
		}
		psvreid->pbin = NULL;
		if (FALSE == common_util_from_folder_entryid(
			pmovecopy->folder_eid, &b_private, &db_id,
			&psvreid->folder_id)) {
			return NULL;	
		}
		psvreid->message_id = 0;
		psvreid->instance = 0;
		pmovecopy1->pfolder_eid = psvreid;
	}
	return pmovecopy1;
}

static REPLY_ACTION* common_util_convert_from_zreply(ZREPLY_ACTION *preply)
{
	int db_id;
	BOOL b_private;
	
	auto preply1 = cu_alloc<REPLY_ACTION>();
	if (NULL == preply1) {
		return NULL;
	}
	if (FALSE == common_util_from_message_entryid(
		preply->message_eid, &b_private, &db_id,
		&preply1->template_folder_id,
		&preply1->template_message_id)) {
		return NULL;	
	}
	preply1->template_guid = preply->template_guid;
	return preply1;
}

BOOL common_util_convert_from_zrule(TPROPVAL_ARRAY *ppropvals)
{
	int i;
	
	auto pactions = static_cast<RULE_ACTIONS *>(common_util_get_propvals(
	                ppropvals, PR_RULE_ACTIONS));
	if (pactions == nullptr)
		return TRUE;
	for (i=0; i<pactions->count; i++) {
		switch (pactions->pblock[i].type) {
		case OP_MOVE:
		case OP_COPY:
			pactions->pblock[i].pdata =
				common_util_convert_from_zmovecopy(
				static_cast<ZMOVECOPY_ACTION *>(pactions->pblock[i].pdata));
			if (NULL == pactions->pblock[i].pdata) {
				return FALSE;
			}
			break;
		case OP_REPLY:
		case OP_OOF_REPLY:
			pactions->pblock[i].pdata =
				common_util_convert_from_zreply(
				static_cast<ZREPLY_ACTION *>(pactions->pblock[i].pdata));
			if (NULL == pactions->pblock[i].pdata) {
				return FALSE;
			}
			break;
		}
	}
	return TRUE;
}

BINARY* common_util_to_store_entryid(STORE_OBJECT *pstore)
{
	EXT_PUSH ext_push;
	char tmp_buff[1024];
	STORE_ENTRYID store_entryid = {};
	
	store_entryid.flags = 0;
	rop_util_get_provider_uid(PROVIDER_UID_STORE,
					store_entryid.provider_uid);
	store_entryid.version = 0;
	store_entryid.flag = 0;
	snprintf(store_entryid.dll_name, sizeof(store_entryid.dll_name), "emsmdb.dll");
	store_entryid.wrapped_flags = 0;
	if (pstore->b_private) {
		rop_util_get_provider_uid(
			PROVIDER_UID_WRAPPED_PRIVATE,
			store_entryid.wrapped_provider_uid);
		store_entryid.wrapped_type = 0x0000000C;
		store_entryid.pserver_name = deconst(pstore->get_account());
		if (!common_util_username_to_essdn(pstore->get_account(),
		    tmp_buff, GX_ARRAY_SIZE(tmp_buff)))
			return NULL;	
	} else {
		rop_util_get_provider_uid(
			PROVIDER_UID_WRAPPED_PUBLIC,
			store_entryid.wrapped_provider_uid);
		store_entryid.wrapped_type = 0x00000006;
		store_entryid.pserver_name = g_hostname;
		auto pinfo = zarafa_server_get_info();
		if (!common_util_username_to_essdn(pinfo->get_username(),
		    tmp_buff, arsizeof(tmp_buff)))
			return NULL;	
	}
	store_entryid.pmailbox_dn = tmp_buff;
	auto pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return NULL;
	}
	pbin->pv = common_util_alloc(1024);
	if (pbin->pb == nullptr ||
	    !ext_push.init(pbin->pv, 1024, EXT_FLAG_UTF16) ||
	    ext_push.p_store_eid(&store_entryid) != EXT_ERR_SUCCESS)
		return NULL;	
	pbin->cb = ext_push.m_offset;
	return pbin;
}

static ZMOVECOPY_ACTION* common_util_convert_to_zmovecopy(
	STORE_OBJECT *pstore, MOVECOPY_ACTION *pmovecopy)
{
	BINARY *pbin;
	EXT_PUSH ext_push;
	
	auto pmovecopy1 = cu_alloc<ZMOVECOPY_ACTION>();
	if (NULL == pmovecopy1) {
		return NULL;
	}
	if (0 == pmovecopy->same_store) {
		pmovecopy1->store_eid.pv = common_util_alloc(1024);
		if (pmovecopy1->store_eid.pv == nullptr ||
		    !ext_push.init(pmovecopy1->store_eid.pv, 1024, EXT_FLAG_UTF16) ||
		    ext_push.p_store_eid(pmovecopy->pstore_eid) != EXT_ERR_SUCCESS)
			return NULL;	
		pmovecopy1->store_eid.cb = ext_push.m_offset;
		pmovecopy1->folder_eid = *(BINARY*)pmovecopy->pfolder_eid;
	} else {
		pbin = common_util_to_store_entryid(pstore);
		if (NULL == pbin) {
			return NULL;
		}
		pmovecopy1->store_eid = *pbin;
		pbin = common_util_to_folder_entryid(
			pstore, ((SVREID*)pmovecopy->pfolder_eid)->folder_id);
		if (NULL == pbin) {
			return NULL;
		}
		pmovecopy1->folder_eid = *pbin;
	}
	return pmovecopy1;
}

static ZREPLY_ACTION* common_util_convert_to_zreply(
	STORE_OBJECT *pstore, REPLY_ACTION *preply)
{
	auto preply1 = cu_alloc<ZREPLY_ACTION>();
	if (NULL == preply1) {
		return NULL;
	}
	if (common_util_to_message_entryid(pstore, preply->template_folder_id,
	    preply->template_message_id) == nullptr)
		return NULL;	
	preply1->template_guid = preply->template_guid;
	return preply1;
}

BOOL common_util_convert_to_zrule_data(
	STORE_OBJECT *pstore, TPROPVAL_ARRAY *ppropvals)
{
	int i;
	
	auto pactions = static_cast<RULE_ACTIONS *>(common_util_get_propvals(ppropvals, PR_RULE_ACTIONS));
	if (pactions == nullptr)
		return TRUE;
	for (i=0; i<pactions->count; i++) {
		switch (pactions->pblock[i].type) {
		case OP_MOVE:
		case OP_COPY:
			pactions->pblock[i].pdata =
				common_util_convert_to_zmovecopy(
				pstore, static_cast<MOVECOPY_ACTION *>(pactions->pblock[i].pdata));
			if (NULL == pactions->pblock[i].pdata) {
				return FALSE;
			}
			break;
		case OP_REPLY:
		case OP_OOF_REPLY:
			pactions->pblock[i].pdata =
				common_util_convert_to_zreply(
				pstore, static_cast<REPLY_ACTION *>(pactions->pblock[i].pdata));
			if (NULL == pactions->pblock[i].pdata) {
				return FALSE;
			}
			break;
		}
	}
	return TRUE;
}

gxerr_t common_util_remote_copy_message(STORE_OBJECT *pstore,
    uint64_t message_id, STORE_OBJECT *pstore1, uint64_t folder_id1)
{
	XID tmp_xid;
	BINARY *pbin;
	BINARY *pbin1;
	uint64_t change_num;
	TAGGED_PROPVAL propval;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pinfo = zarafa_server_get_info();
	auto username = pstore->b_private ? nullptr : pinfo->get_username();
	if (!exmdb_client::read_message(pstore->get_dir(), username,
	    pinfo->cpid, message_id, &pmsgctnt))
		return GXERR_CALL_FAILED;
	if (NULL == pmsgctnt) {
		return GXERR_SUCCESS;
	}
	static constexpr uint32_t tags[] = {
		PROP_TAG_CONVERSATIONID, PR_DISPLAY_TO,
		PR_DISPLAY_TO_A, PR_DISPLAY_CC,
		PR_DISPLAY_CC_A, PR_DISPLAY_BCC,
		PR_DISPLAY_BCC_A, PROP_TAG_MID,
		PR_MESSAGE_SIZE, PR_MESSAGE_SIZE_EXTENDED,
		PROP_TAG_HASNAMEDPROPERTIES, PROP_TAG_HASATTACHMENTS,
		PR_ENTRYID, PROP_TAG_FOLDERID, PR_OBJECT_TYPE,
		PR_PARENT_ENTRYID, PR_STORE_RECORD_KEY,
	};
	for (auto t : tags)
		common_util_remove_propvals(&pmsgctnt->proplist, t);
	if (!exmdb_client::allocate_cn(pstore->get_dir(), &change_num))
		return GXERR_CALL_FAILED;
	propval.proptag = PROP_TAG_CHANGENUMBER;
	propval.pvalue = &change_num;
	common_util_set_propvals(&pmsgctnt->proplist, &propval);
	tmp_xid.guid = pstore->guid();
	rop_util_get_gc_array(change_num, tmp_xid.local_id);
	pbin = common_util_xid_to_binary(22, &tmp_xid);
	if (NULL == pbin) {
		return GXERR_CALL_FAILED;
	}
	propval.proptag = PR_CHANGE_KEY;
	propval.pvalue = pbin;
	common_util_set_propvals(&pmsgctnt->proplist, &propval);
	pbin1 = static_cast<BINARY *>(common_util_get_propvals(&pmsgctnt->proplist,
	        PR_PREDECESSOR_CHANGE_LIST));
	propval.proptag = PR_PREDECESSOR_CHANGE_LIST;
	propval.pvalue = common_util_pcl_append(pbin1, pbin);
	if (NULL == propval.pvalue) {
		return GXERR_CALL_FAILED;
	}
	common_util_set_propvals(&pmsgctnt->proplist, &propval);
	gxerr_t e_result = GXERR_CALL_FAILED;
	if (!exmdb_client::write_message(pstore1->get_dir(),
	    pstore1->get_account(), pinfo->cpid, folder_id1,
	    pmsgctnt, &e_result) || e_result != GXERR_SUCCESS)
		return e_result;
	return GXERR_SUCCESS;
}

static BOOL common_util_create_folder(
	STORE_OBJECT *pstore, uint64_t parent_id,
	TPROPVAL_ARRAY *pproplist, uint64_t *pfolder_id)
{
	XID tmp_xid;
	BINARY *pbin;
	BINARY *pbin1;
	uint64_t tmp_id;
	BINARY *pentryid;
	uint32_t tmp_type;
	uint64_t change_num;
	uint32_t permission;
	TAGGED_PROPVAL propval;
	PERMISSION_DATA permission_row;
	TAGGED_PROPVAL propval_buff[10];

	static constexpr uint32_t tags[] = {	
		PR_ACCESS, PR_ACCESS_LEVEL, PR_ADDRESS_BOOK_ENTRYID,
		PR_ASSOC_CONTENT_COUNT, PROP_TAG_ATTRIBUTEREADONLY,
		PROP_TAG_CONTENTCOUNT, PROP_TAG_CONTENTUNREADCOUNT,
		PR_DELETED_COUNT_TOTAL, PR_DELETED_FOLDER_COUNT,
		PROP_TAG_ARTICLENUMBERNEXT, PROP_TAG_INTERNETARTICLENUMBER,
		PR_DISPLAY_TYPE, PR_DELETED_ON, PR_ENTRYID,
		PROP_TAG_FOLDERCHILDCOUNT, PROP_TAG_FOLDERFLAGS, PROP_TAG_FOLDERID,
		PR_FOLDER_TYPE, PROP_TAG_HASRULES, PROP_TAG_HIERARCHYCHANGENUMBER,
		PROP_TAG_LOCALCOMMITTIME, PROP_TAG_LOCALCOMMITTIMEMAX,
		PR_MESSAGE_SIZE, PR_MESSAGE_SIZE_EXTENDED, PROP_TAG_NATIVEBODY,
		PR_OBJECT_TYPE, PR_PARENT_ENTRYID, PR_RECORD_KEY,
		PROP_TAG_SEARCHKEY, PR_STORE_ENTRYID, PR_STORE_RECORD_KEY,
		PR_SOURCE_KEY, PR_PARENT_SOURCE_KEY,
	};
	for (auto t : tags)
		common_util_remove_propvals(pproplist, t);
	if (common_util_get_propvals(pproplist, PR_DISPLAY_NAME) == nullptr)
		return FALSE;
	propval.proptag = PR_FOLDER_TYPE;
	propval.pvalue = &tmp_type;
	tmp_type = FOLDER_GENERIC;
	common_util_set_propvals(pproplist, &propval);
	propval.proptag = PROP_TAG_PARENTFOLDERID;
	propval.pvalue = &parent_id;
	common_util_set_propvals(pproplist, &propval);
	if (!exmdb_client::allocate_cn(pstore->get_dir(), &change_num))
		return FALSE;
	propval.proptag = PROP_TAG_CHANGENUMBER;
	propval.pvalue = &change_num;
	common_util_set_propvals(pproplist, &propval);
	tmp_xid.guid = pstore->guid();
	rop_util_get_gc_array(change_num, tmp_xid.local_id);
	pbin = common_util_xid_to_binary(22, &tmp_xid);
	if (NULL == pbin) {
		return FALSE;
	}
	propval.proptag = PR_CHANGE_KEY;
	propval.pvalue = pbin;
	common_util_set_propvals(pproplist, &propval);
	pbin1 = static_cast<BINARY *>(common_util_get_propvals(pproplist, PR_PREDECESSOR_CHANGE_LIST));
	propval.proptag = PR_PREDECESSOR_CHANGE_LIST;
	propval.pvalue = common_util_pcl_append(pbin1, pbin);
	if (NULL == propval.pvalue) {
		return FALSE;
	}
	common_util_set_propvals(pproplist, &propval);
	auto pinfo = zarafa_server_get_info();
	if (!exmdb_client::create_folder_by_properties(pstore->get_dir(),
	    pinfo->cpid, pproplist, pfolder_id) || *pfolder_id == 0)
		return FALSE;
	if (pstore->check_owner_mode())
		return TRUE;
	pentryid = common_util_username_to_addressbook_entryid(pinfo->get_username());
	if (pentryid == nullptr)
		return TRUE;
	tmp_id = 1;
	permission = rightsGromox7;
	permission_row.flags = PERMISSION_DATA_FLAG_ADD_ROW;
	permission_row.propvals.count = 3;
	permission_row.propvals.ppropval = propval_buff;
	propval_buff[0].proptag = PR_ENTRYID;
	propval_buff[0].pvalue = pentryid;
	propval_buff[1].proptag = PROP_TAG_MEMBERID;
	propval_buff[1].pvalue = &tmp_id;
	propval_buff[2].proptag = PROP_TAG_MEMBERRIGHTS;
	propval_buff[2].pvalue = &permission;
	exmdb_client::update_folder_permission(pstore->get_dir(),
		*pfolder_id, FALSE, 1, &permission_row);
	return TRUE;
}

static EID_ARRAY* common_util_load_folder_messages(
	STORE_OBJECT *pstore, uint64_t folder_id,
	const char *username)
{
	uint64_t *pmid;
	uint32_t table_id;
	uint32_t row_count;
	TARRAY_SET tmp_set;
	uint32_t tmp_proptag;
	PROPTAG_ARRAY proptags;
	EID_ARRAY *pmessage_ids;
	
	if (!exmdb_client::load_content_table(pstore->get_dir(), 0, folder_id,
	    username, TABLE_FLAG_NONOTIFICATIONS,
	    nullptr, nullptr, &table_id, &row_count))
		return NULL;	
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PROP_TAG_MID;
	if (!exmdb_client::query_table(pstore->get_dir(), nullptr, 0, table_id,
	    &proptags, 0, row_count, &tmp_set))
		return NULL;	
	exmdb_client::unload_table(pstore->get_dir(), table_id);
	pmessage_ids = cu_alloc<EID_ARRAY>();
	if (NULL == pmessage_ids) {
		return NULL;
	}
	pmessage_ids->count = 0;
	pmessage_ids->pids = cu_alloc<uint64_t>(tmp_set.count);
	if (NULL == pmessage_ids->pids) {
		return NULL;
	}
	for (size_t i = 0; i < tmp_set.count; ++i) {
		pmid = static_cast<uint64_t *>(common_util_get_propvals(
		       tmp_set.pparray[i], PROP_TAG_MID));
		if (NULL == pmid) {
			return NULL;
		}
		pmessage_ids->pids[pmessage_ids->count] = *pmid;
		pmessage_ids->count ++;
	}
	return pmessage_ids;
}

gxerr_t common_util_remote_copy_folder(STORE_OBJECT *pstore, uint64_t folder_id,
    STORE_OBJECT *pstore1, uint64_t folder_id1, const char *new_name)
{
	uint64_t new_fid;
	uint32_t table_id;
	uint32_t row_count;
	TARRAY_SET tmp_set;
	uint32_t permission;
	uint64_t *pfolder_id;
	uint32_t tmp_proptag;
	TAGGED_PROPVAL propval;
	EID_ARRAY *pmessage_ids;
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (!exmdb_client::get_folder_all_proptags(pstore->get_dir(),
	    folder_id, &tmp_proptags))
		return GXERR_CALL_FAILED;
	if (!exmdb_client::get_folder_properties(pstore->get_dir(), 0,
	    folder_id, &tmp_proptags, &tmp_propvals))
		return GXERR_CALL_FAILED;
	if (NULL != new_name) {
		propval.proptag = PR_DISPLAY_NAME;
		propval.pvalue = deconst(new_name);
		common_util_set_propvals(&tmp_propvals, &propval);
	}
	if (FALSE == common_util_create_folder(pstore1,
		folder_id1, &tmp_propvals, &new_fid)) {
		return GXERR_CALL_FAILED;
	}
	auto pinfo = zarafa_server_get_info();
	const char *username = nullptr;
	if (!pstore->check_owner_mode()) {
		username = pinfo->get_username();
		if (!exmdb_client::check_folder_permission(pstore->get_dir(),
		    folder_id, username, &permission))
			return GXERR_CALL_FAILED;
		if (!(permission & (frightsReadAny | frightsOwner)))
			return GXERR_CALL_FAILED;
	}
	pmessage_ids = common_util_load_folder_messages(
						pstore, folder_id, username);
	if (NULL == pmessage_ids) {
		return GXERR_CALL_FAILED;
	}
	for (size_t i = 0; i < pmessage_ids->count; ++i) {
		gxerr_t err = common_util_remote_copy_message(pstore,
		              pmessage_ids->pids[i], pstore1, new_fid);
		if (err != GXERR_SUCCESS)
			return err;
	}
	username = pstore->check_owner_mode() ? nullptr : pinfo->get_username();
	if (!exmdb_client::load_hierarchy_table(pstore->get_dir(), folder_id,
	    username, TABLE_FLAG_NONOTIFICATIONS, nullptr,
	    &table_id, &row_count))
		return GXERR_CALL_FAILED;
	tmp_proptags.count = 1;
	tmp_proptags.pproptag = &tmp_proptag;
	tmp_proptag = PROP_TAG_FOLDERID;
	if (!exmdb_client::query_table(pstore->get_dir(), nullptr, 0, table_id,
	    &tmp_proptags, 0, row_count, &tmp_set))
		return GXERR_CALL_FAILED;
	exmdb_client::unload_table(pstore->get_dir(), table_id);
	for (size_t i = 0; i < tmp_set.count; ++i) {
		pfolder_id = static_cast<uint64_t *>(common_util_get_propvals(
		             tmp_set.pparray[i], PROP_TAG_FOLDERID));
		if (NULL == pfolder_id) {
			return GXERR_CALL_FAILED;
		}
		gxerr_t err = common_util_remote_copy_folder(pstore,
		              *pfolder_id, pstore1, new_fid, nullptr);
		if (err != GXERR_SUCCESS)
			return err;
	}
	return GXERR_SUCCESS;
}

const uint8_t *common_util_get_muidecsab()
{
	static const uint8_t MUIDECSAB[] = {
		0xAC, 0x21, 0xA9, 0x50, 0x40, 0xD3, 0xEE, 0x48,
		0xB3, 0x19, 0xFB, 0xA7, 0x53, 0x30, 0x44, 0x25};
	
	return MUIDECSAB;
}

const uint8_t *common_util_get_muidzcsab()
{
	static const uint8_t MUIDZCSAB[] = {
		0x72, 0x7F, 0x04, 0x30, 0xE3, 0x92, 0x4F, 0xDA,
		0xB8, 0x6A, 0xE5, 0x2A, 0x7F, 0xE4, 0x65, 0x71};
	return MUIDZCSAB;
}

BOOL common_util_message_to_rfc822(STORE_OBJECT *pstore,
	uint64_t message_id, BINARY *peml_bin)
{
	int size;
	void *ptr;
	MAIL imail;
	void *pvalue;
	int body_type;
	STREAM tmp_stream;
	LIB_BUFFER *pallocator;
	TAGGED_PROPVAL *ppropval;
	MESSAGE_CONTENT *pmsgctnt;
	
	if (exmdb_client_get_message_property(pstore->get_dir(), nullptr, 0,
	    message_id, PROP_TAG_MIDSTRING, &pvalue) && pvalue != nullptr) try {
		auto eml_path = pstore->get_dir() + "/eml/"s +
		                static_cast<const char *>(pvalue);
		return common_util_load_file(eml_path.c_str(), peml_bin);
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1495: ENOMEM\n");
		return false;
	}
	auto pinfo = zarafa_server_get_info();
	uint32_t cpid = pinfo == nullptr ? 1252 : pinfo->cpid;
	if (!exmdb_client::read_message(pstore->get_dir(), nullptr, cpid,
	    message_id, &pmsgctnt) || pmsgctnt == nullptr)
		return FALSE;
	if (common_util_get_propvals(&pmsgctnt->proplist, PR_INTERNET_CPID) == nullptr) {
		ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt->proplist.count + 1);
		if (NULL == ppropval) {
			return FALSE;
		}
		memcpy(ppropval, pmsgctnt->proplist.ppropval,
			sizeof(TAGGED_PROPVAL)*pmsgctnt->proplist.count);
		ppropval[pmsgctnt->proplist.count].proptag = PR_INTERNET_CPID;
		ppropval[pmsgctnt->proplist.count].pvalue = &cpid;
		pmsgctnt->proplist.ppropval = ppropval;
		pmsgctnt->proplist.count ++;
	}
	pvalue = common_util_get_propvals(&pmsgctnt->proplist,
					PROP_TAG_INTERNETMAILOVERRIDEFORMAT);
	if (NULL == pvalue) {
		body_type = OXCMAIL_BODY_PLAIN_AND_HTML;
	} else {
		if (*(uint32_t*)pvalue & MESSAGE_FORMAT_PLAIN_AND_HTML) {
			body_type = OXCMAIL_BODY_PLAIN_AND_HTML;
		} else if (*(uint32_t*)pvalue & MESSAGE_FORMAT_HTML_ONLY) {
			body_type = OXCMAIL_BODY_HTML_ONLY;
		} else {
			body_type = OXCMAIL_BODY_PLAIN_ONLY;
		}
	}
	common_util_set_dir(pstore->get_dir());
	/* try to avoid TNEF message */
	if (FALSE == oxcmail_export(pmsgctnt, FALSE,
		body_type, g_mime_pool, &imail, common_util_alloc,
		common_util_get_propids, common_util_get_propname)) {
		return FALSE;	
	}
	auto mail_len = mail_get_length(&imail);
	if (mail_len < 0) {
		mail_free(&imail);
		return false;
	}
	pallocator = lib_buffer_init(STREAM_ALLOC_SIZE,
			mail_len / STREAM_BLOCK_SIZE + 1, FALSE);
	if (NULL == pallocator) {
		mail_free(&imail);
		return FALSE;
	}
	stream_init(&tmp_stream, pallocator);
	if (FALSE == mail_serialize(&imail, &tmp_stream)) {
		stream_free(&tmp_stream);
		lib_buffer_free(pallocator);
		mail_free(&imail);
		return FALSE;
	}
	mail_free(&imail);
	peml_bin->pv = common_util_alloc(mail_len + 128);
	if (peml_bin->pv == nullptr) {
		stream_free(&tmp_stream);
		lib_buffer_free(pallocator);
		return FALSE;
	}

	peml_bin->cb = 0;
	size = STREAM_BLOCK_SIZE;
	while ((ptr = stream_getbuffer_for_reading(&tmp_stream,
	       reinterpret_cast<unsigned int *>(&size))) != nullptr) {
		memcpy(peml_bin->pb + peml_bin->cb, ptr, size);
		peml_bin->cb += size;
		size = STREAM_BLOCK_SIZE;
	}
	stream_free(&tmp_stream);
	lib_buffer_free(pallocator);
	return TRUE;
}

MESSAGE_CONTENT* common_util_rfc822_to_message(
	STORE_OBJECT *pstore, const BINARY *peml_bin)
{
	MAIL imail;
	char charset[32], tmzone[64];
	
	auto pinfo = zarafa_server_get_info();
	mail_init(&imail, g_mime_pool);
	if (!mail_retrieve(&imail, peml_bin->pc, peml_bin->cb)) {
		mail_free(&imail);
		return NULL;
	}
	if (!system_services_lang_to_charset(pinfo->get_lang(), charset) ||
	    charset[0] == '\0')
		strcpy(charset, g_default_charset);
	if (!system_services_get_timezone(pinfo->get_username(), tmzone) ||
	    tmzone[0] == '\0')
		strcpy(tmzone, g_default_zone);
	common_util_set_dir(pstore->get_dir());
	auto pmsgctnt = oxcmail_import(charset, tmzone, &imail,
	                common_util_alloc, common_util_get_propids_create);
	mail_free(&imail);
	return pmsgctnt;
}

BOOL common_util_message_to_ical(STORE_OBJECT *pstore,
	uint64_t message_id, BINARY *pical_bin)
{
	ICAL ical;
	char tmp_buff[1024*1024];
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pinfo = zarafa_server_get_info();
	uint32_t cpid = pinfo == nullptr ? 1252 : pinfo->cpid;
	if (!exmdb_client::read_message(pstore->get_dir(), nullptr, cpid,
	    message_id, &pmsgctnt) || pmsgctnt == nullptr)
		return FALSE;
	if (ical_init(&ical) < 0)
		return false;
	common_util_set_dir(pstore->get_dir());
	if (FALSE == oxcical_export(pmsgctnt, &ical,
		common_util_alloc, common_util_get_propids,
		common_util_entryid_to_username_internal,
		common_util_essdn_to_username,
		system_services_lcid_to_ltag)) {
		return FALSE;
	}
	if (!ical_serialize(&ical, tmp_buff, sizeof(tmp_buff)))
		return FALSE;	
	pical_bin->cb = strlen(tmp_buff);
	pical_bin->pc = common_util_dup(tmp_buff);
	return pical_bin->pc != nullptr ? TRUE : FALSE;
}

MESSAGE_CONTENT* common_util_ical_to_message(
	STORE_OBJECT *pstore, const BINARY *pical_bin)
{
	ICAL ical;
	char tmzone[64];
	
	auto pinfo = zarafa_server_get_info();
	if (!system_services_get_timezone(pinfo->get_username(), tmzone) ||
	    tmzone[0] == '\0')
		strcpy(tmzone, g_default_zone);
	auto pbuff = cu_alloc<char>(pical_bin->cb + 1);
	if (NULL == pbuff) {
		return nullptr;
	}
	memcpy(pbuff, pical_bin->pb, pical_bin->cb);
	pbuff[pical_bin->cb] = '\0';
	if (ical_init(&ical) < 0)
		return nullptr;
	if (!ical_retrieve(&ical, pbuff))
		return NULL;
	common_util_set_dir(pstore->get_dir());
	return oxcical_import(tmzone, &ical, common_util_alloc,
	       common_util_get_propids_create, common_util_username_to_entryid);
}

BOOL common_util_message_to_vcf(MESSAGE_OBJECT *pmessage, BINARY *pvcf_bin)
{
	auto pstore = pmessage->get_store();
	auto message_id = pmessage->get_id();
	VCARD vcard;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pinfo = zarafa_server_get_info();
	uint32_t cpid = pinfo == nullptr ? 1252 : pinfo->cpid;
	if (!exmdb_client::read_message(pstore->get_dir(), nullptr, cpid,
	    message_id, &pmsgctnt) || pmsgctnt == nullptr)
		return FALSE;
	common_util_set_dir(pstore->get_dir());
	if (FALSE == oxvcard_export(pmsgctnt,
		&vcard, common_util_get_propids)) {
		return FALSE;
	}
	pvcf_bin->pv = common_util_alloc(VCARD_MAX_BUFFER_LEN);
	if (pvcf_bin->pv == nullptr) {
		vcard_free(&vcard);
		return FALSE;
	}
	if (!vcard_serialize(&vcard, pvcf_bin->pc, VCARD_MAX_BUFFER_LEN)) {
		vcard_free(&vcard);
		return FALSE;	
	}
	vcard_free(&vcard);
	pvcf_bin->cb = strlen(pvcf_bin->pc);
	if (!pmessage->write_message(pmsgctnt))
		/* ignore */;
	return TRUE;
}
	
MESSAGE_CONTENT* common_util_vcf_to_message(
	STORE_OBJECT *pstore, const BINARY *pvcf_bin)
{
	VCARD vcard;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pbuff = cu_alloc<char>(pvcf_bin->cb + 1);
	if (NULL == pbuff) {
		return nullptr;
	}
	memcpy(pbuff, pvcf_bin->pb, pvcf_bin->cb);
	pbuff[pvcf_bin->cb] = '\0';
	vcard_init(&vcard);
	if (FALSE == vcard_retrieve(&vcard, pbuff)) {
		vcard_free(&vcard);
		return nullptr;
	}
	common_util_set_dir(pstore->get_dir());
	pmsgctnt = oxvcard_import(&vcard, common_util_get_propids_create);
	vcard_free(&vcard);
	return pmsgctnt;
}

uint64_t common_util_convert_notification_folder_id(uint64_t folder_id)
{
	if (!(folder_id & 0xFF00000000000000ULL))
		return rop_util_make_eid_ex(1, folder_id);
	return rop_util_make_eid_ex(folder_id >> 48, folder_id & 0x00FFFFFFFFFFFFFFULL);
}

const char* common_util_i18n_to_lang(const char *i18n)
{
	auto pitem = static_cast<LANGMAP_ITEM *>(g_langmap_list->get_list());
	auto num = g_langmap_list->get_size();
	for (decltype(num) i = 0; i < num; ++i) {
		if (0 == strcasecmp(pitem[i].i18n, i18n)) {
			return pitem[i].lang;
		}
	}
	return nullptr;
}

const char* common_util_get_default_timezone()
{
	return g_default_zone;
}

const char* common_util_get_submit_command()
{
	return g_submit_command;
}

void common_util_get_folder_lang(const char *lang, char **ppfolder_lang)
{
	size_t i;
	
	auto line_num = g_folderlang_list->get_size();
	auto pline = static_cast<char *>(g_folderlang_list->get_list());
	for (i=0; i<line_num; i++) {
		if (0 != strcasecmp(pline + 1088*i, lang)) {
			continue;
		}
		for (size_t j = 0; j < RES_TOTAL_NUM; ++j)
			ppfolder_lang[j] = pline + 1088*i + 64*(j + 1);
		break;
	}
	if (i >= line_num) {
		ppfolder_lang[RES_ID_IPM]      = deconst("Top of Information Store");
		ppfolder_lang[RES_ID_INBOX]    = deconst("Inbox");
		ppfolder_lang[RES_ID_DRAFT]    = deconst("Drafts");
		ppfolder_lang[RES_ID_OUTBOX]   = deconst("Outbox");
		ppfolder_lang[RES_ID_SENT]     = deconst("Sent Items");
		ppfolder_lang[RES_ID_DELETED]  = deconst("Deleted Items");
		ppfolder_lang[RES_ID_CONTACTS] = deconst("Contacts");
		ppfolder_lang[RES_ID_CALENDAR] = deconst("Calendar");
		ppfolder_lang[RES_ID_JOURNAL]  = deconst("Journal");
		ppfolder_lang[RES_ID_NOTES]    = deconst("Notes");
		ppfolder_lang[RES_ID_TASKS]    = deconst("Tasks");
		ppfolder_lang[RES_ID_JUNK]     = deconst("Junk E-mail");
		ppfolder_lang[RES_ID_SYNC]     = deconst("Sync Issues");
		ppfolder_lang[RES_ID_CONFLICT] = deconst("Conflicts");
		ppfolder_lang[RES_ID_LOCAL]    = deconst("Local Failures");
		ppfolder_lang[RES_ID_SERVER]   = deconst("Server Failures");
	}
}
