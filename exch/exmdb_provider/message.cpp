// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <libHX/string.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include "bounce_producer.h"
#include <gromox/database.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/hmacmd5.hpp>
#include <gromox/mapidefs.h>
#include <gromox/scope.hpp>
#include <gromox/svc_common.h>
#include <gromox/tpropval_array.hpp>
#include <gromox/proptag_array.hpp>
#include "exmdb_client.h"
#include "exmdb_server.h"
#include "common_util.h"
#include <gromox/ext_buffer.hpp>
#include "db_engine.h"
#include <gromox/rop_util.hpp>
#include <gromox/oxcmail.hpp>
#include <gromox/guid.hpp>
#include <gromox/util.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <cstdio>
#define UI(x) static_cast<unsigned int>(x)
#define LLU(x) static_cast<unsigned long long>(x)

#define MIN_BATCH_MESSAGE_NUM						20

using namespace std::string_literals;
using namespace gromox;

namespace {

struct RULE_NODE {
	DOUBLE_LIST_NODE node;
	uint32_t sequence;
	uint32_t state;
	uint64_t id;
	char *provider;
};

struct DAM_NODE {
	DOUBLE_LIST_NODE node;
	uint64_t rule_id;
	uint64_t folder_id;
	uint64_t message_id;
	char *provider;
	const ACTION_BLOCK *pblock;
};

struct MESSAGE_NODE {
	DOUBLE_LIST_NODE node;
	uint64_t folder_id;
	uint64_t message_id;
};

}

static BOOL message_rule_new_message(BOOL, const char *, const char *, uint32_t, sqlite3 *, uint64_t, uint64_t, const char *, DOUBLE_LIST *, DOUBLE_LIST *);

/* Caution: If a message is soft deleted from a public folder,
	it also should be removed from read_states! if someone's
	read stat is "unread", the item of this user should be
	removed from read_states */

/* can be used when submitting message */
BOOL exmdb_server_movecopy_message(const char *dir,
	int account_id, uint32_t cpid, uint64_t message_id,
	uint64_t dst_fid, uint64_t dst_id, BOOL b_move,
	BOOL *pb_result)
{
	XID tmp_xid;
	void *pvalue;
	BOOL b_result;
	BOOL b_update;
	uint64_t tmp_cn;
	uint64_t nt_time;
	uint64_t mid_val;
	uint64_t dst_val;
	uint64_t fid_val;
	int is_associated;
	uint64_t change_num;
	uint64_t parent_fid;
	char sql_string[256];
	uint32_t message_size;
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;
	TAGGED_PROPVAL tmp_propval;
	TAGGED_PROPVAL tmp_propvals[5];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == b_move &&
	    cu_check_msgsize_overflow(pdb->psqlite, PROP_TAG_STORAGEQUOTALIMIT) &&
		TRUE == common_util_check_msgcnt_overflow(pdb->psqlite)) {
		*pb_result = FALSE;
		return TRUE;		
	}
	mid_val = rop_util_get_gc_value(message_id);
	fid_val = rop_util_get_gc_value(dst_fid);
	dst_val = rop_util_get_gc_value(dst_id);
	if (FALSE == common_util_check_allocated_eid(
		pdb->psqlite, dst_val, &b_result)) {
		return FALSE;
	}
	if (FALSE == b_result) {
		*pb_result = FALSE;
		return TRUE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
	          "FROM messages WHERE message_id=%llu", LLU(dst_val));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		*pb_result = FALSE;
		return TRUE;
	}
	pstmt.finalize();
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "SELECT parent_fid, is_associated"
	          " FROM messages WHERE message_id=%llu", LLU(mid_val));
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		pstmt.finalize();
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		*pb_result = FALSE;
		return TRUE;
	}
	parent_fid = sqlite3_column_int64(pstmt, 0);
	is_associated = sqlite3_column_int64(pstmt, 1);
	pstmt.finalize();
	if (TRUE == b_move) {
		db_engine_proc_dynamic_event(pdb, cpid,
			DYNAMIC_EVENT_DELETE_MESSAGE,
			parent_fid, mid_val, 0);
	}
	if (FALSE == common_util_copy_message(
		pdb->psqlite, account_id, mid_val,
		fid_val, &dst_val, &b_result, &message_size)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (FALSE == b_result) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		*pb_result = FALSE;
		return TRUE;
	}
	db_engine_proc_dynamic_event(pdb, cpid,
		DYNAMIC_EVENT_NEW_MESSAGE, fid_val, dst_val, 0);
	if (FALSE == b_move) {
		db_engine_notify_message_movecopy(pdb, TRUE,
				fid_val, dst_val, parent_fid, mid_val);
	} else {
		db_engine_notify_message_movecopy(pdb, FALSE,
				fid_val, dst_val, parent_fid, mid_val);
	}
	b_update = TRUE;
	if (TRUE == b_move) {
		if (TRUE == exmdb_server_check_private()) {
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM messages"
			        " WHERE message_id=%llu", LLU(mid_val));
			if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
				sql_string, NULL, NULL, NULL)) {
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
			b_update = FALSE;
		} else {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET "
			        "is_deleted=1 WHERE message_id=%llu", LLU(mid_val));
			if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
				sql_string, NULL, NULL, NULL)) {
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM "
			          "read_states message_id=%llu", LLU(mid_val));
			if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
				sql_string, NULL, NULL, NULL)) {
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
		}
	}
	if (TRUE == b_update) {
		if (0 == is_associated) {
			if (FALSE == common_util_increase_store_size(
				pdb->psqlite, message_size, 0)) {
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
		} else {
			if (FALSE == common_util_increase_store_size(
				pdb->psqlite, 0, message_size)) {
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
		}
	}
	nt_time = rop_util_current_nttime();
	if (TRUE == b_move) {
		propvals.count = 5;
		propvals.ppropval = tmp_propvals;
		if (FALSE == common_util_allocate_cn(pdb->psqlite, &change_num)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		tmp_cn = rop_util_make_eid_ex(1, change_num);
		tmp_propvals[0].proptag = PROP_TAG_CHANGENUMBER;
		tmp_propvals[0].pvalue = &tmp_cn;
		tmp_xid.guid = exmdb_server_check_private() ?
		               rop_util_make_user_guid(account_id) :
		               rop_util_make_domain_guid(account_id);
		rop_util_value_to_gc(change_num, tmp_xid.local_id);
		tmp_propvals[1].proptag = PR_CHANGE_KEY;
		tmp_propvals[1].pvalue = common_util_xid_to_binary(22, &tmp_xid);
		if (tmp_propvals[1].pvalue == nullptr ||
		    !common_util_get_property(FOLDER_PROPERTIES_TABLE,
		     parent_fid, 0, pdb->psqlite, PR_PREDECESSOR_CHANGE_LIST,
		     &pvalue)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		tmp_propvals[2].proptag = PR_PREDECESSOR_CHANGE_LIST;
		tmp_propvals[2].pvalue = common_util_pcl_append(static_cast<BINARY *>(pvalue),
		                         static_cast<BINARY *>(tmp_propvals[1].pvalue));
		if (NULL == tmp_propvals[2].pvalue) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		nt_time = rop_util_current_nttime();
		tmp_propvals[3].proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
		tmp_propvals[3].pvalue = &nt_time;
		tmp_propvals[4].proptag = PR_LAST_MODIFICATION_TIME;
		tmp_propvals[4].pvalue = &nt_time;
		common_util_set_properties(FOLDER_PROPERTIES_TABLE,
			parent_fid, 0, pdb->psqlite, &propvals, &problems);
		common_util_increase_deleted_count(pdb->psqlite, parent_fid, 1);
	}
	tmp_propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		fid_val, 0, pdb->psqlite, &tmp_propval, &b_result);
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	*pb_result = TRUE;
	return TRUE;
}

BOOL exmdb_server_movecopy_messages(const char *dir,
	int account_id, uint32_t cpid, BOOL b_guest,
	const char *username, uint64_t src_fid, uint64_t dst_fid,
	BOOL b_copy, const EID_ARRAY *pmessage_ids, BOOL *pb_partial)
{
	XID tmp_xid;
	void *pvalue;
	BOOL b_check;
	BOOL b_owner;
	BOOL b_result;
	BOOL b_update;
	uint64_t tmp_cn;
	uint64_t nt_time;
	uint64_t src_val;
	uint64_t dst_val;
	uint64_t tmp_val;
	uint64_t tmp_val1;
	int is_associated;
	uint64_t fai_size;
	uint32_t del_count;
	uint64_t change_num, parent_fid = 0;
	uint32_t permission;
	xstmt pstmt1;
	char sql_string[256];
	uint32_t folder_type;
	uint64_t normal_size;
	uint32_t message_size;
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;
	TAGGED_PROPVAL tmp_propval;
	TAGGED_PROPVAL tmp_propvals[5];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	*pb_partial = FALSE;
	src_val = rop_util_get_gc_value(src_fid);
	dst_val = rop_util_get_gc_value(dst_fid);
	if (!common_util_get_folder_type(pdb->psqlite, src_val, &folder_type, dir))
		return FALSE;
	if (TRUE == b_guest) {
		if (folder_type != FOLDER_SEARCH) {
			if (FALSE == common_util_check_folder_permission(
				pdb->psqlite, src_val, username, &permission)) {
				return FALSE;
			}
			b_check = (permission & (frightsOwner | frightsReadAny)) ? false : TRUE;
		} else {
			b_check = TRUE;
		}
	} else {
		b_check = FALSE;
	}
	BOOL b_batch = pmessage_ids->count >= MIN_BATCH_MESSAGE_NUM ? TRUE : false;
	if (TRUE == b_batch) {
		db_engine_begin_batch_mode(pdb);
	}
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "SELECT parent_fid, "
		"is_associated FROM messages WHERE message_id=?");
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		if (TRUE == b_batch) {
			db_engine_cancel_batch_mode(pdb);
		}
		return FALSE;
	}
	b_update = TRUE;
	if (FALSE == b_copy) {
		if (TRUE == exmdb_server_check_private()) {
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM "
						"messages WHERE message_id=?");
			b_update = FALSE;
		} else {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE messages"
					" SET is_deleted=1 WHERE message_id=?");
		}
		pstmt1 = gx_sql_prep(pdb->psqlite, sql_string);
		if (pstmt1 == nullptr) {
			pstmt.finalize();
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			if (TRUE == b_batch) {
				db_engine_cancel_batch_mode(pdb);
			}
			return FALSE;
		}
	}
	fai_size = 0;
	del_count = 0;
	normal_size = 0;
	for (size_t i = 0; i < pmessage_ids->count; ++i) {
		tmp_val = rop_util_get_gc_value(pmessage_ids->pids[i]);
		sqlite3_bind_int64(pstmt, 1, tmp_val);
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			*pb_partial = TRUE;
			continue;
		}
		parent_fid = sqlite3_column_int64(pstmt, 0);
		is_associated = sqlite3_column_int64(pstmt, 1);
		sqlite3_reset(pstmt);
		if (folder_type == FOLDER_SEARCH) {
			if (TRUE == b_check) {
				if (FALSE == common_util_check_folder_permission(
					pdb->psqlite, parent_fid, username, &permission)) {
					goto MVCP_FAILURE;
				}
				if (!(permission & (frightsOwner | frightsReadAny))) {
					if (FALSE == common_util_check_message_owner(
						pdb->psqlite, tmp_val, username, &b_owner)) {
						goto MVCP_FAILURE;
					}
					if (FALSE == b_owner) {
						*pb_partial = TRUE;
						continue;
					}
				}
			}
		} else {
			if (parent_fid != src_val) {
				*pb_partial = TRUE;
				continue;
			}
			if (TRUE == b_check) {
				if (FALSE == common_util_check_message_owner(
					pdb->psqlite, tmp_val, username, &b_owner)) {
					goto MVCP_FAILURE;
				}
				if (FALSE == b_owner) {
					*pb_partial = TRUE;
					continue;
				}
			}
		}
		if (FALSE == b_copy) {
			db_engine_proc_dynamic_event(pdb, cpid,
				DYNAMIC_EVENT_DELETE_MESSAGE,
				parent_fid, tmp_val, 0);
		}
		tmp_val1 = 0;
		if (FALSE == common_util_copy_message(
			pdb->psqlite, account_id, tmp_val, dst_val,
			&tmp_val1, &b_result, &message_size)) {
			pstmt.finalize();
			if (FALSE == b_copy) {
				pstmt1.finalize();
			}
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			if (TRUE == b_batch) {
				db_engine_cancel_batch_mode(pdb);
			}
			return FALSE;
		}
		if (FALSE == b_result) {
			*pb_partial = TRUE;
			continue;
		}
		if (0 == is_associated) {
			normal_size += message_size;
		} else {
			fai_size += message_size;
		}
		db_engine_proc_dynamic_event(pdb, cpid,
			DYNAMIC_EVENT_NEW_MESSAGE,
			dst_val, tmp_val1, 0);
		db_engine_notify_message_movecopy(pdb, b_copy,
				dst_val, tmp_val1, src_val, tmp_val);
		if (FALSE == b_copy) {
			del_count ++;
			sqlite3_bind_int64(pstmt1, 1, tmp_val);
			if (SQLITE_DONE != sqlite3_step(pstmt1)) {
				goto MVCP_FAILURE;
			}
			sqlite3_reset(pstmt1);
			if (FALSE == exmdb_server_check_private()) {
				snprintf(sql_string, arsizeof(sql_string), "DELETE FROM read_states"
				        " WHERE message_id=%llu", LLU(tmp_val));
				if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
					sql_string, NULL, NULL, NULL)) {
					goto MVCP_FAILURE;
				}
			}
		}
	}
	pstmt.finalize();
	if (FALSE == b_copy) {
		pstmt1.finalize();
	}
	if (TRUE == b_update && normal_size + fai_size > 0) {
		if (FALSE == common_util_increase_store_size(
			pdb->psqlite, normal_size, fai_size)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			if (TRUE == b_batch) {
				db_engine_cancel_batch_mode(pdb);
			}
			return FALSE;
		}
	}
	nt_time = rop_util_current_nttime();
	if (FALSE == b_copy) {
		propvals.count = 5;
		propvals.ppropval = tmp_propvals;
		if (FALSE == common_util_allocate_cn(pdb->psqlite, &change_num)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		tmp_cn = rop_util_make_eid_ex(1, change_num);
		tmp_propvals[0].proptag = PROP_TAG_CHANGENUMBER;
		tmp_propvals[0].pvalue = &tmp_cn;
		tmp_xid.guid = exmdb_server_check_private() ?
		               rop_util_make_user_guid(account_id) :
		               rop_util_make_domain_guid(account_id);
		rop_util_value_to_gc(change_num, tmp_xid.local_id);
		tmp_propvals[1].proptag = PR_CHANGE_KEY;
		tmp_propvals[1].pvalue = common_util_xid_to_binary(22, &tmp_xid);
		if (tmp_propvals[1].pvalue == nullptr ||
		    !common_util_get_property(FOLDER_PROPERTIES_TABLE,
		    parent_fid, 0, pdb->psqlite, PR_PREDECESSOR_CHANGE_LIST,
		    &pvalue)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		tmp_propvals[2].proptag = PR_PREDECESSOR_CHANGE_LIST;
		tmp_propvals[2].pvalue = common_util_pcl_append(static_cast<BINARY *>(pvalue),
		                         static_cast<BINARY *>(tmp_propvals[1].pvalue));
		if (NULL == tmp_propvals[2].pvalue) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		nt_time = rop_util_current_nttime();
		tmp_propvals[3].proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
		tmp_propvals[3].pvalue = &nt_time;
		tmp_propvals[4].proptag = PR_LAST_MODIFICATION_TIME;
		tmp_propvals[4].pvalue = &nt_time;
		common_util_set_properties(FOLDER_PROPERTIES_TABLE,
			parent_fid, 0, pdb->psqlite, &propvals, &problems);
		common_util_increase_deleted_count(
			pdb->psqlite, parent_fid, del_count);
	}
	tmp_propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		dst_val, 0, pdb->psqlite, &tmp_propval, &b_result);
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	if (TRUE == b_batch) {
		db_engine_commit_batch_mode(std::move(pdb));
	}
	return TRUE;

 MVCP_FAILURE:
	pstmt.finalize();
	if (FALSE == b_copy) {
		pstmt1.finalize();
	}
	sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
	if (TRUE == b_batch) {
		db_engine_cancel_batch_mode(pdb);
	}
	return FALSE;
}

BOOL exmdb_server_delete_messages(const char *dir,
	int account_id, uint32_t cpid, const char *username,
	uint64_t folder_id, const EID_ARRAY *pmessage_ids,
	BOOL b_hard, BOOL *pb_partial)
{
	XID tmp_xid;
	void *pvalue;
	BOOL b_check;
	BOOL b_owner;
	int del_count;
	uint64_t tmp_cn;
	uint64_t nt_time;
	uint64_t src_val;
	uint64_t tmp_val;
	uint64_t fai_size;
	uint32_t permission;
	uint64_t parent_fid;
	uint64_t change_num;
	char sql_string[256];
	uint32_t folder_type;
	uint64_t normal_size;
	PROBLEM_ARRAY problems;
	TPROPVAL_ARRAY propvals;
	TAGGED_PROPVAL tmp_propvals[5];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	*pb_partial = FALSE;
	if (TRUE == exmdb_server_check_private()) {
		b_hard = TRUE;
	}
	src_val = rop_util_get_gc_value(folder_id);
	if (FALSE == common_util_get_folder_type(
		pdb->psqlite, src_val, &folder_type)) {
		return FALSE;
	}
	if (NULL != username) {
		if (folder_type != FOLDER_SEARCH) {
			if (FALSE == common_util_check_folder_permission(
				pdb->psqlite, src_val, username, &permission)) {
				return FALSE;
			}
			b_check = (permission & (frightsOwner | frightsDeleteAny)) ? false : TRUE;
		} else {
			b_check = TRUE;
		}
	} else {
		b_check = FALSE;
	}
	BOOL b_batch = pmessage_ids->count >= MIN_BATCH_MESSAGE_NUM ? TRUE : false;
	if (TRUE == b_batch) {
		db_engine_begin_batch_mode(pdb);
	}
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "SELECT parent_fid, is_associated, "
					"message_size FROM messages WHERE message_id=?");
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		if (TRUE == b_batch) {
			db_engine_cancel_batch_mode(pdb);
		}
		return FALSE;
	}
	if (TRUE == b_hard) {
		snprintf(sql_string, arsizeof(sql_string), "DELETE FROM "
					"messages WHERE message_id=?");
	} else {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages"
				" SET is_deleted=1 WHERE message_id=?");
	}
	auto pstmt1 = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt1 == nullptr) {
		pstmt.finalize();
		sqlite3_exec(pdb->psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		if (TRUE == b_batch) {
			db_engine_cancel_batch_mode(pdb);
		}
		return FALSE;
	}
	fai_size = 0;
	del_count = 0;
	normal_size = 0;
	for (size_t i = 0; i < pmessage_ids->count; ++i) {
		tmp_val = rop_util_get_gc_value(pmessage_ids->pids[i]);
		sqlite3_bind_int64(pstmt, 1, tmp_val);
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			continue;
		}
		parent_fid = sqlite3_column_int64(pstmt, 0);
		if (0 == sqlite3_column_int64(pstmt, 1)) {
			normal_size += sqlite3_column_int64(pstmt, 2);
		} else {
			fai_size += sqlite3_column_int64(pstmt, 2);
		}
		sqlite3_reset(pstmt);
		if (folder_type == FOLDER_SEARCH) {
			if (TRUE == b_check) {
				if (FALSE == common_util_check_folder_permission(
					pdb->psqlite, parent_fid, username, &permission)) {
					pstmt.finalize();
					pstmt1.finalize();
					sqlite3_exec(pdb->psqlite,
						"ROLLBACK", NULL, NULL, NULL);
					if (TRUE == b_batch) {
						db_engine_cancel_batch_mode(pdb);
					}
					return FALSE;
				}
				if (!(permission & (frightsOwner | frightsDeleteAny))) {
					if (FALSE == common_util_check_message_owner(
						pdb->psqlite, tmp_val, username, &b_owner)) {
						pstmt.finalize();
						pstmt1.finalize();
						sqlite3_exec(pdb->psqlite,
							"ROLLBACK", NULL, NULL, NULL);
						if (TRUE == b_batch) {
							db_engine_cancel_batch_mode(pdb);
						}
						return FALSE;
					}
					if (FALSE == b_owner) {
						*pb_partial = TRUE;
						continue;
					}
				}
			}
		} else {
			if (parent_fid != src_val) {
				*pb_partial = TRUE;
				continue;
			}
			if (TRUE == b_check) {
				if (FALSE == common_util_check_message_owner(
					pdb->psqlite, tmp_val, username, &b_owner)) {
					pstmt.finalize();
					pstmt1.finalize();
					sqlite3_exec(pdb->psqlite,
						"ROLLBACK", NULL, NULL, NULL);
					if (TRUE == b_batch) {
						db_engine_cancel_batch_mode(pdb);
					}
					return FALSE;
				}
				if (FALSE == b_owner) {
					*pb_partial = TRUE;
					continue;
				}
			}
		}
		del_count ++;
		db_engine_proc_dynamic_event(pdb, cpid,
			DYNAMIC_EVENT_DELETE_MESSAGE,
			parent_fid, tmp_val, 0);
		if (folder_type == FOLDER_SEARCH)
			db_engine_notify_link_deletion(pdb, src_val, tmp_val);
		else
			db_engine_notify_message_deletion(pdb, src_val, tmp_val);
		sqlite3_bind_int64(pstmt1, 1, tmp_val);
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			pstmt.finalize();
			pstmt1.finalize();
			sqlite3_exec(pdb->psqlite,
				"ROLLBACK", NULL, NULL, NULL);
			if (TRUE == b_batch) {
				db_engine_cancel_batch_mode(pdb);
			}
			return FALSE;
		}
		sqlite3_reset(pstmt1);
		if (FALSE == b_hard) {
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM read_states"
			        " WHERE message_id=%llu", LLU(tmp_val));
			if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
				sql_string, NULL, NULL, NULL)) {
				pstmt.finalize();
				pstmt1.finalize();
				sqlite3_exec(pdb->psqlite,
					"ROLLBACK", NULL, NULL, NULL);
				if (TRUE == b_batch) {
					db_engine_cancel_batch_mode(pdb);
				}
				return FALSE;
			}
		}
	}
	pstmt.finalize();
	pstmt1.finalize();
	if (TRUE == b_hard) {
		if (FALSE == common_util_decrease_store_size(
			pdb->psqlite, normal_size, fai_size)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			if (TRUE == b_batch) {
				db_engine_cancel_batch_mode(pdb);
			}
			return FALSE;
		}
	}
	propvals.count = 5;
	propvals.ppropval = tmp_propvals;
	if (FALSE == common_util_allocate_cn(pdb->psqlite, &change_num)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	tmp_cn = rop_util_make_eid_ex(1, change_num);
	tmp_propvals[0].proptag = PROP_TAG_CHANGENUMBER;
	tmp_propvals[0].pvalue = &tmp_cn;
	tmp_xid.guid = exmdb_server_check_private() ?
	               rop_util_make_user_guid(account_id) :
	               rop_util_make_domain_guid(account_id);
	rop_util_value_to_gc(change_num, tmp_xid.local_id);
	tmp_propvals[1].proptag = PR_CHANGE_KEY;
	tmp_propvals[1].pvalue = common_util_xid_to_binary(22, &tmp_xid);
	if (tmp_propvals[1].pvalue == nullptr ||
	    !common_util_get_property(FOLDER_PROPERTIES_TABLE, src_val, 0,
	    pdb->psqlite, PR_PREDECESSOR_CHANGE_LIST, &pvalue)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	tmp_propvals[2].proptag = PR_PREDECESSOR_CHANGE_LIST;
	tmp_propvals[2].pvalue = common_util_pcl_append(static_cast<BINARY *>(pvalue),
	                         static_cast<BINARY *>(tmp_propvals[1].pvalue));
	if (NULL == tmp_propvals[2].pvalue) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	nt_time = rop_util_current_nttime();
	tmp_propvals[3].proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propvals[3].pvalue = &nt_time;
	tmp_propvals[4].proptag = PR_LAST_MODIFICATION_TIME;
	tmp_propvals[4].pvalue = &nt_time;
	common_util_set_properties(FOLDER_PROPERTIES_TABLE,
		src_val, 0, pdb->psqlite, &propvals, &problems);
	common_util_increase_deleted_count(
		pdb->psqlite, src_val, del_count);
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	if (TRUE == b_batch) {
		db_engine_commit_batch_mode(std::move(pdb));
	}
	return TRUE;
}

static BOOL message_get_message_rcpts(sqlite3 *psqlite,
	uint64_t message_id, TARRAY_SET *pset)
{
	uint32_t row_id;
	uint64_t rcpt_id;
	uint32_t rcpt_num;
	char sql_string[256];
	PROPTAG_ARRAY proptags;
	TAGGED_PROPVAL *ppropval;
	uint32_t tmp_proptags[0x8000];
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(*) FROM"
	          " recipients WHERE message_id=%llu", LLU(message_id));
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	rcpt_num = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	pset->count = 0;
	if (0 == rcpt_num) {
		pset->pparray = NULL;
		return TRUE;
	}
	pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(rcpt_num);
	if (NULL == pset->pparray) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT recipient_id FROM"
	          " recipients WHERE message_id=%llu", LLU(message_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	snprintf(sql_string, arsizeof(sql_string), "SELECT proptag FROM"
		" recipients_properties WHERE recipient_id=?");
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	row_id = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		rcpt_id = sqlite3_column_int64(pstmt, 0);
		sqlite3_bind_int64(pstmt1, 1, rcpt_id);
		proptags.count = 0;
		proptags.pproptag = tmp_proptags;
		while (SQLITE_ROW == sqlite3_step(pstmt1)) {
			tmp_proptags[proptags.count] = 
				sqlite3_column_int64(pstmt1, 0);
			proptags.count ++;
		}
		tmp_proptags[proptags.count] = PROP_TAG_ROWID;
		proptags.count ++;
		sqlite3_reset(pstmt1);
		pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == pset->pparray[pset->count] ||
			FALSE == common_util_get_properties(
			RECIPIENT_PROPERTIES_TABLE,
			rcpt_id, 0, psqlite, &proptags,
			pset->pparray[pset->count])) {
			return FALSE;
		}
		/* PROP_TAG_ROWID MUST be the first */
		memmove(pset->pparray[pset->count]->ppropval + 1,
			pset->pparray[pset->count]->ppropval, sizeof(
			TAGGED_PROPVAL)*pset->pparray[pset->count]->count);
		ppropval = pset->pparray[pset->count]->ppropval;
		pset->pparray[pset->count]->count ++;
		ppropval->proptag = PROP_TAG_ROWID;
		ppropval->pvalue = cu_alloc<uint32_t>();
		if (NULL == ppropval->pvalue) {
			return FALSE;
		}
		*(uint32_t*)ppropval->pvalue = row_id;
		row_id ++;
		pset->count ++;
	}
	return TRUE;
}

BOOL exmdb_server_get_message_brief(const char *dir, uint32_t cpid,
	uint64_t message_id, MESSAGE_CONTENT **ppbrief)
{
	uint32_t count;
	uint64_t mid_val;
	char sql_string[256];
	PROPTAG_ARRAY proptags;
	uint64_t attachment_id;
	uint32_t proptag_buff[16];
	ATTACHMENT_CONTENT *pattachment;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id FROM"
	          " messages WHERE message_id=%llu", LLU(mid_val));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		*ppbrief = NULL;
		return TRUE;
	}
	pstmt.finalize();
	*ppbrief = cu_alloc<MESSAGE_CONTENT>();
	if (NULL == *ppbrief) {
		return FALSE;
	}
	proptags.count = 9;
	proptags.pproptag = proptag_buff;
	proptag_buff[0] = PR_SUBJECT;
	proptag_buff[1] = PROP_TAG_SENTREPRESENTINGNAME;
	proptag_buff[2] = PROP_TAG_SENTREPRESENTINGSMTPADDRESS;
	proptag_buff[3] = PROP_TAG_CLIENTSUBMITTIME;
	proptag_buff[4] = PR_MESSAGE_SIZE;
	proptag_buff[5] = PR_INTERNET_CPID;
	proptag_buff[6] = PROP_TAG_INTERNETMESSAGEID;
	proptag_buff[7] = PR_PARENT_KEY;
	proptag_buff[8] = PROP_TAG_CONVERSATIONINDEX;
	if (FALSE == common_util_get_properties(
		MESSAGE_PROPERTIES_TABLE, mid_val, cpid,
		pdb->psqlite, &proptags, &(*ppbrief)->proplist)) {
		return FALSE;
	}
	(*ppbrief)->children.prcpts = cu_alloc<TARRAY_SET>();
	if (NULL == (*ppbrief)->children.prcpts) {
		return FALSE;
	}
	if (FALSE == message_get_message_rcpts(pdb->psqlite,
		mid_val, (*ppbrief)->children.prcpts)) {
		return FALSE;
	}
	(*ppbrief)->children.pattachments = cu_alloc<ATTACHMENT_LIST>();
	if (NULL == (*ppbrief)->children.pattachments) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(*) FROM "
	          "attachments WHERE message_id=%llu", LLU(mid_val));
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	count = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	(*ppbrief)->children.pattachments->count = 0;
	(*ppbrief)->children.pattachments->pplist = cu_alloc<ATTACHMENT_CONTENT *>(count);
	if (NULL == (*ppbrief)->children.pattachments->pplist) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT attachment_id FROM "
	          "attachments WHERE message_id=%llu", LLU(mid_val));
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	proptags.count = 1;
	proptag_buff[0] = PR_ATTACH_LONG_FILENAME;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		attachment_id = sqlite3_column_int64(pstmt, 0);
		pattachment = cu_alloc<ATTACHMENT_CONTENT>();
		if (NULL == pattachment) {
			return FALSE;
		}
		if (FALSE == common_util_get_properties(
			ATTACHMENT_PROPERTIES_TABLE, attachment_id, cpid,
			pdb->psqlite, &proptags, &pattachment->proplist)) {
			return FALSE;
		}
		pattachment->pembedded = NULL;
		(*ppbrief)->children.pattachments->pplist[
			(*ppbrief)->children.pattachments->count] = pattachment;
		(*ppbrief)->children.pattachments->count ++;
	}
	return TRUE;
}

BOOL exmdb_server_check_message(const char *dir,
	uint64_t folder_id, uint64_t message_id, BOOL *pb_exist)
{
	uint64_t tmp_val;
	uint64_t fid_val;
	uint64_t mid_val;
	char sql_string[256];
	uint32_t folder_type;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	mid_val = rop_util_get_gc_value(message_id);
	if (FALSE == common_util_get_folder_type(
		pdb->psqlite, fid_val, &folder_type)) {
		return FALSE;
	}
	if (folder_type == FOLDER_SEARCH)
		snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id FROM"
					" search_result WHERE folder_id=%llu AND"
					" message_id=%llu", LLU(fid_val), LLU(mid_val));
	else
		snprintf(sql_string, arsizeof(sql_string), "SELECT parent_fid FROM"
					" messages WHERE message_id=%llu", LLU(mid_val));

	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		*pb_exist = FALSE;
		return TRUE;
	}
	tmp_val = sqlite3_column_int64(pstmt, 0);
	*pb_exist = tmp_val == fid_val ? TRUE : false;
	return TRUE;
}

BOOL exmdb_server_check_message_deleted(const char *dir,
	uint64_t message_id, BOOL *pb_del)
{
	uint64_t mid_val;
	char sql_string[256];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	if (TRUE == exmdb_server_check_private()) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
		          "FROM messages WHERE message_id=%llu", LLU(mid_val));
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT is_deleted "
		          "FROM messages WHERE message_id=%llu", LLU(mid_val));
	}
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	*pb_del = sqlite3_step(pstmt) != SQLITE_ROW ||
	          (!exmdb_server_check_private() &&
	          sqlite3_column_int64(pstmt, 0) != 0) ? TRUE : false;
	return TRUE;
}

BOOL exmdb_server_get_message_rcpts(const char *dir,
	uint64_t message_id, TARRAY_SET *pset)
{
	uint64_t mid_val;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	if (FALSE == message_get_message_rcpts(
		pdb->psqlite, mid_val, pset)) {
		return FALSE;
	}
	return TRUE;
}

BOOL exmdb_server_get_message_properties(const char *dir,
	const char *username, uint32_t cpid, uint64_t message_id,
	const PROPTAG_ARRAY *pproptags, TPROPVAL_ARRAY *ppropvals)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	if (FALSE == common_util_get_properties(MESSAGE_PROPERTIES_TABLE,
		rop_util_get_gc_value(message_id), cpid, pdb->psqlite,
		pproptags, ppropvals)) {
		return FALSE;
	}
	return TRUE;
}

/* message_size will not be updated in the function! */
BOOL exmdb_server_set_message_properties(const char *dir,
	const char *username, uint32_t cpid, uint64_t message_id,
	const TPROPVAL_ARRAY *pproperties, PROBLEM_ARRAY *pproblems)
{
	BOOL b_result;
	uint64_t nt_time;
	uint64_t fid_val;
	uint64_t mid_val;
	TAGGED_PROPVAL tmp_propval;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	mid_val = rop_util_get_gc_value(message_id);
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_set_properties(
		MESSAGE_PROPERTIES_TABLE, mid_val, cpid,
		pdb->psqlite, pproperties, pproblems)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (FALSE == common_util_get_message_parent_folder(
		pdb->psqlite, mid_val, &fid_val) || 0 == fid_val) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	nt_time = rop_util_current_nttime();
	tmp_propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		fid_val, 0, pdb->psqlite, &tmp_propval, &b_result);
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	db_engine_proc_dynamic_event(pdb,
		cpid, DYNAMIC_EVENT_MODIFY_MESSAGE,
		fid_val, mid_val, 0);
	db_engine_notify_message_modification(
		pdb, fid_val, mid_val);
	return TRUE;
}

BOOL exmdb_server_remove_message_properties(
	const char *dir, uint32_t cpid, uint64_t message_id,
	const PROPTAG_ARRAY *pproptags)
{
	BOOL b_result;
	uint64_t nt_time;
	uint64_t fid_val;
	uint64_t mid_val;
	TAGGED_PROPVAL tmp_propval;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_remove_properties(
		MESSAGE_PROPERTIES_TABLE, mid_val,
		pdb->psqlite, pproptags)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (FALSE == common_util_get_message_parent_folder(
		pdb->psqlite, mid_val, &fid_val) || 0 == fid_val) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	nt_time = rop_util_current_nttime();
	tmp_propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		fid_val, 0, pdb->psqlite, &tmp_propval, &b_result);
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	db_engine_proc_dynamic_event(pdb,
		cpid, DYNAMIC_EVENT_MODIFY_MESSAGE,
		fid_val, mid_val, 0);
	db_engine_notify_message_modification(
		pdb, fid_val, mid_val);
	return TRUE;
}

BOOL exmdb_server_set_message_read_state(const char *dir,
	const char *username, uint64_t message_id,
	uint8_t mark_as_read, uint64_t *pread_cn)
{
	BOOL b_result;
	uint64_t nt_time;
	uint64_t mid_val;
	uint64_t fid_val;
	uint64_t read_cn;
	char sql_string[128];
	TAGGED_PROPVAL tmp_propval;
	
	mid_val = rop_util_get_gc_value(message_id);
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_allocate_cn(pdb->psqlite, &read_cn)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return false;
	}
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
		common_util_set_message_read(pdb->psqlite,
			mid_val, mark_as_read);
		snprintf(sql_string, arsizeof(sql_string), "REPLACE INTO "
				"read_cns VALUES (%llu, ?, %llu)",
				LLU(mid_val), LLU(read_cn));
		auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
		if (pstmt == nullptr) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		sqlite3_bind_text(pstmt, 1, username, -1, SQLITE_STATIC);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			pstmt.finalize();
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	} else {
		common_util_set_message_read(pdb->psqlite,
			mid_val, mark_as_read);
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET "
			"read_cn=%llu WHERE message_id=%llu",
			LLU(read_cn), LLU(mid_val));
		if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	if (FALSE == common_util_get_message_parent_folder(
		pdb->psqlite, mid_val, &fid_val)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	nt_time = rop_util_current_nttime();
	tmp_propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		fid_val, 0, pdb->psqlite, &tmp_propval, &b_result);
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	db_engine_proc_dynamic_event(pdb,
		0, DYNAMIC_EVENT_MODIFY_MESSAGE,
		fid_val, mid_val, 0);
	db_engine_notify_message_modification(
		pdb, fid_val, mid_val);
	*pread_cn = rop_util_make_eid_ex(1, read_cn);
	return TRUE;
}

/* if folder_id is 0, it means embedded message */
BOOL exmdb_server_allocate_message_id(const char *dir,
	uint64_t folder_id, uint64_t *pmessage_id)
{
	uint64_t eid_val;
	uint64_t fid_val;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (0 == folder_id) {
		if (FALSE == common_util_allocate_eid(
			pdb->psqlite, &eid_val)) {
			return FALSE;
		}
		*pmessage_id = rop_util_make_eid_ex(1, eid_val);
		return TRUE;
	}
	fid_val = rop_util_get_gc_value(folder_id);
	if (FALSE == common_util_allocate_eid_from_folder(
		pdb->psqlite, fid_val, &eid_val)) {
		return FALSE;
	}
	*pmessage_id = rop_util_make_eid_ex(1, eid_val);
	return TRUE;
}

BOOL exmdb_server_get_message_group_id(const char *dir,
	uint64_t message_id, uint32_t **ppgroup_id)
{
	char sql_string[128];
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	snprintf(sql_string, arsizeof(sql_string), "SELECT group_id "
				"FROM messages WHERE message_id=%llu",
				LLU(rop_util_get_gc_value(message_id)));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
		SQLITE_NULL == sqlite3_column_type(pstmt, 0)) {
		*ppgroup_id = NULL;
		return TRUE;
	}
	*ppgroup_id = cu_alloc<uint32_t>();
	if (NULL == *ppgroup_id) {
		return FALSE;
	}
	**ppgroup_id = sqlite3_column_int64(pstmt, 0);
	return TRUE;
}

BOOL exmdb_server_set_message_group_id(const char *dir,
	uint64_t message_id, uint32_t group_id)
{
	char sql_string[128];
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET"
		" group_id=%u WHERE message_id=%llu",
		UI(group_id), LLU(rop_util_get_gc_value(message_id)));
	if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	return TRUE;
}

/* if count of indices and ungroup_proptags are both 0 means full change */
BOOL exmdb_server_save_change_indices(const char *dir,
	uint64_t message_id, uint64_t cn, const INDEX_ARRAY *pindices,
	const PROPTAG_ARRAY *pungroup_proptags)
{
	uint64_t mid_val;
	EXT_PUSH ext_push;
	char sql_string[128];
	uint8_t indices_buff[0x8000];
	uint8_t proptags_buff[0x8000];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	if (0 == pindices->count && 0 == pungroup_proptags->count) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET "
		          "group_id=? WHERE message_id=%llu", LLU(mid_val));
		auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		sqlite3_bind_null(pstmt, 1);
		return sqlite3_step(pstmt) == SQLITE_DONE ? TRUE : false;
	}
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO"
		" message_changes VALUES (?, ?, ?, ?)");
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	sqlite3_bind_int64(pstmt, 1, mid_val);
	sqlite3_bind_int64(pstmt, 2, rop_util_get_gc_value(cn));
	if (!ext_push.init(indices_buff, sizeof(indices_buff), 0) ||
	    ext_push.p_proptag_a(pindices) != EXT_ERR_SUCCESS)
		return false;
	sqlite3_bind_blob(pstmt, 3, ext_push.m_udata, ext_push.m_offset, SQLITE_STATIC);
	if (!ext_push.init(proptags_buff, sizeof(proptags_buff), 0) ||
	    ext_push.p_proptag_a(pungroup_proptags) != EXT_ERR_SUCCESS)
		return false;
	sqlite3_bind_blob(pstmt, 4, ext_push.m_udata, ext_push.m_offset, SQLITE_STATIC);
	return sqlite3_step(pstmt) == SQLITE_DONE ? TRUE : false;
}

/* if count of indices and ungroup_proptags are both 0 means full change */
BOOL exmdb_server_get_change_indices(const char *dir,
	uint64_t message_id, uint64_t cn, INDEX_ARRAY *pindices,
	PROPTAG_ARRAY *pungroup_proptags)
{
	int i;
	uint64_t cn_val;
	uint64_t mid_val;
	EXT_PULL ext_pull;
	char sql_string[128];
	INDEX_ARRAY tmp_indices;
	INDEX_ARRAY *ptmp_indices;
	PROPTAG_ARRAY tmp_proptags;
	PROPTAG_ARRAY *ptmp_proptags;
	
	cn_val = rop_util_get_gc_value(cn);
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	ptmp_indices = proptag_array_init();
	if (NULL == ptmp_indices) {
		return FALSE;
	}
	ptmp_proptags = proptag_array_init();
	if (NULL == ptmp_proptags) {
		proptag_array_free(ptmp_indices);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT change_number,"
				" indices, proptags FROM message_changes"
				" WHERE message_id=%llu", LLU(mid_val));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		proptag_array_free(ptmp_indices);
		proptag_array_free(ptmp_proptags);
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		if (gx_sql_col_uint64(pstmt, 0) <= cn_val)
			continue;
		if (sqlite3_column_bytes(pstmt, 1) > 0) {
			ext_pull.init(sqlite3_column_blob(pstmt, 1),
				sqlite3_column_bytes(pstmt, 1),
				common_util_alloc, 0);
			if (ext_pull.g_proptag_a(&tmp_indices) != EXT_ERR_SUCCESS) {
				proptag_array_free(ptmp_indices);
				proptag_array_free(ptmp_proptags);
				return FALSE;
			}
			for (i=0; i<tmp_indices.count; i++) {
				if (!proptag_array_append(ptmp_indices,
				    tmp_indices.pproptag[i])) {
					proptag_array_free(ptmp_indices);
					proptag_array_free(ptmp_proptags);
					return FALSE;
				}
			}
		}
		if (sqlite3_column_bytes(pstmt, 2) > 0) {
			ext_pull.init(sqlite3_column_blob(pstmt, 2),
				sqlite3_column_bytes(pstmt, 2),
				common_util_alloc, 0);
			if (ext_pull.g_proptag_a(&tmp_proptags) != EXT_ERR_SUCCESS) {
				proptag_array_free(ptmp_indices);
				proptag_array_free(ptmp_proptags);
				return FALSE;
			}
			for (i=0; i<tmp_proptags.count; i++) {
				if (!proptag_array_append(ptmp_proptags,
				    tmp_proptags.pproptag[i])) {
					proptag_array_free(ptmp_indices);
					proptag_array_free(ptmp_proptags);
					return FALSE;
				}
			}
		}
	}
	pstmt.finalize();
	pdb.reset();
	pindices->count = ptmp_indices->count;
	if (ptmp_indices->count > 0) {
		pindices->pproptag = cu_alloc<uint32_t>(ptmp_indices->count);
		if (NULL == pindices->pproptag) {
			proptag_array_free(ptmp_indices);
			proptag_array_free(ptmp_proptags);
			return FALSE;
		}
		memcpy(pindices->pproptag, ptmp_indices->pproptag,
			sizeof(uint32_t)*ptmp_indices->count);
	}
	proptag_array_free(ptmp_indices);
	if (ptmp_proptags->count > 0) {
		pungroup_proptags->count = ptmp_proptags->count;
		pungroup_proptags->pproptag = cu_alloc<uint32_t>(ptmp_proptags->count);
		if (NULL == pungroup_proptags->pproptag) {
			proptag_array_free(ptmp_proptags);
			return FALSE;
		}
		memcpy(pungroup_proptags->pproptag, ptmp_proptags->pproptag,
			sizeof(uint32_t)*ptmp_proptags->count);
	} else {
		pungroup_proptags->count = 0;
		pungroup_proptags->pproptag = NULL;
	}
	proptag_array_free(ptmp_proptags);
	return TRUE;
}

BOOL exmdb_server_mark_modified(const char *dir, uint64_t message_id)
{
	BOOL b_result;
	uint64_t mid_val;
	TAGGED_PROPVAL propval;
	uint32_t *pmessage_flags;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	if (FALSE == common_util_get_message_flags(
		pdb->psqlite, mid_val, TRUE, &pmessage_flags)) {
		return FALSE;
	}
	if (!(*pmessage_flags & MSGFLAG_UNMODIFIED))
		return TRUE;
	*pmessage_flags &= ~MSGFLAG_UNMODIFIED;
	propval.proptag = PR_MESSAGE_FLAGS;
	propval.pvalue = pmessage_flags;
	if (FALSE == common_util_set_property(MESSAGE_PROPERTIES_TABLE,
		mid_val, 0, pdb->psqlite, &propval, &b_result)) {
		return FALSE;
	}
	return TRUE;
}

/* add MSGFLAG_SUBMITTED and clear
	MSGFLAG_UNSENT in message_flags */
BOOL exmdb_server_try_mark_submit(const char *dir,
	uint64_t message_id, BOOL *pb_marked)
{
	uint64_t mid_val;
	TAGGED_PROPVAL propval;
	uint32_t *pmessage_flags;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	if (FALSE == common_util_get_message_flags(
		pdb->psqlite, mid_val, TRUE, &pmessage_flags)) {
		return FALSE;
	}
	if (*pmessage_flags & MSGFLAG_SUBMITTED) {
		*pb_marked = FALSE;
		return TRUE;
	}
	*pmessage_flags |= MSGFLAG_SUBMITTED;
	*pmessage_flags &= ~MSGFLAG_UNSENT;
	propval.proptag = PR_MESSAGE_FLAGS;
	propval.pvalue = pmessage_flags;
	if (FALSE == common_util_set_property(MESSAGE_PROPERTIES_TABLE,
		mid_val, 0, pdb->psqlite, &propval, pb_marked)) {
		return FALSE;
	}
	return TRUE;
}

/* clear MSGFLAG_SUBMITTED set by
	exmdb_server_try_submit, clear timer_id,
	set/clear MSGFLAG_UNSENT by b_unsent */
BOOL exmdb_server_clear_submit(const char *dir,
	uint64_t message_id, BOOL b_unsent)
{
	BOOL b_result;
	uint64_t mid_val;
	char sql_string[256];
	TAGGED_PROPVAL propval;
	uint32_t *pmessage_flags;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	if (FALSE == common_util_get_message_flags(
		pdb->psqlite, mid_val, TRUE, &pmessage_flags)) {
		return FALSE;
	}
	*pmessage_flags &= ~MSGFLAG_SUBMITTED;
	if (TRUE == b_unsent) {
		*pmessage_flags |= MSGFLAG_UNSENT;
	} else {
		*pmessage_flags &= ~MSGFLAG_UNSENT;
	}
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	propval.proptag = PR_MESSAGE_FLAGS;
	propval.pvalue = pmessage_flags;
	if (FALSE == common_util_set_property(MESSAGE_PROPERTIES_TABLE,
		mid_val, 0, pdb->psqlite, &propval, &b_result)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (FALSE == b_result) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return TRUE;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET"
	          " timer_id=? WHERE message_id=%llu", LLU(mid_val));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	sqlite3_bind_null(pstmt, 1);
	if (SQLITE_DONE != sqlite3_step(pstmt)) {
		pstmt.finalize();
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	pstmt.finalize();
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	return TRUE;
}

/* private only */
BOOL exmdb_server_link_message(const char *dir, uint32_t cpid,
	uint64_t folder_id, uint64_t message_id, BOOL *pb_result)
{
	uint64_t fid_val;
	uint64_t mid_val;
	char sql_string[256];
	uint32_t folder_type;
	
	if (FALSE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	mid_val = rop_util_get_gc_value(message_id);
	if (FALSE == common_util_get_folder_type(
		pdb->psqlite, fid_val, &folder_type)) {
		return FALSE;
	}
	if (folder_type != FOLDER_SEARCH) {
		*pb_result = FALSE;
		return TRUE;
	}	
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id FROM "
	          "messages WHERE message_id=%llu", LLU(mid_val));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		*pb_result = FALSE;
		return TRUE;
	}
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO search_result"
	        " VALUES (%llu, %llu)", LLU(fid_val), LLU(mid_val));
	if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	db_engine_proc_dynamic_event(pdb,
		cpid, DYNAMIC_EVENT_NEW_MESSAGE,
		fid_val, mid_val, 0);
	db_engine_notify_link_creation(pdb, fid_val, mid_val);
	*pb_result = TRUE;
	return TRUE;
}

/* private only */
BOOL exmdb_server_unlink_message(const char *dir,
	uint32_t cpid, uint64_t folder_id, uint64_t message_id)
{
	uint64_t fid_val;
	uint64_t mid_val;
	char sql_string[256];
	
	if (FALSE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	mid_val = rop_util_get_gc_value(message_id);
	snprintf(sql_string, arsizeof(sql_string), "DELETE FROM search_result"
		" WHERE folder_id=%llu AND message_id=%llu",
		LLU(fid_val), LLU(mid_val));
	if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	db_engine_proc_dynamic_event(pdb,
		cpid, DYNAMIC_EVENT_DELETE_MESSAGE,
		fid_val, mid_val, 0);
	db_engine_notify_link_deletion(pdb, fid_val, mid_val);
	return TRUE;
}

/* private only */
BOOL exmdb_server_set_message_timer(const char *dir,
	uint64_t message_id, uint32_t timer_id)
{
	char sql_string[256];
	
	if (FALSE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET"
		" timer_id=%u WHERE message_id=%llu",
		UI(timer_id), LLU(rop_util_get_gc_value(message_id)));
	if (SQLITE_OK != sqlite3_exec(pdb->psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	return TRUE;
}

/* private only */
BOOL exmdb_server_get_message_timer(const char *dir,
	uint64_t message_id, uint32_t **pptimer_id)
{
	uint64_t mid_val;
	char sql_string[256];
	
	if (FALSE == exmdb_server_check_private()) {
		return FALSE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	mid_val = rop_util_get_gc_value(message_id);
	snprintf(sql_string, arsizeof(sql_string), "SELECT timer_id FROM "
	          "messages WHERE message_id=%llu", LLU(mid_val));
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (sqlite3_step(pstmt) != SQLITE_ROW ||
	    sqlite3_column_type(pstmt, 0) == SQLITE_NULL) {
		*pptimer_id = NULL;
		return TRUE;
	}
	*pptimer_id = cu_alloc<uint32_t>();
	if (NULL == *pptimer_id) {
		return FALSE;
	}
	**pptimer_id = sqlite3_column_int64(pstmt, 0);
	return TRUE;
}

static BOOL message_read_message(sqlite3 *psqlite, uint32_t cpid,
	uint64_t message_id, MESSAGE_CONTENT **ppmsgctnt)
{
	int i;
	uint32_t count;
	uint32_t attach_num;
	char sql_string[256];
	PROPTAG_ARRAY proptags;
	uint64_t attachment_id;
	TAGGED_PROPVAL *ppropval;
	PROPTAG_ARRAY tmp_proptags;
	uint32_t proptag_buff[0x8000];
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
	*ppmsgctnt = cu_alloc<MESSAGE_CONTENT>();
	if (NULL == *ppmsgctnt) {
		return FALSE;
	}
	if (FALSE == common_util_get_proptags(
		MESSAGE_PROPERTIES_TABLE, message_id,
		psqlite, &tmp_proptags)) {
		return FALSE;	
	}
	proptags.count = 0;
	proptags.pproptag = proptag_buff;
	for (i=0; i<tmp_proptags.count; i++) {
		switch (tmp_proptags.pproptag[i]) {
		case PR_DISPLAY_TO:
		case PR_DISPLAY_TO_A:
		case PR_DISPLAY_CC:
		case PR_DISPLAY_CC_A:
		case PR_DISPLAY_BCC:
		case PR_DISPLAY_BCC_A:
		case PROP_TAG_HASATTACHMENTS:
			continue;
		}
		proptag_buff[proptags.count] = tmp_proptags.pproptag[i];
		proptags.count ++;
	}
	if (FALSE == common_util_get_properties(
		MESSAGE_PROPERTIES_TABLE, message_id, cpid,
		psqlite, &proptags, &(*ppmsgctnt)->proplist)) {
		return FALSE;
	}
	(*ppmsgctnt)->children.prcpts = cu_alloc<TARRAY_SET>();
	if (NULL == (*ppmsgctnt)->children.prcpts) {
		return FALSE;
	}
	if (FALSE == message_get_message_rcpts(psqlite,
		message_id, (*ppmsgctnt)->children.prcpts)) {
		return FALSE;
	}
	(*ppmsgctnt)->children.pattachments = cu_alloc<ATTACHMENT_LIST>();
	if (NULL == (*ppmsgctnt)->children.pattachments) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(*) FROM "
	          "attachments WHERE message_id=%llu", LLU(message_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	count = sqlite3_column_int64(pstmt, 0);
	pstmt.finalize();
	(*ppmsgctnt)->children.pattachments->count = 0;
	(*ppmsgctnt)->children.pattachments->pplist = cu_alloc<ATTACHMENT_CONTENT *>(count);
	if (NULL == (*ppmsgctnt)->children.pattachments->pplist) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT attachment_id FROM "
	          "attachments WHERE message_id=%llu", LLU(message_id));
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	snprintf(sql_string, arsizeof(sql_string), "SELECT message_id"
			" FROM messages WHERE parent_attid=?");
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	attach_num = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		attachment_id = sqlite3_column_int64(pstmt, 0);
		if (FALSE == common_util_get_proptags(
			ATTACHMENT_PROPERTIES_TABLE, attachment_id,
			psqlite, &tmp_proptags)) {
			return FALSE;
		}
		pattachment = cu_alloc<ATTACHMENT_CONTENT>();
		if (NULL == pattachment) {
			return FALSE;
		}
		proptags.count = tmp_proptags.count;
		proptags.pproptag = proptag_buff;
		memcpy(proptag_buff, tmp_proptags.pproptag,
			sizeof(uint32_t)*tmp_proptags.count);
		proptag_buff[proptags.count] = PROP_TAG_ATTACHNUMBER;
		proptags.count ++;
		if (FALSE == common_util_get_properties(
			ATTACHMENT_PROPERTIES_TABLE, attachment_id, cpid,
			psqlite, &proptags, &pattachment->proplist)) {
			return FALSE;
		}
		/* PROP_TAG_ATTACHNUMBER MUST be the first */
		memmove(pattachment->proplist.ppropval + 1,
			pattachment->proplist.ppropval, sizeof(
			TAGGED_PROPVAL)*pattachment->proplist.count);
		ppropval = pattachment->proplist.ppropval;
		pattachment->proplist.count ++;
		ppropval->proptag = PROP_TAG_ATTACHNUMBER;
		ppropval->pvalue = cu_alloc<uint32_t>();
		if (NULL == ppropval->pvalue) {
			return FALSE;
		}
		*(uint32_t*)ppropval->pvalue = attach_num;
		attach_num ++;
		sqlite3_bind_int64(pstmt1, 1, attachment_id);
		if (SQLITE_ROW == sqlite3_step(pstmt1)) {
			if (FALSE == message_read_message(psqlite, cpid,
				sqlite3_column_int64(pstmt1, 0),
				&pattachment->pembedded) ||
				NULL == pattachment->pembedded) {
				return FALSE;
			}
		} else {
			pattachment->pembedded = NULL;
		}
		sqlite3_reset(pstmt1);
		(*ppmsgctnt)->children.pattachments->pplist[
			(*ppmsgctnt)->children.pattachments->count] = pattachment;
		(*ppmsgctnt)->children.pattachments->count ++;
	}
	return TRUE;
}

static bool message_md5_string(const char *string, uint8_t *pdgt) __attribute__((warn_unused_result));
static bool message_md5_string(const char *string, uint8_t *pdgt)
{
	char tmp_string[256];
	uint8_t dgt_buff[MD5_DIGEST_LENGTH];
	
	gx_strlcpy(tmp_string, string, GX_ARRAY_SIZE(tmp_string));
	HX_strupper(tmp_string);
	std::unique_ptr<EVP_MD_CTX, sslfree> ctx(EVP_MD_CTX_new());
	if (ctx != nullptr ||
	    EVP_DigestInit(ctx.get(), EVP_md5()) <= 0 ||
	    EVP_DigestUpdate(ctx.get(), tmp_string, strlen(tmp_string)) <= 0 ||
	    EVP_DigestFinal(ctx.get(), dgt_buff, nullptr) <= 0)
		return false;
	memcpy(pdgt, dgt_buff, 16);
	return true;
}

static BOOL message_rectify_message(const char *account,
	const MESSAGE_CONTENT *pmsgctnt, MESSAGE_CONTENT *pmsgctnt1)
{
	int i;
	void *pvalue;
	BINARY *pbin;
	BINARY *pbin1;
	GUID tmp_guid;
	uint64_t nt_time;
	EXT_PUSH ext_push;
	char cid_string[256];
	MESSAGE_CONTENT *pembedded;
	static constexpr uint8_t fake_true = true;
	static constexpr uint32_t fake_int32 = 0;
	static uint32_t fake_flags = MSGFLAG_UNMODIFIED; /* modified by common_util_set_properties */
	
	pmsgctnt1->proplist.count = 0;
	auto *vc = pmsgctnt1->proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt->proplist.count + 20);
	if (vc == nullptr)
		return FALSE;
	for (i=0; i<pmsgctnt->proplist.count; i++) {
		switch (pmsgctnt->proplist.ppropval[i].proptag) {
		case PROP_TAG_MID:
		case PR_ASSOCIATED:
		case PROP_TAG_CHANGENUMBER:
		case PROP_TAG_MESSAGESTATUS:
			continue;
		case PR_SUBJECT:
		case PR_SUBJECT_A:
			if (common_util_get_propvals(&pmsgctnt->proplist, PR_NORMALIZED_SUBJECT) != nullptr ||
			    common_util_get_propvals(&pmsgctnt->proplist, PR_NORMALIZED_SUBJECT_A) != nullptr)
				continue;	
			break;
		}
		*vc++ = pmsgctnt->proplist.ppropval[i];
		pmsgctnt1->proplist.count ++;
	}
	vc->proptag = PROP_TAG_MESSAGESTATUS;
	vc->pvalue = deconst(&fake_int32);
	pmsgctnt1->proplist.count ++;
	++vc;
	auto msgfl = common_util_get_propvals(&pmsgctnt->proplist, PR_MESSAGE_FLAGS);
	if (msgfl == nullptr) {
		vc->proptag = PR_MESSAGE_FLAGS;
		vc->pvalue = deconst(&fake_flags);
		pmsgctnt1->proplist.count ++;
		++vc;
		if (common_util_get_propvals(&pmsgctnt->proplist, PR_READ) == nullptr) {
			auto x = cu_alloc<uint8_t>();
			if (x == nullptr)
				return false;
			*x = false;
			vc->proptag = PR_READ;
			vc->pvalue = x;
			++pmsgctnt1->proplist.count;
			++vc;
		}
	} else if (common_util_get_propvals(&pmsgctnt->proplist, PR_READ) == nullptr) {
		auto x = cu_alloc<uint8_t>();
		if (x == nullptr)
			return false;
		*x = *static_cast<uint32_t *>(msgfl) & MSGFLAG_READ;
		vc->proptag = PR_READ;
		vc->pvalue = x;
		++pmsgctnt1->proplist.count;
		++vc;
	}
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_SEARCHKEY)) {
		pbin = cu_alloc<BINARY>();
		if (NULL == pbin) {
			return FALSE;
		}
		pbin->cb = 16;
		pbin->pv = common_util_alloc(16);
		if (pbin->pv == nullptr)
			return FALSE;
		tmp_guid = guid_random_new();
		if (!ext_push.init(pbin->pb, 16, 0) ||
		    ext_push.p_guid(&tmp_guid) != EXT_ERR_SUCCESS)
			return false;
		vc->proptag = PROP_TAG_SEARCHKEY;
		vc->pvalue = pbin;
		pmsgctnt1->proplist.count ++;
		++vc;
	}
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_BODYCONTENTID)) {
		tmp_guid = guid_random_new();
		if (!ext_push.init(cid_string, 256, 0) ||
		    ext_push.p_guid(&tmp_guid) != EXT_ERR_SUCCESS)
			return false;
		encode_hex_binary(cid_string, 16, cid_string + 16, 64);
		memmove(cid_string, cid_string + 16, 32);
		cid_string[32] = '@';
		auto pc = strchr(static_cast<const char *>(account), '@');
		if (pc == nullptr)
			pc = account;
		else
			++pc;
		strncpy(cid_string + 33, pc, 128);
		pvalue = common_util_dup(cid_string);
		if (NULL == pvalue) {
			return FALSE;
		}
		vc->proptag = PROP_TAG_BODYCONTENTID;
		vc->pvalue = pvalue;
		pmsgctnt1->proplist.count ++;
		++vc;
	}
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_CREATORNAME)) {
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_SENDERNAME);
		if (NULL == pvalue) {
			pvalue = common_util_get_propvals(
				&pmsgctnt->proplist, PROP_TAG_SENTREPRESENTINGNAME);
		}
		if (NULL != pvalue) {
			vc->proptag = PROP_TAG_CREATORNAME;
			vc->pvalue = pvalue;
			pmsgctnt1->proplist.count ++;
			++vc;
		}
	}
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_CREATORENTRYID)) {
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_SENDERENTRYID);
		if (NULL == pvalue) {
			pvalue = common_util_get_propvals(
				&pmsgctnt->proplist, PROP_TAG_SENTREPRESENTINGENTRYID);
		}
		if (NULL != pvalue) {
			vc->proptag = PROP_TAG_CREATORENTRYID;
			vc->pvalue = pvalue;
			pmsgctnt1->proplist.count ++;
			++vc;
		}
	}
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_LASTMODIFIERNAME)) {
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_SENDERNAME);
		if (NULL == pvalue) {
			pvalue = common_util_get_propvals(
				&pmsgctnt->proplist, PROP_TAG_SENTREPRESENTINGNAME);
		}
		if (NULL != pvalue) {
			vc->proptag = PROP_TAG_LASTMODIFIERNAME;
			vc->pvalue = pvalue;
			pmsgctnt1->proplist.count ++;
			++vc;
		}
	}
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_LASTMODIFIERENTRYID)) {
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_SENDERENTRYID);
		if (NULL == pvalue) {
			pvalue = common_util_get_propvals(
				&pmsgctnt->proplist, PROP_TAG_SENTREPRESENTINGENTRYID);
		}
		if (NULL != pvalue) {
			vc->proptag = PROP_TAG_LASTMODIFIERENTRYID;
			vc->pvalue = pvalue;
			pmsgctnt1->proplist.count ++;
			++vc;
		}
	}
	pbin1 = static_cast<BINARY *>(common_util_get_propvals(
	        &pmsgctnt->proplist, PROP_TAG_CONVERSATIONINDEX));
	pbin = cu_alloc<BINARY>();
	if (NULL == pbin) {
		return FALSE;
	}
	pbin->cb = 16;
	if (NULL != pbin1 && pbin1->cb >= 22) {
		pbin->pb = pbin1->pb + 6;
	} else {
		pbin->pv = common_util_alloc(16);
		if (pbin->pv == nullptr)
			return FALSE;
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_CONVERSATIONTOPIC);
		if (NULL != pvalue && '\0' != ((uint8_t*)pvalue)[0]) {
			if (!message_md5_string(static_cast<char *>(pvalue), pbin->pb))
				return false;
		} else {
			tmp_guid = guid_random_new();
			if (!ext_push.init(pbin->pb, 16, 0) ||
			    ext_push.p_guid(&tmp_guid) != EXT_ERR_SUCCESS)
				return false;
		}
	}
	vc->proptag = PROP_TAG_CONVERSATIONID;
	vc->pvalue = pbin;
	pmsgctnt1->proplist.count ++;
	++vc;
	vc->proptag = PROP_TAG_CONVERSATIONINDEXTRACKING;
	vc->pvalue = deconst(&fake_true);
	pmsgctnt1->proplist.count ++;
	++vc;
	if (NULL == pbin1) {
		pbin1 = cu_alloc<BINARY>();
		if (NULL == pbin1) {
			return FALSE;
		}
		pbin1->pv = common_util_alloc(27);
		if (pbin1->pv == nullptr)
			return FALSE;
		nt_time = rop_util_current_nttime();
		if (!ext_push.init(pbin1->pb, 27, 0) ||
		    ext_push.p_uint8(1) != EXT_ERR_SUCCESS ||
		    ext_push.p_uint32(nt_time >> 32) != EXT_ERR_SUCCESS ||
		    ext_push.p_uint8((nt_time & 0xFFFFFFFF) >> 24) != EXT_ERR_SUCCESS ||
		    ext_push.p_bytes(pbin->pb, 16) != EXT_ERR_SUCCESS ||
		    ext_push.p_uint32(0xFFFFFFFF) != EXT_ERR_SUCCESS ||
		    ext_push.p_uint8(nt_time & 0xFF))
			return false;
		pbin1->cb = 27;
		vc->proptag = PROP_TAG_CONVERSATIONINDEX;
		vc->pvalue = pbin1;
		pmsgctnt1->proplist.count ++;
		++vc;
	}
	pvalue = common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_CONVERSATIONTOPIC);
	if (NULL == pvalue) {
		pvalue = common_util_get_propvals(&pmsgctnt->proplist,
						PROP_TAG_CONVERSATIONTOPIC_STRING8);
	}
	if (NULL == pvalue) {
		pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_NORMALIZED_SUBJECT);
		if (NULL == pvalue) {
			pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_NORMALIZED_SUBJECT_A);
			if (NULL != pvalue) {
				vc->proptag = PROP_TAG_CONVERSATIONTOPIC_STRING8;
				vc->pvalue = pvalue;
				pmsgctnt1->proplist.count ++;
				++vc;
			}
		} else {
			vc->proptag = PROP_TAG_CONVERSATIONTOPIC;
			vc->pvalue = pvalue;
			pmsgctnt1->proplist.count ++;
			++vc;
		}
	}
	pmsgctnt1->children.prcpts = pmsgctnt->children.prcpts;
	if (NULL == pmsgctnt->children.pattachments ||
		0 == pmsgctnt->children.pattachments->count) {
		pmsgctnt1->children.pattachments = NULL;
		return TRUE;
	}
	pmsgctnt1->children.pattachments = cu_alloc<ATTACHMENT_LIST>();
	if (NULL == pmsgctnt1->children.pattachments) {
		return FALSE;
	}
	pmsgctnt1->children.pattachments->count =
		pmsgctnt->children.pattachments->count;
	pmsgctnt1->children.pattachments->pplist = cu_alloc<ATTACHMENT_CONTENT *>(pmsgctnt->children.pattachments->count);
	if (NULL == pmsgctnt1->children.pattachments->pplist) {
		return FALSE;
	}
	for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
		if (NULL == pmsgctnt->children.pattachments->pplist[i]->pembedded) {
			pmsgctnt1->children.pattachments->pplist[i] =
				pmsgctnt->children.pattachments->pplist[i];
		} else {
			pmsgctnt1->children.pattachments->pplist[i] = cu_alloc<ATTACHMENT_CONTENT>();
			if (NULL == pmsgctnt1->children.pattachments->pplist[i]) {
				return FALSE;
			}
			pmsgctnt1->children.pattachments->pplist[i]->proplist =
				pmsgctnt->children.pattachments->pplist[i]->proplist;
			pembedded = cu_alloc<MESSAGE_CONTENT>();
			if (NULL == pembedded) {
				return FALSE;
			}
			if (FALSE == message_rectify_message(account,
				pmsgctnt->children.pattachments->pplist[i]->pembedded,
				pembedded)) {
				return FALSE;	
			}
			pmsgctnt1->children.pattachments->pplist[i]->pembedded =
															pembedded;
		}
	}
	return TRUE;
}
	
static BOOL message_write_message(BOOL b_internal, sqlite3 *psqlite,
	const char *account, uint32_t cpid, BOOL b_embedded,
	uint64_t parent_id, const MESSAGE_CONTENT *pmsgctnt,
	uint64_t *pmessage_id)
{
	BOOL b_cn;
	XID tmp_xid;
	int tmp_int, tmp_int1, is_associated = 0;
	void *pvalue;
	BOOL b_exist;
	BOOL b_result;
	uint32_t next;
	uint64_t tmp_id;
	uint64_t nt_time;
	uint8_t tmp_byte;
	uint64_t change_num;
	uint64_t message_id;
	char sql_string[256];
	uint32_t message_size;
	uint32_t original_size;
	MESSAGE_CONTENT msgctnt;
	TAGGED_PROPVAL tmp_propval;
	PROBLEM_ARRAY tmp_problems;
	static const uint32_t fake_uid = 1;
	const TPROPVAL_ARRAY *pproplist;
	
	pproplist = &pmsgctnt->proplist;
	pvalue = common_util_get_propvals(
		pproplist, PROP_TAG_CHANGENUMBER);
	if (NULL == pvalue) {
		if (FALSE == common_util_allocate_cn(psqlite, &change_num)) {
			return FALSE;
		}
		b_cn = FALSE;
	} else {
		change_num = rop_util_get_gc_value(*(uint64_t*)pvalue);
		b_cn = TRUE;
	}
	if (FALSE == b_internal) {
		if (FALSE == message_rectify_message(
			account, pmsgctnt, &msgctnt)) {
			return FALSE;
		}
		if (FALSE == b_embedded && FALSE == b_cn) {
			if (TRUE == exmdb_server_check_private()) {
				if (FALSE == common_util_get_id_from_username(
					account, &tmp_int)) {
					return FALSE;
				}
				tmp_xid.guid = rop_util_make_user_guid(tmp_int);
			} else {
				if (FALSE == common_util_get_domain_ids(
					account, &tmp_int, &tmp_int1)) {
					return FALSE;
				}
				tmp_xid.guid = rop_util_make_domain_guid(tmp_int);
			}
			rop_util_value_to_gc(change_num, tmp_xid.local_id);
			pvalue = common_util_xid_to_binary(22, &tmp_xid);
			if (NULL == pvalue) {
				return FALSE;
			}
			msgctnt.proplist.ppropval[msgctnt.proplist.count].proptag = PR_CHANGE_KEY;
			msgctnt.proplist.ppropval[msgctnt.proplist.count].pvalue =
																pvalue;
			msgctnt.proplist.count ++;
			pvalue = common_util_pcl_append(nullptr, static_cast<BINARY *>(pvalue));
			if (NULL == pvalue) {
				return FALSE;
			}
			msgctnt.proplist.ppropval[msgctnt.proplist.count].proptag = PR_PREDECESSOR_CHANGE_LIST;
			msgctnt.proplist.ppropval[msgctnt.proplist.count].pvalue =
																pvalue;
			msgctnt.proplist.count ++;
		}
		pmsgctnt = &msgctnt;
	}
	original_size = 0;
	message_size = common_util_calculate_message_size(pmsgctnt);
	if (FALSE == b_embedded) {
		pvalue = common_util_get_propvals(pproplist, PR_ASSOCIATED);
		is_associated = pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0 ? 0 : 1;
		if (TRUE == exmdb_server_check_private()) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT is_search FROM "
			          "folders WHERE folder_id=%llu", LLU(parent_id));
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT is_deleted FROM"
			          " folders WHERE folder_id=%llu", LLU(parent_id));
		}
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			*pmessage_id = 0;
			return TRUE;
		}
		tmp_byte = sqlite3_column_int64(pstmt, 0);
		pstmt.finalize();
		if (0 != tmp_byte) {
			*pmessage_id = 0;
			return TRUE;
		}
		b_exist = FALSE;
		pvalue = common_util_get_propvals(pproplist, PROP_TAG_MID);
		if (NULL == pvalue) {
			if (FALSE == common_util_allocate_eid_from_folder(
				psqlite, parent_id, pmessage_id)) {
				return FALSE;
			}
		} else {
			*pmessage_id = rop_util_get_gc_value(*(uint64_t*)pvalue);
			snprintf(sql_string, arsizeof(sql_string), "SELECT parent_fid, message_size"
			          " FROM messages WHERE message_id=%llu", LLU(*pmessage_id));
			pstmt = gx_sql_prep(psqlite, sql_string);
			if (pstmt == nullptr)
				return FALSE;
			if (SQLITE_ROW != sqlite3_step(pstmt)) {
				if (FALSE == common_util_check_allocated_eid(
					psqlite, *pmessage_id, &b_result)) {
					return FALSE;
				}
				if (FALSE == b_result) {
					*pmessage_id = 0;
					return TRUE;
				}
			} else {
				if (gx_sql_col_uint64(pstmt, 0) != parent_id) {
					*pmessage_id = 0;
					return TRUE;
				}
				b_exist = TRUE;
				original_size = sqlite3_column_int64(pstmt, 1);
			}
			pstmt.finalize();
		}
		if (TRUE == b_exist) {
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM message_properties"
			        " WHERE message_id=%llu", LLU(*pmessage_id));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM recipients"
			        " WHERE message_id=%llu", LLU(*pmessage_id));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM attachments"
			        " WHERE message_id=%llu", LLU(*pmessage_id));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM message_changes"
			        "  WHERE message_id=%llu", LLU(*pmessage_id));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "UPDATE messages SET change_number=%llu,"
				" message_size=%u, group_id=NULL WHERE message_id=%llu",
				LLU(change_num), UI(message_size), LLU(*pmessage_id));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
		} else {
			snprintf(sql_string, arsizeof(sql_string), "INSERT INTO messages (message_id,"
				" parent_fid, parent_attid, is_associated, "
				"change_number, message_size) VALUES (%llu, %llu, "
				"NULL, %d, %llu, %u)", LLU(*pmessage_id), LLU(parent_id),
				is_associated, LLU(change_num), UI(message_size));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
		}
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT count(*) FROM "
		          "attachments WHERE attachment_id=%llu", LLU(parent_id));
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
			return FALSE;
		if (1 != sqlite3_column_int64(pstmt, 0)) {
			*pmessage_id = 0;
			return TRUE;
		}
		pstmt.finalize();
		b_exist = FALSE;
		snprintf(sql_string, arsizeof(sql_string), "SELECT message_id, message_size"
		          " FROM messages WHERE parent_attid=%llu", LLU(parent_id));
		pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		if (SQLITE_ROW == sqlite3_step(pstmt)) {
			*pmessage_id = sqlite3_column_int64(pstmt, 0);
			original_size = sqlite3_column_int64(pstmt, 1);
			b_exist = TRUE;
		}
		pstmt.finalize();
		if (FALSE == b_exist) {
			if (FALSE == common_util_allocate_eid(
				psqlite, pmessage_id)) {
				return FALSE;
			}
		} else {
			snprintf(sql_string, arsizeof(sql_string), "DELETE FROM messages"
			        " WHERE message_id=%llu", LLU(*pmessage_id));
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return FALSE;
			}
		}
		snprintf(sql_string, arsizeof(sql_string), "INSERT INTO messages (message_id,"
			" parent_fid, parent_attid, change_number, "
			"message_size) VALUES (%llu, NULL, %llu, %llu, %u)",
			LLU(*pmessage_id), LLU(parent_id), LLU(change_num), UI(message_size));
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			return FALSE;
		}
	}
	if (FALSE == common_util_set_properties(
		MESSAGE_PROPERTIES_TABLE, *pmessage_id, cpid,
		psqlite, &pmsgctnt->proplist, &tmp_problems)) {
		return FALSE;
	}
	if (FALSE == b_embedded) {
		if (FALSE == common_util_get_property(FOLDER_PROPERTIES_TABLE,
			parent_id, 0, psqlite, PROP_TAG_ARTICLENUMBERNEXT, &pvalue)) {
			return FALSE;
		}
		if (NULL == pvalue) {
			pvalue = deconst(&fake_uid);
		}
		next = *(uint32_t*)pvalue + 1;
		tmp_propval.proptag = PROP_TAG_ARTICLENUMBERNEXT;
		tmp_propval.pvalue = &next;
		if (FALSE == common_util_set_property(FOLDER_PROPERTIES_TABLE,
			parent_id, 0, psqlite, &tmp_propval, &b_result)) {
			return FALSE;	
		}
		tmp_propval.proptag = PROP_TAG_INTERNETARTICLENUMBER;
		tmp_propval.pvalue = pvalue;
		if (FALSE == common_util_set_property(MESSAGE_PROPERTIES_TABLE,
			*pmessage_id, 0, psqlite, &tmp_propval, &b_result)) {
			return FALSE;	
		}
	}
	if (NULL != pmsgctnt->children.prcpts) {
		snprintf(sql_string, arsizeof(sql_string), "INSERT INTO recipients "
		          "(message_id) VALUES (%llu)", LLU(*pmessage_id));
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		for (size_t i = 0; i < pmsgctnt->children.prcpts->count; ++i) {
			if (SQLITE_DONE != sqlite3_step(pstmt)) {
				return FALSE;
			}
			tmp_id = sqlite3_last_insert_rowid(psqlite);
			if (FALSE == common_util_set_properties(
				RECIPIENT_PROPERTIES_TABLE, tmp_id, cpid, psqlite,
				pmsgctnt->children.prcpts->pparray[i], &tmp_problems)) {
				return FALSE;
			}
		}
	}
	if (NULL != pmsgctnt->children.pattachments) {
		snprintf(sql_string, arsizeof(sql_string), "INSERT INTO attachments"
		          " (message_id) VALUES (%llu)", LLU(*pmessage_id));
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		for (size_t i = 0; i < pmsgctnt->children.pattachments->count; ++i) {
			if (SQLITE_DONE != sqlite3_step(pstmt)) {
				return FALSE;
			}
			tmp_id = sqlite3_last_insert_rowid(psqlite);
			if (FALSE == common_util_set_properties(
				ATTACHMENT_PROPERTIES_TABLE, tmp_id, cpid, psqlite,
				&pmsgctnt->children.pattachments->pplist[i]->proplist,
				&tmp_problems)) {
				return FALSE;
			}
			if (NULL != pmsgctnt->children.pattachments->pplist[i]->pembedded) {
				if (FALSE == message_write_message(TRUE,
					psqlite, account, cpid, TRUE, tmp_id,
					pmsgctnt->children.pattachments->pplist[i]->pembedded,
					&message_id)) {
					return FALSE;
				}
				if (0 == message_id) {
					*pmessage_id = 0;
					return TRUE;
				}
			}
		}
	}
	if (TRUE == b_internal) {
		return TRUE;
	}
	if (TRUE == b_embedded) {
		if (original_size > message_size) {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE messages set "
				"message_size=message_size-%u WHERE message_id=?",
				original_size - message_size);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE messages set "
				"message_size=message_size+%u WHERE message_id=?",
				message_size - original_size);
		}
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		snprintf(sql_string, arsizeof(sql_string), "SELECT message_id FROM"
						" attachments WHERE attachment_id=?");
		auto pstmt1 = gx_sql_prep(psqlite, sql_string);
		if (pstmt1 == nullptr) {
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT parent_attid, "
			"is_associated FROM messages WHERE message_id=?");
		auto pstmt2 = gx_sql_prep(psqlite, sql_string);
		if (pstmt2 == nullptr) {
			return FALSE;
		}
		while (true) {
			sqlite3_bind_int64(pstmt1, 1, parent_id);
			if (SQLITE_ROW != sqlite3_step(pstmt1)) {
				*pmessage_id = 0;
				return FALSE;
			}
			message_id = sqlite3_column_int64(pstmt1, 0);
			sqlite3_bind_int64(pstmt, 1, message_id);
			if (SQLITE_DONE != sqlite3_step(pstmt)) {
				return FALSE;
			}
			sqlite3_reset(pstmt);
			sqlite3_bind_int64(pstmt2, 1, message_id);
			if (SQLITE_ROW != sqlite3_step(pstmt2)) {
				*pmessage_id = 0;
				return FALSE;
			}
			if (SQLITE_NULL == sqlite3_column_type(pstmt2, 0)) {
				is_associated = sqlite3_column_int64(pstmt2, 1);
				break;
			}
			parent_id = sqlite3_column_int64(pstmt2, 0);
		}
	}
	if (original_size > message_size) {
		if (0 == is_associated) {
			if (FALSE == common_util_decrease_store_size(
				psqlite, original_size - message_size, 0)) {
				return FALSE;	
			}
		} else {
			if (FALSE == common_util_decrease_store_size(
				psqlite, 0, original_size - message_size)) {
				return FALSE;	
			}
		}
	} else {
		if (0 == is_associated) {
			if (FALSE == common_util_increase_store_size(
				psqlite, message_size - original_size, 0)) {
				return FALSE;	
			}
		} else {
			if (FALSE == common_util_increase_store_size(
				psqlite, 0, message_size - original_size)) {
				return FALSE;	
			}
		}
	}
	if (TRUE == b_embedded) {
		return TRUE;
	}
	nt_time = rop_util_current_nttime();
	tmp_propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	tmp_propval.pvalue = &nt_time;
	return common_util_set_property(FOLDER_PROPERTIES_TABLE,
			parent_id, 0, psqlite, &tmp_propval, &b_result);
}

static BOOL message_load_folder_rules(BOOL b_oof,
	sqlite3 *psqlite, uint64_t folder_id, DOUBLE_LIST *plist)
{
	void *pvalue;
	uint32_t state;
	RULE_NODE *prnode;
	char sql_string[256];
	DOUBLE_LIST_NODE *pnode;
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT state, rule_id,"
					" sequence, provider FROM rules WHERE"
					" folder_id=%lld", LLU(folder_id));
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		state = sqlite3_column_int64(pstmt, 0);
		if ((state & RULE_STATE_PARSE_ERROR)
			|| (state & RULE_STATE_ERROR)) {
			continue;
		}
		if (state & RULE_STATE_ENABLED) {
			/* do nothing */
		} else if (state & RULE_STATE_ONLY_WHEN_OOF) {
			if (FALSE == b_oof) {
				continue;
			}
		} else {
			continue;
		}
		prnode = cu_alloc<RULE_NODE>();
		if (NULL == prnode) {
			return FALSE;
		}
		prnode->node.pdata = prnode;
		prnode->state = state;
		prnode->id = sqlite3_column_int64(pstmt, 1);
		prnode->sequence = sqlite3_column_int64(pstmt, 2);
		pvalue = (void*)sqlite3_column_text(pstmt, 3);
		if (NULL == pvalue) {
			continue;
		}
		prnode->provider = common_util_dup(static_cast<char *>(pvalue));
		if (NULL == prnode->provider) {
			return FALSE;
		}
		for (pnode=double_list_get_head(plist); NULL!=pnode;
			pnode=double_list_get_after(plist, pnode)) {
			if (((RULE_NODE*)pnode->pdata)->sequence >= prnode->sequence) {
				double_list_insert_before(plist, pnode, &prnode->node);
				break;
			}
		}
		if (NULL == pnode) {
			double_list_append_as_tail(plist, &prnode->node);
		}
	}
	return TRUE;
}

static BOOL message_load_folder_ext_rules(BOOL b_oof,
	sqlite3 *psqlite, uint64_t folder_id, DOUBLE_LIST *plist)
{
	void *pvalue;
	uint32_t state;
	uint32_t sequence;
	RULE_NODE *prnode;
	uint64_t message_id;
	char sql_string[256];
	DOUBLE_LIST_NODE *pnode;
	
	if (TRUE == exmdb_server_check_private()) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
				"FROM messages WHERE parent_fid=%llu AND "
				"is_associated=1", LLU(folder_id));
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
				"FROM messages WHERE parent_fid=%llu AND "
				"is_associated=1 AND is_deleted=0",
				LLU(folder_id));
	}
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	size_t count = 0, ext_count = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		count ++;
		if (count > MAX_FAI_COUNT) {
			break;
		}
		message_id = sqlite3_column_int64(pstmt, 0);
		if (FALSE == common_util_get_property(
			MESSAGE_PROPERTIES_TABLE, message_id, 0,
			psqlite, PROP_TAG_MESSAGECLASS, &pvalue)) {
			return FALSE;
		}
		if (pvalue != nullptr && strcasecmp(static_cast<char *>(pvalue),
		    "IPM.ExtendedRule.Message") != 0)
			continue;
		if (!common_util_get_property(MESSAGE_PROPERTIES_TABLE,
		    message_id, 0, psqlite, PR_RULE_MSG_STATE, &pvalue))
			return FALSE;
		if (NULL == pvalue) {
			continue;
		}
		state = *(uint32_t*)pvalue;
		if ((state & RULE_STATE_PARSE_ERROR)
			|| (state & RULE_STATE_ERROR)) {
			continue;
		}
		if (state & RULE_STATE_ENABLED) {
			/* do nothing */
		} else if (state & RULE_STATE_ONLY_WHEN_OOF) {
			if (FALSE == b_oof) {
				continue;
			}
		} else {
			continue;
		}
		if (!common_util_get_property(MESSAGE_PROPERTIES_TABLE,
		    message_id, 0, psqlite, PR_RULE_MSG_SEQUENCE, &pvalue))
			return FALSE;
		if (NULL == pvalue) {
			continue;
		}
		sequence = *(uint32_t*)pvalue;
		if (!common_util_get_property(MESSAGE_PROPERTIES_TABLE,
		    message_id, 0, psqlite, PR_RULE_MSG_PROVIDER, &pvalue))
			return FALSE;
		if (NULL == pvalue) {
			continue;
		}
		prnode = cu_alloc<RULE_NODE>();
		if (NULL == prnode) {
			return FALSE;
		}
		prnode->node.pdata = prnode;
		prnode->state = state;
		prnode->id = message_id;
		prnode->sequence = sequence;
		prnode->provider = static_cast<char *>(pvalue);
		for (pnode=double_list_get_head(plist); NULL!=pnode;
			pnode=double_list_get_after(plist, pnode)) {
			if (((RULE_NODE*)pnode->pdata)->sequence >= prnode->sequence) {
				double_list_insert_before(plist, pnode, &prnode->node);
				break;
			}
		}
		if (NULL == pnode) {
			double_list_append_as_tail(plist, &prnode->node);
		}
		ext_count ++;
		if (ext_count > common_util_get_param(
			COMMON_UTIL_MAX_EXT_RULE_NUMBER)) {
			break;
		}
	}
	return TRUE;
}

static BOOL message_get_real_propid(sqlite3 *psqlite,
	NAMEDPROPERTY_INFOMATION *ppropname_info,
	uint32_t *pproptag, BOOL *pb_replaced)
{
	int i;
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	
	uint16_t propid = PROP_ID(*pproptag);
	*pb_replaced = FALSE;
	if (!is_nameprop_id(propid))
		return TRUE;
	for (i=0; i<ppropname_info->count; i++) {
		if (propid == ppropname_info->ppropid[i]) {
			break;
		}
	}
	if (i >= ppropname_info->count) {
		return TRUE;
	}
	propnames.count = 1;
	propnames.ppropname = &ppropname_info->ppropname[i];
	if (FALSE == common_util_get_named_propids(
		psqlite, TRUE, &propnames, &propids)) {
		return FALSE;
	}
	if (1 != propids.count) {
		return TRUE;
	}
	propid = *propids.ppropid;
	if (0 == propid) {
		return TRUE;
	}
	(*pproptag) &= 0xFFFF;
	(*pproptag) |= ((uint32_t)propid) << 16;
	*pb_replaced = TRUE;
	return TRUE;
}

static BOOL message_replace_restriction_propid(sqlite3 *psqlite,
	NAMEDPROPERTY_INFOMATION *ppropname_info, RESTRICTION *pres)
{
	BOOL b_replaced;
	
	switch (pres->rt) {
	case RES_AND:
	case RES_OR:
		for (size_t i = 0; i < pres->andor->count; ++i)
			if (!message_replace_restriction_propid(psqlite,
			    ppropname_info, &pres->andor->pres[i]))
				return FALSE;
		break;
	case RES_NOT:
		if (!message_replace_restriction_propid(psqlite,
		    ppropname_info, &pres->xnot->res))
			return FALSE;
		break;
	case RES_CONTENT: {
		auto rcon = pres->cont;
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &rcon->proptag, &b_replaced))
			return FALSE;
		if (TRUE == b_replaced) {
			rcon->propval.proptag = rcon->proptag;
		}
		break;
	}
	case RES_PROPERTY: {
		auto rprop = pres->prop;
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &rprop->proptag, &b_replaced))
			return FALSE;
		if (TRUE == b_replaced) {
			rprop->propval.proptag = rprop->proptag;
		}
		break;
	}
	case RES_PROPCOMPARE: {
		auto rprop = pres->pcmp;
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &rprop->proptag1, &b_replaced))
			return FALSE;
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &rprop->proptag2, &b_replaced))
			return FALSE;
		break;
	}
	case RES_BITMASK:
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &pres->bm->proptag, &b_replaced))
			return FALSE;
		break;
	case RES_SIZE:
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &pres->size->proptag, &b_replaced))
			return FALSE;
		break;
	case RES_EXIST:
		if (!message_get_real_propid(psqlite, ppropname_info,
		    &pres->exist->proptag, &b_replaced))
			return FALSE;
		break;
	case RES_SUBRESTRICTION:
		if (!message_replace_restriction_propid(psqlite,
		    ppropname_info, &pres->sub->res))
			return FALSE;
		break;
	case RES_COMMENT: {
		auto rcom = pres->comment;
		for (size_t i = 0; i < rcom->count; ++i)
			if (!message_get_real_propid(psqlite, ppropname_info,
			    &rcom->ppropval[i].proptag, &b_replaced))
				return FALSE;
		if (rcom->pres != nullptr)
			if (!message_replace_restriction_propid(psqlite,
			    ppropname_info, rcom->pres))
				return FALSE;
		break;
	}
	case RES_COUNT:
		if (!message_replace_restriction_propid(psqlite,
		    ppropname_info, &pres->count->sub_res))
			return FALSE;
		break;
	default:
		return TRUE;
	}
	return TRUE;
}

static BOOL message_replace_actions_propid(sqlite3 *psqlite,
	NAMEDPROPERTY_INFOMATION *ppropname_info, EXT_RULE_ACTIONS *pactions)
{
	BOOL b_replaced;
	
	for (size_t i = 0; i < pactions->count; ++i)
		if (pactions->pblock[i].type == OP_TAG) {
			if (FALSE == message_get_real_propid(
				psqlite, ppropname_info, &((TAGGED_PROPVAL*)
				pactions->pblock[i].pdata)->proptag,
				&b_replaced)) {
				return FALSE;
			}
		}
	return TRUE;
}

static BOOL message_make_deferred_error_message(
	const char *username, sqlite3 *psqlite,
	uint64_t folder_id, uint64_t message_id,
	uint64_t rule_id, uint32_t rule_error,
	uint32_t action_type, uint32_t block_index,
	const char *provider, DOUBLE_LIST *pmsg_list)
{
	BOOL b_result;
	uint64_t tmp_eid;
	uint64_t mid_val;
	uint64_t nt_time;
	MESSAGE_NODE *pmnode;
	MESSAGE_CONTENT *pmsg;
	TAGGED_PROPVAL propval;
	
	if (FALSE == exmdb_server_check_private()) {
		return TRUE;
	}
	pmsg = message_content_init();
	if (NULL == pmsg) {
		return FALSE;
	}
	nt_time = rop_util_current_nttime();
	propval.proptag = PROP_TAG_CLIENTSUBMITTIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_CREATION_TIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_LAST_MODIFICATION_TIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PROP_TAG_MESSAGEDELIVERYTIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PROP_TAG_MESSAGECLASS;
	propval.pvalue  = deconst("IPC.Microsoft Exchange 4.0.Deferred Error");
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_ACTION_TYPE;
	propval.pvalue = &action_type;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_ACTION_NUMBER;
	propval.pvalue = &block_index;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_DAM_ORIGINAL_ENTRYID;
	propval.pvalue = common_util_to_private_message_entryid(
				psqlite, username, folder_id, message_id);
	if (NULL == propval.pvalue) {
		message_content_free(pmsg);
		return FALSE;
	}
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_FOLDER_ENTRYID;
	propval.pvalue = common_util_to_private_folder_entryid(
							psqlite, username, folder_id);
	if (NULL == propval.pvalue) {
		message_content_free(pmsg);
		return FALSE;
	}
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_PROVIDER;
	propval.pvalue = deconst(provider);
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	tmp_eid = rop_util_make_eid_ex(1, rule_id);
	propval.proptag = PR_RULE_ID;
	propval.pvalue = &tmp_eid;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	if (FALSE == message_write_message(FALSE, psqlite, username,
		0, FALSE, PRIVATE_FID_DEFERRED_ACTION, pmsg, &mid_val)) {
		message_content_free(pmsg);
		return FALSE;
	}
	message_content_free(pmsg);
	propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		PRIVATE_FID_DEFERRED_ACTION, 0, psqlite,
		&propval, &b_result);
	pmnode = cu_alloc<MESSAGE_NODE>();
	if (NULL == pmnode) {
		return FALSE;
	}
	pmnode->node.pdata = pmnode;
	pmnode->folder_id = PRIVATE_FID_DEFERRED_ACTION;
	pmnode->message_id = mid_val;
	double_list_append_as_tail(pmsg_list, &pmnode->node);
	return TRUE;
}

static BOOL message_disable_rule(sqlite3 *psqlite,
	BOOL b_extended, uint64_t id)
{
	void *pvalue;
	BOOL b_result;
	char sql_string[128];
	TAGGED_PROPVAL propval;
	
	if (FALSE == b_extended) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE rules SET state=state|%u "
		        "WHERE rule_id=%llu", RULE_STATE_ERROR, LLU(id));
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			return FALSE;
		}
	} else {
		if (FALSE == common_util_get_property(
			MESSAGE_PROPERTIES_TABLE, id, 0, psqlite,
			PR_RULE_MSG_STATE, &pvalue) ||
			NULL == pvalue) {
			return FALSE;
		}
		*(uint32_t*)pvalue |= RULE_STATE_ERROR;
		propval.proptag = PR_RULE_MSG_STATE;
		propval.pvalue = pvalue;
		if (!common_util_set_property(MESSAGE_PROPERTIES_TABLE, id, 0,
		    psqlite, &propval, &b_result))
			return FALSE;
	}
	return TRUE;
}

static BOOL message_get_propids(const PROPNAME_ARRAY *ppropnames,
	PROPID_ARRAY *ppropids)
{
	sqlite3 *psqlite;
	
	psqlite = (sqlite3*)common_util_get_tls_var();
	if (NULL == psqlite) {
		return FALSE;
	}
	if (FALSE == common_util_get_named_propids(
		psqlite, FALSE, ppropnames, ppropids)) {
		return FALSE;
	}
	return TRUE;
}

static BOOL message_get_propname(uint16_t propid,
	PROPERTY_NAME **pppropname)
{
	sqlite3 *psqlite;
	PROPID_ARRAY propids;
	PROPNAME_ARRAY propnames;
	
	psqlite = (sqlite3*)common_util_get_tls_var();
	if (NULL == psqlite) {
		return FALSE;
	}
	propids.count = 1;
	propids.ppropid = &propid;
	if (FALSE == common_util_get_named_propnames(
		psqlite, &propids, &propnames)) {
		return FALSE;
	}
	*pppropname = propnames.count != 1 ? nullptr : propnames.ppropname;
	return TRUE;
}

static BOOL message_auto_reply(sqlite3 *psqlite,
	uint64_t message_id, const char *from_address,
	const char *account, uint8_t action_type,
	uint32_t action_flavor, uint32_t template_message_id,
	GUID template_guid, BOOL *pb_result)
{
	MAIL imail;
	MIME *pmime;
	void *pvalue;
	GUID tmp_guid;
	BINARY tmp_bin;
	TARRAY_SET *prcpts;
	DOUBLE_LIST tmp_list;
	char content_type[128];
	TAGGED_PROPVAL propval;
	char tmp_buff[256*1024];
	MESSAGE_CONTENT *pmsgctnt;
	
	if (0 == strcasecmp(from_address, "none@none")) {
		*pb_result = TRUE;
		return TRUE;
	}
	if (FALSE == common_util_get_property(
		MESSAGE_PROPERTIES_TABLE, message_id, 0,
		psqlite, PROP_TAG_AUTORESPONSESUPPRESS, &pvalue)) {
		return FALSE;
	}
	if (NULL != pvalue) {
		if (action_type == OP_REPLY) {
			if ((*(uint32_t*)pvalue) &
				AUTO_RESPONSE_SUPPRESS_AUTOREPLY) {
				*pb_result = TRUE;
				return TRUE;
			}
		} else {
			if ((*(uint32_t*)pvalue) &
				AUTO_RESPONSE_SUPPRESS_OOF) {
				*pb_result = TRUE;
				return TRUE;
			}
		}
	}
	if (FALSE == message_read_message(psqlite,
		0, template_message_id, &pmsgctnt)) {
		return FALSE;
	}
	if (NULL == pmsgctnt) {
		*pb_result = FALSE;
		return TRUE;
	}
	pvalue = common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_MESSAGECLASS);
	if (NULL == pvalue) {
		*pb_result = FALSE;
		return TRUE;
	}
	if (action_type == OP_REPLY) {
		if (strncasecmp(static_cast<char *>(pvalue),
		    "IPM.Note.rules.ReplyTemplate.", 29) != 0) {
			*pb_result = FALSE;
			return TRUE;
		}
	} else {
		if (strncasecmp(static_cast<char *>(pvalue),
		    "IPM.Note.rules.", 15) != 0) {
			*pb_result = FALSE;
			return TRUE;
		}
	}
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_ASSOCIATED);
	if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
		*pb_result = FALSE;
		return TRUE;
	}
	pvalue = common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_REPLYTEMPLATEID);
	if (NULL == pvalue || 16 != ((BINARY*)pvalue)->cb) {
		*pb_result = FALSE;
		return TRUE;
	}
	tmp_guid = rop_util_binary_to_guid(static_cast<BINARY *>(pvalue));
	if (0 != guid_compare(&tmp_guid, &template_guid)) {
		*pb_result = FALSE;
		return TRUE;
	}
	if (action_flavor & ACTION_FLAVOR_NS) {
		if (NULL == pmsgctnt->children.prcpts ||
			0 == pmsgctnt->children.prcpts->count) {
			*pb_result = FALSE;
			return TRUE;
		}
	} else {
		prcpts = cu_alloc<TARRAY_SET>();
		if (NULL == prcpts) {
			return FALSE;
		}
		prcpts->count = 1;
		prcpts->pparray = cu_alloc<TPROPVAL_ARRAY *>(1);
		if (NULL == prcpts->pparray) {
			return FALSE;
		}
		*prcpts->pparray = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == *prcpts->pparray) {
			return FALSE;
		}
		(*prcpts->pparray)->ppropval = cu_alloc<TAGGED_PROPVAL>(3);
		if (NULL == (*prcpts->pparray)->ppropval) {
			return FALSE;
		}
		(*prcpts->pparray)->ppropval[0].proptag = PR_SMTP_ADDRESS;
		if (FALSE == common_util_get_property(
			MESSAGE_PROPERTIES_TABLE, message_id, 0, psqlite,
			PROP_TAG_SENTREPRESENTINGSMTPADDRESS, &pvalue)) {
			return FALSE;
		}
		(*prcpts->pparray)->ppropval[0].pvalue = pvalue == nullptr ?
			deconst(from_address) : pvalue;
		(*prcpts->pparray)->ppropval[1].proptag =
							PROP_TAG_RECIPIENTTYPE;
		pvalue = cu_alloc<uint32_t>();
		if (NULL == pvalue) {
			return FALSE;
		}
		*(uint32_t*)pvalue = RECIPIENT_TYPE_TO;
		(*prcpts->pparray)->ppropval[1].pvalue = pvalue; 
		if (FALSE == common_util_get_property(
			MESSAGE_PROPERTIES_TABLE, message_id, 0,
			psqlite, PROP_TAG_SENTREPRESENTINGNAME,
			&pvalue)) {
			return FALSE;
		}
		if (NULL == pvalue) {
			(*prcpts->pparray)->count = 2;
		} else {
			(*prcpts->pparray)->count = 3;
			(*prcpts->pparray)->ppropval[2].proptag = PR_DISPLAY_NAME;
			(*prcpts->pparray)->ppropval[2].pvalue = pvalue;
		}
		pmsgctnt->children.prcpts = prcpts;
	}
	if (action_flavor & ACTION_FLAVOR_ST) {
		if (FALSE == bounce_producer_make_content(
			from_address, account, psqlite,
			message_id, BOUNCE_AUTO_RESPONSE, NULL,
			NULL, content_type, tmp_buff)) {
			return FALSE;
		}
		common_util_remove_propvals(&pmsgctnt->proplist, PR_ASSOCIATED);
		common_util_remove_propvals(
			&pmsgctnt->proplist, PROP_TAG_MID);
		common_util_remove_propvals(&pmsgctnt->proplist, PR_BODY);
		common_util_remove_propvals(
			&pmsgctnt->proplist, PROP_TAG_HTML);
		common_util_remove_propvals(
			&pmsgctnt->proplist, PROP_TAG_RTFCOMPRESSED);
		if (0 == strcasecmp(content_type, "text/plain")) {
			propval.proptag = PR_BODY;
			propval.pvalue = tmp_buff;
			common_util_set_propvals(&pmsgctnt->proplist, &propval);
		} else if (0 == strcasecmp(content_type, "text/html")) {
			propval.proptag = PROP_TAG_HTML;
			propval.pvalue = &tmp_bin;
			pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_INTERNET_CPID);
			if (NULL != pvalue && 1200 != *(uint32_t*)pvalue) {
				tmp_bin.pc = common_util_convert_copy(
					FALSE, *(uint32_t*)pvalue, tmp_buff);
			} else {
				tmp_bin.pc = tmp_buff;
			}
			tmp_bin.cb = strlen(tmp_bin.pc);
			common_util_set_propvals(&pmsgctnt->proplist, &propval);
		}
	}
	common_util_set_tls_var(psqlite);
	if (FALSE == oxcmail_export(pmsgctnt, FALSE,
		OXCMAIL_BODY_PLAIN_AND_HTML, common_util_get_mime_pool(),
		&imail, common_util_alloc, message_get_propids,
		message_get_propname)) {
		common_util_set_tls_var(NULL);
		return FALSE;
	}
	common_util_set_tls_var(NULL);
	pmime = mail_get_head(&imail);
	if (NULL == pmime) {
		mail_free(&imail);
		return FALSE;
	}
	mime_set_field(pmime, "X-Auto-Response-Suppress", "All");
	const char *pvalue2 = strchr(from_address, '@');
	snprintf(tmp_buff, sizeof(tmp_buff), "auto-reply@%s", pvalue2 == nullptr ? "system.mail" : pvalue2 + 1);
	double_list_init(&tmp_list);
	if (FALSE == common_util_recipients_to_list(
		pmsgctnt->children.prcpts, &tmp_list)) {
		mail_free(&imail);
		return FALSE;
	}
	common_util_send_mail(&imail, tmp_buff, &tmp_list);
	mail_free(&imail);
	*pb_result = TRUE;
	return TRUE;
}

static BOOL message_bounce_message(const char *from_address,
	const char *account, sqlite3 *psqlite,
	uint64_t message_id, uint32_t bounce_code)
{
	MAIL imail;
	void *pvalue;
	int bounce_type;
	char tmp_buff[256];
	DOUBLE_LIST tmp_list;
	DOUBLE_LIST_NODE *pnode;
	
	if (0 == strcasecmp(from_address, "none@none") ||
		NULL == strchr(account, '@')) {
		return TRUE;
	}
	switch (bounce_code) {
	case BOUNCE_CODE_MESSAGE_TOO_LARGE:
		bounce_type = BOUNCE_MAIL_TOO_LARGE;
		break;
	case BOUNCE_CODE_MESSAGE_NOT_DISPLAYED:
		bounce_type = BOUNCE_CANNOT_DISPLAY;
		break;
	case BOUNCE_CODE_MESSAGE_DENIED:
		bounce_type = BOUNCE_GENERIC_ERROR;
		break;
	default:
		return TRUE;
	}
	double_list_init(&tmp_list);
	pnode = cu_alloc<DOUBLE_LIST_NODE>();
	if (NULL == pnode) {
		return FALSE;
	}
	double_list_append_as_tail(&tmp_list, pnode);
	if (FALSE == common_util_get_property(
		MESSAGE_PROPERTIES_TABLE, message_id, 0, psqlite,
		PROP_TAG_SENTREPRESENTINGSMTPADDRESS, &pvalue)) {
		return FALSE;
	}
	pnode->pdata = pvalue == nullptr ? deconst(from_address) : pvalue;
	mail_init(&imail, common_util_get_mime_pool());
	if (FALSE == bounce_producer_make(from_address,
		account, psqlite, message_id, bounce_type,
		&imail)) {
		mail_free(&imail);
		return FALSE;
	}
	const char *pvalue2 = strchr(account, '@');
	snprintf(tmp_buff, sizeof(tmp_buff), "postmaster@%s",
	         pvalue2 == nullptr ? "system.mail" : pvalue2 + 1);
	common_util_send_mail(&imail, tmp_buff, &tmp_list);
	mail_free(&imail);
	return TRUE;
}

static BOOL message_recipient_blocks_to_list(uint32_t count,
	RECIPIENT_BLOCK *pblock, DOUBLE_LIST *prcpt_list)
{
	TARRAY_SET rcpts;
	
	double_list_init(prcpt_list);
	rcpts.count = count;
	rcpts.pparray = cu_alloc<TPROPVAL_ARRAY *>(count);
	if (NULL == rcpts.pparray) {
		return FALSE;
	}
	for (size_t i = 0; i < count; ++i) {
		rcpts.pparray[i] = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == rcpts.pparray[i]) {
			return FALSE;
		}
		rcpts.pparray[i]->count = pblock[i].count;
		rcpts.pparray[i]->ppropval = pblock[i].ppropval;
	}
	return common_util_recipients_to_list(&rcpts, prcpt_list);
}

static BOOL message_ext_recipient_blocks_to_list(uint32_t count,
	EXT_RECIPIENT_BLOCK *pblock, DOUBLE_LIST *prcpt_list)
{
	TARRAY_SET rcpts;
	
	double_list_init(prcpt_list);
	rcpts.count = count;
	rcpts.pparray = cu_alloc<TPROPVAL_ARRAY *>(count);
	if (NULL == rcpts.pparray) {
		return FALSE;
	}
	for (size_t i = 0; i < count; ++i) {
		rcpts.pparray[i] = cu_alloc<TPROPVAL_ARRAY>();
		if (NULL == rcpts.pparray[i]) {
			return FALSE;
		}
		rcpts.pparray[i]->count = pblock[i].count;
		rcpts.pparray[i]->ppropval = pblock[i].ppropval;
	}
	return common_util_recipients_to_list(&rcpts, prcpt_list);
}

static BOOL message_forward_message(const char *from_address,
	const char *username, sqlite3 *psqlite, uint32_t cpid,
	uint64_t message_id, const char *pdigest, uint32_t action_flavor,
	BOOL b_extended, uint32_t count, void *pblock)
{
	int i;
	int num;
	int offset;
	MAIL imail;
	MAIL imail1;
	char *pbuff;
	MIME *pmime;
	void *pvalue;
	int body_type;
	const char *pdomain;
	time_t cur_time;
	char tmp_path[256];
	struct tm time_buff;
	char mid_string[128];
	struct stat node_stat;
	DOUBLE_LIST rcpt_list;
	char tmp_buff[64*1024];
	DOUBLE_LIST_NODE *pnode;
	MESSAGE_CONTENT *pmsgctnt;
	
	pdomain = strchr(username, '@');
	if (NULL != pdomain) {
		pdomain ++;
	} else {
		pdomain = "system.mail";
	}
	if (FALSE == b_extended) {
		if (!message_recipient_blocks_to_list(count,
		    static_cast<RECIPIENT_BLOCK *>(pblock), &rcpt_list))
			return FALSE;
	} else {
		if (!message_ext_recipient_blocks_to_list(count,
		    static_cast<EXT_RECIPIENT_BLOCK *>(pblock), &rcpt_list))
			return FALSE;
	}
	if (NULL != pdigest) {
		get_digest(pdigest, "file", mid_string, arsizeof(mid_string));
		snprintf(tmp_path, arsizeof(tmp_path), "%s/eml/%s",
			exmdb_server_get_dir(), mid_string);
		wrapfd fd = open(tmp_path, O_RDONLY);
		if (fd.get() < 0 || fstat(fd.get(), &node_stat) != 0)
			return false;
		pbuff = me_alloc<char>(node_stat.st_size);
		if (NULL == pbuff) {
			return FALSE;
		}
		if (read(fd.get(), pbuff, node_stat.st_size) != node_stat.st_size) {
			free(pbuff);
			return FALSE;
		}
		mail_init(&imail, common_util_get_mime_pool());
		if (FALSE == mail_retrieve(&imail, pbuff, node_stat.st_size)) {
			free(pbuff);
			return FALSE;
		}
		pmime = mail_get_head(&imail);
		if (NULL == pmime) {
			mail_free(&imail);
			free(pbuff);
			return FALSE;
		}
		num = mime_get_field_num(pmime, "Delivered-To");
		for (i=0; i<num; i++) {
			if (TRUE == mime_search_field(pmime,
				"Delivered-To", i, tmp_buff, 256)) {
				if (0 == strcasecmp(tmp_buff, username)) {
					mail_free(&imail);
					free(pbuff);
					return TRUE;
				}
			}
		}
	} else {
		pbuff = NULL;
		if (FALSE == message_read_message(psqlite, cpid,
			message_id, &pmsgctnt) || NULL == pmsgctnt) {
			return FALSE;
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
		/* try to avoid TNEF message */
		common_util_set_tls_var(psqlite);
		if (FALSE == oxcmail_export(pmsgctnt, FALSE,
			body_type, common_util_get_mime_pool(), &imail,
			common_util_alloc, message_get_propids,
			message_get_propname)) {
			common_util_set_tls_var(NULL);
			return FALSE;
		}
		common_util_set_tls_var(NULL);
	}
	if (action_flavor & ACTION_FLAVOR_AT) {
		mail_init(&imail1, common_util_get_mime_pool());
		pmime = mail_add_head(&imail1);
		if (NULL == pmime) {
			mail_free(&imail);
			if (NULL != pbuff) {
				free(pbuff);
			}
			return FALSE;
		}
		mime_set_content_type(pmime, "message/rfc822");
		if (action_flavor & ACTION_FLAVOR_PR) {
			snprintf(tmp_buff, arsizeof(tmp_buff), "<%s>", from_address);
		} else {
			snprintf(tmp_buff, arsizeof(tmp_buff), "\"Forwarder\"<forwarder@%s>", pdomain);
		}
		mime_set_field(pmime, "From", tmp_buff);
		offset = 0;
		for (pnode=double_list_get_head(&rcpt_list); NULL!=pnode;
			pnode=double_list_get_after(&rcpt_list, pnode)) {
			if (0 == offset) {
				offset = gx_snprintf(tmp_buff, GX_ARRAY_SIZE(tmp_buff),
				         "<%s>", static_cast<const char *>(pnode->pdata));
			} else {
				offset += gx_snprintf(tmp_buff + offset,
				          GX_ARRAY_SIZE(tmp_buff) - offset, ", <%s>",
				          static_cast<const char *>(pnode->pdata));
			}
			mime_append_field(pmime, "Delivered-To", static_cast<char *>(pnode->pdata));
		}
		mime_set_field(pmime, "To", tmp_buff);
		snprintf(tmp_buff, arsizeof(tmp_buff), "Automatic forwarded message from %s", username);
		mime_set_field(pmime, "Subject", tmp_buff);
		time(&cur_time);
		strftime(tmp_buff, 128, "%a, %d %b %Y %H:%M:%S %z", 
			localtime_r(&cur_time, &time_buff));
		mime_set_field(pmime, "Date", tmp_buff);
		mime_write_mail(pmime, &imail);
		if (action_flavor & ACTION_FLAVOR_PR) {
			strcpy(tmp_buff, from_address);
		} else {
			snprintf(tmp_buff, arsizeof(tmp_buff), "forwarder@%s", pdomain);
		}
		common_util_send_mail(&imail1, tmp_buff, &rcpt_list);
		mail_free(&imail1);
	} else {
		pmime = mail_get_head(&imail);
		if (NULL == pmime) {
			mail_free(&imail);
			if (NULL != pbuff) {
				free(pbuff);
			}
			return FALSE;
		}
		for (pnode=double_list_get_head(&rcpt_list); NULL!=pnode;
			pnode=double_list_get_after(&rcpt_list, pnode)) {
			mime_append_field(pmime, "Delivered-To", static_cast<char *>(pnode->pdata));
		}
		if (action_flavor & ACTION_FLAVOR_PR) {
			strcpy(tmp_buff, from_address);
		} else {
			snprintf(tmp_buff, arsizeof(tmp_buff), "forwarder@%s", pdomain);
		}
		common_util_send_mail(&imail, tmp_buff, &rcpt_list);
	}
	mail_free(&imail);
	if (NULL != pbuff) {
		free(pbuff);
	}
	return TRUE;
}

static BOOL message_make_deferred_action_message(
	const char *username, sqlite3 *psqlite,
	uint64_t folder_id, uint64_t message_id,
	const char *provider, DOUBLE_LIST *pdam_list,
	DOUBLE_LIST *pmsg_list)
{
	int i;
	int id_count;
	SVREID svreid;
	BOOL b_result;
	uint64_t tmp_eid;
	uint64_t mid_val;
	uint64_t nt_time;
	uint8_t tmp_byte;
	DAM_NODE *pdnode;
	EXT_PUSH ext_push;
	MESSAGE_NODE *pmnode;
	RULE_ACTIONS actions;
	MESSAGE_CONTENT *pmsg;
	TAGGED_PROPVAL propval;
	DOUBLE_LIST_NODE *pnode;
	uint64_t tmp_ids[MAX_DAMS_PER_RULE_FOLDER];
	
	pmsg = message_content_init();
	if (NULL == pmsg) {
		return FALSE;
	}
	nt_time = rop_util_current_nttime();
	propval.proptag = PROP_TAG_CLIENTSUBMITTIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_CREATION_TIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_LAST_MODIFICATION_TIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PROP_TAG_MESSAGEDELIVERYTIME;
	propval.pvalue = &nt_time;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PROP_TAG_MESSAGECLASS;
	propval.pvalue  = deconst("IPC.Microsoft Exchange 4.0.Deferred Action");
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PROP_TAG_DAMBACKPATCHED;
	propval.pvalue = &tmp_byte;
	tmp_byte = 0;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_DAM_ORIGINAL_ENTRYID;
	propval.pvalue = common_util_to_private_message_entryid(
					psqlite, username, folder_id, message_id);
	if (NULL == propval.pvalue) {
		message_content_free(pmsg);
		return FALSE;
	}
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PROP_TAG_DEFERREDACTIONMESSAGEORIGINALENTRYID;
	propval.pvalue = &svreid;
	svreid.pbin = NULL;
	svreid.folder_id = rop_util_make_eid_ex(1, folder_id);
	svreid.message_id = rop_util_make_eid_ex(1, message_id);
	svreid.instance = 0;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_FOLDER_FID;
	propval.pvalue = &tmp_eid;
	tmp_eid = rop_util_make_eid_ex(1, folder_id);
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_FOLDER_ENTRYID;
	propval.pvalue = common_util_to_private_folder_entryid(
							psqlite, username, folder_id);
	if (NULL == propval.pvalue) {
		message_content_free(pmsg);
		return FALSE;
	}
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	propval.proptag = PR_RULE_PROVIDER;
	propval.pvalue = deconst(provider);
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	actions.pblock = static_cast<ACTION_BLOCK *>(common_util_alloc(sizeof(ACTION_BLOCK) *
	                 double_list_get_nodes_num(pdam_list)));
	if (NULL == actions.pblock) {
		message_content_free(pmsg);
		return FALSE;
	}
	actions.count = 0;
	id_count = 0;
	for (pnode=double_list_get_head(pdam_list); NULL!=pnode;
		pnode=double_list_get_after(pdam_list, pnode)) {
		pdnode = (DAM_NODE*)pnode->pdata;
		memcpy(&actions.pblock[actions.count],
			pdnode->pblock, sizeof(ACTION_BLOCK));
		actions.count ++;
		tmp_eid = rop_util_make_eid_ex(1, pdnode->rule_id);
		for (i=0; i<id_count; i++) {
			if (tmp_ids[i] == tmp_eid) {
				break;
			}
		}
		if (i >= id_count) {
			tmp_ids[id_count] = tmp_eid;
			id_count ++;
		}
	}
	if (!ext_push.init(nullptr, 0, EXT_FLAG_UTF16) ||
	    ext_push.p_rule_actions(&actions) != EXT_ERR_SUCCESS) {
		message_content_free(pmsg);
		return FALSE;
	}
	BINARY tmp_bin;
	tmp_bin.pb = ext_push.m_udata;
	tmp_bin.cb = ext_push.m_offset;
	propval.proptag = PROP_TAG_CLIENTACTIONS;
	propval.pvalue = &tmp_bin;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	tmp_bin.pv = tmp_ids;
	tmp_bin.cb = sizeof(uint64_t)*id_count;
	propval.proptag = PR_RULE_IDS;
	propval.pvalue = &tmp_bin;
	if (!tpropval_array_set_propval(&pmsg->proplist, &propval)) {
		message_content_free(pmsg);
		return FALSE;
	}
	if (FALSE == message_write_message(FALSE, psqlite, username,
		0, FALSE, PRIVATE_FID_DEFERRED_ACTION, pmsg, &mid_val)) {
		message_content_free(pmsg);
		return FALSE;
	}
	message_content_free(pmsg);
	propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		PRIVATE_FID_DEFERRED_ACTION, 0, psqlite,
		&propval, &b_result);
	pmnode = cu_alloc<MESSAGE_NODE>();
	if (NULL == pmnode) {
		return FALSE;
	}
	pmnode->node.pdata = pmnode;
	pmnode->folder_id = PRIVATE_FID_DEFERRED_ACTION;
	pmnode->message_id = mid_val;
	double_list_append_as_tail(pmsg_list, &pmnode->node);
	return TRUE;
}

static BOOL message_make_deferred_action_messages(
	const char *username, sqlite3 *psqlite,
	uint64_t folder_id, uint64_t message_id,
	DOUBLE_LIST *pdam_list, DOUBLE_LIST *pmsg_list)
{
	DAM_NODE *pdnode;
	DOUBLE_LIST tmp_list;
	const char *provider;
	DOUBLE_LIST_NODE *pnode;
	DOUBLE_LIST_NODE *ptail;
	
	if (FALSE == exmdb_server_check_private()) {
		return TRUE;
	}
	if (double_list_get_nodes_num(pdam_list) >
		MAX_DAMS_PER_RULE_FOLDER) {
		common_util_log_info(LV_NOTICE, "user=%s host=unknown  "
			"DAM error: Too many Deferred Actions "
			"triggered by message %llu in folder "
			"%llu", username, LLU(message_id), LLU(folder_id));
		return TRUE;
	}
	provider = NULL;
	double_list_init(&tmp_list);
	ptail = double_list_get_tail(pdam_list);
	while ((pnode = double_list_pop_front(pdam_list)) != nullptr) {
		pdnode = (DAM_NODE*)pnode->pdata;
		if (NULL != provider) {
			if (0 == strcasecmp(provider, pdnode->provider)) {
				double_list_append_as_tail(&tmp_list, pnode);
			} else {
				double_list_append_as_tail(pdam_list, pnode);
			}
		} else {
			provider = pdnode->provider;
			double_list_append_as_tail(&tmp_list, pnode);
		}
		if (pnode == ptail) {
			if (FALSE == message_make_deferred_action_message(
				username, psqlite, folder_id, message_id,
				provider, &tmp_list, pmsg_list)) {
				return FALSE;
			}
			provider = NULL;
			double_list_init(&tmp_list);
			ptail = double_list_get_tail(pdam_list);
		}
	}
	return TRUE;
}

static bool op_move_std(BOOL b_oof, const char *from_address,
    const char *account, uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id,
    uint64_t message_id, const char *pdigest, DOUBLE_LIST *pfolder_list,
    DOUBLE_LIST *pmsg_list, const ACTION_BLOCK &block, size_t rule_idx,
    RULE_NODE *prnode, BOOL &b_del)
{
	auto pmovecopy = static_cast<MOVECOPY_ACTION *>(block.pdata);
	auto dst_fid = rop_util_get_gc_value(static_cast<SVREID *>(
		       pmovecopy->pfolder_eid)->folder_id);
	DOUBLE_LIST_NODE *pnode1;
	for (pnode1 = double_list_get_head(pfolder_list);
	     NULL != pnode1; pnode1 = double_list_get_after(
	     pfolder_list, pnode1)) {
		if (dst_fid == *(uint64_t *)pnode1->pdata) {
			break;
		}
	}
	if (NULL != pnode1) {
		return true;
	}
	BOOL b_exist = false;
	if (FALSE == common_util_check_folder_id(
	    psqlite, dst_fid, &b_exist)) {
		return FALSE;
	}
	if (FALSE == b_exist) {
		message_make_deferred_error_message(account,
			psqlite, folder_id, message_id, prnode->id,
			RULE_ERROR_MOVECOPY, block.type,
			rule_idx, prnode->provider, pmsg_list);
		if (FALSE == message_disable_rule(
		    psqlite, FALSE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	int tmp_id = 0, tmp_id1 = 0;
	if (TRUE == exmdb_server_check_private()) {
		if (FALSE == common_util_get_id_from_username(
		    account, &tmp_id)) {
			return FALSE;
		}
	} else {
		if (FALSE == common_util_get_domain_ids(
		    account, &tmp_id, &tmp_id1)) {
			return FALSE;
		}
	}
	uint64_t dst_mid = 0;
	uint32_t message_size = 0;
	BOOL b_result = false;
	if (FALSE == common_util_copy_message(
	    psqlite, tmp_id, message_id, dst_fid,
	    &dst_mid, &b_result, &message_size)) {
		return FALSE;
	}
	if (FALSE == b_result) {
		message_make_deferred_error_message(account,
			psqlite, folder_id, message_id, prnode->id,
			RULE_ERROR_MOVECOPY, block.type,
			rule_idx, prnode->provider, pmsg_list);
		return true;
	}
	auto nt_time = rop_util_current_nttime();
	TAGGED_PROPVAL propval;
	propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		dst_fid, 0, psqlite, &propval, &b_result);
	if (FALSE == common_util_increase_store_size(
	    psqlite, message_size, 0)) {
		return FALSE;
	}
	pnode1 = cu_alloc<DOUBLE_LIST_NODE>();
	if (NULL == pnode1) {
		return FALSE;
	}
	pnode1->pdata = cu_alloc<uint64_t>();
	if (NULL == pnode1->pdata) {
		return FALSE;
	}
	*(uint64_t *)pnode1->pdata = dst_fid;
	double_list_append_as_tail(pfolder_list, pnode1);
	char tmp_buff[MAX_DIGLEN];
	char *pmid_string = nullptr, *pdigest1 = nullptr;
	if (NULL != pdigest && TRUE == common_util_get_mid_string(
	    psqlite, dst_mid, &pmid_string) && NULL != pmid_string) {
		strcpy(tmp_buff, pdigest);
		char mid_string[128];
		sprintf(mid_string, "\"%s\"", pmid_string);
		set_digest(tmp_buff, MAX_DIGLEN, "file", mid_string);
		pdigest1 = tmp_buff;
	} else {
		pdigest1 = NULL;
	}
	if (FALSE == message_rule_new_message(b_oof,
	    from_address, account, cpid, psqlite,
	    dst_fid, dst_mid, pdigest1, pfolder_list,
	    pmsg_list)) {
		return FALSE;
	}
	if (block.type == OP_MOVE) {
		b_del = TRUE;
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be moved to %llu in folder %llu by"
			" rule", account, LLU(message_id), LLU(folder_id),
			LLU(dst_mid), LLU(dst_fid));
	} else {
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be copied to %llu in folder %llu by"
			" rule", account, LLU(message_id), LLU(folder_id),
			LLU(dst_mid), LLU(dst_fid));
	}
	return true;
}

static bool op_move_same(uint64_t folder_id, uint64_t message_id,
    RULE_NODE *prnode, const ACTION_BLOCK &block, DOUBLE_LIST &dam_list)
{
	if (FALSE == exmdb_server_check_private()) {
		return true;
	}
	auto pdnode = cu_alloc<DAM_NODE>();
	if (NULL == pdnode) {
		return FALSE;
	}
	pdnode->node.pdata = pdnode;
	pdnode->rule_id = prnode->id;
	pdnode->folder_id = folder_id;
	pdnode->message_id = message_id;
	pdnode->provider = prnode->provider;
	pdnode->pblock = &block;
	double_list_append_as_tail(
		&dam_list, &pdnode->node);
	return true;
}

static bool op_reply(const char *from_address, const char *account,
    sqlite3 *psqlite, uint64_t folder_id, uint64_t message_id,
    DOUBLE_LIST *pmsg_list, const ACTION_BLOCK &block, size_t rule_idx,
    RULE_NODE *prnode)
{
	auto preply = static_cast<REPLY_ACTION *>(block.pdata);
	BOOL b_result = false;
	if (FALSE == message_auto_reply(psqlite, message_id,
	    from_address, account, block.type,
	    block.flavor, rop_util_get_gc_value(
	    preply->template_message_id), preply->template_guid,
	    &b_result)) {
		return FALSE;
	}
	if (FALSE == b_result) {
		message_make_deferred_error_message(
			account, psqlite, folder_id, message_id,
			prnode->id, RULE_ERROR_RETRIEVE_TEMPLATE,
			block.type, rule_idx, prnode->provider,
			pmsg_list);
		if (FALSE == message_disable_rule(
		    psqlite, FALSE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	return true;
}

static bool op_defer(uint64_t folder_id, uint64_t message_id,
    const ACTION_BLOCK &block, RULE_NODE *prnode, DOUBLE_LIST &dam_list)
{
	if (FALSE == exmdb_server_check_private()) {
		return true;
	}
	auto pdnode = cu_alloc<DAM_NODE>();
	if (NULL == pdnode) {
		return FALSE;
	}
	pdnode->node.pdata = pdnode;
	pdnode->rule_id = prnode->id;
	pdnode->folder_id = folder_id;
	pdnode->message_id = message_id;
	pdnode->provider = prnode->provider;
	pdnode->pblock = &block;
	double_list_append_as_tail(
		&dam_list, &pdnode->node);
	return true;
}

static bool op_forward(const char *from_address, const char *account,
    uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id, uint64_t message_id,
    const char *pdigest, DOUBLE_LIST *pmsg_list, const ACTION_BLOCK &block,
    size_t rule_idx, RULE_NODE *prnode)
{
	if (FALSE == exmdb_server_check_private()) {
		return true;
	}
	auto pfwddlgt = static_cast<FORWARDDELEGATE_ACTION *>(block.pdata);
	if (pfwddlgt->count > MAX_RULE_RECIPIENTS) {
		message_make_deferred_error_message(
			account, psqlite, folder_id,
			message_id, prnode->id,
			RULE_ERROR_TOO_MANY_RCPTS,
			block.type, rule_idx,
			prnode->provider, pmsg_list);
		if (FALSE == message_disable_rule(
		    psqlite, FALSE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	if (FALSE == message_forward_message(from_address,
	    account, psqlite, cpid, message_id, pdigest,
	    block.flavor, false,
	    pfwddlgt->count, pfwddlgt->pblock))
		return FALSE;
	return true;
}

static bool op_delegate(const char *from_address, const char *account,
    uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id, uint64_t message_id,
    const char *pdigest, DOUBLE_LIST *pmsg_list, const ACTION_BLOCK &block,
    size_t rule_idx, RULE_NODE *prnode)
{
	auto pfwddlgt = static_cast<FORWARDDELEGATE_ACTION *>(block.pdata);
	if (FALSE == exmdb_server_check_private() ||
		NULL == pdigest || 0 == pfwddlgt->count) {
		return true;
	}
	if (pfwddlgt->count > MAX_RULE_RECIPIENTS) {
		message_make_deferred_error_message(
			account, psqlite, folder_id,
			message_id, prnode->id,
			RULE_ERROR_TOO_MANY_RCPTS,
			block.type, rule_idx,
			prnode->provider, pmsg_list);
		if (FALSE == message_disable_rule(
		    psqlite, FALSE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	MESSAGE_CONTENT *pmsgctnt = nullptr;
	if (FALSE == message_read_message(psqlite, cpid,
	    message_id, &pmsgctnt) || NULL == pmsgctnt) {
		return FALSE;
	}
	if (NULL != common_util_get_propvals(
	    &pmsgctnt->proplist, PROP_TAG_DELEGATEDBYRULE)) {
		common_util_log_info(LV_DEBUG, "user=%s host=unkonwn  Delegated"
			" message %llu in folder %llu cannot be delegated"
			" again", account, LLU(message_id), LLU(folder_id));
		return true;
	}
	static constexpr uint32_t tags[] = {
		PR_DISPLAY_TO, PR_DISPLAY_TO_A,
		PR_DISPLAY_CC, PR_DISPLAY_CC_A,
		PR_DISPLAY_BCC, PR_DISPLAY_BCC_A,
		PROP_TAG_MID, PR_MESSAGE_SIZE,
		PR_ASSOCIATED, PROP_TAG_CHANGENUMBER,
		PR_CHANGE_KEY, PR_READ,
		PROP_TAG_HASATTACHMENTS,
		PR_PREDECESSOR_CHANGE_LIST,
		PROP_TAG_MESSAGETOME, PROP_TAG_MESSAGECCME
	};
	for (auto t : tags)
		common_util_remove_propvals(&pmsgctnt->proplist, t);
	if (NULL == common_util_get_propvals(&pmsgctnt->proplist,
	    PROP_TAG_RECEIVEDREPRESENTINGENTRYID)) {
		char essdn_buff[1280];
		memcpy(essdn_buff, "EX:", 3);
		if (!common_util_username_to_essdn(account,
		    essdn_buff + 3, GX_ARRAY_SIZE(essdn_buff) - 3))
			return FALSE;
		HX_strupper(essdn_buff);
		auto pvalue = common_util_username_to_addressbook_entryid(account);
		if (NULL == pvalue) {
			return FALSE;
		}
		TAGGED_PROPVAL propval;
		propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGENTRYID;
		propval.pvalue = pvalue;
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
		propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGADDRESSTYPE;
		propval.pvalue  = deconst("EX");
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
		propval.proptag =
			PROP_TAG_RECEIVEDREPRESENTINGEMAILADDRESS;
		propval.pvalue = essdn_buff + 3;
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
		char display_name[1024];
		if (TRUE == common_util_get_user_displayname(
		    account, display_name)) {
			propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGNAME;
			propval.pvalue = display_name;
			common_util_set_propvals(
				&pmsgctnt->proplist, &propval);
		}
		BINARY searchkey_bin;
		searchkey_bin.cb = strlen(essdn_buff) + 1;
		searchkey_bin.pv = essdn_buff;
		propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGSEARCHKEY;
		propval.pvalue = &searchkey_bin;
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
	}
	static constexpr uint8_t fake_true = true;
	TAGGED_PROPVAL propval;
	propval.proptag = PROP_TAG_DELEGATEDBYRULE;
	propval.pvalue = deconst(&fake_true);
	common_util_set_propvals(&pmsgctnt->proplist, &propval);
	DOUBLE_LIST rcpt_list;
	if (FALSE == message_recipient_blocks_to_list(
	    pfwddlgt->count, pfwddlgt->pblock, &rcpt_list)) {
		return FALSE;
	}
	char mid_string1[128], tmp_path1[256];
	get_digest(pdigest, "file", mid_string1, arsizeof(mid_string1));
	snprintf(tmp_path1, arsizeof(tmp_path1), "%s/eml/%s",
		 exmdb_server_get_dir(), mid_string1);
	for (auto pnode1 = double_list_get_head(&rcpt_list);
	     NULL != pnode1; pnode1 = double_list_get_after(
	     &rcpt_list, pnode1)) {
		char maildir[256];
		if (!common_util_get_maildir(static_cast<char *>(pnode1->pdata), maildir))
			continue;
		auto mid_string = std::to_string(time(nullptr)) + "." +
				  std::to_string(common_util_sequence_ID()) + "." +
				  get_host_ID();
		auto eml_path = maildir + "/eml/"s + mid_string;
		if (!common_util_copy_file(tmp_path1, eml_path.c_str()))
			continue;
		char tmp_buff[MAX_DIGLEN];
		strcpy(tmp_buff, pdigest);
		auto mid_string1 = "\"" + mid_string + "\"";
		set_digest(tmp_buff, MAX_DIGLEN, "file", mid_string1.c_str());
		const char *pdigest1 = tmp_buff;
		uint32_t result = 0;
		if (!exmdb_client_relay_delivery(maildir,
		    from_address, static_cast<char *>(pnode1->pdata),
		    cpid, pmsgctnt, pdigest1, &result))
			return FALSE;
	}
	return true;
}

static bool op_switcheroo(BOOL b_oof, const char *from_address,
    const char *account, uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id,
    uint64_t message_id, const char *pdigest, DOUBLE_LIST *pfolder_list,
    DOUBLE_LIST *pmsg_list, const ACTION_BLOCK &block, size_t rule_idx,
    RULE_NODE *prnode, BOOL &b_del, DOUBLE_LIST &dam_list)
{
	static const uint8_t fake_true = 1;
	switch (block.type) {
	case OP_MOVE:
	case OP_COPY: {
		auto pmovecopy = static_cast<MOVECOPY_ACTION *>(block.pdata);
		auto ret = pmovecopy->same_store ?
		           op_move_same(folder_id, message_id, prnode, block, dam_list) :
		           op_move_std(b_oof, from_address, account, cpid,
		           psqlite, folder_id, message_id, pdigest, pfolder_list,
		           pmsg_list, block, rule_idx, prnode, b_del);
		if (!ret)
			return false;
		break;
	}
	case OP_REPLY:
	case OP_OOF_REPLY:
		if (!op_reply(from_address, account, psqlite, folder_id,
		    message_id, pmsg_list, block, rule_idx, prnode))
			return false;
		break;
	case OP_DEFER_ACTION:
		if (!op_defer(folder_id, message_id, block, prnode, dam_list))
			return false;
		break;
	case OP_BOUNCE:
		if (FALSE == message_bounce_message(
		    from_address, account, psqlite, message_id,
		    *static_cast<uint32_t *>(block.pdata)))
			return FALSE;
		b_del = TRUE;
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be deleted by rule", account,
			LLU(message_id), LLU(folder_id));
		break;
	case OP_FORWARD:
		if (!op_forward(from_address, account, cpid, psqlite, folder_id,
		    message_id, pdigest, pmsg_list, block, rule_idx, prnode))
			return false;
		break;
	case OP_DELEGATE:
		if (!op_delegate(from_address, account, cpid, psqlite,
		    folder_id, message_id, pdigest, pmsg_list, block,
		    rule_idx, prnode))
			return false;
		break;
	case OP_TAG: {
		BOOL b_result = false;
		if (!common_util_set_property(MESSAGE_PROPERTIES_TABLE,
		    message_id, cpid, psqlite,
		    static_cast<TAGGED_PROPVAL *>(block.pdata),
		    &b_result))
			return FALSE;
		break;
	}
	case OP_DELETE:
		b_del = TRUE;
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be deleted by rule", account,
			LLU(message_id), LLU(folder_id));
		break;
	case OP_MARK_AS_READ: {
		if (FALSE == exmdb_server_check_private()) {
			return true;
		}
		TAGGED_PROPVAL propval;
		propval.proptag = PR_READ;
		propval.pvalue = deconst(&fake_true);
		BOOL b_result = false;
		if (FALSE == common_util_set_property(
		    MESSAGE_PROPERTIES_TABLE, message_id,
		    0, psqlite, &propval, &b_result)) {
			return FALSE;
		}
		break;
	}
	}
	return true;
}

static bool opx_move_private(const char *account, sqlite3 *psqlite,
    RULE_NODE *prnode, const EXT_MOVECOPY_ACTION *pextmvcp)
{
	if (EITLT_PRIVATE_FOLDER !=
	    pextmvcp->folder_eid.folder_type) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	int tmp_id = 0;
	if (FALSE == common_util_get_id_from_username(
	    account, &tmp_id)) {
		return true;
	}
	auto tmp_guid = rop_util_make_user_guid(tmp_id);
	if (0 != guid_compare(&tmp_guid,
	    &pextmvcp->folder_eid.database_guid)) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	return true;
}

static bool opx_move_public(const char *account, sqlite3 *psqlite,
    RULE_NODE *prnode, const EXT_MOVECOPY_ACTION *pextmvcp)
{
	if (EITLT_PUBLIC_FOLDER !=
	    pextmvcp->folder_eid.folder_type) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	auto pc = strchr(account, '@');
	if (pc == nullptr)
		pc = account;
	else
		++pc;
	int tmp_id = 0, tmp_id1 = 0;
	if (!common_util_get_domain_ids(pc, &tmp_id, &tmp_id1))
		return true;
	auto tmp_guid = rop_util_make_domain_guid(tmp_id);
	if (0 != guid_compare(&tmp_guid,
	    &pextmvcp->folder_eid.database_guid)) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	return true;
}

static bool opx_move(BOOL b_oof, const char *from_address,
    const char *account, uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id,
    uint64_t message_id, const char *pdigest, DOUBLE_LIST *pfolder_list,
    DOUBLE_LIST *pmsg_list, const EXT_ACTION_BLOCK &block, RULE_NODE *prnode,
    BOOL &b_del)
{
	auto pextmvcp = static_cast<EXT_MOVECOPY_ACTION *>(block.pdata);
	auto ret = exmdb_server_check_private() ?
	           opx_move_private(account, psqlite, prnode, pextmvcp) :
	           opx_move_public(account, psqlite, prnode, pextmvcp);
	if (!ret)
		return false;
	auto dst_fid = rop_util_gc_to_value(
		       pextmvcp->folder_eid.global_counter);
	DOUBLE_LIST_NODE *pnode1;
	for (pnode1 = double_list_get_head(pfolder_list);
	     NULL != pnode1; pnode1 = double_list_get_after(
	     pfolder_list, pnode1)) {
		if (dst_fid == *(uint64_t *)pnode1->pdata) {
			break;
		}
	}
	if (NULL != pnode1) {
		return true;
	}
	BOOL b_exist = false;
	if (FALSE == common_util_check_folder_id(
	    psqlite, dst_fid, &b_exist)) {
		return FALSE;
	}
	if (FALSE == b_exist) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	int tmp_id = 0, tmp_id1 = 0;
	if (TRUE == exmdb_server_check_private()) {
		if (FALSE == common_util_get_id_from_username(
		    account, &tmp_id)) {
			return FALSE;
		}
	} else {
		if (FALSE == common_util_get_domain_ids(
		    account, &tmp_id, &tmp_id1)) {
			return FALSE;
		}
	}
	uint64_t dst_mid = 0;
	uint32_t message_size = 0;
	BOOL b_result = 0;
	if (FALSE == common_util_copy_message(
	    psqlite, tmp_id, message_id, dst_fid,
	    &dst_mid, &b_result, &message_size)) {
		return FALSE;
	}
	if (FALSE == b_result) {
		return true;
	}
	auto nt_time = rop_util_current_nttime();
	TAGGED_PROPVAL propval;
	propval.proptag = PROP_TAG_LOCALCOMMITTIMEMAX;
	propval.pvalue = &nt_time;
	common_util_set_property(FOLDER_PROPERTIES_TABLE,
		dst_fid, 0, psqlite, &propval, &b_result);
	if (FALSE == common_util_increase_store_size(
	    psqlite, message_size, 0)) {
		return FALSE;
	}
	pnode1 = cu_alloc<DOUBLE_LIST_NODE>();
	if (NULL == pnode1) {
		return FALSE;
	}
	pnode1->pdata = cu_alloc<uint64_t>();
	if (NULL == pnode1->pdata) {
		return FALSE;
	}
	*(uint64_t *)pnode1->pdata = dst_fid;
	double_list_append_as_tail(pfolder_list, pnode1);
	char tmp_buff[MAX_DIGLEN];
	char *pmid_string = nullptr, *pdigest1 = nullptr;
	if (NULL != pdigest && TRUE == common_util_get_mid_string(
	    psqlite, dst_mid, &pmid_string) && NULL != pmid_string) {
		strcpy(tmp_buff, pdigest);
		char mid_string[128];
		sprintf(mid_string, "\"%s\"", pmid_string);
		set_digest(tmp_buff, MAX_DIGLEN, "file", mid_string);
		pdigest1 = tmp_buff;
	} else {
		pdigest1 = NULL;
	}
	if (FALSE == message_rule_new_message(b_oof,
	    from_address, account, cpid, psqlite,
	    dst_fid, dst_mid, pdigest1, pfolder_list,
	    pmsg_list)) {
		return FALSE;
	}
	if (block.type == OP_MOVE) {
		b_del = TRUE;
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be moved to %llu in folder %llu by "
			"ext rule", account, LLU(message_id),
			LLU(folder_id), LLU(dst_mid), LLU(dst_fid));
	} else {
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be copied to %llu in folder %llu by "
			"ext rule", account, LLU(message_id),
			LLU(folder_id), LLU(dst_mid), LLU(dst_fid));
	}
	return true;
}

static bool opx_reply(const char *from_address, const char *account,
    sqlite3 *psqlite, uint64_t message_id, const EXT_ACTION_BLOCK &block,
    RULE_NODE *prnode)
{
	auto pextreply = static_cast<EXT_REPLY_ACTION *>(block.pdata);
	if (TRUE == exmdb_server_check_private()) {
		int tmp_id = 0;
		if (FALSE == common_util_get_id_from_username(
		    account, &tmp_id)) {
			return true;
		}
		auto tmp_guid = rop_util_make_user_guid(tmp_id);
		if (0 != guid_compare(&tmp_guid,
		    &pextreply->message_eid.message_database_guid)) {
			if (FALSE == message_disable_rule(
			    psqlite, TRUE, prnode->id)) {
				return FALSE;
			}
			return true;
		}
	} else {
		auto pc = strchr(account, '@');
		if (pc == nullptr)
			return true;
		++pc;
		int tmp_id = 0, tmp_id1 = 0;
		if (!common_util_get_domain_ids(pc, &tmp_id, &tmp_id1))
			return true;
		auto tmp_guid = rop_util_make_domain_guid(tmp_id);
		if (0 != guid_compare(&tmp_guid,
		    &pextreply->message_eid.message_database_guid)) {
			if (FALSE == message_disable_rule(
			    psqlite, TRUE, prnode->id)) {
				return FALSE;
			}
			return true;
		}
	}
	auto dst_mid = rop_util_gc_to_value(
		       pextreply->message_eid.message_global_counter);
	BOOL b_result = false;
	if (FALSE == message_auto_reply(
	    psqlite, message_id, from_address, account,
	    block.type, block.flavor,
	    dst_mid, pextreply->template_guid, &b_result))
		return FALSE;
	if (FALSE == b_result) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	return true;
}

static bool opx_delegate(const char *from_address, const char *account,
    uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id, uint64_t message_id,
    const char *pdigest, const EXT_ACTION_BLOCK &block, RULE_NODE *prnode)
{
	auto pextfwddlgt = static_cast<EXT_FORWARDDELEGATE_ACTION *>(block.pdata);
	if (FALSE == exmdb_server_check_private() ||
	    NULL == pdigest || 0 == pextfwddlgt->count) {
		return true;
	}
	if (pextfwddlgt->count > MAX_RULE_RECIPIENTS) {
		if (FALSE == message_disable_rule(
		    psqlite, TRUE, prnode->id)) {
			return FALSE;
		}
		return true;
	}
	MESSAGE_CONTENT *pmsgctnt = nullptr;
	if (FALSE == message_read_message(psqlite, cpid,
	    message_id, &pmsgctnt) || NULL == pmsgctnt) {
		return FALSE;
	}
	if (NULL != common_util_get_propvals(
	    &pmsgctnt->proplist, PROP_TAG_DELEGATEDBYRULE)) {
		common_util_log_info(LV_DEBUG, "user=%s host=unkonwn  Delegated"
			" message %llu in folder %llu cannot be delegated"
			" again", account, LLU(message_id), LLU(folder_id));
		return true;
	}
	static constexpr uint32_t tags[] = {
		PR_DISPLAY_TO, PR_DISPLAY_TO_A,
		PR_DISPLAY_CC, PR_DISPLAY_CC_A,
		PR_DISPLAY_BCC, PR_DISPLAY_BCC_A,
		PROP_TAG_MID, PR_MESSAGE_SIZE,
		PR_ASSOCIATED, PROP_TAG_CHANGENUMBER,
		PR_CHANGE_KEY, PR_READ,
		PROP_TAG_HASATTACHMENTS,
		PR_PREDECESSOR_CHANGE_LIST,
		PROP_TAG_MESSAGETOME, PROP_TAG_MESSAGECCME,
	};
	for (auto t : tags)
		common_util_remove_propvals(&pmsgctnt->proplist, t);
	if (NULL == common_util_get_propvals(&pmsgctnt->proplist,
	    PROP_TAG_RECEIVEDREPRESENTINGENTRYID)) {
		char essdn_buff[1280];
		memcpy(essdn_buff, "EX:", 3);
		if (!common_util_username_to_essdn(account,
		    essdn_buff + 3, GX_ARRAY_SIZE(essdn_buff) - 3))
			return FALSE;
		auto pvalue = common_util_username_to_addressbook_entryid(account);
		if (NULL == pvalue) {
			return FALSE;
		}
		TAGGED_PROPVAL propval;
		propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGENTRYID;
		propval.pvalue = pvalue;
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
		propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGADDRESSTYPE;
		propval.pvalue  = deconst("EX");
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
		propval.proptag =
			PROP_TAG_RECEIVEDREPRESENTINGEMAILADDRESS;
		propval.pvalue = essdn_buff + 3;
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
		char display_name[1024];
		if (TRUE == common_util_get_user_displayname(
		    account, display_name)) {
			propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGNAME;
			propval.pvalue = display_name;
			common_util_set_propvals(
				&pmsgctnt->proplist, &propval);
		}
		BINARY searchkey_bin;
		searchkey_bin.cb = strlen(essdn_buff) + 1;
		searchkey_bin.pv = essdn_buff;
		propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGSEARCHKEY;
		propval.pvalue = &searchkey_bin;
		common_util_set_propvals(&pmsgctnt->proplist, &propval);
	}
	static constexpr uint8_t fake_true = true;
	TAGGED_PROPVAL propval;
	propval.proptag = PROP_TAG_DELEGATEDBYRULE;
	propval.pvalue = deconst(&fake_true);
	common_util_set_propvals(&pmsgctnt->proplist, &propval);
	DOUBLE_LIST rcpt_list;
	if (FALSE == message_ext_recipient_blocks_to_list(
	    pextfwddlgt->count, pextfwddlgt->pblock, &rcpt_list)) {
		return FALSE;
	}
	char mid_string1[128], tmp_path1[256];
	get_digest(pdigest, "file", mid_string1, arsizeof(mid_string1));
	snprintf(tmp_path1, arsizeof(tmp_path1), "%s/eml/%s",
		exmdb_server_get_dir(), mid_string1);
	for (auto pnode1 = double_list_get_head(&rcpt_list);
	     NULL != pnode1; pnode1 = double_list_get_after(
	     &rcpt_list, pnode1)) {
		char maildir[256];
		if (!common_util_get_maildir(static_cast<char *>(pnode1->pdata), maildir))
			continue;
		auto mid_string = std::to_string(time(nullptr)) + "." +
				  std::to_string(common_util_sequence_ID()) + "." +
				  get_host_ID();
		auto eml_path = maildir + "/eml/"s + mid_string;
		if (!common_util_copy_file(tmp_path1, eml_path.c_str()))
			continue;
		char tmp_buff[MAX_DIGLEN];
		strcpy(tmp_buff, pdigest);
		auto mid_string1 = "\"" + mid_string + "\"";
		set_digest(tmp_buff, MAX_DIGLEN, "file", mid_string1.c_str());
		const char *pdigest1 = tmp_buff;
		uint32_t result = 0;
		if (!exmdb_client_relay_delivery(maildir,
		    from_address, static_cast<char *>(pnode1->pdata),
		    cpid, pmsgctnt, pdigest1, &result))
			return FALSE;
	}
	return true;
}

static bool opx_switcheroo(BOOL b_oof, const char *from_address,
    const char *account, uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id,
    uint64_t message_id, const char *pdigest, DOUBLE_LIST *pfolder_list,
    DOUBLE_LIST *pmsg_list, const EXT_ACTION_BLOCK &block,
    size_t rule_idx, RULE_NODE *prnode, BOOL &b_del)
{
	static constexpr uint8_t fake_true = 1;
	switch (block.type) {
	case OP_MOVE:
	case OP_COPY:
		if (!opx_move(b_oof, from_address, account, cpid, psqlite,
		    folder_id, message_id, pdigest, pfolder_list, pmsg_list,
		    block, prnode, b_del))
			return false;
		break;
	case OP_REPLY:
	case OP_OOF_REPLY:
		if (!opx_reply(from_address, account, psqlite, message_id,
		    block, prnode))
			return false;
		break;
	case OP_DEFER_ACTION:
		break;
	case OP_BOUNCE:
		if (FALSE == message_bounce_message(
		    from_address, account, psqlite, message_id,
		    *static_cast<uint32_t *>(block.pdata)))
			return FALSE;
		b_del = TRUE;
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be deleted by ext rule", account,
			LLU(message_id), LLU(folder_id));
		break;
	case OP_FORWARD: {
		auto pextfwddlgt = static_cast<EXT_FORWARDDELEGATE_ACTION *>(block.pdata);
		if (pextfwddlgt->count > MAX_RULE_RECIPIENTS) {
			if (FALSE == message_disable_rule(
			    psqlite, TRUE, prnode->id)) {
				return FALSE;
			}
			return true;
		}
		if (FALSE == message_forward_message(from_address,
		    account, psqlite, cpid, message_id, pdigest,
		    block.flavor, TRUE,
		    pextfwddlgt->count, pextfwddlgt->pblock))
			return FALSE;
		break;
	}
	case OP_DELEGATE:
		if (!opx_delegate(from_address, account, cpid, psqlite,
		    folder_id, message_id, pdigest, block, prnode))
			return false;
		break;
	case OP_TAG: {
		BOOL b_result = false;
		if (!common_util_set_property(MESSAGE_PROPERTIES_TABLE,
		    message_id, cpid, psqlite,
		    static_cast<TAGGED_PROPVAL *>(block.pdata),
		    &b_result))
			return FALSE;
		break;
	}
	case OP_DELETE:
		b_del = TRUE;
		common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
			"Message %llu in folder %llu is going"
			" to be deleted by ext rule", account,
			LLU(message_id), LLU(folder_id));
		break;
	case OP_MARK_AS_READ: {
		if (FALSE == exmdb_server_check_private()) {
			return true;
		}
		TAGGED_PROPVAL propval;
		propval.proptag = PR_READ;
		propval.pvalue = deconst(&fake_true);
		BOOL b_result = false;
		if (FALSE == common_util_set_property(
		    MESSAGE_PROPERTIES_TABLE, message_id,
		    0, psqlite, &propval, &b_result)) {
			return FALSE;
		}
		break;
	}
	}
	return true;
}

static bool opx_process(BOOL b_oof, const char *from_address,
    const char *account, uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id,
    uint64_t message_id, const char *pdigest, DOUBLE_LIST *pfolder_list,
    DOUBLE_LIST *pmsg_list, DOUBLE_LIST_NODE *pnode, BOOL &b_del, BOOL &b_exit)
{
	auto prnode = static_cast<RULE_NODE *>(pnode->pdata);
	if (TRUE == b_exit && 0 == (prnode->state
	    & RULE_STATE_ONLY_WHEN_OOF)) {
		return true;
	}
	void *pvalue = nullptr;
	if (FALSE == common_util_get_property(
	    MESSAGE_PROPERTIES_TABLE, prnode->id, 0, psqlite,
	    PROP_TAG_EXTENDEDRULEMESSAGECONDITION, &pvalue)) {
		return FALSE;
	}
	auto bv = static_cast<BINARY *>(pvalue);
	if (pvalue == nullptr || bv->cb == 0)
		return true;
	EXT_PULL ext_pull;
	ext_pull.init(bv->pb, bv->cb, common_util_alloc,
		EXT_FLAG_WCOUNT | EXT_FLAG_UTF16);
	NAMEDPROPERTY_INFOMATION propname_info;
	RESTRICTION restriction;
	if (ext_pull.g_namedprop_info(&propname_info) != EXT_ERR_SUCCESS ||
	    ext_pull.g_restriction(&restriction) != EXT_ERR_SUCCESS)
		return true;
	if (FALSE == message_replace_restriction_propid(
	    psqlite, &propname_info, &restriction)) {
		return FALSE;
	}
	if (FALSE == common_util_evaluate_message_restriction(
	    psqlite, 0, message_id, &restriction)) {
		return true;
	}
	if (prnode->state & RULE_STATE_EXIT_LEVEL) {
		b_exit = TRUE;
	}
	if (FALSE == common_util_get_property(
	    MESSAGE_PROPERTIES_TABLE, prnode->id, 0, psqlite,
	    PROP_TAG_EXTENDEDRULEMESSAGEACTIONS, &pvalue)) {
		return FALSE;
	}
	if (NULL == pvalue) {
		return true;
	}
	ext_pull.init(bv->pb, bv->cb, common_util_alloc,
		EXT_FLAG_WCOUNT | EXT_FLAG_UTF16);
	EXT_RULE_ACTIONS ext_actions;
	uint32_t version = 0;
	if (ext_pull.g_namedprop_info(&propname_info) != EXT_ERR_SUCCESS ||
	    ext_pull.g_uint32(&version) != EXT_ERR_SUCCESS ||
	    version != 1 ||
	    ext_pull.g_ext_rule_actions(&ext_actions) != EXT_ERR_SUCCESS)
		return true;
	if (FALSE == message_replace_actions_propid(
	    psqlite, &propname_info, &ext_actions)) {
		return FALSE;
	}
	for (size_t i = 0; i < ext_actions.count; ++i)
		if (!opx_switcheroo(b_oof, from_address, account, cpid, psqlite,
		    folder_id, message_id, pdigest, pfolder_list,
		    pmsg_list, ext_actions.pblock[i], i, prnode, b_del))
			return false;
	return true;
}
/* extended rules do not produce DAM or DEM */
static BOOL message_rule_new_message(BOOL b_oof,
	const char *from_address, const char *account,
	uint32_t cpid, sqlite3 *psqlite, uint64_t folder_id,
	uint64_t message_id, const char *pdigest,
	DOUBLE_LIST *pfolder_list, DOUBLE_LIST *pmsg_list)
{
	DOUBLE_LIST dam_list, rule_list, ext_rule_list;
	
	double_list_init(&dam_list);
	double_list_init(&rule_list);
	double_list_init(&ext_rule_list);
	if (FALSE == message_load_folder_rules(
		b_oof, psqlite, folder_id, &rule_list) ||
		FALSE == message_load_folder_ext_rules(
		b_oof, psqlite, folder_id, &ext_rule_list)) {
		return FALSE;
	}
	BOOL b_del = false, b_exit = false;
	for (auto pnode = double_list_get_head(&rule_list); pnode != nullptr;
		pnode=double_list_get_after(&rule_list, pnode)) {
		auto prnode = static_cast<RULE_NODE *>(pnode->pdata);
		if (TRUE == b_exit && 0 == (prnode->state
			& RULE_STATE_ONLY_WHEN_OOF)) {
			continue;
		}
		void *pvalue = nullptr;
		if (!common_util_get_rule_property(prnode->id, psqlite,
		    PR_RULE_CONDITION, &pvalue))
			return FALSE;
		if (pvalue == nullptr || !common_util_evaluate_message_restriction(psqlite,
		    0, message_id, static_cast<RESTRICTION *>(pvalue)))
			continue;
		if (prnode->state & RULE_STATE_EXIT_LEVEL) {
			b_exit = TRUE;
		}
		RULE_ACTIONS *pactions = nullptr;
		if (!common_util_get_rule_property(prnode->id, psqlite,
		    PR_RULE_ACTIONS, reinterpret_cast<void **>(&pactions)))
			return FALSE;
		if (NULL == pactions) {
			continue;
		}
		for (size_t i = 0; i < pactions->count; ++i)
			if (!op_switcheroo(b_oof, from_address, account, cpid, psqlite,
			    folder_id, message_id, pdigest, pfolder_list,
			    pmsg_list, pactions->pblock[i], i, prnode, b_del, dam_list))
				return false;
	}
	if (double_list_get_nodes_num(&dam_list) > 0) {
		if (FALSE == message_make_deferred_action_messages(
			account, psqlite, folder_id, message_id,
			&dam_list, pmsg_list)) {
			return FALSE;
		}
	}
	for (auto pnode = double_list_get_head(&ext_rule_list); pnode != nullptr;
	     pnode = double_list_get_after(&ext_rule_list, pnode))
		if (!opx_process(b_oof, from_address, account, cpid, psqlite,
		    folder_id, message_id, pdigest, pfolder_list, pmsg_list,
		    pnode, b_del, b_exit))
			return false;

	if (TRUE == b_del) {
		void *pvalue = nullptr;
		if (!common_util_get_property(MESSAGE_PROPERTIES_TABLE,
		    message_id, 0, psqlite, PR_MESSAGE_SIZE, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		auto message_size = *static_cast<uint32_t *>(pvalue);
		char sql_string[128];
		snprintf(sql_string, arsizeof(sql_string), "DELETE FROM messages"
		        " WHERE message_id=%llu", LLU(message_id));
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			return FALSE;
		}
		if (FALSE == common_util_decrease_store_size(
			psqlite, message_size, 0)) {
			return FALSE;
		}
		if (NULL != pdigest) {
			char mid_string1[128], tmp_path1[256];
			get_digest(pdigest, "file", mid_string1, arsizeof(mid_string1));
			snprintf(tmp_path1, arsizeof(tmp_path1), "%s/eml/%s",
				exmdb_server_get_dir(), mid_string1);
			remove(tmp_path1);
		}
	} else {
		auto pmnode = cu_alloc<MESSAGE_NODE>();
		if (NULL == pmnode) {
			return FALSE;
		}
		pmnode->node.pdata = pmnode;
		pmnode->folder_id = folder_id;
		pmnode->message_id = message_id;
		double_list_append_as_tail(pmsg_list, &pmnode->node);
	}
	return TRUE;
}

/* 0 means success, 1 means mailbox full, other unknown error */
BOOL exmdb_server_delivery_message(const char *dir,
	const char *from_address, const char *account,
	uint32_t cpid, const MESSAGE_CONTENT *pmsg,
	const char *pdigest, uint32_t *presult)
{
	int fd;
	BOOL b_oof;
	BOOL b_to_me;
	BOOL b_cc_me;
	void *pvalue;
	uint64_t nt_time;
	uint64_t fid_val;
	BINARY *pentryid;
	char tmp_path[256];
	uint64_t message_id;
	BINARY searchkey_bin;
	DOUBLE_LIST msg_list;
	MESSAGE_NODE *pmnode;
	const char *paccount;
	char mid_string[128];
	char essdn_buff[1280];
	TAGGED_PROPVAL propval;
	char display_name[1024];
	DOUBLE_LIST folder_list;
	DOUBLE_LIST_NODE *pnode;
	MESSAGE_CONTENT tmp_msg;
	char digest_buff[MAX_DIGLEN];
	static const uint8_t fake_true = 1;
	
	if (NULL != pdigest && strlen(pdigest) >= MAX_DIGLEN) {
		return FALSE;
	}
	b_to_me = FALSE;
	b_cc_me = FALSE;
	if (NULL != pmsg->children.prcpts) {
		for (size_t i = 0; i < pmsg->children.prcpts->count; ++i) {
			pvalue = common_util_get_propvals(
				pmsg->children.prcpts->pparray[i],
				PROP_TAG_RECIPIENTTYPE);
			if (NULL == pvalue) {
				continue;
			}
			switch (*(uint32_t*)pvalue) {
			case RECIPIENT_TYPE_TO:
				pvalue = common_util_get_propvals(pmsg->children.prcpts->pparray[i], PR_SMTP_ADDRESS);
				if (pvalue != nullptr && strcasecmp(account, static_cast<char *>(pvalue)) == 0)
					b_to_me = TRUE;	
				break;
			case RECIPIENT_TYPE_CC:
				pvalue = common_util_get_propvals(pmsg->children.prcpts->pparray[i], PR_SMTP_ADDRESS);
				if (pvalue != nullptr && strcasecmp(account, static_cast<char *>(pvalue)) == 0)
					b_cc_me = TRUE;	
				break;
			}
			if (TRUE == b_to_me || TRUE == b_cc_me) {
				break;
			}
		}
	}
	if (TRUE == exmdb_server_check_private()) {
		paccount = account;
	} else {
		paccount = strchr(account, '@');
		if (NULL == paccount) {
			return FALSE;
		}
		paccount ++;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (cu_check_msgsize_overflow(pdb->psqlite, PROP_TAG_PROHIBITRECEIVEQUOTA) ||
		TRUE == common_util_check_msgcnt_overflow(pdb->psqlite)) {
		*presult = 1;
		return TRUE;
	}
	if (TRUE == exmdb_server_check_private()) {
		if (FALSE == common_util_get_property(STORE_PROPERTIES_TABLE,
			0, 0, pdb->psqlite, PROP_TAG_OUTOFOFFICESTATE, &pvalue)) {
			return FALSE;
		}
		b_oof = pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0 ? false : TRUE;
		fid_val = PRIVATE_FID_INBOX;
	} else {
		b_oof = FALSE;
		//TODO get public folder id
		fprintf(stderr, "%s - public folder not implemented\n", __func__);
		return false;
	}
	double_list_init(&msg_list);
	double_list_init(&folder_list);
	pnode = cu_alloc<DOUBLE_LIST_NODE>();
	if (NULL == pnode) {
		return FALSE;
	}
	pnode->pdata = cu_alloc<uint64_t>();
	if (NULL == pnode->pdata) {
		return FALSE;
	}
	*(uint64_t*)pnode->pdata = fid_val;
	double_list_append_as_tail(&folder_list, pnode);
	tmp_msg = *pmsg;
	if (TRUE == exmdb_server_check_private()) {
		tmp_msg.proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pmsg->proplist.count + 15);
		if (NULL == tmp_msg.proplist.ppropval) {
			return FALSE;
		}
		memcpy(tmp_msg.proplist.ppropval, pmsg->proplist.ppropval,
					sizeof(TAGGED_PROPVAL)*pmsg->proplist.count);
		pentryid = common_util_username_to_addressbook_entryid(account);
		if (NULL == pentryid) {
			return FALSE;	
		}
		memcpy(essdn_buff, "EX:", 3);
		if (!common_util_username_to_essdn(account,
		    essdn_buff + 3, GX_ARRAY_SIZE(essdn_buff) - 3))
			return FALSE;
		HX_strupper(essdn_buff);
		propval.proptag = PROP_TAG_RECEIVEDBYENTRYID;
		propval.pvalue = pentryid;
		common_util_set_propvals(&tmp_msg.proplist, &propval);
		propval.proptag = PROP_TAG_RECEIVEDBYADDRESSTYPE;
		propval.pvalue  = deconst("EX");
		common_util_set_propvals(&tmp_msg.proplist, &propval);
		propval.proptag = PROP_TAG_RECEIVEDBYEMAILADDRESS;
		propval.pvalue = essdn_buff + 3;
		common_util_set_propvals(&tmp_msg.proplist, &propval);
		if (TRUE == common_util_get_user_displayname(
			account, display_name)) {
			propval.proptag = PROP_TAG_RECEIVEDBYNAME;
			propval.pvalue = display_name;
			common_util_set_propvals(&tmp_msg.proplist, &propval);
		} else {
			display_name[0] = '\0';
		}
		searchkey_bin.cb = strlen(essdn_buff) + 1;
		searchkey_bin.pv = essdn_buff;
		propval.proptag = PROP_TAG_RECEIVEDBYSEARCHKEY;
		propval.pvalue = &searchkey_bin;
		common_util_set_propvals(&tmp_msg.proplist, &propval);
		if (NULL == common_util_get_propvals(&pmsg->proplist,
			PROP_TAG_RECEIVEDREPRESENTINGENTRYID)) {
			propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGENTRYID;
			propval.pvalue = pentryid;
			common_util_set_propvals(&tmp_msg.proplist, &propval);	
			propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGADDRESSTYPE;
			propval.pvalue  = deconst("EX");
			common_util_set_propvals(&tmp_msg.proplist, &propval);
			propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGEMAILADDRESS;
			propval.pvalue = essdn_buff + 3;
			common_util_set_propvals(&tmp_msg.proplist, &propval);
			if ('\0' != display_name[0]) {
				propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGNAME;
				propval.pvalue = display_name;
				common_util_set_propvals(&tmp_msg.proplist, &propval);
			}
			propval.proptag = PROP_TAG_RECEIVEDREPRESENTINGSEARCHKEY;
			propval.pvalue = &searchkey_bin;
			common_util_set_propvals(&tmp_msg.proplist, &propval);
		}
		if (TRUE == b_to_me) {
			propval.proptag = PROP_TAG_MESSAGETOME;
			propval.pvalue = deconst(&fake_true);
			common_util_set_propvals(&tmp_msg.proplist, &propval);
		} else if (TRUE == b_cc_me) {
			propval.proptag = PROP_TAG_MESSAGECCME;
			propval.pvalue = deconst(&fake_true);
			common_util_set_propvals(&tmp_msg.proplist, &propval);
		}
	}
	nt_time = rop_util_current_nttime();
	pvalue = common_util_get_propvals(&tmp_msg.proplist,
							PROP_TAG_MESSAGEDELIVERYTIME);
	if (NULL != pvalue) {
		*(uint64_t*)pvalue = nt_time;
	}
	pvalue = common_util_get_propvals(&tmp_msg.proplist, PR_LAST_MODIFICATION_TIME);
	if (NULL != pvalue) {
		*(uint64_t*)pvalue = nt_time;
	}
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == message_write_message(FALSE, pdb->psqlite,
		paccount, cpid, FALSE, fid_val, &tmp_msg, &message_id)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (0 == message_id) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		*presult = 2;
		return TRUE;
	}
	if (pdigest != nullptr &&
	    get_digest(pdigest, "file", mid_string, arsizeof(mid_string))) {
		strcpy(digest_buff, pdigest);
		set_digest(digest_buff, MAX_DIGLEN, "file", "\"\"");
		snprintf(tmp_path, arsizeof(tmp_path), "%s/ext/%s", exmdb_server_get_dir(), mid_string);
		fd = open(tmp_path, O_CREAT|O_TRUNC|O_WRONLY, 0666);
		if (-1 != fd) {
			write(fd, digest_buff, strlen(digest_buff));
			close(fd);
			if (FALSE == common_util_set_mid_string(
				pdb->psqlite, message_id, mid_string)) {
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
		}
	}
	common_util_log_info(LV_DEBUG, "user=%s host=unknown  "
		"Message %llu is delivered into folder "
		"%llu", account, LLU(message_id), LLU(fid_val));
	if (FALSE == message_rule_new_message(b_oof,
		from_address, account, cpid, pdb->psqlite,
		fid_val, message_id, pdigest, &folder_list, &msg_list)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION",  NULL, NULL, NULL);
	for (pnode=double_list_get_head(&msg_list); NULL!=pnode;
		pnode=double_list_get_after(&msg_list, pnode)) {
		pmnode = (MESSAGE_NODE*)pnode->pdata;
		db_engine_proc_dynamic_event(
			pdb, cpid, DYNAMIC_EVENT_NEW_MESSAGE,
			pmnode->folder_id, pmnode->message_id, 0);
		if (message_id == pmnode->message_id) {
			db_engine_notify_new_mail(pdb, 
				pmnode->folder_id, pmnode->message_id);
		} else {
			db_engine_notify_message_creation(pdb,
				pmnode->folder_id, pmnode->message_id);
		}
	}
	*presult = 0;
	return TRUE;
}

/* create or cover message under folder, if message exists
	in somewhere except the folder, result will be FALSE */
BOOL exmdb_server_write_message(const char *dir, const char *account,
    uint32_t cpid, uint64_t folder_id, const MESSAGE_CONTENT *pmsgctnt,
    gxerr_t *pe_result)
{
	BOOL b_exist;
	void *pvalue;
	uint64_t *pmid;
	uint64_t nt_time;
	uint64_t mid_val;
	uint64_t fid_val;
	uint64_t fid_val1;
	
	if (NULL == common_util_get_propvals(
		&pmsgctnt->proplist, PROP_TAG_CHANGENUMBER)) {
		*pe_result = GXERR_CALL_FAILED;
		return TRUE;
	}
	b_exist = FALSE;
	pmid = static_cast<uint64_t *>(common_util_get_propvals(
	       &pmsgctnt->proplist, PROP_TAG_MID));
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (cu_check_msgsize_overflow(pdb->psqlite, PROP_TAG_STORAGEQUOTALIMIT) ||
		TRUE == common_util_check_msgcnt_overflow(pdb->psqlite)) {
		*pe_result = GXERR_OVER_QUOTA;
		return TRUE;	
	}
	fid_val = rop_util_get_gc_value(folder_id);
	if (NULL != pmid) {
		if (FALSE == common_util_get_message_parent_folder(
			pdb->psqlite, rop_util_get_gc_value(*pmid), &fid_val1)) {
			return FALSE;	
		}
		if (0 != fid_val1) {
			b_exist = TRUE;
			if (fid_val != fid_val1) {
				*pe_result = GXERR_CALL_FAILED;
				return TRUE;
			}
		}
	}
	nt_time = rop_util_current_nttime();
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_LAST_MODIFICATION_TIME);
	if (NULL != pvalue) {
		*(uint64_t*)pvalue = nt_time;
	}
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == message_write_message(FALSE, pdb->psqlite,
		account, cpid, FALSE, fid_val, pmsgctnt, &mid_val)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (0 == mid_val) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		*pe_result = GXERR_CALL_FAILED;
	} else {
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		*pe_result = GXERR_SUCCESS;
	}
	if (TRUE == b_exist) {
		db_engine_proc_dynamic_event(pdb, cpid,
			DYNAMIC_EVENT_MODIFY_MESSAGE, fid_val, mid_val, 0);
		db_engine_notify_message_modification(
			pdb, fid_val, mid_val);
	} else {
		db_engine_proc_dynamic_event(pdb, cpid,
			DYNAMIC_EVENT_NEW_MESSAGE, fid_val, mid_val, 0);
		db_engine_notify_message_creation(
			pdb, fid_val, mid_val);
	}
	return TRUE;
}

BOOL exmdb_server_read_message(const char *dir, const char *username,
	uint32_t cpid, uint64_t message_id, MESSAGE_CONTENT **ppmsgctnt)
{
	uint64_t mid_val;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	mid_val = rop_util_get_gc_value(message_id);
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == common_util_begin_message_optimize(pdb->psqlite)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (FALSE == message_read_message(
		pdb->psqlite, cpid, mid_val, ppmsgctnt)) {
		common_util_end_message_optimize();
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	common_util_end_message_optimize();
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	return TRUE;
}

BOOL exmdb_server_rule_new_message(const char *dir,
	const char *username, const char *account, uint32_t cpid,
	uint64_t folder_id, uint64_t message_id)
{
	int fd, len;
	char *pdigest;
	uint64_t fid_val;
	uint64_t mid_val;
	char *pmid_string;
	char tmp_path[256];
	DOUBLE_LIST msg_list;
	MESSAGE_NODE *pmnode;
	DOUBLE_LIST folder_list;
	DOUBLE_LIST_NODE *pnode;
	char digest_buff[MAX_DIGLEN];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	fid_val = rop_util_get_gc_value(folder_id);
	mid_val = rop_util_get_gc_value(message_id);
	pdigest = NULL;
	if (FALSE == common_util_get_mid_string(
		pdb->psqlite, mid_val, &pmid_string)) {
		return FALSE;
	}
	if (NULL != pmid_string) {
		snprintf(tmp_path, arsizeof(tmp_path), "%s/ext/%s",
			exmdb_server_get_dir(), pmid_string);
		fd = open(tmp_path, O_RDONLY);
		if (-1 != fd) {
			len = read(fd, digest_buff, MAX_DIGLEN);
			if (len > 0) {
				digest_buff[len] = '\0';
				pdigest = digest_buff;
			}
			close(fd);
		}
	}
	double_list_init(&msg_list);
	double_list_init(&folder_list);
	pnode = cu_alloc<DOUBLE_LIST_NODE>();
	if (NULL == pnode) {
		return FALSE;
	}
	pnode->pdata = cu_alloc<uint64_t>();
	if (NULL == pnode->pdata) {
		return FALSE;
	}
	*(uint64_t*)pnode->pdata = fid_val;
	double_list_append_as_tail(&folder_list, pnode);
	sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (FALSE == message_rule_new_message(FALSE, "none@none",
		account, cpid, pdb->psqlite, fid_val, mid_val,
		pdigest, &folder_list, &msg_list)) {
		sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION",  NULL, NULL, NULL);
	for (pnode=double_list_get_head(&msg_list); NULL!=pnode;
		pnode=double_list_get_after(&msg_list, pnode)) {
		pmnode = (MESSAGE_NODE*)pnode->pdata;
		if (mid_val == pmnode->message_id) {
			continue;
		}
		db_engine_proc_dynamic_event(
			pdb, cpid, DYNAMIC_EVENT_NEW_MESSAGE,
			pmnode->folder_id, pmnode->message_id, 0);
		db_engine_notify_message_creation(pdb,
			pmnode->folder_id, pmnode->message_id);
	}
	return TRUE;
}
