/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

#include "iso1999.h"
#include "messages.h"
#include "writer.h"
#include "error.h"
#include "image.h"
#include "filesrc.h"
#include "eltorito.h"

#include <stdlib.h>
#include <string.h>

static
char *get_iso1999_name(Ecma119Image *t, const char *str)
{
    int ret;
    char *name;

    if (str == NULL) {
        /* not an error, can be root node */
        return NULL;
    }
    
    if (!strcmp(t->input_charset, t->output_charset)) {
        /* no conversion needed */
        name = strdup(str);
    } else {
        ret = strconv(str, t->input_charset, t->output_charset, &name);
        if (ret < 0) {
            iso_msg_sorry(t->image->id, LIBISO_CHARSET_ERROR, 
                "Charset conversion error. Can't convert %s from %s to %s",
                str, t->input_charset, t->output_charset);
    
            /* use the original name, it's the best we can do */
            name = strdup(str);
        }
    }
    
    /* ISO 9660:1999 7.5.1 */
    if (strlen(name) > 207) {
        name[207] = '\0';
    }
    
    return name;
}

static
void iso1999_node_free(Iso1999Node *node)
{
    if (node == NULL) {
        return;
    }
    if (node->type == ISO1999_DIR) {
        int i;
        for (i = 0; i < node->info.dir->nchildren; i++) {
            iso1999_node_free(node->info.dir->children[i]);
        }
        free(node->info.dir->children);
        free(node->info.dir);
    }
    iso_node_unref(node->node);
    free(node->name);
    free(node);
}

/**
 * Create a low level ISO 9660:1999 node
 * @return
 *      1 success, 0 ignored, < 0 error
 */
static
int create_node(Ecma119Image *t, IsoNode *iso, Iso1999Node **node)
{
    int ret;
    Iso1999Node *n;

    n = calloc(1, sizeof(Iso1999Node));
    if (n == NULL) {
        return ISO_MEM_ERROR;
    }

    if (iso->type == LIBISO_DIR) {
        IsoDir *dir = (IsoDir*) iso;
        n->info.dir = calloc(1, sizeof(struct iso1999_dir_info));
        if (n->info.dir == NULL) {
            free(n);
            return ISO_MEM_ERROR;
        }
        n->info.dir->children = calloc(sizeof(void*), dir->nchildren);
        if (n->info.dir->children == NULL) {
            free(n->info.dir);
            free(n);
            return ISO_MEM_ERROR;
        }
        n->type = ISO1999_DIR;
    } else if (iso->type == LIBISO_FILE) {
        /* it's a file */
        off_t size;
        IsoFileSrc *src;
        IsoFile *file = (IsoFile*) iso;

        size = iso_stream_get_size(file->stream);
        if (size > (off_t)0xffffffff) {
            iso_msg_note(t->image->id, LIBISO_FILE_IGNORED,
                         "File \"%s\" can't be added to image because is "
                         "greater than 4GB", iso->name);
            free(n);
            return 0;
        }

        ret = iso_file_src_create(t, file, &src);
        if (ret < 0) {
            free(n);
            return ret;
        }
        n->info.file = src;
        n->type = ISO1999_FILE;
    } else if (iso->type == LIBISO_BOOT) {
        /* it's a el-torito boot catalog, that we write as a file */
        IsoFileSrc *src;

        ret = el_torito_catalog_file_src_create(t, &src);
        if (ret < 0) {
            free(n);
            return ret;
        }
        n->info.file = src;
        n->type = ISO1999_FILE;
    } else {
        /* should never happen */
        free(n);
        return ISO_ERROR;
    }

    /* take a ref to the IsoNode */
    n->node = iso;
    iso_node_ref(iso);

    *node = n;
    return ISO_SUCCESS;
}

/**
 * Create the low level ISO 9660:1999 tree from the high level ISO tree.
 * 
 * @return
 *      1 success, 0 file ignored, < 0 error
 */
static
int create_tree(Ecma119Image *t, IsoNode *iso, Iso1999Node **tree, int pathlen)
{
    int ret, max_path;
    Iso1999Node *node = NULL;
    char *iso_name = NULL;

    if (t == NULL || iso == NULL || tree == NULL) {
        return ISO_NULL_POINTER;
    }

    if (iso->hidden & LIBISO_HIDE_ON_1999) {
        /* file will be ignored */
        return 0;
    }
    iso_name = get_iso1999_name(t, iso->name);
    max_path = pathlen + 1 + (iso_name ? strlen(iso_name): 0);
    if (!t->allow_longer_paths && max_path > 255) {
        iso_msg_note(t->image->id, LIBISO_FILE_IGNORED,
                     "File \"%s\" can't be added to ISO 9660:1999 tree, "
                     "because its path length is larger than 255", iso->name);
        free(iso_name);
        return 0;
    }

    switch (iso->type) {
    case LIBISO_FILE:
        ret = create_node(t, iso, &node);
        break;
    case LIBISO_DIR:
        {
            IsoNode *pos;
            IsoDir *dir = (IsoDir*)iso;
            ret = create_node(t, iso, &node);
            if (ret < 0) {
                free(iso_name);
                return ret;
            }
            pos = dir->children;
            while (pos) {
                int cret;
                Iso1999Node *child;
                cret = create_tree(t, pos, &child, max_path);
                if (cret < 0) {
                    /* error */
                    iso1999_node_free(node);
                    ret = cret;
                    break;
                } else if (cret == ISO_SUCCESS) {
                    /* add child to this node */
                    int nchildren = node->info.dir->nchildren++;
                    node->info.dir->children[nchildren] = child;
                    child->parent = node;
                }
                pos = pos->next;
            }
        }
        break;
    case LIBISO_BOOT:
        if (t->eltorito) {
            ret = create_node(t, iso, &node);
        } else {
            /* log and ignore */
            iso_msg_note(t->image->id, LIBISO_FILE_IGNORED, 
                "El-Torito catalog found on a image without El-Torito.", 
                iso->name);
            ret = 0;
        }
        break;
    case LIBISO_SYMLINK:
    case LIBISO_SPECIAL:
        iso_msg_note(t->image->id, LIBISO_FILE_IGNORED, 
                     "Can't add %s to ISO 9660:1999 tree. This kind of files "
                     "can only be added to a Rock Ridget tree. Skipping.", 
                     iso->name);
        ret = 0;
        break;
    default:
        /* should never happen */
        return ISO_ERROR;
    }
    if (ret <= 0) {
        free(iso_name);
        return ret;
    }
    node->name = iso_name;
    *tree = node;
    return ISO_SUCCESS;
}

static int
cmp_node(const void *f1, const void *f2)
{
    Iso1999Node *f = *((Iso1999Node**)f1);
    Iso1999Node *g = *((Iso1999Node**)f2);

    /**
     * TODO #00027 Follow ISO 9660:1999 specs when sorting files
     * strcmp do not does exactly what ISO 9660:1999, 9.3, as characters
     * < 0x20 " " are allowed, so name len must be taken into accout
     */
    return strcmp(f->name, g->name);
}

/**
 * Sort the entries inside an ISO 9660:1999 directory, according to 
 * ISO 9660:1999, 9.3 
 */
static 
void sort_tree(Iso1999Node *root)
{
    size_t i;

    qsort(root->info.dir->children, root->info.dir->nchildren, 
          sizeof(void*), cmp_node);
    for (i = 0; i < root->info.dir->nchildren; i++) {
        Iso1999Node *child = root->info.dir->children[i];
        if (child->type == ISO1999_DIR)
            sort_tree(child);
    }
}

static
int iso1999_tree_create(Ecma119Image *t)
{
    int ret;
    Iso1999Node *root;
    
    if (t == NULL) {
        return ISO_NULL_POINTER;
    }

    ret = create_tree(t, (IsoNode*)t->image->root, &root, 0);
    if (ret <= 0) {
        if (ret == 0) {
            /* unexpected error, root ignored!! This can't happen */
            ret = ISO_ERROR;
        }
        return ret;
    }
    
    /* the ISO 9660:1999 tree is stored in Ecma119Image target */
    t->iso1999_root = root;

    iso_msg_debug(t->image->id, "Sorting the ISO 9660:1999 tree...");
    sort_tree(root);

    /* 
     * FIXME #00001 : Mangle ISO 9660:1999 names
     * iso_msg_debug(t->image->id, "Mangling ISO 9660:1999 names...");
     * FIXME ret = mangle_tree(t, 1);
     */

    return ISO_SUCCESS;
}

/**
 * Compute the size of a directory entry for a single node
 */
static
size_t calc_dirent_len(Ecma119Image *t, Iso1999Node *n)
{
    int ret = n->name ? strlen(n->name) + 33 : 34;
    if (ret % 2)
        ret++;
    return ret;
}

/**
 * Computes the total size of all directory entries of a single dir, as
 * stated in ISO 9660:1999, 6.8.1.3
 */
static
size_t calc_dir_size(Ecma119Image *t, Iso1999Node *dir)
{
    size_t i, len;

    /* size of "." and ".." entries */
    len = 34 + 34;

    for (i = 0; i < dir->info.dir->nchildren; ++i) {
        size_t remaining;
        Iso1999Node *child = dir->info.dir->children[i];
        size_t dirent_len = calc_dirent_len(t, child);
        remaining = BLOCK_SIZE - (len % BLOCK_SIZE);
        if (dirent_len > remaining) {
            /* child directory entry doesn't fit on block */
            len += remaining + dirent_len;
        } else {
            len += dirent_len;
        }
    }
    
    /*
     * The size of a dir is always a multiple of block size, as we must add 
     * the size of the unused space after the last directory record 
     * (ISO 9660:1999, 6.8.1.3)
     */
    len = ROUND_UP(len, BLOCK_SIZE);

    /* cache the len */
    dir->info.dir->len = len;
    return len;
}

static
void calc_dir_pos(Ecma119Image *t, Iso1999Node *dir)
{
    size_t i, len;

    t->iso1999_ndirs++;
    dir->info.dir->block = t->curblock;
    len = calc_dir_size(t, dir);
    t->curblock += DIV_UP(len, BLOCK_SIZE);
    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Iso1999Node *child = dir->info.dir->children[i];
        if (child->type == ISO1999_DIR) {
            calc_dir_pos(t, child);
        }
    }
}

/**
 * Compute the length of the path table (ISO 9660:1999, 6.9), in bytes.
 */
static
uint32_t calc_path_table_size(Iso1999Node *dir)
{
    uint32_t size;
    size_t i;

    /* size of path table for this entry */
    size = 8;
    size += dir->name ? strlen(dir->name) : 2;
    size += (size % 2);

    /* and recurse */
    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Iso1999Node *child = dir->info.dir->children[i];
        if (child->type == ISO1999_DIR) {
            size += calc_path_table_size(child);
        }
    }
    return size;
}

static
int iso1999_writer_compute_data_blocks(IsoImageWriter *writer)
{
    Ecma119Image *t;
    uint32_t path_table_size;

    if (writer == NULL) {
        return ISO_MEM_ERROR;
    }

    t = writer->target;

    /* compute position of directories */
    iso_msg_debug(t->image->id, 
                  "Computing position of ISO 9660:1999 dir structure");
    t->iso1999_ndirs = 0;
    calc_dir_pos(t, t->iso1999_root);

    /* compute length of pathlist */
    iso_msg_debug(t->image->id, "Computing length of ISO 9660:1999 pathlist");
    path_table_size = calc_path_table_size(t->iso1999_root);

    /* compute location for path tables */
    t->iso1999_l_path_table_pos = t->curblock;
    t->curblock += DIV_UP(path_table_size, BLOCK_SIZE);
    t->iso1999_m_path_table_pos = t->curblock;
    t->curblock += DIV_UP(path_table_size, BLOCK_SIZE);
    t->iso1999_path_table_size = path_table_size;

    return ISO_SUCCESS;
}

/**
 * Write a single directory record (ISO 9660:1999, 9.1).
 * 
 * @param file_id
 *     if >= 0, we use it instead of the filename (for "." and ".." entries).
 * @param len_fi
 *     Computed length of the file identifier.
 */
static
void write_one_dir_record(Ecma119Image *t, Iso1999Node *node, int file_id,
                          uint8_t *buf, size_t len_fi)
{
    uint32_t len;
    uint32_t block;
    uint8_t len_dr; /*< size of dir entry */
    uint8_t *name = (file_id >= 0) ? (uint8_t*)&file_id
            : (uint8_t*)node->name;

    struct ecma119_dir_record *rec = (struct ecma119_dir_record*)buf;

    len_dr = 33 + len_fi + (len_fi % 2 ? 0 : 1);

    memcpy(rec->file_id, name, len_fi);

    if (node->type == ISO1999_DIR) {
        /* use the cached length */
        len = node->info.dir->len;
        block = node->info.dir->block;
    } else if (node->type == ISO1999_FILE) {
        len = iso_file_src_get_size(node->info.file);
        block = node->info.file->block;
    } else {
        /* 
         * for nodes other than files and dirs, we set both 
         * len and block to 0 
         */
        len = 0;
        block = 0;
    }

    /*
     * For ".." entry we need to write the parent info!
     */
    if (file_id == 1 && node->parent)
        node = node->parent;

    rec->len_dr[0] = len_dr;
    iso_bb(rec->block, block, 4);
    iso_bb(rec->length, len, 4);
    iso_datetime_7(rec->recording_time, t->now);
    rec->flags[0] = (node->type == ISO1999_DIR) ? 2 : 0;
    iso_bb(rec->vol_seq_number, 1, 2);
    rec->len_fi[0] = len_fi;
}

/**
 * Write the enhanced volume descriptor (ISO/IEC 9660:1999, 8.5) 
 */
static
int iso1999_writer_write_vol_desc(IsoImageWriter *writer)
{
    IsoImage *image;
    Ecma119Image *t;
    
    /* The enhanced volume descriptor is like the sup vol desc */
    struct ecma119_sup_vol_desc vol;

    char *vol_id = NULL, *pub_id = NULL, *data_id = NULL;
    char *volset_id = NULL, *system_id = NULL, *application_id = NULL;
    char *copyright_file_id = NULL, *abstract_file_id = NULL;
    char *biblio_file_id = NULL;

    if (writer == NULL) {
        return ISO_MEM_ERROR;
    }

    t = writer->target;
    image = t->image;

    iso_msg_debug(image->id, "Write Enhanced Vol Desc (ISO 9660:1999)");

    memset(&vol, 0, sizeof(struct ecma119_sup_vol_desc));

    vol_id = get_iso1999_name(t, image->volume_id);
    str2a_char(t->input_charset, image->publisher_id, &pub_id);
    str2a_char(t->input_charset, image->data_preparer_id, &data_id);
    volset_id = get_iso1999_name(t, image->volset_id);

    str2a_char(t->input_charset, image->system_id, &system_id);
    str2a_char(t->input_charset, image->application_id, &application_id);
    copyright_file_id = get_iso1999_name(t, image->copyright_file_id);
    abstract_file_id = get_iso1999_name(t, image->abstract_file_id);
    biblio_file_id = get_iso1999_name(t, image->biblio_file_id);

    vol.vol_desc_type[0] = 2;
    memcpy(vol.std_identifier, "CD001", 5);
    
    /* descriptor version is 2 (ISO/IEC 9660:1999, 8.5.2) */
    vol.vol_desc_version[0] = 2;
    strncpy_pad((char*)vol.volume_id, vol_id, 32);

    iso_bb(vol.vol_space_size, t->vol_space_size, 4);
    iso_bb(vol.vol_set_size, 1, 2);
    iso_bb(vol.vol_seq_number, 1, 2);
    iso_bb(vol.block_size, BLOCK_SIZE, 2);
    iso_bb(vol.path_table_size, t->iso1999_path_table_size, 4);
    iso_lsb(vol.l_path_table_pos, t->iso1999_l_path_table_pos, 4);
    iso_msb(vol.m_path_table_pos, t->iso1999_m_path_table_pos, 4);

    write_one_dir_record(t, t->iso1999_root, 0, vol.root_dir_record, 1);

    strncpy_pad((char*)vol.vol_set_id, volset_id, 128);
    strncpy_pad((char*)vol.publisher_id, pub_id, 128);
    strncpy_pad((char*)vol.data_prep_id, data_id, 128);
    
    strncpy_pad((char*)vol.system_id, system_id, 32);

    strncpy_pad((char*)vol.application_id, application_id, 128);
    strncpy_pad((char*)vol.copyright_file_id, copyright_file_id, 37);
    strncpy_pad((char*)vol.abstract_file_id, abstract_file_id, 37);
    strncpy_pad((char*)vol.bibliographic_file_id, biblio_file_id, 37);

    iso_datetime_17(vol.vol_creation_time, t->now);
    iso_datetime_17(vol.vol_modification_time, t->now);
    iso_datetime_17(vol.vol_effective_time, t->now);
    vol.file_structure_version[0] = 1;

    free(vol_id);
    free(volset_id);
    free(pub_id);
    free(data_id);
    free(system_id);
    free(application_id);
    free(copyright_file_id);
    free(abstract_file_id);
    free(biblio_file_id);

    /* Finally write the Volume Descriptor */
    return iso_write(t, &vol, sizeof(struct ecma119_sup_vol_desc));
}

static
int write_one_dir(Ecma119Image *t, Iso1999Node *dir)
{
    int ret;
    uint8_t buffer[BLOCK_SIZE];
    size_t i;
    size_t fi_len, len;

    /* buf will point to current write position on buffer */
    uint8_t *buf = buffer;

    /* initialize buffer with 0s */
    memset(buffer, 0, BLOCK_SIZE);

    /* write the "." and ".." entries first */
    write_one_dir_record(t, dir, 0, buf, 1);
    buf += 34;
    write_one_dir_record(t, dir, 1, buf, 1);
    buf += 34;

    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Iso1999Node *child = dir->info.dir->children[i];

        /* compute len of directory entry */
        fi_len = strlen(child->name);
        len = fi_len + 33 + (fi_len % 2 ? 0 : 1);

        if ( (buf + len - buffer) > BLOCK_SIZE) {
            /* dir doesn't fit in current block */
            ret = iso_write(t, buffer, BLOCK_SIZE);
            if (ret < 0) {
                return ret;
            }
            memset(buffer, 0, BLOCK_SIZE);
            buf = buffer;
        }
        /* write the directory entry in any case */
        write_one_dir_record(t, child, -1, buf, fi_len);
        buf += len;
    }

    /* write the last block */
    ret = iso_write(t, buffer, BLOCK_SIZE);
    return ret;
}

static
int write_dirs(Ecma119Image *t, Iso1999Node *root)
{
    int ret;
    size_t i;

    /* write all directory entries for this dir */
    ret = write_one_dir(t, root);
    if (ret < 0) {
        return ret;
    }

    /* recurse */
    for (i = 0; i < root->info.dir->nchildren; i++) {
        Iso1999Node *child = root->info.dir->children[i];
        if (child->type == ISO1999_DIR) {
            ret = write_dirs(t, child);
            if (ret < 0) {
                return ret;
            }
        }
    }
    return ISO_SUCCESS;
}

static
int write_path_table(Ecma119Image *t, Iso1999Node **pathlist, int l_type)
{
    size_t i, len;
    uint8_t buf[256]; /* 256 is just a convenient size larger enought */
    struct ecma119_path_table_record *rec;
    void (*write_int)(uint8_t*, uint32_t, int);
    Iso1999Node *dir;
    uint32_t path_table_size;
    int parent = 0;
    int ret= ISO_SUCCESS;

    path_table_size = 0;
    write_int = l_type ? iso_lsb : iso_msb;

    for (i = 0; i < t->iso1999_ndirs; i++) {
        dir = pathlist[i];

        /* find the index of the parent in the table */
        while ((i) && pathlist[parent] != dir->parent) {
            parent++;
        }

        /* write the Path Table Record (ECMA-119, 9.4) */
        memset(buf, 0, 256);
        rec = (struct ecma119_path_table_record*) buf;
        rec->len_di[0] = dir->parent ? (uint8_t) strlen(dir->name) : 1;
        rec->len_xa[0] = 0;
        write_int(rec->block, dir->info.dir->block, 4);
        write_int(rec->parent, parent + 1, 2);
        if (dir->parent) {
            memcpy(rec->dir_id, dir->name, rec->len_di[0]);
        }
        len = 8 + rec->len_di[0] + (rec->len_di[0] % 2);
        ret = iso_write(t, buf, len);
        if (ret < 0) {
            /* error */
            return ret;
        }
        path_table_size += len;
    }

    /* we need to fill the last block with zeros */
    path_table_size %= BLOCK_SIZE;
    if (path_table_size) {
        uint8_t zeros[BLOCK_SIZE];
        len = BLOCK_SIZE - path_table_size;
        memset(zeros, 0, len);
        ret = iso_write(t, zeros, len);
    }
    return ret;
}

static
int write_path_tables(Ecma119Image *t)
{
    int ret;
    size_t i, j, cur;
    Iso1999Node **pathlist;

    iso_msg_debug(t->image->id, "Writing ISO 9660:1999 Path tables");

    /* allocate temporal pathlist */
    pathlist = malloc(sizeof(void*) * t->iso1999_ndirs);
    if (pathlist == NULL) {
        return ISO_MEM_ERROR;
    }
    pathlist[0] = t->iso1999_root;
    cur = 1;

    for (i = 0; i < t->iso1999_ndirs; i++) {
        Iso1999Node *dir = pathlist[i];
        for (j = 0; j < dir->info.dir->nchildren; j++) {
            Iso1999Node *child = dir->info.dir->children[j];
            if (child->type == ISO1999_DIR) {
                pathlist[cur++] = child;
            }
        }
    }

    /* Write L Path Table */
    ret = write_path_table(t, pathlist, 1);
    if (ret < 0) {
        goto write_path_tables_exit;
    }

    /* Write L Path Table */
    ret = write_path_table(t, pathlist, 0);

    write_path_tables_exit: ;
    free(pathlist);
    return ret;
}

static
int iso1999_writer_write_data(IsoImageWriter *writer)
{
    int ret;
    Ecma119Image *t;

    if (writer == NULL) {
        return ISO_NULL_POINTER;
    }
    t = writer->target;

    /* first of all, we write the directory structure */
    ret = write_dirs(t, t->iso1999_root);
    if (ret < 0) {
        return ret;
    }

    /* and write the path tables */
    ret = write_path_tables(t);

    return ret;
}

static
int iso1999_writer_free_data(IsoImageWriter *writer)
{
    /* free the ISO 9660:1999 tree */
    Ecma119Image *t = writer->target;
    iso1999_node_free(t->iso1999_root);
    return ISO_SUCCESS;
}

int iso1999_writer_create(Ecma119Image *target)
{
    int ret;
    IsoImageWriter *writer;

    writer = malloc(sizeof(IsoImageWriter));
    if (writer == NULL) {
        return ISO_MEM_ERROR;
    }

    writer->compute_data_blocks = iso1999_writer_compute_data_blocks;
    writer->write_vol_desc = iso1999_writer_write_vol_desc;
    writer->write_data = iso1999_writer_write_data;
    writer->free_data = iso1999_writer_free_data;
    writer->data = NULL;
    writer->target = target;

    iso_msg_debug(target->image->id, 
                  "Creating low level ISO 9660:1999 tree...");
    ret = iso1999_tree_create(target);
    if (ret < 0) {
        return ret;
    }

    /* add this writer to image */
    target->writers[target->nwriters++] = writer;

    /* we need the volume descriptor */
    target->curblock++;
    return ISO_SUCCESS;
}