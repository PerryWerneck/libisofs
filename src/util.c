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

int div_up(int n, int div)
{
    return (n + div - 1) / div;
}

int round_up(int n, int mul)
{
    return div_up(n, mul) * mul;
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
        
        if( errno != EINVAL ) {
            /* error, should never occur */
            iconv_close(conv);
            free(wstr);
            return ISO_CHARSET_CONV_ERROR;
        }
            
        /* invalid input string charset, just ignore */
        /* printf("String %s is not encoded in %s\n", str, codeset); */
        inbytes--;

        if (!inbytes)
            break;
        n = iconv(conv, &src, &inbytes, &ret, &outbytes);
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

void iso_dirid(char *src, int maxlen)
{
    size_t len, i;

    len = strlen(src);
    if (len > maxlen) {
        src[maxlen] = '\0';
        len = maxlen;
    }
    for (i = 0; i < len; i++) {
        char c = toupper(src[i]);
        src[i] = valid_d_char(c) ? c : '_';
    }
}

void iso_1_fileid(char *src)
{
    char *dot; /* Position of the last dot in the filename, will be used to 
                  calculate lname and lext. */
    int lname, lext, pos, i;

    dot = strrchr(src, '.');

    lext = dot ? strlen(dot + 1) : 0;
    lname = strlen(src) - lext - (dot ? 1 : 0);

    /* If we can't build a filename, return. */
    if (lname == 0 && lext == 0) {
        return;
    }

    pos = 0;
    /* Convert up to 8 characters of the filename. */
    for (i = 0; i < lname && i < 8; i++) {
        char c = toupper(src[i]);
        src[pos++] = valid_d_char(c) ? c : '_';
    }
    
    /* This dot is mandatory, even if there is no extension. */
    src[pos++] = '.';
    
    /* Convert up to 3 characters of the extension, if any. */
    for (i = 0; i < lext && i < 3; i++) {
        char c = toupper(src[lname + 1 + i]);
        src[pos++] = valid_d_char(c) ? c : '_';
    }
    src[pos] = '\0';
}

void iso_2_fileid(char *src)
{
    char *dot;
    int lname, lext, lnname, lnext, pos, i;

    if (!src)
        return;

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
        return;
    }

    pos = 0;
    /* Convert up to lnname characters of the filename. */
    for (i = 0; i < lnname; i++) {
        char c = toupper(src[i]);
        src[pos++] = valid_d_char(c) ? c : '_';
    }
    src[pos++] = '.';
    /* Convert up to lnext characters of the extension, if any. */
    for (i = 0; i < lnext; i++) {
        char c = toupper(src[lname + 1 + i]);
        src[pos++] = valid_d_char(c) ? c : '_';
    }
    src[pos] = '\0';
}