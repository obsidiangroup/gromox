// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include "cache_queue.h"
#include "exmdb_local.h"
#include "net_failure.h"
#include "bounce_audit.h"
#include "bounce_producer.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdlib>
#include <fcntl.h>
#include <cstdio>
#define MAX_CIRCLE_NUMBER   0x7FFFFFFF
#define DEF_MODE            S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH

static char g_path[256];
static int g_mess_id;
static int g_scan_interval;
static int g_retrying_times;
static pthread_t g_thread_id;
static std::mutex g_id_lock;
static std::atomic<bool> g_notify_stop{true};

static int cache_queue_retrieve_mess_ID();
static int cache_queue_increase_mess_ID();
static void *mdl_thrwork(void *);

/*
 *	@param
 *		path [in]				queue path
 *		scan_interval			interval of queue scanning
 *		retrying_times			retrying times of delivery
 */
void cache_queue_init(const char *path, int scan_interval, int retrying_times)
{
	gx_strlcpy(g_path, path, GX_ARRAY_SIZE(g_path));
	g_scan_interval = scan_interval;
	g_retrying_times = retrying_times;
	g_notify_stop = true;
}

/*
 *	@return
 *		 0				OK
 *		<>0				fail
 */
int cache_queue_run()
{
	pthread_attr_t  attr;
	struct stat node_stat;

	/* check the directory */
    if (0 != stat(g_path, &node_stat)) {
        printf("[exmdb_local]: can not find %s directory\n", g_path);
        return -1;
    }
    if (0 == S_ISDIR(node_stat.st_mode)) {
        printf("[exmdb_local]: %s is not a directory\n", g_path);
        return -2;
    }
	g_mess_id = cache_queue_retrieve_mess_ID();
	g_notify_stop = false;
	pthread_attr_init(&attr);
	auto ret = pthread_create(&g_thread_id, &attr, mdl_thrwork, nullptr);
	if (ret != 0) {
		pthread_attr_destroy(&attr);
		g_notify_stop = true;
		printf("[exmdb_local]: failed to create timer thread: %s\n", strerror(ret));
		return -3;
	}
	pthread_setname_np(g_thread_id, "cache_queue");
	pthread_attr_destroy(&attr);
	return 0;
}

void cache_queue_stop()
{
	if (!g_notify_stop) {
		g_notify_stop = true;
		pthread_kill(g_thread_id, SIGALRM);
		pthread_join(g_thread_id, NULL);
	}
}

void cache_queue_free()
{
	g_path[0] = '\0';
	g_scan_interval = 0;
	g_retrying_times = 0;
}

/*
 *	put message into timer queue
 *	@param
 *		pcontext [in]		message context to be sent
 *		rcpt_to [in]        rcpt address
 *		original_time		original time
 *	@return
 *		>=0					timer ID
 *		<0					fail
 */
int cache_queue_put(MESSAGE_CONTEXT *pcontext, const char *rcpt_to,
	time_t original_time)
{
	int mess_id, temp_len;
	char file_name[256];
	int fd, times;

	mess_id = cache_queue_increase_mess_ID();
	snprintf(file_name, GX_ARRAY_SIZE(file_name), "%s/%d", g_path, mess_id);
	fd = open(file_name, O_WRONLY|O_CREAT|O_TRUNC, DEF_MODE);
	if (-1 == fd) {
		return -1;
	}
	/* write 0 at the begin of file to indicate message is now being writing */
	times = 0;
	if (sizeof(int) != write(fd, &times, sizeof(int)) ||
		sizeof(time_t) != write(fd, &original_time, sizeof(time_t))) {
		close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1353: remove %s: %s\n", file_name, strerror(errno));
        return -1;
	}
	/* at the begin of file, write the length of message */
	int32_t len = std::max(mail_get_length(pcontext->pmail), static_cast<ssize_t>(INT32_MAX));
	if (len < 0) {
		printf("[exmdb_local]: fail to get mail length\n");
		close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1354: remove %s: %s\n", file_name, strerror(errno));
        return -1;
	}
	static_assert(sizeof(len) == sizeof(int32_t));
	if (write(fd, &len, sizeof(len)) == sizeof(len)) {
		close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1355: remove %s: %s\n", file_name, strerror(errno));
        return -1;
	}
	if (FALSE == mail_to_file(pcontext->pmail, fd) ||
		sizeof(int) != write(fd, &pcontext->pcontrol->queue_ID, sizeof(int)) ||
		sizeof(int) != write(fd, &pcontext->pcontrol->bound_type, sizeof(int))||
		sizeof(BOOL) != write(fd, &pcontext->pcontrol->is_spam, sizeof(BOOL))||
		sizeof(BOOL) != write(fd, &pcontext->pcontrol->need_bounce,
		sizeof(BOOL))) {
        close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1356: remove %s: %s\n", file_name, strerror(errno));
        return -1;
    }
	/* write envelope from */
    temp_len = strlen(pcontext->pcontrol->from);
    temp_len ++;
    if (temp_len != write(fd, pcontext->pcontrol->from, temp_len)) {
        close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1357: remove %s: %s\n", file_name, strerror(errno));
        return -1;
    }
	/* write envelope rcpt */
	temp_len = strlen(rcpt_to) + 1;
	if (temp_len != write(fd, rcpt_to, temp_len)) {
		close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1358: remove %s: %s\n", file_name, strerror(errno));
		return -1;
    }
    /* last null character for indicating end of rcpt to array */
    if (1 != write(fd, "", 1)) {
		close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1359: remove %s: %s\n", file_name, strerror(errno));
        return -1;
	}
	lseek(fd, 0, SEEK_SET);
	times = 1;
	if (sizeof(int) != write(fd, &times, sizeof(int))) {
		close(fd);
		if (remove(file_name) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1360: remove %s: %s\n", file_name, strerror(errno));
        return -1;
	}
	close(fd);
	return mess_id;
}

/*
 *	retrieve the message ID in the queue
 *	@return
 *		message ID
 */
static int cache_queue_retrieve_mess_ID()
{
	DIR *dirp;
    struct dirent *direntp;
    int max_ID = 0, temp_ID;

    /*
    read every file under directory and retrieve the maximum number and
    return it
    */
    dirp = opendir(g_path);
    while ((direntp = readdir(dirp)) != NULL) {
		if (strcmp(direntp->d_name, ".") == 0 ||
		    strcmp(direntp->d_name, "..") == 0)
			continue;
    	temp_ID = atoi(direntp->d_name);
        if (temp_ID > max_ID) {
            max_ID = temp_ID;
        }
    }
    closedir(dirp);
    return max_ID;
}

/*
 *	increase the message ID with 1
 *	@return
 *		message ID before increasement
 */
static int cache_queue_increase_mess_ID()
{
	int current_id;
	std::unique_lock hold(g_id_lock);
    if (MAX_CIRCLE_NUMBER == g_mess_id) {
        g_mess_id = 1;
    } else {
        g_mess_id ++;
    }
    current_id  = g_mess_id;
    return current_id;
}

static void *mdl_thrwork(void *arg)
{
	DIR *dirp;
	int i, times, size, bounce_type = 0, scan_interval, mess_len;
	time_t scan_begin, scan_end, original_time;
    struct dirent *direntp;
	char temp_from[UADDR_SIZE], temp_rcpt[UADDR_SIZE];
	char *ptr;
	MESSAGE_CONTEXT *pcontext, *pbounce_context;
	BOOL need_bounce, need_remove;

	pcontext = get_context();
	if (NULL == pcontext) {
		printf("[exmdb_local]: fail to get context in cache queue thread\n");
		return nullptr;
	}
	dirp = opendir(g_path);
	if (NULL == dirp) {
		printf("[exmdb_local]: failed to open cache directory %s: %s\n",
			g_path, strerror(errno));
		return nullptr;
	}
	i = 0;
	scan_interval = g_scan_interval;
	while (!g_notify_stop) {
		if (i < scan_interval) {
			i ++;
			sleep(1);
			continue;
		}
		seekdir(dirp, 0);
		time(&scan_begin);
    	while ((direntp = readdir(dirp)) != NULL) {
			if (g_notify_stop)
				break;
			if (strcmp(direntp->d_name, ".") == 0 ||
			    strcmp(direntp->d_name, "..") == 0)
				continue;
			std::string temp_path;
			int fd = -1;
			try {
				temp_path = std::string(g_path) + "/" + direntp->d_name;
				fd = open(temp_path.c_str(), O_RDWR);
			} catch (const std::bad_alloc &) {
				fprintf(stderr, "E-1475: ENOMEM\n");
			}
			if (-1 == fd) {
				continue;
			}
			struct stat node_stat;
			if (fstat(fd, &node_stat) != 0 || !S_ISREG(node_stat.st_mode)) {
				close(fd);
				continue;
			}
			if (sizeof(int) != read(fd, &times, sizeof(int))) {
				close(fd);
				continue;
			}
			if (0 == times) {
				close(fd);
                continue;
			}
			if (sizeof(time_t) != read(fd, &original_time, sizeof(time_t)) ||
				sizeof(int) != read(fd, &mess_len, sizeof(int))) {
				printf("[exmdb_local]: fail to read information from %s "
					"in timer queue\n", direntp->d_name);
				close(fd);
				continue;
			}
			size = node_stat.st_size - sizeof(time_t) - 2*sizeof(int);
			auto pbuff = static_cast<char *>(malloc(((size - 1) / (64 * 1024) + 1) * 64 * 1024));
			if (NULL == pbuff) {
				printf("[exmdb_local]: Failed to allocate memory for %s "
					"in timer queue thread\n", direntp->d_name);
				close(fd);
				continue;
			}
			if (size != read(fd, pbuff, size)) {
				free(pbuff);
				printf("[exmdb_local]: fail to read information from %s "
					"in timer queue\n", direntp->d_name);
				close(fd);
				continue;
			}
			if (FALSE == mail_retrieve(pcontext->pmail, pbuff, mess_len)) {
				free(pbuff);
				printf("[exmdb_local]: fail to retrieve message %s in "
					"cache queue into mail object\n", direntp->d_name);
				close(fd);
				continue;
			}
			ptr = pbuff + mess_len;
			pcontext->pcontrol->queue_ID = *(int*)ptr;
			ptr += sizeof(int);
			pcontext->pcontrol->bound_type = *(int*)ptr;
			ptr += sizeof(int);
			pcontext->pcontrol->is_spam = *(BOOL*)ptr;
			ptr += sizeof(BOOL);
			pcontext->pcontrol->need_bounce = *(BOOL*)ptr;
			ptr += sizeof(BOOL);
			gx_strlcpy(pcontext->pcontrol->from, ptr, GX_ARRAY_SIZE(pcontext->pcontrol->from));
			gx_strlcpy(temp_from, ptr, GX_ARRAY_SIZE(temp_from));
			ptr += strlen(pcontext->pcontrol->from) + 1;
			mem_file_clear(&pcontext->pcontrol->f_rcpt_to);
			mem_file_writeline(&pcontext->pcontrol->f_rcpt_to, ptr);
			gx_strlcpy(temp_rcpt, ptr, GX_ARRAY_SIZE(temp_rcpt));
			
			if (g_retrying_times <= times) {
				need_bounce = TRUE;
				need_remove = TRUE;
				bounce_type = BOUNCE_OPERATION_ERROR;
			} else {
				need_bounce = FALSE;
				need_remove = FALSE;
			}
			switch (exmdb_local_deliverquota(pcontext, ptr)) {
			case DELIVERY_OPERATION_OK:
				need_bounce = FALSE;
				need_remove = TRUE;
				net_failure_statistic(1, 0, 0, 0);
				break;
			case DELIVERY_OPERATION_DELIVERED:
				bounce_type = BOUNCE_MAIL_DELIVERED;
				need_bounce = TRUE;
				need_remove = TRUE;
				net_failure_statistic(1, 0, 0, 0);
				break;
			case DELIVERY_NO_USER:
			    bounce_type = BOUNCE_NO_USER;
			    need_bounce = TRUE;
				need_remove = TRUE;
				net_failure_statistic(0, 0, 0, 1);
				break;
			case DELIVERY_MAILBOX_FULL:
				bounce_type = BOUNCE_MAILBOX_FULL;
			    need_bounce = TRUE;
				need_remove = TRUE;
			    break;
			case DELIVERY_OPERATION_ERROR:
				bounce_type = BOUNCE_OPERATION_ERROR;
				need_bounce = TRUE;
				need_remove = TRUE;
				net_failure_statistic(0, 0, 1, 0);
				break;
			case DELIVERY_OPERATION_FAILURE:
				net_failure_statistic(0, 1, 0, 0);
				break;
			}
			if (FALSE == need_remove) {
				/* rewrite type and until time */
				lseek(fd, 0, SEEK_SET);
				times ++;
				if (sizeof(int) != write(fd, &times, sizeof(int))) {
					printf("[exmdb_local]: error while updating "
						"times\n");
				}
			}
			close(fd);
			if (need_remove && remove(temp_path.c_str()) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1432: remove %s: %s\n",
				        temp_path.c_str(), strerror(errno));
			need_bounce &= pcontext->pcontrol->need_bounce;
			
			if (TRUE == need_bounce && 0 != strcasecmp(
				pcontext->pcontrol->from, "none@none")) {
				pbounce_context = get_context();
				if (NULL == pbounce_context) {
					exmdb_local_log_info(pcontext, ptr, LV_ERR, "fail to get one "
						"context for bounce mail");
				} else {
					if (FALSE == bounce_audit_check(temp_rcpt)) {
						exmdb_local_log_info(pcontext, ptr, LV_ERR, "will not "
							"produce bounce message, because of too many "
							"mails to %s", temp_rcpt);
						put_context(pbounce_context);
					} else {
						bounce_producer_make(temp_from, temp_rcpt,
							pcontext->pmail, original_time, bounce_type,
							pbounce_context->pmail);
						sprintf(pbounce_context->pcontrol->from,
							"postmaster@%s", get_default_domain());
						mem_file_writeline(
							&pbounce_context->pcontrol->f_rcpt_to,
							pcontext->pcontrol->from);
						enqueue_context(pbounce_context);
					}
				}
			}
			free(pbuff);
		}
		time(&scan_end);
		if (scan_end - scan_begin >= g_scan_interval) {
			scan_interval = 0;
		} else {
			scan_interval = g_scan_interval - (scan_end - scan_begin);
		}
		i = 0;
	}
	closedir(dirp);
	return NULL;
}

int cache_queue_get_param(int param)
{
	if (CACHE_QUEUE_SCAN_INTERVAL == param) {
		return g_scan_interval;
	} else if (CACHE_QUEUE_RETRYING_TIMES == param) {
		return g_retrying_times;
	}
	return 0;
}

void cache_queue_set_param(int param, int val)
{
	if (CACHE_QUEUE_SCAN_INTERVAL == param) {
		g_scan_interval = val;
	} else if (CACHE_QUEUE_RETRYING_TIMES == param) {
		g_retrying_times = val;
	}
}
