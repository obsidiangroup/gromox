// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/util.hpp>
#include <gromox/guid.hpp>
#include <gromox/mapidefs.h>
#include <gromox/proptags.hpp>
#include "ab_tree.h"
#include <gromox/ndr_stack.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <pthread.h> 
#include <sys/stat.h>
#include <sys/types.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <gromox/hmacmd5.hpp>
#include "../mysql_adaptor/mysql_adaptor.h"
#include "common_util.h"
#include "nsp_types.h"

#define BASE_STATUS_CONSTRUCTING			0
#define BASE_STATUS_LIVING					1
#define BASE_STATUS_DESTRUCTING				2

#define MINID_TYPE_ADDRESS					0x0
#define MINID_TYPE_DOMAIN					0x4
#define MINID_TYPE_GROUP					0x5
#define MINID_TYPE_CLASS					0x6

/* 0x00 ~ 0x10 minid reserved by nspi */
#define MINID_TYPE_RESERVED					7

#define HGROWING_SIZE						100

/* 
	PERSON: username, real_name, title, memo, cell, tel,
			nickname, homeaddress, create_day, maildir
	ROOM: title
	EQUIPMENT: title
	MLIST: listname, list_type(int), list_privilege(int)
	DOMAIN: domainname, title, address
	GROUP: groupname, title
	CLASS: classname
*/

using namespace gromox;

namespace {

struct AB_NODE {
	SIMPLE_TREE_NODE node;
	uint8_t node_type;
	uint32_t minid;
	void *d_info;
	int id;
};

struct ab_sort_item {
	SIMPLE_TREE_NODE *pnode;
	char *string;
};

}

static size_t g_base_size;
static int g_file_blocks, g_ab_cache_interval;
static std::atomic<bool> g_notify_stop{false};
static pthread_t g_scan_id;
static char g_nsp_org_name[256];
static std::unordered_map<int, AB_BASE> g_base_hash;
static std::mutex g_base_lock, g_remote_lock;
static LIB_BUFFER *g_file_allocator;

static decltype(mysql_adaptor_get_org_domains) *get_org_domains;
static decltype(mysql_adaptor_get_domain_info) *get_domain_info;
static decltype(mysql_adaptor_get_domain_groups) *get_domain_groups;
static decltype(mysql_adaptor_get_group_classes) *get_group_classes;
static decltype(mysql_adaptor_get_sub_classes) *get_sub_classes;
static decltype(mysql_adaptor_get_class_users) *get_class_users;
static decltype(mysql_adaptor_get_group_users) *get_group_users;
static decltype(mysql_adaptor_get_domain_users) *get_domain_users;
static decltype(mysql_adaptor_get_mlist_ids) *get_mlist_ids;

static BOOL (*get_lang)(uint32_t codepage,
	const char *tag, char *value, int len);

static void *nspab_scanwork(void *);

static uint32_t ab_tree_make_minid(uint8_t type, int value)
{
	uint32_t minid;
	
	if (MINID_TYPE_ADDRESS == type && value <= 0x10) {
		type = MINID_TYPE_RESERVED;
	}
	minid = type;
	minid <<= 29;
	minid |= value;
	return minid;
}

static int ab_tree_get_minid_value(uint32_t minid)
{
	if (0 == (minid & 0x80000000)) {
		return minid;
	}
	return minid & 0x1FFFFFFF;
}

uint32_t ab_tree_get_leaves_num(SIMPLE_TREE_NODE *pnode)
{
	uint32_t count;
	
	pnode = simple_tree_node_get_child(pnode);
	if (NULL == pnode) {
		return 0;
	}
	count = 0;
	do {
		if (ab_tree_get_node_type(pnode) < 0x80) {
			count ++;
		}
	} while ((pnode = simple_tree_node_get_sibling(pnode)) != nullptr);
	return count;
}

static SINGLE_LIST_NODE* ab_tree_get_snode()
{
	return new(std::nothrow) SINGLE_LIST_NODE;
}

static void ab_tree_put_snode(SINGLE_LIST_NODE *psnode)
{
	delete psnode;
}

static AB_NODE* ab_tree_get_abnode()
{
	auto n = new(std::nothrow) AB_NODE;
	if (n == nullptr)
		return nullptr;
	n->d_info = nullptr;
	n->minid = 0;
	return n;
}

static void ab_tree_put_abnode(AB_NODE *pabnode)
{
	switch (pabnode->node_type) {
	case NODE_TYPE_DOMAIN:
		delete static_cast<sql_domain *>(pabnode->d_info);
		break;
	case NODE_TYPE_PERSON:
	case NODE_TYPE_ROOM:
	case NODE_TYPE_EQUIPMENT:
	case NODE_TYPE_MLIST:
		delete static_cast<sql_user *>(pabnode->d_info);
		break;
	case NODE_TYPE_GROUP:
		delete static_cast<sql_group *>(pabnode->d_info);
		break;
	case NODE_TYPE_CLASS:
		delete static_cast<sql_class *>(pabnode->d_info);
		break;
	}
	delete pabnode;
}

SIMPLE_TREE_NODE* ab_tree_minid_to_node(AB_BASE *pbase, uint32_t minid)
{
	SINGLE_LIST_NODE *psnode;
	auto ppnode = static_cast<SIMPLE_TREE_NODE **>(int_hash_query(pbase->phash, minid));
	if (NULL != ppnode) {
		return *ppnode;
	}
	std::lock_guard rhold(g_remote_lock);
	for (psnode=single_list_get_head(&pbase->remote_list); NULL!=psnode;
		psnode=single_list_get_after(&pbase->remote_list, psnode)) {
		if (minid == ((AB_NODE*)psnode->pdata)->minid) {
			return static_cast<SIMPLE_TREE_NODE *>(psnode->pdata);
		}
	}
	return NULL;
}

void ab_tree_init(const char *org_name, size_t base_size,
	int cache_interval, int file_blocks)
{
	gx_strlcpy(g_nsp_org_name, org_name, arsizeof(g_nsp_org_name));
	g_base_size = base_size;
	g_ab_cache_interval = cache_interval;
	g_file_blocks = file_blocks;
	g_notify_stop = true;
}

int ab_tree_run()
{
#define E(f, s) do { \
	query_service2(s, f); \
	if ((f) == nullptr) { \
		printf("[%s]: failed to get the \"%s\" service\n", "exchange_nsp", (s)); \
		return -1; \
	} \
} while (false)

	E(get_org_domains, "get_org_domains");
	E(get_domain_info, "get_domain_info");
	E(get_domain_groups, "get_domain_groups");
	E(get_group_classes, "get_group_classes");
	E(get_sub_classes, "get_sub_classes");
	E(get_class_users, "get_class_users");
	E(get_group_users, "get_group_users");
	E(get_domain_users, "get_domain_users");
	E(get_mlist_ids, "get_mlist_ids");
	E(get_lang, "get_lang");
#undef E
	g_file_allocator = lib_buffer_init(
		FILE_ALLOC_SIZE, g_file_blocks, TRUE);
	if (NULL == g_file_allocator) {
		printf("[exchange_nsp]: Failed to allocate file blocks\n");
		return -3;
	}
	g_notify_stop = false;
	auto ret = pthread_create(&g_scan_id, nullptr, nspab_scanwork, nullptr);
	if (ret != 0) {
		printf("[exchange_nsp]: failed to create scanning thread: %s\n", strerror(ret));
		g_notify_stop = true;
		return -4;
	}
	pthread_setname_np(g_scan_id, "nsp_abtree_scan");
	return 0;
}

static void ab_tree_destruct_tree(SIMPLE_TREE *ptree)
{
	SIMPLE_TREE_NODE *proot;
	
	proot = simple_tree_get_root(ptree);
	if (NULL != proot) {
		simple_tree_destroy_node(ptree, proot,
			(SIMPLE_TREE_DELETE)ab_tree_put_abnode);
	}
	simple_tree_free(ptree);
}

void AB_BASE::unload()
{
	auto pbase = this;
	SINGLE_LIST_NODE *pnode;
	
	while ((pnode = single_list_pop_front(&pbase->list)) != nullptr) {
		ab_tree_destruct_tree(&((DOMAIN_NODE*)pnode->pdata)->tree);
		free(pnode->pdata);
	}
	while ((pnode = single_list_pop_front(&pbase->gal_list)) != nullptr)
		ab_tree_put_snode(pnode);
	while ((pnode = single_list_pop_front(&pbase->remote_list)) != nullptr) {
		ab_tree_put_abnode(static_cast<AB_NODE *>(pnode->pdata));
		ab_tree_put_snode(pnode);
	}
	if (NULL != pbase->phash) {
		int_hash_free(pbase->phash);
		pbase->phash = NULL;
	}
}

AB_BASE::AB_BASE()
{
	single_list_init(&list);
	single_list_init(&gal_list);
	single_list_init(&remote_list);
}

void ab_tree_stop()
{
	if (!g_notify_stop) {
		g_notify_stop = true;
		pthread_kill(g_scan_id, SIGALRM);
		pthread_join(g_scan_id, NULL);
	}
	g_base_hash.clear();
	if (NULL != g_file_allocator) {
		lib_buffer_free(g_file_allocator);
		g_file_allocator = NULL;
	}
}

static BOOL ab_tree_cache_node(AB_BASE *pbase, AB_NODE *pabnode)
{
	int tmp_id;
	void *ptmp_value;
	INT_HASH_ITER *iter;
	
	if (NULL == pbase->phash) {
		pbase->phash = int_hash_init(HGROWING_SIZE, sizeof(AB_NODE *));
		if (NULL == pbase->phash) {
			return FALSE;
		}
	}
	if (1 != int_hash_add(pbase->phash, pabnode->minid, &pabnode)) {
		INT_HASH_TABLE *phash = int_hash_init(pbase->phash->capacity +
		                        HGROWING_SIZE, sizeof(AB_NODE *));
		if (NULL == phash) {
			return FALSE;
		}
		iter = int_hash_iter_init(pbase->phash);
		for (int_hash_iter_begin(iter); !int_hash_iter_done(iter);
			int_hash_iter_forward(iter)) {
			ptmp_value = int_hash_iter_get_value(iter, &tmp_id);
			int_hash_add(phash, tmp_id, ptmp_value);
		}
		int_hash_iter_free(iter);
		int_hash_free(pbase->phash);
		pbase->phash = phash;
		int_hash_add(pbase->phash, pabnode->minid, &pabnode);
	}
	return TRUE;
}

static BOOL ab_tree_load_user(AB_NODE *pabnode,
    sql_user &&usr, AB_BASE *pbase)
{
	switch (usr.dtypx) {
	case DT_ROOM:
		pabnode->node_type = NODE_TYPE_ROOM;
		break;
	case DT_EQUIPMENT:
		pabnode->node_type = NODE_TYPE_EQUIPMENT;
		break;
	default:
		pabnode->node_type = NODE_TYPE_PERSON;
		break;
	}
	pabnode->id = usr.id;
	pabnode->minid = ab_tree_make_minid(MINID_TYPE_ADDRESS, usr.id);
	((SIMPLE_TREE_NODE*)pabnode)->pdata = int_hash_query(
							pbase->phash, pabnode->minid);
	if (NULL == ((SIMPLE_TREE_NODE*)pabnode)->pdata) {
		if (FALSE == ab_tree_cache_node(pbase, pabnode)) {
			return FALSE;
		}
	}
	pabnode->d_info = new(std::nothrow) sql_user(std::move(usr));
	if (pabnode->d_info == nullptr)
		return false;
	return TRUE;
}

static BOOL ab_tree_load_mlist(AB_NODE *pabnode,
    sql_user &&usr, AB_BASE *pbase)
{
	pabnode->node_type = NODE_TYPE_MLIST;
	pabnode->id = usr.id;
	pabnode->minid = ab_tree_make_minid(MINID_TYPE_ADDRESS, usr.id);
	((SIMPLE_TREE_NODE*)pabnode)->pdata = int_hash_query(
							pbase->phash, pabnode->minid);
	if (NULL == ((SIMPLE_TREE_NODE*)pabnode)->pdata) {
		if (FALSE == ab_tree_cache_node(pbase, pabnode)) {
			return FALSE;
		}
	}
	pabnode->d_info = new(std::nothrow) sql_user(std::move(usr));
	if (pabnode->d_info == nullptr)
		return false;
	return TRUE;
}

static int ab_tree_cmpstring(const void *p1, const void *p2)
{
	return strcasecmp(static_cast<const ab_sort_item *>(p1)->string,
	       static_cast<const ab_sort_item *>(p2)->string);
}

static BOOL ab_tree_load_class(
	int class_id, SIMPLE_TREE *ptree,
	SIMPLE_TREE_NODE *pnode, AB_BASE *pbase)
{
	int i;
	int rows;
	AB_NODE *pabnode;
	char temp_buff[1024];
	SIMPLE_TREE_NODE *pclass;
	
	std::vector<sql_class> file_subclass;
	if (!get_sub_classes(class_id, file_subclass))
		return FALSE;
	for (auto &&cls : file_subclass) {
		pabnode = ab_tree_get_abnode();
		if (NULL == pabnode) {
			return FALSE;
		}
		pabnode->node_type = NODE_TYPE_CLASS;
		pabnode->id = cls.child_id;
		pabnode->minid = ab_tree_make_minid(MINID_TYPE_CLASS, cls.child_id);
		if (NULL == int_hash_query(pbase->phash, pabnode->minid)) {
			if (FALSE == ab_tree_cache_node(pbase, pabnode)) {
				return FALSE;
			}
		}
		auto child_id = cls.child_id;
		pabnode->d_info = new(std::nothrow) sql_class(std::move(cls));
		if (pabnode->d_info == nullptr)
			return false;
		pclass = (SIMPLE_TREE_NODE*)pabnode;
		simple_tree_add_child(ptree, pnode,
			pclass, SIMPLE_TREE_ADD_LAST);
		if (!ab_tree_load_class(child_id, ptree, pclass, pbase))
			return FALSE;
	}

	std::vector<sql_user> file_user;
	rows = get_class_users(class_id, file_user);
	if (-1 == rows) {
		return FALSE;
	} else if (0 == rows) {
		return TRUE;
	}
	auto parray = static_cast<ab_sort_item *>(malloc(sizeof(ab_sort_item) * rows));
	if (NULL == parray) {
		return FALSE;
	}
	i = 0;
	for (auto &&usr : file_user) {
		pabnode = ab_tree_get_abnode();
		if (NULL == pabnode) {
			goto LOAD_FAIL;
		}
		if (usr.dtypx == DT_DISTLIST) {
			if (!ab_tree_load_mlist(pabnode, std::move(usr), pbase)) {
				ab_tree_put_abnode(pabnode);
				goto LOAD_FAIL;
			}
		} else {
			if (!ab_tree_load_user(pabnode, std::move(usr), pbase)) {
				ab_tree_put_abnode(pabnode);
				goto LOAD_FAIL;
			}
		}
		parray[i].pnode = (SIMPLE_TREE_NODE*)pabnode;
		ab_tree_get_display_name(parray[i].pnode, 1252, temp_buff, arsizeof(temp_buff));
		parray[i].string = strdup(temp_buff);
		if (NULL == parray[i].string) {
			ab_tree_put_abnode(pabnode);
			goto LOAD_FAIL;
		}
		i ++;
	}

	qsort(parray, rows, sizeof(*parray), ab_tree_cmpstring);
	for (i=0; i<rows; i++) {
		simple_tree_add_child(ptree, pnode,
			parray[i].pnode, SIMPLE_TREE_ADD_LAST);
		free(parray[i].string);
	}
	free(parray);
	return TRUE;
 LOAD_FAIL:
	for (i-=1; i>=0; i--) {
		free(parray[i].string);
		ab_tree_put_abnode((AB_NODE*)parray[i].pnode);
	}
	free(parray);
	return FALSE;
}

static BOOL ab_tree_load_tree(int domain_id,
	SIMPLE_TREE *ptree, AB_BASE *pbase)
{
	int i;
	int rows;
	AB_NODE *pabnode;
	ab_sort_item *parray = nullptr;
	sql_domain dinfo;
	SIMPLE_TREE_NODE *pgroup;
	SIMPLE_TREE_NODE *pclass;
	SIMPLE_TREE_NODE *pdomain;
	
    {
	if (!get_domain_info(domain_id, dinfo))
		return FALSE;
	pabnode = ab_tree_get_abnode();
	if (NULL == pabnode) {
		return FALSE;
	}
	pabnode->node_type = NODE_TYPE_DOMAIN;
	pabnode->id = domain_id;
	pabnode->minid = ab_tree_make_minid(MINID_TYPE_DOMAIN, domain_id);
	if (FALSE == ab_tree_cache_node(pbase, pabnode)) {
		return FALSE;
	}
	if (!utf8_check(dinfo.name.c_str()))
		utf8_filter(dinfo.name.data());
	if (!utf8_check(dinfo.title.c_str()))
		utf8_filter(dinfo.title.data());
	if (!utf8_check(dinfo.address.c_str()))
		utf8_filter(dinfo.address.data());
	pabnode->d_info = new(std::nothrow) sql_domain(std::move(dinfo));
	if (pabnode->d_info == nullptr)
		return false;
	pdomain = (SIMPLE_TREE_NODE*)pabnode;
	simple_tree_set_root(ptree, pdomain);

	std::vector<sql_group> file_group;
	if (!get_domain_groups(domain_id, file_group))
		return FALSE;
	for (auto &&grp : file_group) {
		pabnode = ab_tree_get_abnode();
		if (NULL == pabnode) {
			return FALSE;
		}
		pabnode->node_type = NODE_TYPE_GROUP;
		pabnode->id = grp.id;
		pabnode->minid = ab_tree_make_minid(MINID_TYPE_GROUP, grp.id);
		if (FALSE == ab_tree_cache_node(pbase, pabnode)) {
			return FALSE;
		}
		auto grp_id = grp.id;
		pabnode->d_info = new(std::nothrow) sql_group(std::move(grp));
		if (pabnode->d_info == nullptr)
			return false;
		pgroup = (SIMPLE_TREE_NODE*)pabnode;
		simple_tree_add_child(ptree, pdomain, pgroup, SIMPLE_TREE_ADD_LAST);
		
		std::vector<sql_class> file_class;
		if (!get_group_classes(grp_id, file_class))
			return FALSE;
		for (auto &&cls : file_class) {
			pabnode = ab_tree_get_abnode();
			if (NULL == pabnode) {
				return FALSE;
			}
			pabnode->node_type = NODE_TYPE_CLASS;
			pabnode->id = cls.child_id;
			pabnode->minid = ab_tree_make_minid(MINID_TYPE_CLASS, cls.child_id);
			if (NULL == int_hash_query(pbase->phash, pabnode->minid)) {
				if (FALSE == ab_tree_cache_node(pbase, pabnode)) {
					ab_tree_put_abnode(pabnode);
					return FALSE;
				}
			}
			pabnode->d_info = new(std::nothrow) sql_class(std::move(cls));
			if (pabnode->d_info == nullptr)
				return false;
			pclass = (SIMPLE_TREE_NODE*)pabnode;
			simple_tree_add_child(ptree, pgroup,
				pclass, SIMPLE_TREE_ADD_LAST);
			if (!ab_tree_load_class(cls.child_id, ptree, pclass, pbase))
				return FALSE;
		}
		
		std::vector<sql_user> file_user;
		rows = get_group_users(grp_id, file_user);
		if (-1 == rows) {
			return FALSE;
		} else if (0 == rows) {
			continue;
		}
		parray = static_cast<ab_sort_item *>(malloc(sizeof(ab_sort_item) * rows));
		if (NULL == parray) {
			return FALSE;
		}
		i = 0;
		for (auto &&usr : file_user) {
			pabnode = ab_tree_get_abnode();
			if (NULL == pabnode) {
				goto LOAD_FAIL;
			}
			if (usr.dtypx == DT_DISTLIST) {
				if (!ab_tree_load_mlist(pabnode, std::move(usr), pbase)) {
					ab_tree_put_abnode(pabnode);
					goto LOAD_FAIL;
				}
			} else {
				if (!ab_tree_load_user(pabnode, std::move(usr), pbase)) {
					ab_tree_put_abnode(pabnode);
					goto LOAD_FAIL;
				}
			}
			parray[i].pnode = (SIMPLE_TREE_NODE*)pabnode;
			char temp_buff[1024];
			ab_tree_get_display_name(parray[i].pnode, 1252, temp_buff, arsizeof(temp_buff));
			parray[i].string = strdup(temp_buff);
			if (NULL == parray[i].string) {
				ab_tree_put_abnode(pabnode);
				goto LOAD_FAIL;
			}
			i ++;
		}
		
		qsort(parray, rows, sizeof(ab_sort_item), ab_tree_cmpstring);
		for (i=0; i<rows; i++) {
			simple_tree_add_child(ptree, pgroup,
				parray[i].pnode, SIMPLE_TREE_ADD_LAST);
			free(parray[i].string);
		}
		free(parray);
	}
	
	std::vector<sql_user> file_user;
	rows = get_domain_users(domain_id, file_user);
	if (-1 == rows) {
		return FALSE;
	} else if (0 == rows) {
		return TRUE;
	}
	parray = static_cast<ab_sort_item *>(malloc(sizeof(ab_sort_item) * rows));
	if (NULL == parray) {
		return FALSE;	
	}
	i = 0;
	for (auto &&usr : file_user) {
		pabnode = ab_tree_get_abnode();
		if (NULL == pabnode) {
			goto LOAD_FAIL;
		}
		if (usr.dtypx == DT_DISTLIST) {
			if (!ab_tree_load_mlist(pabnode, std::move(usr), pbase)) {
				ab_tree_put_abnode(pabnode);
				goto LOAD_FAIL;
			}
		} else {
			if (!ab_tree_load_user(pabnode, std::move(usr), pbase)) {
				ab_tree_put_abnode(pabnode);
				goto LOAD_FAIL;
			}
		}
		parray[i].pnode = (SIMPLE_TREE_NODE*)pabnode;
		char temp_buff[1024];
		ab_tree_get_display_name(parray[i].pnode, 1252, temp_buff, arsizeof(temp_buff));
		parray[i].string = strdup(temp_buff);
		if (NULL == parray[i].string) {
			ab_tree_put_abnode(pabnode);
			goto LOAD_FAIL;
		}
		i ++;
	}
	
	qsort(parray, rows, sizeof(*parray), ab_tree_cmpstring);
	for (i=0; i<rows; i++) {
		simple_tree_add_child(ptree, pdomain,
			parray[i].pnode, SIMPLE_TREE_ADD_LAST);
		free(parray[i].string);
	}
	free(parray);
	return TRUE;
    }
 LOAD_FAIL:
	for (i-=1; i>=0; i--) {
		free(parray[i].string);
		ab_tree_put_abnode((AB_NODE*)parray[i].pnode);
	}
	free(parray);
	return FALSE;
}

static void ab_tree_enum_nodes(SIMPLE_TREE_NODE *pnode, void *pparam)
{
	uint8_t node_type;
	SINGLE_LIST_NODE *psnode;
	
	node_type = ab_tree_get_node_type(pnode);
	if (node_type > 0x80) {
		return;
	}
	if (NULL != pnode->pdata) {
		return;	
	}
	psnode = ab_tree_get_snode();
	if (NULL == psnode) {
		return;
	}
	psnode->pdata = pnode;
	single_list_append_as_tail((SINGLE_LIST*)pparam, psnode);
}

static BOOL ab_tree_load_base(AB_BASE *pbase)
{
	int i, num;
	DOMAIN_NODE *pdomain;
	char temp_buff[1024];
	SIMPLE_TREE_NODE *proot;
	SINGLE_LIST_NODE *pnode;
	
	if (pbase->base_id > 0) {
		std::vector<int> temp_file;
		if (!get_org_domains(pbase->base_id, temp_file))
			return FALSE;
		for (auto domain_id : temp_file) {
			pdomain = (DOMAIN_NODE*)malloc(sizeof(DOMAIN_NODE));
			if (NULL == pdomain) {
				return FALSE;
			}
			pdomain->node.pdata = pdomain;
			pdomain->domain_id = domain_id;
			simple_tree_init(&pdomain->tree);
			if (FALSE == ab_tree_load_tree(
				domain_id, &pdomain->tree, pbase)) {
				ab_tree_destruct_tree(&pdomain->tree);
				free(pdomain);
				return FALSE;
			}
			single_list_append_as_tail(&pbase->list, &pdomain->node);
		}
	} else {
		pdomain = (DOMAIN_NODE*)malloc(sizeof(DOMAIN_NODE));
		if (NULL == pdomain) {
			return FALSE;
		}
		pdomain->node.pdata = pdomain;
		int domain_id = -pbase->base_id;
		pdomain->domain_id = -pbase->base_id;
		simple_tree_init(&pdomain->tree);
		if (FALSE == ab_tree_load_tree(
			domain_id, &pdomain->tree, pbase)) {
			ab_tree_destruct_tree(&pdomain->tree);
			free(pdomain);
			return FALSE;
		}
		single_list_append_as_tail(&pbase->list, &pdomain->node);
	}
	for (pnode=single_list_get_head(&pbase->list); NULL!=pnode;
		pnode=single_list_get_after(&pbase->list, pnode)) {
		pdomain = (DOMAIN_NODE*)pnode->pdata;
		proot = simple_tree_get_root(&pdomain->tree);
		if (NULL == proot) {
			continue;
		}
		simple_tree_enum_from_node(proot,
			ab_tree_enum_nodes, &pbase->gal_list);
	}
	num = single_list_get_nodes_num(&pbase->gal_list);
	if (num <= 1) {
		return TRUE;
	}
	auto parray = static_cast<ab_sort_item *>(malloc(sizeof(ab_sort_item) * num));
	if (NULL == parray) {
		return TRUE;
	}
	i = 0;
	for (pnode=single_list_get_head(&pbase->gal_list); NULL!=pnode;
		pnode=single_list_get_after(&pbase->gal_list, pnode)) {
		ab_tree_get_display_name(static_cast<SIMPLE_TREE_NODE *>(pnode->pdata),
			1252, temp_buff, arsizeof(temp_buff));
		parray[i].pnode = static_cast<SIMPLE_TREE_NODE *>(pnode->pdata);
		parray[i].string = strdup(temp_buff);
		if (NULL == parray[i].string) {
			for (i-=1; i>=0; i--) {
				free(parray[i].string);
			}
			free(parray);
			return TRUE;
		}
		i ++;
	}
	qsort(parray, num, sizeof(ab_sort_item), ab_tree_cmpstring);
	i = 0;
	for (pnode=single_list_get_head(&pbase->gal_list); NULL!=pnode;
		pnode=single_list_get_after(&pbase->gal_list, pnode)) {
		pnode->pdata = parray[i].pnode;
		free(parray[i].string);
		i ++;
	}
	free(parray);
	return TRUE;
}

AB_BASE_REF ab_tree_get_base(int base_id)
{
	int count;
	AB_BASE *pbase;
	
	count = 0;
 RETRY_LOAD_BASE:
	std::unique_lock bhold(g_base_lock);
	auto it = g_base_hash.find(base_id);
	if (it == g_base_hash.end()) {
		if (g_base_hash.size() >= g_base_size) {
			printf("[exchange_nsp]: W-1298: AB base hash is full\n");
			return nullptr;
		}
		try {
			auto xp = g_base_hash.try_emplace(base_id);
			if (!xp.second)
				return nullptr;
			it = xp.first;
			pbase = &xp.first->second;
		} catch (const std::bad_alloc &) {
			return nullptr;
		}
		pbase->base_id = base_id;
		pbase->status = BASE_STATUS_CONSTRUCTING;
		pbase->guid = guid_random_new();
		memcpy(pbase->guid.node, &base_id, sizeof(int));
		pbase->phash = NULL;
		bhold.unlock();
		if (FALSE == ab_tree_load_base(pbase)) {
			pbase->unload();
			bhold.lock();
			g_base_hash.erase(it);
			bhold.unlock();
			return nullptr;
		}
		time(&pbase->load_time);
		bhold.lock();
		pbase->status = BASE_STATUS_LIVING;
	} else {
		pbase = &it->second;
		if (pbase->status != BASE_STATUS_LIVING) {
			bhold.unlock();
			count ++;
			if (count > 60) {
				return nullptr;
			}
			sleep(1);
			goto RETRY_LOAD_BASE;
		}
	}
	pbase->reference ++;
	return AB_BASE_REF(pbase);
}

void ab_tree_del::operator()(AB_BASE *pbase)
{
	std::lock_guard bhold(g_base_lock);
	pbase->reference --;
}

static void *nspab_scanwork(void *param)
{
	AB_BASE *pbase;
	SINGLE_LIST_NODE *pnode;
	
	while (!g_notify_stop) {
		pbase = NULL;
		std::unique_lock bhold(g_base_lock);
		for (auto &kvpair : g_base_hash) {
			auto &base = kvpair.second;
			if (base.status != BASE_STATUS_LIVING ||
			    base.reference != 0 ||
			    time(nullptr) - base.load_time < g_ab_cache_interval)
				continue;
			pbase = &base;
			pbase->status = BASE_STATUS_CONSTRUCTING;
			break;
		}
		bhold.unlock();
		if (NULL == pbase) {
			sleep(1);
			continue;
		}
		while ((pnode = single_list_pop_front(&pbase->list)) != nullptr) {
			ab_tree_destruct_tree(&((DOMAIN_NODE*)pnode->pdata)->tree);
			free(pnode->pdata);
		}
		while ((pnode = single_list_pop_front(&pbase->gal_list)) != nullptr)
			ab_tree_put_snode(pnode);
		while ((pnode = single_list_pop_front(&pbase->remote_list)) != nullptr) {
			ab_tree_put_abnode(static_cast<AB_NODE *>(pnode->pdata));
			ab_tree_put_snode(pnode);
		}
		if (NULL != pbase->phash) {
			int_hash_free(pbase->phash);
			pbase->phash = NULL;
		}
		if (FALSE == ab_tree_load_base(pbase)) {
			pbase->unload();
			bhold.lock();
			g_base_hash.erase(pbase->base_id);
			bhold.unlock();
		} else {
			bhold.lock();
			time(&pbase->load_time);
			pbase->status = BASE_STATUS_LIVING;
			bhold.unlock();
		}
	}
	return NULL;
}

static int ab_tree_node_to_rpath(SIMPLE_TREE_NODE *pnode,
	char *pbuff, int length)
{
	int len;
	AB_NODE *pabnode;
	char temp_buff[1024];
	
	pabnode = (AB_NODE*)pnode;
	switch (pabnode->node_type) {
	case NODE_TYPE_DOMAIN:
		len = sprintf(temp_buff, "d%d", pabnode->id);
		break;
	case NODE_TYPE_GROUP:
		len = sprintf(temp_buff, "g%d", pabnode->id);
		break;
	case NODE_TYPE_CLASS:
		len = sprintf(temp_buff, "c%d", pabnode->id);
		break;
	case NODE_TYPE_PERSON:
		len = sprintf(temp_buff, "p%d", pabnode->id);
		break;
	case NODE_TYPE_MLIST:
		len = sprintf(temp_buff, "l%d", pabnode->id);
		break;
	case NODE_TYPE_ROOM:
		len = sprintf(temp_buff, "r%d", pabnode->id);
		break;
	case NODE_TYPE_EQUIPMENT:
		len = sprintf(temp_buff, "e%d", pabnode->id);
		break;
	default:
		return 0;
	}
	if (len >= length) {
		return 0;
	}
	memcpy(pbuff, temp_buff, len + 1);
	return len;
}

static BOOL ab_tree_node_to_path(SIMPLE_TREE_NODE *pnode,
	char *pbuff, int length)
{
	int len;
	int offset;
	AB_BASE_REF pbase;
	SIMPLE_TREE_NODE **ppnode;
	
	if (NODE_TYPE_REMOTE == ((AB_NODE*)pnode)->node_type) {
		pbase = ab_tree_get_base(-reinterpret_cast<AB_NODE *>(pnode)->id);
		if (pbase == nullptr)
			return FALSE;
		ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash,
		         reinterpret_cast<AB_NODE *>(pnode)->minid));
		if (NULL == ppnode) {
			return FALSE;
		}
		pnode = *ppnode;
	}
	
	offset = 0;
	do {
		len = ab_tree_node_to_rpath(pnode,
			pbuff + offset, length - offset);
		if (0 == len) {
			return FALSE;
		}
		offset += len;
	} while ((pnode = simple_tree_node_get_parent(pnode)) != NULL);
	return TRUE;
}


static bool ab_tree_md5_path(const char *path, uint64_t *pdgt) __attribute__((warn_unused_result));
static bool ab_tree_md5_path(const char *path, uint64_t *pdgt)
{
	int i;
	uint64_t b;
	uint8_t dgt_buff[MD5_DIGEST_LENGTH];
	std::unique_ptr<EVP_MD_CTX, sslfree> ctx(EVP_MD_CTX_new());

	if (ctx == nullptr ||
	    EVP_DigestInit(ctx.get(), EVP_md5()) <= 0 ||
	    EVP_DigestUpdate(ctx.get(), path, strlen(path)) <= 0 ||
	    EVP_DigestFinal(ctx.get(), dgt_buff, nullptr) <= 0)
		return false;
	*pdgt = 0;
	for (i=0; i<16; i+=2) {
		b = dgt_buff[i];
		*pdgt |= (b << 4*i);
	}
	return true;
}

bool ab_tree_node_to_guid(SIMPLE_TREE_NODE *pnode, GUID *pguid)
{
	uint64_t dgt;
	uint32_t tmp_id;
	AB_NODE *pabnode;
	char temp_path[512];
	SIMPLE_TREE_NODE *proot;
	SIMPLE_TREE_NODE *pnode1;
	
	pabnode = (AB_NODE*)pnode;
	if (pabnode->node_type < 0x80 && NULL != pnode->pdata) {
		return ab_tree_node_to_guid(static_cast<SIMPLE_TREE_NODE *>(pnode->pdata), pguid);
	}
	memset(pguid, 0, sizeof(GUID));
	pguid->time_low = static_cast<unsigned int>(pabnode->node_type) << 24;
	if (NODE_TYPE_REMOTE == pabnode->node_type) {
		pguid->time_low |= pabnode->id;
		tmp_id = ab_tree_get_minid_value(pabnode->minid);
		pguid->time_hi_and_version = (tmp_id & 0xFFFF0000) >> 16;
		pguid->time_mid = tmp_id & 0xFFFF;
	} else {
		proot = pnode;
		while ((pnode1 = simple_tree_node_get_parent(proot)) != NULL)
			proot = pnode1;
		pguid->time_low |= ((AB_NODE*)proot)->id;
		pguid->time_hi_and_version = (pabnode->id & 0xFFFF0000) >> 16;
		pguid->time_mid = pabnode->id & 0xFFFF;
	}
	memset(temp_path, 0, sizeof(temp_path));
	ab_tree_node_to_path((SIMPLE_TREE_NODE*)
		pabnode, temp_path, sizeof(temp_path));
	if (!ab_tree_md5_path(temp_path, &dgt))
		return false;
	pguid->node[0] = dgt & 0xFF;
	pguid->node[1] = (dgt & 0xFF00) >> 8;
	pguid->node[2] = (dgt & 0xFF0000) >> 16;
	pguid->node[3] = (dgt & 0xFF000000) >> 24;
	pguid->node[4] = (dgt & 0xFF00000000ULL) >> 32;
	pguid->node[5] = (dgt & 0xFF0000000000ULL) >> 40;
	pguid->clock_seq[0] = (dgt & 0xFF000000000000ULL) >> 48;
	pguid->clock_seq[1] = (dgt & 0xFF00000000000000ULL) >> 56;
	return true;
}

BOOL ab_tree_node_to_dn(SIMPLE_TREE_NODE *pnode, char *pbuff, int length)
{
	int id;
	char *ptoken;
	int domain_id;
	AB_BASE_REF pbase;
	AB_NODE *pabnode;
	char cusername[UADDR_SIZE];
	char hex_string[32];
	char hex_string1[32];
	SIMPLE_TREE_NODE **ppnode;
	
	pabnode = (AB_NODE*)pnode;
	if (NODE_TYPE_REMOTE == pabnode->node_type) {
		pbase = ab_tree_get_base(-pabnode->id);
		if (pbase == nullptr)
			return FALSE;
		ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash, pabnode->minid));
		if (NULL == ppnode) {
			return FALSE;
		}
		pabnode = (AB_NODE*)*ppnode;
		pnode = *ppnode;
	}
	switch (pabnode->node_type) {
	case NODE_TYPE_PERSON:
	case NODE_TYPE_ROOM:
	case NODE_TYPE_EQUIPMENT:
		id = pabnode->id;
		ab_tree_get_user_info(pnode, USER_MAIL_ADDRESS, cusername, arsizeof(cusername));
		ptoken = strchr(cusername, '@');
		if (NULL != ptoken) {
			*ptoken = '\0';
		}
		while ((pnode = simple_tree_node_get_parent(pnode)) != NULL)
			pabnode = (AB_NODE*)pnode;
		if (pabnode->node_type != NODE_TYPE_DOMAIN) {
			return FALSE;
		}
		domain_id = pabnode->id;
		encode_hex_int(id, hex_string);
		encode_hex_int(domain_id, hex_string1);
		sprintf(pbuff, "/o=%s/ou=Exchange Administrative Group"
				" (FYDIBOHF23SPDLT)/cn=Recipients/cn=%s%s-%s",
			g_nsp_org_name, hex_string1, hex_string, cusername);
		HX_strupper(pbuff);
		break;
	case NODE_TYPE_MLIST: try {
		id = pabnode->id;
		auto obj = static_cast<sql_user *>(pabnode->d_info);
		std::string username = obj->username;
		auto pos = username.find('@');
		if (pos != username.npos)
			username.erase(pos);
		while ((pnode = simple_tree_node_get_parent(pnode)) != NULL)
			pabnode = (AB_NODE*)pnode;
		if (pabnode->node_type != NODE_TYPE_DOMAIN) {
			return FALSE;
		}
		domain_id = pabnode->id;
		encode_hex_int(id, hex_string);
		encode_hex_int(domain_id, hex_string1);
		sprintf(pbuff, "/o=%s/ou=Exchange Administrative Group"
				" (FYDIBOHF23SPDLT)/cn=Recipients/cn=%s%s-%s",
			g_nsp_org_name, hex_string1, hex_string, username.c_str());
		HX_strupper(pbuff);
		break;
	} catch (...) {
		return false;
	}
	default:
		return FALSE;
	}
	return TRUE;	
}

SIMPLE_TREE_NODE* ab_tree_dn_to_node(AB_BASE *pbase, const char *pdn)
{
	int id;
	int temp_len;
	int domain_id;
	uint32_t minid;
	AB_NODE *pabnode;
	SINGLE_LIST_NODE *psnode;
	char prefix_string[1024];
	SIMPLE_TREE_NODE **ppnode;
	
	temp_len = gx_snprintf(prefix_string, GX_ARRAY_SIZE(prefix_string), "/o=%s/ou=Exchange "
			"Administrative Group (FYDIBOHF23SPDLT)", g_nsp_org_name);
	if (temp_len < 0 || strncasecmp(pdn, prefix_string, temp_len) != 0)
		return NULL;
	if (strncasecmp(pdn + temp_len, "/cn=Configuration/cn=Servers/cn=", 32) == 0 &&
	    strlen(pdn) >= static_cast<size_t>(temp_len) + 60) {
		/* Reason for 60: see DN format in ab_tree_get_server_dn */
		id = decode_hex_int(pdn + temp_len + 60);
		minid = ab_tree_make_minid(MINID_TYPE_ADDRESS, id);
		ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash, minid));
		if (NULL != ppnode) {
			return *ppnode;
		} else {
			return NULL;
		}
	}
	if (0 != strncasecmp(pdn + temp_len, "/cn=Recipients/cn=", 18)) {
		return NULL;
	}
	domain_id = decode_hex_int(pdn + temp_len + 18);
	id = decode_hex_int(pdn + temp_len + 26);
	minid = ab_tree_make_minid(MINID_TYPE_ADDRESS, id);
	ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash, minid));
	if (NULL != ppnode) {
		return *ppnode;
	}
	std::unique_lock rhold(g_remote_lock);
	for (psnode=single_list_get_head(&pbase->remote_list); NULL!=psnode;
		psnode=single_list_get_after(&pbase->remote_list, psnode)) {
		if (minid == ((AB_NODE*)psnode->pdata)->minid) {
			return static_cast<SIMPLE_TREE_NODE *>(psnode->pdata);
		}
	}
	rhold.unlock();
	for (psnode=single_list_get_head(&pbase->list); NULL!=psnode;
		psnode=single_list_get_after(&pbase->list, psnode)) {
		if (((DOMAIN_NODE*)psnode->pdata)->domain_id == domain_id) {
			return NULL;
		}
	}
	auto pbase1 = ab_tree_get_base(-domain_id);
	if (pbase1 == nullptr)
		return NULL;
	ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase1->phash, minid));
	if (NULL == ppnode) {
		return NULL;
	}
	psnode = ab_tree_get_snode();
	if (NULL == psnode) {
		return NULL;
	}
	pabnode = ab_tree_get_abnode();
	if (NULL == pabnode) {
		ab_tree_put_snode(psnode);
		return NULL;
	}
	psnode->pdata = pabnode;
	((SIMPLE_TREE_NODE*)pabnode)->pdata = NULL;
	pabnode->node_type = NODE_TYPE_REMOTE;
	pabnode->minid = ((AB_NODE*)*ppnode)->minid;
	pabnode->id = domain_id;
	pabnode->d_info = new(std::nothrow) sql_domain(*static_cast<sql_domain *>(reinterpret_cast<AB_NODE *>(*ppnode)->d_info));
	if (pabnode->d_info == nullptr) {
		ab_tree_put_abnode(pabnode);
		ab_tree_put_snode(psnode);
		return nullptr;
	}
	pbase1.reset();
	rhold.lock();
	single_list_append_as_tail(&pbase->remote_list, psnode);
	return (SIMPLE_TREE_NODE*)pabnode;
}

SIMPLE_TREE_NODE* ab_tree_uid_to_node(AB_BASE *pbase, int user_id)
{
	uint32_t minid;
	
	minid = ab_tree_make_minid(MINID_TYPE_ADDRESS, user_id);
	auto ppnode = static_cast<SIMPLE_TREE_NODE **>(int_hash_query(pbase->phash, minid));
	if (NULL == ppnode) {
		return NULL;
	}
	return *ppnode;
}

uint32_t ab_tree_get_node_minid(SIMPLE_TREE_NODE *pnode)
{
	return ((AB_NODE*)pnode)->minid;
}

uint8_t ab_tree_get_node_type(SIMPLE_TREE_NODE *pnode)
{
	AB_NODE *pabnode;
	uint8_t node_type;
	SIMPLE_TREE_NODE **ppnode;
	
	pabnode = (AB_NODE*)pnode;
	if (pabnode->node_type != NODE_TYPE_REMOTE)
		return pabnode->node_type;
	auto pbase = ab_tree_get_base(-pabnode->id);
	if (pbase == nullptr)
		return NODE_TYPE_REMOTE;
	ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash, pabnode->minid));
	if (NULL == ppnode) {
		return NODE_TYPE_REMOTE;
	}
	node_type = ((AB_NODE*)*ppnode)->node_type;
	return node_type;
}

void ab_tree_get_display_name(SIMPLE_TREE_NODE *pnode, uint32_t codepage,
    char *str_dname, size_t dn_size)
{
	char *ptoken;
	AB_NODE *pabnode;
	char lang_string[256];
	
	pabnode = (AB_NODE*)pnode;
	if (dn_size > 0)
		str_dname[0] = '\0';
	switch (pabnode->node_type) {
	case NODE_TYPE_DOMAIN: {
		auto obj = static_cast<sql_domain *>(pabnode->d_info);
		HX_strlcpy(str_dname, obj->title.c_str(), dn_size);
		break;
	}
	case NODE_TYPE_GROUP: {
		auto obj = static_cast<sql_group *>(pabnode->d_info);
		HX_strlcpy(str_dname, obj->title.c_str(), dn_size);
		break;
	}
	case NODE_TYPE_CLASS: {
		auto obj = static_cast<sql_class *>(pabnode->d_info);
		HX_strlcpy(str_dname, obj->name.c_str(), dn_size);
		break;
	}
	case NODE_TYPE_PERSON:
	case NODE_TYPE_ROOM:
	case NODE_TYPE_EQUIPMENT: {
		auto obj = static_cast<sql_user *>(pabnode->d_info);
		auto it = obj->propvals.find(PR_DISPLAY_NAME);
		if (it != obj->propvals.cend()) {
			HX_strlcpy(str_dname, it->second.c_str(), dn_size);
		} else {
			HX_strlcpy(str_dname, obj->username.c_str(), dn_size);
			ptoken = strchr(str_dname, '@');
			if (NULL != ptoken) {
				*ptoken = '\0';
			}
		}
		break;
	}
	case NODE_TYPE_MLIST: {
		auto obj = static_cast<sql_user *>(pabnode->d_info);
		auto it = obj->propvals.find(PR_DISPLAY_NAME);
		switch (obj->list_type) {
		case MLIST_TYPE_NORMAL:
			if (FALSE == get_lang(codepage, "mlist0", lang_string, 256)) {
				strcpy(lang_string, "custom address list");
			}
			snprintf(str_dname, dn_size, "%s(%s)", obj->username.c_str(), lang_string);
			break;
		case MLIST_TYPE_GROUP:
			if (FALSE == get_lang(codepage, "mlist1", lang_string, 256)) {
				strcpy(lang_string, "all users in department of %s");
			}
			snprintf(str_dname, dn_size, lang_string, it != obj->propvals.cend() ? it->second.c_str() : "");
			break;
		case MLIST_TYPE_DOMAIN:
			if (!get_lang(codepage, "mlist2", str_dname, dn_size))
				HX_strlcpy(str_dname, "all users in domain", dn_size);
			break;
		case MLIST_TYPE_CLASS:
			if (FALSE == get_lang(codepage, "mlist3", lang_string, 256)) {
				strcpy(lang_string, "all users in group of %s");
			}
			snprintf(str_dname, dn_size, lang_string, it != obj->propvals.cend() ? it->second.c_str() : "");
			break;
		default:
			snprintf(str_dname, dn_size, "unknown address list type %u", obj->list_type);
		}
		break;
	}
	}
}

std::vector<std::string> ab_tree_get_object_aliases(SIMPLE_TREE_NODE *pnode, unsigned int type)
{
	std::vector<std::string> alist;
	auto pabnode = reinterpret_cast<AB_NODE *>(pnode);
	for (const auto &a : static_cast<sql_user *>(pabnode->d_info)->aliases)
		alist.push_back(a);
	return alist;
}

void ab_tree_get_user_info(SIMPLE_TREE_NODE *pnode, int type, char *value, size_t vsize)
{
	AB_NODE *pabnode;
	
	value[0] = '\0';
	pabnode = (AB_NODE*)pnode;
	if (pabnode->node_type != NODE_TYPE_PERSON &&
		pabnode->node_type != NODE_TYPE_ROOM &&
		pabnode->node_type != NODE_TYPE_EQUIPMENT &&
		pabnode->node_type != NODE_TYPE_REMOTE) {
		return;
	}
	auto u = static_cast<sql_user *>(pabnode->d_info);
	unsigned int tag = 0;
	switch (type) {
	case USER_MAIL_ADDRESS: gx_strlcpy(value, u->username.c_str(), vsize); return;
	case USER_REAL_NAME: tag = PR_DISPLAY_NAME; break;
	case USER_JOB_TITLE: tag = PROP_TAG_TITLE; break;
	case USER_COMMENT: tag = PROP_TAG_COMMENT; break;
	case USER_MOBILE_TEL: tag = PROP_TAG_MOBILETELEPHONENUMBER; break;
	case USER_BUSINESS_TEL: tag = PROP_TAG_PRIMARYTELEPHONENUMBER; break;
	case USER_NICK_NAME: tag = PROP_TAG_NICKNAME; break;
	case USER_HOME_ADDRESS: tag = PROP_TAG_HOMEADDRESSSTREET; break;
	case USER_CREATE_DAY: *value = '\0'; return;
	case USER_STORE_PATH: strcpy(value, u->maildir.c_str()); return;
	}
	if (tag == 0)
		return;
	auto it = u->propvals.find(tag);
	if (it != u->propvals.cend())
		gx_strlcpy(value, it->second.c_str(), vsize);
}

void ab_tree_get_mlist_info(SIMPLE_TREE_NODE *pnode,
	char *mail_address, char *create_day, int *plist_privilege)
{
	AB_NODE *pabnode;
	
	pabnode = (AB_NODE*)pnode;
	if (pabnode->node_type != NODE_TYPE_MLIST &&
		pabnode->node_type != NODE_TYPE_REMOTE) {
		mail_address[0] = '\0';
		*plist_privilege = 0;
		return;
	}
	auto obj = static_cast<sql_user *>(pabnode->d_info);
	if (mail_address != nullptr)
		strcpy(mail_address, obj->username.c_str());
	if (create_day != nullptr)
		*create_day = '\0';
	if (plist_privilege != nullptr)
		*plist_privilege = obj->list_priv;
}

void ab_tree_get_mlist_title(uint32_t codepage, char *str_title)
{
	if (FALSE == get_lang(codepage, "mlist", str_title, 256)) {
		strcpy(str_title, "Address List");
	}
}

void ab_tree_get_server_dn(SIMPLE_TREE_NODE *pnode, char *dn, int length)
{
	char *ptoken;
	char username[UADDR_SIZE];
	char hex_string[32];
	
	if (reinterpret_cast<AB_NODE *>(pnode)->node_type >= 0x80)
		return;
	memset(username, 0, sizeof(username));
	ab_tree_get_user_info(pnode, USER_MAIL_ADDRESS, username, GX_ARRAY_SIZE(username));
	ptoken = strchr(username, '@');
	HX_strlower(username);
	if (NULL != ptoken) {
		ptoken++;
	} else {
		ptoken = username;
	}
	if (NODE_TYPE_REMOTE == ((AB_NODE *)pnode)->node_type) {
		encode_hex_int(ab_tree_get_minid_value(
			((AB_NODE *)pnode)->minid), hex_string);
	} else {
		encode_hex_int(((AB_NODE *)pnode)->id, hex_string);
	}
	snprintf(dn, length, "/o=%s/ou=Exchange Administrative "
	         "Group (FYDIBOHF23SPDLT)/cn=Configuration/cn=Servers"
	         "/cn=%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x"
	         "-%02x%02x%s@%s", g_nsp_org_name, username[0], username[1],
	         username[2], username[3], username[4], username[5],
	         username[6], username[7], username[8], username[9],
	         username[10], username[11], hex_string, ptoken);
	HX_strupper(dn);
}

void ab_tree_get_company_info(SIMPLE_TREE_NODE *pnode,
	char *str_name, char *str_address)
{
	AB_BASE_REF pbase;
	AB_NODE *pabnode;
	SIMPLE_TREE_NODE **ppnode;
	
	pabnode = (AB_NODE*)pnode;
	if (NODE_TYPE_REMOTE == pabnode->node_type) {
		pbase = ab_tree_get_base(-pabnode->id);
		if (pbase == nullptr) {
			str_name[0] = '\0';
			str_address[0] = '\0';
			return;
		}
		ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash, pabnode->minid));
		if (NULL == ppnode) {
			str_name[0] = '\0';
			str_address[0] = '\0';
			return;
		}
		pnode = *ppnode;
		pabnode = (AB_NODE*)*ppnode;
	}
	while ((pnode = simple_tree_node_get_parent(pnode)) != NULL)
		pabnode = (AB_NODE*)pnode;
	auto obj = static_cast<sql_domain *>(pabnode->d_info);
	if (str_name != nullptr)
		strcpy(str_name, obj->title.c_str());
	if (str_address != nullptr)
		strcpy(str_address, obj->address.c_str());
}

void ab_tree_get_department_name(SIMPLE_TREE_NODE *pnode, char *str_name)
{
	AB_BASE_REF pbase;
	AB_NODE *pabnode;
	SIMPLE_TREE_NODE **ppnode;
	
	if (NODE_TYPE_REMOTE == ((AB_NODE*)pnode)->node_type) {
		pbase = ab_tree_get_base(-reinterpret_cast<AB_NODE *>(pnode)->id);
		if (pbase == nullptr) {
			str_name[0] = '\0';
			return;
		}
		ppnode = static_cast<decltype(ppnode)>(int_hash_query(pbase->phash, reinterpret_cast<AB_NODE *>(pnode)->minid));
		if (NULL == ppnode) {
			str_name[0] = '\0';
			return;
		}
		pnode = *ppnode;
	}
	do {
		pabnode = (AB_NODE*)pnode;
		if (NODE_TYPE_GROUP == pabnode->node_type) {
			break;
		}
	} while ((pnode = simple_tree_node_get_parent(pnode)) != NULL);
	if (NULL == pnode) {
		str_name[0] = '\0';
		return;
	}
	auto obj = static_cast<sql_group *>(pabnode->d_info);
	strcpy(str_name, obj->title.c_str());
}

int ab_tree_get_guid_base_id(GUID guid)
{
	int base_id;
	
	memcpy(&base_id, guid.node, sizeof(int));
	std::lock_guard bhold(g_base_lock);
	return g_base_hash.find(base_id) != g_base_hash.end() ? base_id : 0;
}

ec_error_t ab_tree_fetchprop(SIMPLE_TREE_NODE *node, unsigned int codepage,
    unsigned int proptag, PROPERTY_VALUE *prop)
{
	auto node_type = ab_tree_get_node_type(node);
	if (node_type != NODE_TYPE_PERSON && node_type != NODE_TYPE_ROOM &&
	    node_type != NODE_TYPE_EQUIPMENT && node_type != NODE_TYPE_MLIST)
		return ecNotFound;
	const auto &obj = *static_cast<sql_user *>(reinterpret_cast<AB_NODE *>(node)->d_info);
	auto it = obj.propvals.find(proptag);
	if (it == obj.propvals.cend())
		return ecNotFound;

	switch (PROP_TYPE(proptag)) {
	case PT_BOOLEAN:
		prop->value.b = strtol(it->second.c_str(), nullptr, 0) != 0;
		return ecSuccess;
	case PT_SHORT:
		prop->value.s = strtol(it->second.c_str(), nullptr, 0);
		return ecSuccess;
	case PT_LONG:
		prop->value.l = strtol(it->second.c_str(), nullptr, 0);
		return ecSuccess;
	case PT_I8:
		prop->value.l = strtoll(it->second.c_str(), nullptr, 0);
		return ecSuccess;
	case PT_SYSTIME:
		common_util_day_to_filetime(it->second.c_str(), &prop->value.ftime);
		return ecSuccess;
	case PT_STRING8: {
		auto tg = ndr_stack_anew<char>(NDR_STACK_OUT, it->second.size() + 1);
		if (tg == nullptr)
			return ecMAPIOOM;
		auto ret = common_util_from_utf8(codepage, it->second.c_str(), tg, it->second.size());
		if (ret < 0)
			return ecError;
		tg[ret] = '\0';
		prop->value.pstr = tg;
		return ecSuccess;
	}
	case PT_UNICODE: {
		auto tg = ndr_stack_anew<char>(NDR_STACK_OUT, it->second.size() + 1);
		if (tg == nullptr)
			return ecMAPIOOM;
		strcpy(tg, it->second.c_str());
		prop->value.pstr = tg;
		return ecSuccess;
	}
	case PT_BINARY: {
		prop->value.bin.cb = it->second.size();
		prop->value.bin.pv = ndr_stack_alloc(NDR_STACK_OUT, it->second.size());
		if (prop->value.bin.pv == nullptr)
			return ecMAPIOOM;
		memcpy(prop->value.bin.pv, it->second.data(), prop->value.bin.cb);
		return ecSuccess;
	}
	case PT_MV_UNICODE: {
		auto &x = prop->value.string_array;
		x.count = 1;
		x.ppstr = ndr_stack_anew<char *>(NDR_STACK_OUT);
		if (x.ppstr == nullptr)
			return ecMAPIOOM;
		auto tg = ndr_stack_anew<char>(NDR_STACK_OUT, it->second.size() + 1);
		if (tg == nullptr)
			return ecMAPIOOM;
		strcpy(tg, it->second.c_str());
		x.ppstr[0] = tg;
		return ecSuccess;
	}
	}
	return ecNotFound;
}

void ab_tree_invalidate_cache()
{
	printf("[exchange_nsp]: Invalidating AB caches\n");
	std::unique_lock bl_hold(g_base_lock);
	for (auto &kvpair : g_base_hash)
		kvpair.second.load_time = 0;
}
