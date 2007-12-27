/*
 * Copyright (c) 2007 Vreixo Formoso
 * Copyright (c) 2007 Mario Danic
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

#include "util.h"
#include "error.h"

#include <stdlib.h>
#include <wchar.h>
#include <iconv.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <limits.h>

int int_pow(int base, int power)
{
    int result = 1;
    while (--power >= 0) {
        result *= base;
    }
    return result;
}


int strconv(const char *str, const char *icharset, const char *ocharset,
            char **output)
{
    size_t inbytes;
    size_t outbytes;
    size_t n;
    iconv_t conv;
    char *out;
    char *src;
    char *ret;

    inbytes = strlen(str);
    outbytes = (inbytes + 1) * MB_LEN_MAX;
    out = malloc(outbytes);
    if (out == NULL) {
        return ISO_MEM_ERROR;
    }

    conv = iconv_open(ocharset, icharset);
    if (conv == (iconv_t)(-1)) {
        return ISO_CHARSET_CONV_ERROR;
    }
    src = (char *)str;
    ret = (char *)out;

    n = iconv(conv, &src, &inbytes, &ret, &outbytes);
    if (n == -1) {
        /* error */
        iconv_close(conv);
        return ISO_CHARSET_CONV_ERROR;
    }
    iconv_close(conv);
    *ret = '\0';

    *output = realloc(out, ret - out);
    return ISO_SUCCESS;
}

/**
 * Convert a str in a specified codeset to WCHAR_T. 
 * The result must be free() when no more needed
 * 
 * @return
 *      1 success, < 0 error
 */
static 
int str2wchar(const char *icharset, const char *input, wchar_t **output) 
{
    iconv_t conv;
    size_t inbytes;
    size_t outbytes;
    char *ret;
    char *src;
    wchar_t *wstr;
    size_t n;
    
    if (icharset == NULL || input == NULL || output == NULL) {
        return ISO_NULL_POINTER;
    }

    conv = iconv_open("WCHAR_T", icharset);
    if (conv == (iconv_t)-1) {
        return ISO_CHARSET_CONV_ERROR;
    }
    
    inbytes = strlen(input);
    outbytes = (inbytes + 1) * sizeof(wchar_t);

    /* we are sure that numchars <= inbytes */
    wstr = malloc(outbytes);
    if (wstr == NULL) {
        return ISO_MEM_ERROR;
    }
    ret = (char *)wstr;
    src = (char *)input;

    n = iconv(conv, &src, &inbytes, &ret, &outbytes);
    while (n == -1) {
        
        if (errno == E2BIG) {
            /* error, should never occur */
            iconv_close(conv);
            free(wstr);
            return ISO_CHARSET_CONV_ERROR;
        } else {
            wchar_t *wret;
            
            /* 
             * Invalid input string charset.
             * This can happen if input is in fact encoded in a charset 
             * different than icharset.
             * We can't do anything better than replace by "_" and continue.
             */
            /* TODO we need a way to report this */
            /* printf("String %s is not encoded in %s\n", str, codeset); */
            inbytes--;
            src++;
            
            wret = (wchar_t*) ret;
            *wret++ = (wchar_t) '_';
            ret = (char *) wret;
            outbytes -= sizeof(wchar_t);

            if (!inbytes)
                break;
            n = iconv(conv, &src, &inbytes, &ret, &outbytes);
        }
    }
    iconv_close(conv);
    
    *( (wchar_t *)ret )='\0';
    *output = wstr;
    return ISO_SUCCESS;
}

int str2ascii(const char *icharset, const char *input, char **output)
{
    int result;
    wchar_t *wsrc_;
    char *ret;
    char *ret_;
    char *src;
    iconv_t conv;
    size_t numchars;
    size_t outbytes;
    size_t inbytes;
    size_t n;
    
    if (icharset == NULL || input == NULL || output == NULL) {
        return ISO_NULL_POINTER;
    }

    /* convert the string to a wide character string. Note: outbytes
     * is in fact the number of characters in the string and doesn't
     * include the last NULL character.
     */
    result = str2wchar(icharset, input, &wsrc_);
    if (result < 0) {
        return result;
    }
    src = (char *)wsrc_;
    numchars = wcslen(wsrc_);
    
    inbytes = numchars * sizeof(wchar_t);

    ret_ = malloc(numchars + 1);
    if (ret_ == NULL) {
        return ISO_MEM_ERROR;
    }
    outbytes = numchars;
    ret = ret_;

    /* initialize iconv */
    conv = iconv_open("ASCII", "WCHAR_T");
    if (conv == (iconv_t)-1) {
        free(wsrc_);
        free(ret_);
        return ISO_CHARSET_CONV_ERROR;
    }

    n = iconv(conv, &src, &inbytes, &ret, &outbytes);
    while(n == -1) {
        /* The destination buffer is too small. Stops here. */
        if (errno == E2BIG)
            break;

        /* An incomplete multi bytes sequence was found. We 
         * can't do anything here. That's quite unlikely. */
        if (errno == EINVAL)
            break;

        /* The last possible error is an invalid multi bytes
         * sequence. Just replace the character with a "_". 
         * Probably the character doesn't exist in ascii like
         * "é, è, à, ç, ..." in French. */
        *ret++ = '_';
        outbytes--;

        if (!outbytes)
            break;

        /* There was an error with one character but some other remain
         * to be converted. That's probably a multibyte character.
         * See above comment. */
        src += sizeof(wchar_t);
        inbytes -= sizeof(wchar_t);

        if (!inbytes)
            break;

        n = iconv(conv, &src, &inbytes, &ret, &outbytes);
    }

    iconv_close(conv);

    *ret='\0';
    free(wsrc_);
    
    *output = ret_;
    return ISO_SUCCESS;
}

static int valid_d_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c == '_');
}

static int valid_a_char(char c)
{
    return (c >= ' ' && c <= '"') || (c >= '%' && c <= '?')
           || (c >= 'A' && c <= 'Z') || (c == '_');
}

static 
char *iso_dirid(const char *src, int size)
{
    size_t len, i;
    char name[32];
    
    len = strlen(src);
    if (len > size) {
        len = size;
    }
    for (i = 0; i < len; i++) {
        char c = toupper(src[i]);
        name[i] = valid_d_char(c) ? c : '_';
    }
    
    name[len] = '\0';
    return strdup(name);
}

char *iso_1_dirid(const char *src)
{
    return iso_dirid(src, 8);
}

char *iso_2_dirid(const char *src)
{
    return iso_dirid(src, 31);
}

char *iso_1_fileid(const char *src)
{
    char *dot; /* Position of the last dot in the filename, will be used 
                  to calculate lname and lext. */
    int lname, lext, pos, i;
    char dest[13]; /*  13 = 8 (name) + 1 (.) + 3 (ext) + 1 (\0) */

    if (src == NULL) {
        return NULL;
    }
    dot = strrchr(src, '.');

    lext = dot ? strlen(dot + 1) : 0;
    lname = strlen(src) - lext - (dot ? 1 : 0);

    /* If we can't build a filename, return NULL. */
    if (lname == 0 && lext == 0) {
        return NULL;
    }

    pos = 0;
    
    /* Convert up to 8 characters of the filename. */
    for (i = 0; i < lname && i < 8; i++) {
        char c = toupper(src[i]);

        dest[pos++] = valid_d_char(c) ? c : '_';
    }
    
    /* This dot is mandatory, even if there is no extension. */
    dest[pos++] = '.';
    
    /* Convert up to 3 characters of the extension, if any. */
    for (i = 0; i < lext && i < 3; i++) {
        char c = toupper(src[lname + 1 + i]);

        dest[pos++] = valid_d_char(c) ? c : '_';
    }
    
    dest[pos] = '\0';
    return strdup(dest);
}

char *iso_2_fileid(const char *src)
{
    char *dot;
    int lname, lext, lnname, lnext, pos, i;
    char dest[32]; /* 32 = 30 (name + ext) + 1 (.) + 1 (\0) */

    if (src == NULL) {
        return NULL;
    }

    dot = strrchr(src, '.');

    /* 
     * Since the maximum length can be divided freely over the name and
     * extension, we need to calculate their new lengths (lnname and
     * lnext). If the original filename is too long, we start by trimming
     * the extension, but keep a minimum extension length of 3. 
     */
    if (dot == NULL || *(dot + 1) == '\0') {
        lname = strlen(src);
        lnname = (lname > 30) ? 30 : lname;
        lext = lnext = 0;
    } else {
        lext = strlen(dot + 1);
        lname = strlen(src) - lext - 1;
        lnext = (strlen(src) > 31 && lext > 3)
            ? (lname < 27 ? 30 - lname : 3) : lext;
        lnname = (strlen(src) > 31) ? 30 - lnext : lname;
    }

    if (lnname == 0 && lnext == 0) {
        return NULL;
    }

    pos = 0;
    
    /* Convert up to lnname characters of the filename. */
    for (i = 0; i < lnname; i++) {
        char c = toupper(src[i]);

        dest[pos++] = valid_d_char(c) ? c : '_';
    }
    dest[pos++] = '.';
    
    /* Convert up to lnext characters of the extension, if any. */
    for (i = 0; i < lnext; i++) {
        char c = toupper(src[lname + 1 + i]);

        dest[pos++] = valid_d_char(c) ? c : '_';
    }
    dest[pos] = '\0';
    return strdup(dest);
}

//void iso_dirid(char *src, int maxlen)
//{
//    size_t len, i;
//
//    len = strlen(src);
//    if (len > maxlen) {
//        src[maxlen] = '\0';
//        len = maxlen;
//    }
//    for (i = 0; i < len; i++) {
//        char c = toupper(src[i]);
//        src[i] = valid_d_char(c) ? c : '_';
//    }
//}

//void iso_1_fileid(char *src)
//{
//    char *dot; /* Position of the last dot in the filename, will be used to 
//                  calculate lname and lext. */
//    int lname, lext, pos, i;
//
//    dot = strrchr(src, '.');
//
//    lext = dot ? strlen(dot + 1) : 0;
//    lname = strlen(src) - lext - (dot ? 1 : 0);
//
//    /* If we can't build a filename, return. */
//    if (lname == 0 && lext == 0) {
//        return;
//    }
//
//    pos = 0;
//    /* Convert up to 8 characters of the filename. */
//    for (i = 0; i < lname && i < 8; i++) {
//        char c = toupper(src[i]);
//        src[pos++] = valid_d_char(c) ? c : '_';
//    }
//    
//    /* This dot is mandatory, even if there is no extension. */
//    src[pos++] = '.';
//    
//    /* Convert up to 3 characters of the extension, if any. */
//    for (i = 0; i < lext && i < 3; i++) {
//        char c = toupper(src[lname + 1 + i]);
//        src[pos++] = valid_d_char(c) ? c : '_';
//    }
//    src[pos] = '\0';
//}
//
//void iso_2_fileid(char *src)
//{
//    char *dot;
//    int lname, lext, lnname, lnext, pos, i;
//
//    if (!src)
//        return;
//
//    dot = strrchr(src, '.');
//
//    /* 
//     * Since the maximum length can be divided freely over the name and
//     * extension, we need to calculate their new lengths (lnname and
//     * lnext). If the original filename is too long, we start by trimming
//     * the extension, but keep a minimum extension length of 3. 
//     */
//    if (dot == NULL || *(dot + 1) == '\0') {
//        lname = strlen(src);
//        lnname = (lname > 30) ? 30 : lname;
//        lext = lnext = 0;
//    } else {
//        lext = strlen(dot + 1);
//        lname = strlen(src) - lext - 1;
//        lnext = (strlen(src) > 31 && lext > 3)
//            ? (lname < 27 ? 30 - lname : 3) : lext;
//        lnname = (strlen(src) > 31) ? 30 - lnext : lname;
//    }
//
//    if (lnname == 0 && lnext == 0) {
//        return;
//    }
//
//    pos = 0;
//    /* Convert up to lnname characters of the filename. */
//    for (i = 0; i < lnname; i++) {
//        char c = toupper(src[i]);
//        src[pos++] = valid_d_char(c) ? c : '_';
//    }
//    src[pos++] = '.';
//    /* Convert up to lnext characters of the extension, if any. */
//    for (i = 0; i < lnext; i++) {
//        char c = toupper(src[lname + 1 + i]);
//        src[pos++] = valid_d_char(c) ? c : '_';
//    }
//    src[pos] = '\0';
//}
    
int str2d_char(const char *icharset, const char *input, char **output)
{
    int ret;
    char *ascii;
    size_t len, i;
    
    if (output == NULL) {
        return ISO_MEM_ERROR;
    }
    
    /** allow NULL input */
    if (input == NULL) {
        *output = NULL;
        return 0;
    }
    
    /* this checks for NULL parameters */
    ret = str2ascii(icharset, input, &ascii);
    if (ret < 0) {
        *output = NULL;
        return ret;
    }
    
    len = strlen(ascii);
    
    for (i = 0; i < len; ++i) {
        char c = toupper(ascii[i]);
        ascii[i] = valid_d_char(c) ? c : '_';
    }
    
    *output = ascii;
    return ISO_SUCCESS;
}
    
int str2a_char(const char *icharset, const char *input, char **output)
{
    int ret;
    char *ascii;
    size_t len, i;
    
    if (output == NULL) {
        return ISO_MEM_ERROR;
    }
    
    /** allow NULL input */
    if (input == NULL) {
        *output = NULL;
        return 0;
    }
    
    /* this checks for NULL parameters */
    ret = str2ascii(icharset, input, &ascii);
    if (ret < 0) {
        *output = NULL;
        return ret;
    }
    
    len = strlen(ascii);
    
    for (i = 0; i < len; ++i) {
        char c = toupper(ascii[i]);
        ascii[i] = valid_a_char(c) ? c : '_';
    }
    
    *output = ascii;
    return ISO_SUCCESS;
}

void iso_lsb(uint8_t *buf, uint32_t num, int bytes)
{
    int i;

    for (i = 0; i < bytes; ++i)
        buf[i] = (num >> (8 * i)) & 0xff;
}

void iso_msb(uint8_t *buf, uint32_t num, int bytes)
{
    int i;

    for (i = 0; i < bytes; ++i)
        buf[bytes - 1 - i] = (num >> (8 * i)) & 0xff;
}

void iso_bb(uint8_t *buf, uint32_t num, int bytes)
{
    iso_lsb(buf, num, bytes);
    iso_msb(buf+bytes, num, bytes);
}

uint32_t iso_read_lsb(const uint8_t *buf, int bytes)
{
    int i;
    uint32_t ret = 0;

    for (i=0; i<bytes; i++) {
        ret += ((uint32_t) buf[i]) << (i*8);
    }
    return ret;
}

uint32_t iso_read_msb(const uint8_t *buf, int bytes)
{
    int i;
    uint32_t ret = 0;

    for (i=0; i<bytes; i++) {
        ret += ((uint32_t) buf[bytes-i-1]) << (i*8);
    }
    return ret;
}

void iso_datetime_7(unsigned char *buf, time_t t)
{
    static int tzsetup = 0;
    int tzoffset;
    struct tm tm;

    if (!tzsetup) {
        tzset();
        tzsetup = 1;
    }

    localtime_r(&t, &tm);

    buf[0] = tm.tm_year;
    buf[1] = tm.tm_mon + 1;
    buf[2] = tm.tm_mday;
    buf[3] = tm.tm_hour;
    buf[4] = tm.tm_min;
    buf[5] = tm.tm_sec;
#ifdef HAVE_TM_GMTOFF
    tzoffset = tm.tm_gmtoff / 60 / 15;
#else
    tzoffset = timezone / 60 / 15;
#endif
    if (tzoffset > 52)
        tzoffset -= 101;
    buf[6] = tzoffset;
}

void iso_datetime_17(unsigned char *buf, time_t t)
{
    static int tzsetup = 0;
    static int tzoffset;
    struct tm tm;

    if (t == (time_t) - 1) {
        /* unspecified time */
        memset(buf, '0', 16);
        buf[16] = 0;
    } else {
        if (!tzsetup) {
            tzset();
            tzsetup = 1;
        }

        localtime_r(&t, &tm);

        sprintf((char*)&buf[0], "%04d", tm.tm_year + 1900);
        sprintf((char*)&buf[4], "%02d", tm.tm_mon + 1);
        sprintf((char*)&buf[6], "%02d", tm.tm_mday);
        sprintf((char*)&buf[8], "%02d", tm.tm_hour);
        sprintf((char*)&buf[10], "%02d", tm.tm_min);
        sprintf((char*)&buf[12], "%02d", MIN(59, tm.tm_sec));
        memcpy(&buf[14], "00", 2);
#ifdef HAVE_TM_GMTOFF
        tzoffset = tm.tm_gmtoff / 60 / 15;
#else
        tzoffset = timezone / 60 / 15;
#endif
        if (tzoffset > 52)
            tzoffset -= 101;
        buf[16] = tzoffset;
    }
}

time_t iso_datetime_read_7(const uint8_t *buf)
{
    struct tm tm;

    tm.tm_year = buf[0];
    tm.tm_mon = buf[1] - 1;
    tm.tm_mday = buf[2];
    tm.tm_hour = buf[3];
    tm.tm_min = buf[4];
    tm.tm_sec = buf[5];
    return timegm(&tm) - buf[6] * 60 * 15;
}

time_t iso_datetime_read_17(const uint8_t *buf)
{
    struct tm tm;

    sscanf((char*)&buf[0], "%4d", &tm.tm_year);
    sscanf((char*)&buf[4], "%2d", &tm.tm_mon);
    sscanf((char*)&buf[6], "%2d", &tm.tm_mday);
    sscanf((char*)&buf[8], "%2d", &tm.tm_hour);
    sscanf((char*)&buf[10], "%2d", &tm.tm_min);
    sscanf((char*)&buf[12], "%2d", &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    return timegm(&tm) - buf[16] * 60 * 15;
}
