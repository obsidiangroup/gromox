// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#define DECLARE_API_STATIC
#include <atomic>
#include <csignal>
#include <cstdint>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/socket.h>
#include <gromox/svc_common.h>
#include <gromox/double_list.hpp>
#include <gromox/config_file.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <csignal>
#include <pthread.h>
#include <sys/types.h>  
#include <sys/socket.h>
#include <poll.h>


#define SOCKET_TIMEOUT          60

#define MAX_CMD_LENGTH			64*1024

namespace {

struct BACK_CONN {
    DOUBLE_LIST_NODE node;
	pthread_t thr_id;
    int sockd;
};

}

typedef void (*EVENT_STUB_FUNC)(char *);

static std::atomic<bool> g_notify_stop{false};
static char g_event_ip[40];
static uint16_t g_event_port;
static DOUBLE_LIST g_back_list;
static EVENT_STUB_FUNC g_event_stub_func;

static void *evst_thrwork(void *);
static int read_line(int sockd, char *buff, int length);
static int connect_event();
static void install_event_stub(EVENT_STUB_FUNC event_stub_func);

static BOOL svc_event_stub(int reason, void **ppdata)
{
	int i, conn_num;
    BACK_CONN *pback;
	char file_name[256];
	char config_path[256], *psearch;
	DOUBLE_LIST_NODE *pnode;
	
	switch(reason) {
	case PLUGIN_INIT: {
		LINK_API(ppdata);
		g_notify_stop = true;
		g_event_stub_func = NULL;
		double_list_init(&g_back_list);
		gx_strlcpy(file_name, get_plugin_name(), GX_ARRAY_SIZE(file_name));
		psearch = strrchr(file_name, '.');
		if (NULL != psearch) {
			*psearch = '\0';
		}
		snprintf(config_path, GX_ARRAY_SIZE(config_path), "%s.cfg", file_name);
		auto pfile = config_file_initd(config_path, get_config_path());
		if (NULL == pfile) {
			printf("[event_stub]: config_file_initd %s: %s\n",
			       config_path, strerror(errno));
			return FALSE;
		}

		auto str_value = pfile->get_value("CONNECTION_NUM");
		if (NULL == str_value) {
			conn_num = 8;
		} else {
			conn_num = atoi(str_value);
			if (conn_num < 0)
				conn_num = 8;
		}
		printf("[event_stub]: event connection number is %d\n", conn_num);

		str_value = pfile->get_value("EVENT_HOST");
		gx_strlcpy(g_event_ip, str_value != nullptr ? str_value : "::1",
		           GX_ARRAY_SIZE(g_event_ip));
		str_value = pfile->get_value("EVENT_PORT");
		if (NULL == str_value) {
			g_event_port = 33333;
		} else {
			g_event_port = strtoul(str_value, nullptr, 0);
			if (g_event_port == 0)
				g_event_port = 33333;
		}
		printf("[event_stub]: event address is [%s]:%hu\n",
		       *g_event_ip == '\0' ? "*" : g_event_ip, g_event_port);

		g_notify_stop = false;
		int ret = 0;
		for (i=0; i<conn_num; i++) {
			pback = (BACK_CONN*)malloc(sizeof(BACK_CONN));
			if (NULL != pback) {
		        pback->node.pdata = pback;
				pback->sockd = -1;
				ret = pthread_create(&pback->thr_id, nullptr, evst_thrwork, pback);
				if (ret != 0) {
					free(pback);
					break;
				}
				char buf[32];
				snprintf(buf, sizeof(buf), "event_stub/%u", i);
				pthread_setname_np(pback->thr_id, buf);
				double_list_append_as_tail(&g_back_list, &pback->node);
			}
		}

		if (i < conn_num) {
			g_notify_stop = true;
			while ((pnode = double_list_pop_front(&g_back_list)) != nullptr) {
				pback = (BACK_CONN*)pnode->pdata;
				if (-1 != pback->sockd) {
					close(pback->sockd);
					pback->sockd = -1;
				}
				pthread_kill(pback->thr_id, SIGALRM);
				pthread_join(pback->thr_id, NULL);
				free(pback);
			}
			double_list_free(&g_back_list);
			printf("[event_stub]: failed to create stub thread: %s\n", strerror(ret));
			return FALSE;
		}

		if (!register_service("install_event_stub", install_event_stub))
			printf("[event_stub]: failed to register install_event_stub\n");
		return TRUE;
	}
	case PLUGIN_FREE:
		if (!g_notify_stop) {
			g_notify_stop = true;
			while ((pnode = double_list_pop_front(&g_back_list)) != nullptr) {
				pback = static_cast<BACK_CONN *>(pnode->pdata);
				pthread_kill(pback->thr_id, SIGALRM);
				pthread_join(pback->thr_id, nullptr);
			}
		}
		double_list_free(&g_back_list);
		g_event_stub_func = NULL;
		return TRUE;
	}
	return TRUE;
}
SVC_ENTRY(svc_event_stub);

static int read_line(int sockd, char *buff, int length)
{
	int offset;
	int tv_usec;
	int read_len;
	struct pollfd pfd_read;

	offset = 0;
	while (1) {
		tv_usec = SOCKET_TIMEOUT * 1000000;
		pfd_read.fd = sockd;
		pfd_read.events = POLLIN|POLLPRI;
		if (1 != poll(&pfd_read, 1, tv_usec)) {
			return -1;
		}
		read_len = read(sockd, buff + offset, length - offset);
		if (read_len <= 0) {
			return -1;
		}
		offset += read_len;
		if (offset >= 2 &&
			'\r' == buff[offset - 2] && '\n' == buff[offset - 1]) {
			buff[offset - 2] = '\0';
			return 0;
		}
		if (length == offset) {
			return -1;
		}
	}
}


static int connect_event()
{
	int temp_len;
    char temp_buff[1024];
	int sockd = gx_inet_connect(g_event_ip, g_event_port, 0);
	if (sockd < 0) {
		fprintf(stderr, "gx_inet_connect event_stub@[%s]:%hu: %s\n",
		        g_event_ip, g_event_port, strerror(-sockd));
		return -1;
	}
	if (-1 == read_line(sockd, temp_buff, 1024) ||
		0 != strcasecmp(temp_buff, "OK")) {
        close(sockd);
        return -1;
	}
	
	temp_len = gx_snprintf(temp_buff, GX_ARRAY_SIZE(temp_buff), "LISTEN %s:%d\r\n",
				get_host_ID(), getpid());
	if (temp_len != write(sockd, temp_buff, temp_len)) {
		close(sockd);
		return -1;
	}

	if (-1 == read_line(sockd, temp_buff, 1024) ||
		0 != strcasecmp(temp_buff, "TRUE")) {
		close(sockd);
		return -1;
	}

	return sockd;
}

static void *evst_thrwork(void *param)
{
	BACK_CONN *pback;
	char buff[MAX_CMD_LENGTH];	
	
	pback = (BACK_CONN*)param;

	while (!g_notify_stop) {
		if (-1 == (pback->sockd = connect_event())) {
			sleep(3);
			continue;
		}

		while (!g_notify_stop) {
			if (-1 == read_line(pback->sockd, buff, MAX_CMD_LENGTH)) {
				close(pback->sockd);
				pback->sockd = -1;
				break;
			}
		
			if (0 == strcasecmp(buff, "PING")) {
				write(pback->sockd, "TRUE\r\n", 6);
				continue;
			}

			if (NULL != g_event_stub_func) {
				g_event_stub_func(buff);
			}
			
			write(pback->sockd, "TRUE\r\n", 6);
		}
	}
	
	if (-1 != pback->sockd) {
		close(pback->sockd);
		pback->sockd = -1;
	}
	return nullptr;
}


static void install_event_stub(EVENT_STUB_FUNC event_stub_func)
{
	if (NULL == g_event_stub_func) {
		g_event_stub_func = event_stub_func;
	}
}

