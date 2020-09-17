#pragma once
#include "common_types.h"

void data_source_init(const char *host, int port, const char *user,
	const char *password, const char *db_name);
BOOL data_source_get_datadir(char *path_buff);
extern void *data_source_lock_flush(void);
void data_source_unlock(void *pmysql);
