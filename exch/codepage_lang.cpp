// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#define DECLARE_API_STATIC
#include <cerrno>
#include <shared_mutex>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/single_list.hpp>
#include <gromox/svc_common.h>
#include <gromox/util.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace gromox;

enum {
	RETRIEVE_NONE,
	RETRIEVE_TAG_FINDING,
	RETRIEVE_TAG_FOUND,
	RETRIEVE_TAG_END,
	RETRIEVE_VALUE_FINDING,
	RETRIEVE_VALUE_FOUND,
	RETRIEVE_VALUE_END,
	RETRIEVE_END
};

namespace {

struct CODEPAGE_NODE {
	SINGLE_LIST_NODE node;
	uint32_t codepage;
	SINGLE_LIST lang_list;
};

struct LANG_NODE {
	SINGLE_LIST_NODE node;
	char *tag;
	char *value;
};

}

static SINGLE_LIST g_cp_list;
static std::shared_mutex g_list_lock;

static void codepage_lang_unload_langlist(SINGLE_LIST *plist)
{
	LANG_NODE *plang;
	SINGLE_LIST_NODE *pnode;
	
	while ((pnode = single_list_pop_front(plist)) != nullptr) {
		plang = (LANG_NODE*)pnode->pdata;
		free(plang->tag);
		free(plang->value);
		free(plang);
	}
}

static BOOL codepage_lang_load_langlist(SINGLE_LIST *plist,
	char *digest_buff, int length)
{
	int val_len;
	int i, rstat;
	int last_pos;
	size_t temp_len;
	char temp_tag[128];
	char temp_value[2048];
	char temp_value1[1024];
	
	
	last_pos = 0;
	rstat = RETRIEVE_NONE;
    for (i=0; i<length; i++) {
		switch (rstat) {
		case RETRIEVE_NONE:
			/* get the first "{" in the buffer */
			if ('{' == digest_buff[i]) {
				rstat = RETRIEVE_TAG_FINDING;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_TAG_FINDING:
			if ('"' == digest_buff[i]) {
				rstat = RETRIEVE_TAG_FOUND;
				last_pos = i + 1;
			} else if ('}' == digest_buff[i]) {
				rstat = RETRIEVE_END;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_TAG_FOUND:
			if ('"' == digest_buff[i] && '\\' != digest_buff[i - 1]) {
				if (i < last_pos || i - last_pos > 127) {
					return FALSE;
				}
				rstat = RETRIEVE_TAG_END;
				if (static_cast<size_t>(i - last_pos) > sizeof(temp_tag))
					return FALSE;
				memcpy(temp_tag, digest_buff + last_pos, i - last_pos);
				temp_tag[i - last_pos] = '\0';
			}
			break;
		case RETRIEVE_TAG_END:
			if (':' == digest_buff[i]) {
				rstat = RETRIEVE_VALUE_FINDING;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_VALUE_FINDING:
			if ('"' == digest_buff[i]) {
				rstat = RETRIEVE_VALUE_FOUND;
				last_pos = i + 1;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_VALUE_FOUND:
			if ('"' == digest_buff[i] && '\\' != digest_buff[i - 1]) {
				if (i < last_pos || static_cast<size_t>(i - last_pos) >= sizeof(temp_value))
					return FALSE;
				val_len = i - last_pos;
				memcpy(temp_value, digest_buff + last_pos, val_len);
				temp_value[val_len] = '\0';
				if (0 != decode64(temp_value, val_len,
					temp_value1, &temp_len)) {
					return FALSE;
				}
				auto plang = static_cast<LANG_NODE *>(malloc(sizeof(LANG_NODE)));
				if (NULL == plang) {
					return FALSE;
				}
				plang->node.pdata = plang;
				plang->tag = strdup(temp_tag);
				if (NULL == plang->tag) {
					free(plang);
					return FALSE;
				}
				plang->value = strdup(temp_value1);
				if (NULL == plang->value) {
					free(plang->tag);
					free(plang);
					return FALSE;
				}
				single_list_append_as_tail(plist, &plang->node);
				rstat = RETRIEVE_VALUE_END;
			}
			break;
		case RETRIEVE_VALUE_END:
			if (',' == digest_buff[i]) {
				rstat = RETRIEVE_TAG_FINDING;
			} else if ('}' == digest_buff[i]) {
				rstat = RETRIEVE_END;
			} else if (' ' != digest_buff[i] && '\t' != digest_buff[i]) {
				return FALSE;
			}
			break;
		case RETRIEVE_END:
			if (' ' != digest_buff[i] && '\t' != digest_buff[i] &&
				'\r' != digest_buff[i] && '\n' != digest_buff[i] &&
				'\0' != digest_buff[i]) {
				return FALSE;
			}
			break;
		}
	}
	
	if (RETRIEVE_END != rstat) {
		return FALSE;
	}
	
	return TRUE;
}

static BOOL codepage_lang_load_cplist(const char *filename, const char *sdlist, SINGLE_LIST *plist)
{
	char *ptr;
	size_t temp_len;
	char temp_line[64*1024];
	
	auto fp = fopen_sd(filename, sdlist);
	if (NULL == fp) {
		printf("[codepage_lang]: fopen_sd %s: %s\n", filename, strerror(errno));
       return FALSE;
    }
	
	for (int i = 0; fgets(temp_line, sizeof(temp_line), fp.get()); ++i) {
		if ('\r' == temp_line[0] || '\n' == temp_line[0] ||
			'#' == temp_line[0]) {
		   /* skip empty line or comments */
		   continue;
		}

		ptr = strchr(temp_line, ':');
		if (NULL == ptr) {
			printf("[codepage_lang]: line %d format error in %s\n", i + 1, filename);
			return FALSE;
		}
		
		*ptr = '\0';
		ptr ++;
		temp_len = strlen(ptr);
		auto pcodepage = static_cast<CODEPAGE_NODE *>(malloc(sizeof(CODEPAGE_NODE)));
		if (NULL == pcodepage) {
			printf("[codepage_lang]: out of memory while loading file %s\n", filename);
			return FALSE;
		}
		
		pcodepage->node.pdata = pcodepage;
		pcodepage->codepage = atoi(temp_line);
		single_list_init(&pcodepage->lang_list);
		if (FALSE == codepage_lang_load_langlist(&pcodepage->lang_list,
			ptr, temp_len)) {
			codepage_lang_unload_langlist(&pcodepage->lang_list);
			free(pcodepage);
			printf("[codepage_lang]: fail to parse line %d in %s\n", i + 1, filename);
			return FALSE;
		}
		single_list_append_as_tail(plist, &pcodepage->node);
	}
	
	if (0 == single_list_get_nodes_num(plist)) {
		return FALSE;
	}
	return TRUE;
}

static void codepage_lang_unload_cplist(SINGLE_LIST *plist)
{
	SINGLE_LIST_NODE *pnode;
	CODEPAGE_NODE *pcodepage;
	
	while ((pnode = single_list_pop_front(plist)) != nullptr) {
		pcodepage = (CODEPAGE_NODE*)pnode->pdata;
		codepage_lang_unload_langlist(&pcodepage->lang_list);
		free(pcodepage);
	}
}

static int codepage_lang_run(const char *filename, const char *sdlist)
{
	single_list_init(&g_cp_list);
	if (!codepage_lang_load_cplist(filename, sdlist, &g_cp_list))
		return -1;
	return 0;
}

static void codepage_lang_stop()
{
	codepage_lang_unload_cplist(&g_cp_list);
}

static BOOL codepage_lang_get_lang(uint32_t codepage, const char *tag,
	char *value, int len)
{
	LANG_NODE *plang;
	SINGLE_LIST_NODE *pnode;
	CODEPAGE_NODE *pdefault;
	CODEPAGE_NODE *pcodepage;
	
	pdefault = NULL;
	std::lock_guard rd_hold(g_list_lock);
	for (pnode=single_list_get_head(&g_cp_list); NULL!=pnode;
		pnode=single_list_get_after(&g_cp_list, pnode)) {
		pcodepage = (CODEPAGE_NODE*)pnode->pdata;
		if (NULL == pdefault) {
			pdefault = pcodepage;
		}
		if (codepage == pcodepage->codepage) {
			break;
		}
	}
	if (NULL == pnode) {
		pcodepage = pdefault;
	}
	for (pnode=single_list_get_head(&pcodepage->lang_list); NULL!=pnode;
		pnode=single_list_get_after(&pcodepage->lang_list, pnode)) {
		plang = (LANG_NODE*)pnode->pdata;
		if (0 == strcasecmp(plang->tag, tag)) {
			strncpy(value, plang->value, len);
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL svc_codepage_lang(int reason, void **ppdata)
{
	char *psearch, tmp_path[256], file_name[256];

	switch (reason) {
	case PLUGIN_INIT:
		LINK_API(ppdata);
		gx_strlcpy(file_name, get_plugin_name(), GX_ARRAY_SIZE(file_name));
		psearch = strrchr(file_name, '.');
		if (psearch != nullptr)
			*psearch = '\0';
		snprintf(tmp_path, GX_ARRAY_SIZE(tmp_path), "%s.txt", file_name);
		if (codepage_lang_run(tmp_path, get_data_path()) != 0) {
			printf("[codepage_lang]: failed to run the module\n");
			return false;
		}
		if (!register_service("get_lang", codepage_lang_get_lang)) {
			printf("[codepage_lang]: failed to register \"get_lang\" service\n");
			return false;
		}
		return TRUE;
	case PLUGIN_FREE:
		codepage_lang_stop();
		return TRUE;
	}
	return TRUE;
}
SVC_ENTRY(svc_codepage_lang);
