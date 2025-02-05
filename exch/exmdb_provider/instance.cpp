// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <cstdint>
#include <string>
#include <gromox/database.h>
#include <gromox/fileio.h>
#include <gromox/mapidefs.h>
#include <gromox/scope.hpp>
#include <gromox/tpropval_array.hpp>
#include <gromox/proptag_array.hpp>
#include "exmdb_server.h"
#include "common_util.h"
#include <gromox/tarray_set.hpp>
#include "db_engine.h"
#include <gromox/mail_func.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#define UI(x) static_cast<unsigned int>(x)
#define LLU(x) static_cast<unsigned long long>(x)

#define PROP_TAG_BODY_UNSPECIFIED						0x10000000
#define PROP_TAG_TRANSPORTMESSAGEHEADERS_UNSPECIFIED	0x007D0000
#define PROP_TAG_HTML_UNSPECIFIED						0x10130000
#define MAX_RECIPIENT_NUMBER							4096
#define MAX_ATTACHMENT_NUMBER							1024

using namespace gromox;

namespace {
struct msg_delete {
	void operator()(MESSAGE_CONTENT *msg) { message_content_free(msg); }
};
}

static BOOL instance_read_message(
	const MESSAGE_CONTENT *pmsgctnt1, MESSAGE_CONTENT *pmsgctnt);

static BOOL instance_identify_message(MESSAGE_CONTENT *pmsgctnt);

static BOOL instance_load_message(sqlite3 *psqlite,
	uint64_t message_id, uint32_t *plast_id,
	MESSAGE_CONTENT **ppmsgctnt)
{
	int i;
	uint64_t cid;
	uint32_t row_id;
	uint32_t last_id;
	uint32_t proptag;
	uint64_t rcpt_id;
	TARRAY_SET *prcpts;
	char sql_string[256];
	uint64_t message_id1;
	TAGGED_PROPVAL propval;
	PROPTAG_ARRAY proptags;
	uint64_t attachment_id;
	TPROPVAL_ARRAY *pproplist;
	MESSAGE_CONTENT *pmsgctnt;
	MESSAGE_CONTENT *pmsgctnt1;
	ATTACHMENT_LIST *pattachments;
	ATTACHMENT_CONTENT *pattachment;
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id FROM"
	          " messages WHERE message_id=%llu", LLU(message_id));
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		*ppmsgctnt = NULL;
		return TRUE;
	}
	pstmt.finalize();
	pmsgctnt = message_content_init();
	if (NULL == pmsgctnt) {
		return FALSE;
	}
	if (FALSE == common_util_get_proptags(
		MESSAGE_PROPERTIES_TABLE, message_id,
		psqlite, &proptags)) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	for (i=0; i<proptags.count; i++) {
		switch (proptags.pproptag[i]) {
		case PR_DISPLAY_TO:
		case PR_DISPLAY_TO_A:
		case PR_DISPLAY_CC:
		case PR_DISPLAY_CC_A:
		case PR_DISPLAY_BCC:
		case PR_DISPLAY_BCC_A:
		case PR_SUBJECT:
		case PR_SUBJECT_A:
		case PR_MESSAGE_SIZE:
		case PROP_TAG_HASATTACHMENTS:
			continue;
		case PR_BODY:
		case PR_BODY_A:
			snprintf(sql_string, arsizeof(sql_string), "SELECT proptag, propval FROM "
				"message_properties WHERE (message_id=%llu AND proptag=%u)"
				" OR (message_id=%llu AND proptag=%u)", LLU(message_id),
				PR_BODY, LLU(message_id), PR_BODY_A);
			pstmt = gx_sql_prep(psqlite, sql_string);
			if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			proptag = sqlite3_column_int64(pstmt, 0);
			cid = sqlite3_column_int64(pstmt, 1);
			pstmt.finalize();
			propval.proptag = proptag == PR_BODY ?
			                  ID_TAG_BODY : ID_TAG_BODY_STRING8;
			propval.pvalue = &cid;
			if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
				message_content_free(pmsgctnt);
				return FALSE;	
			}
			break;
		case PROP_TAG_HTML:
		case PROP_TAG_RTFCOMPRESSED:
			snprintf(sql_string, arsizeof(sql_string), "SELECT propval FROM "
				"message_properties WHERE message_id=%llu AND "
				"proptag=%u", LLU(message_id), UI(proptags.pproptag[i]));
			pstmt = gx_sql_prep(psqlite, sql_string);
			if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			cid = sqlite3_column_int64(pstmt, 0);
			pstmt.finalize();
			propval.proptag = proptags.pproptag[i] == PROP_TAG_HTML ?
			                  ID_TAG_HTML : ID_TAG_RTFCOMPRESSED;
			propval.pvalue = &cid;
			if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			break;
		case PROP_TAG_TRANSPORTMESSAGEHEADERS:
		case PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8:
			snprintf(sql_string, arsizeof(sql_string), "SELECT proptag, propval FROM "
				"message_properties WHERE (message_id=%llu AND proptag=%u)"
				" OR (message_id=%llu AND proptag=%u)", LLU(message_id),
				PROP_TAG_TRANSPORTMESSAGEHEADERS, LLU(message_id),
				PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8);
			pstmt = gx_sql_prep(psqlite, sql_string);
			if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			proptag = sqlite3_column_int64(pstmt, 0);
			cid = sqlite3_column_int64(pstmt, 1);
			pstmt.finalize();
			propval.proptag = proptag == PROP_TAG_TRANSPORTMESSAGEHEADERS ?
			                  ID_TAG_TRANSPORTMESSAGEHEADERS :
			                  ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8;
			propval.pvalue = &cid;
			if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
				message_content_free(pmsgctnt);
				return FALSE;	
			}
			break;
		default:
			propval.proptag = proptags.pproptag[i];
			if (FALSE == common_util_get_property(
				MESSAGE_PROPERTIES_TABLE, message_id,
				0, psqlite, propval.proptag,
				&propval.pvalue) || NULL == propval.pvalue
				||
			    !tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			break;
		}
	}
	prcpts = tarray_set_init();
	if (NULL == prcpts) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	message_content_set_rcpts_internal(pmsgctnt, prcpts);
	snprintf(sql_string, arsizeof(sql_string), "SELECT recipient_id FROM"
	          " recipients WHERE message_id=%llu", LLU(message_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT proptag FROM"
		" recipients_properties WHERE recipient_id=?");
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	row_id = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		pproplist = tpropval_array_init();
		if (NULL == pproplist) {
			message_content_free(pmsgctnt);
			return FALSE;
		}
		if (!tarray_set_append_internal(prcpts, pproplist)) {
			tpropval_array_free(pproplist);
			message_content_free(pmsgctnt);
			return FALSE;
		}
		propval.proptag = PROP_TAG_ROWID;
		propval.pvalue = &row_id;
		if (!tpropval_array_set_propval(pproplist, &propval)) {
			message_content_free(pmsgctnt);
			return FALSE;	
		}
		row_id ++;
		rcpt_id = sqlite3_column_int64(pstmt, 0);
		sqlite3_bind_int64(pstmt1, 1, rcpt_id);
		while (SQLITE_ROW == sqlite3_step(pstmt1)) {
			propval.proptag = sqlite3_column_int64(pstmt1, 0);
			if (FALSE == common_util_get_property(
				RECIPIENT_PROPERTIES_TABLE, rcpt_id,
				0, psqlite, propval.proptag,
				&propval.pvalue) || NULL == propval.pvalue
				||
			    !tpropval_array_set_propval(pproplist, &propval)) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
		}
		sqlite3_reset(pstmt1);
	}
	pstmt.finalize();
	pstmt1.finalize();
	pattachments = attachment_list_init();
	if (NULL == pattachments) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	message_content_set_attachments_internal(pmsgctnt, pattachments);
	snprintf(sql_string, arsizeof(sql_string), "SELECT attachment_id FROM "
	          "attachments WHERE message_id=%llu", LLU(message_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id"
			" FROM messages WHERE parent_attid=?");
	pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		pattachment = attachment_content_init();
		if (NULL == pattachment) {
			message_content_free(pmsgctnt);
			return FALSE;
		}
		if (FALSE == attachment_list_append_internal(
			pattachments, pattachment)) {
			attachment_content_free(pattachment);
			message_content_free(pmsgctnt);
			return FALSE;
		}
		propval.proptag = PROP_TAG_ATTACHNUMBER;
		propval.pvalue = plast_id;
		if (!tpropval_array_set_propval(&pattachment->proplist, &propval)) {
			message_content_free(pmsgctnt);
			return FALSE;	
		}
		(*plast_id) ++;
		attachment_id = sqlite3_column_int64(pstmt, 0);
		if (FALSE == common_util_get_proptags(
			ATTACHMENT_PROPERTIES_TABLE,
			attachment_id, psqlite, &proptags)) {
			message_content_free(pmsgctnt);
			return FALSE;
		}
		for (i=0; i<proptags.count; i++) {
			switch (proptags.pproptag[i]) {
			case PR_ATTACH_DATA_BIN:
			case PR_ATTACH_DATA_OBJ: {
				snprintf(sql_string, arsizeof(sql_string), "SELECT propval FROM "
					"attachment_properties WHERE attachment_id=%llu AND"
					" proptag=%u", static_cast<unsigned long long>(attachment_id),
					static_cast<unsigned int>(proptags.pproptag[i]));
				auto pstmt2 = gx_sql_prep(psqlite, sql_string);
				if (pstmt2 == nullptr || sqlite3_step(pstmt2) != SQLITE_ROW) {
					message_content_free(pmsgctnt);
					return FALSE;
				}
				cid = sqlite3_column_int64(pstmt2, 0);
				pstmt2.finalize();
				propval.proptag = proptags.pproptag[i] == PR_ATTACH_DATA_BIN ?
				                  ID_TAG_ATTACHDATABINARY : ID_TAG_ATTACHDATAOBJECT;
				propval.pvalue = &cid;
				if (!tpropval_array_set_propval(&pattachment->proplist, &propval)) {
					message_content_free(pmsgctnt);
					return FALSE;
				}
				break;
			}
			default:
				propval.proptag = proptags.pproptag[i];
				if (FALSE == common_util_get_property(
					ATTACHMENT_PROPERTIES_TABLE, attachment_id,
					0, psqlite, propval.proptag,
					&propval.pvalue) || NULL == propval.pvalue
					||
				    !tpropval_array_set_propval(&pattachment->proplist, &propval)) {
					message_content_free(pmsgctnt);
					return FALSE;
				}
				break;
			}
		}
		sqlite3_bind_int64(pstmt1, 1, attachment_id);
		if (SQLITE_ROW == sqlite3_step(pstmt1)) {
			message_id1 = sqlite3_column_int64(pstmt1, 0);
			last_id = 0;
			if (FALSE == instance_load_message(psqlite,
				message_id1, &last_id, &pmsgctnt1)) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			attachment_content_set_embedded_internal(pattachment, pmsgctnt1);
		}
		sqlite3_reset(pstmt1);
	}
	*ppmsgctnt = pmsgctnt;
	return TRUE;
}

BOOL exmdb_server_load_message_instance(const char *dir,
	const char *username, uint32_t cpid, BOOL b_new,
	uint64_t folder_id, uint64_t message_id,
	uint32_t *pinstance_id)
{
	uint64_t mid_val;
	uint32_t tmp_int32;
	TAGGED_PROPVAL propval;
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	pnode = double_list_get_tail(&pdb->instance_list);
	uint32_t instance_id = pnode == nullptr ? 0 :
	                       static_cast<INSTANCE_NODE *>(pnode->pdata)->instance_id;
	instance_id ++;
	auto pinstance = me_alloc<INSTANCE_NODE>();
	if (NULL == pinstance) {
		return FALSE;
	}
	memset(pinstance, 0, sizeof(INSTANCE_NODE));
	pinstance->node.pdata = pinstance;
	pinstance->instance_id = instance_id;
	pinstance->folder_id = rop_util_get_gc_value(folder_id);
	pinstance->cpid = cpid;
	mid_val = rop_util_get_gc_value(message_id);
	pinstance->type = INSTANCE_TYPE_MESSAGE;
	if (FALSE == exmdb_server_check_private()) {
		pinstance->username = strdup(username);
		if (NULL == pinstance->username) {
			free(pinstance);
			return FALSE;
		}
	}
	if (TRUE == b_new) {
		/* message_id MUST NOT exist in messages table */
		pinstance->b_new = TRUE;
		pinstance->pcontent = message_content_init();
		if (NULL == pinstance->pcontent) {
			if (NULL != pinstance->username) {
				free(pinstance->username);
			}
			free(pinstance);
			return FALSE;
		}
		propval.proptag = PROP_TAG_MID;
		propval.pvalue = &message_id;
		if (!tpropval_array_set_propval(&static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, &propval)) {
			message_content_free(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
			if (NULL != pinstance->username) {
				free(pinstance->username);
			}
			free(pinstance);
			return FALSE;
		}
		propval.proptag = PROP_TAG_MESSAGESTATUS;
		propval.pvalue = &tmp_int32;
		tmp_int32 = 0;
		if (!tpropval_array_set_propval(&static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, &propval)) {
			message_content_free(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
			if (pinstance->username != nullptr)
				free(pinstance->username);
			free(pinstance);
			return false;
		}
		double_list_append_as_tail(&pdb->instance_list, &pinstance->node);
		*pinstance_id = instance_id;
		return TRUE;
	}
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_begin_message_optimize(pdb->psqlite)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (FALSE == instance_load_message(
		pdb->psqlite, mid_val, &pinstance->last_id,
		(MESSAGE_CONTENT**)&pinstance->pcontent)) {
		common_util_end_message_optimize();
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		if (NULL != pinstance->username) {
			free(pinstance->username);
		}
		free(pinstance);
		return FALSE;
	}
	common_util_end_message_optimize();
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	if (NULL == pinstance->pcontent) {
		if (NULL != pinstance->username) {
			free(pinstance->username);
		}
		free(pinstance);
		*pinstance_id = 0;
		return TRUE;
	}
	pinstance->b_new = FALSE;
	double_list_append_as_tail(&pdb->instance_list, &pinstance->node);
	*pinstance_id = instance_id;
	return TRUE;
}

static INSTANCE_NODE* instance_get_instance(db_item_ptr &pdb, uint32_t instance_id)
{
	DOUBLE_LIST_NODE *pnode;
	
	for (pnode=double_list_get_head(&pdb->instance_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->instance_list, pnode)) {
		if (((INSTANCE_NODE*)pnode->pdata)->instance_id == instance_id) {
			return static_cast<INSTANCE_NODE *>(pnode->pdata);
		}
	}
	return NULL;
}

BOOL exmdb_server_load_embedded_instance(const char *dir,
	BOOL b_new, uint32_t attachment_instance_id,
	uint32_t *pinstance_id)
{
	uint64_t mid_val;
	uint64_t message_id;
	uint32_t *pattach_id;
	TAGGED_PROPVAL propval;
	DOUBLE_LIST_NODE *pnode;
	INSTANCE_NODE *pinstance;
	INSTANCE_NODE *pinstance1;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	pnode = double_list_get_tail(&pdb->instance_list);
	uint32_t instance_id = pnode == nullptr ? 0 :
	                       static_cast<INSTANCE_NODE *>(pnode->pdata)->instance_id;
	instance_id ++;
	pinstance1 = instance_get_instance(pdb, attachment_instance_id);
	if (NULL == pinstance1 || INSTANCE_TYPE_ATTACHMENT != pinstance1->type) {
		return FALSE;
	}
	pmsgctnt = ((ATTACHMENT_CONTENT*)pinstance1->pcontent)->pembedded;
	if (NULL == pmsgctnt) {
		if (FALSE == b_new) {
			*pinstance_id = 0;
			return TRUE;
		}
		if (FALSE == common_util_allocate_eid(pdb->psqlite, &mid_val)) {
			return FALSE;
		}
		message_id = rop_util_make_eid_ex(1, mid_val);
		pinstance = me_alloc<INSTANCE_NODE>();
		if (NULL == pinstance) {
			return FALSE;
		}
		memset(pinstance, 0, sizeof(INSTANCE_NODE));
		pinstance->node.pdata = pinstance;
		pinstance->instance_id = instance_id;
		pinstance->parent_id = attachment_instance_id;
		pinstance->cpid = pinstance1->cpid;
		if (NULL != pinstance1->username) {
			pinstance->username = strdup(pinstance1->username);
			if (NULL == pinstance->username) {
				free(pinstance);
				return FALSE;
			}
		}
		pinstance->type = INSTANCE_TYPE_MESSAGE;
		pinstance->b_new = TRUE;
		pinstance->pcontent = message_content_init();
		if (NULL == pinstance->pcontent) {
			if (NULL != pinstance->username) {
				free(pinstance->username);
			}
			free(pinstance);
			return FALSE;
		}
		propval.proptag = PROP_TAG_MID;
		propval.pvalue = &message_id;
		if (!tpropval_array_set_propval(&static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, &propval)) {
			message_content_free(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
			if (NULL != pinstance->username) {
				free(pinstance->username);
			}
			free(pinstance);
			return FALSE;
		}
		double_list_append_as_tail(&pdb->instance_list, &pinstance->node);
		*pinstance_id = instance_id;
		return TRUE;
	}
	if (TRUE == b_new) {
		*pinstance_id = 0;
		return TRUE;
	}
	pinstance = me_alloc<INSTANCE_NODE>();
	if (NULL == pinstance) {
		return FALSE;
	}
	memset(pinstance, 0, sizeof(INSTANCE_NODE));
	pinstance->node.pdata = pinstance;
	pinstance->instance_id = instance_id;
	pinstance->parent_id = attachment_instance_id;
	if (NULL != pmsgctnt->children.pattachments &&
		0 != pmsgctnt->children.pattachments->count) {
		pattachment = pmsgctnt->children.pattachments->pplist[
					pmsgctnt->children.pattachments->count - 1];
		pattach_id = static_cast<uint32_t *>(tpropval_array_get_propval(
			&pattachment->proplist, PROP_TAG_ATTACHNUMBER));
		if (NULL != pattach_id) {
			pinstance->last_id = *pattach_id;
			pinstance->last_id ++;
		}
	}
	pinstance->cpid = pinstance1->cpid;
	if (NULL != pinstance1->username) {
		pinstance->username = strdup(pinstance1->username);
		if (NULL == pinstance->username) {
			free(pinstance);
			return FALSE;
		}
	}
	pinstance->type = INSTANCE_TYPE_MESSAGE;
	pinstance->b_new = FALSE;
	pinstance->pcontent = message_content_dup(pmsgctnt);
	if (NULL == pinstance->pcontent) {
		if (NULL != pinstance->username) {
			free(pinstance->username);
		}
		free(pinstance);
		return FALSE;
	}
	double_list_append_as_tail(&pdb->instance_list, &pinstance->node);
	*pinstance_id = instance_id;
	return TRUE;
}

/* get PROP_TAG_CHANGENUMBER from embedded message */
BOOL exmdb_server_get_embedded_cn(const char *dir, uint32_t instance_id,
    uint64_t **ppcn)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	*ppcn = pinstance->parent_id == 0 ? nullptr :
	        static_cast<uint64_t *>(tpropval_array_get_propval(
	        &static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, PROP_TAG_CHANGENUMBER));
	return TRUE;
}

/* if instance does not exist, do not reload the instance */
BOOL exmdb_server_reload_message_instance(
	const char *dir, uint32_t instance_id, BOOL *pb_result)
{
	void *pvalue;
	uint32_t last_id;
	uint32_t *pattach_id;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	if (TRUE == pinstance->b_new) {
		*pb_result = FALSE;
		return TRUE;
	}
	if (0 == pinstance->parent_id) {
		pvalue = tpropval_array_get_propval(&((MESSAGE_CONTENT*)
					pinstance->pcontent)->proplist, PROP_TAG_MID);
		if (NULL == pvalue) {
			return FALSE;
		}
		last_id = 0;
		if (FALSE == instance_load_message(pdb->psqlite,
			*(uint64_t*)pvalue, &last_id, &pmsgctnt)) {
			return FALSE;	
		}
		if (NULL == pmsgctnt) {
			*pb_result = FALSE;
			return TRUE;
		}
		if (pinstance->last_id < last_id) {
			pinstance->last_id = last_id;
		}
	} else {
		auto pinstance1 = instance_get_instance(pdb, pinstance->parent_id);
		if (NULL == pinstance1 || INSTANCE_TYPE_ATTACHMENT
			!= pinstance1->type) {
			return FALSE;
		}
		if (NULL == ((ATTACHMENT_CONTENT*)
			pinstance1->pcontent)->pembedded) {
			*pb_result = FALSE;
			return TRUE;	
		}
		pmsgctnt = message_content_dup(((ATTACHMENT_CONTENT*)
							pinstance1->pcontent)->pembedded);
		if (NULL == pmsgctnt) {
			return FALSE;
		}
		if (NULL != pmsgctnt->children.pattachments &&
			0 != pmsgctnt->children.pattachments->count) {
			pattachment = pmsgctnt->children.pattachments->pplist[
						pmsgctnt->children.pattachments->count - 1];
			pattach_id = static_cast<uint32_t *>(tpropval_array_get_propval(
				&pattachment->proplist, PROP_TAG_ATTACHNUMBER));
			if (NULL != pattach_id && pinstance->last_id <= *pattach_id) {
				pinstance->last_id = *pattach_id;
				pinstance->last_id ++;
			}
		}
	}
	message_content_free(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
	pinstance->pcontent = pmsgctnt;
	*pb_result = TRUE;
	return TRUE;
}

BOOL exmdb_server_clear_message_instance(
	const char *dir, uint32_t instance_id)
{
	void *pvalue;
	TAGGED_PROPVAL propval;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pvalue = tpropval_array_get_propval(&((MESSAGE_CONTENT*)
				pinstance->pcontent)->proplist, PROP_TAG_MID);
	if (NULL == pvalue) {
		return FALSE;
	}
	pmsgctnt = message_content_init();
	if (NULL == pmsgctnt) {
		return FALSE;
	}
	propval.proptag = PROP_TAG_MID;
	propval.pvalue = pvalue;
	if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
		message_content_free(pmsgctnt);
		return FALSE;
	}
	message_content_free(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
	pinstance->pcontent = pmsgctnt;
	return TRUE;
}

void *instance_read_cid_content(uint64_t cid, uint32_t *plen)
{
	char *pbuff;
	char path[256];
	const char *dir;
	struct stat node_stat;
	
	dir = exmdb_server_get_dir();
	snprintf(path, sizeof(path), "%s/cid/%llu", dir, static_cast<unsigned long long>(cid));
	auto fd = open(path, O_RDONLY);
	if (-1 == fd) {
		return NULL;
	}
	if (fstat(fd, &node_stat) != 0) {
		close(fd);
		return NULL;
	}
	pbuff = cu_alloc<char>(node_stat.st_size);
	if (pbuff == nullptr ||
	    node_stat.st_size != read(fd, pbuff, node_stat.st_size)) {
		close(fd);
		return NULL;
	}
	close(fd);
	if (NULL != plen) {
		*plen = node_stat.st_size;
	}
	return pbuff;
}

static BOOL instance_read_attachment(
	const ATTACHMENT_CONTENT *pattachment1,
	ATTACHMENT_CONTENT *pattachment)
{
	int i;
	BINARY *pbin;
	uint64_t cid;
	
	if (pattachment1->proplist.count > 1) {
		pattachment->proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pattachment1->proplist.count);
		if (NULL == pattachment->proplist.ppropval) {
			return FALSE;
		}
	} else {
		pattachment->proplist.count = 0;
		pattachment->proplist.ppropval = NULL;
		return TRUE;
	}
	pattachment->proplist.count = 0;
	for (i=0; i<pattachment1->proplist.count; i++) {
		switch (pattachment1->proplist.ppropval[i].proptag) {
		case ID_TAG_ATTACHDATABINARY:
		case ID_TAG_ATTACHDATAOBJECT:
			pbin = cu_alloc<BINARY>();
			if (NULL == pbin) {
				return FALSE;
			}
			cid = *(uint64_t*)pattachment1->proplist.ppropval[i].pvalue;
			pbin->pv = instance_read_cid_content(cid, &pbin->cb);
			if (pbin->pv == nullptr)
				return FALSE;
			if (ID_TAG_ATTACHDATABINARY ==
				pattachment1->proplist.ppropval[i].proptag) {
				pattachment->proplist.ppropval[pattachment->proplist.count].proptag = PR_ATTACH_DATA_BIN;
			} else {
				pattachment->proplist.ppropval[pattachment->proplist.count].proptag = PR_ATTACH_DATA_OBJ;
			}
			pattachment->proplist.ppropval[
				pattachment->proplist.count].pvalue = pbin;
			break;
		default:
			pattachment->proplist.ppropval[
				pattachment->proplist.count] =
				pattachment1->proplist.ppropval[i];
			break;
		}
		pattachment->proplist.count ++;
	}
	if (NULL != pattachment1->pembedded) {
		pattachment->pembedded = cu_alloc<MESSAGE_CONTENT>();
		if (NULL == pattachment->pembedded) {
			return FALSE;
		}
		return instance_read_message(
				pattachment1->pembedded,
				pattachment->pembedded);
	} else {
		pattachment->pembedded = NULL;
	}
	return TRUE;
}

static BOOL instance_read_message(
	const MESSAGE_CONTENT *pmsgctnt1,
	MESSAGE_CONTENT *pmsgctnt)
{
	void *pbuff;
	BINARY *pbin;
	uint64_t cid;
	uint32_t length;
	const char *psubject_prefix, *pnormalized_subject;
	TPROPVAL_ARRAY *pproplist;
	TPROPVAL_ARRAY *pproplist1;
	ATTACHMENT_CONTENT *pattachment;
	ATTACHMENT_CONTENT *pattachment1;
	
	pmsgctnt->proplist.count = pmsgctnt1->proplist.count;
	if (0 != pmsgctnt1->proplist.count) {
		pmsgctnt->proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt1->proplist.count + 1);
		if (NULL == pmsgctnt->proplist.ppropval) {
			return FALSE;
		}
	} else {
		pmsgctnt->proplist.ppropval = NULL;
	}
	size_t i;
	for (i=0; i<pmsgctnt1->proplist.count; i++) {
		switch (pmsgctnt1->proplist.ppropval[i].proptag) {
		case ID_TAG_BODY:
			cid = *(uint64_t*)pmsgctnt1->proplist.ppropval[i].pvalue;
			pbuff = instance_read_cid_content(cid, NULL);
			if (NULL == pbuff) {
				return FALSE;
			}
			pmsgctnt->proplist.ppropval[i].proptag = PR_BODY;
			pmsgctnt->proplist.ppropval[i].pvalue = static_cast<char *>(pbuff) + sizeof(int);
			break;
		case ID_TAG_BODY_STRING8:
			cid = *(uint64_t*)pmsgctnt1->proplist.ppropval[i].pvalue;
			pbuff = instance_read_cid_content(cid, NULL);
			if (NULL == pbuff) {
				return FALSE;
			}
			pmsgctnt->proplist.ppropval[i].proptag = PR_BODY_A;
			pmsgctnt->proplist.ppropval[i].pvalue = pbuff;
			break;
		case ID_TAG_HTML:
		case ID_TAG_RTFCOMPRESSED:
			cid = *(uint64_t*)pmsgctnt1->proplist.ppropval[i].pvalue;
			pbuff = instance_read_cid_content(cid, &length);
			if (NULL == pbuff) {
				return FALSE;
			}
			pmsgctnt->proplist.ppropval[i].proptag =
				pmsgctnt1->proplist.ppropval[i].proptag == ID_TAG_HTML ?
				PROP_TAG_HTML : PROP_TAG_RTFCOMPRESSED;
			pbin = cu_alloc<BINARY>();
			if (NULL == pbin) {
				return FALSE;
			}
			pbin->cb = length;
			pbin->pv = pbuff;
			pmsgctnt->proplist.ppropval[i].pvalue = pbin;
			break;
		case ID_TAG_TRANSPORTMESSAGEHEADERS:
			cid = *(uint64_t*)pmsgctnt1->proplist.ppropval[i].pvalue;
			pbuff = instance_read_cid_content(cid, NULL);
			if (NULL == pbuff) {
				return FALSE;
			}
			pmsgctnt->proplist.ppropval[i].proptag =
					PROP_TAG_TRANSPORTMESSAGEHEADERS;
			pmsgctnt->proplist.ppropval[i].pvalue = static_cast<char *>(pbuff) + sizeof(int);
			break;
		case ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8:
			cid = *(uint64_t*)pmsgctnt1->proplist.ppropval[i].pvalue;
			pbuff = instance_read_cid_content(cid, NULL);
			if (NULL == pbuff) {
				return FALSE;
			}
			pmsgctnt->proplist.ppropval[i].proptag =
				PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8;
			pmsgctnt->proplist.ppropval[i].pvalue = pbuff;
			break;
		default:
			pmsgctnt->proplist.ppropval[i] = pmsgctnt1->proplist.ppropval[i];
			break;
		}
	}
	pnormalized_subject = static_cast<char *>(tpropval_array_get_propval(
	                      reinterpret_cast<const TPROPVAL_ARRAY *>(pmsgctnt1),
	                      PR_NORMALIZED_SUBJECT));
	if (NULL == pnormalized_subject) {
		pnormalized_subject = static_cast<char *>(tpropval_array_get_propval(
		                      reinterpret_cast<const TPROPVAL_ARRAY *>(pmsgctnt1),
		                      PR_NORMALIZED_SUBJECT_A));
		if (NULL != pnormalized_subject) {
			psubject_prefix = static_cast<char *>(tpropval_array_get_propval(
			                  reinterpret_cast<const TPROPVAL_ARRAY *>(pmsgctnt1),
			                  PR_SUBJECT_PREFIX_A));
			if (NULL == psubject_prefix) {
				psubject_prefix = "";
			}
			length = strlen(pnormalized_subject)
					+ strlen(psubject_prefix) + 1;
			pmsgctnt->proplist.ppropval[i].proptag = PR_SUBJECT_A;
			pmsgctnt->proplist.ppropval[i].pvalue =
						common_util_alloc(length);
			if (NULL == pmsgctnt->proplist.ppropval[i].pvalue) {
				return FALSE;
			}
			sprintf(static_cast<char *>(pmsgctnt->proplist.ppropval[i].pvalue),
				"%s%s", psubject_prefix, pnormalized_subject);
			pmsgctnt->proplist.count ++;
		} else {
			psubject_prefix = static_cast<char *>(tpropval_array_get_propval(
			                  reinterpret_cast<const TPROPVAL_ARRAY *>(pmsgctnt1),
			                  PR_SUBJECT_PREFIX));
			if (NULL == psubject_prefix) {
				psubject_prefix = static_cast<char *>(tpropval_array_get_propval(
				                  reinterpret_cast<const TPROPVAL_ARRAY *>(pmsgctnt1),
				                  PR_SUBJECT_PREFIX_A));
				if (NULL != psubject_prefix) {
					pmsgctnt->proplist.ppropval[i].proptag = PR_SUBJECT_A;
					pmsgctnt->proplist.ppropval[i].pvalue =
						deconst(psubject_prefix);
					pmsgctnt->proplist.count ++;
				}
			} else {
				pmsgctnt->proplist.ppropval[i].proptag = PR_SUBJECT;
				pmsgctnt->proplist.ppropval[i].pvalue =
					deconst(psubject_prefix);
				pmsgctnt->proplist.count ++;
			}
		}
	} else {
		psubject_prefix = static_cast<char *>(tpropval_array_get_propval(
		                  reinterpret_cast<const TPROPVAL_ARRAY *>(pmsgctnt1),
		                  PR_SUBJECT_PREFIX));
		if (NULL == psubject_prefix) {
			psubject_prefix = "";
		}
		length = strlen(pnormalized_subject)
					+ strlen(psubject_prefix) + 1;
		pmsgctnt->proplist.ppropval[i].proptag = PR_SUBJECT;
		pmsgctnt->proplist.ppropval[i].pvalue =
					common_util_alloc(length);
		if (NULL == pmsgctnt->proplist.ppropval[i].pvalue) {
			return FALSE;
		}
		sprintf(static_cast<char *>(pmsgctnt->proplist.ppropval[i].pvalue),
			"%s%s", psubject_prefix, pnormalized_subject);
		pmsgctnt->proplist.count ++;
	}
	if (NULL == pmsgctnt1->children.prcpts) {
		pmsgctnt->children.prcpts = NULL;
	} else {
		pmsgctnt->children.prcpts = cu_alloc<TARRAY_SET>();
		if (NULL == pmsgctnt->children.prcpts) {
			return FALSE;
		}
		pmsgctnt->children.prcpts->count =
			pmsgctnt1->children.prcpts->count;
		if (0 != pmsgctnt1->children.prcpts->count) {
			pmsgctnt->children.prcpts->pparray = cu_alloc<TPROPVAL_ARRAY *>(pmsgctnt1->children.prcpts->count);
			if (NULL == pmsgctnt->children.prcpts->pparray) {
				return FALSE;
			}
		} else {
			pmsgctnt->children.prcpts->pparray = NULL;
		}
		for (i=0; i<pmsgctnt1->children.prcpts->count; i++) {
			pproplist = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pproplist) {
				return FALSE;
			}
			pmsgctnt->children.prcpts->pparray[i] = pproplist;
			pproplist1 = pmsgctnt1->children.prcpts->pparray[i];
			if (pproplist1->count > 1) {
				pproplist->ppropval = cu_alloc<TAGGED_PROPVAL>(pproplist1->count);
				if (NULL == pproplist->ppropval) {
					return FALSE;
				}
			} else {
				pproplist->count = 0;
				pproplist->ppropval = NULL;
				continue;
			}
			pproplist->count = 0;
			for (size_t j = 0; j < pproplist1->count; ++j) {
				pproplist->ppropval[pproplist->count] =
								pproplist1->ppropval[j];
				pproplist->count ++;
			}
		}
	}
	if (NULL == pmsgctnt1->children.pattachments) {
		pmsgctnt->children.pattachments = NULL;
	} else {
		pmsgctnt->children.pattachments = cu_alloc<ATTACHMENT_LIST>();
		if (NULL == pmsgctnt->children.pattachments) {
			return FALSE;
		}
		pmsgctnt->children.pattachments->count =
			pmsgctnt1->children.pattachments->count;
		if (0 != pmsgctnt1->children.pattachments->count) {
			pmsgctnt->children.pattachments->pplist = cu_alloc<ATTACHMENT_CONTENT *>(pmsgctnt1->children.pattachments->count);
			if (NULL == pmsgctnt->children.pattachments->pplist) {
				return FALSE;
			}
		} else {
			pmsgctnt->children.pattachments->pplist = NULL;
		}
		for (i=0; i<pmsgctnt1->children.pattachments->count; i++) {
			pattachment = cu_alloc<ATTACHMENT_CONTENT>();
			if (NULL == pattachment) {
				return FALSE;
			}
			memset(pattachment, 0 ,sizeof(ATTACHMENT_CONTENT));
			pmsgctnt->children.pattachments->pplist[i] = pattachment;
			pattachment1 = pmsgctnt1->children.pattachments->pplist[i];
			if (FALSE == instance_read_attachment(
				pattachment1, pattachment)) {
				return FALSE;	
			}
		}
	}
	return TRUE;
}

BOOL exmdb_server_read_message_instance(const char *dir,
	uint32_t instance_id, MESSAGE_CONTENT *pmsgctnt)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	memset(pmsgctnt, 0, sizeof(MESSAGE_CONTENT));
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	if (FALSE == instance_read_message(
	    static_cast<MESSAGE_CONTENT *>(pinstance->pcontent), pmsgctnt)) {
		return FALSE;
	}
	return TRUE;
}

static BOOL instance_identify_rcpts(TARRAY_SET *prcpts)
{
	uint32_t i;
	TAGGED_PROPVAL propval;
	
	for (i=0; i<prcpts->count; i++) {
		propval.proptag = PROP_TAG_ROWID;
		propval.pvalue = &i;
		if (!tpropval_array_set_propval(prcpts->pparray[i], &propval))
			return FALSE;
	}
	return TRUE;
}

static BOOL instance_identify_attachments(ATTACHMENT_LIST *pattachments)
{
	uint32_t i;
	TAGGED_PROPVAL propval;
	
	for (i=0; i<pattachments->count; i++) {
		propval.proptag = PROP_TAG_ATTACHNUMBER;
		propval.pvalue = &i;
		if (!tpropval_array_set_propval(&pattachments->pplist[i]->proplist, &propval))
			return FALSE;	
		if (NULL != pattachments->pplist[i]->pembedded) {
			if (FALSE == instance_identify_message(
				pattachments->pplist[i]->pembedded)) {
				return FALSE;	
			}
		}
	}
	return TRUE;
}

static BOOL instance_identify_message(MESSAGE_CONTENT *pmsgctnt)
{
	if (NULL != pmsgctnt->children.prcpts) {
		if (FALSE == instance_identify_rcpts(
			pmsgctnt->children.prcpts)) {
			return FALSE;	
		}
	}
	if (NULL != pmsgctnt->children.pattachments) {
		if (FALSE == instance_identify_attachments(
			pmsgctnt->children.pattachments)) {
			return FALSE;	
		}
	}
	return TRUE;
}

/* pproptags is for returning successful proptags */
BOOL exmdb_server_write_message_instance(const char *dir,
	uint32_t instance_id, const MESSAGE_CONTENT *pmsgctnt,
	BOOL b_force, PROPTAG_ARRAY *pproptags,
	PROBLEM_ARRAY *pproblems)
{
	int i;
	uint32_t proptag;
	TARRAY_SET *prcpts;
	TPROPVAL_ARRAY *pproplist;
	ATTACHMENT_LIST *pattachments;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pproblems->count = 0;
	pproblems->pproblem = cu_alloc<PROPERTY_PROBLEM>(pmsgctnt->proplist.count + 2);
	if (NULL == pproblems->pproblem) {
		return FALSE;
	}
	pproptags->count = 0;
	pproptags->pproptag = cu_alloc<uint32_t>(pmsgctnt->proplist.count + 2);
	if (NULL == pproptags->pproptag) {
		return FALSE;
	}
	pproplist = &((MESSAGE_CONTENT*)pinstance->pcontent)->proplist;
	for (i=0; i<pmsgctnt->proplist.count; i++) {
		proptag = pmsgctnt->proplist.ppropval[i].proptag;
		switch (proptag) {
		case PR_ASSOCIATED:
			if (TRUE == pinstance->b_new) {
				break;
			}
		case PROP_TAG_MID:
		case PR_ENTRYID:
		case PROP_TAG_FOLDERID:
		case PROP_TAG_CODEPAGEID:
		case PROP_TAG_PARENTFOLDERID:
		case PROP_TAG_INSTANCESVREID:
		case PROP_TAG_HASNAMEDPROPERTIES:
		case PR_MESSAGE_SIZE:
		case PROP_TAG_HASATTACHMENTS:
		case PR_DISPLAY_TO:
		case PR_DISPLAY_CC:
		case PR_DISPLAY_BCC:
		case PR_DISPLAY_TO_A:
		case PR_DISPLAY_CC_A:
		case PR_DISPLAY_BCC_A:
			pproblems->pproblem[pproblems->count].index = i;
			pproblems->pproblem[pproblems->count].proptag = proptag;
			pproblems->pproblem[pproblems->count].err = ecAccessDenied;
			pproblems->count ++;
			continue;
		default:
			break;
		}
		if (FALSE == b_force) {
			switch (proptag) {
			case PR_BODY:
			case PR_BODY_A:	
				if (NULL != tpropval_array_get_propval(
					pproplist, ID_TAG_BODY) ||
					NULL != tpropval_array_get_propval(
					pproplist, ID_TAG_BODY_STRING8)) {
					continue;	
				}
				break;
			case PROP_TAG_HTML:
				if (NULL != tpropval_array_get_propval(
					pproplist, ID_TAG_HTML)) {
					continue;	
				}
				break;
			case PROP_TAG_RTFCOMPRESSED:
				if (NULL != tpropval_array_get_propval(
					pproplist, ID_TAG_RTFCOMPRESSED)) {
					continue;	
				}
				break;
			}
			if (PROP_TYPE(proptag) == PT_STRING8) {
				if (tpropval_array_get_propval(pproplist,
				    CHANGE_PROP_TYPE(proptag, PT_UNICODE)) != nullptr)
					continue;
			} else if (PROP_TYPE(proptag) == PT_UNICODE) {
				if (tpropval_array_get_propval(pproplist,
				    CHANGE_PROP_TYPE(proptag, PT_STRING8)) != nullptr)
					continue;
			}
			if (NULL != tpropval_array_get_propval(pproplist, proptag)) {
				continue;
			}
		}
		switch (proptag) {
		case PR_BODY:
		case PR_BODY_A:	
			tpropval_array_remove_propval(
				pproplist, ID_TAG_BODY);
			tpropval_array_remove_propval(
				pproplist, ID_TAG_BODY_STRING8);
			pinstance->change_mask |= CHANGE_MASK_BODY;
			break;
		case PROP_TAG_HTML:
			tpropval_array_remove_propval(
				pproplist, ID_TAG_HTML);
			tpropval_array_remove_propval(
				pproplist, PROP_TAG_BODYHTML);
			tpropval_array_remove_propval(
				pproplist, PROP_TAG_BODYHTML_STRING8);
			pinstance->change_mask |= CHANGE_MASK_HTML;
			break;
		case PROP_TAG_RTFCOMPRESSED:
			tpropval_array_remove_propval(
				pproplist, ID_TAG_RTFCOMPRESSED);
			break;
		}
		if (!tpropval_array_set_propval(pproplist, pmsgctnt->proplist.ppropval + i)) {
			return FALSE;
		}
		switch (proptag) {
		case PR_CHANGE_KEY:
		case PROP_TAG_CHANGENUMBER:
		case PR_PREDECESSOR_CHANGE_LIST:
			continue;
		}
		pproptags->pproptag[pproptags->count] = proptag;
		pproptags->count ++;
	}
	if (NULL != pmsgctnt->children.prcpts) {
		if (TRUE == b_force || NULL == ((MESSAGE_CONTENT*)
			pinstance->pcontent)->children.prcpts) {
			prcpts = tarray_set_dup(pmsgctnt->children.prcpts);
			if (NULL == prcpts) {
				return FALSE;
			}
			if (FALSE == instance_identify_rcpts(prcpts)) {
				tarray_set_free(prcpts);
				return FALSE;
			}
			message_content_set_rcpts_internal(
				static_cast<MESSAGE_CONTENT *>(pinstance->pcontent), prcpts);
			pproptags->pproptag[pproptags->count++] = PR_MESSAGE_RECIPIENTS;
		}
	}
	if (NULL != pmsgctnt->children.pattachments) {
		if (TRUE == b_force || NULL == ((MESSAGE_CONTENT*)
			pinstance->pcontent)->children.pattachments) {
			pattachments = attachment_list_dup(
				pmsgctnt->children.pattachments);
			if (NULL == pattachments) {
				return FALSE;
			}
			if (FALSE == instance_identify_attachments(pattachments)) {
				attachment_list_free(pattachments);
				return FALSE;
			}
			message_content_set_attachments_internal(
				static_cast<MESSAGE_CONTENT *>(pinstance->pcontent), pattachments);
			pproptags->pproptag[pproptags->count++] = PR_MESSAGE_ATTACHMENTS;
		}
	}
	return TRUE;
}

BOOL exmdb_server_load_attachment_instance(const char *dir,
	uint32_t message_instance_id, uint32_t attachment_num,
	uint32_t *pinstance_id)
{
	int i;
	void *pvalue;
	DOUBLE_LIST_NODE *pnode;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment = nullptr;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	pnode = double_list_get_tail(&pdb->instance_list);
	uint32_t instance_id = pnode == nullptr ? 0 :
	                       static_cast<INSTANCE_NODE *>(pnode->pdata)->instance_id;
	instance_id ++;
	auto pinstance1 = instance_get_instance(pdb, message_instance_id);
	if (NULL == pinstance1 || INSTANCE_TYPE_MESSAGE != pinstance1->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance1->pcontent;
	if (NULL == pmsgctnt->children.pattachments) {
		*pinstance_id = 0;
		return TRUE;
	}
	for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
		pattachment = pmsgctnt->children.pattachments->pplist[i];
		pvalue = tpropval_array_get_propval(
			&pattachment->proplist, PROP_TAG_ATTACHNUMBER);
		if (NULL == pvalue) {
			return FALSE;
		}
		if (*(uint32_t*)pvalue == attachment_num) {
			break;
		}
	}
	if (i >= pmsgctnt->children.pattachments->count) {
		*pinstance_id = 0;
		return TRUE;
	}
	auto pinstance = me_alloc<INSTANCE_NODE>();
	if (NULL == pinstance) {
		return FALSE;
	}
	memset(pinstance, 0, sizeof(INSTANCE_NODE));
	pinstance->node.pdata = pinstance;
	pinstance->instance_id = instance_id;
	pinstance->parent_id = message_instance_id;
	pinstance->cpid = pinstance1->cpid;
	if (NULL != pinstance1->username) {
		pinstance->username = strdup(pinstance1->username);
		if (NULL == pinstance->username) {
			free(pinstance);
			return FALSE;
		}
	}
	pinstance->type = INSTANCE_TYPE_ATTACHMENT;
	pinstance->b_new = FALSE;
	pinstance->pcontent = attachment_content_dup(pattachment);
	if (NULL == pinstance->pcontent) {
		if (NULL != pinstance->username) {
			free(pinstance->username);
		}
		free(pinstance);
		return FALSE;
	}
	double_list_append_as_tail(&pdb->instance_list, &pinstance->node);
	*pinstance_id = instance_id;
	return TRUE;
}

BOOL exmdb_server_create_attachment_instance(const char *dir,
	uint32_t message_instance_id, uint32_t *pinstance_id,
	uint32_t *pattachment_num)
{
	TAGGED_PROPVAL propval;
	DOUBLE_LIST_NODE *pnode;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	pnode = double_list_get_tail(&pdb->instance_list);
	uint32_t instance_id = pnode == nullptr ? 0 :
	                       static_cast<INSTANCE_NODE *>(pnode->pdata)->instance_id;
	instance_id ++;
	auto pinstance1 = instance_get_instance(pdb, message_instance_id);
	if (NULL == pinstance1 || INSTANCE_TYPE_MESSAGE != pinstance1->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance1->pcontent;
	if (NULL != pmsgctnt->children.pattachments &&
		pmsgctnt->children.pattachments->count >=
		MAX_ATTACHMENT_NUMBER) {
		*pinstance_id = 0;
		*pattachment_num = ATTACHMENT_NUM_INVALID;
		return TRUE;	
	}
	auto pinstance = me_alloc<INSTANCE_NODE>();
	if (NULL == pinstance) {
		return FALSE;
	}
	memset(pinstance, 0, sizeof(INSTANCE_NODE));
	pinstance->node.pdata = pinstance;
	pinstance->instance_id = instance_id;
	pinstance->parent_id = message_instance_id;
	pinstance->cpid = pinstance1->cpid;
	if (NULL != pinstance1->username) {
		pinstance->username = strdup(pinstance1->username);
		if (NULL == pinstance->username) {
			free(pinstance);
			return FALSE;
		}
	}
	pinstance->type = INSTANCE_TYPE_ATTACHMENT;
	pinstance->b_new = TRUE;
	pattachment = attachment_content_init();
	if (NULL == pattachment) {
		if (NULL != pinstance->username) {
			free(pinstance->username);
		}
		free(pinstance);
		return FALSE;
	}
	*pattachment_num = pinstance1->last_id;
	pinstance1->last_id ++;
	propval.proptag = PROP_TAG_ATTACHNUMBER;
	propval.pvalue = pattachment_num;
	if (!tpropval_array_set_propval(&pattachment->proplist, &propval)) {
		attachment_content_free(pattachment);
		if (NULL != pinstance->username) {
			free(pinstance->username);
		}
		free(pinstance);
		return FALSE;
	}
	pinstance->pcontent = pattachment;
	double_list_append_as_tail(&pdb->instance_list, &pinstance->node);
	*pinstance_id = instance_id;
	return TRUE;
}

BOOL exmdb_server_read_attachment_instance(const char *dir,
	uint32_t instance_id, ATTACHMENT_CONTENT *pattctnt)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	memset(pattctnt, 0, sizeof(ATTACHMENT_CONTENT));
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_ATTACHMENT != pinstance->type) {
		return FALSE;
	}
	if (FALSE == instance_read_attachment(
	    static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent), pattctnt)) {
		return FALSE;
	}
	return TRUE;
}

BOOL exmdb_server_write_attachment_instance(const char *dir,
	uint32_t instance_id, const ATTACHMENT_CONTENT *pattctnt,
	BOOL b_force, PROBLEM_ARRAY *pproblems)
{
	int i;
	uint32_t proptag;
	TPROPVAL_ARRAY *pproplist;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_ATTACHMENT != pinstance->type) {
		return FALSE;
	}
	pproblems->count = 0;
	pproblems->pproblem = cu_alloc<PROPERTY_PROBLEM>(pattctnt->proplist.count + 1);
	if (NULL == pproblems->pproblem) {
		return FALSE;
	}
	pproplist = &((ATTACHMENT_CONTENT*)pinstance->pcontent)->proplist;
	for (i=0; i<pattctnt->proplist.count; i++) {
		proptag = pattctnt->proplist.ppropval[i].proptag;
		switch (proptag) {
		case PR_RECORD_KEY:
			pproblems->pproblem[pproblems->count].index = i;
			pproblems->pproblem[pproblems->count].proptag = proptag;
			pproblems->pproblem[pproblems->count].err = ecAccessDenied;
			pproblems->count ++;
			continue;
		}
		if (FALSE == b_force) {
			switch (proptag) {
			case PR_ATTACH_DATA_BIN:
				if (NULL != tpropval_array_get_propval(
					pproplist, ID_TAG_ATTACHDATABINARY)) {
					continue;	
				}
				break;
			case PR_ATTACH_DATA_OBJ:
				if (NULL != tpropval_array_get_propval(
					pproplist, ID_TAG_ATTACHDATAOBJECT)) {
					continue;	
				}
				break;
			}
			if (PROP_TYPE(proptag) == PT_STRING8) {
				if (tpropval_array_get_propval(pproplist,
				    CHANGE_PROP_TYPE(proptag, PT_UNICODE)) != nullptr)
					continue;
			} else if (PROP_TYPE(proptag) == PT_UNICODE) {
				if (tpropval_array_get_propval(pproplist,
				    CHANGE_PROP_TYPE(proptag, PT_STRING8)) != nullptr)
					continue;
			}
			if (NULL != tpropval_array_get_propval(pproplist, proptag)) {
				continue;
			}
		}
		switch (proptag) {
		case PR_ATTACH_DATA_BIN:
			tpropval_array_remove_propval(
				pproplist, ID_TAG_ATTACHDATABINARY);
			break;
		case PR_ATTACH_DATA_OBJ:
			tpropval_array_remove_propval(
				pproplist, ID_TAG_ATTACHDATAOBJECT);
			break;
		}
		if (!tpropval_array_set_propval(pproplist, pattctnt->proplist.ppropval + i)) {
			return FALSE;
		}
	}
	if (NULL != pattctnt->pembedded) {
		if (FALSE == b_force || NULL == ((ATTACHMENT_CONTENT*)
			pinstance->pcontent)->pembedded) {
			pmsgctnt = message_content_dup(pattctnt->pembedded);
			if (NULL == pmsgctnt) {
				return FALSE;
			}
			if (FALSE == instance_identify_message(pmsgctnt)) {
				message_content_free(pmsgctnt);
				return FALSE;
			}
			attachment_content_set_embedded_internal(static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent), pmsgctnt);
		}
	}
	return TRUE;
}

BOOL exmdb_server_delete_message_instance_attachment(
	const char *dir, uint32_t message_instance_id,
	uint32_t attachment_num)
{
	int i;
	void *pvalue;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, message_instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL == pmsgctnt->children.pattachments) {
		return TRUE;
	}
	for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
		pattachment = pmsgctnt->children.pattachments->pplist[i];
		pvalue = tpropval_array_get_propval(
			&pattachment->proplist, PROP_TAG_ATTACHNUMBER);
		if (NULL == pvalue) {
			return FALSE;
		}
		if (*(uint32_t*)pvalue == attachment_num) {
			break;
		}
	}
	if (i >= pmsgctnt->children.pattachments->count) {
		return TRUE;
	}
	attachment_list_remove(pmsgctnt->children.pattachments, i);
	if (0 == pmsgctnt->children.pattachments->count) {
		attachment_list_free(pmsgctnt->children.pattachments);
		pmsgctnt->children.pattachments = NULL;
	}
	return TRUE;
}

/* account must be available when it is a normal message instance */ 
BOOL exmdb_server_flush_instance(const char *dir, uint32_t instance_id,
    const char *account, gxerr_t *pe_result)
{
	int i;
	void *pvalue;
	BINARY *pbin;
	uint32_t *pcpid;
	uint64_t folder_id;
	char tmp_buff[1024];
	char address_type[16];
	TAGGED_PROPVAL propval;
	uint32_t attachment_num;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance) {
		return FALSE;
	}
	if (INSTANCE_TYPE_ATTACHMENT == pinstance->type) {
		auto pinstance1 = instance_get_instance(pdb, pinstance->parent_id);
		if (NULL == pinstance1 ||
			INSTANCE_TYPE_MESSAGE != pinstance1->type) {
			return FALSE;
		}
		pmsgctnt = (MESSAGE_CONTENT*)pinstance1->pcontent;
		pattachment = attachment_content_dup(static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent));
		if (NULL == pattachment) {
			return FALSE;
		}
		if (TRUE == pinstance->b_new) {
			if (NULL == pmsgctnt->children.pattachments) {
				pmsgctnt->children.pattachments = attachment_list_init();
				if (NULL == pmsgctnt->children.pattachments) {
					attachment_content_free(pattachment);
					return FALSE;
				}
			}
			if (FALSE == attachment_list_append_internal(
				pmsgctnt->children.pattachments, pattachment)) {
				attachment_content_free(pattachment);
				return FALSE;
			}
			pinstance->b_new = FALSE;
		} else {
			if (NULL == pmsgctnt->children.pattachments) {
				pmsgctnt->children.pattachments = attachment_list_init();
				if (NULL == pmsgctnt->children.pattachments) {
					attachment_content_free(pattachment);
					return FALSE;
				}
			}
			pvalue = tpropval_array_get_propval(
				&pattachment->proplist, PROP_TAG_ATTACHNUMBER);
			if (NULL == pvalue) {
				attachment_content_free(pattachment);
				return FALSE;
			}
			attachment_num = *(uint32_t*)pvalue;
			for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
				pvalue = tpropval_array_get_propval(
					&pmsgctnt->children.pattachments->pplist[i]->proplist,
					PROP_TAG_ATTACHNUMBER);
				if (NULL == pvalue) {
					attachment_content_free(pattachment);
					return FALSE;
				}
				if (*(uint32_t*)pvalue == attachment_num) {
					break;
				}
			}
			if (i < pmsgctnt->children.pattachments->count) {
				attachment_content_free(
					pmsgctnt->children.pattachments->pplist[i]);
				pmsgctnt->children.pattachments->pplist[i] = pattachment;
			} else {
				if (FALSE == attachment_list_append_internal(
					pmsgctnt->children.pattachments, pattachment)) {
					attachment_content_free(pattachment);
					return FALSE;
				}
			}
		}
		*pe_result = GXERR_SUCCESS;
		return TRUE;
	}
	if ((pinstance->change_mask & CHANGE_MASK_HTML) &&
		0 == (pinstance->change_mask & CHANGE_MASK_BODY)) {
		pbin = static_cast<BINARY *>(tpropval_array_get_propval(
		       &static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, PROP_TAG_HTML));
		pcpid = static_cast<uint32_t *>(tpropval_array_get_propval(
		        &static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, PR_INTERNET_CPID));
		if (NULL != pbin && NULL != pcpid) {
			std::string plainbuf;
			auto ret = html_to_plain(pbin->pc, pbin->cb, plainbuf);
			if (ret < 0)
				return false;
			propval.proptag = PR_BODY_W;
			if (ret == 65001 || *pcpid == 65001) {
				propval.pvalue = plainbuf.data();
			} else {
				pvalue = common_util_convert_copy(TRUE, *pcpid, plainbuf.c_str());
				if (pvalue == nullptr)
					return false;
				propval.pvalue = pvalue;
			}
			if (!tpropval_array_set_propval(&static_cast<MESSAGE_CONTENT *>(pinstance->pcontent)->proplist, &propval))
				return false;
		}
	}
	pinstance->change_mask = 0;
	if (0 != pinstance->parent_id) {
		auto pinstance1 = instance_get_instance(pdb, pinstance->parent_id);
		if (NULL == pinstance1 ||
			INSTANCE_TYPE_ATTACHMENT != pinstance1->type) {
			return FALSE;
		}
		pmsgctnt = message_content_dup(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
		if (NULL == pmsgctnt) {
			return FALSE;
		}
		attachment_content_set_embedded_internal(static_cast<ATTACHMENT_CONTENT *>(pinstance1->pcontent), pmsgctnt);
		*pe_result = GXERR_SUCCESS;
		return TRUE;
	}
	pmsgctnt = message_content_dup(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
	if (NULL == pmsgctnt) {
		return FALSE;	
	}
	std::unique_ptr<MESSAGE_CONTENT, msg_delete> upmsgctnt(pmsgctnt);
	pbin = static_cast<BINARY *>(tpropval_array_get_propval(&pmsgctnt->proplist,
	       PROP_TAG_SENTREPRESENTINGENTRYID));
	if (NULL != pbin && NULL == tpropval_array_get_propval(
		&pmsgctnt->proplist, PROP_TAG_SENTREPRESENTINGEMAILADDRESS)) {
		pvalue = tpropval_array_get_propval(&pmsgctnt->proplist,
						PROP_TAG_SENTREPRESENTINGADDRESSTYPE);
		if (NULL == pvalue) {
			if (common_util_parse_addressbook_entryid(pbin,
			    address_type, GX_ARRAY_SIZE(address_type),
			    tmp_buff, GX_ARRAY_SIZE(tmp_buff))) {
				propval.proptag = PROP_TAG_SENTREPRESENTINGADDRESSTYPE;
				propval.pvalue = address_type;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
				propval.proptag = PROP_TAG_SENTREPRESENTINGEMAILADDRESS;
				propval.pvalue = tmp_buff;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
			}
		} else if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
			if (common_util_addressbook_entryid_to_essdn(pbin,
			    tmp_buff, GX_ARRAY_SIZE(tmp_buff))) {
				propval.proptag = PROP_TAG_SENTREPRESENTINGEMAILADDRESS;
				propval.pvalue = tmp_buff;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
			}
		} else if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
			if (common_util_addressbook_entryid_to_username(pbin,
			    tmp_buff, GX_ARRAY_SIZE(tmp_buff))) {
				propval.proptag = PROP_TAG_SENTREPRESENTINGEMAILADDRESS;
				propval.pvalue = tmp_buff;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
			}
		}
	}
	pbin = static_cast<BINARY *>(tpropval_array_get_propval(
	       &pmsgctnt->proplist, PROP_TAG_SENDERENTRYID));
	if (NULL != pbin && NULL == tpropval_array_get_propval(
		&pmsgctnt->proplist, PROP_TAG_SENDEREMAILADDRESS)) {
		pvalue = tpropval_array_get_propval(
			&pmsgctnt->proplist, PROP_TAG_SENDERADDRESSTYPE);
		if (NULL == pvalue) {
			if (common_util_parse_addressbook_entryid(pbin,
			    address_type, GX_ARRAY_SIZE(address_type),
			    tmp_buff, GX_ARRAY_SIZE(tmp_buff))) {
				propval.proptag = PROP_TAG_SENDERADDRESSTYPE;
				propval.pvalue = address_type;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
				propval.proptag = PROP_TAG_SENDEREMAILADDRESS;
				propval.pvalue = tmp_buff;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
			}
		} else if (strcasecmp(static_cast<char *>(pvalue), "EX") == 0) {
			if (common_util_addressbook_entryid_to_essdn(pbin,
			    tmp_buff, GX_ARRAY_SIZE(tmp_buff))) {
				propval.proptag = PROP_TAG_SENDEREMAILADDRESS;
				propval.pvalue = tmp_buff;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
			}
		} else if (strcasecmp(static_cast<char *>(pvalue), "SMTP") == 0) {
			if (common_util_addressbook_entryid_to_username(pbin,
			    tmp_buff, GX_ARRAY_SIZE(tmp_buff))) {
				propval.proptag = PROP_TAG_SENDEREMAILADDRESS;
				propval.pvalue = tmp_buff;
				if(!tpropval_array_set_propval(&pmsgctnt->proplist, &propval))
					return FALSE;
			}
		}
	}
	pinstance->b_new = FALSE;
	folder_id = rop_util_make_eid_ex(1, pinstance->folder_id);
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(pinstance->username);
	}
	pdb.reset();
	common_util_set_tls_var(pmsgctnt);
	BOOL b_result = exmdb_server_write_message(dir, account, 0, folder_id,
	                pmsgctnt, pe_result);
	common_util_set_tls_var(NULL);
	return b_result;
}
	
BOOL exmdb_server_unload_instance(
	const char *dir, uint32_t instance_id)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance) {
		return TRUE;
	}
	double_list_remove(&pdb->instance_list, &pinstance->node);
	if (INSTANCE_TYPE_ATTACHMENT == pinstance->type) {
		attachment_content_free(static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent));
	} else {
		message_content_free(static_cast<MESSAGE_CONTENT *>(pinstance->pcontent));
	}
	if (NULL != pinstance->username) {
		free(pinstance->username);
	}
	free(pinstance);
	return TRUE;
}

BOOL exmdb_server_get_instance_all_proptags(
	const char *dir, uint32_t instance_id,
	PROPTAG_ARRAY *pproptags)
{
	int i;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment = nullptr;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance) {
		return FALSE;
	}
	if (INSTANCE_TYPE_MESSAGE == pinstance->type) {
		pmsgctnt = static_cast<MESSAGE_CONTENT *>(pinstance->pcontent);
		pproptags->count = pmsgctnt->proplist.count + 6;
		if (NULL != pmsgctnt->children.prcpts) {
			pproptags->count ++;
		}
		if (NULL != pmsgctnt->children.pattachments) {
			pproptags->count ++;
		}
		pproptags->pproptag = cu_alloc<uint32_t>(pproptags->count);
		if (NULL == pproptags->pproptag) {
			pproptags->count = 0;
			return FALSE;
		}
		for (i=0; i<pmsgctnt->proplist.count; i++) {
			switch (pmsgctnt->proplist.ppropval[i].proptag) {
			case ID_TAG_BODY:
				pproptags->pproptag[i] = PR_BODY;
				break;
			case ID_TAG_BODY_STRING8:
				pproptags->pproptag[i] = PR_BODY_A;
				break;
			case ID_TAG_HTML:
				pproptags->pproptag[i] = PROP_TAG_HTML;
				break;
			case ID_TAG_RTFCOMPRESSED:
				pproptags->pproptag[i] = PROP_TAG_RTFCOMPRESSED;
				break;
			case ID_TAG_TRANSPORTMESSAGEHEADERS:
				pproptags->pproptag[i] = PROP_TAG_TRANSPORTMESSAGEHEADERS;
				break;
			case ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8:
				pproptags->pproptag[i] =
					PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8;
				break;
			default:
				pproptags->pproptag[i] =
					pmsgctnt->proplist.ppropval[i].proptag;
				break;
			}
		}
		pproptags->count = pmsgctnt->proplist.count;
		pproptags->pproptag[pproptags->count] = PROP_TAG_CODEPAGEID;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count] = PR_MESSAGE_SIZE;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count] = PROP_TAG_HASATTACHMENTS;
		pproptags->count ++;
		pproptags->pproptag[pproptags->count++] = PR_DISPLAY_TO;
		pproptags->pproptag[pproptags->count++] = PR_DISPLAY_CC;
		pproptags->pproptag[pproptags->count++] = PR_DISPLAY_BCC;
	} else {
		pattachment = static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent);
		pproptags->count = pattachment->proplist.count + 1;
		if (NULL != pattachment->pembedded) {
			pproptags->count ++;
		}
		pproptags->pproptag = cu_alloc<uint32_t>(pproptags->count);
		if (NULL == pproptags->pproptag) {
			pproptags->count = 0;
			return FALSE;
		}
		for (i=0; i<pattachment->proplist.count; i++) {
			switch (pattachment->proplist.ppropval[i].proptag) {
			case ID_TAG_ATTACHDATABINARY:
				pproptags->pproptag[i] = PR_ATTACH_DATA_BIN;
				break;
			case ID_TAG_ATTACHDATAOBJECT:
				pproptags->pproptag[i] = PR_ATTACH_DATA_OBJ;
				break;
			default:
				pproptags->pproptag[i] =
					pattachment->proplist.ppropval[i].proptag;
				break;
			}
		}
		pproptags->count = pattachment->proplist.count;
		pproptags->pproptag[pproptags->count] = PROP_TAG_ATTACHSIZE;
		pproptags->count ++;
	}
	return TRUE;
}

static BOOL instance_get_message_display_recipients(
	TARRAY_SET *prcpts, uint32_t cpid, uint32_t proptag,
	void **ppvalue)
{
	void *pvalue;
	char tmp_buff[64*1024];
	uint32_t recipient_type = 0;
	static uint8_t fake_empty;

	fake_empty = 0;
	switch (proptag) {
	case PR_DISPLAY_TO:
	case PR_DISPLAY_TO_A:
		recipient_type = RECIPIENT_TYPE_TO;
		break;
	case PR_DISPLAY_CC:
	case PR_DISPLAY_CC_A:
		recipient_type = RECIPIENT_TYPE_CC;
		break;
	case PR_DISPLAY_BCC:
	case PR_DISPLAY_BCC_A:
		recipient_type = RECIPIENT_TYPE_BCC;
		break;
	}
	size_t offset = 0;
	for (size_t i = 0; i < prcpts->count; ++i) {
		pvalue = tpropval_array_get_propval(
			prcpts->pparray[i], PROP_TAG_RECIPIENTTYPE);
		if (NULL == pvalue || *(uint32_t*)pvalue != recipient_type) {
			continue;
		}
		pvalue = tpropval_array_get_propval(prcpts->pparray[i], PR_DISPLAY_NAME);
		if (NULL == pvalue) {
			pvalue = tpropval_array_get_propval(prcpts->pparray[i], PR_DISPLAY_NAME_A);
			if (NULL != pvalue) {
				pvalue = static_cast<char *>(common_util_convert_copy(TRUE, cpid, static_cast<char *>(pvalue)));
			}
		}
		if (NULL == pvalue) {
			pvalue = tpropval_array_get_propval(prcpts->pparray[i], PR_SMTP_ADDRESS);
		}
		if (NULL == pvalue) {
			continue;
		}
		if (0 == offset) {
			offset = gx_snprintf(tmp_buff, GX_ARRAY_SIZE(tmp_buff), "%s",
			         static_cast<const char *>(pvalue));
		} else {
			offset += gx_snprintf(tmp_buff + offset,
			          GX_ARRAY_SIZE(tmp_buff) - offset, "; %s",
			          static_cast<const char *>(pvalue));
		}
	}
	if  (0 == offset) {
		*ppvalue = deconst(&fake_empty);
		return TRUE;
	}
	*ppvalue = PROP_TYPE(proptag) == PT_UNICODE ? common_util_dup(tmp_buff) :
	           common_util_convert_copy(FALSE, cpid, tmp_buff);
	return *ppvalue != nullptr ? TRUE : false;
}

static uint32_t instance_get_message_flags(MESSAGE_CONTENT *pmsgctnt)
{
	void *pvalue;
	TPROPVAL_ARRAY *pproplist;
	
	pproplist = &pmsgctnt->proplist;
	pvalue = tpropval_array_get_propval(pproplist, PR_MESSAGE_FLAGS);
	uint32_t message_flags = pvalue == nullptr ? 0 : *static_cast<uint32_t *>(pvalue);
	message_flags &= ~(MSGFLAG_READ | MSGFLAG_HASATTACH | MSGFLAG_FROMME |
	                 MSGFLAG_ASSOCIATED | MSGFLAG_RN_PENDING |
	                 MSGFLAG_NRN_PENDING);
	pvalue = tpropval_array_get_propval(pproplist, PR_READ);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		message_flags |= MSGFLAG_READ;
	}
	if (NULL != pmsgctnt->children.pattachments &&
		0 != pmsgctnt->children.pattachments->count) {
		message_flags |= MSGFLAG_HASATTACH;
	}
	pvalue = tpropval_array_get_propval(pproplist, PR_ASSOCIATED);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		message_flags |= MSGFLAG_ASSOCIATED;
	}
	pvalue = tpropval_array_get_propval(pproplist, PR_READ_RECEIPT_REQUESTED);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		message_flags |= MSGFLAG_RN_PENDING;
	}
	pvalue = tpropval_array_get_propval(pproplist, PR_NON_RECEIPT_NOTIFICATION_REQUESTED);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		message_flags |= MSGFLAG_NRN_PENDING;
	}
	return message_flags;
}

static BOOL instance_get_message_subject(
	TPROPVAL_ARRAY *pproplist, uint16_t cpid,
	uint32_t proptag, void **ppvalue)
{
	char *pvalue;
	const char *psubject_prefix, *pnormalized_subject;
	
	psubject_prefix = NULL;
	pnormalized_subject = NULL;
	pnormalized_subject = static_cast<char *>(tpropval_array_get_propval(pproplist, PR_NORMALIZED_SUBJECT));
	if (NULL == pnormalized_subject) {
		pvalue = static_cast<char *>(tpropval_array_get_propval(pproplist, PR_NORMALIZED_SUBJECT_A));
		if (NULL != pvalue) {
			pnormalized_subject =
				common_util_convert_copy(TRUE, cpid, pvalue);
		}
	}
	psubject_prefix = static_cast<char *>(tpropval_array_get_propval(
	                  pproplist, PR_SUBJECT_PREFIX));
	if (NULL == psubject_prefix) {
		pvalue = static_cast<char *>(tpropval_array_get_propval(pproplist,
		         PR_SUBJECT_PREFIX_A));
		if (NULL != pvalue) {
			psubject_prefix =
				common_util_convert_copy(TRUE, cpid, pvalue);
		}
	}
	if (NULL == pnormalized_subject && NULL == psubject_prefix) {
		*ppvalue = NULL;
		return TRUE;
	}
	if (NULL == pnormalized_subject) {
		pnormalized_subject = "";
	}
	if (NULL == psubject_prefix) {
		psubject_prefix = "";
	}
	pvalue = cu_alloc<char>(strlen(pnormalized_subject) + strlen(psubject_prefix) + 1);
	if (NULL == pvalue) {
		return FALSE;
	}
	strcpy(pvalue, psubject_prefix);
	strcat(pvalue, pnormalized_subject);
	if (PROP_TYPE(proptag) == PT_UNICODE) {
		*ppvalue = common_util_dup(pvalue);
		if (NULL == *ppvalue) {
			return FALSE;
		}
	} else {
		*ppvalue = common_util_convert_copy(FALSE, cpid, pvalue);
	}
	return TRUE;
}

static BOOL instance_get_attachment_properties(uint32_t cpid,
	const uint64_t *pmessage_id, ATTACHMENT_CONTENT *pattachment,
	const PROPTAG_ARRAY *pproptags, TPROPVAL_ARRAY *ppropvals)
{
	int i;
	BINARY *pbin;
	void *pvalue;
	uint32_t length;
	uint32_t proptag;
	uint16_t proptype;
	
	ppropvals->count = 0;
	ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	if (NULL == ppropvals->ppropval) {
		return FALSE;
	}
	for (i=0; i<pproptags->count; i++) {
		pvalue = tpropval_array_get_propval(
			&pattachment->proplist, pproptags->pproptag[i]);
		auto &vc = ppropvals->ppropval[ppropvals->count];
		if (NULL != pvalue) {
			vc.proptag = pproptags->pproptag[i];
			vc.pvalue = pvalue;
			ppropvals->count ++;
			continue;
		}
		vc.pvalue = NULL;
		if (PROP_TYPE(pproptags->pproptag[i]) == PT_STRING8) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_UNICODE);
			pvalue = tpropval_array_get_propval(
				&pattachment->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy(false,
				            cpid, static_cast<char *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_UNICODE) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_STRING8);
			pvalue = tpropval_array_get_propval(
				&pattachment->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy(TRUE,
				            cpid, static_cast<char *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_MV_STRING8) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_UNICODE);
			pvalue = tpropval_array_get_propval(
					&pattachment->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy_string_array(false,
				            cpid, static_cast<STRING_ARRAY *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_MV_UNICODE) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_STRING8);
			pvalue = tpropval_array_get_propval(
					&pattachment->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy_string_array(TRUE,
				            cpid, static_cast<STRING_ARRAY *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_UNSPECIFIED) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_UNICODE);
			pvalue = tpropval_array_get_propval(
				&pattachment->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				auto tp = cu_alloc<TYPED_PROPVAL>();
				vc.pvalue = tp;
				if (tp == nullptr)
					return FALSE;	
				tp->type = PT_UNICODE;
				tp->pvalue = pvalue;
			} else {
				proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_STRING8);
				pvalue = tpropval_array_get_propval(
					&pattachment->proplist, proptag);
				if (NULL != pvalue) {
					vc.proptag = pproptags->pproptag[i];
					auto tp = cu_alloc<TYPED_PROPVAL>();
					vc.pvalue = tp;
					if (tp == nullptr)
						return FALSE;	
					tp->type = PT_UNICODE;
					tp->pvalue = pvalue;
				}
			}
		}
		if (vc.pvalue != nullptr) {
			ppropvals->count ++;
			continue;
		}
		switch (pproptags->pproptag[i]) {
		case PROP_TAG_MID:
			if (NULL != pmessage_id) {
				auto pv = cu_alloc<uint64_t>();
				vc.pvalue = pv;
				if (pv == nullptr)
					return FALSE;
				*pv = rop_util_make_eid_ex(1, *pmessage_id);
				vc.proptag = pproptags->pproptag[i];
				ppropvals->count ++;
				continue;
			}
			break;
		case PROP_TAG_ATTACHSIZE:
			vc.proptag = pproptags->pproptag[i];
			length = common_util_calculate_attachment_size(pattachment);
			pvalue = cu_alloc<uint32_t>();
			if (NULL == pvalue) {
				return FALSE;
			}
			*(uint32_t*)pvalue = length;
			vc.pvalue = pvalue;
			ppropvals->count ++;
			continue;
		case CHANGE_PROP_TYPE(PR_ATTACH_DATA_BIN, PT_UNSPECIFIED):
			proptype = PT_BINARY;
			pbin = static_cast<BINARY *>(tpropval_array_get_propval(
			       &pattachment->proplist, PR_ATTACH_DATA_BIN));
			if (NULL == pbin) {
				pvalue = tpropval_array_get_propval(
							&pattachment->proplist,
							ID_TAG_ATTACHDATABINARY);
				if (NULL != pvalue) {
					pvalue = instance_read_cid_content(
							*(uint64_t*)pvalue, &length);
					if (NULL == pvalue) {
						return FALSE;
					}
					pbin = cu_alloc<BINARY>();
					if (NULL == pbin) {
						return FALSE;
					}
					pbin->cb = length;
					pbin->pv = pvalue;
				}
			}
			if (NULL == pbin) {
				proptype = PT_OBJECT;
				pbin = static_cast<BINARY *>(tpropval_array_get_propval(
				       &pattachment->proplist, PR_ATTACH_DATA_OBJ));
				if (NULL == pbin) {
					pvalue = tpropval_array_get_propval(
								&pattachment->proplist,
								ID_TAG_ATTACHDATAOBJECT);
					if (NULL != pvalue) {
						pvalue = instance_read_cid_content(
								*(uint64_t*)pvalue, &length);
						if (NULL == pvalue) {
							return FALSE;
						}
						pbin = cu_alloc<BINARY>();
						if (NULL == pbin) {
							return FALSE;
						}
						pbin->cb = length;
						pbin->pv = pvalue;
					}
				}
			}
			if (NULL != pbin) {
				vc.proptag = pproptags->pproptag[i];
				auto tp = cu_alloc<TYPED_PROPVAL>();
				vc.pvalue = tp;
				if (tp == nullptr)
					return FALSE;	
				tp->type = proptype;
				tp->pvalue = pbin;
				ppropvals->count ++;
				continue;
			}
			break;
		case PR_ATTACH_DATA_BIN:
		case PR_ATTACH_DATA_OBJ:
			if (pproptags->pproptag[i] == PR_ATTACH_DATA_BIN)
				pvalue = tpropval_array_get_propval(
					&pattachment->proplist, ID_TAG_ATTACHDATABINARY);
			else
				pvalue = tpropval_array_get_propval(
					&pattachment->proplist, ID_TAG_ATTACHDATAOBJECT);
			if (NULL != pvalue) {
				pvalue = instance_read_cid_content(
						*(uint64_t*)pvalue, &length);
				if (NULL == pvalue) {
					return FALSE;
				}
				pbin = cu_alloc<BINARY>();
				if (NULL == pbin) {
					return FALSE;
				}
				pbin->cb = length;
				pbin->pv = pvalue;
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = pbin;
				ppropvals->count ++;
				continue;
			}
			break;
		}
	}
	return TRUE;
}	

BOOL exmdb_server_get_instance_properties(
	const char *dir, uint32_t size_limit, uint32_t instance_id,
	const PROPTAG_ARRAY *pproptags, TPROPVAL_ARRAY *ppropvals)
{
	int i, j;
	void *pvalue;
	uint16_t propid;
	uint32_t length;
	uint32_t proptag;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance) {
		return FALSE;
	}
	if (INSTANCE_TYPE_ATTACHMENT == pinstance->type) {
		auto pinstance1 = instance_get_instance(pdb, pinstance->parent_id);
		if (NULL == pinstance1) {
			return FALSE;
		}
		pvalue = tpropval_array_get_propval(&((MESSAGE_CONTENT*)
				pinstance1->pcontent)->proplist, PROP_TAG_MID);
		if (FALSE == instance_get_attachment_properties(
		    pinstance->cpid, static_cast<uint64_t *>(pvalue),
		    static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent),
			pproptags, ppropvals)) {
			return FALSE;
		}
		return TRUE;
	}
	pmsgctnt = static_cast<MESSAGE_CONTENT *>(pinstance->pcontent);
	ppropvals->count = 0;
	ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	if (NULL == ppropvals->ppropval) {
		return FALSE;
	}
	for (i=0; i<pproptags->count; i++) {
		auto &vc = ppropvals->ppropval[ppropvals->count];
		if (pproptags->pproptag[i] == PR_MESSAGE_FLAGS) {
			vc.proptag = pproptags->pproptag[i];
			vc.pvalue = cu_alloc<uint32_t>();
			if (vc.pvalue == nullptr)
				return FALSE;
			*static_cast<uint32_t *>(vc.pvalue) = instance_get_message_flags(pmsgctnt);
			ppropvals->count ++;
			continue;
		}
		pvalue = tpropval_array_get_propval(
			&pmsgctnt->proplist, pproptags->pproptag[i]);
		if (NULL != pvalue) {
			vc.proptag = pproptags->pproptag[i];
			vc.pvalue = pvalue;
			ppropvals->count ++;
			continue;
		}
		vc.pvalue = nullptr;
		if (PROP_TYPE(pproptags->pproptag[i]) == PT_STRING8) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_UNICODE);
			pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy(false,
				            pinstance->cpid, static_cast<char *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_UNICODE) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_STRING8);
			pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy(TRUE,
				            pinstance->cpid, static_cast<char *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_MV_STRING8) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_UNICODE);
			pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy_string_array(false,
				            pinstance->cpid, static_cast<STRING_ARRAY *>(pvalue));
			}
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_MV_UNICODE) {
			proptag = CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_STRING8);
			pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, proptag);
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = common_util_convert_copy_string_array(TRUE,
				            pinstance->cpid, static_cast<STRING_ARRAY *>(pvalue));
			}	
		} else if (PROP_TYPE(pproptags->pproptag[i]) == PT_UNSPECIFIED) {
			propid = PROP_ID(pproptags->pproptag[i]);
			for (j=0; j<pmsgctnt->proplist.count; j++) {
				if (PROP_ID(pmsgctnt->proplist.ppropval[j].proptag) == propid) {
					vc.proptag = pproptags->pproptag[i];
					vc.pvalue = cu_alloc<TYPED_PROPVAL>();
					if (vc.pvalue == nullptr)
						return FALSE;	
					static_cast<TYPED_PROPVAL *>(vc.pvalue)->type = PROP_TYPE(pmsgctnt->proplist.ppropval[j].proptag);
					static_cast<TYPED_PROPVAL *>(vc.pvalue)->pvalue = pmsgctnt->proplist.ppropval[j].pvalue;
					break;
				}
			}
		}
		if (vc.pvalue != nullptr) {
			ppropvals->count ++;
			continue;
		}
		switch (pproptags->pproptag[i]) {
		case PR_BODY_A:
		case PR_BODY_W:
		case CHANGE_PROP_TYPE(PR_BODY, PT_UNSPECIFIED):
		case PR_HTML:
		case CHANGE_PROP_TYPE(PR_HTML, PT_UNSPECIFIED):
		case PR_RTF_COMPRESSED: {
			auto ret = instance_get_message_body(pmsgctnt, pproptags->pproptag[i], pinstance->cpid, ppropvals);
			if (ret < 0) {
				return false;
			}
			break;
		}
		case PROP_TAG_TRANSPORTMESSAGEHEADERS_UNSPECIFIED:
			pvalue = tpropval_array_get_propval(
				&pmsgctnt->proplist, ID_TAG_TRANSPORTMESSAGEHEADERS);
			if (NULL != pvalue) {
				pvalue = instance_read_cid_content(
						*(uint64_t*)pvalue, NULL);
				if (NULL == pvalue) {
					return FALSE;
				}
				vc.proptag = PROP_TAG_TRANSPORTMESSAGEHEADERS_UNSPECIFIED;
				vc.pvalue = cu_alloc<TYPED_PROPVAL>();
				if (vc.pvalue == nullptr)
					return FALSE;	
				static_cast<TYPED_PROPVAL *>(vc.pvalue)->type = PT_UNICODE;
				static_cast<TYPED_PROPVAL *>(vc.pvalue)->pvalue = static_cast<char *>(pvalue) + sizeof(int);
				ppropvals->count ++;
				continue;
			} else {
				pvalue = tpropval_array_get_propval(&pmsgctnt->proplist,
								ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8);
				if (NULL != pvalue) {
					pvalue = instance_read_cid_content(
							*(uint64_t*)pvalue, NULL);
					if (NULL == pvalue) {
						return FALSE;
					}
					vc.proptag = PROP_TAG_TRANSPORTMESSAGEHEADERS_UNSPECIFIED;
					vc.pvalue = cu_alloc<TYPED_PROPVAL>();
					if (vc.pvalue == nullptr)
						return FALSE;	
					static_cast<TYPED_PROPVAL *>(vc.pvalue)->type = PT_STRING8;
					static_cast<TYPED_PROPVAL *>(vc.pvalue)->pvalue = pvalue;
					ppropvals->count ++;
					continue;
				}
			}
			break;
		case PR_SUBJECT:
		case PR_SUBJECT_A:
			if (FALSE == instance_get_message_subject(
				&pmsgctnt->proplist, pinstance->cpid,
				pproptags->pproptag[i], &pvalue)) {
				return FALSE;	
			}
			if (NULL != pvalue) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = pvalue;
				ppropvals->count ++;
				continue;
			}
			break;
		case PROP_TAG_TRANSPORTMESSAGEHEADERS:
			pvalue = tpropval_array_get_propval(
				&pmsgctnt->proplist, ID_TAG_TRANSPORTMESSAGEHEADERS);
			if (NULL != pvalue) {
				pvalue = instance_read_cid_content(
						*(uint64_t*)pvalue, NULL);
				if (NULL == pvalue) {
					return FALSE;
				}
				vc.proptag = PROP_TAG_TRANSPORTMESSAGEHEADERS;
				vc.pvalue = static_cast<char *>(pvalue) + sizeof(int);
				ppropvals->count ++;
				continue;
			}
			pvalue = tpropval_array_get_propval(&pmsgctnt->proplist,
							ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8);
			if (NULL != pvalue) {
				pvalue = instance_read_cid_content(
						*(uint64_t*)pvalue, NULL);
				if (NULL == pvalue) {
					return FALSE;
				}
				vc.proptag = PROP_TAG_TRANSPORTMESSAGEHEADERS;
				vc.pvalue = common_util_convert_copy(TRUE,
				            pinstance->cpid, static_cast<char *>(pvalue));
				if (vc.pvalue != nullptr) {
					ppropvals->count ++;
					continue;	
				}
			}
			break;
		case PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8:
			pvalue = tpropval_array_get_propval(
				&pmsgctnt->proplist, ID_TAG_TRANSPORTMESSAGEHEADERS_STRING8);
			if (NULL != pvalue) {
				pvalue = instance_read_cid_content(
						*(uint64_t*)pvalue, NULL);
				if (NULL == pvalue) {
					return FALSE;
				}
				vc.proptag = PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8;
				vc.pvalue = pvalue;
				ppropvals->count ++;
				continue;
			}
			pvalue = tpropval_array_get_propval(
				&pmsgctnt->proplist, ID_TAG_TRANSPORTMESSAGEHEADERS);
			if (NULL != pvalue) {
				pvalue = instance_read_cid_content(
						*(uint64_t*)pvalue, NULL);
				if (NULL == pvalue) {
					return FALSE;
				}
				vc.proptag = PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8;
				vc.pvalue = common_util_convert_copy(false,
				            pinstance->cpid, static_cast<char *>(pvalue) + sizeof(int));
				if (vc.pvalue != nullptr) {
					ppropvals->count ++;
					continue;	
				}
			}
			break;
		case PROP_TAG_FOLDERID:
			if (0 == pinstance->parent_id) {
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = cu_alloc<uint64_t>();
				if (vc.pvalue == nullptr)
					return FALSE;	
				*static_cast<uint64_t *>(vc.pvalue) = rop_util_make_eid_ex(1, pinstance->folder_id);
				ppropvals->count ++;
				continue;
			}
			break;
		case PROP_TAG_CODEPAGEID:
			vc.proptag = pproptags->pproptag[i];
			vc.pvalue = &pinstance->cpid;
			ppropvals->count ++;
			continue;
		case PR_MESSAGE_SIZE:
		case PR_MESSAGE_SIZE_EXTENDED:
			vc.proptag = pproptags->pproptag[i];
			length = common_util_calculate_message_size(pmsgctnt);
			if (pproptags->pproptag[i] == PR_MESSAGE_SIZE) {
				pvalue = cu_alloc<uint32_t>();
				if (NULL == pvalue) {
					return FALSE;
				}
				*(uint32_t*)pvalue = length;
			} else {
				pvalue = cu_alloc<uint64_t>();
				if (NULL == pvalue) {
					return FALSE;
				}
				*(uint64_t*)pvalue = length;
			}
			vc.pvalue = pvalue;
			ppropvals->count ++;
			continue;
		case PROP_TAG_HASATTACHMENTS:
			vc.proptag = pproptags->pproptag[i];
			pvalue = cu_alloc<uint8_t>();
			if (NULL == pvalue) {
				return FALSE;
			}
			vc.pvalue = pvalue;
			ppropvals->count ++;
			*static_cast<uint8_t *>(pvalue) = pmsgctnt->children.pattachments == nullptr ||
				pmsgctnt->children.pattachments->count == 0 ? 0 : 1;
			continue;
		case PR_DISPLAY_TO:
		case PR_DISPLAY_TO_A:
		case PR_DISPLAY_CC:
		case PR_DISPLAY_CC_A:
		case PR_DISPLAY_BCC:
		case PR_DISPLAY_BCC_A:
			if (NULL != pmsgctnt->children.prcpts) {
				if (FALSE == instance_get_message_display_recipients(
					pmsgctnt->children.prcpts, pinstance->cpid,
					pproptags->pproptag[i], &pvalue)) {
					return FALSE;
				}
				vc.proptag = pproptags->pproptag[i];
				vc.pvalue = pvalue;
				ppropvals->count ++;
				continue;
			}
			break;
		}
	}
	return TRUE;
}

BOOL exmdb_server_set_instance_properties(const char *dir,
	uint32_t instance_id, const TPROPVAL_ARRAY *pproperties,
	PROBLEM_ARRAY *pproblems)
{
	int i;
	void *pvalue;
	uint8_t tmp_byte;
	uint32_t body_type;
	uint32_t message_flags;
	TAGGED_PROPVAL propval;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance) {
		return FALSE;
	}
	pproblems->count = 0;
	pproblems->pproblem = cu_alloc<PROPERTY_PROBLEM>(pproperties->count);
	if (NULL == pproblems->pproblem) {
		return FALSE;
	}
	if (INSTANCE_TYPE_MESSAGE == pinstance->type) {
		pmsgctnt = static_cast<MESSAGE_CONTENT *>(pinstance->pcontent);
		for (i=0; i<pproperties->count; i++) {
			switch (pproperties->ppropval[i].proptag) {
			case PR_ASSOCIATED:
				if (TRUE == pinstance->b_new) {
					break;
				}
			case ID_TAG_BODY:
			case ID_TAG_BODY_STRING8:
			case ID_TAG_HTML:
			case ID_TAG_RTFCOMPRESSED:
			case PROP_TAG_MID:
			case PR_ENTRYID:
			case PROP_TAG_FOLDERID:
			case PROP_TAG_CODEPAGEID:
			case PROP_TAG_PARENTFOLDERID:
			case PROP_TAG_INSTANCESVREID:
			case PROP_TAG_HASNAMEDPROPERTIES:
			case PR_MESSAGE_SIZE:
			case PROP_TAG_HASATTACHMENTS:
			case PR_DISPLAY_TO:
			case PR_DISPLAY_CC:
			case PR_DISPLAY_BCC:
			case PR_DISPLAY_TO_A:
			case PR_DISPLAY_CC_A:
			case PR_DISPLAY_BCC_A:
			case PROP_TAG_TRANSPORTMESSAGEHEADERS:
			case PROP_TAG_TRANSPORTMESSAGEHEADERS_STRING8:
				pproblems->pproblem[pproblems->count].index = i;
				pproblems->pproblem[pproblems->count].proptag =
								pproperties->ppropval[i].proptag;
				pproblems->pproblem[pproblems->count].err = ecAccessDenied;
				pproblems->count ++;
				continue;
			case PR_READ:
				if (0 != *(uint8_t*)pproperties->ppropval[i].pvalue) {
					pvalue = tpropval_array_get_propval(
					         &pmsgctnt->proplist, PR_MESSAGE_FLAGS);
					if (NULL != pvalue) {
						*static_cast<uint32_t *>(pvalue) |= MSGFLAG_EVERREAD;
					}
				}
				break;
			case PROP_TAG_MESSAGESTATUS:
				/* PidTagMessageStatus can only be
					set by RopSetMessageStatus */
				continue;
			case PR_MESSAGE_FLAGS:
				if (FALSE == pinstance->b_new) {
					pproblems->pproblem[pproblems->count].index = i;
					pproblems->pproblem[pproblems->count].proptag =
									pproperties->ppropval[i].proptag;
					pproblems->pproblem[pproblems->count].err = ecAccessDenied;
					pproblems->count ++;
					continue;
				}
				message_flags = *(uint32_t*)pproperties->ppropval[i].pvalue;
				if (message_flags & MSGFLAG_READ) {
					propval.proptag = PR_READ;
					propval.pvalue = &tmp_byte;
					tmp_byte = 1;
					if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
						return FALSE;
					}
				}
				if (message_flags & MSGFLAG_ASSOCIATED) {
					propval.proptag = PR_ASSOCIATED;
					propval.pvalue = &tmp_byte;
					tmp_byte = 1;
					if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
						return FALSE;
					}	
				}
				if (message_flags & MSGFLAG_RN_PENDING) {
					propval.proptag = PR_READ_RECEIPT_REQUESTED;
					propval.pvalue = &tmp_byte;
					tmp_byte = 1;
					if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
						return FALSE;
					}	
				}
				if (message_flags & MSGFLAG_NRN_PENDING) {
					propval.proptag = PR_NON_RECEIPT_NOTIFICATION_REQUESTED;
					propval.pvalue = &tmp_byte;
					tmp_byte = 1;
					if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
						return FALSE;
					}	
				}
				message_flags &= ~(MSGFLAG_READ | MSGFLAG_UNMODIFIED |
				                 MSGFLAG_HASATTACH | MSGFLAG_FROMME |
				                 MSGFLAG_ASSOCIATED | MSGFLAG_RN_PENDING |
				                 MSGFLAG_NRN_PENDING);
				*(uint32_t*)pproperties->ppropval[i].pvalue = message_flags;
				break;
			case PR_SUBJECT:
			case PR_SUBJECT_A:
				tpropval_array_remove_propval(&pmsgctnt->proplist, PR_SUBJECT_PREFIX);
				tpropval_array_remove_propval(&pmsgctnt->proplist, PR_SUBJECT_PREFIX_A);
				tpropval_array_remove_propval(&pmsgctnt->proplist, PR_NORMALIZED_SUBJECT);
				tpropval_array_remove_propval(&pmsgctnt->proplist, PR_NORMALIZED_SUBJECT_A);
				propval.proptag = PR_NORMALIZED_SUBJECT;
				if (pproperties->ppropval[i].proptag == PR_SUBJECT) {
					propval.pvalue = pproperties->ppropval[i].pvalue;
				} else {
					propval.pvalue = common_util_convert_copy(TRUE,
						pinstance->cpid, static_cast<char *>(pproperties->ppropval[i].pvalue));
					if (NULL == propval.pvalue) {
						return FALSE;
					}
				}
				if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
					return FALSE;
				}
				continue;
			case PR_BODY:
			case PR_BODY_A:
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_BODY);
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_BODY_STRING8);
				break;
			case PROP_TAG_HTML:
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_HTML);
				break;
			case PROP_TAG_RTFCOMPRESSED:
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_RTFCOMPRESSED);
				break;
			}
			switch (PROP_TYPE(pproperties->ppropval[i].proptag)) {
			case PT_STRING8:
			case PT_UNICODE:
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_STRING8));
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_UNICODE));
				propval.proptag = CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_UNICODE);
				if (PROP_TYPE(pproperties->ppropval[i].proptag) == PT_UNICODE) {
					propval.pvalue = pproperties->ppropval[i].pvalue;
				} else {
					propval.pvalue = common_util_convert_copy(TRUE,
						pinstance->cpid, static_cast<char *>(pproperties->ppropval[i].pvalue));
					if (NULL == propval.pvalue) {
						return FALSE;
					}
				}
				break;
			case PT_MV_STRING8:
			case PT_MV_UNICODE:
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_MV_STRING8));
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_MV_UNICODE));
				propval.proptag = CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_MV_UNICODE);
				if (PROP_TYPE(pproperties->ppropval[i].proptag) == PT_MV_UNICODE) {
					propval.pvalue = pproperties->ppropval[i].pvalue;
				} else {
					propval.pvalue = common_util_convert_copy_string_array(
										TRUE, pinstance->cpid,
					                 static_cast<STRING_ARRAY *>(pproperties->ppropval[i].pvalue));
					if (NULL == propval.pvalue) {
						return FALSE;
					}
				}
				break;
			default:
				propval = pproperties->ppropval[i];
				break;
			}
			if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
				return FALSE;
			}
			if (propval.proptag == PR_BODY ||
				PROP_TAG_HTML == propval.proptag ||
				PROP_TAG_BODYHTML == propval.proptag ||
				PROP_TAG_RTFCOMPRESSED == propval.proptag) {
				switch (propval.proptag) {
				case PR_BODY:
					pinstance->change_mask |= CHANGE_MASK_BODY;
					body_type = NATIVE_BODY_PLAIN;
					break;
				case PROP_TAG_HTML:
					pinstance->change_mask |= CHANGE_MASK_HTML;
					[[fallthrough]];
				case PROP_TAG_BODYHTML:
					body_type = NATIVE_BODY_HTML;
					break;
				case PROP_TAG_RTFCOMPRESSED:
					body_type = NATIVE_BODY_RTF;
					break;
				}
				propval.proptag = PROP_TAG_NATIVEBODY;
				propval.pvalue = &body_type;
				if (!tpropval_array_set_propval(&pmsgctnt->proplist, &propval)) {
					return FALSE;
				}
			}
		}
	} else {
		pattachment = static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent);
		for (i=0; i<pproperties->count; i++) {
			switch (pproperties->ppropval[i].proptag) {
			case ID_TAG_ATTACHDATABINARY:
			case ID_TAG_ATTACHDATAOBJECT:
			case PROP_TAG_ATTACHNUMBER:
			case PR_RECORD_KEY:
				pproblems->pproblem[pproblems->count].index = i;
				pproblems->pproblem[pproblems->count].proptag =
								pproperties->ppropval[i].proptag;
				pproblems->pproblem[pproblems->count].err = ecAccessDenied;
				pproblems->count ++;
				continue;
			case PR_ATTACH_DATA_BIN:
				tpropval_array_remove_propval(
					&pattachment->proplist, ID_TAG_ATTACHDATABINARY);
				break;
			case PR_ATTACH_DATA_OBJ:
				tpropval_array_remove_propval(
					&pattachment->proplist, ID_TAG_ATTACHDATAOBJECT);
				break;
			}
			switch (PROP_TYPE(pproperties->ppropval[i].proptag)) {
			case PT_STRING8:
			case PT_UNICODE:
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_STRING8));
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_UNICODE));
				propval.proptag = CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_UNICODE);
				if (PROP_TYPE(pproperties->ppropval[i].proptag) == PT_UNICODE) {
					propval.pvalue = pproperties->ppropval[i].pvalue;
				} else {
					propval.pvalue = common_util_convert_copy(TRUE,
						pinstance->cpid, static_cast<char *>(pproperties->ppropval[i].pvalue));
					if (NULL == propval.pvalue) {
						return FALSE;
					}
				}
				break;
			case PT_MV_STRING8:
			case PT_MV_UNICODE:
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_MV_STRING8));
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_MV_UNICODE));
				propval.proptag = CHANGE_PROP_TYPE(pproperties->ppropval[i].proptag, PT_MV_UNICODE);
				if (PROP_TYPE(pproperties->ppropval[i].proptag) == PT_MV_UNICODE) {
					propval.pvalue = pproperties->ppropval[i].pvalue;
				} else {
					propval.pvalue = common_util_convert_copy_string_array(
										TRUE, pinstance->cpid,
					                 static_cast<STRING_ARRAY *>(pproperties->ppropval[i].pvalue));
					if (NULL == propval.pvalue) {
						return FALSE;
					}
				}
				break;
			default:
				propval = pproperties->ppropval[i];
				break;
			}
			if (!tpropval_array_set_propval(&pattachment->proplist, &propval)) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

BOOL exmdb_server_remove_instance_properties(
	const char *dir, uint32_t instance_id,
	const PROPTAG_ARRAY *pproptags, PROBLEM_ARRAY *pproblems)
{
	int i;
	void *pvalue;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance) {
		return FALSE;
	}
	pproblems->count = 0;
	if (INSTANCE_TYPE_MESSAGE == pinstance->type) {
		pmsgctnt = static_cast<MESSAGE_CONTENT *>(pinstance->pcontent);
		for (i=0; i<pproptags->count; i++) {
			switch (pproptags->pproptag[i]) {
			case PR_BODY:
			case PR_BODY_A:
			case PROP_TAG_BODY_UNSPECIFIED:
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_BODY);
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_BODY_STRING8);
				if (NULL != (pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, PROP_TAG_NATIVEBODY)) &&
					NATIVE_BODY_PLAIN == *(uint32_t*)pvalue) {
					*(uint32_t*)pvalue = NATIVE_BODY_UNDEFINED;	
				}
				break;
			case PROP_TAG_HTML:
			case PROP_TAG_BODYHTML:
			case PROP_TAG_BODYHTML_STRING8:
			case PROP_TAG_HTML_UNSPECIFIED:
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, PROP_TAG_BODYHTML);
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, PROP_TAG_BODYHTML_STRING8);
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_HTML);
				if (NULL != (pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, PROP_TAG_NATIVEBODY)) &&
					NATIVE_BODY_HTML == *(uint32_t*)pvalue) {
					*(uint32_t*)pvalue = NATIVE_BODY_UNDEFINED;	
				}
				break;
			case PROP_TAG_RTFCOMPRESSED:
				if (NULL != (pvalue = tpropval_array_get_propval(
					&pmsgctnt->proplist, PROP_TAG_NATIVEBODY)) &&
					NATIVE_BODY_RTF == *(uint32_t*)pvalue) {
					*(uint32_t*)pvalue = NATIVE_BODY_UNDEFINED;	
				}
				tpropval_array_remove_propval(
					&pmsgctnt->proplist, ID_TAG_RTFCOMPRESSED);
				break;
			}
			tpropval_array_remove_propval(
				&pmsgctnt->proplist, pproptags->pproptag[i]);
			switch (PROP_TYPE(pproptags->pproptag[i])) {
			case PT_STRING8:
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_UNICODE));
				break;
			case PT_UNICODE:
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_STRING8));
				break;
			case PT_MV_STRING8:
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_UNICODE));
				break;
			case PT_MV_UNICODE:
				tpropval_array_remove_propval(&pmsgctnt->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_STRING8));
				break;
			}
		}
	} else {
		pattachment = static_cast<ATTACHMENT_CONTENT *>(pinstance->pcontent);
		for (i=0; i<pproptags->count; i++) {
			switch (pproptags->pproptag[i]) {
			case PR_ATTACH_DATA_BIN:
				tpropval_array_remove_propval(
					&pattachment->proplist, ID_TAG_ATTACHDATABINARY);
				break;
			case PR_ATTACH_DATA_OBJ:
				tpropval_array_remove_propval(
					&pattachment->proplist, ID_TAG_ATTACHDATAOBJECT);
				break;
			}
			tpropval_array_remove_propval(
				&pattachment->proplist, pproptags->pproptag[i]);
			switch (PROP_TYPE(pproptags->pproptag[i])) {
			case PT_STRING8:
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_UNICODE));
				break;
			case PT_UNICODE:
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_STRING8));
				break;
			case PT_MV_STRING8:
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_UNICODE));
				break;
			case PT_MV_UNICODE:
				tpropval_array_remove_propval(&pattachment->proplist,
					CHANGE_PROP_TYPE(pproptags->pproptag[i], PT_MV_STRING8));
				break;
			}
		}
	}
	return TRUE;
}

BOOL exmdb_server_check_instance_cycle(const char *dir,
	uint32_t src_instance_id, uint32_t dst_instance_id, BOOL *pb_cycle)
{
	if (src_instance_id == dst_instance_id) {
		*pb_cycle = TRUE;
		return TRUE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, dst_instance_id);
	while (NULL != pinstance && 0 != pinstance->parent_id) {
		if (pinstance->parent_id == src_instance_id) {
			*pb_cycle = TRUE;
			return TRUE;
		}
		pinstance = instance_get_instance(pdb, pinstance->parent_id);
	}
	*pb_cycle = FALSE;
	return TRUE;
}

BOOL exmdb_server_empty_message_instance_rcpts(
	const char *dir, uint32_t instance_id)
{
	MESSAGE_CONTENT *pmsgctnt;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL != pmsgctnt->children.prcpts) {
		tarray_set_free(pmsgctnt->children.prcpts);
		pmsgctnt->children.prcpts = NULL;
	}
	return TRUE;
}

BOOL exmdb_server_get_message_instance_rcpts_num(
	const char *dir, uint32_t instance_id, uint16_t *pnum)
{
	MESSAGE_CONTENT *pmsgctnt;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	*pnum = pmsgctnt->children.prcpts == nullptr ? 0 :
	        pmsgctnt->children.prcpts->count;
	return TRUE;
}

BOOL exmdb_server_get_message_instance_rcpts_all_proptags(
	const char *dir, uint32_t instance_id, PROPTAG_ARRAY *pproptags)
{
	TARRAY_SET *prcpts;
	MESSAGE_CONTENT *pmsgctnt;
	PROPTAG_ARRAY *pproptags1;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL == pmsgctnt->children.prcpts) {
		pproptags->count = 0;
		pproptags->pproptag = NULL;
		return TRUE;
	}
	pproptags1 = proptag_array_init();
	if (NULL == pproptags1) {
		return FALSE;
	}
	prcpts = pmsgctnt->children.prcpts;
	for (size_t i = 0; i < prcpts->count; ++i)
		for (size_t j = 0; j < prcpts->pparray[i]->count; ++j)
			if (!proptag_array_append(pproptags1,
			    prcpts->pparray[i]->ppropval[j].proptag)) {
				proptag_array_free(pproptags1);
				return FALSE;
			}
	pproptags->count = pproptags1->count;
	pproptags->pproptag = cu_alloc<uint32_t>(pproptags1->count);
	if (NULL == pproptags->pproptag) {
		proptag_array_free(pproptags1);
		return FALSE;
	}
	memcpy(pproptags->pproptag, pproptags1->pproptag,
				sizeof(uint32_t)*pproptags1->count);
	proptag_array_free(pproptags1);
	return TRUE;
}

BOOL exmdb_server_get_message_instance_rcpts(
	const char *dir, uint32_t instance_id, uint32_t row_id,
	uint16_t need_count, TARRAY_SET *pset)
{
	uint32_t *prow_id;
	TARRAY_SET *prcpts;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL == pmsgctnt->children.prcpts) {
		pset->count = 0;
		pset->pparray = NULL;
		return TRUE;
	}
	prcpts = pmsgctnt->children.prcpts;
	size_t i;
	for (i=0; i<prcpts->count; i++) {
		prow_id = static_cast<uint32_t *>(tpropval_array_get_propval(
		          prcpts->pparray[i], PROP_TAG_ROWID));
		if (NULL != prow_id && row_id == *prow_id) {
			break;
		}
	}
	if (i >= prcpts->count) {
		pset->count = 0;
		pset->pparray = NULL;
		return TRUE;
	}
	auto begin_pos = i;
	if (begin_pos + need_count > prcpts->count) {
		need_count = prcpts->count - begin_pos;
	}
	pset->count = need_count;
	pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(need_count);
	if (NULL == pset->pparray) {
		return FALSE;
	}
	for (i=0; i<need_count; i++) {
		pset->pparray[i] = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == pset->pparray[i]) {
			return FALSE;
		}
		pset->pparray[i]->count =
			prcpts->pparray[begin_pos + i]->count;
		pset->pparray[i]->ppropval = cu_alloc<TAGGED_PROPVAL>(pset->pparray[i]->count);
		if (NULL == pset->pparray[i]->ppropval) {
			pset->pparray[i]->count = 0;
			return FALSE;
		}
		memcpy(pset->pparray[i]->ppropval,
			prcpts->pparray[begin_pos + i]->ppropval,
			sizeof(TAGGED_PROPVAL)*pset->pparray[i]->count);
	}
	return TRUE;
}

/* if only PROP_TAG_ROWID in propvals, means delete this row */
BOOL exmdb_server_update_message_instance_rcpts(
	const char *dir, uint32_t instance_id, const TARRAY_SET *pset)
{
	uint32_t row_id;
	uint32_t *prow_id;
	TPROPVAL_ARRAY *prcpt;
	MESSAGE_CONTENT *pmsgctnt;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL == pmsgctnt->children.prcpts) {
		pmsgctnt->children.prcpts = tarray_set_init();
		if (NULL == pmsgctnt->children.prcpts) {
			return FALSE;
		}
	}
	for (size_t i = 0; i < pset->count; ++i) {
		prow_id = static_cast<uint32_t *>(tpropval_array_get_propval(
		          pset->pparray[i], PROP_TAG_ROWID));
		if (NULL == prow_id) {
			continue;
		}
		row_id = *prow_id;
		size_t j;
		for (j=0; j<pmsgctnt->children.prcpts->count; j++) {
			prow_id = static_cast<uint32_t *>(tpropval_array_get_propval(
				pmsgctnt->children.prcpts->pparray[j],
			          PROP_TAG_ROWID));
			if (NULL != prow_id && *prow_id == row_id) {
				if (1 == pset->pparray[i]->count) {
					tarray_set_remove(pmsgctnt->children.prcpts, j);
				} else {
					prcpt = tpropval_array_dup(pset->pparray[i]);
					if (NULL == prcpt) {
						return FALSE;
					}
					tpropval_array_free(
						pmsgctnt->children.prcpts->pparray[j]);
					pmsgctnt->children.prcpts->pparray[j] = prcpt;
				}
				break;
			}
		}
		if (j >= pmsgctnt->children.prcpts->count) {
			if (pmsgctnt->children.prcpts->count
				>= MAX_RECIPIENT_NUMBER) {
				return FALSE;
			}
			prcpt = tpropval_array_dup(pset->pparray[i]);
			if (NULL == prcpt) {
				return FALSE;
			}
			if (!tarray_set_append_internal(pmsgctnt->children.prcpts, prcpt)) {
				tpropval_array_free(prcpt);
				return FALSE;
			}
		}
	}
	return TRUE;
}

BOOL exmdb_server_copy_instance_rcpts(
	const char *dir, BOOL b_force, uint32_t src_instance_id,
	uint32_t dst_instance_id, BOOL *pb_result)
{
	TARRAY_SET *prcpts;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance_src = instance_get_instance(pdb, src_instance_id);
	if (NULL == pinstance_src ||
		INSTANCE_TYPE_MESSAGE != pinstance_src->type) {
		return FALSE;
	}
	if (NULL == ((MESSAGE_CONTENT*)
		pinstance_src->pcontent)->children.prcpts) {
		*pb_result = FALSE;
		return TRUE;
	}
	auto pinstance_dst = instance_get_instance(pdb, dst_instance_id);
	if (NULL == pinstance_dst ||
		INSTANCE_TYPE_MESSAGE != pinstance_dst->type) {
		return FALSE;
	}
	if (FALSE == b_force && NULL != ((MESSAGE_CONTENT*)
		pinstance_dst->pcontent)->children.prcpts) {
		*pb_result = FALSE;
		return TRUE;	
	}
	prcpts = tarray_set_dup(((MESSAGE_CONTENT*)
		pinstance_src->pcontent)->children.prcpts);
	if (NULL == prcpts) {
		return FALSE;
	}
	if (NULL != ((MESSAGE_CONTENT*)
		pinstance_dst->pcontent)->children.prcpts) {
		tarray_set_free(((MESSAGE_CONTENT*)
			pinstance_dst->pcontent)->children.prcpts);
	}
	((MESSAGE_CONTENT*)pinstance_dst->pcontent)->children.prcpts =
															prcpts;
	*pb_result = TRUE;
	return TRUE;
}

BOOL exmdb_server_empty_message_instance_attachments(
	const char *dir, uint32_t instance_id)
{
	MESSAGE_CONTENT *pmsgctnt;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL != pmsgctnt->children.pattachments) {
		attachment_list_free(pmsgctnt->children.pattachments);
		pmsgctnt->children.pattachments = NULL;
	}
	return TRUE;
}

BOOL exmdb_server_get_message_instance_attachments_num(
	const char *dir, uint32_t instance_id, uint16_t *pnum)
{
	MESSAGE_CONTENT *pmsgctnt;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	*pnum = pmsgctnt->children.pattachments == nullptr ? 0 :
	        pmsgctnt->children.pattachments->count;
	return TRUE;
}

BOOL exmdb_server_get_message_instance_attachment_table_all_proptags(
	const char *dir, uint32_t instance_id, PROPTAG_ARRAY *pproptags)
{
	int i, j;
	MESSAGE_CONTENT *pmsgctnt;
	PROPTAG_ARRAY *pproptags1;
	ATTACHMENT_LIST *pattachments;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL == pmsgctnt->children.pattachments) {
		pproptags->count = 0;
		pproptags->pproptag = NULL;
		return TRUE;
	}
	pproptags1 = proptag_array_init();
	if (NULL == pproptags1) {
		return FALSE;
	}
	pattachments = pmsgctnt->children.pattachments;
	for (i=0; i<pattachments->count; i++) {
		for (j=0; j<pattachments->pplist[i]->proplist.count; j++) {
			if (!proptag_array_append(pproptags1,
			    pattachments->pplist[i]->proplist.ppropval[j].proptag)) {
				proptag_array_free(pproptags1);
				return FALSE;
			}
		}
	}
	pproptags->count = pproptags1->count;
	pproptags->pproptag = cu_alloc<uint32_t>(pproptags1->count);
	if (NULL == pproptags->pproptag) {
		proptag_array_free(pproptags1);
		return FALSE;
	}
	memcpy(pproptags->pproptag, pproptags1->pproptag,
				sizeof(uint32_t)*pproptags1->count);
	proptag_array_free(pproptags1);
	return TRUE;
}

BOOL exmdb_server_copy_instance_attachments(
	const char *dir, BOOL b_force, uint32_t src_instance_id,
	uint32_t dst_instance_id, BOOL *pb_result)
{
	ATTACHMENT_LIST *pattachments;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance_src = instance_get_instance(pdb, src_instance_id);
	if (NULL == pinstance_src ||
		INSTANCE_TYPE_MESSAGE != pinstance_src->type) {
		return FALSE;
	}
	if (NULL == ((MESSAGE_CONTENT*)
		pinstance_src->pcontent)->children.pattachments) {
		*pb_result = FALSE;
		return TRUE;	
	}
	auto pinstance_dst = instance_get_instance(pdb, dst_instance_id);
	if (NULL == pinstance_dst ||
		INSTANCE_TYPE_MESSAGE != pinstance_dst->type) {
		return FALSE;
	}
	if (FALSE == b_force && NULL != ((MESSAGE_CONTENT*)
		pinstance_dst->pcontent)->children.pattachments) {
		*pb_result = FALSE;
		return TRUE;	
	}
	pattachments = attachment_list_dup(((MESSAGE_CONTENT*)
		pinstance_src->pcontent)->children.pattachments);
	if (NULL == pattachments) {
		return FALSE;
	}
	if (NULL != ((MESSAGE_CONTENT*)
		pinstance_dst->pcontent)->children.pattachments) {
		attachment_list_free(((MESSAGE_CONTENT*)
			pinstance_dst->pcontent)->children.pattachments);
	}
	((MESSAGE_CONTENT*)pinstance_dst->pcontent)->children.pattachments =
															pattachments;
	return TRUE;
}

BOOL exmdb_server_query_message_instance_attachment_table(
	const char *dir, uint32_t instance_id,
	const PROPTAG_ARRAY *pproptags, uint32_t start_pos,
	int32_t row_needed, TARRAY_SET *pset)
{
	int i;
	void *pvalue;
	int32_t end_pos;
	MESSAGE_CONTENT *pmsgctnt;
	ATTACHMENT_LIST *pattachments;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsgctnt = (MESSAGE_CONTENT*)pinstance->pcontent;
	if (NULL == pmsgctnt->children.pattachments ||
		0 == pmsgctnt->children.pattachments->count ||
		start_pos >= pmsgctnt->children.pattachments->count) {
		pset->count = 0;
		pset->pparray = NULL;
		return TRUE;
	}
	pvalue = tpropval_array_get_propval(
		&pmsgctnt->proplist, PROP_TAG_MID);
	pattachments = pmsgctnt->children.pattachments;
	if (row_needed > 0) {
		end_pos = start_pos + row_needed;
		if (end_pos >= pattachments->count) {
			end_pos = pattachments->count - 1;
		}
		pset->count = 0;
		pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(end_pos - start_pos + 1);
		if (NULL == pset->pparray) {
			return FALSE;
		}
		for (i=start_pos; i<=end_pos; i++) {
			pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pset->pparray[pset->count]) {
				return FALSE;
			}
			if (FALSE == instance_get_attachment_properties(
			    pinstance->cpid, static_cast<uint64_t *>(pvalue),
			    pattachments->pplist[i],
				pproptags, pset->pparray[pset->count])) {
				return FALSE;
			}
			pset->count ++;
		}
	} else {
		end_pos = start_pos + row_needed;
		if (end_pos < 0) {
			end_pos = 0;
		}
		pset->count = 0;
		pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(start_pos - end_pos + 1);
		if (NULL == pset->pparray) {
			return FALSE;
		}
		for (i=start_pos; i>=end_pos; i--) {
			pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pset->pparray[pset->count]) {
				return FALSE;
			}
			if (FALSE == instance_get_attachment_properties(
			    pinstance->cpid, static_cast<uint64_t *>(pvalue),
			    pattachments->pplist[i],
				pproptags, pset->pparray[pset->count])) {
				return FALSE;
			}
			pset->count ++;
		}
	}
	return TRUE;
}

BOOL exmdb_server_set_message_instance_conflict(const char *dir,
	uint32_t instance_id, const MESSAGE_CONTENT *pmsgctnt)
{
	void *pvalue;
	uint8_t tmp_byte;
	BOOL b_inconflict;
	uint32_t tmp_status;
	MESSAGE_CONTENT *pmsg;
	TAGGED_PROPVAL propval;
	MESSAGE_CONTENT msgctnt;
	MESSAGE_CONTENT *pembedded;
	ATTACHMENT_LIST *pattachments;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	auto pinstance = instance_get_instance(pdb, instance_id);
	if (NULL == pinstance || INSTANCE_TYPE_MESSAGE != pinstance->type) {
		return FALSE;
	}
	pmsg = (MESSAGE_CONTENT*)pinstance->pcontent;
	pvalue = tpropval_array_get_propval(
		&pmsg->proplist, PROP_TAG_MESSAGESTATUS);
	b_inconflict = FALSE;
	if (NULL != pvalue) {
		if (*(uint32_t*)pvalue & MESSAGE_STATUS_IN_CONFLICT) {
			b_inconflict = TRUE;
		}
	}
	if (FALSE == b_inconflict) {
		if (FALSE == instance_read_message(pmsg, &msgctnt)) {
			return FALSE;
		}
		if (NULL == pmsg->children.pattachments) {
			pattachments = attachment_list_init();
			if (NULL == pattachments) {
				return FALSE;
			}
			pmsg->children.pattachments = pattachments;
		} else {
			pattachments = pmsg->children.pattachments;
		}
		pattachment = attachment_content_init();
		if (NULL == pattachment) {
			return FALSE;
		}
		pembedded = message_content_dup(&msgctnt);
		if (NULL == pembedded) {
			attachment_content_free(pattachment);
			return FALSE;
		}
		tpropval_array_remove_propval(&pembedded->proplist, PROP_TAG_MID);
		attachment_content_set_embedded_internal(pattachment, pembedded);
		if (FALSE == attachment_list_append_internal(
			pattachments, pattachment)) {
			attachment_content_free(pattachment);
			return FALSE;
		}
		propval.proptag = PROP_TAG_INCONFLICT;
		propval.pvalue = &tmp_byte;
		tmp_byte = 1;
		if (!tpropval_array_set_propval(&pattachment->proplist, &propval))
			/* ignore; reevaluate another time */;
	} else {
		if (NULL == pmsg->children.pattachments) {
			pattachments = attachment_list_init();
			if (NULL == pattachments) {
				return FALSE;
			}
			pmsg->children.pattachments = pattachments;
		} else {
			pattachments = pmsg->children.pattachments;
		}
	}
	pattachment = attachment_content_init();
	if (NULL == pattachment) {
		return FALSE;
	}
	pembedded = message_content_dup(pmsgctnt);
	if (NULL == pembedded) {
		attachment_content_free(pattachment);
		return FALSE;
	}
	tpropval_array_remove_propval(&pembedded->proplist, PROP_TAG_MID);
	attachment_content_set_embedded_internal(pattachment, pembedded);
	if (FALSE == attachment_list_append_internal(
		pattachments, pattachment)) {
		attachment_content_free(pattachment);
		return FALSE;
	}
	propval.proptag = PROP_TAG_INCONFLICT;
	propval.pvalue = &tmp_byte;
	tmp_byte = 1;
	if (!tpropval_array_set_propval(&pattachment->proplist, &propval))
		/* ignore; reevaluate */;
	propval.proptag = PROP_TAG_MESSAGESTATUS;
	pvalue = tpropval_array_get_propval(
		&pmsg->proplist, PROP_TAG_MESSAGESTATUS);
	if (NULL == pvalue) {
		propval.pvalue = &tmp_status;
		tmp_status = MESSAGE_STATUS_IN_CONFLICT;
	} else {
		*(uint32_t*)pvalue |= MESSAGE_STATUS_IN_CONFLICT;
		propval.pvalue = pvalue;
	}
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval))
		/* ignore; reevaluate */;
	return TRUE;
}
