// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <cstdint>
#include <memory>
#include <gromox/defs.h>
#include <gromox/mapidefs.h>
#include "attachment_object.h"
#include "system_services.h"
#include "message_object.h"
#include <gromox/tpropval_array.hpp>
#include "zarafa_server.h"
#include <gromox/proptag_array.hpp>
#include "store_object.h"
#include "exmdb_client.h"
#include "common_util.h"
#include <gromox/ext_buffer.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/idset.hpp>
#include <gromox/guid.hpp>
#include <gromox/util.hpp>
#include <gromox/pcl.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static BOOL message_object_set_properties_internal(
	MESSAGE_OBJECT *pmessage, BOOL b_check,
	const TPROPVAL_ARRAY *ppropvals);

BOOL MESSAGE_OBJECT::get_recipient_all_proptags(PROPTAG_ARRAY *pproptags)
{
	auto pmessage = this;
	return exmdb_client::get_message_instance_rcpts_all_proptags(
	       pmessage->pstore->get_dir(), pmessage->instance_id, pproptags);
}

std::unique_ptr<MESSAGE_OBJECT> message_object_create(STORE_OBJECT *pstore,
	BOOL b_new, uint32_t cpid, uint64_t message_id,
	void *pparent, uint32_t tag_access, BOOL b_writable,
	ICS_STATE *pstate)
{
	uint64_t *pchange_num;
	std::unique_ptr<MESSAGE_OBJECT> pmessage;
	try {
		pmessage = std::make_unique<MESSAGE_OBJECT>();
	} catch (const std::bad_alloc &) {
		return NULL;
	}
	pmessage->pstore = pstore;
	pmessage->b_new = b_new;
	pmessage->cpid = cpid;
	pmessage->message_id = message_id;
	pmessage->tag_access = tag_access;
	pmessage->b_writable = b_writable;
	pmessage->pstate = pstate;
	if (0 == message_id) {
		pmessage->pembedding = static_cast<ATTACHMENT_OBJECT *>(pparent);
		if (!exmdb_client::load_embedded_instance(pstore->get_dir(),
		    b_new, static_cast<ATTACHMENT_OBJECT *>(pparent)->instance_id,
		    &pmessage->instance_id))
			return NULL;
		/* cannot find embedded message in attachment, return
			immediately to caller and the caller check the
			result by calling message_object_get_instance_id */
		if (!b_new && pmessage->instance_id == 0)
			return pmessage;
	} else {
		pmessage->folder_id = *(uint64_t*)pparent;
		if (pmessage->pstore->b_private) {
			if (!exmdb_client::load_message_instance(pstore->get_dir(),
			    nullptr, cpid, b_new, pmessage->folder_id, message_id,
			    &pmessage->instance_id))
				return NULL;
		} else {
			auto pinfo = zarafa_server_get_info();
			if (!exmdb_client::load_message_instance(pstore->get_dir(),
			    pinfo->get_username(), cpid, b_new, pmessage->folder_id,
			    message_id, &pmessage->instance_id))
				return NULL;
		}
	}
	if (0 == pmessage->instance_id) {
		return NULL;
	}
	pmessage->pchanged_proptags = proptag_array_init();
	if (NULL == pmessage->pchanged_proptags) {
		return NULL;
	}
	pmessage->premoved_proptags = proptag_array_init();
	if (NULL == pmessage->premoved_proptags) {
		return NULL;
	}
	if (FALSE == b_new) {
		if (!exmdb_client_get_instance_property(pstore->get_dir(),
		    pmessage->instance_id, PROP_TAG_CHANGENUMBER,
		    reinterpret_cast<void **>(&pchange_num)))
			return NULL;
		if (NULL != pchange_num) {
			pmessage->change_num = *pchange_num;
		}
	}
	return pmessage;
}

BOOL MESSAGE_OBJECT::check_orignal_touched(BOOL *pb_touched)
{
	auto pmessage = this;
	uint64_t *pchange_num;
	
	if (TRUE == pmessage->b_new) {
		*pb_touched = FALSE;
		return TRUE;
	}
	if (0 != pmessage->message_id) {
		if (!exmdb_client_get_message_property(pmessage->pstore->get_dir(),
		    nullptr, 0, pmessage->message_id, PROP_TAG_CHANGENUMBER,
		    reinterpret_cast<void **>(&pchange_num)))
			return FALSE;
	} else {
		if (!exmdb_client::get_embedded_cn(pmessage->pstore->get_dir(),
		    pmessage->instance_id, &pchange_num))
			return FALSE;	
	}
	/* if it cannot find PROP_TAG_CHANGENUMBER, it means message does not exist any more */
	*pb_touched = pchange_num == nullptr || *pchange_num != pmessage->change_num ? TRUE : false;
	return TRUE;
}

MESSAGE_OBJECT::~MESSAGE_OBJECT()
{	
	auto pmessage = this;
	if (0 != pmessage->instance_id) { 
		exmdb_client::unload_instance(pmessage->pstore->get_dir(),
			pmessage->instance_id);
	}
	if (NULL != pmessage->pchanged_proptags) {
		proptag_array_free(pmessage->pchanged_proptags);
	}
	if (NULL != pmessage->premoved_proptags) {
		proptag_array_free(pmessage->premoved_proptags);
	}
}

BOOL MESSAGE_OBJECT::init_message(BOOL b_fai, uint32_t new_cpid)
{
	auto pmessage = this;
	void *pvalue;
	GUID tmp_guid;
	EXT_PUSH ext_push;
	char id_string[256];
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;
	
	if (!pmessage->b_new)
		return FALSE;
	propvals.count = 0;
	propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(20);
	if (NULL == propvals.ppropval) {
		return FALSE;
	}
	
	propvals.ppropval[propvals.count].proptag = PR_MESSAGE_CODEPAGE;
	pvalue = cu_alloc<uint32_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*static_cast<uint32_t *>(pvalue) = new_cpid;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_IMPORTANCE;
	pvalue = cu_alloc<uint32_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint32_t*)pvalue = 1;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_MESSAGECLASS;
	propvals.ppropval[propvals.count].pvalue  = deconst("IPM.Note");
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_SENSITIVITY;
	pvalue = cu_alloc<uint32_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint32_t*)pvalue = 0;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
					PROP_TAG_ORIGINALDISPLAYBCC;
	propvals.ppropval[propvals.count].pvalue  = deconst("");
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
					PROP_TAG_ORIGINALDISPLAYCC;
	propvals.ppropval[propvals.count].pvalue  = deconst("");
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
					PROP_TAG_ORIGINALDISPLAYTO;
	propvals.ppropval[propvals.count].pvalue  = deconst("");
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag = PR_MESSAGE_FLAGS;
	pvalue = cu_alloc<uint32_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*static_cast<uint32_t *>(pvalue) = MSGFLAG_UNSENT | MSGFLAG_UNMODIFIED;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag = PR_READ;
	pvalue = cu_alloc<uint8_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint8_t*)pvalue = 1;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag = PR_ASSOCIATED;
	pvalue = cu_alloc<uint8_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*static_cast<uint8_t *>(pvalue) = !!b_fai;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_TRUSTSENDER;
	pvalue = cu_alloc<uint32_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint32_t*)pvalue = 1;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag = PR_CREATION_TIME;
	pvalue = cu_alloc<uint64_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint64_t*)pvalue = rop_util_current_nttime();
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_SEARCHKEY;
	pvalue = cu_alloc<BINARY>();
	if (NULL == pvalue) {
		return FALSE;
	}
	
	pvalue = common_util_guid_to_binary(guid_random_new());
	if (NULL == pvalue) {
		return FALSE;
	}
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag = PR_MESSAGE_LOCALE_ID;
	pvalue = cu_alloc<uint32_t>();
	if (NULL == pvalue) {
		return FALSE;
	}
	*(uint32_t*)pvalue = 0x0409;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	propvals.ppropval[propvals.count].proptag = PR_LOCALE_ID;
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
							PROP_TAG_CREATORNAME;
	pvalue = common_util_alloc(1024);
	if (NULL == pvalue) {
		return FALSE;
	}
	auto pinfo = zarafa_server_get_info();
	if (!system_services_get_user_displayname(pinfo->get_username(),
	    static_cast<char *>(pvalue)) ||
	    *static_cast<char *>(pvalue) == '\0')
		strcpy(static_cast<char *>(pvalue), pinfo->get_username());
	propvals.ppropval[propvals.count].pvalue = pvalue;
	propvals.count ++;
	
	propvals.ppropval[propvals.count].proptag =
						PROP_TAG_CREATORENTRYID;
	pvalue = common_util_username_to_addressbook_entryid(pinfo->get_username());
	if (NULL == pvalue) {
		return FALSE;
	}
	propvals.ppropval[propvals.count].pvalue = pvalue;	
	propvals.count ++;
	
	tmp_guid = guid_random_new();
	if (!ext_push.init(id_string, 256, 0) ||
	    ext_push.p_guid(&tmp_guid) != EXT_ERR_SUCCESS)
		return false;
	encode_hex_binary(id_string, 16, id_string + 16, 64);
	id_string[0] = '<';
	memmove(id_string + 1, id_string + 16, 32);
	snprintf(id_string + 33, 128, "@%s>", common_util_get_hostname());
	propvals.ppropval[propvals.count].proptag =
					PROP_TAG_INTERNETMESSAGEID;
	propvals.ppropval[propvals.count].pvalue = id_string;
	propvals.count ++;
	
	if (!exmdb_client::set_instance_properties(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &propvals, &problems))
		return FALSE;	
	pmessage->b_touched = TRUE;
	return TRUE;
}

gxerr_t MESSAGE_OBJECT::save()
{
	auto pmessage = this;
	int i;
	XID tmp_xid;
	void *pvalue;
	uint32_t result;
	BINARY *pbin_pcl;
	uint32_t tmp_index;
	uint32_t *pgroup_id;
	INDEX_ARRAY *pindices;
	BINARY *pbin_changekey;
	INDEX_ARRAY tmp_indices;
	TAGGED_PROPVAL tmp_propval;
	TPROPVAL_ARRAY tmp_propvals;
	PROPERTY_GROUPINFO *pgpinfo;
	PROPTAG_ARRAY *pungroup_proptags;
	
	
	if (FALSE == pmessage->b_new &&
		FALSE == pmessage->b_touched) {
		return GXERR_SUCCESS;
	}
	auto dir = pmessage->pstore->get_dir();
	auto pinfo = zarafa_server_get_info();
	if (!exmdb_client::allocate_cn(
		dir, &pmessage->change_num)) {
		return GXERR_CALL_FAILED;
	}
	if (!exmdb_client_get_instance_property(dir, pmessage->instance_id,
	    PR_ASSOCIATED, &pvalue))
		return GXERR_CALL_FAILED;
	BOOL b_fai = pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0 ? false : TRUE;
	tmp_propvals.count = 0;
	tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(8);
	if (NULL == tmp_propvals.ppropval) {
		return GXERR_CALL_FAILED;
	}
	
	tmp_propvals.ppropval[tmp_propvals.count].proptag =
								PROP_TAG_LOCALCOMMITTIME;
	pvalue = cu_alloc<uint64_t>();
	if (NULL == pvalue) {
		return GXERR_CALL_FAILED;
	}
	*(uint64_t*)pvalue = rop_util_current_nttime();
	tmp_propvals.ppropval[tmp_propvals.count].pvalue = pvalue;
	tmp_propvals.count ++;
	
	if (!proptag_array_check(pmessage->pchanged_proptags,
	    PR_LAST_MODIFICATION_TIME)) {
		tmp_propvals.ppropval[tmp_propvals.count].proptag = PR_LAST_MODIFICATION_TIME;
		tmp_propvals.ppropval[tmp_propvals.count].pvalue = pvalue;
		tmp_propvals.count ++;
	}
	
	if (!proptag_array_check(pmessage->pchanged_proptags,
	    PROP_TAG_LASTMODIFIERNAME)) {
		tmp_propvals.ppropval[tmp_propvals.count].proptag =
									PROP_TAG_LASTMODIFIERNAME;
		pvalue = common_util_alloc(1024);
		if (NULL == pvalue) {
			return GXERR_CALL_FAILED;
		}
		if (!system_services_get_user_displayname(pinfo->get_username(),
		    static_cast<char *>(pvalue)) ||
		    *static_cast<char *>(pvalue) == '\0')
			strcpy(static_cast<char *>(pvalue), pinfo->get_username());
		tmp_propvals.ppropval[tmp_propvals.count].pvalue = pvalue;
		tmp_propvals.count ++;
	}
	
	tmp_propvals.ppropval[tmp_propvals.count].proptag =
							PROP_TAG_LASTMODIFIERENTRYID;
	pvalue = common_util_username_to_addressbook_entryid(pinfo->get_username());
	if (NULL == pvalue) {
		return GXERR_CALL_FAILED;
	}
	tmp_propvals.ppropval[tmp_propvals.count].pvalue = pvalue;
	tmp_propvals.count ++;
	
	if (0 != pmessage->message_id) {
		if (!exmdb_client_get_instance_property(dir,
		    pmessage->instance_id, PR_PREDECESSOR_CHANGE_LIST,
		    reinterpret_cast<void **>(&pbin_pcl)))
			return GXERR_CALL_FAILED;
		if (!pmessage->b_new && pbin_pcl == nullptr)
			return GXERR_CALL_FAILED;
		tmp_xid.guid = pmessage->pstore->guid();
		rop_util_get_gc_array(pmessage->change_num, tmp_xid.local_id);
		tmp_propvals.ppropval[tmp_propvals.count].proptag = PR_CHANGE_KEY;
		pbin_changekey = common_util_xid_to_binary(22, &tmp_xid);
		if (NULL == pbin_changekey) {
			return GXERR_CALL_FAILED;
		}
		tmp_propvals.ppropval[tmp_propvals.count].pvalue =
											pbin_changekey;
		tmp_propvals.count ++;
		
		pbin_pcl = common_util_pcl_append(pbin_pcl, pbin_changekey);
		if (NULL == pbin_pcl) {
			return GXERR_CALL_FAILED;
		}
		tmp_propvals.ppropval[tmp_propvals.count].proptag = PR_PREDECESSOR_CHANGE_LIST;
		tmp_propvals.ppropval[tmp_propvals.count].pvalue = pbin_pcl;	
		tmp_propvals.count ++;
	}
	
	if (FALSE == message_object_set_properties_internal(
		pmessage, FALSE, &tmp_propvals)) {
		return GXERR_CALL_FAILED;
	}
	
	/* change number of embedding message is used for message
		modification's check when the rop_savechangesmessage
		is called, it is useless for ICS!
	*/
	tmp_propval.proptag = PROP_TAG_CHANGENUMBER;
	tmp_propval.pvalue = &pmessage->change_num;
	if (FALSE == exmdb_client_set_instance_property(dir,
		pmessage->instance_id, &tmp_propval, &result)) {
		return GXERR_CALL_FAILED;
	}
	
	gxerr_t e_result = GXERR_CALL_FAILED;
	if (!exmdb_client::flush_instance(dir, pmessage->instance_id,
	    pmessage->pstore->get_account(), &e_result) ||
	    e_result != GXERR_SUCCESS)
		return e_result;

	auto is_new = pmessage->b_new;
	pmessage->b_new = FALSE;
	pmessage->b_touched = FALSE;
	if (0 == pmessage->message_id) {
		pmessage->pembedding->b_touched = TRUE;
		return GXERR_SUCCESS;
	}
	
	if (NULL != pmessage->pstate) {
		idset_append(pmessage->pstate->pgiven,
						pmessage->message_id);
		if (FALSE == b_fai) {
			idset_append(pmessage->pstate->pseen,
							pmessage->change_num);
		} else {
			idset_append(pmessage->pstate->pseen_fai,
								pmessage->change_num);
		}
	}
	
	if (0 == pmessage->message_id || TRUE == b_fai) {
		proptag_array_clear(pmessage->pchanged_proptags);
		proptag_array_clear(pmessage->premoved_proptags);
		return GXERR_SUCCESS;
	}
	if (is_new)
		goto SAVE_FULL_CHANGE;
	if (!exmdb_client::get_message_group_id(
		dir, pmessage->message_id, &pgroup_id)) {
		return GXERR_CALL_FAILED;
	}
	if (NULL == pgroup_id) {
		pgpinfo = pmessage->pstore->get_last_property_groupinfo();
		if (NULL == pgpinfo) {
			return GXERR_CALL_FAILED;
		}
		if (!exmdb_client::set_message_group_id(
			dir, pmessage->message_id, pgpinfo->group_id)) {
			return GXERR_CALL_FAILED;
		}
	}  else {
		pgpinfo = pmessage->pstore->get_property_groupinfo(*pgroup_id);
		if (NULL == pgpinfo) {
			return GXERR_CALL_FAILED;
		}
	}
	
	if (!exmdb_client::mark_modified(
		dir, pmessage->message_id)) {
		return GXERR_CALL_FAILED;
	}
	
	pindices = proptag_array_init();
	if (NULL == pindices) {
		return GXERR_CALL_FAILED;
	}
	pungroup_proptags = proptag_array_init();
	if (NULL == pungroup_proptags) {
		proptag_array_free(pindices);
		return GXERR_CALL_FAILED;
	}
	/* always mark PR_MESSAGE_FLAGS as changed */
	if (!proptag_array_append(pmessage->pchanged_proptags,
	    PR_MESSAGE_FLAGS)) {
		proptag_array_free(pindices);
		proptag_array_free(pungroup_proptags);
		return GXERR_CALL_FAILED;
	}
	for (i=0; i<pmessage->pchanged_proptags->count; i++) {
		if (FALSE == property_groupinfo_get_partial_index(pgpinfo,
			pmessage->pchanged_proptags->pproptag[i], &tmp_index)) {
			if (!proptag_array_append(pungroup_proptags,
			    pmessage->pchanged_proptags->pproptag[i])) {
				proptag_array_free(pindices);
				proptag_array_free(pungroup_proptags);
				return GXERR_CALL_FAILED;
			}
		} else {
			if (!proptag_array_append(pindices, tmp_index)) {
				proptag_array_free(pindices);
				proptag_array_free(pungroup_proptags);
				return GXERR_CALL_FAILED;
			}
		}
	}
	for (i=0; i<pmessage->premoved_proptags->count; i++) {
		if (FALSE == property_groupinfo_get_partial_index(pgpinfo,
			pmessage->premoved_proptags->pproptag[i], &tmp_index)) {
			proptag_array_free(pindices);
			proptag_array_free(pungroup_proptags);
			goto SAVE_FULL_CHANGE;
		} else {
			if (!proptag_array_append(pindices, tmp_index)) {
				proptag_array_free(pindices);
				proptag_array_free(pungroup_proptags);
				return GXERR_CALL_FAILED;
			}
		}
	}
	if (!exmdb_client::save_change_indices(
		dir, pmessage->message_id, pmessage->change_num,
		pindices, pungroup_proptags)) {
		proptag_array_free(pindices);
		proptag_array_free(pungroup_proptags);
		return GXERR_CALL_FAILED;
	}
	proptag_array_free(pindices);
	proptag_array_free(pungroup_proptags);
	proptag_array_clear(pmessage->pchanged_proptags);
	proptag_array_clear(pmessage->premoved_proptags);
	return GXERR_SUCCESS;
	
 SAVE_FULL_CHANGE:
	proptag_array_clear(pmessage->pchanged_proptags);
	proptag_array_clear(pmessage->premoved_proptags);
	tmp_indices.count = 0;
	tmp_indices.pproptag = NULL;
	if (!exmdb_client::save_change_indices(
		dir, pmessage->message_id, pmessage->change_num,
		&tmp_indices, (PROPTAG_ARRAY*)&tmp_indices)) {
		return GXERR_CALL_FAILED;
	}
	/* trigger the rule evaluation under public mode 
		when the message is first saved to the folder */
	if (is_new && !b_fai && pmessage->message_id != 0 &&
	    !pmessage->pstore->b_private)
		exmdb_client::rule_new_message(dir, pinfo->get_username(),
			pmessage->pstore->get_account(), pmessage->cpid,
			pmessage->folder_id, pmessage->message_id);
	return GXERR_SUCCESS;
}

BOOL MESSAGE_OBJECT::reload()
{
	auto pmessage = this;
	BOOL b_result;
	uint64_t *pchange_num;
	
	if (pmessage->b_new)
		return TRUE;
	if (!exmdb_client::reload_message_instance(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &b_result) || !b_result)
		return FALSE;	
	proptag_array_clear(pmessage->pchanged_proptags);
	proptag_array_clear(pmessage->premoved_proptags);
	pmessage->b_touched = FALSE;
	pmessage->change_num = 0;
	if (FALSE == pmessage->b_new) {
		if (!exmdb_client_get_instance_property(pmessage->pstore->get_dir(),
		    pmessage->instance_id, PROP_TAG_CHANGENUMBER,
		    reinterpret_cast<void **>(&pchange_num)) ||
		    pchange_num == nullptr)
			return FALSE;
		pmessage->change_num = *pchange_num;
	}
	return TRUE;
}

static constexpr uint32_t trimtags[] = {
	PROP_TAG_MID, PR_DISPLAY_TO, PR_DISPLAY_CC,
	PR_DISPLAY_BCC, PR_MESSAGE_SIZE, PROP_TAG_HASATTACHMENTS,
	PR_CHANGE_KEY, PROP_TAG_CHANGENUMBER,
	PR_PREDECESSOR_CHANGE_LIST,
};

BOOL MESSAGE_OBJECT::write_message(const MESSAGE_CONTENT *pmsgctnt)
{
	auto pmessage = this;
	PROPTAG_ARRAY proptags;
	MESSAGE_CONTENT msgctnt;
	PROBLEM_ARRAY tmp_problems;
	
	msgctnt = *pmsgctnt;
	msgctnt.proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt->proplist.count);
	if (NULL == msgctnt.proplist.ppropval) {
		return FALSE;
	}
	memcpy(msgctnt.proplist.ppropval, pmsgctnt->proplist.ppropval,
				sizeof(TAGGED_PROPVAL)*pmsgctnt->proplist.count);
	for (auto t : trimtags)
		common_util_remove_propvals(&msgctnt.proplist, t);
	if (!exmdb_client::clear_message_instance(pmessage->pstore->get_dir(),
	    pmessage->instance_id))
		return FALSE;
	if (!exmdb_client::write_message_instance(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &msgctnt, TRUE, &proptags, &tmp_problems))
		return FALSE;	
	proptag_array_clear(pmessage->pchanged_proptags);
	proptag_array_clear(pmessage->premoved_proptags);
	pmessage->b_new = TRUE;
	pmessage->b_touched = TRUE;
	return TRUE;
}

BOOL MESSAGE_OBJECT::read_recipients(uint32_t row_id, uint16_t need_count,
    TARRAY_SET *pset)
{
	auto pmessage = this;
	return exmdb_client::get_message_instance_rcpts(pmessage->pstore->get_dir(),
		pmessage->instance_id, row_id, need_count, pset);
}

BOOL MESSAGE_OBJECT::get_rowid_begin(uint32_t *pbegin_id)
{
	auto pmessage = this;
	int last_rowid;
	TARRAY_SET tmp_set;
	
	if (!exmdb_client::get_message_instance_rcpts(pmessage->pstore->get_dir(),
	    pmessage->instance_id, 0, 0xFFFF, &tmp_set))
		return FALSE;	
	last_rowid = -1;
	for (size_t i = 0; i < tmp_set.count; ++i) {
		auto prow_id = static_cast<int32_t *>(common_util_get_propvals(
		               tmp_set.pparray[i], PROP_TAG_ROWID));
		if (NULL != prow_id && *prow_id > last_rowid) {
			last_rowid = *prow_id;
		}
	}
	*pbegin_id = last_rowid + 1;
	return TRUE;
}

BOOL MESSAGE_OBJECT::get_recipient_num(uint16_t *pnum)
{
	auto pmessage = this;
	return exmdb_client::get_message_instance_rcpts_num(pmessage->pstore->get_dir(),
			pmessage->instance_id, pnum);
}

BOOL MESSAGE_OBJECT::empty_rcpts()
{
	auto pmessage = this;
	if (!exmdb_client::empty_message_instance_rcpts(pmessage->pstore->get_dir(),
	    pmessage->instance_id))
		return FALSE;	
	pmessage->b_touched = TRUE;
	if (pmessage->b_new || pmessage->message_id == 0)
		return TRUE;
	proptag_array_append(pmessage->pchanged_proptags, PR_MESSAGE_RECIPIENTS);
	return TRUE;
}

BOOL MESSAGE_OBJECT::set_rcpts(const TARRAY_SET *pset)
{
	auto pmessage = this;
	if (!exmdb_client::update_message_instance_rcpts(pmessage->pstore->get_dir(),
	    pmessage->instance_id, pset))
		return FALSE;	
	pmessage->b_touched = TRUE;
	if (pmessage->b_new || pmessage->message_id == 0)
		return TRUE;
	proptag_array_append(pmessage->pchanged_proptags, PR_MESSAGE_RECIPIENTS);
	return TRUE;
}

BOOL MESSAGE_OBJECT::get_attachments_num(uint16_t *pnum)
{
	auto pmessage = this;
	return exmdb_client::get_message_instance_attachments_num(pmessage->pstore->get_dir(),
	       pmessage->instance_id, pnum);
}

BOOL MESSAGE_OBJECT::delete_attachment(uint32_t attachment_num)
{
	auto pmessage = this;
	if (!exmdb_client::delete_message_instance_attachment(pmessage->pstore->get_dir(),
	    pmessage->instance_id, attachment_num))
		return FALSE;
	pmessage->b_touched = TRUE;
	if (pmessage->b_new || pmessage->message_id == 0)
		return TRUE;
	proptag_array_append(pmessage->pchanged_proptags, PR_MESSAGE_ATTACHMENTS);
	return TRUE;
}

BOOL MESSAGE_OBJECT::get_attachment_table_all_proptags(PROPTAG_ARRAY *pproptags)
{
	auto pmessage = this;
	return exmdb_client::get_message_instance_attachment_table_all_proptags(
	       pmessage->pstore->get_dir(), pmessage->instance_id, pproptags);
}

BOOL MESSAGE_OBJECT::query_attachment_table(const PROPTAG_ARRAY *pproptags,
	uint32_t start_pos, int32_t row_needed, TARRAY_SET *pset)
{
	auto pmessage = this;
	return exmdb_client::query_message_instance_attachment_table(
	       pmessage->pstore->get_dir(), pmessage->instance_id, pproptags,
	       start_pos, row_needed, pset);
}

BOOL MESSAGE_OBJECT::clear_unsent()
{
	auto pmessage = this;
	uint32_t result;
	uint32_t *pmessage_flags;
	TAGGED_PROPVAL tmp_propval;
	
	if (0 == pmessage->message_id) {
		return FALSE;
	}
	if (!exmdb_client_get_instance_property(pmessage->pstore->get_dir(),
	    pmessage->instance_id, PR_MESSAGE_FLAGS, reinterpret_cast<void **>(&pmessage_flags)))
		return FALSE;	
	if (NULL == pmessage_flags) {
		return TRUE;
	}
	*pmessage_flags &= ~MSGFLAG_UNSENT;
	tmp_propval.proptag = PR_MESSAGE_FLAGS;
	tmp_propval.pvalue = pmessage_flags;
	return exmdb_client_set_instance_property(pmessage->pstore->get_dir(),
		pmessage->instance_id, &tmp_propval, &result);
}

BOOL MESSAGE_OBJECT::get_all_proptags(PROPTAG_ARRAY *pproptags)
{
	auto pmessage = this;
	int i;
	PROPTAG_ARRAY tmp_proptags;
	
	if (!exmdb_client::get_instance_all_proptags(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &tmp_proptags))
		return FALSE;	
	pproptags->count = 0;
	pproptags->pproptag = cu_alloc<uint32_t>(tmp_proptags.count + 15);
	if (NULL == pproptags->pproptag) {
		return FALSE;
	}
	for (i=0; i<tmp_proptags.count; i++) {
		switch (tmp_proptags.pproptag[i]) {
		case PROP_TAG_MID:
		case PR_ASSOCIATED:
		case PROP_TAG_CHANGENUMBER:
			continue;
		default:
			pproptags->pproptag[pproptags->count] =
							tmp_proptags.pproptag[i];
			pproptags->count ++;
			break;
		}
	}
	pproptags->pproptag[pproptags->count++] = PR_ACCESS;
	pproptags->pproptag[pproptags->count] = PR_ENTRYID;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_ACCESS_LEVEL;
	pproptags->pproptag[pproptags->count] = PR_OBJECT_TYPE;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_PARENT_ENTRYID;
	pproptags->pproptag[pproptags->count++] = PR_PARENT_SOURCE_KEY;
	pproptags->pproptag[pproptags->count] = PR_RECORD_KEY;
	pproptags->count ++;
	pproptags->pproptag[pproptags->count++] = PR_STORE_RECORD_KEY;
	pproptags->pproptag[pproptags->count++] = PR_MAPPING_SIGNATURE;
	pproptags->pproptag[pproptags->count++] = PR_STORE_ENTRYID;
	if (pmessage->pembedding == nullptr &&
	    common_util_index_proptags(pproptags, PR_SOURCE_KEY) < 0) {
		pproptags->pproptag[pproptags->count] = PR_SOURCE_KEY;
		pproptags->count ++;
	}
	if (common_util_index_proptags(pproptags, PR_MESSAGE_LOCALE_ID) < 0)
		pproptags->pproptag[pproptags->count++] = PR_MESSAGE_LOCALE_ID;
	if (common_util_index_proptags(pproptags, PR_MESSAGE_CODEPAGE) < 0)
		pproptags->pproptag[pproptags->count++] = PR_MESSAGE_CODEPAGE;
	return TRUE;
}

static BOOL msgo_check_readonly_property(const MESSAGE_OBJECT *pmessage,
   uint32_t proptag)
{ 
	if (PROP_TYPE(proptag) == PT_OBJECT)
		return TRUE;
	switch (proptag) {
	case PR_ACCESS:
	case PR_ACCESS_LEVEL:
	case PR_ASSOCIATED:
	case PROP_TAG_CHANGENUMBER:
	case PROP_TAG_CONVERSATIONID:
	case PROP_TAG_CREATORNAME:
	case PROP_TAG_CREATORENTRYID:
	case PR_DISPLAY_BCC:
	case PR_DISPLAY_CC:
	case PR_DISPLAY_TO:
	case PR_ENTRYID:
	case PROP_TAG_FOLDERID:
	case PROP_TAG_HASATTACHMENTS:
	case PROP_TAG_HASNAMEDPROPERTIES:
	case PROP_TAG_LASTMODIFIERENTRYID:
	case PROP_TAG_MID:
	case PROP_TAG_MIMESKELETON:
	case PROP_TAG_NATIVEBODY:
	case PR_OBJECT_TYPE:
	case PR_PARENT_ENTRYID:
	case PR_PARENT_SOURCE_KEY:
	case PR_STORE_ENTRYID:
	case PR_STORE_RECORD_KEY:
	case PR_RECORD_KEY:
	case PR_MESSAGE_SIZE:
	case PROP_TAG_MESSAGESTATUS:
	case PROP_TAG_TRANSPORTMESSAGEHEADERS:
		return TRUE;
	case PR_CHANGE_KEY:
	case PR_CREATION_TIME:
	case PR_LAST_MODIFICATION_TIME:
	case PR_PREDECESSOR_CHANGE_LIST:
	case PR_SOURCE_KEY:
		if (pmessage->b_new)
			return FALSE;
		return TRUE;
	case PR_READ:
		if (NULL == pmessage->pembedding) {
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

static BOOL message_object_get_calculated_property(
	MESSAGE_OBJECT *pmessage, uint32_t proptag, void **ppvalue)
{
	switch (proptag) {
	case PR_ACCESS:
		*ppvalue = &pmessage->tag_access;
		return TRUE;
	case PR_ACCESS_LEVEL:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*static_cast<uint32_t *>(*ppvalue) = pmessage->b_writable ?
			ACCESS_LEVEL_MODIFY : ACCESS_LEVEL_READ_ONLY;
		return TRUE;
	case PR_ENTRYID:
		if (0 == pmessage->message_id) {
			return FALSE;
		}
		*ppvalue = common_util_to_message_entryid(pmessage->pstore,
						pmessage->folder_id, pmessage->message_id);
		return TRUE;
	case PR_SOURCE_KEY:
		if (NULL == pmessage->pembedding) {
			*ppvalue = common_util_calculate_message_sourcekey(
						pmessage->pstore, pmessage->message_id);
			return TRUE;
		}
		return FALSE;
	case PR_OBJECT_TYPE:
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*static_cast<uint32_t *>(*ppvalue) = MAPI_MESSAGE;
		return TRUE;
	case PR_PARENT_ENTRYID:
		if (0 == pmessage->message_id) {
			return FALSE;
		}
		*ppvalue = common_util_to_folder_entryid(
			pmessage->pstore, pmessage->folder_id);
		return TRUE;
	case PROP_TAG_FOLDERID:
	case PROP_TAG_PARENTFOLDERID:
		if (0 == pmessage->message_id) {
			return FALSE;
		}
		*ppvalue = &pmessage->folder_id;
		return TRUE;
	case PR_PARENT_SOURCE_KEY:
		*ppvalue = common_util_calculate_folder_sourcekey(
					pmessage->pstore, pmessage->folder_id);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	case PROP_TAG_MID:
		if (0 == pmessage->message_id) {
			return FALSE;
		}
		*ppvalue = &pmessage->message_id;
		return TRUE;
	case PR_RECORD_KEY:
		if (0 == pmessage->message_id) {
			return FALSE;
		}
		*ppvalue = common_util_to_folder_entryid(
			pmessage->pstore, pmessage->message_id);
		return TRUE;
	case PR_STORE_RECORD_KEY:
	case PR_MAPPING_SIGNATURE:
		*ppvalue = common_util_guid_to_binary(pmessage->pstore->mailbox_guid);
		return TRUE;
	case PR_STORE_ENTRYID:
		*ppvalue = common_util_to_store_entryid(pmessage->pstore);
		if (NULL == *ppvalue) {
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

BOOL MESSAGE_OBJECT::get_properties(const PROPTAG_ARRAY *pproptags,
    TPROPVAL_ARRAY *ppropvals)
{
	auto pmessage = this;
	int i;
	void *pvalue;
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	static const uint32_t lcid_default = 0x0409;
	
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
	for (i=0; i<pproptags->count; i++) {
		if (TRUE == message_object_get_calculated_property(
			pmessage, pproptags->pproptag[i], &pvalue)) {
			if (NULL == pvalue) {
				return FALSE;
			}
			ppropvals->ppropval[ppropvals->count].proptag =
										pproptags->pproptag[i];
			ppropvals->ppropval[ppropvals->count].pvalue = pvalue;
			ppropvals->count ++;
			continue;
		}
		tmp_proptags.pproptag[tmp_proptags.count] =
							pproptags->pproptag[i];
		tmp_proptags.count ++;
	}
	if (0 == tmp_proptags.count) {
		return TRUE;
	}
	if (!exmdb_client::get_instance_properties(pmessage->pstore->get_dir(),
	    pmessage->cpid, pmessage->instance_id, &tmp_proptags, &tmp_propvals))
		return FALSE;	
	if (tmp_propvals.count > 0) {
		memcpy(ppropvals->ppropval +
			ppropvals->count, tmp_propvals.ppropval,
			sizeof(TAGGED_PROPVAL)*tmp_propvals.count);
		ppropvals->count += tmp_propvals.count;
	}
	if (common_util_index_proptags(pproptags, PR_MESSAGE_LOCALE_ID) >= 0 &&
	    common_util_get_propvals(ppropvals, PR_MESSAGE_LOCALE_ID) == nullptr) {
		ppropvals->ppropval[ppropvals->count].proptag = PR_MESSAGE_LOCALE_ID;
		ppropvals->ppropval[ppropvals->count].pvalue = deconst(&lcid_default);
		ppropvals->count ++;
	}
	if (common_util_index_proptags(pproptags, PR_MESSAGE_CODEPAGE) >= 0 &&
	    common_util_get_propvals(ppropvals, PR_MESSAGE_CODEPAGE) == nullptr) {
		ppropvals->ppropval[ppropvals->count].proptag = PR_MESSAGE_CODEPAGE;
		ppropvals->ppropval[ppropvals->count].pvalue =
										&pmessage->cpid;
		ppropvals->count ++;
	}
	return TRUE;	
}

static BOOL message_object_set_properties_internal(
	MESSAGE_OBJECT *pmessage, BOOL b_check,
	const TPROPVAL_ARRAY *ppropvals)
{
	int i, j;
	void *pvalue;
	uint32_t proptag;
	uint8_t tmp_bytes[3];
	PROBLEM_ARRAY problems;
	PROBLEM_ARRAY tmp_problems;
	TPROPVAL_ARRAY tmp_propvals;
	uint16_t *poriginal_indices;
	TPROPVAL_ARRAY tmp_propvals1;
	TAGGED_PROPVAL propval_buff[3];
	
	if (!pmessage->b_writable)
		return FALSE;
	problems.count = 0;
	problems.pproblem = cu_alloc<PROPERTY_PROBLEM>(ppropvals->count);
	if (NULL == problems.pproblem) {
		return FALSE;
	}
	tmp_propvals.count = 0;
	tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(ppropvals->count);
	if (NULL == tmp_propvals.ppropval) {
		return FALSE;
	}
	poriginal_indices = cu_alloc<uint16_t>(ppropvals->count);
	if (NULL == poriginal_indices) {
		return FALSE;
	}
	for (i=0; i<ppropvals->count; i++) {
		if (TRUE == b_check) {
			if (msgo_check_readonly_property(pmessage, ppropvals->ppropval[i].proptag)) {
				problems.pproblem[problems.count].index = i;
				problems.count ++;
				continue;
			} else if (PROP_TAG_EXTENDEDRULEMESSAGECONDITION
				== ppropvals->ppropval[i].proptag) {
				if (!exmdb_client_get_instance_property(pmessage->pstore->get_dir(),
				    pmessage->instance_id, PR_ASSOCIATED, &pvalue))
					return FALSE;	
				if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
					problems.pproblem[problems.count].index = i;
					problems.count ++;
					continue;
				}
				if (((BINARY*)ppropvals->ppropval[i].pvalue)->cb
					> common_util_get_param(
					COMMON_UTIL_MAX_EXTRULE_LENGTH)) {
					problems.pproblem[problems.count].index = i;
					problems.count ++;
					continue;
				}
			} else if (ppropvals->ppropval[i].proptag == PR_MESSAGE_FLAGS) {
				tmp_propvals1.count = 3;
				tmp_propvals1.ppropval = propval_buff;
				propval_buff[0].proptag = PR_READ;
				propval_buff[0].pvalue = &tmp_bytes[0];
				propval_buff[1].proptag = PR_READ_RECEIPT_REQUESTED;
				propval_buff[1].pvalue = &tmp_bytes[1];
				propval_buff[2].proptag = PR_NON_RECEIPT_NOTIFICATION_REQUESTED;
				propval_buff[2].pvalue = &tmp_bytes[2];
				tmp_bytes[0] = !!(*static_cast<uint32_t *>(ppropvals->ppropval[i].pvalue) & MSGFLAG_READ);
				tmp_bytes[1] = !!(*static_cast<uint32_t *>(ppropvals->ppropval[i].pvalue) & MSGFLAG_RN_PENDING);
				tmp_bytes[2] = !!(*static_cast<uint32_t *>(ppropvals->ppropval[i].pvalue) & MSGFLAG_NRN_PENDING);
				if (!exmdb_client::set_instance_properties(pmessage->pstore->get_dir(),
				    pmessage->instance_id, &tmp_propvals1, &tmp_problems))
					return FALSE;	
			}
		}
		tmp_propvals.ppropval[tmp_propvals.count] =
							ppropvals->ppropval[i];
		poriginal_indices[tmp_propvals.count] = i;
		tmp_propvals.count ++;
	}
	if (0 == tmp_propvals.count) {
		return TRUE;
	}
	if (!exmdb_client::set_instance_properties(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &tmp_propvals, &tmp_problems))
		return FALSE;	
	if (tmp_problems.count > 0) {
		for (i=0; i<tmp_problems.count; i++) {
			tmp_problems.pproblem[i].index =
				poriginal_indices[tmp_problems.pproblem[i].index];
		}
		memcpy(problems.pproblem + problems.count,
			tmp_problems.pproblem, tmp_problems.count
			*sizeof(PROPERTY_PROBLEM));
		problems.count += tmp_problems.count;
	}
	if (pmessage->b_new || pmessage->message_id == 0)
		return TRUE;
	for (i=0; i<ppropvals->count; i++) {
		for (j=0; j<problems.count; j++) {
			if (i == problems.pproblem[j].index) {
				break;
			}
		}
		if (j < problems.count) {
			continue;
		}
		pmessage->b_touched = TRUE;
		proptag = ppropvals->ppropval[i].proptag;
		proptag_array_remove(
			pmessage->premoved_proptags, proptag);
		if (!proptag_array_append(pmessage->pchanged_proptags, proptag))
			return FALSE;	
	}
	return TRUE;
}

BOOL MESSAGE_OBJECT::set_properties(const TPROPVAL_ARRAY *ppropvals)
{
	auto pmessage = this;
	char *psubject;
	char *pnormalized_subject;
	
	/* seems some php-mapi users do not understand well
		the relationship between PR_SUBJECT and
		PR_NORMALIZED_SUBJECT, we try to resolve
		the conflict when there exist both of them */
	psubject = static_cast<char *>(common_util_get_propvals(ppropvals, PR_SUBJECT));
	if (NULL == psubject) {
		psubject = static_cast<char *>(common_util_get_propvals(ppropvals, PR_SUBJECT_A));
	}
	if (NULL != psubject) {
		pnormalized_subject = static_cast<char *>(common_util_get_propvals(ppropvals, PR_NORMALIZED_SUBJECT));
		if (NULL == pnormalized_subject) {
			pnormalized_subject = static_cast<char *>(common_util_get_propvals(ppropvals, PR_NORMALIZED_SUBJECT_A));
		}
		if (NULL != pnormalized_subject) {
			if ('\0' == pnormalized_subject[0] && '\0' != psubject[0]) {
				common_util_remove_propvals(const_cast<TPROPVAL_ARRAY *>(ppropvals), PR_NORMALIZED_SUBJECT);
				common_util_remove_propvals(const_cast<TPROPVAL_ARRAY *>(ppropvals), PR_NORMALIZED_SUBJECT_A);
			}
		}
	}
	return message_object_set_properties_internal(
						pmessage, TRUE, ppropvals);
}

BOOL MESSAGE_OBJECT::remove_properties(const PROPTAG_ARRAY *pproptags)
{
	auto pmessage = this;
	int i, j;
	uint32_t proptag;
	PROBLEM_ARRAY problems;
	PROBLEM_ARRAY tmp_problems;
	PROPTAG_ARRAY tmp_proptags;
	uint16_t *poriginal_indices;
	
	if (!pmessage->b_writable)
		return FALSE;
	problems.count = 0;
	problems.pproblem = cu_alloc<PROPERTY_PROBLEM>(pproptags->count);
	if (NULL == problems.pproblem) {
		return FALSE;
	}
	tmp_proptags.count = 0;
	tmp_proptags.pproptag = cu_alloc<uint32_t>(pproptags->count);
	if (NULL == tmp_proptags.pproptag) {
		return FALSE;
	}
	poriginal_indices = cu_alloc<uint16_t>(pproptags->count);
	if (NULL == poriginal_indices) {
		return FALSE;
	}
	for (i=0; i<pproptags->count; i++) {
		if (msgo_check_readonly_property(pmessage, pproptags->pproptag[i])) {
			problems.pproblem[problems.count].index = i;
			problems.count ++;
			continue;
		}
		tmp_proptags.pproptag[tmp_proptags.count] =
							pproptags->pproptag[i];
		poriginal_indices[tmp_proptags.count] = i;
		tmp_proptags.count ++;
	}
	if (0 == tmp_proptags.count) {
		return TRUE;
	}
	if (!exmdb_client::remove_instance_properties(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &tmp_proptags, &tmp_problems))
		return FALSE;	
	if (tmp_problems.count > 0) {
		for (i=0; i<tmp_problems.count; i++) {
			tmp_problems.pproblem[i].index =
				poriginal_indices[tmp_problems.pproblem[i].index];
		}
		memcpy(problems.pproblem + problems.count,
			tmp_problems.pproblem, tmp_problems.count*
			sizeof(PROPERTY_PROBLEM));
		problems.count += tmp_problems.count;
	}
	if (pmessage->b_new || pmessage->message_id == 0)
		return TRUE;
	for (i=0; i<pproptags->count; i++) {
		for (j=0; j<problems.count; j++) {
			if (i == problems.pproblem[j].index) {
				break;
			}
		}
		if (j < problems.count) {
			continue;
		}
		pmessage->b_touched = TRUE;
		proptag = pproptags->pproptag[i];
		proptag_array_remove(
			pmessage->pchanged_proptags, proptag);
		if (!proptag_array_append(pmessage->premoved_proptags, proptag))
			return FALSE;	
	}
	return TRUE;
}

BOOL MESSAGE_OBJECT::copy_to(MESSAGE_OBJECT *pmessage_src,
    const PROPTAG_ARRAY *pexcluded_proptags, BOOL b_force, BOOL *pb_cycle)
{
	auto pmessage = this;
	int i;
	PROPTAG_ARRAY proptags;
	MESSAGE_CONTENT msgctnt;
	PROBLEM_ARRAY tmp_problems;
	
	if (!exmdb_client::check_instance_cycle(pmessage->pstore->get_dir(),
	    pmessage_src->instance_id, pmessage->instance_id, pb_cycle))
		return FALSE;	
	if (*pb_cycle)
		return TRUE;
	if (!exmdb_client::read_message_instance(pmessage_src->pstore->get_dir(),
	    pmessage_src->instance_id, &msgctnt))
		return FALSE;
	for (auto t : trimtags)
		common_util_remove_propvals(&msgctnt.proplist, t);
	i = 0;
	while (i < msgctnt.proplist.count) {
		if (common_util_index_proptags(pexcluded_proptags,
			msgctnt.proplist.ppropval[i].proptag) >= 0) {
			common_util_remove_propvals(&msgctnt.proplist,
					msgctnt.proplist.ppropval[i].proptag);
			continue;
		}
		i ++;
	}
	if (common_util_index_proptags(pexcluded_proptags, PR_MESSAGE_RECIPIENTS) >= 0)
		msgctnt.children.prcpts = NULL;
	if (common_util_index_proptags(pexcluded_proptags, PR_MESSAGE_ATTACHMENTS) >= 0)
		msgctnt.children.pattachments = NULL;
	if (!exmdb_client::write_message_instance(pmessage->pstore->get_dir(),
	    pmessage->instance_id, &msgctnt, b_force, &proptags, &tmp_problems))
		return FALSE;	
	if (pmessage->b_new || pmessage->message_id == 0)
		return TRUE;
	for (i=0; i<proptags.count; i++) {
		proptag_array_append(pmessage->pchanged_proptags,
			proptags.pproptag[i]);
	}
	return TRUE;
}

BOOL MESSAGE_OBJECT::set_readflag(uint8_t read_flag, BOOL *pb_changed)
{
	auto pmessage = this;
	void *pvalue;
	BOOL b_notify;
	uint32_t result;
	uint64_t read_cn;
	uint8_t tmp_byte;
	PROBLEM_ARRAY problems;
	TAGGED_PROPVAL propval;
	MESSAGE_CONTENT *pbrief;
	TPROPVAL_ARRAY propvals;
	static const uint8_t fake_false = false;
	TAGGED_PROPVAL propval_buff[2];
	
	read_flag &= MSG_READ_FLAG_SUPPRESS_RECEIPT|
				MSG_READ_FLAG_CLEAR_READ_FLAG|
				MSG_READ_FLAG_GENERATE_RECEIPT_ONLY|
				MSG_READ_FLAG_CLEAR_NOTIFY_READ|
				MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD;
	const char *username = nullptr;
	if (!pmessage->pstore->b_private) {
		auto pinfo = zarafa_server_get_info();
		username = pinfo->get_username();
	}
	b_notify = FALSE;
	*pb_changed = FALSE;
	auto dir = pmessage->pstore->get_dir();
	switch (read_flag) {
	case MSG_READ_FLAG_DEFAULT:
	case MSG_READ_FLAG_SUPPRESS_RECEIPT:
		if (!exmdb_client_get_instance_property(dir,
		    pmessage->instance_id, PR_READ, &pvalue))
			return FALSE;	
		if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
			tmp_byte = 1;
			*pb_changed = TRUE;
			if (MSG_READ_FLAG_DEFAULT == read_flag) {
				if (!exmdb_client_get_instance_property(dir,
				    pmessage->instance_id,
				    PR_READ_RECEIPT_REQUESTED, &pvalue))
					return FALSE;
				if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
					b_notify = TRUE;
				}
			}
		}
		break;
	case MSG_READ_FLAG_CLEAR_READ_FLAG:
		if (!exmdb_client_get_instance_property(dir,
		    pmessage->instance_id, PR_READ, &pvalue))
			return FALSE;	
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			tmp_byte = 0;
			*pb_changed = TRUE;
		}
		break;
	case MSG_READ_FLAG_GENERATE_RECEIPT_ONLY:
		if (!exmdb_client_get_instance_property(dir,
		    pmessage->instance_id, PR_READ_RECEIPT_REQUESTED,
		    &pvalue))
			return FALSE;
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			b_notify = TRUE;
		}
		break;
	case MSG_READ_FLAG_CLEAR_NOTIFY_READ:
	case MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD:
	case MSG_READ_FLAG_CLEAR_NOTIFY_READ |
		MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD:
		if (read_flag & MSG_READ_FLAG_CLEAR_NOTIFY_READ) {
			if (!exmdb_client_remove_instance_property(dir,
			    pmessage->instance_id, PR_READ_RECEIPT_REQUESTED,
			    &result))
				return FALSE;	
			if (exmdb_client_get_message_property(dir, username, 0,
			    pmessage->message_id, PR_READ_RECEIPT_REQUESTED,
			    &pvalue) && pvalue != nullptr &&
			    *static_cast<uint8_t *>(pvalue) != 0 &&
			    !exmdb_client_remove_message_property(dir,
			    pmessage->cpid, pmessage->message_id,
			    PR_READ_RECEIPT_REQUESTED))
				return FALSE;
		}
		if (read_flag & MSG_READ_FLAG_CLEAR_NOTIFY_UNREAD) {
			if (!exmdb_client_remove_instance_property(dir,
			    pmessage->instance_id, PR_NON_RECEIPT_NOTIFICATION_REQUESTED,
			    &result))
				return FALSE;	
			if (exmdb_client_get_message_property(dir, username, 0,
			    pmessage->message_id, PR_NON_RECEIPT_NOTIFICATION_REQUESTED,
			    &pvalue) && pvalue != nullptr && *static_cast<uint8_t *>(pvalue) != 0 &&
			    !exmdb_client_remove_message_property(dir,
			    pmessage->cpid, pmessage->message_id,
			    PR_NON_RECEIPT_NOTIFICATION_REQUESTED))
				return FALSE;
		}
		if (!exmdb_client_get_instance_property(dir,
		    pmessage->instance_id, PR_MESSAGE_FLAGS,
		    &pvalue) || pvalue == nullptr)
			return FALSE;	
		if (*static_cast<uint32_t *>(pvalue) & MSGFLAG_UNMODIFIED) {
			*static_cast<uint32_t *>(pvalue) &= ~MSGFLAG_UNMODIFIED;
			propval.proptag = PR_MESSAGE_FLAGS;
			propval.pvalue = pvalue;
			if (FALSE == exmdb_client_set_instance_property(
				dir, pmessage->instance_id, &propval, &result)) {
				return FALSE;
			}
			if (!exmdb_client::mark_modified(
				dir, pmessage->message_id)) {
				return FALSE;
			}
		}
		return TRUE;
	default:
		return TRUE;
	}
	if (TRUE == *pb_changed) {
		if (!exmdb_client::set_message_read_state(
			dir, username, pmessage->message_id, tmp_byte,
			&read_cn)) {
			return FALSE;
		}
		propval.proptag = PR_READ;
		propval.pvalue = &tmp_byte;
		if (FALSE == exmdb_client_set_instance_property(
			dir, pmessage->instance_id, &propval, &result)) {
			return FALSE;	
		}
		if (0 != result) {
			return TRUE;
		}
	}
	if (TRUE == b_notify) {
		if (!exmdb_client::get_message_brief(
			dir, pmessage->cpid, pmessage->message_id,
			&pbrief)) {
			return FALSE;	
		}
		if (NULL != pbrief) {
			common_util_notify_receipt(pmessage->pstore->get_account(),
				NOTIFY_RECEIPT_READ, pbrief);
		}
		propvals.count = 2;
		propvals.ppropval = propval_buff;
		propval_buff[0].proptag = PR_READ_RECEIPT_REQUESTED;
		propval_buff[0].pvalue = deconst(&fake_false);
		propval_buff[1].proptag = PR_NON_RECEIPT_NOTIFICATION_REQUESTED;
		propval_buff[1].pvalue = deconst(&fake_false);
		exmdb_client::set_instance_properties(dir,
			pmessage->instance_id, &propvals, &problems);
		exmdb_client::set_message_properties(dir, username,
			0, pmessage->message_id, &propvals, &problems);
	}
	return TRUE;
}
