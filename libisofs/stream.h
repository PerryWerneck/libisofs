/*
 * Copyright (c) 2007 Vreixo Formoso
 *
 * This file is part of the libisofs project; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See COPYING file for details.
 */
#ifndef LIBISO_STREAM_H_
#define LIBISO_STREAM_H_

/*
 * Definitions of streams.
 */
#include "fsource.h"

typedef struct
{
    IsoFileSource *src;

    /* key for file identification inside filesystem */
    dev_t dev_id;
    ino_t ino_id;
    off_t size; /**< size of this file */
} FSrcStreamData;

/**
 * Get an identifier for the file of the source, for debug purposes
 * @param name
 *      Should provide at least PATH_MAX bytes
 */
void iso_stream_get_file_name(IsoStream *stream, char *name);

/**
 * Create a stream to read from a IsoFileSource.
 * The stream will take the ref. to the IsoFileSource, so after a successfully
 * exectution of this function, you musn't unref() the source, unless you
 * take an extra ref.
 *
 * @return
 *      1 sucess, < 0 error
 *      Possible errors:
 *
 */
int iso_file_source_stream_new(IsoFileSource *src, IsoStream **stream);

/**
 * Create a new stream to read a chunk of an IsoFileSource..
 * The stream will add a ref. to the IsoFileSource.
 *
 * @return
 *      1 sucess, < 0 error
 */
int iso_cut_out_stream_new(IsoFileSource *src, off_t offset, off_t size,
                           IsoStream **stream);

/**
 * Create a stream for reading from a arbitrary memory buffer.
 * When the Stream refcount reach 0, the buffer is free(3).
 *
 * @return
 *      1 sucess, < 0 error
 */
int iso_memory_stream_new(unsigned char *buf, size_t size, IsoStream **stream);

/**
 * Obtain eventual zisofs ZF field entry parameters from a file source out
 * of a loaded ISO image.
 * To make hope for non-zero reply the stream has to be the original stream
 * of an IsoFile with .from_old_session==1. The call is safe with any stream
 * type, though, unless fsrc_stream_class would be used without FSrcStreamData.
 * @return  1= returned parameters are valid, 0=no ZF info found , <0 error
 */
int iso_stream_get_src_zf(IsoStream *stream, int *header_size_div4,
                          int *block_size_log2, uint32_t *uncompressed_size,
                          int flag);

#endif /*STREAM_H_*/
