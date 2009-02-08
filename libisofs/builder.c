/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

/* ts A90116 : libisofs.h eventually defines aaip_xinfo_func */
#include "libisofs.h"

#include "builder.h"
#include "node.h"
#include "fsource.h"

/* ts A90121 : needed for image->builder_ignore_acl */
#include "image.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ts A90207 */
#ifdef Libisofs_with_aaiP
#include "aaip_0_2.h"
#endif


void iso_node_builder_ref(IsoNodeBuilder *builder)
{
    ++builder->refcount;
}

void iso_node_builder_unref(IsoNodeBuilder *builder)
{
    if (--builder->refcount == 0) {
        /* free private data */
        builder->free(builder);
        free(builder);
    }
}

static
int default_create_file(IsoNodeBuilder *builder, IsoImage *image,
                        IsoFileSource *src, IsoFile **file)
{
    int ret;
    struct stat info;
    IsoStream *stream;
    IsoFile *node;
    char *name;

    if (builder == NULL || src == NULL || file == NULL) {
        return ISO_NULL_POINTER;
    }

    ret = iso_file_source_stat(src, &info);
    if (ret < 0) {
        return ret;
    }

    /* this will fail if src is a dir, is not accessible... */
    ret = iso_file_source_stream_new(src, &stream);
    if (ret < 0) {
        return ret;
    }

    /* take a ref to the src, as stream has taken our ref */
    iso_file_source_ref(src);
    
    name = iso_file_source_get_name(src);
    ret = iso_node_new_file(name, stream, &node);
    if (ret < 0) {
        iso_stream_unref(stream);
        free(name);
        return ret;
    }

    /* fill node fields */
    iso_node_set_permissions((IsoNode*)node, info.st_mode);
    iso_node_set_uid((IsoNode*)node, info.st_uid);
    iso_node_set_gid((IsoNode*)node, info.st_gid);
    iso_node_set_atime((IsoNode*)node, info.st_atime);
    iso_node_set_mtime((IsoNode*)node, info.st_mtime);
    iso_node_set_ctime((IsoNode*)node, info.st_ctime);
    iso_node_set_uid((IsoNode*)node, info.st_uid);

    *file = node;
    return ISO_SUCCESS;
}

static
int default_create_node(IsoNodeBuilder *builder, IsoImage *image,
                        IsoFileSource *src, IsoNode **node)
{
    int ret;
    struct stat info;
    IsoNode *new;
    char *name;

#ifdef Libisofs_with_aaiP
    unsigned char *aa_string;
    char *a_text = NULL, *d_text = NULL;
#endif /* Libisofs_with_aaiP */

    if (builder == NULL || src == NULL || node == NULL) {
        return ISO_NULL_POINTER;
    }

    /* get info about source */
    if (iso_tree_get_follow_symlinks(image)) {
        ret = iso_file_source_stat(src, &info);
    } else {
        ret = iso_file_source_lstat(src, &info);
    }
    if (ret < 0) {
        return ret;
    }

    name = iso_file_source_get_name(src);
    new = NULL;

    switch (info.st_mode & S_IFMT) {
    case S_IFREG:
        {
            /* source is a regular file */
            IsoStream *stream;
            IsoFile *file;
            ret = iso_file_source_stream_new(src, &stream);
            if (ret < 0) {
                break;
            }
            /* take a ref to the src, as stream has taken our ref */
            iso_file_source_ref(src);
            
            /* create the file */
            ret = iso_node_new_file(name, stream, &file);
            if (ret < 0) {
                iso_stream_unref(stream);
            }
            new = (IsoNode*) file;
        }
        break;
    case S_IFDIR:
        {
            /* source is a directory */
            IsoDir *dir;
            ret = iso_node_new_dir(name, &dir);
            new = (IsoNode*)dir;
        }
        break;
    case S_IFLNK:
        {
            /* source is a symbolic link */
            char dest[PATH_MAX];
            IsoSymlink *link;

            ret = iso_file_source_readlink(src, dest, PATH_MAX);
            if (ret < 0) {
                break;
            }
            ret = iso_node_new_symlink(name, strdup(dest), &link);
            new = (IsoNode*) link;
        }
        break;
    case S_IFSOCK:
    case S_IFBLK:
    case S_IFCHR:
    case S_IFIFO:
        {
            /* source is an special file */
            IsoSpecial *special;
            ret = iso_node_new_special(name, info.st_mode, info.st_rdev, 
                                       &special);
            new = (IsoNode*) special;
        }
        break;
    }
    
    if (ret < 0) {
        free(name);
        return ret;
    }

    /* fill fields */
    iso_node_set_perms_internal(new, info.st_mode, 1);
    iso_node_set_uid(new, info.st_uid);
    iso_node_set_gid(new, info.st_gid);
    iso_node_set_atime(new, info.st_atime);
    iso_node_set_mtime(new, info.st_mtime);
    iso_node_set_ctime(new, info.st_ctime);
    iso_node_set_uid(new, info.st_uid);

#ifdef Libisofs_with_aaiP

    /* ts A90207 */
    /* Eventually set S_IRWXG from ACL */
    if (image->builder_ignore_acl) {
        ret = iso_file_source_get_aa_string(src, &aa_string, 4);
        if (aa_string != NULL)
            iso_aa_get_acl_text(aa_string, info.st_mode, &a_text, &d_text, 16);
        if (a_text != NULL) {
            aaip_cleanout_st_mode(a_text, &(info.st_mode), 4 | 16);
            iso_node_set_perms_internal(new, info.st_mode, 1);
        }
        iso_aa_get_acl_text(aa_string, info.st_mode, &a_text, &d_text,
                            1 << 15); /* free ACL texts */
    }

    /* ts A90115 */
    /* obtain ownership of eventual AA string */
    ret = iso_file_source_get_aa_string(src, &aa_string,
            1 | (image->builder_ignore_acl << 1) |
                (image->builder_ignore_ea << 2 ));
    if (ret == 1 && aa_string != NULL) {
        ret = iso_node_add_xinfo(new, aaip_xinfo_func, aa_string);
        if (ret < 0)
            return ret;
    }

#endif /* Libisofs_with_aaiP */

    *node = new;

    return ISO_SUCCESS;
}

static
void default_free(IsoNodeBuilder *builder)
{
    return;
}

int iso_node_basic_builder_new(IsoNodeBuilder **builder)
{
    IsoNodeBuilder *b;

    if (builder == NULL) {
        return ISO_NULL_POINTER;
    }

    b = malloc(sizeof(IsoNodeBuilder));
    if (b == NULL) {
        return ISO_OUT_OF_MEM;
    }

    b->refcount = 1;
    b->create_file_data = NULL;
    b->create_node_data = NULL;
    b->create_file = default_create_file;
    b->create_node = default_create_node;
    b->free = default_free;

    *builder = b;
    return ISO_SUCCESS;
}
