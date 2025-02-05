#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <gromox/proc_common.h>
#include <gromox/simple_tree.hpp>
#include <gromox/single_list.hpp>
#include <gromox/int_hash.hpp>
#define NODE_TYPE_DOMAIN					0x81
#define NODE_TYPE_GROUP						0x82
#define NODE_TYPE_CLASS						0x83
#define NODE_TYPE_REMOTE					0x0
#define NODE_TYPE_PERSON					0x1
#define NODE_TYPE_MLIST						0x2
#define NODE_TYPE_ROOM						0x3
#define NODE_TYPE_EQUIPMENT					0x4
#define NODE_TYPE_FOLDER					0x5
#define USER_MAIL_ADDRESS					0
#define USER_REAL_NAME						1
#define USER_JOB_TITLE						2
#define USER_COMMENT						3
#define USER_MOBILE_TEL						4
#define USER_BUSINESS_TEL					5
#define USER_NICK_NAME						6
#define USER_HOME_ADDRESS					7
#define USER_CREATE_DAY						8
#define USER_STORE_PATH						9

struct PROPERTY_VALUE;

struct DOMAIN_NODE {
	SINGLE_LIST_NODE node;
	int domain_id;
	SIMPLE_TREE tree;
};

struct AB_BASE {
	AB_BASE();
	~AB_BASE() { unload(); }
	void unload();

	GUID guid{};
	std::atomic<int> status{0}, reference{0};
	time_t load_time = 0;
	int base_id = 0;
	SINGLE_LIST list, gal_list, remote_list{};
	INT_HASH_TABLE *phash = nullptr;
};

struct ab_tree_del {
	void operator()(AB_BASE *);
};

using AB_BASE_REF = std::unique_ptr<AB_BASE, ab_tree_del>;

extern void ab_tree_init(const char *org_name, size_t base_size, int cache_interval, int file_blocks);
extern int ab_tree_run();
extern void ab_tree_stop();
extern AB_BASE_REF ab_tree_get_base(int base_id);
uint32_t ab_tree_get_leaves_num(SIMPLE_TREE_NODE *pnode);
extern bool ab_tree_node_to_guid(SIMPLE_TREE_NODE *, GUID *) __attribute__((warn_unused_result));
BOOL ab_tree_node_to_dn(SIMPLE_TREE_NODE *pnode, char *pbuff, int length);
SIMPLE_TREE_NODE* ab_tree_dn_to_node(AB_BASE *pbase, const char *pdn);
SIMPLE_TREE_NODE* ab_tree_uid_to_node(AB_BASE *pbase, int user_id);
SIMPLE_TREE_NODE* ab_tree_minid_to_node(AB_BASE *pbase, uint32_t minid);
uint32_t ab_tree_get_node_minid(SIMPLE_TREE_NODE *pnode);
uint8_t ab_tree_get_node_type(SIMPLE_TREE_NODE *pnode);
extern void ab_tree_get_display_name(SIMPLE_TREE_NODE *, uint32_t codepage, char *str_dname, size_t dn_size);
extern std::vector<std::string> ab_tree_get_object_aliases(SIMPLE_TREE_NODE *, unsigned int type);
extern void ab_tree_get_user_info(SIMPLE_TREE_NODE *, int type, char *value, size_t vsize);
void ab_tree_get_mlist_info(SIMPLE_TREE_NODE *pnode,
	char *mail_address, char *create_day, int *plist_privilege);
void ab_tree_get_mlist_title(uint32_t codepage, char *str_title);
void ab_tree_get_company_info(SIMPLE_TREE_NODE *pnode,
	char *str_name, char *str_address);
void ab_tree_get_department_name(SIMPLE_TREE_NODE *pnode,
	char *str_name);
void ab_tree_get_server_dn(SIMPLE_TREE_NODE *pnode, char *dn, int length);
int ab_tree_get_guid_base_id(GUID guid);
extern ec_error_t ab_tree_fetchprop(SIMPLE_TREE_NODE *, unsigned int codepage, unsigned int proptag, PROPERTY_VALUE *);
extern void ab_tree_invalidate_cache();
