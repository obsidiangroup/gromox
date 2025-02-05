// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
/* pop3 parser is a module, which first read data from socket, parses the pop3 
 * commands and then do the corresponding action. 
 */ 
#include <cerrno>
#include <mutex>
#include <unistd.h>
#include <vector>
#include <libHX/string.h>
#include <gromox/defs.h>
#include "pop3_parser.h"
#include "pop3_cmd_handler.h"
#include "blocks_allocator.h"
#include <gromox/threads_pool.hpp>
#include "system_services.h"
#include "resource.h"
#include <gromox/lib_buffer.hpp>
#include <gromox/util.hpp>
#include <gromox/mail_func.hpp>
#include <pthread.h>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <openssl/err.h>
#if (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2090000fL) || \
    (defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER < 0x1010000fL)
#	define OLD_SSL 1
#endif
#define CALCULATE_INTERVAL(a, b) \
    (((a).tv_usec >= (b).tv_usec) ? ((a).tv_sec - (b).tv_sec) : \
    ((a).tv_sec - (b).tv_sec - 1))


#define SLEEP_BEFORE_CLOSE    usleep(1000)

using namespace gromox;

static int pop3_parser_dispatch_cmd(const char *cmd_line, int line_length, 
    POP3_CONTEXT *pcontext);

static void pop3_parser_context_clear(POP3_CONTEXT *pcontext);

static size_t g_context_num, g_retrieving_size;
static unsigned int g_timeout;
static int g_max_auth_times;
static int g_block_auth_fail;
static int g_ssl_port;
static std::vector<POP3_CONTEXT> g_context_list;
static std::vector<SCHEDULE_CONTEXT *> g_context_list2;
static BOOL g_support_stls;
static BOOL g_force_stls;
static char g_cdn_path[256];
static char g_certificate_path[256];
static char g_private_key_path[256];
static char g_certificate_passwd[1024];
static SSL_CTX *g_ssl_ctx;
static std::unique_ptr<std::mutex[]> g_ssl_mutex_buf;

void pop3_parser_init(int context_num, size_t retrieving_size, int timeout,
	int max_auth_times, int block_auth_fail, BOOL support_stls, BOOL force_stls,
	const char *certificate_path, const char *cb_passwd, const char *key_path,
	const char *cdn_path)
{
    g_context_num           = context_num;
	g_retrieving_size       = retrieving_size;
    g_timeout               = timeout;
	g_max_auth_times        = max_auth_times;
	g_block_auth_fail       = block_auth_fail;
	g_support_stls          = support_stls;
	g_ssl_mutex_buf         = NULL;
	if (TRUE == support_stls) {
		g_force_stls = force_stls;
		gx_strlcpy(g_certificate_path, certificate_path, GX_ARRAY_SIZE(g_certificate_path));
		if (NULL != cb_passwd) {
			gx_strlcpy(g_certificate_passwd, cb_passwd, GX_ARRAY_SIZE(g_certificate_passwd));
		} else {
			g_certificate_passwd[0] = '\0';
		}
		gx_strlcpy(g_private_key_path, key_path, GX_ARRAY_SIZE(g_private_key_path));
	}
	gx_strlcpy(g_cdn_path, cdn_path, GX_ARRAY_SIZE(g_cdn_path));
}

#ifdef OLD_SSL
static void pop3_parser_ssl_locking(int mode,
	int n, const char *file, int line)
{
	if (mode & CRYPTO_LOCK)
		g_ssl_mutex_buf[n].lock();
	else
		g_ssl_mutex_buf[n].unlock();
}

static void pop3_parser_ssl_id(CRYPTO_THREADID* id)
{
	CRYPTO_THREADID_set_numeric(id, (uintptr_t)pthread_self());
}
#endif

/* 
 *    @return
 *         0    success
 *        <>0    fail    
 */
int pop3_parser_run()
{
	if (TRUE == g_support_stls) {
		SSL_library_init();
		OpenSSL_add_all_algorithms();
		SSL_load_error_strings();
		g_ssl_ctx = SSL_CTX_new(SSLv23_server_method());
		if (NULL == g_ssl_ctx) {
			printf("[pop3_parser]: Failed to init SSL context\n");
			return -1;
		}
		
		if ('\0' != g_certificate_passwd[0]) {
			SSL_CTX_set_default_passwd_cb_userdata(g_ssl_ctx,
				g_certificate_passwd);
		}
		

		if (SSL_CTX_use_certificate_chain_file(g_ssl_ctx,
			g_certificate_path) <= 0) {
			printf("[pop3_parser]: fail to use certificate file:");
			ERR_print_errors_fp(stdout);
			return -2;
		}
		
		if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, g_private_key_path,
			SSL_FILETYPE_PEM) <= 0) {
			printf("[pop3_parser]: fail to use private key file:");
			ERR_print_errors_fp(stdout);
			return -3;
		}
		
		if (1 != SSL_CTX_check_private_key(g_ssl_ctx)) {
			printf("[pop3_parser]: private key does not match certificate:");
			ERR_print_errors_fp(stdout);
			return -4;
		}

		try {
			g_ssl_mutex_buf = std::make_unique<std::mutex[]>(CRYPTO_num_locks());
		} catch (const std::bad_alloc &) {
			printf("[pop3_parser]: Failed to allocate SSL locking buffer\n");
			return -5;
		}
#ifdef OLD_SSL
		CRYPTO_THREADID_set_callback(pop3_parser_ssl_id);
		CRYPTO_set_locking_callback(pop3_parser_ssl_locking);
#endif
	}

	try {
		g_context_list.resize(g_context_num);
		g_context_list2.resize(g_context_num);
		for (size_t i = 0; i < g_context_num; ++i) {
			g_context_list[i].context_id = i;
			g_context_list2[i] = &g_context_list[i];
		}
	} catch (const std::bad_alloc &) {
		printf("[pop3_parser]: Failed to allocate POP3 contexts\n");
        return -4;
    }
	if (!resource_get_integer("LISTEN_SSL_PORT", &g_ssl_port))
		g_ssl_port = 0;
    return 0;
}

void pop3_parser_stop()
{
	g_context_list2.clear();
	g_context_list.clear();
	if (TRUE == g_support_stls && NULL != g_ssl_ctx) {
		SSL_CTX_free(g_ssl_ctx);
		g_ssl_ctx = NULL;
	}

	if (TRUE == g_support_stls && NULL != g_ssl_mutex_buf) {
		CRYPTO_set_id_callback(NULL);
		CRYPTO_set_locking_callback(NULL);
		g_ssl_mutex_buf.reset();
	}
}

void pop3_parser_free()
{
    g_context_num		= 0;
	g_retrieving_size	= 0;
    g_timeout           = 0x7FFFFFFF;
	g_block_auth_fail   = 0;
}

int pop3_parser_threads_event_proc(int action)
{
    return 0;
}

int pop3_parser_get_context_socket(SCHEDULE_CONTEXT *ctx)
{
	return static_cast<POP3_CONTEXT *>(ctx)->connection.sockd;
}

struct timeval pop3_parser_get_context_timestamp(SCHEDULE_CONTEXT *ctx)
{
	return static_cast<POP3_CONTEXT *>(ctx)->connection.last_timestamp;
}

int pop3_parser_process(POP3_CONTEXT *pcontext)
{
	int len;
	unsigned int tmp_len;
	int read_len;
	int ssl_errno;
	int written_len;
	const char *host_ID;
	char temp_command[1024];
	char reply_buf[1024];
    struct timeval current_time;
	size_t ub, string_length = 0;
	
	if (TRUE == pcontext->is_stls) {
		if (NULL == pcontext->connection.ssl) {
			pcontext->connection.ssl = SSL_new(g_ssl_ctx);
			if (NULL == pcontext->connection.ssl) {
				auto pop3_reply_str = resource_get_pop3_code(1723, 1, &string_length);
				write(pcontext->connection.sockd, pop3_reply_str, string_length);
				pop3_parser_log_info(pcontext, LV_WARN, "out of memory for TLS object");
				SLEEP_BEFORE_CLOSE;
				close(pcontext->connection.sockd);
				if (system_services_container_remove_ip != nullptr)
					system_services_container_remove_ip(pcontext->connection.client_ip);
				pop3_parser_context_clear(pcontext);
				return PROCESS_CLOSE;
			}
			SSL_set_fd(pcontext->connection.ssl, pcontext->connection.sockd);
		}
		
		if (-1 == SSL_accept(pcontext->connection.ssl)) {
			ssl_errno = SSL_get_error(pcontext->connection.ssl, -1);
			if (SSL_ERROR_WANT_READ == ssl_errno ||
				SSL_ERROR_WANT_WRITE == ssl_errno) {
				gettimeofday(&current_time, NULL);
				if (CALCULATE_INTERVAL(current_time,
					pcontext->connection.last_timestamp) < g_timeout) {
					return PROCESS_POLLING_RDONLY;
				}
				auto pop3_reply_str = resource_get_pop3_code(1701, 1, &string_length);
				write(pcontext->connection.sockd, pop3_reply_str, string_length);
				pop3_parser_log_info(pcontext, LV_DEBUG, "time out");
				SLEEP_BEFORE_CLOSE;
			}
			SSL_free(pcontext->connection.ssl);
			pcontext->connection.ssl = NULL;
			close(pcontext->connection.sockd);
			if (system_services_container_remove_ip != nullptr)
				system_services_container_remove_ip(pcontext->connection.client_ip);
			pop3_parser_context_clear(pcontext);
			return PROCESS_CLOSE;
		} else {
			pcontext->is_stls = FALSE;
			if (pcontext->connection.server_port == g_ssl_port) {
				/* +OK <domain> Service ready */
				auto pop3_reply_str = resource_get_pop3_code(1711, 1, &string_length);
				auto pop3_reply_str2 = resource_get_pop3_code(1711, 2, &string_length);
				host_ID = resource_get_string("HOST_ID");
				len = sprintf(reply_buf, "%s%s%s", pop3_reply_str, host_ID,
						      pop3_reply_str2);
				SSL_write(pcontext->connection.ssl, reply_buf, len);
			}
		}
	}
	
	if (TRUE == pcontext->data_stat) {
		if (NULL != pcontext->connection.ssl) {
			written_len = SSL_write(pcontext->connection.ssl,
							pcontext->write_buff + pcontext->write_offset,
							pcontext->write_length - pcontext->write_offset);
		} else {
			written_len = write(pcontext->connection.sockd,
							pcontext->write_buff + pcontext->write_offset,
							pcontext->write_length - pcontext->write_offset);
		}
			
		gettimeofday(&current_time, NULL);
			
		if (0 == written_len) {
			pop3_parser_log_info(pcontext, LV_DEBUG, "connection lost");
			goto END_TRANSPORT;
		} else if (written_len < 0) {
			if (EAGAIN != errno) {
				pop3_parser_log_info(pcontext, LV_DEBUG, "connection lost");
				goto END_TRANSPORT;
			}
			/* check if context is timed out */
			if (CALCULATE_INTERVAL(current_time,
				pcontext->connection.last_timestamp) >= g_timeout) {
				pop3_parser_log_info(pcontext, LV_DEBUG, "time out");
				goto END_TRANSPORT;
			} else {
				return PROCESS_POLLING_WRONLY;
			}
		}
		pcontext->connection.last_timestamp = current_time;	
		pcontext->write_offset += written_len;

		if (pcontext->write_offset < pcontext->write_length) {
			return PROCESS_CONTINUE;
		}
		pcontext->write_offset = 0;
		tmp_len = MAX_LINE_LENGTH;
		pcontext->write_buff = static_cast<char *>(stream_getbuffer_for_reading(
		                       &pcontext->stream, &tmp_len));
		pcontext->write_length = tmp_len;
		if (NULL == pcontext->write_buff) {
			stream_clear(&pcontext->stream);
			switch (pop3_parser_retrieve(pcontext)) {
			case POP3_RETRIEVE_TERM:
				pcontext->data_stat = FALSE;
				return PROCESS_CONTINUE;
			case POP3_RETRIEVE_ERROR:
				goto ERROR_TRANSPROT;
			}
		}
		return PROCESS_CONTINUE;
	}

	if (TRUE == pcontext->list_stat) {
		if (NULL != pcontext->connection.ssl) {
			written_len = SSL_write(pcontext->connection.ssl,
							pcontext->write_buff + pcontext->write_offset,
							pcontext->write_length - pcontext->write_offset);
		} else {
			written_len = write(pcontext->connection.sockd,
							pcontext->write_buff + pcontext->write_offset,
							pcontext->write_length - pcontext->write_offset);
		}
			
		gettimeofday(&current_time, NULL);
			
		if (0 == written_len) {
			pop3_parser_log_info(pcontext, LV_DEBUG, "connection lost");
			goto END_TRANSPORT;
		} else if (written_len < 0) {
			if (EAGAIN != errno) {
				pop3_parser_log_info(pcontext, LV_DEBUG, "connection lost");
				goto END_TRANSPORT;
			}
			/* check if context is timed out */
			if (CALCULATE_INTERVAL(current_time,
				pcontext->connection.last_timestamp) >= g_timeout) {
				pop3_parser_log_info(pcontext, LV_DEBUG, "time out");
				goto END_TRANSPORT;
			} else {
				return PROCESS_POLLING_WRONLY;
			}
		}
		pcontext->connection.last_timestamp = current_time;	
		pcontext->write_offset += written_len;

		if (pcontext->write_offset < pcontext->write_length) {
			return PROCESS_CONTINUE;
		}

		pcontext->write_offset = 0;
		tmp_len = MAX_LINE_LENGTH;
		pcontext->write_buff = static_cast<char *>(stream_getbuffer_for_reading(&pcontext->stream, &tmp_len));
		pcontext->write_length = tmp_len;
		if (NULL == pcontext->write_buff) {
			stream_clear(&pcontext->stream);
			pcontext->write_length = 0;
			pcontext->write_offset = 0;
			pcontext->list_stat = FALSE;
		}
		return PROCESS_CONTINUE;
	}

	if (NULL != pcontext->connection.ssl) {
		read_len = SSL_read(pcontext->connection.ssl, pcontext->read_buffer +
					pcontext->read_offset, 1024 - pcontext->read_offset);
	} else {
		read_len = read(pcontext->connection.sockd, pcontext->read_buffer +
					pcontext->read_offset, 1024 - pcontext->read_offset);
	}
	gettimeofday(&current_time, NULL);
	if (0 == read_len) {
 LOST_READ:
		if (NULL != pcontext->connection.ssl) {
			SSL_shutdown(pcontext->connection.ssl);
			SSL_free(pcontext->connection.ssl);
			pcontext->connection.ssl = NULL;
		}
		pop3_parser_log_info(pcontext, LV_DEBUG, "connection lost");
		close(pcontext->connection.sockd);
		if (system_services_container_remove_ip != nullptr)
			system_services_container_remove_ip(pcontext->connection.client_ip);
		pop3_parser_context_clear(pcontext);
		return PROCESS_CLOSE;
	} else if (read_len < 0) {
		if (EAGAIN != errno) {
			goto LOST_READ;
		}
		/* check if context is timed out */
		if (CALCULATE_INTERVAL(current_time,
			pcontext->connection.last_timestamp) >= g_timeout) {
			auto pop3_reply_str = resource_get_pop3_code(1701, 1, &string_length);
			if (NULL != pcontext->connection.ssl) {
				SSL_write(pcontext->connection.ssl, pop3_reply_str, string_length);
			} else {
				write(pcontext->connection.sockd, pop3_reply_str, string_length);
			}
			pop3_parser_log_info(pcontext, LV_DEBUG, "time out");
			if (NULL != pcontext->connection.ssl) {
				SSL_shutdown(pcontext->connection.ssl);
				SSL_free(pcontext->connection.ssl);
				pcontext->connection.ssl = NULL;
			}
			SLEEP_BEFORE_CLOSE;
			close(pcontext->connection.sockd);
			if (system_services_container_remove_ip != nullptr)
				system_services_container_remove_ip(pcontext->connection.client_ip);
			pop3_parser_context_clear(pcontext);
			return PROCESS_CLOSE;
		} else {
			return PROCESS_POLLING_RDONLY;
		}
	}
	
	pcontext->connection.last_timestamp = current_time;	
	pcontext->read_offset += read_len;
	ub = pcontext->read_offset > 0 ? pcontext->read_offset - 1 : 0;
	for (size_t i = 0; i < ub; ++i) {
		if ('\r' == pcontext->read_buffer[i] &&
			'\n' == pcontext->read_buffer[i + 1]) {
			memcpy(temp_command, pcontext->read_buffer, i);
			temp_command[i] = '\0';
			HX_strrtrim(temp_command);
			HX_strltrim(temp_command);
			pcontext->read_offset -= i + 2;
			memmove(pcontext->read_buffer, pcontext->read_buffer + i + 2,
				pcontext->read_offset);
			switch (pop3_parser_dispatch_cmd(temp_command,
				strlen(temp_command), pcontext)) {
			case DISPATCH_CONTINUE:
				return PROCESS_CONTINUE;
			case DISPATCH_SHOULD_CLOSE:
				if (NULL != pcontext->connection.ssl) {
					SSL_shutdown(pcontext->connection.ssl);
					SSL_free(pcontext->connection.ssl);
					pcontext->connection.ssl = NULL;
				}
				SLEEP_BEFORE_CLOSE;
				close(pcontext->connection.sockd);
				if (system_services_container_remove_ip != nullptr)
					system_services_container_remove_ip(pcontext->connection.client_ip);
				pop3_parser_context_clear(pcontext);
				return PROCESS_CLOSE;
			case DISPATCH_DATA:
				pcontext->data_stat = TRUE;
				return PROCESS_CONTINUE;
			case DISPATCH_LIST:
				pcontext->list_stat = TRUE;
				return PROCESS_CONTINUE;
			}
		}
	}
	if (1024 == pcontext->read_offset) {
		pcontext->read_offset = 0;
		auto pop3_reply_str = resource_get_pop3_code(1702, 1, &string_length);
		if (NULL != pcontext->connection.ssl) {
			SSL_write(pcontext->connection.ssl, pop3_reply_str, string_length);
		} else {
			write(pcontext->connection.sockd, pop3_reply_str, string_length);
		}
	}
	return PROCESS_CONTINUE;
	
 ERROR_TRANSPROT:
	if (NULL != pcontext->connection.ssl) {
		SSL_write(pcontext->connection.ssl, "\r\n.\r\n", 5);
	} else {
		write(pcontext->connection.sockd, "\r\n.\r\n", 5);
	}
	if (pcontext->message_fd != -1) {
		close(pcontext->message_fd);
		pcontext->message_fd = -1;
	}
	stream_clear(&pcontext->stream);
	pcontext->write_length = 0;
	pcontext->write_offset = 0;
	pcontext->data_stat = FALSE;
	return PROCESS_CONTINUE;

 END_TRANSPORT:
	if (pcontext->message_fd != -1) {
		close(pcontext->message_fd);
		pcontext->message_fd = -1;
	}
	if (NULL != pcontext->connection.ssl) {
		SSL_shutdown(pcontext->connection.ssl);
		SSL_free(pcontext->connection.ssl);
		pcontext->connection.ssl = NULL;
	}
	close(pcontext->connection.sockd);
	if (system_services_container_remove_ip != nullptr)
		system_services_container_remove_ip(pcontext->connection.client_ip);
	pop3_parser_context_clear(pcontext);
	return PROCESS_CLOSE;

}

int pop3_parser_retrieve(POP3_CONTEXT *pcontext)
{
	unsigned int size, tmp_len, line_length;
	int read_len;
	int copy_result;
	int last_result;
	BOOL b_stop;
	STREAM temp_stream;
	char line_buff[MAX_LINE_LENGTH + 3];
	
	pcontext->write_length = 0;
	pcontext->write_offset = 0;
		
	if (-1 == pcontext->message_fd) {
		return POP3_RETRIEVE_TERM;
	}

	stream_init(&temp_stream, blocks_allocator_get_allocator());
	while (stream_get_total_length(&temp_stream) < g_retrieving_size) {
		size = STREAM_BLOCK_SIZE;
		void *pbuff = stream_getbuffer_for_writing(&temp_stream, &size);
		if (NULL == pbuff) {
			pop3_parser_log_info(pcontext, LV_WARN, "out of memory");
			stream_free(&temp_stream);
			return POP3_RETRIEVE_ERROR;
		}
		read_len = read(pcontext->message_fd, pbuff, size);
		if (read_len < 0) {
			pop3_parser_log_info(pcontext, LV_WARN, "failed to read message file");
			stream_free(&temp_stream);
			return POP3_RETRIEVE_ERROR;
		} else if (0 == read_len) {
			close(pcontext->message_fd);
			pcontext->message_fd = -1;
			break;
		} else {
			stream_forward_writing_ptr(&temp_stream, read_len);
		}
	}
	b_stop = FALSE;
	last_result = STREAM_COPY_OK;
	while (FALSE == b_stop) {
		line_length = MAX_LINE_LENGTH;
		copy_result = stream_copyline(&temp_stream, line_buff, &line_length);
		switch (copy_result) {
		case STREAM_COPY_END:
			if (-1 == pcontext->message_fd) {
				stream_write(&pcontext->stream, ".\r\n", 3);
			}
			b_stop = TRUE;
			break;
		case STREAM_COPY_TERM:
			stream_write(&pcontext->stream, line_buff, line_length);
			if (-1 == pcontext->message_fd) {
				stream_write(&pcontext->stream, "\r\n.\r\n", 5);
			}
			b_stop = TRUE;
			break;
		case STREAM_COPY_OK:
		case STREAM_COPY_PART:
			if (pcontext->cur_line < 0 && 0 == line_length) {
				pcontext->cur_line = 0;
			}
			if ('.' == line_buff[0] && STREAM_COPY_OK == last_result) {
				memmove(line_buff + 1, line_buff, line_length);
				line_length ++;
			}
			if (STREAM_COPY_OK == copy_result) {
				memcpy(line_buff + line_length, "\r\n", 2);
				line_length += 2;
			}
			stream_write(&pcontext->stream, line_buff, line_length);
			
			if (STREAM_COPY_OK == copy_result && pcontext->cur_line >= 0) {
				if (pcontext->until_line == pcontext->cur_line) {
					stream_write(&pcontext->stream, ".\r\n", 3);
					if (-1 != pcontext->message_fd) {
						close(pcontext->message_fd);
						pcontext->message_fd = -1;
					}
					b_stop = TRUE;
					break;
				} else {
					pcontext->cur_line ++;
				}
			}
			break;
		}
		last_result = copy_result;
	}
	stream_free(&temp_stream);
	tmp_len = STREAM_BLOCK_SIZE;
	pcontext->write_buff = static_cast<char *>(stream_getbuffer_for_reading(
	                       &pcontext->stream, &tmp_len));
	pcontext->write_length = tmp_len;
	if (NULL == pcontext->write_buff) {
		pop3_parser_log_info(pcontext, LV_WARN, "error on stream object");
		stream_clear(&pcontext->stream);
		return POP3_RETRIEVE_ERROR;
	}
	return POP3_RETRIEVE_OK;
}

int pop3_parser_get_param(int param)
{
    switch (param) {
    case MAX_AUTH_TIMES:
        return g_max_auth_times;
    case BLOCK_AUTH_FAIL:
        return g_block_auth_fail;
    case POP3_SESSION_TIMEOUT:
        return g_timeout;
	case POP3_SUPPORT_STLS:
		return g_support_stls;
	case POP3_FORCE_STLS:
		return g_force_stls;
    default:
        return 0;
    }
}

/* 
 *    get contexts list for contexts pool
 *    @return
 *        contexts array's address
 */
SCHEDULE_CONTEXT **pop3_parser_get_contexts_list()
{
	return g_context_list2.data();
}

int pop3_parser_set_param(int param, int value)
{
    switch (param) {
    case MAX_AUTH_TIMES:
        g_max_auth_times = value;
        break;
    case POP3_SESSION_TIMEOUT:
        g_timeout = value;
        break;
	case BLOCK_AUTH_FAIL:
		g_block_auth_fail = value;
		break;
	case POP3_FORCE_STLS:
		if (TRUE == g_support_stls) {
			g_force_stls = value;
		}
		break;
    default:
        return -1;
    }
    return 0;
}

char* pop3_parser_cdn_path()
{
	return g_cdn_path;
}

/* 
 *    dispatch the pop3 command to the corresponding procedure
 *    @param
 *        cmd_line [in]        command string
 *        line_length            length of command line
 *        pcontext [in, out]    context object
 *     @return
 *         DISPATCH_CONTINUE        continue to dispatch command
 *         DISPATCH_SHOULD_CLOSE    quit command is read
 *         DISPATCH_DATA            data command is met
 *         DISPATCH_LIST			need to respond list
 */
static int pop3_parser_dispatch_cmd2(const char *cmd_line, int line_length,
    POP3_CONTEXT *pcontext)
{
    if (0 == strncasecmp(cmd_line, "CAPA", 4)) {
        return pop3_cmd_handler_capa(cmd_line, line_length, pcontext);    
	} else if (0 == strncasecmp(cmd_line, "STLS", 4)) {
        return pop3_cmd_handler_stls(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "USER", 4)) {
        return pop3_cmd_handler_user(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "PASS", 4)) {
        return pop3_cmd_handler_pass(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "STAT", 4)) {
        return pop3_cmd_handler_stat(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "UIDL", 4)) {
        return pop3_cmd_handler_uidl(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "LIST", 4)) {
        return pop3_cmd_handler_list(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "RETR", 4)) {
        return pop3_cmd_handler_retr(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "RSET", 4)) {
        return pop3_cmd_handler_rset(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "NOOP", 4)) {
        return pop3_cmd_handler_noop(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "DELE", 4)) {
        return pop3_cmd_handler_dele(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "TOP", 3)) {
        return pop3_cmd_handler_top(cmd_line, line_length, pcontext);    
    } else if (0 == strncasecmp(cmd_line, "QUIT", 4)) {
        return pop3_cmd_handler_quit(cmd_line, line_length, pcontext);    
    } else {
        return pop3_cmd_handler_else(cmd_line, line_length, pcontext);    
    }
}

static int pop3_parser_dispatch_cmd(const char *line, int len, POP3_CONTEXT *ctx)
{
	auto ret = pop3_parser_dispatch_cmd2(line, len, ctx);
	auto code = ret & DISPATCH_VALMASK;
	if (code == 0)
		return ret & DISPATCH_ACTMASK;
	size_t zlen = 0;
	auto str = resource_get_pop3_code(code, 1, &zlen);
	if (ctx->connection.ssl != nullptr)
		SSL_write(ctx->connection.ssl, str, zlen);
	else
		write(ctx->connection.sockd, str, zlen);
	return ret & DISPATCH_ACTMASK;
}

POP3_CONTEXT::POP3_CONTEXT()
{
	auto pcontext = this;
	auto palloc_stream = blocks_allocator_get_allocator();
    pcontext->connection.sockd = -1;
    stream_init(&pcontext->stream, palloc_stream);
	single_list_init(&pcontext->list);
}

static void pop3_parser_context_clear(POP3_CONTEXT *pcontext)
{
    if (NULL == pcontext) {
        return;
    }
    memset(&pcontext->connection, 0, sizeof(CONNECTION));
    pcontext->connection.sockd = -1;
	pcontext->message_fd = -1;
	pcontext->array.clear();
	single_list_init(&pcontext->list);
	stream_clear(&pcontext->stream);
	memset(pcontext->read_buffer, 0, arsizeof(pcontext->read_buffer));
	pcontext->read_offset = 0;
	pcontext->write_buff = NULL;
	pcontext->write_length = 0;
	pcontext->write_offset = 0;
	pcontext->data_stat = 0;
	pcontext->list_stat = 0;
	pcontext->cur_line = -1;
	pcontext->until_line = 0x7FFFFFFF;
	pcontext->total_mail = 0;
	pcontext->total_size = 0;
	pcontext->is_login = 0;
	pcontext->is_stls = 0;
	pcontext->auth_times = 0;
	memset(pcontext->username, 0, GX_ARRAY_SIZE(pcontext->username));
	memset(pcontext->maildir, 0, arsizeof(pcontext->maildir));
}

POP3_CONTEXT::~POP3_CONTEXT()
{
	auto pcontext = this;
	pcontext->array.clear();
    stream_free(&pcontext->stream);
	if (NULL != pcontext->connection.ssl) {
		SSL_shutdown(pcontext->connection.ssl);
		SSL_free(pcontext->connection.ssl);
		pcontext->connection.ssl = NULL;
	}
	if (-1 != pcontext->connection.sockd) {
		close(pcontext->connection.sockd);
	}
	if (-1 != pcontext->message_fd) {
		close(pcontext->message_fd);
	}
}

void pop3_parser_log_info(POP3_CONTEXT *pcontext, int level, const char *format, ...)
{
	char log_buf[2048];
	va_list ap;

	va_start(ap, format);
	vsnprintf(log_buf, sizeof(log_buf) - 1, format, ap);
	va_end(ap);
	log_buf[sizeof(log_buf) - 1] = '\0';
	
	system_services_log_info(level, "user: %s, IP: %s  %s",
		pcontext->username, pcontext->connection.client_ip, log_buf);

}
