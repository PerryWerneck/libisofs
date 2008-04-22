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

#endif /*STREAM_H_*/
