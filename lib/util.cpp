// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
/*
 *	this file includes some utility functions that will be used by many 
 *	programs
 */
#include <cstdint>
#include <ctime>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/fileio.h>
#include <gromox/util.hpp>
#include <ctime>
#include <cstdio>
#include <crypt.h>
#include <iconv.h>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <unistd.h>
#include <sys/time.h>


static int utf8_byte_num(unsigned char ch)
{
	int byte_num = 0;

	if (ch >= 0xFC && ch < 0xFE) {
		byte_num = 6;
	} else if (ch >= 0xF8) {
		byte_num = 5;
	} else if (ch >= 0xF0) {
		byte_num = 4;
	} else if (ch >= 0xE0) {
		byte_num = 3;
	} else if (ch >= 0xC0) {
		byte_num = 2;
	} else if (0 == (ch & 0x80)) {
		byte_num = 1;
	}
	return byte_num;
}
 
/* check for invalid UTF-8 */
BOOL utf8_check(const char *str)
{
	int byte_num = 0;
	unsigned char ch;
	const char *ptr = str;

	if (NULL == str) {
		return FALSE;
	}
	while (*ptr != '\0') {
		ch = (unsigned char)*ptr;
		if (byte_num == 0) {
			if (0 == (byte_num = utf8_byte_num(ch))) {
				return FALSE;
			}
		}
		else {
			if ((ch & 0xC0) != 0x80) {
				return FALSE;
			}
		}
		byte_num --;
		ptr ++;
	}
	if (byte_num > 0) {
		return FALSE;
	}
	return TRUE;
}

BOOL utf8_len(const char *str, int *plen)
{
	int len = 0;
	int clen = 0;
	int byte_num = 0;
	unsigned char ch;
	const char *ptr = str;
	
	clen = strlen(str);
	while (*ptr != '\0' && len < clen) {
		ch = (unsigned char)*ptr;
		if (0 == (byte_num = utf8_byte_num(ch))) {
			return FALSE;
		}
		ptr += byte_num;
		len ++;
	}
	*plen = len;
	return TRUE;
}

BOOL utf8_truncate(char *str, int length)
{
	int len = 0;
	int clen = 0;
	int byte_num = 0;
	unsigned char ch;
	char *ptr = str;
	
	clen = strlen(str);
	while (*ptr != '\0' && len < clen) {
		if (length == len) {
			*ptr = '\0';
			return TRUE;
		}
		ch = (unsigned char)*ptr;
		if (0 == (byte_num = utf8_byte_num(ch))) {
			return FALSE;
		}
		ptr += byte_num;
		len ++;
	}
	return TRUE;
}

/* Strip all UTF-8 and replace by '?' */
void utf8_filter(char *string)  
{
	int m;
	int count_s = 0;
	int minus_s = 0;
	auto bytes = reinterpret_cast<unsigned char *>(string);
	unsigned char *end = bytes + strlen(string);
  
	while (bytes < end) {
		if (bytes[0] > 0x7F) {
			if (minus_s) {
				m = count_s - minus_s + 1;
				memset((bytes-m), '?', m);
			}
			minus_s = 0;
			count_s = 0;
			bytes[0] = '?';
			bytes ++;
			continue;
		}
		if (bytes[0] <= 0x7F) {
			if (minus_s) {
				m = count_s - minus_s + 1;
				memset(bytes - m, '?', m);
			}
			minus_s = 0;  
			count_s = 0;
            if (0x09 == bytes[0]  || 0x0A == bytes[0] || 0x0D == bytes[0]
				|| (0x20 <= bytes[0] && bytes[0] <= 0x7E)) {
				/* do nothing */ 
			} else {
				bytes[0] = '?';
			}
			bytes ++;
			continue;
		}
		if ((bytes[0] & 0xF8) == 0xF0) {
			if (minus_s) {
				m = count_s - minus_s + 1;
				memset(bytes-m, '?', m);
			}
			count_s = 3;
			minus_s = 3;
			bytes ++;
			continue;
		}
		if ((bytes[0] & 0xF0) == 0xE0) {
			if (minus_s) {
				m = count_s - minus_s + 1;
				memset(bytes - m, '?', m);
			}
			count_s = 2;
			minus_s = 2;
			bytes ++;
			continue;
		}
		if ((bytes[0] & 0xE0) == 0xC0) {
			if (minus_s) {
				m = count_s - minus_s + 1;
				memset(bytes - m, '?', m);
			}
			count_s = 1;
			minus_s = 1;
			bytes ++;
			continue;
		}
		if ((bytes[0] & 0xC0) == 0x80) {
			if (minus_s) {
				-- minus_s;
			} else {
				bytes[0] = '?';
			}
			bytes ++;
			continue;
		}
		if (minus_s) {
			m = count_s - minus_s + 1;
			memset(bytes-m, '?', m);
		} else {
			bytes[0] = '?';
		}
        minus_s = 0;
        count_s = 0;
        bytes ++;
        continue;
    }
	if (minus_s) {
		m = count_s - minus_s + 1;
		memset(bytes - m, '?', m);
	}
}

void wchar_to_utf8(uint32_t wchar, char *zstring)
{
	auto string = reinterpret_cast<unsigned char *>(zstring);
	if (wchar < 0x7f) {
		string[0] = wchar;
		string[1] = '\0';
	} else if (wchar < 0x7ff) {
		string[0] = 192 + (wchar/64);
		string[1] = 128 + (wchar%64);
		string[2] = '\0';
	} else if (wchar < 0xffff) {
		string[0] = 224 + wchar/(64*64);
		string[1] = 128 + (wchar/64)%64;
		string[2] = 128 + wchar%64;
		string[3] = '\0';
	} else if (wchar < 0x1FFFFF) {
		string[0] = 240 + wchar/(64*64*64);
		string[1] = 128 + (wchar/(64*64))%64;
		string[2] = 128 + (wchar/64)%64;
		string[3] = 128 + wchar % 64;
		string[4] = '\0';
	} else if (wchar < 0x3FFFFFF) {
		string[0] = 248 + wchar/(64*64*64*64);
		string[1] = 128 + (wchar/(64*64*64))%64;
		string[2] = 128 + (wchar/(64*64))%64;
		string[3] = 128 + (wchar/64)%64;
		string[4] = 128 + wchar % 64;
		string[5] = '\0';
	} else if (wchar < 0x7FFFFFFF) {
		string[0] = 252 + wchar/(64*64*64*64*64);
		string[1] = 128 + (wchar/(64*64*64*64))%64;
		string[2] = 128 + (wchar/(64*64*64))%64;
		string[3] = 128 + (wchar/(64*64))%64;
		string[4] = 128 + (wchar/64)%64;
		string[5] = 128 + wchar % 64;
		string[6] = '\0';
	}
}

const char* replace_iconv_charset(const char *charset)
{
	if (0 == strcasecmp(charset, "gb2312")) {
		return "gbk";
	} else if (0 == strcasecmp(charset, "ksc_560") ||
		0 == strcasecmp(charset, "ks_c_5601") ||
		0 == strcasecmp(charset, "ks_c_5601-1987") ||
		0 == strcasecmp(charset, "csksc56011987")) {
		return "cp949";
	} else if (0 == strcasecmp(charset, "iso-2022-jp")) {
		return "iso-2022-jp-ms";
	} else if (0 == strcasecmp(charset, "unicode-1-1-utf-7")) {
		return "utf-7";
	}
	return charset;
}

BOOL string_to_utf8(const char *charset,
	const char *in_string, char *out_string)
{
	int length;
	iconv_t conv_id;
	char *pin, *pout;
	char tmp_charset[64];
	size_t in_len, out_len;
	
	
	if (0 == strcasecmp(charset, "UTF-8") ||
		0 == strcasecmp(charset, "ASCII") ||
		0 == strcasecmp(charset, "US-ASCII")) {
		strcpy(out_string, in_string);
		return TRUE;
	}
	
	length = strlen(in_string);
	if (0 == length) {
		out_string[0] = '\0';
		return TRUE;
	}
	gx_strlcpy(tmp_charset, replace_iconv_charset(charset), GX_ARRAY_SIZE(tmp_charset));
	if (0 != strcasecmp("utf-7", tmp_charset)) {
		length ++;
	}
	conv_id = iconv_open("UTF-8", tmp_charset);
	if ((iconv_t)-1 == conv_id) {
		return FALSE;
	}
	pin = (char*)in_string;
	pout = out_string;
	in_len = length;
	out_len = 2*length;
	if (iconv(conv_id, &pin, &in_len, &pout, &out_len) == static_cast<size_t>(-1)) {
		iconv_close(conv_id);
		return FALSE;
	}
	iconv_close(conv_id);
	if (0 == strcasecmp("utf-7", tmp_charset)) {
		out_string[2*length - out_len] = '\0';	
	}
	return TRUE;
}

BOOL string_from_utf8(const char *charset,
	const char *in_string, char *out_string)
{
	int length;
	iconv_t conv_id;
	char *pin, *pout;
	size_t in_len, out_len;
	
	
	if (0 == strcasecmp(charset, "UTF-8") ||
		0 == strcasecmp(charset, "ASCII") ||
		0 == strcasecmp(charset, "US-ASCII")) {
		strcpy(out_string, in_string);
		return TRUE;
	}
	
	length = strlen(in_string);
	if (0 == length) {
		out_string[0] = '\0';
		return TRUE;
	}
	
	length ++;
	
	conv_id = iconv_open(replace_iconv_charset(charset), "UTF-8");
	if ((iconv_t)-1 == conv_id) {
		return FALSE;
	}
	pin = (char*)in_string;
	pout = out_string;
	in_len = length;
	out_len = 2*length;
	if (iconv(conv_id, &pin, &in_len, &pout, &out_len) == static_cast<size_t>(-1)) {
		iconv_close(conv_id);
		return FALSE;
	}
	iconv_close(conv_id);
	return TRUE;
}

int utf8_to_utf16le(const char *src, void *dst, size_t len)
{
	size_t in_len;
	size_t out_len;
	iconv_t conv_id;

	conv_id = iconv_open("UTF-16LE", "UTF-8");
	auto pin  = deconst(src);
	auto pout = static_cast<char *>(dst);
	in_len = strlen(src) + 1;
	memset(dst, 0, len);
	out_len = len;
	if (iconv(conv_id, &pin, &in_len, &pout, &len) == static_cast<size_t>(-1)) {
		iconv_close(conv_id);
		return -1;
	} else {
		iconv_close(conv_id);
		return out_len - len;
	}
}

BOOL utf16le_to_utf8(const void *src, size_t src_len, char *dst, size_t len)
{
	char *pin, *pout;
	iconv_t conv_id;
	
	conv_id = iconv_open("UTF-8", "UTF-16LE");
	pin = (char*)src;
	pout = dst;
	memset(dst, 0, len);
	if (iconv(conv_id, &pin, &src_len, &pout, &len) == static_cast<size_t>(-1)) {
		iconv_close(conv_id);
		return FALSE;
	} else {
		iconv_close(conv_id);
		return TRUE;
	}
}

BOOL get_digest(const char *src, const char *tag, char *buff, size_t buff_len)
{
	size_t len;
	size_t length;
	const char *ptr1, *ptr2;
	char temp_tag[256];

	length = strlen(src);
	len = gx_snprintf(temp_tag, GX_ARRAY_SIZE(temp_tag), "\"%s\"", tag);
	ptr1 = search_string(src, temp_tag, length);
	if (NULL == ptr1) {
		return FALSE;
	}

	ptr1 += len;
	ptr1 = static_cast<const char *>(memchr(ptr1, ':', length - (ptr1 - src)));
	if (NULL == ptr1) {
		return FALSE;
	}
	ptr1 ++;
	while (' ' == *ptr1 || '\t' == *ptr1) {
		ptr1 ++;
		if (static_cast<size_t>(ptr1 - src) >= length)
			return FALSE;
	}
	ptr2 = ptr1;
	if ('"' == *ptr2) {
		do {
			ptr2 ++;
			if (static_cast<size_t>(ptr2 - src) >= length)
				return FALSE;
		} while ('"' != *ptr2 || '\\' == *(ptr2 - 1));
	}
	while (',' != *ptr2 && '}' != *ptr2) {
		ptr2 ++;
		if (static_cast<size_t>(ptr2 - src) >= length)
			return FALSE;
	}

	if (static_cast<size_t>(ptr2 - ptr1) <= buff_len - 1)
		len = ptr2 - ptr1;
	else
		len = buff_len - 1;
	memcpy(buff, ptr1, len);
	buff[len] = '\0';
	if ('"' == buff[0]) {
		len --;
		memmove(buff, buff + 1, len);
		buff[len] = '\0';
	}
	if ('"' == buff[len - 1]) {
		buff[len - 1] = '\0';
	}
	return TRUE;
}

BOOL set_digest(char *src, size_t length, const char *tag, const char *value)
{
	size_t len;
	size_t temp_len;
	size_t temp_len1;
	char *ptr1, *ptr2;
	char temp_tag[256];

	temp_len = strlen(src) + 1;
	len = gx_snprintf(temp_tag, GX_ARRAY_SIZE(temp_tag), "\"%s\"", tag);
	ptr1 = search_string(src, temp_tag, temp_len);
	if (NULL == ptr1) {
		return FALSE;
	}

	ptr1 += len;
	ptr1 = static_cast<char *>(memchr(ptr1, ':', temp_len - (ptr1 - src)));
	if (NULL == ptr1) {
		return FALSE;
	}
	ptr1 ++;
	
	while (' ' == *ptr1 || '\t' == *ptr1) {
		if (static_cast<size_t>(ptr1 - src) >= temp_len)
			return FALSE;
		ptr1 ++;
	}
	ptr2 = ptr1;
	if ('"' == *ptr2) {
		do {
			ptr2 ++;
			if (static_cast<size_t>(ptr2 - src) >= temp_len)
				return FALSE;
		} while ('"' != *ptr2 || '\\' == *(ptr2 - 1));
	}
	while (',' != *ptr2 && '}' != *ptr2) {
		ptr2 ++;
		if (static_cast<size_t>(ptr2 - src) >= temp_len)
			return FALSE;
	}

	len = strlen(value);
	
	temp_len1 = temp_len + len - (ptr2 - ptr1);
	
	if (temp_len1 > length) {
		return FALSE;
	}
	if (static_cast<size_t>(ptr2 - ptr1) < len)
		memmove(ptr1 + len, ptr2, temp_len1 - (ptr1 - src + len));
	else if (static_cast<size_t>(ptr2 - ptr1) > len)
		memmove(ptr1 + len, ptr2, src + temp_len - ptr2);
	memcpy(ptr1, value, len);
	return TRUE;
}

BOOL add_digest(char *src, size_t length, const char *tag, const char *value)
{
	size_t i, len;
	size_t temp_len;
	char temp_tag[256];
	char temp_buff[1024];
	
	temp_len = strlen(src) + 1;
	snprintf(temp_tag, 255, "\"%s\"", tag);
	if (NULL != search_string(src, temp_tag, temp_len)) {
		return set_digest(src, length, tag, value);
		
	}
	
	for (i=temp_len-1; i>0; i--) {
		if ('}' == src[i]) {
			len = gx_snprintf(temp_buff, GX_ARRAY_SIZE(temp_buff), ",\"%s\":%s", tag, value);
			if (length - i < len + 2) {
				return FALSE;
			}
			memcpy(src + i, temp_buff, len);
			memcpy(src + i + len, "}", 2);
			return TRUE;
		}
	}
	return FALSE;
}

/*
 *	swap a string from src and fill it into dest
 *	@param
 *		  dest [out]	buffer for writing out
 *		  src [in]	  buffer for reading
 */
void swap_string(char *dest, const char *src)
{
	long i, j, str_len;

	str_len = strlen(src);
	for (i=str_len-1, j=0; i>=0; i--,j++) {
		dest[j] = src[i];
	}
	dest[str_len] = '\0';
}

/*
 *	search a substring in a string
 *	@param
 *		haystack [in]  string to be searched
 *		needle [in]	   substring to be found
 *		haystacklen	   maximum length of haystack
 *	@return
 *		pointer to first address of found substring
 */
char* search_string(const char *haystack, const char *needle,
	size_t haystacklen)
{
	char *p;
	size_t plen;
	size_t len = strlen(needle);

	if (*needle == '\0')	/* everything matches empty string */
	return (char *) haystack;

	plen = haystacklen;
	p = (char *) haystack;
	while (1) {
		if (plen <= 0)
			return NULL;

		if (strncasecmp(p, needle, len) == 0) {
			return (p);
		}
		p++;
		plen--;

	}
	return NULL;
}

char* itvltoa(long interval, char *string)
{
	long days, hours;
	long minutes, seconds;
	long rest, offset;
	
	string[44] = '\0'; /* help static checkers flag callers */
	days = 0;
	hours = 0;
	minutes = 0;
	offset = 0;
	if (0 == interval) {
		strcpy(string, "0second");
		return string;
	} else if (interval < 0) {
		interval = -interval;
	}
	if (interval >= 86400) {
		days = interval/86400;
		rest = interval%86400;
		hours = rest/3600;
		rest = rest%3600;
		minutes = rest/60;
		seconds = rest%60;
	} else if (interval >= 3600 && interval < 86400) {
		hours = interval/3600;
		rest = interval%3600;
		minutes = rest/60;
		seconds = rest%60;
	} else if (interval >= 60 && interval < 3600) {
		minutes = interval/60;
		seconds = interval%60;
	}else {
		seconds = interval;
	}
	if (0 != days) {
		if (days > 1) {
			sprintf(string, "%ld", days);
			offset = strlen(string);
			strcpy(string + offset, "days");
			offset += 4;
		} else if (1 == days) {
			strcpy(string, "1day");
			offset = 5;
		}
	}
	if (0 != hours) {
		if (hours > 1) {
			sprintf(string + offset, "%ld", hours);
			offset = strlen(string);
			strcpy(string + offset, "hours");
			offset += 5;
		} else if (1 == hours) {
			strcpy(string + offset, "1hour");
			offset += 6;
		}
	}
	if (0 != minutes) {
		if (minutes > 1) {
			sprintf(string + offset, "%ld", minutes);
			offset = strlen(string);
			strcpy(string + offset, "minutes");
			offset += 7;
		} else if (1 == minutes) {
			strcpy(string + offset, "1minute");
			offset += 8;
		}
	}
	if (0 != seconds) {
		if (seconds > 1) {
			sprintf(string + offset, "%ld", seconds);
			offset = strlen(string);
			strcpy(string + offset, "seconds");
		} else if (1 == seconds) {
			strcpy(string + offset, "1second");
		}
	}
	return string;
}

char* bytetoa(uint64_t byte, char *string)
{
	if (byte < 1024) {
		sprintf(string, "%llu", static_cast<unsigned long long>(byte));
	} else if (byte >= 1024 && byte < 1024*1024) {
		sprintf(string, "%4.1lfK", (double)byte/1024);
	} else if (byte >= 1024*1024 && byte < 1024*1024*1024) {
		sprintf(string, "%4.1lfM", (double)byte/(1024*1024));
	} else if (byte >= 1024*1024*1024 && byte < 0x10000000000LL) {
		sprintf(string, "%1.1lfG", (double)byte/(1024*1024*1024));
	} else {
		sprintf(string, "%1.1lfT", (double)byte/0x10000000000LL);
	}
	HX_strltrim(string);
	return string;
}

uint64_t atobyte(const char *string)
{
	int length, last_pos;
	char unit;
	char temp_buff[36]; 

	length = strlen(string);
	if (length > 32) {
		return 0;
	}
	strcpy(temp_buff, string);
	HX_strrtrim(temp_buff);
	HX_strltrim(temp_buff);
	last_pos = strlen(temp_buff) - 1;
	unit = temp_buff[last_pos];
	if ('B' == unit) {
		temp_buff[last_pos] = '\0';
		return atoll(temp_buff);
	} else if ('K' == unit || 'k' == unit) {
		temp_buff[last_pos] = '\0';
		return atoll(temp_buff)*1024;
	} else if ('M' == unit || 'm' == unit) {
		temp_buff[last_pos] = '\0';
		return atoll(temp_buff)*1024*1024;
	} else if ('G' == unit || 'g' == unit) {
		temp_buff[last_pos] = '\0';
		return atoll(temp_buff)*1024*1024*1024;
	} else if ('T' == unit || 't' == unit) {
		temp_buff[last_pos] = '\0';
		return atoll(temp_buff)*0x10000000000LL;
	}
	return atoll(temp_buff);
}

static char crypt_salt[65]=
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789./";

const char *crypt_wrapper(const char *pw)
{
	char salt[21] = "$6$";
	randstring_k(salt + 3, 16, crypt_salt);
	salt[19] = '$';
	salt[20] = '\0';
	auto ret = crypt(pw, salt);
	if (ret != nullptr && ret[0] == '$')
		return ret;
	salt[1] = '1';
	ret = crypt(pw, salt);
	return ret != nullptr ? ret : "*0";
}


#define WILDS '*'  /* matches 0 or more characters (including spaces) */
#define WILDQ '?'  /* matches ecactly one character */

#define NOMATCH 0
#define MATCH (match+sofar)

int wildcard_match(const char *data, const char *mask, BOOL icase)
{
  const char *ma = mask, *na = data, *lsm = nullptr, *lsn = nullptr;
  int match = 1;
  int sofar = 0;

  /* null strings should never match */
	if (ma == nullptr || na == nullptr || *ma == '\0' || *na == '\0')
	return NOMATCH;
  /* find the end of each string */
  while (*(++mask));
  mask--;
  while (*(++data));
  data--;

  while (data >= na) {
	/* If the mask runs out of chars before the string, fall back on
	 * a wildcard or fail. */
	if (mask < ma) {
	  if (lsm) {
		data = --lsn;
		mask = lsm;
		if (data < na)
					lsm = nullptr;
		sofar = 0;
	  }
	  else
		return NOMATCH;
	}

	switch (*mask) {
	case WILDS:				   /* Matches anything */
	  do
		mask--;					   /* Zap redundant wilds */
	  while ((mask >= ma) && (*mask == WILDS));
	  lsm = mask;
	  lsn = data;
	  match += sofar;
	  sofar = 0;				/* Update fallback pos */
	  if (mask < ma)
		return MATCH;
	  continue;					/* Next char, please */
	case WILDQ:
	  mask--;
	  data--;
	  continue;					/* '?' always matches */
	}
	if (icase ? HX_toupper(*mask) == HX_toupper(*data) :
	(*mask == *data)) {		/* If matching char */
	  mask--;
	  data--;
	  sofar++;					/* Tally the match */
	  continue;					/* Next char, please */
	}
	if (lsm) {					/* To to fallback on '*' */
	  data = --lsn;
	  mask = lsm;
	  if (data < na)
				lsm = nullptr; /* Rewind to saved pos */
	  sofar = 0;
	  continue;					/* Next char, please */
	}
	return NOMATCH;				/* No fallback=No match */
  }
  while ((mask >= ma) && (*mask == WILDS))
	mask--;						   /* Zap leftover %s & *s */
  return (mask >= ma) ? NOMATCH : MATCH;   /* Start of both = match */
}

void randstring_k(char *buff, int length, const char *string)
{	 
	int i, key;
	int string_len;
	static int myseed = 25011984;
	
	if (length <= 0) {
		length = 1;
	}
	string_len = strlen(string);
	
	srand(time(NULL) * length + ++myseed);
	
	for (i=0; i<length; i++) {
		key = rand() % string_len;
		buff[i] = string[key];
	}
	buff[length] = '\0';
}

void randstring(char *b, int l)
{
	const char p[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,.-#'?!";
	return randstring_k(b, l, p);
}

#define OK	(0)
#define FAIL	(-1)
#define BUFOVER (-2)


#define CHAR64(c)  (((c) < 0 || (c) > 127) ? -1 : index_64[(c)])

static char basis_64[] =
   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/???????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????";

static int8_t index_64[128] = {
	-1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	-1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
	-1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
	52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
	-1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
	15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
	-1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
	41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1
};


int encode64(const void *vin, size_t inlen, char *out,
    size_t outmax, size_t *outlen)
{
	auto in = static_cast<const unsigned char *>(vin);
	unsigned char oval;
	size_t olen;

	/* Will it fit? */
	olen = (inlen + 2) / 3 * 4;
	if (outlen)
	  *outlen = olen;
	if (outmax < olen)
	  return BUFOVER;

	/* Do the work... */
	while (inlen >= 3) {
	  /* user provided max buffer size; make sure we don't go over it */
		*out++ = basis_64[in[0] >> 2];
		*out++ = basis_64[((in[0] << 4) & 0x30) | (in[1] >> 4)];
		*out++ = basis_64[((in[1] << 2) & 0x3c) | (in[2] >> 6)];
		*out++ = basis_64[in[2] & 0x3f];
		in += 3;
		inlen -= 3;
	}
	if (inlen > 0) {
	  /* user provided max buffer size; make sure we don't go over it */
		*out++ = basis_64[in[0] >> 2];
		oval = (in[0] << 4) & 0x30;
		if (inlen > 1) oval |= in[1] >> 4;
		*out++ = basis_64[oval];
		*out++ = (inlen < 2) ? '=' : basis_64[(in[1] << 2) & 0x3c];
		*out++ = '=';
	}

	if (olen < outmax)
	  *out = '\0';
	
	return OK;
}

int decode64(const char *in, size_t inlen, void *vout, size_t *outlen)
{
	auto out = static_cast<uint8_t *>(vout);
	size_t len = 0,lup;
	int c1, c2, c3, c4;

	/* check parameters */
	if (out==NULL) return FAIL;

	/* xxx these necessary? */
	if (in[0] == '+' && in[1] == ' ') in += 2;
	if (*in == '\r') return FAIL;

	for (lup=0;lup<inlen/4;lup++)
	{
		c1 = in[0];
		if (CHAR64(c1) == -1) return FAIL;
		c2 = in[1];
		if (CHAR64(c2) == -1) return FAIL;
		c3 = in[2];
		if (c3 != '=' && CHAR64(c3) == -1) return FAIL; 
		c4 = in[3];
		if (c4 != '=' && CHAR64(c4) == -1) return FAIL;
		in += 4;
		*out++ = (CHAR64(c1) << 2) | (CHAR64(c2) >> 4);
		++len;
		if (c3 != '=') {
			*out++ = ((CHAR64(c2) << 4) & 0xf0) | (CHAR64(c3) >> 2);
			++len;
			if (c4 != '=') {
				*out++ = ((CHAR64(c3) << 6) & 0xc0) | CHAR64(c4);
				++len;
			}
		}
	}

	*out=0; /* terminate string */
	*outlen=len;
	return OK;
}


#define DW_EOL "\r\n"
#define MAXLINE	76
static char base64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz0123456789+/";

static char base64idx[128] = {
	'\377','\377','\377','\377','\377','\377','\377','\377',
	'\377','\377','\377','\377','\377','\377','\377','\377',
	'\377','\377','\377','\377','\377','\377','\377','\377',
	'\377','\377','\377','\377','\377','\377','\377','\377',
	'\377','\377','\377','\377','\377','\377','\377','\377',
	'\377','\377','\377',	 62,'\377','\377','\377',	 63,
		52,	   53,	  54,	 55,	56,	   57,	  58,	 59,
		60,	   61,'\377','\377','\377','\377','\377','\377',
	'\377',		0,	   1,	  2,	 3,		4,	   5,	  6,
		 7,		8,	   9,	 10,	11,	   12,	  13,	 14,
		15,	   16,	  17,	 18,	19,	   20,	  21,	 22,
		23,	   24,	  25,'\377','\377','\377','\377','\377',
	'\377',	   26,	  27,	 28,	29,	   30,	  31,	 32,
		33,	   34,	  35,	 36,	37,	   38,	  39,	 40,
		41,	   42,	  43,	 44,	45,	   46,	  47,	 48,
		49,	   50,	  51,'\377','\377','\377','\377','\377'
};

static char hextab[] = "0123456789ABCDEF";


#define isbase64(a) (  ('A' <= (a) && (a) <= 'Z') \
					|| ('a' <= (a) && (a) <= 'z') \
					|| ('0' <= (a) && (a) <= '9') \
					||	(a) == '+' || (a) == '/'  )



int encode64_ex(const void *vin, size_t inlen, char *_out,
	size_t outmax, size_t *outlen)
{
	auto _in = static_cast<const uint8_t *>(vin);
	size_t inLen = inlen;
	size_t i;
	char* out = _out;
	size_t outsize = (inLen+2)/3*4;		/* 3:4 conversion ratio */
	size_t inpos  = 0;
	size_t outPos = 0;
	int c1, c2, c3;
	int lineLen = 0;
	const char* cp;
	
	if (!_in || !_out || !outlen) {
		return -1;
	}
	outsize += strlen(DW_EOL)*outsize/MAXLINE + 2;	/* Space for newlines and NUL */
	if (outmax < outsize) {
		return -1;
	}
	/* Get three characters at a time and encode them. */
	for (i=0; i < inLen/3; ++i) {
		c1 = _in[inpos++] & 0xFF;
		c2 = _in[inpos++] & 0xFF;
		c3 = _in[inpos++] & 0xFF;
		out[outPos++] = base64tab[(c1 & 0xFC) >> 2];
		out[outPos++] = base64tab[((c1 & 0x03) << 4) | ((c2 & 0xF0) >> 4)];
		out[outPos++] = base64tab[((c2 & 0x0F) << 2) | ((c3 & 0xC0) >> 6)];
		out[outPos++] = base64tab[c3 & 0x3F];
		lineLen += 4;
		if (lineLen >= MAXLINE-3) {
			const char *cq = DW_EOL;
			out[outPos++] = *cq++;
			if (*cq != '\0')
				out[outPos++] = *cq;
			lineLen = 0;
		}
	}
	/* Encode the remaining one or two characters. */
	switch (inLen % 3) {
	case 0:
		cp = DW_EOL;
		out[outPos++] = *cp++;
		if (*cp) {
			out[outPos++] = *cp;
		}
		break;
	case 1:
		c1 = _in[inpos] & 0xFF;
		out[outPos++] = base64tab[(c1 & 0xFC) >> 2];
		out[outPos++] = base64tab[((c1 & 0x03) << 4)];
		out[outPos++] = '=';
		out[outPos++] = '=';
		cp = DW_EOL;
		out[outPos++] = *cp++;
		if (*cp) {
			out[outPos++] = *cp;
		}
		break;
	case 2:
		c1 = _in[inpos++] & 0xFF;
		c2 = _in[inpos] & 0xFF;
		out[outPos++] = base64tab[(c1 & 0xFC) >> 2];
		out[outPos++] = base64tab[((c1 & 0x03) << 4) | ((c2 & 0xF0) >> 4)];
		out[outPos++] = base64tab[((c2 & 0x0F) << 2)];
		out[outPos++] = '=';
		cp = DW_EOL;
		out[outPos++] = *cp++;
		if (*cp) {
			out[outPos++] = *cp;
		}
		break;
	}
	out[outPos] = 0;
	*outlen = outPos;
	return 0;
}


int decode64_ex(const char *_in, size_t inlen, void *vout,
	size_t outmax, size_t *outlen)
{
	auto out = static_cast<uint8_t *>(vout);
	size_t inLen = inlen;
	size_t outsize = ( ( inLen + 3 ) / 4 ) * 3;
	/* Get four input chars at a time and decode them. Ignore white space
	 * chars (CR, LF, SP, HT). If '=' is encountered, terminate input. If
	 * a char other than white space, base64 char, or '=' is encountered,
	 * flag an input error, but otherwise ignore the char.
	 */
	int is_err = 0;
	int is_endSeen = 0;
	int b1, b2, b3;
	int a1, a2, a3, a4;
	size_t inpos = 0;
	size_t outPos = 0;
	
	if (_in == nullptr || vout == nullptr || outlen == nullptr)
		return -1;
	if (outmax < outsize) {
		*outlen = 0;
		return -1;
	}
	while (inpos < inLen) {
		a1 = a2 = a3 = a4 = 0;
		while (inpos < inLen) {
			a1 = _in[inpos++] & 0xFF;
			if (isbase64(a1)) {
				break;
			}
			else if (a1 == '=') {
				is_endSeen = 1;
				break;
			}
			else if (a1 != '\r' && a1 != '\n' && a1 != ' ' && a1 != '\t') {
				is_err = 1;
			}
		}
		while (inpos < inLen) {
			a2 = _in[inpos++] & 0xFF;
			if (isbase64(a2)) {
				break;
			}
			else if (a2 == '=') {
				is_endSeen = 1;
				break;
			}
			else if (a2 != '\r' && a2 != '\n' && a2 != ' ' && a2 != '\t') {
				is_err = 1;
			}
		}
		while (inpos < inLen) {
			a3 = _in[inpos++] & 0xFF;
			if (isbase64(a3)) {
				break;
			}
			else if (a3 == '=') {
				is_endSeen = 1;
				break;
			}
			else if (a3 != '\r' && a3 != '\n' && a3 != ' ' && a3 != '\t') {
				is_err = 1;
			}
		}
		while (inpos < inLen) {
			a4 = _in[inpos++] & 0xFF;
			if (isbase64(a4)) {
				break;
			}
			else if (a4 == '=') {
				is_endSeen = 1;
				break;
			}
			else if (a4 != '\r' && a4 != '\n' && a4 != ' ' && a4 != '\t') {
				is_err = 1;
			}
		}
		if (isbase64(a1) && isbase64(a2) && isbase64(a3) && isbase64(a4)) {
			a1 = base64idx[a1] & 0xFF;
			a2 = base64idx[a2] & 0xFF;
			a3 = base64idx[a3] & 0xFF;
			a4 = base64idx[a4] & 0xFF;
			b1 = ((a1 << 2) & 0xFC) | ((a2 >> 4) & 0x03);
			b2 = ((a2 << 4) & 0xF0) | ((a3 >> 2) & 0x0F);
			b3 = ((a3 << 6) & 0xC0) | ( a4		 & 0x3F);
			out[outPos++] = (char)b1;
			out[outPos++] = (char)b2;
			out[outPos++] = (char)b3;
		}
		else if (isbase64(a1) && isbase64(a2) && isbase64(a3) && a4 == '=') {
			a1 = base64idx[a1] & 0xFF;
			a2 = base64idx[a2] & 0xFF;
			a3 = base64idx[a3] & 0xFF;
			b1 = ((a1 << 2) & 0xFC) | ((a2 >> 4) & 0x03);
			b2 = ((a2 << 4) & 0xF0) | ((a3 >> 2) & 0x0F);
			out[outPos++] = (char)b1;
			out[outPos++] = (char)b2;
			break;
		}
		else if (isbase64(a1) && isbase64(a2) && a3 == '=' && a4 == '=') {
			a1 = base64idx[a1] & 0xFF;
			a2 = base64idx[a2] & 0xFF;
			b1 = ((a1 << 2) & 0xFC) | ((a2 >> 4) & 0x03);
			out[outPos++] = (char)b1;
			break;
		}
		else {
			break;
		}
		if (is_endSeen) {
			break;
		}
	} /* end while loop */
	out[outPos] = 0;
	*outlen = outPos;
	return (is_err) ? -1 : 0;
}

ssize_t qp_encode_ex(void *voutput, size_t outlen, const char *input, size_t length)
{
	auto output = static_cast<uint8_t *>(voutput);
	size_t inpos, outpos, linelen;
	int ch;

	if (!input || !output) {
		return -1;
	}
	inpos  = 0;
	outpos = 0;
	linelen = 0;
	while (inpos < length) {
		ch = input[inpos++] & 0xFF;
		/* '.' at beginning of line (confuses some SMTPs) */
		if (linelen == 0 && ch == '.') {
			output[outpos++] = '=';
			output[outpos++] = hextab[(ch >> 4) & 0x0F];
			output[outpos++] = hextab[ch & 0x0F];
			linelen += 3;
		}
		/* "From " at beginning of line (gets mangled in mbox folders) */
		else if (linelen == 0 && inpos+3 < length && ch == 'F'
				 && input[inpos	 ] == 'r' && input[inpos+1] == 'o'
				 && input[inpos+2] == 'm' && input[inpos+3] == ' ') {
			output[outpos++] = '=';
			output[outpos++] = hextab[(ch >> 4) & 0x0F];
			output[outpos++] = hextab[ch & 0x0F];
			linelen += 3;
		}
		/* Normal printable char */
		else if ((62 <= ch && ch <= 126) || (33 <= ch && ch <= 60)) {
			output[outpos++] = (char) ch;
			++linelen;
		}
		/* Space */
		else if (ch == ' ') {
			/* Space at end of line or end of input must be encoded */
			if (inpos >= length			  /* End of input? */
				|| (inpos < length-1	  /* End of line? */
					&& input[inpos	] == '\r' 
					&& input[inpos+1] == '\n') ) {

				output[outpos++] = '=';
				output[outpos++] = '2';
				output[outpos++] = '0';
				linelen += 3;
			}
			else {
				output[outpos++] = ' ';
				++linelen;
			}
		}
		/* Hard line break */
		else if (inpos < length && ch == '\r' && input[inpos] == '\n') {
			++inpos;
			output[outpos++] = '\r';
			output[outpos++] = '\n';
			linelen = 0;
		}
		/* Non-printable char */
		else if (ch & 0x80		  /* 8-bit char */
				 || !(ch & 0xE0)  /* control char */
				 || ch == 0x7F	  /* DEL */
				 || ch == '=') {  /* special case */
			output[outpos++] = '=';
			output[outpos++] = hextab[(ch >> 4) & 0x0F];
			output[outpos++] = hextab[ch & 0x0F];
			linelen += 3;
		}
		/* Soft line break */
		if (linelen >= MAXLINE-3 && !(inpos < length-1 && 
			input[inpos] == '\r' && input[inpos+1] == '\n')) {

			output[outpos++] = '=';
			output[outpos++] = '\r';
			output[outpos++] = '\n';
			linelen = 0;
		}
	}
	output[outpos] = 0;
	return outpos;
}

void debug_info(const char *format, ...)
{
#ifdef _DEBUG_UMTA
	char msg[256];
	va_list ap;

	memset(msg, 0, sizeof(msg));
	va_start(ap, format);
	vsprintf(msg, format, ap);
	va_end(ap);
	printf("%s\n", msg);
#endif
}

/*	qpdecode.c -- quoted-printable decoding routine
 *	Copyright (C) 2001-2003 Mark Weaver
 *	Written by Mark Weaver <mark@npsl.co.uk>
 *
 *	This library is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU Library General Public
 *	License as published by the Free Software Foundation; either
 *	version 2 of the License, or (at your option) any later version.
 *
 *	This library is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	Library General Public License for more details.
 *
 *	You should have received a copy of the GNU Library General Public
 *	License along with this library; if not, write to the
 *	Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *	Boston, MA	02111-1307, USA.
 */



/* 'robust' QP decode accepts =3e as encouraged by the standard, although
 *	it is illegal to encode this way
 */
static const unsigned char hex_tab[256] = 
{
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x01,
	0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
	0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0A, 0x0B, 0x0C,
	0x0D, 0x0E, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	0x10, 0x10, 0x10, 0x10, 0x10, 0x10
};

size_t qp_decode(void *voutput, const char *input, size_t length)
{
	auto output = static_cast<uint8_t *>(voutput);
	int c;
	size_t i, cnt = 0;
	for (i = 0; i < length; i++) {

		c = input[i];

		switch (c) {
		case '=':
			/* quoted char, process it */
			if (i < length - 2 && HX_isxdigit(input[i+1]) &&
			    HX_isxdigit(input[i+2])) { /* OK, this is =HEX */
				output[cnt++] = (hex_tab[input[i+1] & 0xff] << 4) | 
					hex_tab[input[i+2] & 0xff];
				i +=2;
				break;
			}
			/* indicates 'soft-line break', implying ignore 
			   it & the following CR 
			*/
			if (i < length - 2 && input[i+1] == '\r' && 
				input[i+2] == '\n') {
					i +=2;
					break;
			}
			/* just ignore it, it doesn't seem to be correctly quoting
			   anything (report an error/add a fussy mode?) 
			*/
			break;
		default:
			/* pass other characters through unmolested */
			output[cnt++] = c;
			break;
		}
	}
	output[cnt] = '\0';
	return cnt;
}

ssize_t qp_decode_ex(void *voutput, size_t out_len, const char *input,
    size_t length)
{
	auto output = static_cast<uint8_t *>(voutput);
	int c;
	size_t i, cnt = 0;
	for (i = 0; i < length; i++) {

		c = input[i];

		switch (c) {
		case '=':
			/* quoted char, process it */
			if (i < length - 2 && HX_isxdigit(input[i+1]) &&
			    HX_isxdigit(input[i+2])) { /* OK, this is =HEX */
				cnt++;
				i +=2;
				break;
			}
			/* indicates 'soft-line break', implying ignore 
			   it & the following CR 
			*/
			if (i < length - 2 && input[i+1] == '\r' && 
				input[i+2] == '\n') {
					i +=2;
					break;
			}
			/* just ignore it, it doesn't seem to be correctly quoting
			   anything (report an error/add a fussy mode?) 
			*/
			break;
		default:
			/* pass other characters through unmolested */
			cnt++;
			break;
		}
	}
	if (cnt >= out_len) {
		return -1;
	}
	return qp_decode(output, input, length);
}

void encode_hex_int(int id, char *out)
{
	static const char codes[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
							'8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	char t_char;
	size_t i, j;
	
	for (i=0,j=0; i<sizeof(int); i++,j+=2) {
		t_char = (id >> i*8) & 0xFF;
		out[j+1] = codes[t_char&0x0f];
		out[j] = codes[(t_char&0xf0)>>4];
	}
	out[j] = '\0';
}

int decode_hex_int(const char *in)
{
	int retval;
	char t_buff[3];
	
	if (strlen(in) < 2*sizeof(int)) {
		return 0;
	}

	retval = 0;
	for (size_t i = 0; i < sizeof(int); ++i) {
		t_buff[0] = in[2*i];
		t_buff[1] = in[2*i+1];
		t_buff[2] = '\0';
		retval |= strtol(t_buff, NULL, 16) << i*8;
	}
	return retval;
}

BOOL encode_hex_binary(const void *vsrc, int srclen, char *dst, int dstlen)
{
	auto src = static_cast<const uint8_t *>(vsrc);
	static const char codes[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
							 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	int i, j;
	char t_char;
	
	if (2*srclen + 1 > dstlen) {
		return FALSE;
	}
	for (i=0,j=0; i<srclen; i++,j+=2) {
		t_char = src[i];
		dst[j + 1] = codes[t_char&0x0f];
		dst[j] = codes[(t_char&0xf0)>>4];
	}
	dst[j] = '\0';
	return TRUE;
}

BOOL decode_hex_binary(const char *src, void *vdst, int dstlen)
{
	auto dst = static_cast<uint8_t *>(vdst);
	char t_buff[3];
	int i, j, len;

	len = strlen(src);
	if (len/2 > dstlen) {
		return FALSE;
	}
	for (i=0,j=0; i<len; i+=2,j++) {
		t_buff[0] = src[i];
		t_buff[1] = src[i+1];
		t_buff[2] = '\0';
		dst[j] = strtol(t_buff, NULL, 16);
	}
	if (j < dstlen) {
		dst[j] = '\0';
	}
	return TRUE;
}

#define DEC(c)	(((c) - ' ') & 077)

#define ENC(c) ((c) ? (((c) & 077) + ' ') : '`')


int uudecode(const char *in, size_t inlen, int *pmode,
	char *file_name, char *out, size_t *outlen)
{
	int i, j;
	int mode;
	char *ptr;
	char *pline;
	int line_len;
	char buff[80]{};
	int c1, c2, c3;
	int n, expected;
	const char *pend;
	char tmp_name[256];
	
	*outlen = 0;
	mode = 0666;
	tmp_name[0] = '\0';
	ptr = search_string(in, "begin ", inlen);
	if (NULL != ptr) {
		ptr += 6;
		if (' ' != ptr[3]) {
			return -1;
		}
		if (1 != sscanf(ptr, "%o ", &mode)) {
			return -1;
		}
		ptr += 4;
		for (i=0; i<256; i++,ptr++) {
			if ('\r' == *ptr || '\n' == *ptr) {
				ptr ++;
				tmp_name[i] = '\0';
				break;
			}
			tmp_name[i] = *ptr;
		}
		if (i >= 256) {
			return -1;
		}
		if ('\r' == *(ptr - 1) && '\n' == *ptr) {
			ptr ++;
		}
	} else {
		ptr = (char*)in;
	}
	pline = ptr;
	pend = in + inlen;
	while (pline < pend) {
		for (j=0; j<80; j++) {
			if ('\r' == pline[j] || '\n' == pline[j]) {
				line_len = j;
				break;
			}
			if (pline + j >= pend) {
				return -1;
			}
			buff[j] = pline[j];
		}
		if (j >= 80) {
			return -1;
		}
		if ('\r' == pline[j] && '\n' == pline[j + 1]) {
			pline += j + 2;
		} else {
			pline += j + 1;
		}
		n = DEC(buff[0]);
		if (n <= 0 || 0 == line_len) {
			break;
		}
		/* Calculate expected # of chars and pad if necessary */
		expected = ((n + 2) / 3) << 2;
		for (j=line_len; j<=expected; j++) {
			buff[j] = ' ';
		}
		ptr = buff + 1;
		while (n > 0) {
			c1 = DEC(*ptr) << 2 | DEC(ptr[1]) >> 4;
			c2 = DEC(ptr[1]) << 4 | DEC(ptr[2]) >> 2;
			c3 = DEC(ptr[2]) << 6 | DEC(ptr[3]);
			if (n >= 1) {
				out[(*outlen)++] = c1;
			}
			if (n >= 2) {
				out[(*outlen)++] = c2;
			}
			if (n >= 3) {
				out[(*outlen)++] = c3;
			}
			ptr += 4;
			n -= 3;
		}
	}
	if (NULL != pmode) {
		*pmode = mode;
	}
	if (NULL != file_name) {
		strcpy(file_name, tmp_name);
	}
	return 0;
}

int uuencode(int mode, const char *file_name, const char *in,
	size_t inlen, char *out, size_t outmax, size_t *outlen)
{
	int i, n;
	size_t offset;
	const char *ptr;
	int c1, c2, c3, c4;
	
	if (NULL != file_name) {
		offset = gx_snprintf(out, outmax, "begin %o %s\r\n", mode, file_name);
		if (offset >= outmax) {
			return -1;
		}
	} else {
		offset = 0;
	}
	ptr = in;
	while (true) {
		/* 1 (up to) 45 character line */
		if (in + inlen - ptr >= 45) {
			n = 45;
		} else {
			n = in + inlen - ptr;
		}
		out[offset] = ENC(n);
		offset ++;
		if (offset >= outmax) {
			return -1;
		}
		for (i=0; i<n; i+=3) {
			c1 = ptr[i] >> 2;
			c2 = ((ptr[i] << 4) & 060) | ((ptr[i + 1] >> 4) & 017);
			c3 = ((ptr[i + 1] << 2) & 074) | ((ptr[i + 2] >> 6) & 03);
			c4 = ptr[i + 2] & 077;
			out[offset++] = ENC(c1);
			if (offset >= outmax) {
				return -1;
			}
			out[offset++] = ENC(c2);
			if (offset >= outmax) {
				return -1;
			}
			out[offset++] = ENC(c3);
			if (offset >= outmax) {
				return -1;
			}
			out[offset++] = ENC(c4);
			if (offset >= outmax) {
				return -1;
			}
		}
		ptr += n;
		out[offset++] = '\r';
		if (offset >= outmax) {
			return -1;
		}
		out[offset++] = '\n';
		if (offset >= outmax) {
			return -1;
		}
		if (n <= 0) {
			break;
		}
	}
	if (NULL != file_name) {
		if (outmax - offset < 5) {
			return -1;
		}
		memcpy(out + offset, "end\r\n", 5);
		offset += 5;
	}
	*outlen = offset;
	return 0;
}
