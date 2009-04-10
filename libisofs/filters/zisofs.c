/*
 * Copyright (c) 2009 Thomas Schmitt
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 *
 * It implements a filter facility which can pipe a IsoStream into zisofs
 * compression resp. uncompression, read its output and forward it as IsoStream
 * output to an IsoFile.
 * The zisofs format was invented by H. Peter Anvin. See doc/zisofs_format.txt
 * It is writeable and readable by zisofs-tools, readable by Linux kernels.
 *
 */

#include "../libisofs.h"
#include "../filter.h"
#include "../fsource.h"
#include "../util.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#ifdef Libisofs_with_zliB
#include <zlib.h>
#else
/* If zlib is not available then this code is a dummy */
#endif


/*
 * A filter that encodes or decodes the content of zisofs compressed files.
 */


/* --------------------------- ZisofsFilterRuntime ------------------------- */


/* Sizes to be used for compression. Decompression learns from input header. */
#define Libisofs_zisofs_block_log2 15
#define Libisofs_zisofs_block_sizE 32768


/* Individual runtime properties exist only as long as the stream is opened.
 */
typedef struct
{
    int state; /* processing: 0= header, 1= block pointers, 2= data blocks */

    int block_size;
    int block_pointer_fill;
    int block_pointer_rpos;
    uint32_t *block_pointers; /* These are in use only with uncompression.
                                 Compression streams hold the pointer in
                                 their persistent data.
                               */

    char *read_buffer;
    char *block_buffer;
    int buffer_size;
    int buffer_fill;
    int buffer_rpos;

    off_t block_counter;
    off_t in_counter;
    off_t out_counter;

    int error_ret;

} ZisofsFilterRuntime;


static
int ziso_running_destroy(ZisofsFilterRuntime **running, int flag)
{
    ZisofsFilterRuntime *o= *running;
    if (o == NULL)
        return 0;
    if (o->block_pointers != NULL)
        free(o->block_pointers);
    if (o->read_buffer != NULL)
        free(o->read_buffer);
    if (o->block_buffer != NULL)
        free(o->block_buffer);
    free((char *) o);
    *running = NULL;
    return 1;
}


/*
 * @param flag bit0= do not set block_size, do not allocate buffers
 */
static
int ziso_running_new(ZisofsFilterRuntime **running, int flag)
{
    ZisofsFilterRuntime *o;
    *running = o = calloc(sizeof(ZisofsFilterRuntime), 1);
    if (o == NULL) {
        return ISO_OUT_OF_MEM;
    }
    o->state = 0;
    o->block_size= 0;
    o->block_pointer_fill = 0;
    o->block_pointer_rpos = 0;
    o->block_pointers = NULL;
    o->read_buffer = NULL;
    o->block_buffer = NULL;
    o->buffer_size = 0;
    o->buffer_fill = 0;
    o->buffer_rpos = 0;
    o->block_counter = 0;
    o->in_counter = 0;
    o->out_counter = 0;
    o->error_ret = 0;

    if (flag & 1)
        return 1;

    o->block_size = Libisofs_zisofs_block_sizE;
#ifdef Libisofs_with_zliB
    o->buffer_size= compressBound((uLong) Libisofs_zisofs_block_sizE);
#else
    o->buffer_size= 2 * Libisofs_zisofs_block_sizE;
#endif
    o->read_buffer = calloc(o->block_size, 1);
    o->block_buffer = calloc(o->buffer_size, 1);
    if (o->block_buffer == NULL || o->read_buffer == NULL)
        goto failed;
    return 1;
failed:
    ziso_running_destroy(running, 0);
    return -1;
}


/* ---------------------------- ZisofsFilterStreamData --------------------- */

/* The first 8 bytes of a zisofs compressed data file */
static unsigned char zisofs_magic[9] =
                              {0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07};


/*
 * The common data payload of an individual Zisofs Filter IsoStream
 */
typedef struct
{
    IsoStream *orig;

    off_t size; /* -1 means that the size is unknown yet */

    ZisofsFilterRuntime *running; /* is non-NULL when open */

    ino_t id;

} ZisofsFilterStreamData;


/*
 * The data payload of an individual Zisofs Filter Compressor IsoStream
 */
typedef struct
{   
    ZisofsFilterStreamData std;

    uint32_t orig_size;
    uint32_t *block_pointers; /* Cache for output block addresses. They get
                                 written before the data and so need 2 passes.
                                 This cache avoids surplus passes.
                               */
} ZisofsComprStreamData;


/*
 * The data payload of an individual Zisofs Filter Uncompressor IsoStream
 */
typedef struct
{
    ZisofsFilterStreamData std;

    unsigned char header_size_div4;
    unsigned char block_size_log2;

} ZisofsUncomprStreamData;


/* Each individual ZisofsFilterStreamData needs a unique id number. */
/* >>> This is very suboptimal:
       The counter can rollover.
*/
static ino_t ziso_ino_id = 0;


/*
 * Methods for the IsoStreamIface of an External Filter object.
 */

static
int ziso_stream_uncompress(IsoStream *stream, void *buf, size_t desired);

/*
 * @param flag  bit0= original stream is not open
 */
static
int ziso_stream_close_flag(IsoStream *stream, int flag)
{
    ZisofsFilterStreamData *data;

    if (stream == NULL) {
        return ISO_NULL_POINTER;
    }
    data = stream->data;

    if (data->running == NULL) {
        return 1;
    }
    ziso_running_destroy(&(data->running), 0);
    if (flag & 1)
        return 1;
    return iso_stream_close(data->orig);
}


static
int ziso_stream_close(IsoStream *stream)
{
    return ziso_stream_close_flag(stream, 0);
}


/*
 * @param flag  bit0= do not run .get_size() if size is < 0
 */
static
int ziso_stream_open_flag(IsoStream *stream, int flag)
{
    ZisofsFilterStreamData *data;
    ZisofsFilterRuntime *running = NULL;
    int ret;

    if (stream == NULL) {
        return ISO_NULL_POINTER;
    }
    data = (ZisofsFilterStreamData*) stream->data;
    if (data->running != NULL) {
        return ISO_FILE_ALREADY_OPENED;
    }
    if (data->size < 0 && !(flag & 1)) {
        /* Do the size determination run now, so that the size gets cached
           and .get_size() will not fail on an opened stream.
        */
        stream->class->get_size(stream);
    }

    ret = ziso_running_new(&running,
                           stream->class->read == &ziso_stream_uncompress);
    if (ret < 0) {
        return ret;
    }
    data->running = running;

    ret = iso_stream_open(data->orig);
    if (ret < 0) {
        return ret;
    }
    return 1;
}


static
int ziso_stream_open(IsoStream *stream)
{
    return ziso_stream_open_flag(stream, 0);
}


static
int ziso_stream_compress(IsoStream *stream, void *buf, size_t desired)
{

#ifdef Libisofs_with_zliB

    int ret, todo, i;
    ZisofsComprStreamData *data;
    ZisofsFilterRuntime *rng;
    size_t fill = 0;
    off_t orig_size, next_pt;
    char *cbuf = buf;
    uLongf buf_len;

    if (stream == NULL) {
        return ISO_NULL_POINTER;
    }
    data = stream->data;
    rng= data->std.running;
    if (rng == NULL) {
        return ISO_FILE_NOT_OPENED;
    }
    if (rng->error_ret < 0) {
        return rng->error_ret;
    }

    while (1) {
        if (rng->state == 0) {
            /* Delivering file header */

            if (rng->buffer_fill == 0) {
                memcpy(rng->block_buffer, zisofs_magic, 8);
                orig_size = iso_stream_get_size(data->std.orig);
                if (orig_size > 4294967295.0) {
                    return (rng->error_ret = ISO_ZISOFS_TOO_LARGE);
                }
                data->orig_size = orig_size;
                iso_lsb((unsigned char *) (rng->block_buffer + 8),
                        (uint32_t) orig_size, 4);
                rng->block_buffer[12] = 4;
                rng->block_buffer[13] = Libisofs_zisofs_block_log2;
                rng->block_buffer[14] = rng->block_buffer[15] = 0;
                rng->buffer_fill = 16;
                rng->buffer_rpos = 0;
            } else if (rng->buffer_rpos >= rng->buffer_fill) {
                rng->buffer_fill = rng->buffer_rpos = 0;
                rng->state = 1; /* header is delivered */
            }
        }
        if (rng->state == 1) {
            /* Delivering block pointers */;

            if (rng->block_pointer_fill == 0) {
                /* Initialize block pointer writing */
                rng->block_pointer_rpos = 0;
                rng->block_pointer_fill = data->orig_size / rng->block_size
                                   + 1 + !!(data->orig_size % rng->block_size);
                if (data->block_pointers == NULL) {
                    /* On the first pass, create pointer array with all 0s */
                    data->block_pointers = calloc(rng->block_pointer_fill, 4);
                    if (data->block_pointers == NULL) {
                        rng->block_pointer_fill = 0;
                        return (rng->error_ret = ISO_OUT_OF_MEM);
                    }
                }
            }
            if (rng->buffer_rpos >= rng->buffer_fill) {
                if (rng->block_pointer_rpos >= rng->block_pointer_fill) {
                    rng->buffer_fill = rng->buffer_rpos = 0;
                    rng->block_counter = 0;
                    data->block_pointers[0] = 16 + rng->block_pointer_fill * 4;
                    rng->state = 2; /* block pointers are delivered */
                } else {
                    /* Provide a buffer full of block pointers */
                    todo = rng->block_pointer_fill - rng->block_pointer_rpos;
                    if (todo * 4 > rng->buffer_size)
                        todo = rng->buffer_size / 4;
                    memcpy(rng->block_buffer,
                           data->block_pointers + 4 * rng->block_pointer_rpos,
                           todo * 4);
                    rng->buffer_rpos = 0;
                    rng->buffer_fill = todo * 4;
                    rng->block_pointer_rpos += todo;
                }
            }
        }
        if (rng->state == 2 && rng->buffer_rpos >= rng->buffer_fill) {
            /* Delivering data blocks */;

            ret = iso_stream_read(data->std.orig, rng->read_buffer,
                                  rng->block_size);
            if (ret > 0) {
                rng->in_counter += ret;
                if (rng->in_counter > data->orig_size) {
                    /* Input size became larger */
                    return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
                }
                /* Check whether all 0 : represent as 0-length block */;
                for (i = 0; i < ret; i++)
                    if (rng->read_buffer[i])
                break;
                if (i >= ret) { /* All 0-bytes. Bypass compression. */
                    buf_len = 0;
                } else {
                    buf_len = rng->buffer_size;
                    ret = compress2((Bytef *) rng->block_buffer, &buf_len,
                                   (Bytef *) rng->read_buffer, (uLong) ret, 9);
                    if (ret != Z_OK) {
                        return (rng->error_ret = ISO_ZLIB_COMPR_ERR);
                    }
                }
                rng->buffer_fill = buf_len;
                rng->buffer_rpos = 0;

                next_pt = data->block_pointers[rng->block_counter] + buf_len;

                if (data->std.size >= 0 && next_pt > data->std.size) {
                    /* Compression yields more bytes than on first run */
                    return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
                }

                /* Record resp. check block pointer */
                rng->block_counter++;
                if (data->block_pointers[rng->block_counter] > 0) {
                    if (next_pt != data->block_pointers[rng->block_counter] ) {
                        /* block pointers mismatch , content has changed */
                        return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
                    }
                } else {
                    data->block_pointers[rng->block_counter] = next_pt;
                }

            } else if (ret == 0) {
                rng->state = 3;
                if (rng->in_counter != data->orig_size) {
                    /* Input size shrunk */
                    return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
                }
                return fill;
            } else
                return (rng->error_ret = ret);
            if (rng->buffer_fill == 0) {
    continue;
            }
        }
        if (rng->state == 3 && rng->buffer_rpos >= rng->buffer_fill) {
            return 0; /* EOF */
        }

        /* Transfer from rng->block_buffer to buf */
        todo = desired - fill;
        if (todo > rng->buffer_fill - rng->buffer_rpos)
            todo = rng->buffer_fill - rng->buffer_rpos;
        memcpy(cbuf + fill, rng->block_buffer + rng->buffer_rpos, todo);
        fill += todo;
        rng->buffer_rpos += todo;
        rng->out_counter += todo;

        if (fill >= desired) {
           return fill;
        }
    }
    return ISO_FILE_READ_ERROR; /* should never be hit */

#else

    return ISO_ZLIB_NOT_ENABLED;

#endif

}


/* Note: A call with desired==0 directly after .open() only checks the file
         head and loads the uncompressed size from that head.
*/
static
int ziso_stream_uncompress(IsoStream *stream, void *buf, size_t desired)
{

#ifdef Libisofs_with_zliB

    int ret, todo, i, header_size, bs_log2, block_max = 1;
    ZisofsFilterStreamData *data;
    ZisofsFilterRuntime *rng;
    ZisofsUncomprStreamData *nstd;
    size_t fill = 0;
    char *cbuf = buf;
    uLongf buf_len;
    char zisofs_head[16];

    if (stream == NULL) {
        return ISO_NULL_POINTER;
    }
    data = stream->data;
    nstd = stream->data;
    rng= data->running;
    if (rng == NULL) {
        return ISO_FILE_NOT_OPENED;
    }
    if (rng->error_ret < 0) {
        return rng->error_ret;
    }

    while (1) {
        if (rng->state == 0) {
            /* Reading file header */
            ret = iso_stream_read(data->orig, zisofs_head, 16);
            if (ret < 0)
                return (rng->error_ret = ret);
            header_size = ((unsigned char *) zisofs_head)[12] * 4;
            bs_log2 = ((unsigned char *) zisofs_head)[13];
            if (ret != 16 || memcmp(zisofs_head, zisofs_magic, 8) != 0 ||
                header_size < 16 || bs_log2 < 15 || bs_log2 > 17) {
                return (rng->error_ret = ISO_ZISOFS_WRONG_INPUT);
            }
            rng->block_size = 1 << bs_log2;
            if (header_size > 16) {
                /* Skip surplus header bytes */
                ret = iso_stream_read(data->orig, zisofs_head, header_size-16);
                if (ret < 0)
                    return (rng->error_ret = ret);
                if (ret != header_size - 16)
                   return (rng->error_ret = ISO_ZISOFS_WRONG_INPUT); 
            }
            data->size = iso_read_lsb(((uint8_t *) zisofs_head) + 8, 4);
            nstd->header_size_div4 = header_size / 4;
            nstd->block_size_log2 = bs_log2;

            if (desired == 0) {
                return 0;
            }

            /* Create and read pointer array */
            rng->block_pointer_rpos = 0;
            rng->block_pointer_fill = data->size / rng->block_size
                                     + 1 + !!(data->size % rng->block_size);
            rng->block_pointers = calloc(rng->block_pointer_fill, 4);
            if (rng->block_pointers == NULL) {
                rng->block_pointer_fill = 0;
                return (rng->error_ret = ISO_OUT_OF_MEM);
            }
            ret = iso_stream_read(data->orig, rng->block_pointers,
                                  rng->block_pointer_fill * 4);
            if (ret < 0)
                return (rng->error_ret = ret);
            if (ret != rng->block_pointer_fill * 4)
               return (rng->error_ret = ISO_ZISOFS_WRONG_INPUT);
            for (i = 0; i < rng->block_pointer_fill; i++) {
                 rng->block_pointers[i] =
                      iso_read_lsb((uint8_t *) (rng->block_pointers + i), 4);
                 if (i > 0)
                     if (rng->block_pointers[i] - rng->block_pointers[i - 1]
                         > block_max)
                         block_max = rng->block_pointers[i]
                                     - rng->block_pointers[i - 1];
            }

            rng->read_buffer = calloc(block_max, 1);
            rng->block_buffer = calloc(rng->block_size, 1);
            if (rng->read_buffer == NULL || rng->block_buffer == NULL)
                return (rng->error_ret = ISO_OUT_OF_MEM);
            rng->state = 2; /* block pointers are read */
            rng->buffer_fill = rng->buffer_rpos = 0;
        }

        if (rng->state == 2 && rng->buffer_rpos >= rng->buffer_fill) {
            /* Delivering data blocks */;
            i = ++(rng->block_pointer_rpos);
            if (i >= rng->block_pointer_fill) {
                /* More data blocks than announced */
                return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
            }
            todo = rng->block_pointers[i] - rng->block_pointers[i- 1];
            if (todo == 0) {
                memset(rng->block_buffer, 0, rng->block_size);
                rng->buffer_fill = rng->block_size;
                if (rng->in_counter + rng->buffer_fill > data->size &&
                    i == rng->block_pointer_fill - 1)
                    rng->buffer_fill = data->size - rng->in_counter;
                rng->in_counter += rng->buffer_fill;
            } else {
                ret = iso_stream_read(data->orig, rng->read_buffer, todo);
                if (ret > 0) {
                    rng->in_counter += ret;
                    buf_len = rng->block_size;
                    ret = uncompress((Bytef *) rng->block_buffer, &buf_len,
                                     (Bytef *) rng->read_buffer, (uLong) ret);
                    if (ret != Z_OK)
                        return (rng->error_ret = ISO_ZLIB_COMPR_ERR);
                    rng->buffer_fill = buf_len;
                    if (buf_len < rng->block_size &&
                        i != rng->block_pointer_fill - 1)
                        return (rng->error_ret = ISO_ZISOFS_WRONG_INPUT);
                } else if(ret == 0) {
                    rng->state = 3;
                    if (rng->out_counter != data->size) {
                        /* Input size shrunk */
                        return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
                    }
                    return fill;
                } else
                    return (rng->error_ret = ret);
            }
            rng->buffer_rpos = 0;

            if (rng->out_counter + rng->buffer_fill > data->size) {
                /* Uncompression yields more bytes than announced by header */
                return (rng->error_ret = ISO_FILTER_WRONG_INPUT);
            }
        }
        if (rng->state == 3 && rng->buffer_rpos >= rng->buffer_fill) {
            return 0; /* EOF */
        }

        /* Transfer from rng->block_buffer to buf */
        todo = desired - fill;
        if (todo > rng->buffer_fill - rng->buffer_rpos)
            todo = rng->buffer_fill - rng->buffer_rpos;
        memcpy(cbuf + fill, rng->block_buffer + rng->buffer_rpos, todo);
        fill += todo;
        rng->buffer_rpos += todo;
        rng->out_counter += todo;

        if (fill >= desired) {
           return fill;
        }
    }
    return (rng->error_ret = ISO_FILE_READ_ERROR); /* should never be hit */

#else

    return ISO_ZLIB_NOT_ENABLED;

#endif

}


static
off_t ziso_stream_get_size(IsoStream *stream)
{
    int ret, ret_close;
    off_t count = 0;
    ZisofsFilterStreamData *data;
    char buf[64 * 1024];
    size_t bufsize = 64 * 1024;

    if (stream == NULL) {
        return ISO_NULL_POINTER;
    }
    data = stream->data;

    if (data->size >= 0) {
        return data->size;
    }

    /* Run filter command and count output bytes */
    ret = ziso_stream_open_flag(stream, 1);
    if (ret < 0) {
        return ret;
    }
    if (stream->class->read == &ziso_stream_uncompress) {
        /* It is enough to read the header part of a compressed file */
        ret = ziso_stream_uncompress(stream, buf, 0);
        count = data->size;
    } else {
        /* The size of the compression result has to be counted */
        while (1) {
            ret = stream->class->read(stream, buf, bufsize);
            if (ret <= 0)
        break;
            count += ret;
        }
    }
    ret_close = ziso_stream_close(stream);
    if (ret < 0)
        return ret;
    if (ret_close < 0)
        return ret_close;

    data->size = count;
    return count;
}


static
int ziso_stream_is_repeatable(IsoStream *stream)
{
    /* Only repeatable streams are accepted as orig */
    return 1;
}


static
void ziso_stream_get_id(IsoStream *stream, unsigned int *fs_id, 
                        dev_t *dev_id, ino_t *ino_id)
{
    ZisofsFilterStreamData *data;

    data = stream->data;
    *fs_id = ISO_FILTER_FS_ID;
    *dev_id = ISO_FILTER_ZISOFS_DEV_ID;
    *ino_id = data->id;
}


static
void ziso_stream_free(IsoStream *stream)
{
    ZisofsFilterStreamData *data;

    if (stream == NULL) {
        return;
    }
    data = stream->data;
    if (data->running != NULL) {
        ziso_stream_close(stream);
    }
    if (stream->class->read != &ziso_stream_uncompress) {
        ZisofsComprStreamData *nstd;
        nstd = stream->data;
        if (nstd->block_pointers != NULL)
            free((char *) nstd->block_pointers);
    }
    iso_stream_unref(data->orig);
    free(data);
}


static
int ziso_update_size(IsoStream *stream)
{
    /* By principle size is determined only once */
    return 1;
}


static
IsoStream *ziso_get_input_stream(IsoStream *stream, int flag)
{
    ZisofsFilterStreamData *data;

    if (stream == NULL) {
        return NULL;
    }
    data = stream->data;
    return data->orig;
}


IsoStreamIface ziso_stream_compress_class = {
    2,
    "ziso",
    ziso_stream_open,
    ziso_stream_close,
    ziso_stream_get_size,
    ziso_stream_compress,
    ziso_stream_is_repeatable,
    ziso_stream_get_id,
    ziso_stream_free,
    ziso_update_size,
    ziso_get_input_stream
};


IsoStreamIface ziso_stream_uncompress_class = {
    2,
    "osiz",
    ziso_stream_open,
    ziso_stream_close,
    ziso_stream_get_size,
    ziso_stream_uncompress,
    ziso_stream_is_repeatable,
    ziso_stream_get_id,
    ziso_stream_free,
    ziso_update_size,
    ziso_get_input_stream
};


static
void ziso_filter_free(FilterContext *filter)
{
    /* no data are allocated */;
}


/*
 * @param flag bit1= Install a decompression filter
 */
static
int ziso_filter_get_filter(FilterContext *filter, IsoStream *original, 
                           IsoStream **filtered, int flag)
{
    IsoStream *str;
    ZisofsFilterStreamData *data;
    ZisofsComprStreamData *cnstd;
    ZisofsUncomprStreamData *unstd;

    if (filter == NULL || original == NULL || filtered == NULL) {
        return ISO_NULL_POINTER;
    }

    str = calloc(sizeof(IsoStream), 1);
    if (str == NULL) {
        return ISO_OUT_OF_MEM;
    }
    if (flag & 2) {
        unstd = calloc(sizeof(ZisofsUncomprStreamData), 1);
        data = (ZisofsFilterStreamData *) unstd;
    } else {
        cnstd = calloc(sizeof(ZisofsComprStreamData), 1);
        data = (ZisofsFilterStreamData *) cnstd;
    }
    if (data == NULL) {
        free(str);
        return ISO_OUT_OF_MEM;
    }

    /* These data items are not owned by this filter object */
    data->id = ++ziso_ino_id;
    data->orig = original;
    data->size = -1;
    data->running = NULL;

    /* get reference to the source */
    iso_stream_ref(data->orig);

    str->refcount = 1;
    str->data = data;
    if (flag & 2) {
        unstd->header_size_div4 = 0;
        unstd->block_size_log2 = 0;
        str->class = &ziso_stream_uncompress_class;
    } else {
        cnstd->orig_size = 0;
        cnstd->block_pointers = NULL;
        str->class = &ziso_stream_compress_class;
    }

    *filtered = str;

    return ISO_SUCCESS;
}


/* To be called by iso_file_add_filter().
 * The FilterContext input parameter is not furtherly needed for the 
 * emerging IsoStream.
 */
static
int ziso_filter_get_compressor(FilterContext *filter, IsoStream *original, 
                               IsoStream **filtered)
{
    return ziso_filter_get_filter(filter, original, filtered, 0);
}

static
int ziso_filter_get_uncompressor(FilterContext *filter, IsoStream *original, 
                                 IsoStream **filtered)
{
    return ziso_filter_get_filter(filter, original, filtered, 2);
}


/* Produce a parameter object suitable for iso_file_add_filter().
 * It may be disposed by free() after all those calls are made.
 *
 * This is quite a dummy as it does not carry individual data.
 * @param flag bit1= Install a decompression filter
 */
static
int ziso_create_context(FilterContext **filter, int flag)
{
    FilterContext *f;
    
    *filter = f = calloc(1, sizeof(FilterContext));
    if (f == NULL) {
        return ISO_OUT_OF_MEM;
    }
    f->refcount = 1;
    f->version = 0;
    f->data = NULL;
    f->free = ziso_filter_free;
    if (flag & 2)
        f->get_filter = ziso_filter_get_uncompressor;
    else
        f->get_filter = ziso_filter_get_compressor;
    return ISO_SUCCESS;
}


/*
 * @param flag bit0= if_block_reduction rather than if_reduction
 *             bit1= Install a decompression filter
 *             bit2= only inquire availability of zisofs filtering
 */
int iso_file_add_zisofs_filter(IsoFile *file, int flag)
{

#ifdef Libisofs_with_zliB

    int ret;
    FilterContext *f = NULL;
    IsoStream *stream;
    off_t original_size = 0, filtered_size = 0;

    if (flag & 4)
        return 2;

    original_size = iso_file_get_size(file);
    if (!(flag & 2)) {
        if (original_size <= 0) {
            return 2;
        }
        if (original_size > 4294967295.0) {
            return ISO_ZISOFS_TOO_LARGE;
        }
    }

    ret = ziso_create_context(&f, flag & 2);
    if (ret < 0) {
        return ret;
    }
    ret = iso_file_add_filter(file, f, 0);
    free(f);
    if (ret < 0) {
        return ret;
    }
    /* Run a full filter process getsize so that the size is cached */
    stream = iso_file_get_stream(file);
    filtered_size = iso_stream_get_size(stream);
    if (filtered_size < 0) {
        iso_file_remove_filter(file, 0);
        return filtered_size;
    }
    if ((filtered_size >= original_size ||
        ((flag & 1) && filtered_size / 2048 >= original_size / 2048))
        && !(flag & 2)){
        ret = iso_file_remove_filter(file, 0);
        if (ret < 0) {
            return ret;
        }
        return 2;
    }
    return ISO_SUCCESS;

#else

    return ISO_ZLIB_NOT_ENABLED;

#endif /* ! Libisofs_with_zliB */

}


/* Determine stream type : 1=ziso , -1=osiz , 0=other 
   and eventual ZF field parameters
*/
int ziso_is_zisofs_stream(IsoStream *stream, int *stream_type,
                          int *header_size_div4, int *block_size_log2,
                          uint32_t *uncompressed_size, int flag)
{
    ZisofsFilterStreamData *data;
    ZisofsComprStreamData *cnstd;
    ZisofsUncomprStreamData *unstd;

    *stream_type = 0; 
    if (stream->class == &ziso_stream_compress_class) {
        *stream_type = 1;
        cnstd = stream->data;
        *header_size_div4 = 4;
        *block_size_log2 = Libisofs_zisofs_block_log2;
        *uncompressed_size = cnstd->orig_size;
        return 1;
    } else if(stream->class == &ziso_stream_uncompress_class) {
        *stream_type = -1;
        data = stream->data;
        unstd = stream->data;
        *header_size_div4 = unstd->header_size_div4;
        *block_size_log2 = unstd->block_size_log2;
        *uncompressed_size = data->size;
        return 1;
    }
    return 0;
}

