// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif
#include <cctype>
#include <cstdint>
#include <libHX/string.h>
#include "msgchg_grouping.h"
#include "system_services.h"
#include "zarafa_server.h"
#include "store_object.h"
#include "exmdb_client.h"
#include "common_util.h"
#include "object_tree.h"
#include <gromox/config_file.hpp>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/mail_func.hpp>
#include <gromox/mapidefs.h>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>
#include <gromox/util.hpp>
#include <gromox/guid.hpp>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

#define HGROWING_SIZE									0x500

#define PROP_TAG_USERNAME								0x661A001F

#define PROP_TAG_ECSERVERVERSION						0x6716001F
#define PROP_TAG_ECUSERLANGUAGE							0x6770001F
#define PROP_TAG_ECUSERTIMEZONE							0x6771001F

using namespace std::string_literals;
using namespace gromox;

static BOOL store_object_enlarge_propid_hash(STORE_OBJECT *pstore)
{
	int tmp_id;
	void *ptmp_value;
	INT_HASH_ITER *iter;
	INT_HASH_TABLE *phash = int_hash_init(pstore->ppropid_hash->capacity +
	                        HGROWING_SIZE, sizeof(PROPERTY_NAME));
	if (NULL == phash) {
		return FALSE;
	}
	iter = int_hash_iter_init(pstore->ppropid_hash);
	for (int_hash_iter_begin(iter); !int_hash_iter_done(iter);
		int_hash_iter_forward(iter)) {
		ptmp_value = int_hash_iter_get_value(iter, &tmp_id);
		int_hash_add(phash, tmp_id, ptmp_value);
	}
	int_hash_iter_free(iter);
	int_hash_free(pstore->ppropid_hash);
	pstore->ppropid_hash = phash;
	return TRUE;
}

static BOOL store_object_enlarge_propname_hash(STORE_OBJECT *pstore)
{
	void *ptmp_value;
	STR_HASH_ITER *iter;
	char tmp_string[256];
	STR_HASH_TABLE *phash;
	
	phash = str_hash_init(pstore->ppropname_hash->capacity
				+ HGROWING_SIZE, sizeof(uint16_t), NULL);
	if (NULL == phash) {
		return FALSE;
	}
	iter = str_hash_iter_init(pstore->ppropname_hash);
	for (str_hash_iter_begin(iter); !str_hash_iter_done(iter);
		str_hash_iter_forward(iter)) {
		ptmp_value = str_hash_iter_get_value(iter, tmp_string);
		str_hash_add(phash, tmp_string, ptmp_value);
	}
	str_hash_iter_free(iter);
	str_hash_free(pstore->ppropname_hash);
	pstore->ppropname_hash = phash;
	return TRUE;
}

static BOOL store_object_cache_propname(STORE_OBJECT *pstore,
	uint16_t propid, const PROPERTY_NAME *ppropname)
{
	char tmp_guid[64];
	char tmp_string[256];
	PROPERTY_NAME tmp_name;
	
	if (NULL == pstore->ppropid_hash) {
		pstore->ppropid_hash = int_hash_init(HGROWING_SIZE, sizeof(PROPERTY_NAME));
		if (NULL == pstore->ppropid_hash) {
			return FALSE;
		}
	}
	if (NULL == pstore->ppropname_hash) {
		pstore->ppropname_hash = str_hash_init(
			HGROWING_SIZE, sizeof(uint16_t), NULL);
		if (NULL == pstore->ppropname_hash) {
			int_hash_free(pstore->ppropid_hash);
			return FALSE;
		}
	}
	tmp_name.kind = ppropname->kind;
	tmp_name.guid = ppropname->guid;
	guid_to_string(&ppropname->guid, tmp_guid, 64);
	switch (ppropname->kind) {
	case MNID_ID:
		tmp_name.lid = ppropname->lid;
		tmp_name.pname = NULL;
		snprintf(tmp_string, arsizeof(tmp_string), "%s:lid:%u", tmp_guid, ppropname->lid);
		break;
	case MNID_STRING:
		tmp_name.lid = 0;
		tmp_name.pname = strdup(ppropname->pname);
		if (NULL == tmp_name.pname) {
			return FALSE;
		}
		snprintf(tmp_string, 256, "%s:name:%s", tmp_guid, ppropname->pname);
		break;
	default:
		return FALSE;
	}
	if (NULL == int_hash_query(pstore->ppropid_hash, propid)) {
		if (1 != int_hash_add(pstore->ppropid_hash, propid, &tmp_name)) {
			if (FALSE == store_object_enlarge_propid_hash(pstore) ||
				1 != int_hash_add(pstore->ppropid_hash, propid, &tmp_name)) {
				if (NULL != tmp_name.pname) {
					free(tmp_name.pname);
				}
				return FALSE;
			}
		}
	} else {
		if (NULL != tmp_name.pname) {
			free(tmp_name.pname);
		}
	}
	HX_strlower(tmp_string);
	if (NULL == str_hash_query(pstore->ppropname_hash, tmp_string)) {
		if (1 != str_hash_add(pstore->ppropname_hash, tmp_string, &propid)) {
			if (FALSE == store_object_enlarge_propname_hash(pstore)
				|| 1 != str_hash_add(pstore->ppropname_hash,
				tmp_string, &propid)) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

std::unique_ptr<STORE_OBJECT> store_object_create(BOOL b_private,
	int account_id, const char *account, const char *dir)
{
	void *pvalue;
	uint32_t proptag;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	
	proptags.count = 1;
	proptags.pproptag = &proptag;
	proptag = PR_STORE_RECORD_KEY;
	if (!exmdb_client::get_store_properties(
		dir, 0, &proptags, &propvals)) {
		printf("get_store_properties %s: failed\n", dir);
		return NULL;	
	}
	pvalue = common_util_get_propvals(&propvals, PR_STORE_RECORD_KEY);
	if (NULL == pvalue) {
		return NULL;
	}
	std::unique_ptr<STORE_OBJECT> pstore;
	try {
		pstore = std::make_unique<STORE_OBJECT>();
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	pstore->b_private = b_private;
	pstore->account_id = account_id;
	gx_strlcpy(pstore->account, account, GX_ARRAY_SIZE(pstore->account));
	gx_strlcpy(pstore->dir, dir, GX_ARRAY_SIZE(pstore->dir));
	pstore->mailbox_guid = rop_util_binary_to_guid(static_cast<BINARY *>(pvalue));
	pstore->m_gpinfo = nullptr;
	pstore->ppropid_hash = NULL;
	pstore->ppropname_hash = NULL;
	double_list_init(&pstore->group_list);
	return pstore;
}

STORE_OBJECT::~STORE_OBJECT()
{
	INT_HASH_ITER *piter;
	DOUBLE_LIST_NODE *pnode;
	PROPERTY_NAME *ppropname;

	auto pstore = this;
	if (m_gpinfo != nullptr)
		property_groupinfo_free(m_gpinfo);
	while ((pnode = double_list_pop_front(&pstore->group_list)) != nullptr) {
		property_groupinfo_free(static_cast<PROPERTY_GROUPINFO *>(pnode->pdata));
		free(pnode);
	}
	double_list_free(&pstore->group_list);
	if (NULL != pstore->ppropid_hash) {
		piter = int_hash_iter_init(pstore->ppropid_hash);
		for (int_hash_iter_begin(piter); !int_hash_iter_done(piter);
			int_hash_iter_forward(piter)) {
			ppropname = static_cast<PROPERTY_NAME *>(int_hash_iter_get_value(piter, nullptr));
			switch( ppropname->kind) {
			case MNID_STRING:
				free(ppropname->pname);
				break;
			}
		}
		int_hash_iter_free(piter);
		int_hash_free(pstore->ppropid_hash);
	}
	if (NULL != pstore->ppropname_hash) {
		str_hash_free(pstore->ppropname_hash);
	}
}

GUID STORE_OBJECT::guid() const
{
	return b_private ? rop_util_make_user_guid(account_id) :
	       rop_util_make_domain_guid(account_id);
}

BOOL STORE_OBJECT::check_owner_mode() const
{
	auto pstore = this;
	if (!pstore->b_private)
		return FALSE;
	auto pinfo = zarafa_server_get_info();
	if (pinfo->user_id == pstore->account_id)
		return TRUE;
	std::unique_lock lk(pinfo->eowner_lock);
	auto i = pinfo->extra_owner.find(pstore->account_id);
	if (i == pinfo->extra_owner.end())
		return false;
	auto age = time(nullptr) - i->second;
	if (age < 60)
		return TRUE;
	lk.unlock();
	uint32_t perm = rightsNone;
	if (!exmdb_client::check_mailbox_permission(pstore->dir,
	    pinfo->get_username(), &perm))
		return false;
	if (!(perm & frightsGromoxStoreOwner))
		return false;
	lk.lock();
	i = pinfo->extra_owner.find(pstore->account_id);
	if (i == pinfo->extra_owner.end())
		return false;
	i->second = time(nullptr);
	return TRUE;
}

BOOL STORE_OBJECT::get_named_propnames(const PROPID_ARRAY *ppropids, PROPNAME_ARRAY *ppropnames)
{
	int i;
	PROPERTY_NAME *pname;
	PROPID_ARRAY tmp_propids;
	PROPNAME_ARRAY tmp_propnames;
	
	if (0 == ppropids->count) {
		ppropnames->count = 0;
		return TRUE;
	}
	auto pindex_map = cu_alloc<int>(ppropids->count);
	if (NULL == pindex_map) {
		return FALSE;
	}
	ppropnames->ppropname = cu_alloc<PROPERTY_NAME>(ppropids->count);
	if (NULL == ppropnames->ppropname) {
		return FALSE;
	}
	ppropnames->count = ppropids->count;
	tmp_propids.count = 0;
	tmp_propids.ppropid = cu_alloc<uint16_t>(ppropids->count);
	if (NULL == tmp_propids.ppropid) {
		return FALSE;
	}
	auto pstore = this;
	for (i=0; i<ppropids->count; i++) {
		if (ppropids->ppropid[i] < 0x8000) {
			rop_util_get_common_pset(PS_MAPI,
				&ppropnames->ppropname[i].guid);
			ppropnames->ppropname[i].kind = MNID_ID;
			ppropnames->ppropname[i].lid = ppropids->ppropid[i];
			pindex_map[i] = i;
			continue;
		}
		pname = pstore->ppropid_hash == nullptr ? nullptr :
		        static_cast<PROPERTY_NAME *>(int_hash_query(pstore->ppropid_hash, ppropids->ppropid[i]));
		if (NULL != pname) {
			pindex_map[i] = i;
			ppropnames->ppropname[i] = *pname;
		} else {
			tmp_propids.ppropid[tmp_propids.count] =
								ppropids->ppropid[i];
			tmp_propids.count ++;
			pindex_map[i] = -tmp_propids.count;
		}
	}
	if (0 == tmp_propids.count) {
		return TRUE;
	}
	if (!exmdb_client::get_named_propnames(
		pstore->dir, &tmp_propids, &tmp_propnames)) {
		return FALSE;	
	}
	for (i=0; i<ppropids->count; i++) {
		if (pindex_map[i] >= 0)
			continue;
		ppropnames->ppropname[i] = tmp_propnames.ppropname[-pindex_map[i]-1];
		if (ppropnames->ppropname[i].kind == MNID_ID ||
		    ppropnames->ppropname[i].kind == MNID_STRING)
			store_object_cache_propname(pstore,
				ppropids->ppropid[i], ppropnames->ppropname + i);
	}
	return TRUE;
}

static BOOL store_object_get_named_propid(STORE_OBJECT *pstore,
	BOOL b_create, const PROPERTY_NAME *ppropname,
	uint16_t *ppropid)
{
	GUID guid;
	uint16_t *pid;
	char tmp_guid[64];
	char tmp_string[256];
	
	rop_util_get_common_pset(PS_MAPI, &guid);
	if (0 == guid_compare(&ppropname->guid, &guid)) {
		*ppropid = ppropname->kind == MNID_ID ? ppropname->lid : 0;
		return TRUE;
	}
	guid_to_string(&ppropname->guid, tmp_guid, 64);
	switch (ppropname->kind) {
	case MNID_ID:
		snprintf(tmp_string, arsizeof(tmp_string), "%s:lid:%u", tmp_guid, ppropname->lid);
		break;
	case MNID_STRING:
		snprintf(tmp_string, 256, "%s:name:%s", tmp_guid, ppropname->pname);
		HX_strlower(tmp_string);
		break;
	default:
		*ppropid = 0;
		return TRUE;
	}
	if (NULL != pstore->ppropname_hash) {
		pid = static_cast<uint16_t *>(str_hash_query(pstore->ppropname_hash, tmp_string));
		if (NULL != pid) {
			*ppropid = *pid;
			return TRUE;
		}
	}
	if (FALSE == exmdb_client_get_named_propid(
		pstore->dir, b_create, ppropname, ppropid)) {
		return FALSE;
	}
	if (0 == *ppropid) {
		return TRUE;
	}
	store_object_cache_propname(pstore, *ppropid, ppropname);
	return TRUE;
}

BOOL STORE_OBJECT::get_named_propids(BOOL b_create,
    const PROPNAME_ARRAY *ppropnames, PROPID_ARRAY *ppropids)
{
	int i;
	GUID guid;
	uint16_t *pid;
	char tmp_guid[64];
	char tmp_string[256];
	PROPID_ARRAY tmp_propids;
	PROPNAME_ARRAY tmp_propnames;
	
	if (0 == ppropnames->count) {
		ppropids->count = 0;
		return TRUE;
	}
	rop_util_get_common_pset(PS_MAPI, &guid);
	auto pindex_map = cu_alloc<int>(ppropnames->count);
	if (NULL == pindex_map) {
		return FALSE;
	}
	ppropids->count = ppropnames->count;
	ppropids->ppropid = cu_alloc<uint16_t>(ppropnames->count);
	if (NULL == ppropids->ppropid) {
		return FALSE;
	}
	tmp_propnames.count = 0;
	tmp_propnames.ppropname = cu_alloc<PROPERTY_NAME>(ppropnames->count);
	if (NULL == tmp_propnames.ppropname) {
		return FALSE;
	}
	auto pstore = this;
	for (i=0; i<ppropnames->count; i++) {
		if (0 == guid_compare(&ppropnames->ppropname[i].guid, &guid)) {
			ppropids->ppropid[i] = ppropnames->ppropname[i].kind == MNID_ID ?
			                       ppropnames->ppropname[i].lid : 0;
			pindex_map[i] = i;
			continue;
		}
		guid_to_string(&ppropnames->ppropname[i].guid, tmp_guid, 64);
		switch (ppropnames->ppropname[i].kind) {
		case MNID_ID:
			snprintf(tmp_string, 256, "%s:lid:%u",
			         tmp_guid, ppropnames->ppropname[i].lid);
			break;
		case MNID_STRING:
			snprintf(tmp_string, 256, "%s:name:%s",
				tmp_guid, ppropnames->ppropname[i].pname);
			HX_strlower(tmp_string);
			break;
		default:
			ppropids->ppropid[i] = 0;
			pindex_map[i] = i;
			continue;
		}
		pid = pstore->ppropname_hash == nullptr ? nullptr :
		      static_cast<uint16_t *>(str_hash_query(pstore->ppropname_hash, tmp_string));
		if (NULL != pid) {
			pindex_map[i] = i;
			ppropids->ppropid[i] = *pid;
		} else {
			tmp_propnames.ppropname[tmp_propnames.count] =
									ppropnames->ppropname[i];
			tmp_propnames.count ++;
			pindex_map[i] = -tmp_propnames.count;
		}
	}
	if (0 == tmp_propnames.count) {
		return TRUE;
	}
	if (!exmdb_client::get_named_propids(pstore->dir,
		b_create, &tmp_propnames, &tmp_propids)) {
		return FALSE;	
	}
	for (i=0; i<ppropnames->count; i++) {
		if (pindex_map[i] >= 0)
			continue;
		ppropids->ppropid[i] = tmp_propids.ppropid[-pindex_map[i]-1];
		if (0 != ppropids->ppropid[i]) {
			store_object_cache_propname(pstore,
				ppropids->ppropid[i], ppropnames->ppropname + i);
		}
	}
	return TRUE;
}

static BOOL gnpwrap(void *store, BOOL create, const PROPERTY_NAME *pn, uint16_t *pid)
{
	return store_object_get_named_propid(static_cast<STORE_OBJECT *>(store), create, pn, pid);
}

PROPERTY_GROUPINFO *STORE_OBJECT::get_last_property_groupinfo()
{
	auto pstore = this;
	if (m_gpinfo == nullptr)
		m_gpinfo = msgchg_grouping_get_groupinfo(gnpwrap,
		           pstore, msgchg_grouping_get_last_group_id());
	return m_gpinfo;
}

PROPERTY_GROUPINFO *STORE_OBJECT::get_property_groupinfo(uint32_t group_id)
{
	auto pstore = this;
	
	if (group_id == msgchg_grouping_get_last_group_id()) {
		return get_last_property_groupinfo();
	}
	for (auto pnode = double_list_get_head(&pstore->group_list); pnode != nullptr;
	     pnode = double_list_get_after(&pstore->group_list, pnode)) {
		auto pgpinfo = static_cast<PROPERTY_GROUPINFO *>(pnode->pdata);
		if (pgpinfo->group_id == group_id) {
			return pgpinfo;
		}
	}
	auto pnode = me_alloc<DOUBLE_LIST_NODE>();
	if (NULL == pnode) {
		return NULL;
	}
	auto pgpinfo = msgchg_grouping_get_groupinfo(gnpwrap, pstore, group_id);
	if (NULL == pgpinfo) {
		free(pnode);
		return NULL;
	}
	pnode->pdata = pgpinfo;
	double_list_append_as_tail(&pstore->group_list, pnode);
	return pgpinfo;
}

static BOOL store_object_check_readonly_property(
	STORE_OBJECT *pstore, uint32_t proptag)
{
	if (PROP_TYPE(proptag) == PT_OBJECT)
		return TRUE;
	switch (proptag) {
	case PR_ACCESS:
	case PR_ACCESS_LEVEL:
	case PR_EMS_AB_DISPLAY_NAME_PRINTABLE:
	case PROP_TAG_CODEPAGEID:
	case PROP_TAG_CONTENTCOUNT:
	case PROP_TAG_DEFAULTSTORE:
	case PROP_TAG_DELETEAFTERSUBMIT:
	case PR_DELETED_ASSOC_MESSAGE_SIZE:
	case PR_DELETED_ASSOC_MESSAGE_SIZE_EXTENDED:
	case PR_DELETED_ASSOC_MSG_COUNT:
	case PR_DELETED_MESSAGE_SIZE:
	case PR_DELETED_MESSAGE_SIZE_EXTENDED:
	case PR_DELETED_MSG_COUNT:
	case PR_DELETED_NORMAL_MESSAGE_SIZE:
	case PR_DELETED_NORMAL_MESSAGE_SIZE_EXTENDED:
	case PR_EMAIL_ADDRESS:
	case PR_ENTRYID:
	case PROP_TAG_EXTENDEDRULESIZELIMIT:
	case PROP_TAG_INTERNETARTICLENUMBER:
	case PR_LOCALE_ID:
	case PR_MAPPING_SIGNATURE:
	case PR_MAX_SUBMIT_MESSAGE_SIZE:
	case PR_MAILBOX_OWNER_ENTRYID:
	case PR_MAILBOX_OWNER_NAME:
	case PR_MESSAGE_SIZE:
	case PR_MESSAGE_SIZE_EXTENDED:
	case PR_ASSOC_MESSAGE_SIZE:
	case PR_ASSOC_MESSAGE_SIZE_EXTENDED:
	case PR_NORMAL_MESSAGE_SIZE:
	case PROP_TAG_NORMALMESSAGESIZEEXTENDED:
	case PR_OBJECT_TYPE:
	case PROP_TAG_OUTOFOFFICESTATE:
	case PROP_TAG_PROHIBITRECEIVEQUOTA:
	case PROP_TAG_PROHIBITSENDQUOTA:
	case PROP_TAG_INSTANCEKEY:
	case PR_RECORD_KEY:
	case PR_RIGHTS:
	case PROP_TAG_SEARCHKEY:
	case PROP_TAG_SORTLOCALEID:
	case PROP_TAG_STORAGEQUOTALIMIT:
	case PR_STORE_ENTRYID:
	case PR_STORE_OFFLINE:
	case PR_MDB_PROVIDER:
	case PR_STORE_RECORD_KEY:
	case PR_STORE_STATE:
	case PR_STORE_SUPPORT_MASK:
	case PR_TEST_LINE_SPEED:
	case PR_USER_ENTRYID:
	case PROP_TAG_VALIDFOLDERMASK:
	case PROP_TAG_HIERARCHYSERVER:
	case PR_FINDER_ENTRYID:
	case PR_IPM_FAVORITES_ENTRYID:
	case PR_IPM_SUBTREE_ENTRYID:
	case PR_IPM_OUTBOX_ENTRYID:
	case PR_IPM_SENTMAIL_ENTRYID:
	case PR_IPM_WASTEBASKET_ENTRYID:
	case PR_SCHEDULE_FOLDER_ENTRYID:
	case PR_IPM_PUBLIC_FOLDERS_ENTRYID:
	case PR_NON_IPM_SUBTREE_ENTRYID:
	case PR_EFORMS_REGISTRY_ENTRYID:
		return TRUE;
	}
	return FALSE;
}

BOOL STORE_OBJECT::get_all_proptags(PROPTAG_ARRAY *pproptags)
{
	auto pstore = this;
	PROPTAG_ARRAY tmp_proptags;
	
	if (!exmdb_client::get_store_all_proptags(
		pstore->dir, &tmp_proptags)) {
		return FALSE;	
	}
	pproptags->pproptag = cu_alloc<uint32_t>(tmp_proptags.count + 50);
	if (NULL == pproptags->pproptag) {
		return FALSE;
	}
	memcpy(pproptags->pproptag, tmp_proptags.pproptag,
				sizeof(uint32_t)*tmp_proptags.count);
	pproptags->count = tmp_proptags.count;
	if (pstore->b_private) {
		pproptags->pproptag[pproptags->count] =
					PR_MAILBOX_OWNER_NAME;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count] =
				PR_MAILBOX_OWNER_ENTRYID;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count++] = PR_MAX_SUBMIT_MESSAGE_SIZE;
		pproptags->pproptag[pproptags->count] = PR_EMAIL_ADDRESS;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count] =
		PR_EMS_AB_DISPLAY_NAME_PRINTABLE;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count++] = PR_FINDER_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_IPM_OUTBOX_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_IPM_SENTMAIL_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_IPM_WASTEBASKET_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_SCHEDULE_FOLDER_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_OOF_STATE;
		pproptags->pproptag[pproptags->count++] = PR_EC_OUTOFOFFICE_MSG;
		pproptags->pproptag[pproptags->count++] = PR_EC_OUTOFOFFICE_SUBJECT;
		pproptags->pproptag[pproptags->count++] = PR_EC_OUTOFOFFICE_FROM;
		pproptags->pproptag[pproptags->count++] = PR_EC_OUTOFOFFICE_UNTIL;
		pproptags->pproptag[pproptags->count++] = PR_EC_ALLOW_EXTERNAL;
		pproptags->pproptag[pproptags->count++] = PR_EC_EXTERNAL_AUDIENCE;
		pproptags->pproptag[pproptags->count++] = PR_EC_EXTERNAL_REPLY;
		pproptags->pproptag[pproptags->count++] = PR_EC_EXTERNAL_SUBJECT;
	} else {
		pproptags->pproptag[pproptags->count] =
						PROP_TAG_HIERARCHYSERVER;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count++] = PR_IPM_PUBLIC_FOLDERS_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_NON_IPM_SUBTREE_ENTRYID;
		pproptags->pproptag[pproptags->count++] = PR_EFORMS_REGISTRY_ENTRYID;
		pproptags->count ++;
		/* TODO: For PR_EMAIL_ADDRESS,
		check if the mail address of a public folder exists. */
	}
	pproptags->pproptag[pproptags->count++] = PR_IPM_FAVORITES_ENTRYID;
	pproptags->pproptag[pproptags->count++] = PR_IPM_SUBTREE_ENTRYID;
	pproptags->pproptag[pproptags->count++] = PR_MDB_PROVIDER;
	pproptags->pproptag[pproptags->count] =
					PROP_TAG_DEFAULTSTORE;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count] = PR_DISPLAY_NAME;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count] =
			PROP_TAG_EXTENDEDRULESIZELIMIT;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_USER_ENTRYID;
	pproptags->pproptag[pproptags->count] =
					PROP_TAG_CONTENTCOUNT;
	pproptags->count ++;	
	pproptags->pproptag[pproptags->count] = PR_OBJECT_TYPE;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_PROVIDER_DISPLAY;
	pproptags->pproptag[pproptags->count++] = PR_RESOURCE_FLAGS;
	pproptags->pproptag[pproptags->count++] = PR_RESOURCE_TYPE;
	pproptags->pproptag[pproptags->count] = PR_RECORD_KEY;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count] =
						PROP_TAG_INSTANCEKEY;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_STORE_RECORD_KEY;
	pproptags->pproptag[pproptags->count++] = PR_MAPPING_SIGNATURE;
	pproptags->pproptag[pproptags->count] = PR_ENTRYID;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_STORE_ENTRYID;
	pproptags->pproptag[pproptags->count++] = PR_STORE_SUPPORT_MASK;
	pproptags->pproptag[pproptags->count] =
					PROP_TAG_ECSERVERVERSION;
	pproptags->count ++;
	return TRUE;
}

static void* store_object_get_oof_property(
	const char *maildir, uint32_t proptag)
{
	int fd;
	int offset;
	char *pbuff;
	int buff_len;
	void *pvalue;
	const char *str_value;
	char subject[1024];
	char temp_path[256];
	MIME_FIELD mime_field;
	struct stat node_stat;
	static constexpr uint8_t fake_true = true;
	static constexpr uint8_t fake_false = false;
	
	switch (proptag) {
	case PR_OOF_STATE: {
		pvalue = cu_alloc<uint32_t>();
		if (NULL == pvalue) {
			return NULL;
		}
		sprintf(temp_path, "%s/config/autoreply.cfg", maildir);
		auto pconfig = config_file_prg(nullptr, temp_path);
		if (NULL == pconfig) {
			*(uint32_t*)pvalue = 0;
		} else {
			str_value = pconfig->get_value("OOF_STATE");
			if (NULL == str_value) {
				*(uint32_t*)pvalue = 0;
			} else {
				*(uint32_t*)pvalue = atoi(str_value);
				if (*(uint32_t*)pvalue > 2) {
					*(uint32_t*)pvalue = 0;
				}
			}
		}
		return pvalue;
	}
	case PR_EC_OUTOFOFFICE_MSG:
	case PR_EC_EXTERNAL_REPLY:
		snprintf(temp_path, GX_ARRAY_SIZE(temp_path),
		         proptag == PR_EC_OUTOFOFFICE_MSG ?
		         "%s/config/internal-reply" : "%s/config/external-reply",
		         maildir);
		fd = open(temp_path, O_RDONLY);
		if (-1 == fd) {
			return NULL;
		}
		if (fstat(fd, &node_stat) != 0) {
			close(fd);
			return nullptr;
		}
		buff_len = node_stat.st_size;
		pbuff = cu_alloc<char>(buff_len + 1);
		if (NULL == pbuff) {
			close(fd);
			return NULL;
		}
		if (buff_len != read(fd, pbuff, buff_len)) {
			close(fd);
			return NULL;
		}
		close(fd);
		pbuff[buff_len] = '\0';
		return strstr(pbuff, "\r\n\r\n");
	case PR_EC_OUTOFOFFICE_SUBJECT:
	case PR_EC_EXTERNAL_SUBJECT: {
		snprintf(temp_path, GX_ARRAY_SIZE(temp_path),
		         proptag == PR_EC_OUTOFOFFICE_SUBJECT ?
		         "%s/config/internal-reply" : "%s/config/external-reply",
		         maildir);
		if (0 != stat(temp_path, &node_stat)) {
			return NULL;
		}
		buff_len = node_stat.st_size;
		fd = open(temp_path, O_RDONLY);
		if (-1 == fd) {
			return NULL;
		}
		pbuff = cu_alloc<char>(buff_len);
		if (NULL == pbuff) {
			close(fd);
			return NULL;
		}
		if (buff_len != read(fd, pbuff, buff_len)) {
			close(fd);
			return NULL;
		}
		close(fd);
		offset = 0;
		size_t parsed_length;
		while ((parsed_length = parse_mime_field(pbuff + offset, buff_len - offset, &mime_field)) != 0) {
			offset += parsed_length;
			if (0 == strncasecmp("Subject", mime_field.field_name, 7)
				&& mime_field.field_value_len < sizeof(subject)) {
				mime_field.field_value[mime_field.field_value_len] = '\0';
				if (TRUE == mime_string_to_utf8("utf-8",
					mime_field.field_value, subject)) {
					return common_util_dup(subject);
				}
			}
			if ('\r' == pbuff[offset] && '\n' == pbuff[offset + 1]) {
				return NULL;
			}
		}
		return NULL;
	}
	case PR_EC_OUTOFOFFICE_FROM:
	case PR_EC_OUTOFOFFICE_UNTIL: {
		sprintf(temp_path, "%s/config/autoreply.cfg", maildir);
		auto pconfig = config_file_prg(nullptr, temp_path);
		if (NULL == pconfig) {
			return NULL;
		}
		pvalue = cu_alloc<uint64_t>();
		if (NULL == pvalue) {
			return NULL;
		}
		str_value = pconfig->get_value(proptag == PR_EC_OUTOFOFFICE_FROM ? "START_TIME" : "END_TIME");
		if (NULL == str_value) {
			return NULL;
		}
		*(uint64_t*)pvalue = rop_util_unix_to_nttime(atoll(str_value));
		return pvalue;
	}
	case PR_EC_ALLOW_EXTERNAL:
	case PR_EC_EXTERNAL_AUDIENCE: {
		sprintf(temp_path, "%s/config/autoreply.cfg", maildir);
		auto pconfig = config_file_prg(nullptr, temp_path);
		if (NULL == pconfig) {
			return deconst(&fake_false);
		}
		str_value = pconfig->get_value(proptag == PR_EC_ALLOW_EXTERNAL ?
		            "ALLOW_EXTERNAL_OOF" : "EXTERNAL_AUDIENCE");
		pvalue = str_value == nullptr || atoi(str_value) == 0 ?
		         deconst(&fake_false) : deconst(&fake_true);
		return pvalue;
	}
	}
	return NULL;
}

static BOOL store_object_get_calculated_property(
	STORE_OBJECT *pstore, uint32_t proptag, void **ppvalue)
{
	int i;
	int temp_len;
	void *pvalue;
	uint32_t permission;
	char temp_buff[1024];
	static const uint8_t private_uid[] = {
		0x54, 0x94, 0xA1, 0xC0, 0x29, 0x7F, 0x10, 0x1B,
		0xA5, 0x87, 0x08, 0x00, 0x2B, 0x2A, 0x25, 0x17
	};
	static const uint8_t public_uid[] = {
		0x78, 0xB2, 0xFA, 0x70, 0xAF, 0xF7, 0x11, 0xCD,
		0x9B, 0xC8, 0x00, 0xAA, 0x00, 0x2F, 0xC4, 0x5A
	};
	static const uint8_t share_uid[] = {
		0x9E, 0xB4, 0x77, 0x00, 0x74, 0xE4, 0x11, 0xCE,
		0x8C, 0x5E, 0x00, 0xAA, 0x00, 0x42, 0x54, 0xE2
	};
	
	switch (proptag) {
	case PR_MDB_PROVIDER:
		*ppvalue = cu_alloc<BINARY>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		((BINARY*)*ppvalue)->cb = 16;
		static_cast<BINARY *>(*ppvalue)->pb = deconst(
			!pstore->b_private ? public_uid :
			pstore->check_owner_mode() ?
			private_uid : share_uid);
		return TRUE;
	case PR_DISPLAY_NAME:
		*ppvalue = common_util_alloc(256);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		if (TRUE == pstore->b_private) {
			if (!system_services_get_user_displayname(pstore->account,
			    static_cast<char *>(*ppvalue)))
				strcpy(static_cast<char *>(*ppvalue), pstore->account);
		} else {
			sprintf(static_cast<char *>(*ppvalue), "Public Folders - %s", pstore->account);
		}
		return TRUE;
	case PR_EMS_AB_DISPLAY_NAME_PRINTABLE:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_alloc(256);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		if (!system_services_get_user_displayname(pstore->account,
		    static_cast<char *>(*ppvalue)))
			return FALSE;	
		temp_len = strlen(static_cast<char *>(*ppvalue));
		for (i=0; i<temp_len; i++) {
			if (0 == isascii(((char*)(*ppvalue))[i])) {
				strcpy(static_cast<char *>(*ppvalue), pstore->account);
				pvalue = strchr(static_cast<char *>(*ppvalue), '@');
				*(char*)pvalue = '\0';
				break;
			}
		}
		return TRUE;
	case PROP_TAG_DEFAULTSTORE:
		*ppvalue = cu_alloc<uint8_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*static_cast<uint8_t *>(*ppvalue) = !!pstore->check_owner_mode();
		return TRUE;
	case PR_ACCESS: {
		*ppvalue = cu_alloc<uint8_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		if (pstore->check_owner_mode()) {
			*(uint32_t*)*ppvalue =
				TAG_ACCESS_MODIFY | TAG_ACCESS_READ |
				TAG_ACCESS_DELETE | TAG_ACCESS_HIERARCHY |
				TAG_ACCESS_CONTENTS | TAG_ACCESS_FAI_CONTENTS;
			return TRUE;
		}
		auto pinfo = zarafa_server_get_info();
		if (!pstore->b_private) {
			*static_cast<uint32_t *>(*ppvalue) =
				TAG_ACCESS_MODIFY | TAG_ACCESS_READ |
				TAG_ACCESS_DELETE | TAG_ACCESS_HIERARCHY |
				TAG_ACCESS_CONTENTS | TAG_ACCESS_FAI_CONTENTS;
			return TRUE;
		}
		if (!exmdb_client::check_mailbox_permission(pstore->dir,
		    pinfo->get_username(), &permission))
			return FALSE;
		permission &= ~frightsGromoxStoreOwner;
		*(uint32_t *)*ppvalue = TAG_ACCESS_READ;
		if (permission & frightsOwner) {
			*(uint32_t *)*ppvalue =
				TAG_ACCESS_MODIFY | TAG_ACCESS_DELETE |
				TAG_ACCESS_HIERARCHY | TAG_ACCESS_CONTENTS |
				TAG_ACCESS_FAI_CONTENTS;
			return TRUE;
		}
		if (permission & frightsCreate)
			*(uint32_t *)*ppvalue |= TAG_ACCESS_CONTENTS |
				TAG_ACCESS_FAI_CONTENTS;
		if (permission & frightsCreateSubfolder)
			*(uint32_t *)*ppvalue |= TAG_ACCESS_HIERARCHY;
		return TRUE;
	}
	case PR_RIGHTS: {
		*ppvalue = cu_alloc<uint8_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		if (pstore->check_owner_mode()) {
			*static_cast<uint32_t *>(*ppvalue) = rightsAll | frightsContact;
			return TRUE;
		}
		auto pinfo = zarafa_server_get_info();
		if (TRUE == pstore->b_private) {
			if (!exmdb_client::check_mailbox_permission(pstore->dir,
			    pinfo->get_username(), &permission))
				return FALSE;
			*static_cast<uint32_t *>(*ppvalue) &= ~(frightsGromoxSendAs | frightsGromoxStoreOwner);
			return TRUE;
		}
		*static_cast<uint32_t *>(*ppvalue) = rightsAll | frightsContact;
		return TRUE;
	}
	case PR_EMAIL_ADDRESS:
		if (TRUE == pstore->b_private) {
			if (!common_util_username_to_essdn(pstore->account,
			    temp_buff, GX_ARRAY_SIZE(temp_buff)))
				return FALSE;	
		} else {
			if (!common_util_public_to_essdn(pstore->account,
			    temp_buff, GX_ARRAY_SIZE(temp_buff)))
				return FALSE;	
		}
		*ppvalue = common_util_alloc(strlen(temp_buff) + 1);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		strcpy(static_cast<char *>(*ppvalue), temp_buff);
		return TRUE;
	case PROP_TAG_EXTENDEDRULESIZELIMIT:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*(uint32_t*)(*ppvalue) = common_util_get_param(
						COMMON_UTIL_MAX_EXTRULE_LENGTH);
		return TRUE;
	case PR_MAILBOX_OWNER_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_username_to_addressbook_entryid(
												pstore->account);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_MAILBOX_OWNER_NAME:
		if (!pstore->b_private)
			return FALSE;
		if (FALSE == system_services_get_user_displayname(
			pstore->account, temp_buff)) {
			return FALSE;	
		}
		if ('\0' == temp_buff[0]) {
			*ppvalue = common_util_alloc(strlen(pstore->account) + 1);
			if (NULL == *ppvalue) {
				return FALSE;
			}
			strcpy(static_cast<char *>(*ppvalue), pstore->account);
			return TRUE;
		}
		*ppvalue = common_util_alloc(strlen(temp_buff) + 1);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		strcpy(static_cast<char *>(*ppvalue), temp_buff);
		return TRUE;
	case PR_MAX_SUBMIT_MESSAGE_SIZE:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*(uint32_t*)(*ppvalue) = common_util_get_param(
							COMMON_UTIL_MAX_MAIL_LENGTH);
		return TRUE;
	case PR_OBJECT_TYPE:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*static_cast<uint32_t *>(*ppvalue) = MAPI_STORE;
		return TRUE;
	case PR_PROVIDER_DISPLAY:
		*ppvalue = deconst("Exchange Message Store");
		return TRUE;
	case PR_RESOURCE_FLAGS:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		if (pstore->check_owner_mode())
			*(uint32_t*)(*ppvalue) = STATUS_PRIMARY_IDENTITY|
					STATUS_DEFAULT_STORE|STATUS_PRIMARY_STORE;
		else
			*(uint32_t*)(*ppvalue) = STATUS_NO_DEFAULT_STORE;
		return TRUE;
	case PR_RESOURCE_TYPE:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*(uint32_t*)(*ppvalue) = MAPI_STORE_PROVIDER;
		return TRUE;
	case PR_STORE_SUPPORT_MASK: {
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		if (!pstore->b_private) {
			*static_cast<uint32_t *>(*ppvalue) = EC_SUPPORTMASK_PUBLIC;
			return TRUE;
		}
		if (pstore->check_owner_mode()) {
			*(uint32_t*)(*ppvalue) = EC_SUPPORTMASK_OWNER;
			return TRUE;
		}
		*(uint32_t *)(*ppvalue) = EC_SUPPORTMASK_OTHER;
		auto pinfo = zarafa_server_get_info();
		if (common_util_check_delegate_permission(pinfo->get_username(), pstore->dir))
			*(uint32_t *)(*ppvalue) |= STORE_SUBMIT_OK;
		return TRUE;
	}
	case PR_RECORD_KEY:
	case PROP_TAG_INSTANCEKEY:
	case PR_STORE_RECORD_KEY:
	case PR_MAPPING_SIGNATURE:
		*ppvalue = common_util_guid_to_binary(pstore->mailbox_guid);
		return TRUE;
	case PR_ENTRYID:
	case PR_STORE_ENTRYID:
		*ppvalue = common_util_to_store_entryid(pstore);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PROP_TAG_USERNAME: {
		auto pinfo = zarafa_server_get_info();
		*ppvalue = deconst(pinfo->get_username());
		return TRUE;
	}
	case PR_USER_ENTRYID: {
		auto pinfo = zarafa_server_get_info();
		*ppvalue = common_util_username_to_addressbook_entryid(pinfo->get_username());
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	}
	case PR_FINDER_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PRIVATE_FID_FINDER));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_IPM_FAVORITES_ENTRYID:
		*ppvalue = common_util_to_folder_entryid(pstore,
		           rop_util_make_eid_ex(1, pstore->b_private ?
		           PRIVATE_FID_SHORTCUTS : PUBLIC_FID_IPMSUBTREE));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_IPM_SUBTREE_ENTRYID:
		/* else case:: different from native MAPI */
		*ppvalue = common_util_to_folder_entryid(pstore, rop_util_make_eid_ex(1,
		           pstore->b_private ? PRIVATE_FID_IPMSUBTREE : PUBLIC_FID_IPMSUBTREE));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_IPM_OUTBOX_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PRIVATE_FID_OUTBOX));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_IPM_SENTMAIL_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PRIVATE_FID_SENT_ITEMS));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_IPM_WASTEBASKET_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PRIVATE_FID_DELETED_ITEMS));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_SCHEDULE_FOLDER_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PRIVATE_FID_SCHEDULE));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_COMMON_VIEWS_ENTRYID:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PRIVATE_FID_COMMON_VIEWS));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_IPM_PUBLIC_FOLDERS_ENTRYID:
	case PR_NON_IPM_SUBTREE_ENTRYID:
		if (pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PUBLIC_FID_NONIPMSUBTREE));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PR_EFORMS_REGISTRY_ENTRYID:
		if (pstore->b_private)
			return FALSE;
		*ppvalue = common_util_to_folder_entryid(pstore,
			rop_util_make_eid_ex(1, PUBLIC_FID_EFORMSREGISTRY));
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PROP_TAG_ECSERVERVERSION:
		*ppvalue = deconst(PROJECT_VERSION);
		return TRUE;
	case PR_OOF_STATE:
	case PR_EC_OUTOFOFFICE_MSG:
	case PR_EC_OUTOFOFFICE_SUBJECT:
	case PR_EC_OUTOFOFFICE_FROM:
	case PR_EC_OUTOFOFFICE_UNTIL:
	case PR_EC_ALLOW_EXTERNAL:
	case PR_EC_EXTERNAL_AUDIENCE:
	case PR_EC_EXTERNAL_REPLY:
	case PR_EC_EXTERNAL_SUBJECT:
		if (!pstore->b_private)
			return FALSE;
		*ppvalue = store_object_get_oof_property(pstore->get_dir(), proptag);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PROP_TAG_ECUSERLANGUAGE:
		if (!pstore->b_private)
			return FALSE;
		if (FALSE == system_services_get_user_lang(
			pstore->account, temp_buff) ||
			'\0' == temp_buff[0]) {
			return FALSE;	
		}
		HX_strlcat(temp_buff, ".UTF-8", sizeof(temp_buff));
		*ppvalue = common_util_dup(temp_buff);
		return TRUE;
	case PROP_TAG_ECUSERTIMEZONE:
		if (!pstore->b_private)
			return FALSE;
		if (FALSE == system_services_get_timezone(
			pstore->account, temp_buff) || '\0' ==
			temp_buff[0]) {
			*ppvalue = deconst(common_util_get_default_timezone());
			return TRUE;
		}
		*ppvalue = common_util_dup(temp_buff);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

BOOL STORE_OBJECT::get_properties(const PROPTAG_ARRAY *pproptags,
    TPROPVAL_ARRAY *ppropvals)
{
	int i;
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	if (NULL == ppropvals->ppropval) {
		return FALSE;
	}
	tmp_proptags.count = 0;
	tmp_proptags.pproptag = cu_alloc<uint32_t>(pproptags->count);
	if (NULL == tmp_proptags.pproptag) {
		return FALSE;
	}
	ppropvals->count = 0;
	auto pstore = this;
	for (i=0; i<pproptags->count; i++) {
		void *pvalue = nullptr;
		if (store_object_get_calculated_property(this,
		    pproptags->pproptag[i], &pvalue)) {
			if (NULL == pvalue) {
				return FALSE;
			}
			ppropvals->ppropval[ppropvals->count].proptag =
										pproptags->pproptag[i];
			ppropvals->ppropval[ppropvals->count].pvalue = pvalue;
			ppropvals->count ++;
		} else {
			tmp_proptags.pproptag[tmp_proptags.count] =
								pproptags->pproptag[i];
			tmp_proptags.count ++;
		}
	}
	if (0 == tmp_proptags.count) {
		return TRUE;
	}
	auto pinfo = zarafa_server_get_info();
	if (TRUE == pstore->b_private &&
		pinfo->user_id == pstore->account_id) {
		for (i=0; i<tmp_proptags.count; i++) {
			auto pvalue = pinfo->ptree->get_zstore_propval(tmp_proptags.pproptag[i]);
			if (pvalue == nullptr)
				continue;
			ppropvals->ppropval[ppropvals->count].proptag =
				tmp_proptags.pproptag[i];
			ppropvals->ppropval[ppropvals->count].pvalue = pvalue;
			ppropvals->count++;
			tmp_proptags.count--;
			if (i < tmp_proptags.count) {
				memmove(tmp_proptags.pproptag + i,
					tmp_proptags.pproptag + i + 1,
					sizeof(uint32_t) * (tmp_proptags.count - i));
			}
		}	
		if (0 == tmp_proptags.count) {
			return TRUE;
		}	
	}
	if (!exmdb_client::get_store_properties(
		pstore->dir, pinfo->cpid, &tmp_proptags,
		&tmp_propvals)) {
		return FALSE;	
	}
	if (0 == tmp_propvals.count) {
		return TRUE;
	}
	memcpy(ppropvals->ppropval +
		ppropvals->count, tmp_propvals.ppropval,
		sizeof(TAGGED_PROPVAL)*tmp_propvals.count);
	ppropvals->count += tmp_propvals.count;
	return TRUE;	
}

static BOOL store_object_set_oof_property(const char *maildir,
	uint32_t proptag, const void *pvalue)
{
	char *pbuff;
	int buff_len;
	char *ptoken;
	char temp_buff[64];
	std::string autoreply_path;
	
	try {
		autoreply_path = maildir + "/config/autoreply.cfg"s;
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1483: ENOMEM\n");
		return false;
	}
	/* Ensure file exists for config_file_prg */
	auto fd = open(autoreply_path.c_str(), O_CREAT | O_WRONLY, 0666);
	if (fd < 0)
		return false;
	close(fd);
	switch (proptag) {
	case PR_OOF_STATE: {
		auto pconfig = config_file_prg(nullptr, autoreply_path.c_str());
		if (NULL == pconfig) {
			return FALSE;
		}
		switch (*(uint32_t*)pvalue) {
		case 1:
			pconfig->set_value("OOF_STATE", "1");
			break;
		case 2:
			pconfig->set_value("OOF_STATE", "2");
			break;
		default:
			pconfig->set_value("OOF_STATE", "0");
			break;
		}
		return pconfig->save();
	}
	case PR_EC_OUTOFOFFICE_FROM:
	case PR_EC_OUTOFOFFICE_UNTIL: {
		auto pconfig = config_file_prg(nullptr, autoreply_path.c_str());
		if (NULL == pconfig) {
			return FALSE;
		}
		sprintf(temp_buff, "%lu", rop_util_nttime_to_unix(*(uint64_t*)pvalue));
		pconfig->set_value(proptag == PR_EC_OUTOFOFFICE_FROM ?
		                      "START_TIME" : "END_TIME", temp_buff);
		return pconfig->save();
	}
	case PR_EC_OUTOFOFFICE_MSG:
	case PR_EC_EXTERNAL_REPLY: {
		try {
			autoreply_path = maildir;
			autoreply_path += proptag == PR_EC_OUTOFOFFICE_MSG ?
			             "/config/internal-reply" : "/config/external-reply";
		} catch (const std::bad_alloc &) {
			fprintf(stderr, "E-1484: ENOMEM\n");
			return false;
		}
		wrapfd fd = open(autoreply_path.c_str(), O_RDONLY);
		struct stat node_stat;
		if (fd.get() < 0 || fstat(fd.get(), &node_stat) != 0) {
			buff_len = strlen(static_cast<const char *>(pvalue));
			pbuff = cu_alloc<char>(buff_len + 256);
			if (NULL == pbuff) {
				return FALSE;
			}
			buff_len = sprintf(pbuff, "Content-Type: text/html;\r\n"
			           "\tcharset=\"utf-8\"\r\n\r\n%s",
			           static_cast<const char *>(pvalue));
		} else {
			buff_len = node_stat.st_size;
			pbuff = cu_alloc<char>(buff_len + strlen(static_cast<const char *>(pvalue)) + 1);
			if (pbuff == nullptr || read(fd.get(), pbuff, buff_len) != buff_len)
				return FALSE;
			pbuff[buff_len] = '\0';
			ptoken = strstr(pbuff, "\r\n\r\n");
			if (NULL != ptoken) {
				strcpy(ptoken + 4, static_cast<const char *>(pvalue));
				buff_len = strlen(pbuff);
			} else {
				buff_len = sprintf(pbuff, "Content-Type: text/html;\r\n"
				           "\tcharset=\"utf-8\"\r\n\r\n%s",
				           static_cast<const char *>(pvalue));
			}
		}
		fd = open(autoreply_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
		if (fd.get() < 0 || write(fd.get(), pbuff, buff_len) != buff_len)
			return FALSE;
		return TRUE;
	}
	case PR_EC_OUTOFOFFICE_SUBJECT:
	case PR_EC_EXTERNAL_SUBJECT: {
		try {
			autoreply_path = maildir;
			autoreply_path += proptag == PR_EC_OUTOFOFFICE_MSG ?
			             "/config/internal-reply" : "/config/external-reply";
		} catch (const std::bad_alloc &) {
			fprintf(stderr, "E-1485: ENOMEM\n");
			return false;
		}
		struct stat node_stat;
		if (stat(autoreply_path.c_str(), &node_stat) != 0) {
			buff_len = strlen(static_cast<const char *>(pvalue));
			pbuff = cu_alloc<char>(buff_len + 256);
			if (NULL == pbuff) {
				return FALSE;
			}
			buff_len = sprintf(pbuff, "Content-Type: text/html;\r\n\t"
			           "charset=\"utf-8\"\r\nSubject: %s\r\n\r\n",
			           static_cast<const char *>(pvalue));
		} else {
			buff_len = node_stat.st_size;
			pbuff = cu_alloc<char>(buff_len + strlen(static_cast<const char *>(pvalue)) + 16);
			if (NULL == pbuff) {
				return FALSE;
			}
			ptoken = cu_alloc<char>(buff_len + 1);
			if (NULL == ptoken) {
				return FALSE;
			}
			auto fd = open(autoreply_path.c_str(), O_RDONLY);
			if (-1 == fd) {
				return FALSE;
			}
			if (buff_len != read(fd, ptoken, buff_len)) {
				close(fd);
				return FALSE;
			}
			close(fd);
			ptoken[buff_len] = '\0';
			ptoken = strstr(ptoken, "\r\n\r\n");
			if (NULL == ptoken) {
				buff_len = sprintf(pbuff, "Content-Type: text/html;\r\n\t"
				           "charset=\"utf-8\"\r\nSubject: %s\r\n\r\n",
				           static_cast<const char *>(pvalue));
			} else {
				buff_len = sprintf(pbuff, "Content-Type: text/html;\r\n\t"
				           "charset=\"utf-8\"\r\nSubject: %s%s",
				           static_cast<const char *>(pvalue), ptoken);
			}
		}
		auto fd = open(autoreply_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
		if (-1 == fd) {
			return FALSE;
		}
		if (buff_len != write(fd, pbuff, buff_len)) {
			close(fd);
			return FALSE;
		}
		close(fd);
		return TRUE;
	}
	case PR_EC_ALLOW_EXTERNAL:
	case PR_EC_EXTERNAL_AUDIENCE: {
		auto pconfig = config_file_prg(nullptr, autoreply_path.c_str());
		if (NULL == pconfig) {
			return FALSE;
		}
		pconfig->set_value(proptag == PR_EC_ALLOW_EXTERNAL ?
		                      "ALLOW_EXTERNAL_OOF" : "EXTERNAL_AUDIENCE",
		                      *static_cast<const uint8_t *>(pvalue) == 0 ? "0" : "1");
		return pconfig->save();
	}
	}
	return FALSE;
}

static BOOL store_object_set_folder_name(STORE_OBJECT *pstore,
	uint64_t fid_val, const char *pdisplayname)
{
	XID tmp_xid;
	BINARY *pbin_pcl;
	uint64_t folder_id;
	uint64_t last_time;
	uint64_t change_num;
	BINARY *pbin_changekey;
	PROBLEM_ARRAY tmp_problems;
	TPROPVAL_ARRAY tmp_propvals;
	TAGGED_PROPVAL propval_buff[5];
	
	if (!pstore->b_private)
		return FALSE;
	folder_id = rop_util_make_eid_ex(1, fid_val);
	tmp_propvals.ppropval = propval_buff;
	tmp_propvals.count = 5;
	tmp_propvals.ppropval[0].proptag = PR_DISPLAY_NAME;
	tmp_propvals.ppropval[0].pvalue = deconst(pdisplayname);
	if (!exmdb_client::allocate_cn(pstore->dir, &change_num)) {
		return FALSE;
	}
	tmp_propvals.ppropval[1].proptag = PROP_TAG_CHANGENUMBER;
	tmp_propvals.ppropval[1].pvalue = &change_num;
	if (!exmdb_client_get_folder_property(pstore->dir, 0, folder_id,
	    PR_PREDECESSOR_CHANGE_LIST, reinterpret_cast<void **>(&pbin_pcl)) ||
	    pbin_pcl == nullptr)
		return FALSE;
	tmp_xid.guid = rop_util_make_user_guid(pstore->account_id);
	rop_util_get_gc_array(change_num, tmp_xid.local_id);
	pbin_changekey = common_util_xid_to_binary(22, &tmp_xid);
	if (NULL == pbin_changekey) {
		return FALSE;
	}
	pbin_pcl = common_util_pcl_append(pbin_pcl, pbin_changekey);
	if (NULL == pbin_pcl) {
		return FALSE;
	}
	last_time = rop_util_current_nttime();
	tmp_propvals.ppropval[2].proptag = PR_CHANGE_KEY;
	tmp_propvals.ppropval[2].pvalue = pbin_changekey;
	tmp_propvals.ppropval[3].proptag = PR_PREDECESSOR_CHANGE_LIST;
	tmp_propvals.ppropval[3].pvalue = pbin_pcl;
	tmp_propvals.ppropval[4].proptag = PR_LAST_MODIFICATION_TIME;
	tmp_propvals.ppropval[4].pvalue = &last_time;
	return exmdb_client::set_folder_properties(
		pstore->dir, 0, folder_id, &tmp_propvals,
		&tmp_problems);
}

/**
 * @locale:	input string like "en_US.UTF-8"
 */
static void set_store_lang(STORE_OBJECT *store, const char *locale)
{
	/*
	 * If Offline Mode happens to write this prop even though it is
	 * unchanged, it may appear as if folder names have reset.
	 */
	if (!store->b_private)
		return;
	auto lang = common_util_i18n_to_lang(locale);
	if (lang == nullptr) {
		fprintf(stderr, "W-1506: %s requested to set folder names to %s, but this language is unknown.\n",
		        store->account, locale);
	} else {
		char *fnam[RES_TOTAL_NUM];
		common_util_get_folder_lang(lang, fnam);
		store_object_set_folder_name(store, PRIVATE_FID_IPMSUBTREE, fnam[RES_ID_IPM]);
		store_object_set_folder_name(store, PRIVATE_FID_INBOX, fnam[RES_ID_INBOX]);
		store_object_set_folder_name(store, PRIVATE_FID_DRAFT, fnam[RES_ID_DRAFT]);
		store_object_set_folder_name(store, PRIVATE_FID_OUTBOX, fnam[RES_ID_OUTBOX]);
		store_object_set_folder_name(store, PRIVATE_FID_SENT_ITEMS, fnam[RES_ID_SENT]);
		store_object_set_folder_name(store, PRIVATE_FID_DELETED_ITEMS, fnam[RES_ID_DELETED]);
		store_object_set_folder_name(store, PRIVATE_FID_CONTACTS, fnam[RES_ID_CONTACTS]);
		store_object_set_folder_name(store, PRIVATE_FID_CALENDAR, fnam[RES_ID_CALENDAR]);
		store_object_set_folder_name(store, PRIVATE_FID_JOURNAL, fnam[RES_ID_JOURNAL]);
		store_object_set_folder_name(store, PRIVATE_FID_NOTES, fnam[RES_ID_NOTES]);
		store_object_set_folder_name(store, PRIVATE_FID_TASKS, fnam[RES_ID_TASKS]);
		store_object_set_folder_name(store, PRIVATE_FID_JUNK, fnam[RES_ID_JUNK]);
		store_object_set_folder_name(store, PRIVATE_FID_SYNC_ISSUES, fnam[RES_ID_SYNC]);
		store_object_set_folder_name(store, PRIVATE_FID_CONFLICTS, fnam[RES_ID_CONFLICT]);
		store_object_set_folder_name(store, PRIVATE_FID_LOCAL_FAILURES, fnam[RES_ID_LOCAL]);
		store_object_set_folder_name(store, PRIVATE_FID_SERVER_FAILURES, fnam[RES_ID_SERVER]);
	}

	char mloc[32];
	gx_strlcpy(mloc, locale, arsizeof(mloc));
	auto p = strchr(mloc, '.');
	if (p != nullptr)
		*p = '\0';
	p = strchr(mloc, '@');
	if (p != nullptr)
		*p = '\0';
	system_services_set_user_lang(store->account, mloc);
}

BOOL STORE_OBJECT::set_properties(const TPROPVAL_ARRAY *ppropvals)
{
	int i;
	auto pinfo = zarafa_server_get_info();
	auto pstore = this;
	if (FALSE == pstore->b_private ||
		pinfo->user_id != pstore->account_id) {
		return TRUE;
	}
	for (i=0; i<ppropvals->count; i++) {
		if (TRUE == store_object_check_readonly_property(
			pstore, ppropvals->ppropval[i].proptag)) {
			continue;
		}
		switch (ppropvals->ppropval[i].proptag) {
		case PR_OOF_STATE:
		case PR_EC_OUTOFOFFICE_FROM:
		case PR_EC_OUTOFOFFICE_UNTIL:
		case PR_EC_OUTOFOFFICE_MSG:
		case PR_EC_OUTOFOFFICE_SUBJECT:
		case PR_EC_ALLOW_EXTERNAL:
		case PR_EC_EXTERNAL_AUDIENCE:
		case PR_EC_EXTERNAL_SUBJECT:
		case PR_EC_EXTERNAL_REPLY:
			if (!store_object_set_oof_property(pstore->get_dir(),
			    ppropvals->ppropval[i].proptag,
			    ppropvals->ppropval[i].pvalue))
				return FALSE;	
			break;
		case PROP_TAG_ECUSERLANGUAGE:
			set_store_lang(pstore, static_cast<char *>(ppropvals->ppropval[i].pvalue));
			break;
		case PROP_TAG_ECUSERTIMEZONE:
			if (pstore->b_private)
				system_services_set_timezone(pstore->account,
					static_cast<char *>(ppropvals->ppropval[i].pvalue));
			break;
		case PROP_TAG_THUMBNAILPHOTO: {
			if (!pstore->b_private)
				break;
			int fd = -1;
			try {
				auto pic_path = pstore->dir + "/config/portrait.jpg"s;
				fd = open(pic_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
			} catch (const std::bad_alloc &) {
				fprintf(stderr, "E-1494: ENOMEM\n");
			}
			if (-1 != fd) {
				write(fd, ((BINARY *)ppropvals->ppropval[i].pvalue)->pb,
					((BINARY *)ppropvals->ppropval[i].pvalue)->cb);
				close(fd);
			}
			break;
		}
		default:
			if (!pinfo->ptree->set_zstore_propval(&ppropvals->ppropval[i]))
				return FALSE;	
			break;
		}
	}
	return TRUE;
}

BOOL STORE_OBJECT::remove_properties(const PROPTAG_ARRAY *pproptags)
{
	auto pstore = this;
	int i;
	auto pinfo = zarafa_server_get_info();
	if (FALSE == pstore->b_private ||
		pinfo->user_id != pstore->account_id) {
		return TRUE;
	}
	for (i=0; i<pproptags->count; i++) {
		if (TRUE == store_object_check_readonly_property(
			pstore, pproptags->pproptag[i])) {
			continue;
		}
		pinfo->ptree->remove_zstore_propval(pproptags->pproptag[i]);
	}
	return TRUE;
}

static BOOL store_object_get_folder_permissions(
	STORE_OBJECT *pstore, uint64_t folder_id,
	PERMISSION_SET *pperm_set)
{
	BINARY *pentryid;
	uint32_t row_num;
	uint32_t table_id;
	uint32_t *prights;
	uint32_t max_count;
	PROPTAG_ARRAY proptags;
	TARRAY_SET permission_set;
	PERMISSION_ROW *pperm_row;
	static const uint32_t proptag_buff[] = {
		PR_ENTRYID,
		PROP_TAG_MEMBERRIGHTS
	};
	
	if (!exmdb_client::load_permission_table(
		pstore->dir, folder_id, 0, &table_id, &row_num)) {
		return FALSE;
	}
	proptags.count = 2;
	proptags.pproptag = deconst(proptag_buff);
	if (!exmdb_client::query_table(pstore->dir, NULL,
		0, table_id, &proptags, 0, row_num, &permission_set)) {
		exmdb_client::unload_table(pstore->dir, table_id);
		return FALSE;
	}
	exmdb_client::unload_table(pstore->dir, table_id);
	max_count = (pperm_set->count/100)*100;
	for (size_t i = 0; i < permission_set.count; ++i) {
		if (max_count == pperm_set->count) {
			max_count += 100;
			pperm_row = cu_alloc<PERMISSION_ROW>(max_count);
			if (NULL == pperm_row) {
				return FALSE;
			}
			if (0 != pperm_set->count) {
				memcpy(pperm_row, pperm_set->prows,
					sizeof(PERMISSION_ROW)*pperm_set->count);
			}
			pperm_set->prows = pperm_row;
		}
		pentryid = static_cast<BINARY *>(common_util_get_propvals(
		           permission_set.pparray[i], PR_ENTRYID));
		/* ignore the default and anonymous user */
		if (NULL == pentryid || 0 == pentryid->cb) {
			continue;
		}
		size_t j;
		for (j=0; j<pperm_set->count; j++) {
			if (pperm_set->prows[j].entryid.cb ==
				pentryid->cb && 0 == memcmp(
				pperm_set->prows[j].entryid.pb,
				pentryid->pb, pentryid->cb)) {
				break;	
			}
		}
		prights = static_cast<uint32_t *>(common_util_get_propvals(
				permission_set.pparray[i],
		          PROP_TAG_MEMBERRIGHTS));
		if (NULL == prights) {
			continue;
		}
		if (j < pperm_set->count) {
			pperm_set->prows[j].member_rights |= *prights;
			continue;
		}
		pperm_set->prows[pperm_set->count].flags = RIGHT_NORMAL;
		pperm_set->prows[pperm_set->count].entryid = *pentryid;
		pperm_set->prows[pperm_set->count].member_rights = *prights;
		pperm_set->count ++;
	}
	return TRUE;
}

BOOL STORE_OBJECT::get_permissions(PERMISSION_SET *pperm_set)
{
	auto pstore = this;
	uint32_t row_num;
	uint32_t table_id;
	TARRAY_SET tmp_set;
	uint32_t tmp_proptag;
	PROPTAG_ARRAY proptags;
	uint64_t folder_id = rop_util_make_eid_ex(1, pstore->b_private ?
	                     PRIVATE_FID_IPMSUBTREE : PUBLIC_FID_IPMSUBTREE);
	
	if (!exmdb_client::load_hierarchy_table(
		pstore->dir, folder_id, NULL, TABLE_FLAG_DEPTH,
		NULL, &table_id, &row_num)) {
		return FALSE;
	}
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PROP_TAG_FOLDERID;
	if (!exmdb_client::query_table(pstore->dir, NULL,
		0, table_id, &proptags, 0, row_num, &tmp_set)) {
		return FALSE;
	}
	pperm_set->count = 0;
	pperm_set->prows = NULL;
	for (size_t i = 0; i < tmp_set.count; ++i) {
		if (0 == tmp_set.pparray[i]->count) {
			continue;
		}
		if (!store_object_get_folder_permissions(this,
		    *static_cast<uint64_t *>(tmp_set.pparray[i]->ppropval[0].pvalue), pperm_set))
			return FALSE;	
	}
	return TRUE;
}
