/*
 * Copyright (c) 2007 Vreixo Formoso
 * Copyright (c) 2007 Mario Danic
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

#include "libisofs.h"
#include "ecma119.h"
#include "joliet.h"
#include "iso1999.h"
#include "eltorito.h"
#include "ecma119_tree.h"
#include "error.h"
#include "filesrc.h"
#include "image.h"
#include "writer.h"
#include "messages.h"
#include "rockridge.h"
#include "util.h"

#include "libburn/libburn.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <locale.h>
#include <langinfo.h>

/*
 * TODO #00011 : guard against bad path table usage with more than 65535 dirs
 * image with more than 65535 directories have path_table related problems
 * due to 16 bits parent id. Note that this problem only affects to folders
 * that are parent of another folder.
 */

static
void ecma119_image_free(Ecma119Image *t)
{
    size_t i;

    ecma119_node_free(t->root);
    iso_image_unref(t->image);
    iso_rbtree_destroy(t->files, iso_file_src_free);
    iso_ring_buffer_free(t->buffer);

    for (i = 0; i < t->nwriters; ++i) {
        IsoImageWriter *writer = t->writers[i];
        writer->free_data(writer);
        free(writer);
    }
    free(t->input_charset);
    free(t->output_charset);
    free(t->writers);
    free(t);
}

/**
 * Check if we should add version number ";" to the given node name.
 */
static
int need_version_number(Ecma119Image *t, Ecma119Node *n)
{
    if (t->omit_version_numbers) {
        return 0;
    }
    if (n->type == ECMA119_DIR || n->type == ECMA119_PLACEHOLDER) {
        return 0;
    } else {
        return 1;
    }
}

/**
 * Compute the size of a directory entry for a single node
 */
static
size_t calc_dirent_len(Ecma119Image *t, Ecma119Node *n)
{
    int ret = n->iso_name ? strlen(n->iso_name) + 33 : 34;
    if (need_version_number(t, n)) {
        ret += 2; /* take into account version numbers */
    }
    if (ret % 2)
        ret++;
    return ret;
}

/**
 * Computes the total size of all directory entries of a single dir,
 * acording to ECMA-119 6.8.1.1
 * 
 * This also take into account the size needed for RR entries and
 * SUSP continuation areas (SUSP, 5.1).
 * 
 * @param ce
 *      Will be filled with the size needed for Continuation Areas
 * @return
 *      The size needed for all dir entries of the given dir, without
 *      taking into account the continuation areas.
 */
static
size_t calc_dir_size(Ecma119Image *t, Ecma119Node *dir, size_t *ce)
{
    size_t i, len;
    size_t ce_len = 0;

    /* size of "." and ".." entries */
    len = 34 + 34;
    if (t->rockridge) {
        len += rrip_calc_len(t, dir, 1, 255 - 34, &ce_len);
        *ce += ce_len;
        len += rrip_calc_len(t, dir, 2, 255 - 34, &ce_len);
        *ce += ce_len;
    }

    for (i = 0; i < dir->info.dir->nchildren; ++i) {
        size_t remaining;
        Ecma119Node *child = dir->info.dir->children[i];
        size_t dirent_len = calc_dirent_len(t, child);
        if (t->rockridge) {
            dirent_len += rrip_calc_len(t, child, 0, 255 - dirent_len, &ce_len);
            *ce += ce_len;
        }
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
     * (ECMA-119, 6.8.1.3)
     */
    len = ROUND_UP(len, BLOCK_SIZE);

    /* cache the len */
    dir->info.dir->len = len;
    return len;
}

static
void calc_dir_pos(Ecma119Image *t, Ecma119Node *dir)
{
    size_t i, len;
    size_t ce_len = 0;

    t->ndirs++;
    dir->info.dir->block = t->curblock;
    len = calc_dir_size(t, dir, &ce_len);
    t->curblock += DIV_UP(len, BLOCK_SIZE);
    if (t->rockridge) {
        t->curblock += DIV_UP(ce_len, BLOCK_SIZE);
    }
    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Ecma119Node *child = dir->info.dir->children[i];
        if (child->type == ECMA119_DIR) {
            calc_dir_pos(t, child);
        }
    }
}

/**
 * Compute the length of the path table, in bytes.
 */
static
uint32_t calc_path_table_size(Ecma119Node *dir)
{
    uint32_t size;
    size_t i;

    /* size of path table for this entry */
    size = 8;
    size += dir->iso_name ? strlen(dir->iso_name) : 1;
    size += (size % 2);

    /* and recurse */
    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Ecma119Node *child = dir->info.dir->children[i];
        if (child->type == ECMA119_DIR) {
            size += calc_path_table_size(child);
        }
    }
    return size;
}

static
int ecma119_writer_compute_data_blocks(IsoImageWriter *writer)
{
    Ecma119Image *target;
    uint32_t path_table_size;

    if (writer == NULL) {
        return ISO_ASSERT_FAILURE;
    }

    target = writer->target;

    /* compute position of directories */
    iso_msg_debug(target->image->id, "Computing position of dir structure");
    target->ndirs = 0;
    calc_dir_pos(target, target->root);

    /* compute length of pathlist */
    iso_msg_debug(target->image->id, "Computing length of pathlist");
    path_table_size = calc_path_table_size(target->root);

    /* compute location for path tables */
    target->l_path_table_pos = target->curblock;
    target->curblock += DIV_UP(path_table_size, BLOCK_SIZE);
    target->m_path_table_pos = target->curblock;
    target->curblock += DIV_UP(path_table_size, BLOCK_SIZE);
    target->path_table_size = path_table_size;

    return ISO_SUCCESS;
}

/**
 * Write a single directory record (ECMA-119, 9.1)
 * 
 * @param file_id
 *     if >= 0, we use it instead of the filename (for "." and ".." entries).
 * @param len_fi
 *     Computed length of the file identifier. Total size of the directory
 *     entry will be len + 33 + padding if needed (ECMA-119, 9.1.12)
 * @param info
 *     SUSP entries for the given directory record. It will be NULL for the 
 *     root directory record in the PVD (ECMA-119, 8.4.18) (in order to 
 *     distinguish it from the "." entry in the root directory)
 */
static
void write_one_dir_record(Ecma119Image *t, Ecma119Node *node, int file_id,
                          uint8_t *buf, size_t len_fi, struct susp_info *info)
{
    uint32_t len;
    uint32_t block;
    uint8_t len_dr; /*< size of dir entry without SUSP fields */
    uint8_t *name = (file_id >= 0) ? (uint8_t*)&file_id
            : (uint8_t*)node->iso_name;

    struct ecma119_dir_record *rec = (struct ecma119_dir_record*)buf;

    len_dr = 33 + len_fi + (len_fi % 2 ? 0 : 1);

    memcpy(rec->file_id, name, len_fi);

    if (need_version_number(t, node)) {
        len_dr += 2;
        rec->file_id[len_fi++] = ';';
        rec->file_id[len_fi++] = '1';
    }

    if (node->type == ECMA119_DIR) {
        /* use the cached length */
        len = node->info.dir->len;
        block = node->info.dir->block;
    } else if (node->type == ECMA119_FILE) {
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

    rec->len_dr[0] = len_dr + (info != NULL ? info->suf_len : 0);
    iso_bb(rec->block, block, 4);
    iso_bb(rec->length, len, 4);
    iso_datetime_7(rec->recording_time, t->now, t->always_gmt);
    rec->flags[0] = (node->type == ECMA119_DIR) ? 2 : 0;
    iso_bb(rec->vol_seq_number, 1, 2);
    rec->len_fi[0] = len_fi;

    /* and finally write the SUSP fields */
    if (info != NULL) {
        rrip_write_susp_fields(t, info, buf + len_dr);
    }
}

/**
 * Write the Primary Volume Descriptor (ECMA-119, 8.4)
 */
static
int ecma119_writer_write_vol_desc(IsoImageWriter *writer)
{
    IsoImage *image;
    Ecma119Image *t;
    struct ecma119_pri_vol_desc vol;

    char *vol_id, *pub_id, *data_id, *volset_id;
    char *system_id, *application_id, *copyright_file_id;
    char *abstract_file_id, *biblio_file_id;

    if (writer == NULL) {
        return ISO_ASSERT_FAILURE;
    }

    t = writer->target;
    image = t->image;

    iso_msg_debug(image->id, "Write Primary Volume Descriptor");

    memset(&vol, 0, sizeof(struct ecma119_pri_vol_desc));

    str2d_char(t->input_charset, image->volume_id, &vol_id);
    str2a_char(t->input_charset, image->publisher_id, &pub_id);
    str2a_char(t->input_charset, image->data_preparer_id, &data_id);
    str2d_char(t->input_charset, image->volset_id, &volset_id);

    str2a_char(t->input_charset, image->system_id, &system_id);
    str2a_char(t->input_charset, image->application_id, &application_id);
    str2d_char(t->input_charset, image->copyright_file_id, &copyright_file_id);
    str2d_char(t->input_charset, image->abstract_file_id, &abstract_file_id);
    str2d_char(t->input_charset, image->biblio_file_id, &biblio_file_id);

    vol.vol_desc_type[0] = 1;
    memcpy(vol.std_identifier, "CD001", 5);
    vol.vol_desc_version[0] = 1;
    strncpy_pad((char*)vol.system_id, system_id, 32);
    strncpy_pad((char*)vol.volume_id, vol_id, 32);
    iso_bb(vol.vol_space_size, t->vol_space_size, 4);
    iso_bb(vol.vol_set_size, 1, 2);
    iso_bb(vol.vol_seq_number, 1, 2);
    iso_bb(vol.block_size, BLOCK_SIZE, 2);
    iso_bb(vol.path_table_size, t->path_table_size, 4);
    iso_lsb(vol.l_path_table_pos, t->l_path_table_pos, 4);
    iso_msb(vol.m_path_table_pos, t->m_path_table_pos, 4);

    write_one_dir_record(t, t->root, 0, vol.root_dir_record, 1, NULL);

    strncpy_pad((char*)vol.vol_set_id, volset_id, 128);
    strncpy_pad((char*)vol.publisher_id, pub_id, 128);
    strncpy_pad((char*)vol.data_prep_id, data_id, 128);

    strncpy_pad((char*)vol.application_id, application_id, 128);
    strncpy_pad((char*)vol.copyright_file_id, copyright_file_id, 37);
    strncpy_pad((char*)vol.abstract_file_id, abstract_file_id, 37);
    strncpy_pad((char*)vol.bibliographic_file_id, biblio_file_id, 37);

    iso_datetime_17(vol.vol_creation_time, t->now, t->always_gmt);
    iso_datetime_17(vol.vol_modification_time, t->now, t->always_gmt);
    iso_datetime_17(vol.vol_effective_time, t->now, t->always_gmt);
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
    return iso_write(t, &vol, sizeof(struct ecma119_pri_vol_desc));
}

static
int write_one_dir(Ecma119Image *t, Ecma119Node *dir)
{
    int ret;
    uint8_t buffer[BLOCK_SIZE];
    size_t i;
    size_t fi_len, len;
    struct susp_info info;

    /* buf will point to current write position on buffer */
    uint8_t *buf = buffer;

    /* initialize buffer with 0s */
    memset(buffer, 0, BLOCK_SIZE);

    /* 
     * set susp_info to 0's, this way code for both plain ECMA-119 and
     * RR is very similar
     */
    memset(&info, 0, sizeof(struct susp_info));
    if (t->rockridge) {
        /* initialize the ce_block, it might be needed */
        info.ce_block = dir->info.dir->block + DIV_UP(dir->info.dir->len, 
                                                      BLOCK_SIZE);
    }

    /* write the "." and ".." entries first */
    if (t->rockridge) {
        ret = rrip_get_susp_fields(t, dir, 1, 255 - 32, &info);
        if (ret < 0) {
            return ret;
        }
    }
    len = 34 + info.suf_len;
    write_one_dir_record(t, dir, 0, buf, 1, &info);
    buf += len;

    if (t->rockridge) {
        ret = rrip_get_susp_fields(t, dir, 2, 255 - 32, &info);
        if (ret < 0) {
            return ret;
        }
    }
    len = 34 + info.suf_len;
    write_one_dir_record(t, dir, 1, buf, 1, &info);
    buf += len;

    for (i = 0; i < dir->info.dir->nchildren; i++) {
        Ecma119Node *child = dir->info.dir->children[i];

        /* compute len of directory entry */
        fi_len = strlen(child->iso_name);
        len = fi_len + 33 + (fi_len % 2 ? 0 : 1);
        if (need_version_number(t, child)) {
            len += 2;
        }

        /* get the SUSP fields if rockridge is enabled */
        if (t->rockridge) {
            ret = rrip_get_susp_fields(t, child, 0, 255 - len, &info);
            if (ret < 0) {
                return ret;
            }
            len += info.suf_len;
        }

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
        write_one_dir_record(t, child, -1, buf, fi_len, &info);
        buf += len;
    }

    /* write the last block */
    ret = iso_write(t, buffer, BLOCK_SIZE);
    if (ret < 0) {
        return ret;
    }

    /* write the Continuation Area if needed */
    if (info.ce_len > 0) {
        ret = rrip_write_ce_fields(t, &info);
    }

    return ret;
}

static
int write_dirs(Ecma119Image *t, Ecma119Node *root)
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
        Ecma119Node *child = root->info.dir->children[i];
        if (child->type == ECMA119_DIR) {
            ret = write_dirs(t, child);
            if (ret < 0) {
                return ret;
            }
        }
    }
    return ISO_SUCCESS;
}

static
int write_path_table(Ecma119Image *t, Ecma119Node **pathlist, int l_type)
{
    size_t i, len;
    uint8_t buf[64]; /* 64 is just a convenient size larger enought */
    struct ecma119_path_table_record *rec;
    void (*write_int)(uint8_t*, uint32_t, int);
    Ecma119Node *dir;
    uint32_t path_table_size;
    int parent = 0;
    int ret= ISO_SUCCESS;

    path_table_size = 0;
    write_int = l_type ? iso_lsb : iso_msb;

    for (i = 0; i < t->ndirs; i++) {
        dir = pathlist[i];

        /* find the index of the parent in the table */
        while ((i) && pathlist[parent] != dir->parent) {
            parent++;
        }

        /* write the Path Table Record (ECMA-119, 9.4) */
        memset(buf, 0, 64);
        rec = (struct ecma119_path_table_record*) buf;
        rec->len_di[0] = dir->parent ? (uint8_t) strlen(dir->iso_name) : 1;
        rec->len_xa[0] = 0;
        write_int(rec->block, dir->info.dir->block, 4);
        write_int(rec->parent, parent + 1, 2);
        if (dir->parent) {
            memcpy(rec->dir_id, dir->iso_name, rec->len_di[0]);
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
    Ecma119Node **pathlist;

    iso_msg_debug(t->image->id, "Writing ISO Path tables");

    /* allocate temporal pathlist */
    pathlist = malloc(sizeof(void*) * t->ndirs);
    if (pathlist == NULL) {
        return ISO_OUT_OF_MEM;
    }
    pathlist[0] = t->root;
    cur = 1;

    for (i = 0; i < t->ndirs; i++) {
        Ecma119Node *dir = pathlist[i];
        for (j = 0; j < dir->info.dir->nchildren; j++) {
            Ecma119Node *child = dir->info.dir->children[j];
            if (child->type == ECMA119_DIR) {
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

/**
 * Write both the directory structure (ECMA-119, 6.8) and the L and M
 * Path Tables (ECMA-119, 6.9).
 */
static
int ecma119_writer_write_data(IsoImageWriter *writer)
{
    int ret;
    Ecma119Image *t;

    if (writer == NULL) {
        return ISO_ASSERT_FAILURE;
    }
    t = writer->target;

    /* first of all, we write the directory structure */
    ret = write_dirs(t, t->root);
    if (ret < 0) {
        return ret;
    }

    /* and write the path tables */
    ret = write_path_tables(t);

    return ret;
}

static
int ecma119_writer_free_data(IsoImageWriter *writer)
{
    /* nothing to do */
    return ISO_SUCCESS;
}

int ecma119_writer_create(Ecma119Image *target)
{
    int ret;
    IsoImageWriter *writer;

    writer = malloc(sizeof(IsoImageWriter));
    if (writer == NULL) {
        return ISO_OUT_OF_MEM;
    }

    writer->compute_data_blocks = ecma119_writer_compute_data_blocks;
    writer->write_vol_desc = ecma119_writer_write_vol_desc;
    writer->write_data = ecma119_writer_write_data;
    writer->free_data = ecma119_writer_free_data;
    writer->data = NULL;
    writer->target = target;

    /* add this writer to image */
    target->writers[target->nwriters++] = writer;

    iso_msg_debug(target->image->id, "Creating low level ECMA-119 tree...");
    ret = ecma119_tree_create(target);
    if (ret < 0) {
        return ret;
    }

    /* we need the volume descriptor */
    target->curblock++;
    return ISO_SUCCESS;
}

/** compute how many padding bytes are needed */
static
int pad_writer_compute_data_blocks(IsoImageWriter *writer)
{
    Ecma119Image *target;

    if (writer == NULL) {
        return ISO_ASSERT_FAILURE;
    }

    target = writer->target;
    if (target->curblock < 32) {
        target->pad_blocks = 32 - target->curblock;
        target->curblock = 32;
    }
    return ISO_SUCCESS;
}

static
int pad_writer_write_vol_desc(IsoImageWriter *writer)
{
    /* nothing to do */
    return ISO_SUCCESS;
}
static
int pad_writer_write_data(IsoImageWriter *writer)
{
    int ret;
    Ecma119Image *t;
    uint32_t pad[BLOCK_SIZE];
    size_t i;

    if (writer == NULL) {
        return ISO_ASSERT_FAILURE;
    }
    t = writer->target;
    
    if (t->pad_blocks == 0) {
        return ISO_SUCCESS;
    }

    memset(pad, 0, BLOCK_SIZE);
    for (i = 0; i < t->pad_blocks; ++i) {
        ret = iso_write(t, pad, BLOCK_SIZE);
        if (ret < 0) {
            return ret;
        }
    }

    return ISO_SUCCESS;
}

static
int pad_writer_free_data(IsoImageWriter *writer)
{
    /* nothing to do */
    return ISO_SUCCESS;
}

static
int pad_writer_create(Ecma119Image *target)
{
    IsoImageWriter *writer;

    writer = malloc(sizeof(IsoImageWriter));
    if (writer == NULL) {
        return ISO_OUT_OF_MEM;
    }

    writer->compute_data_blocks = pad_writer_compute_data_blocks;
    writer->write_vol_desc = pad_writer_write_vol_desc;
    writer->write_data = pad_writer_write_data;
    writer->free_data = pad_writer_free_data;
    writer->data = NULL;
    writer->target = target;

    /* add this writer to image */
    target->writers[target->nwriters++] = writer;
    return ISO_SUCCESS;
}

static
void *write_function(void *arg)
{
    int res;
    size_t i;
    uint8_t buf[BLOCK_SIZE];
    IsoImageWriter *writer;

    Ecma119Image *target = (Ecma119Image*)arg;
    iso_msg_debug(target->image->id, "Starting image writing...");

    target->bytes_written = (off_t) 0;
    target->percent_written = 0;

    /* Write System Area, 16 blocks of zeros (ECMA-119, 6.2.1) */
    memset(buf, 0, BLOCK_SIZE);
    for (i = 0; i < 16; ++i) {
        res = iso_write(target, buf, BLOCK_SIZE);
        if (res < 0) {
            goto write_error;
        }
    }

    /* write volume descriptors, one per writer */
    iso_msg_debug(target->image->id, "Write volume descriptors");
    for (i = 0; i < target->nwriters; ++i) {
        writer = target->writers[i];
        res = writer->write_vol_desc(writer);
        if (res < 0) {
            goto write_error;
        }
    }

    /* write Volume Descriptor Set Terminator (ECMA-119, 8.3) */
    {
        struct ecma119_vol_desc_terminator *vol;
        vol = (struct ecma119_vol_desc_terminator *) buf;

        vol->vol_desc_type[0] = 255;
        memcpy(vol->std_identifier, "CD001", 5);
        vol->vol_desc_version[0] = 1;

        res = iso_write(target, buf, BLOCK_SIZE);
        if (res < 0) {
            goto write_error;
        }
    }

    /* write data for each writer */
    for (i = 0; i < target->nwriters; ++i) {
        writer = target->writers[i];
        res = writer->write_data(writer);
        if (res < 0) {
            goto write_error;
        }
    }

    iso_ring_buffer_writer_close(target->buffer, 0);
    pthread_exit(NULL);

    write_error: ;
    iso_msg_submit(target->image->id, ISO_WRITE_ERROR, res, 
                   "Image write error");
    iso_ring_buffer_writer_close(target->buffer, 1);
    pthread_exit(NULL);
}

static
int ecma119_image_new(IsoImage *src, IsoWriteOpts *opts, Ecma119Image **img)
{
    int ret, i, voldesc_size, nwriters;
    Ecma119Image *target;

    /* 1. Allocate target and copy opts there */
    target = calloc(1, sizeof(Ecma119Image));
    if (target == NULL) {
        return ISO_OUT_OF_MEM;
    }

    /* create the tree for file caching */
    ret = iso_rbtree_new(iso_file_src_cmp, &(target->files));
    if (ret < 0) {
        free(target);
        return ret;
    }

    target->image = src;
    iso_image_ref(src);

    target->iso_level = opts->level;
    target->rockridge = opts->rockridge;
    target->joliet = opts->joliet;
    target->iso1999 = opts->iso1999;
    target->always_gmt = opts->always_gmt;
    target->ino = 0;
    target->omit_version_numbers = opts->omit_version_numbers 
                                 | opts->max_37_char_filenames;
    target->allow_deep_paths = opts->allow_deep_paths;
    target->allow_longer_paths = opts->allow_longer_paths;
    target->max_37_char_filenames = opts->max_37_char_filenames;
    target->no_force_dots = opts->no_force_dots;
    target->allow_lowercase = opts->allow_lowercase;
    target->allow_full_ascii = opts->allow_full_ascii;
    target->joliet_longer_paths = opts->joliet_longer_paths;
    target->sort_files = opts->sort_files;

    target->replace_uid = opts->replace_uid ? 1 : 0;
    target->replace_gid = opts->replace_gid ? 1 : 0;
    target->replace_dir_mode = opts->replace_dir_mode ? 1 : 0;
    target->replace_file_mode = opts->replace_file_mode ? 1 : 0;

    target->uid = opts->replace_uid == 2 ? opts->uid : 0;
    target->gid = opts->replace_gid == 2 ? opts->gid : 0;
    target->dir_mode = opts->replace_dir_mode == 2 ? opts->dir_mode : 0555;
    target->file_mode = opts->replace_file_mode == 2 ? opts->file_mode : 0444;

    target->now = time(NULL);
    target->ms_block = opts->ms_block;
    target->appendable = opts->appendable;

    target->replace_timestamps = opts->replace_timestamps ? 1 : 0;
    target->timestamp = opts->replace_timestamps == 2 ? 
                        opts->timestamp : target->now;

    /* el-torito? */
    target->eltorito = (src->bootcat == NULL ? 0 : 1);
    target->catalog = src->bootcat;

    /* default to locale charset */
    setlocale(LC_CTYPE, "");
    target->input_charset = strdup(nl_langinfo(CODESET));
    if (target->input_charset == NULL) {
        iso_image_unref(src);
        free(target);
        return ISO_OUT_OF_MEM;
    }

    if (opts->output_charset != NULL) {
        target->output_charset = strdup(opts->output_charset);
    } else {
        target->output_charset = strdup(target->input_charset);
    }
    if (target->output_charset == NULL) {
        iso_image_unref(src);
        free(target);
        return ISO_OUT_OF_MEM;
    }

    /* 
     * 2. Based on those options, create needed writers: iso, joliet...
     * Each writer inits its structures and stores needed info into
     * target. 
     * If the writer needs an volume descriptor, it increments image
     * current block.
     * Finally, create Writer for files.
     */
    target->curblock = target->ms_block + 16;

    /* the number of writers is dependent of the extensions */
    nwriters = 1 + 1 + 1; /* ECMA-119 + padding + files */

    if (target->eltorito) {
        nwriters++;
    }
    if (target->joliet) {
        nwriters++;
    }
    if (target->iso1999) {
        nwriters++;
    }

    target->writers = malloc(nwriters * sizeof(void*));
    if (target->writers == NULL) {
        iso_image_unref(src);
        free(target);
        return ISO_OUT_OF_MEM;
    }

    /* create writer for ECMA-119 structure */
    ret = ecma119_writer_create(target);
    if (ret < 0) {
        goto target_cleanup;
    }
    
    /* create writer for El-Torito */
    if (target->eltorito) {
        ret = eltorito_writer_create(target);
        if (ret < 0) {
            goto target_cleanup;
        }
    }
    
    /* create writer for Joliet structure */
    if (target->joliet) {
        ret = joliet_writer_create(target);
        if (ret < 0) {
            goto target_cleanup;
        }
    }
    
    /* create writer for ISO 9660:1999 structure */
    if (target->iso1999) {
        ret = iso1999_writer_create(target);
        if (ret < 0) {
            goto target_cleanup;
        }
    }

    voldesc_size = target->curblock - target->ms_block - 16;
    
    /* Volume Descriptor Set Terminator */
    target->curblock++;

    /* 
     * Create the writer for possible padding to ensure that in case of image
     * growing we can safety overwrite the first 64 KiB of image.
     */
    ret = pad_writer_create(target);
    if (ret < 0) {
        goto target_cleanup;
    }

    /* create writer for file contents */
    ret = iso_file_src_writer_create(target);
    if (ret < 0) {
        goto target_cleanup;
    }

    /*
     * 3. 
     * Call compute_data_blocks() in each Writer.
     * That function computes the size needed by its structures and
     * increments image current block propertly.
     */
    for (i = 0; i < target->nwriters; ++i) {
        IsoImageWriter *writer = target->writers[i];
        ret = writer->compute_data_blocks(writer);
        if (ret < 0) {
            goto target_cleanup;
        }
    }

    /* create the ring buffer */
    ret = iso_ring_buffer_new(opts->fifo_size, &target->buffer);
    if (ret < 0) {
        goto target_cleanup;
    }

    /* check if we need to provide a copy of volume descriptors */
    if (opts->overwrite) {

        /*
         * Get a copy of the volume descriptors to be written in a DVD+RW
         * disc
         */

        uint8_t *buf;
        struct ecma119_vol_desc_terminator *vol;
        IsoImageWriter *writer;

        /*
         * In the PVM to be written in the 16th sector of the disc, we
         * need to specify the full size.
         */
        target->vol_space_size = target->curblock;

        /* write volume descriptor */
        for (i = 0; i < target->nwriters; ++i) {
            writer = target->writers[i];
            ret = writer->write_vol_desc(writer);
            if (ret < 0) {
                iso_msg_debug(target->image->id,
                              "Error writing overwrite volume descriptors");
                goto target_cleanup;
            }
        }

        /* skip the first 16 blocks (system area) */
        buf = opts->overwrite + 16 * BLOCK_SIZE;
        voldesc_size *= BLOCK_SIZE;

        /* copy the volume descriptors to the overwrite buffer... */
        ret = iso_ring_buffer_read(target->buffer, buf, voldesc_size);
        if (ret < 0) {
            iso_msg_debug(target->image->id,
                          "Error reading overwrite volume descriptors");
            goto target_cleanup;
        }

        /* ...including the vol desc terminator */
        memset(buf + voldesc_size, 0, BLOCK_SIZE);
        vol = (struct ecma119_vol_desc_terminator*) (buf + voldesc_size);
        vol->vol_desc_type[0] = 255;
        memcpy(vol->std_identifier, "CD001", 5);
        vol->vol_desc_version[0] = 1;
    }

    /*
     * The volume space size is just the size of the last session, in
     * case of ms images.
     */
    target->vol_space_size = target->curblock - target->ms_block;
    target->total_size = (off_t) target->vol_space_size * BLOCK_SIZE;

    /* 4. Create and start writting thread */

    /* ensure the thread is created joinable */
    pthread_attr_init(&(target->th_attr));
    pthread_attr_setdetachstate(&(target->th_attr), PTHREAD_CREATE_JOINABLE);

    ret = pthread_create(&(target->wthread), &(target->th_attr),
                         write_function, (void *) target);
    if (ret != 0) {
        iso_msg_submit(target->image->id, ISO_THREAD_ERROR, 0,
                      "Cannot create writer thread");
        ret = ISO_THREAD_ERROR;
        goto target_cleanup;
    }

    /*
     * Notice that once we reach this point, target belongs to the writer
     * thread and should not be modified until the writer thread finished.
     * There're however, specific fields in target that can be accessed, or
     * even modified by the read thread (look inside bs_* functions)
     */

    *img = target;
    return ISO_SUCCESS;

    target_cleanup: ;
    ecma119_image_free(target);
    return ret;
}

static int bs_read(struct burn_source *bs, unsigned char *buf, int size)
{
    int ret;
    Ecma119Image *t = (Ecma119Image*)bs->data;

    ret = iso_ring_buffer_read(t->buffer, buf, size);
    if (ret == ISO_SUCCESS) {
        return size;
    } else if (ret < 0) {
        /* error */
        iso_msg_submit(t->image->id, ISO_BUF_READ_ERROR, ret, NULL);
        return -1;
    } else {
        /* EOF */
        return 0;
    }
}

static off_t bs_get_size(struct burn_source *bs)
{
    Ecma119Image *target = (Ecma119Image*)bs->data;
    return target->total_size;
}

static void bs_free_data(struct burn_source *bs)
{
    Ecma119Image *target = (Ecma119Image*)bs->data;

    /* forces writer to stop if it is still running */
    iso_ring_buffer_reader_close(target->buffer, 0);

    /* wait until writer thread finishes */
    pthread_join(target->wthread, NULL);

    iso_msg_debug(target->image->id, "Writer thread joined");
    iso_msg_debug(target->image->id, 
                  "Ring buffer was %d times full and %d times empty", 
                  iso_ring_buffer_get_times_full(target->buffer),
                  iso_ring_buffer_get_times_empty(target->buffer));

    /* now we can safety free target */
    ecma119_image_free(target);
}

int bs_cancel(struct burn_source *bs)
{
    Ecma119Image *target = (Ecma119Image*)bs->data;
    
    iso_msg_debug(target->image->id, "Reader thread being cancelled");

    /* forces writer to stop if it is still running */
    iso_ring_buffer_reader_close(target->buffer, ISO_CANCELED);

    /* wait until writer thread finishes */
    pthread_join(target->wthread, NULL);

    iso_msg_debug(target->image->id, "Writer thread joined");
    return ISO_SUCCESS;
}

static
int bs_set_size(struct burn_source *bs, off_t size)
{
    Ecma119Image *target = (Ecma119Image*)bs->data;

    /* 
     * just set the value to be returned by get_size. This is not used at
     * all by libisofs, it is here just for helping libburn to correctly pad
     * the image if needed.
     */
    target->total_size = size;
    return 1;
}

int iso_image_create_burn_source(IsoImage *image, IsoWriteOpts *opts,
                                 struct burn_source **burn_src)
{
    int ret;
    struct burn_source *source;
    Ecma119Image *target= NULL;

    if (image == NULL || opts == NULL || burn_src == NULL) {
        return ISO_NULL_POINTER;
    }

    source = calloc(1, sizeof(struct burn_source));
    if (source == NULL) {
        return ISO_OUT_OF_MEM;
    }

    ret = ecma119_image_new(image, opts, &target);
    if (ret < 0) {
        free(source);
        return ret;
    }

    source->refcount = 1;
    source->version = 1;
    source->read = NULL;
    source->get_size = bs_get_size;
    source->set_size = bs_set_size;
    source->free_data = bs_free_data;
    source->read_xt = bs_read;
    source->cancel = bs_cancel;
    source->data = target;

    *burn_src = source;
    return ISO_SUCCESS;
}

int iso_write(Ecma119Image *target, void *buf, size_t count)
{
    int ret;

    ret = iso_ring_buffer_write(target->buffer, buf, count);
    if (ret == 0) {
        /* reader cancelled */
        return ISO_CANCELED;
    }

    /* total size is 0 when writing the overwrite buffer */
    if (ret > 0 && (target->total_size != (off_t) 0)){
        unsigned int kbw, kbt;
        int percent;
        
        target->bytes_written += (off_t) count;
        kbw = (unsigned int) (target->bytes_written >> 10);
        kbt = (unsigned int) (target->total_size >> 10);
        percent = (kbw * 100) / kbt;
        
        /* only report in 5% chunks */
        if (percent >= target->percent_written + 5) {
            iso_msg_debug(target->image->id, "Processed %u of %u KB (%d %%)",
                          kbw, kbt, percent);
            target->percent_written = percent;
        }
    }
    
    return ret;
}

int iso_write_opts_new(IsoWriteOpts **opts, int profile)
{
    IsoWriteOpts *wopts;
    
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    if (profile < 0 || profile > 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    
    wopts = calloc(1, sizeof(IsoWriteOpts));
    if (wopts == NULL) {
        return ISO_OUT_OF_MEM;
    }
    
    switch (profile) {
    case 0:
        wopts->level = 1;
        break;
    case 1:
        wopts->level = 2;
        wopts->rockridge = 1;
        break;
    case 2:
        wopts->level = 2;
        wopts->rockridge = 1;
        wopts->joliet = 1;
        wopts->replace_dir_mode = 1;
        wopts->replace_file_mode = 1;
        wopts->replace_uid = 1;
        wopts->replace_gid = 1;
        wopts->replace_timestamps = 1;
        wopts->always_gmt = 1;
        break;
    default:
        /* should never happen */
        free(wopts);
        return ISO_ASSERT_FAILURE;
        break;
    }
    wopts->fifo_size = 1024; /* 2 MB buffer */
    
    *opts = wopts;
    return ISO_SUCCESS;
}

void iso_write_opts_free(IsoWriteOpts *opts)
{
    if (opts == NULL) {
        return;
    }
    
    free(opts->output_charset);
    free(opts);
}

int iso_write_opts_set_iso_level(IsoWriteOpts *opts, int level)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    if (level != 1 && level != 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    opts->level = level;
    return ISO_SUCCESS;
}

int iso_write_opts_set_rockridge(IsoWriteOpts *opts, int enable)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->rockridge = enable ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_joliet(IsoWriteOpts *opts, int enable)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->joliet = enable ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_iso1999(IsoWriteOpts *opts, int enable)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->iso1999 = enable ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_omit_version_numbers(IsoWriteOpts *opts, int omit)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->omit_version_numbers = omit ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_allow_deep_paths(IsoWriteOpts *opts, int allow)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->allow_deep_paths = allow ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_allow_longer_paths(IsoWriteOpts *opts, int allow)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->allow_longer_paths = allow ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_max_37_char_filenames(IsoWriteOpts *opts, int allow)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->max_37_char_filenames = allow ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_no_force_dots(IsoWriteOpts *opts, int no)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->no_force_dots = no ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_allow_lowercase(IsoWriteOpts *opts, int allow)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->allow_lowercase = allow ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_allow_full_ascii(IsoWriteOpts *opts, int allow)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->allow_full_ascii = allow ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_joliet_longer_paths(IsoWriteOpts *opts, int allow)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->joliet_longer_paths = allow ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_sort_files(IsoWriteOpts *opts, int sort)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->sort_files = sort ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_replace_mode(IsoWriteOpts *opts, int dir_mode,
                                    int file_mode, int uid, int gid)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    if (dir_mode < 0 || dir_mode > 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    if (file_mode < 0 || file_mode > 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    if (uid < 0 || uid > 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    if (gid < 0 || gid > 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    opts->replace_dir_mode = dir_mode;
    opts->replace_file_mode = file_mode;
    opts->replace_uid = uid;
    opts->replace_gid = gid;
    return ISO_SUCCESS;
}

int iso_write_opts_set_default_dir_mode(IsoWriteOpts *opts, mode_t dir_mode)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->dir_mode = dir_mode;
    return ISO_SUCCESS;
}

int iso_write_opts_set_default_file_mode(IsoWriteOpts *opts, mode_t file_mode)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->file_mode = file_mode;
    return ISO_SUCCESS;
}

int iso_write_opts_set_default_uid(IsoWriteOpts *opts, uid_t uid)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->uid = uid;
    return ISO_SUCCESS;
}

int iso_write_opts_set_default_gid(IsoWriteOpts *opts, gid_t gid)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->gid = gid;
    return ISO_SUCCESS;
}

int iso_write_opts_set_replace_timestamps(IsoWriteOpts *opts, int replace)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    if (replace < 0 || replace > 2) {
        return ISO_WRONG_ARG_VALUE;
    }
    opts->replace_timestamps = replace;
    return ISO_SUCCESS;
}

int iso_write_opts_set_default_timestamp(IsoWriteOpts *opts, time_t timestamp)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->timestamp = timestamp;
    return ISO_SUCCESS;
}

int iso_write_opts_set_always_gmt(IsoWriteOpts *opts, int gmt)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->always_gmt = gmt ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_output_charset(IsoWriteOpts *opts, const char *charset)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->output_charset = charset ? strdup(charset) : NULL;
    return ISO_SUCCESS;
}

int iso_write_opts_set_appendable(IsoWriteOpts *opts, int appendable)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->appendable = appendable ? 1 : 0;
    return ISO_SUCCESS;
}

int iso_write_opts_set_ms_block(IsoWriteOpts *opts, uint32_t ms_block)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->ms_block = ms_block;
    return ISO_SUCCESS;
}

int iso_write_opts_set_overwrite_buf(IsoWriteOpts *opts, uint8_t *overwrite)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    opts->overwrite = overwrite;
    return ISO_SUCCESS;
}

int iso_write_opts_set_fifo_size(IsoWriteOpts *opts, size_t fifo_size)
{
    if (opts == NULL) {
        return ISO_NULL_POINTER;
    }
    if (fifo_size < 32) {
        return ISO_WRONG_ARG_VALUE;
    }
    opts->fifo_size = fifo_size;
    return ISO_SUCCESS;
}