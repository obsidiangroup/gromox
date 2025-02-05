#pragma once
#include <gromox/double_list.hpp>
#include <pthread.h>

struct CONNECTION {
	DOUBLE_LIST_NODE node;
	int sockd;
	BOOL is_selecting;
	pthread_t thr_id;
};

using MIDB_CMD_HANDLER = int (*)(int argc, char **argv, int sockd);

extern void cmd_parser_init(size_t threads_num, int timeout, unsigned int debug);
extern int cmd_parser_run();
extern void cmd_parser_stop();
extern void cmd_parser_free();
extern CONNECTION *cmd_parser_get_connection();
void cmd_parser_put_connection(CONNECTION *pconnection);
extern void cmd_parser_register_command(const char *command, MIDB_CMD_HANDLER);
extern void cmd_write(int fd, const void *buf, size_t size);
