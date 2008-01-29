/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

#ifndef LIBISO_FSOURCE_H_
#define LIBISO_FSOURCE_H_

/*
 * Definitions for the file sources. Most functions/structures related with
 * this were moved to libisofs.h.
 */

#include "libisofs.h"

#define ISO_LOCAL_FS_ID        1
#define ISO_IMAGE_FS_ID        2
#define ISO_ELTORITO_FS_ID     3
#define ISO_MEM_FS_ID          4

/**
 * Create a new IsoFilesystem to deal with local filesystem.
 * 
 * @return
 *     1 sucess, < 0 error
 */
int iso_local_filesystem_new(IsoFilesystem **fs);

#endif /*LIBISO_FSOURCE_H_*/