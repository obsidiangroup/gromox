#pragma once
#include "common_types.h"

void data_source_init(const char *host, int port, const char *user,
	const char *password, const char *db_name);
extern int data_source_run(void);
extern int data_source_stop(void);
extern void data_source_free(void);
BOOL data_source_get_maildir(const char *username, char *path_buff);