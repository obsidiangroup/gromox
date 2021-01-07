#pragma once
#include "common_types.h"
#include "mem_file.h"

#ifdef __cplusplus
extern "C" {
#endif

void mod_rewrite_init(const char *list_path);
extern int mod_rewrite_run(void);
extern void mod_rewrite_stop(void);
extern void mod_rewrite_free(void);
BOOL mod_rewrite_process(const char *uri_buff,
	int uri_len, MEM_FILE *pf_request_uri);

#ifdef __cplusplus
} /* extern "C" */
#endif