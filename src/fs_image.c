/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

/*
 * Filesystem/FileSource implementation to access an ISO image, using an
 * IsoDataSource to read image data.
 */

#include "fs_image.h"
#include "error.h"
#include "ecma119.h"
#include "messages.h"
#include "rockridge.h"
#include "image.h"
#include "tree.h"

#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <langinfo.h>
#include <limits.h>


static int ifs_fs_open(IsoImageFilesystem *fs);
static int ifs_fs_close(IsoImageFilesystem *fs);
static int iso_file_source_new_ifs(IsoImageFilesystem *fs, 
           IsoFileSource *parent, struct ecma119_dir_record *record, 
           IsoFileSource **src);

/** unique identifier for each image */
unsigned int fs_dev_id = 0;

/**
 * Should the RR extensions be read?
 */
enum read_rr_ext {
    RR_EXT_NO = 0, /*< Do not use RR extensions */
    RR_EXT_110 = 1, /*< RR extensions conforming version 1.10 */
    RR_EXT_112 = 2 /*< RR extensions conforming version 1.12 */
};

/**
 * Private data for the image IsoFilesystem
 */
typedef struct
{
    /** DataSource from where data will be read */
    IsoDataSource *src;
    
    /** unique id for the each image (filesystem instance) */
    unsigned int id;

    /** 
     * Counter of the times the filesystem has been openned still pending of
     * close. It is used to keep track of when we need to actually open or 
     * close the IsoDataSource. 
     */
    unsigned int open_count;

    uid_t uid; /**< Default uid when no RR */
    gid_t gid; /**< Default uid when no RR */
    mode_t mode; /**< Default mode when no RR (only permissions) */

    struct libiso_msgs *messenger;

    char *input_charset; /**< Input charset for RR names */
    char *local_charset; /**< For RR names, will be set to the locale one */

    /** 
     * Will be filled with the block lba of the extend for the root directory
     * of the hierarchy that will be read, either from the PVD (ISO, RR) or
     * from the SVD (Joliet)
     */
    uint32_t iso_root_block;

    /**
     * Will be filled with the block lba of the extend for the root directory,
     * as read from the PVM
     */
    uint32_t pvd_root_block;

    /**
     * Will be filled with the block lba of the extend for the root directory,
     * as read from the SVD
     */
    uint32_t svd_root_block;

    /**
     * If we need to read RR extensions. i.e., if the image contains RR 
     * extensions, and the user wants to read them. 
     */
    enum read_rr_ext rr;

    /**
     * The function used to read the name from a directoy record. For ISO, 
     * the name is in US-ASCII. For Joliet, in UCS-2BE. Thus, we need 
     * different functions for both.
     */
    char *(*get_name)(const char *, size_t);

    /**
     * Bytes skipped within the System Use field of a directory record, before 
     * the beginning of the SUSP system user entries. See IEEE 1281, SUSP. 5.3. 
     */
    uint8_t len_skp;

    /* Volume attributes */
    char *volset_id;
    char *volume_id; /**< Volume identifier. */
    char *publisher_id; /**< Volume publisher. */
    char *data_preparer_id; /**< Volume data preparer. */
    char *system_id; /**< Volume system identifier. */
    char *application_id; /**< Volume application id */
    char *copyright_file_id;
    char *abstract_file_id;
    char *biblio_file_id;

    /* extension information */

    /**
     * RR version being used in image.
     * 0 no RR extension, 1 RRIP 1.10, 2 RRIP 1.12
     */
    enum read_rr_ext rr_version;
    
    /** If Joliet extensions are available on image */
    unsigned int joliet : 1;

    /**
     * Number of blocks of the volume, as reported in the PVM.
     */
    uint32_t nblocks;

    //TODO el-torito information

} _ImageFsData;

typedef struct image_fs_data ImageFileSourceData;

struct image_fs_data
{
    IsoImageFilesystem *fs; /**< reference to the image it belongs to */
    IsoFileSource *parent; /**< reference to the parent (NULL if root) */
    
    struct stat info; /**< filled struct stat */
    char *name; /**< name of this file */
    
    uint32_t block; /**< block of the extend */
    unsigned int opened : 2; /**< 0 not opened, 1 opened file, 2 opened dir */
    
    /* info for content reading */
    struct 
    {
        /**
         * - For regular files, once opened it points to a temporary data 
         *   buffer of 2048 bytes.
         * - For dirs, once opened it points to a IsoFileSource* array with
         *   its children
         * - For symlinks, it points to link destination
         */
        void *content;

        /**
         * - For regular files, number of bytes already read.
         */
        off_t offset;
    } data;
};

struct child_list
{
    IsoFileSource *file;
    struct child_list *next;
};

void child_list_free(struct child_list *list)
{
    struct child_list *temp;
    struct child_list *next = list;
    while (next != NULL) {
        temp = next->next;
        iso_file_source_unref(next->file);
        free(next);
        next = temp;
    }
}

static
char* ifs_get_path(IsoFileSource *src)
{
    ImageFileSourceData *data;
    data = src->data;
    
    if (data->parent == NULL) {
        return strdup("");
    } else {
        char *path = ifs_get_path(data->parent);
        int pathlen = strlen(path);
        path = realloc(path, pathlen + strlen(data->name) + 2);
        path[pathlen] = '/';
        path[pathlen + 1] = '\0';
        return strcat(path, data->name);
    }
}

static
char* ifs_get_name(IsoFileSource *src)
{
    ImageFileSourceData *data;
    data = src->data;
    return data->name == NULL ? NULL : strdup(data->name);
}

static
int ifs_lstat(IsoFileSource *src, struct stat *info)
{
    ImageFileSourceData *data;

    if (src == NULL || info == NULL) {
        return ISO_NULL_POINTER;
    }

    data = src->data;
    *info = data->info;
    return ISO_SUCCESS;
}

static
int ifs_stat(IsoFileSource *src, struct stat *info)
{
    ImageFileSourceData *data;
        
    if (src == NULL || info == NULL || src->data == NULL) {
        return ISO_NULL_POINTER;
    }

    data = (ImageFileSourceData*)src->data;
    
    if (S_ISLNK(data->info.st_mode)) {
        /* TODO follow symlinks not supported yet */ 
        return ISO_FILE_BAD_PATH;
    }
    *info = data->info;
    return ISO_SUCCESS;
}

static
int ifs_access(IsoFileSource *src)
{
    /* we always have access, it is controlled by DataSource */
    return ISO_SUCCESS;
}

/**
 * Read all directory records in a directory, and creates an IsoFileSource for
 * each of them, storing them in the data field of the IsoFileSource for the
 * given dir. 
 */
static
int read_dir(ImageFileSourceData *data)
{
    int ret;
    uint32_t size;
    uint32_t block;
    IsoImageFilesystem *fs;
    _ImageFsData *fsdata;
    struct ecma119_dir_record *record;
    uint8_t buffer[BLOCK_SIZE];
    IsoFileSource *child = NULL;
    uint32_t pos = 0;
    uint32_t tlen = 0;

    if (data == NULL) {
        return ISO_NULL_POINTER;
    }

    fs = data->fs;
    fsdata = fs->fs.data;

    block = data->block;
    ret = fsdata->src->read_block(fsdata->src, block, buffer);
    if (ret < 0) {
        return ret;
    }

    /* "." entry, get size of the dir and skip */
    record = (struct ecma119_dir_record *)(buffer + pos);
    size = iso_read_bb(record->length, 4, NULL);
    tlen += record->len_dr[0];
    pos += record->len_dr[0];

    /* skip ".." */
    record = (struct ecma119_dir_record *)(buffer + pos);
    tlen += record->len_dr[0];
    pos += record->len_dr[0];

    while (tlen < size) {

        record = (struct ecma119_dir_record *)(buffer + pos);
        if (pos == 2048 || record->len_dr[0] == 0) {
            /* 
             * The directory entries are splitted in several blocks
             * read next block 
             */
            ret = fsdata->src->read_block(fsdata->src, ++block, buffer);
            if (ret < 0) {
                return ret;
            }
            tlen += 2048 - pos;
            pos = 0;
            continue;
        }

        /*
         * What about ignoring files with existence flag?
         * if (record->flags[0] & 0x01)
         *	continue;
         */

        /*
         * For a extrange reason, mkisofs relocates directories under
         * a RR_MOVED dir. It seems that it is only used for that purposes,
         * and thus it should be removed from the iso tree before 
         * generating a new image with libisofs, that don't uses it.
         */
        if (data->parent == NULL && record->len_fi[0] == 8
            && !strncmp((char*)record->file_id, "RR_MOVED", 8)) {
            
            iso_msg_debug(fsdata->messenger, "Skipping RR_MOVE entry.");

            tlen += record->len_dr[0];
            pos += record->len_dr[0];
            continue;
        }

        /*
         * We pass a NULL parent instead of dir, to prevent the circular 
         * reference from child to parent.
         */
        ret = iso_file_source_new_ifs(fs, NULL, record, &child);
        if (ret < 0) {
            return ret;
        }

        /* add to the child list */
        if (ret != 0) {
            struct child_list *node;
            node = malloc(sizeof(struct child_list));
            if (node == NULL) {
                iso_file_source_unref(child);
                return ISO_MEM_ERROR;
            }
            /*
             * Note that we insert in reverse order. This leads to faster
             * addition here, but also when adding to the tree, as insertion
             * will be done, sorted, in the first position of the list.
             */
            node->next = data->data.content;
            node->file = child;
            data->data.content = node;
        }
        
        tlen += record->len_dr[0];
        pos += record->len_dr[0];
    }

    return ISO_SUCCESS;
}

static
int ifs_open(IsoFileSource *src)
{
    int ret;
    ImageFileSourceData *data;
    
    if (src == NULL || src->data == NULL) {
        return ISO_NULL_POINTER;
    }
    data = (ImageFileSourceData*)src->data;

    if (data->opened) {
        return ISO_FILE_ALREADY_OPENNED;
    }
    
    if (S_ISDIR(data->info.st_mode)) {
        /* ensure fs is openned */
        ret = data->fs->open(data->fs);
        if (ret < 0) {
            return ret;
        }
        
        /* 
         * Cache all directory entries. 
         * This can waste more memory, but improves as disc is read in much more
         * sequencially way, thus reducing jump between tracks on disc
         */
        ret = read_dir(data);
        data->fs->close(data->fs);
        
        if (ret < 0) {
            /* free probably allocated children */
            child_list_free((struct child_list*)data->data.content);
        } else {
            data->opened = 2;
        }
        
        return ret;
    } else if (S_ISREG(data->info.st_mode)) {
        /* ensure fs is openned */
        ret = data->fs->open(data->fs);
        if (ret < 0) {
            return ret;
        }
        data->data.content = malloc(BLOCK_SIZE);
        if (data->data.content == NULL) {
            return ISO_MEM_ERROR;
        }
        data->data.offset = 0;
        data->opened = 1;
    } else {
        /* symlinks and special files inside image can't be openned */
        return ISO_FILE_ERROR;
    }
    return ISO_SUCCESS;
}

static
int ifs_close(IsoFileSource *src)
{
    ImageFileSourceData *data;
    
    if (src == NULL || src->data == NULL) {
        return ISO_NULL_POINTER;
    }
    data = (ImageFileSourceData*)src->data;
    
    if (!data->opened) {
        return ISO_FILE_NOT_OPENNED;
    }
    
    if (data->opened == 2) {
        /* 
         * close a dir, free all pending pre-allocated children.
         * not that we don't need to close the filesystem, it was already 
         * closed
         */
        child_list_free((struct child_list*) data->data.content);
        data->data.content = NULL;
        data->opened = 0;
    } else if (data->opened == 1) {
        /* close regular file */
        free(data->data.content);
        data->fs->close(data->fs);
        data->data.content = NULL;
        data->opened = 0;
    } else {
        /* TODO only dirs and files supported for now */
        return ISO_ERROR;
    }
    
    return ISO_SUCCESS;
}

/**
 * Attempts to read up to count bytes from the given source into
 * the buffer starting at buf.
 * 
 * The file src must be open() before calling this, and close() when no 
 * more needed. Not valid for dirs. On symlinks it reads the destination
 * file.
 * 
 * @return 
 *     number of bytes read, 0 if EOF, < 0 on error
 *      Error codes:
 *         ISO_FILE_ERROR
 *         ISO_NULL_POINTER
 *         ISO_FILE_NOT_OPENNED
 *         ISO_FILE_IS_DIR
 *         ISO_MEM_ERROR
 *         ISO_INTERRUPTED
 */
static
int ifs_read(IsoFileSource *src, void *buf, size_t count)
{
    int ret;
    ImageFileSourceData *data;
    uint32_t read = 0;
    
    if (src == NULL || src->data == NULL || buf == NULL) {
        return ISO_NULL_POINTER;
    }
    if (count == 0) {
        return ISO_WRONG_ARG_VALUE;
    }
    data = (ImageFileSourceData*)src->data;
    
    if (!data->opened) {
        return ISO_FILE_NOT_OPENNED;
    } else if (data->opened != 1) {
        return ISO_FILE_IS_DIR;
    }
    
    while (read < count && data->data.offset < data->info.st_size) {
        size_t bytes;
        uint8_t *orig;
        
        if (data->data.offset % BLOCK_SIZE == 0) {
            /* we need to buffer next block */
            uint32_t block;
            _ImageFsData *fsdata;
            
            if (data->data.offset >= data->info.st_size) {
                /* EOF */
                break;
            }
            fsdata = data->fs->fs.data;
            block = data->block + (data->data.offset / BLOCK_SIZE);
            ret = fsdata->src->read_block(fsdata->src, block, 
                                          data->data.content);
            if (ret < 0) {
                return ret;
            }
        }
        
        /* how much can I read */
        bytes = MIN(BLOCK_SIZE - (data->data.offset % BLOCK_SIZE), 
                    count - read);
	if (data->data.offset + (off_t)bytes > data->info.st_size) {
             bytes = data->info.st_size - data->data.offset;
        }
        orig = data->data.content;
        orig += data->data.offset % BLOCK_SIZE;
        memcpy((uint8_t*)buf + read, orig, bytes);
        read += bytes;
        data->data.offset += (off_t)bytes;
    }
    return read;
}

static
int ifs_readdir(IsoFileSource *src, IsoFileSource **child)
{
    ImageFileSourceData *data, *cdata;
    struct child_list *children;
    
    if (src == NULL || src->data == NULL || child == NULL) {
        return ISO_NULL_POINTER;
    }
    data = (ImageFileSourceData*)src->data;
    
    if (!data->opened) {
        return ISO_FILE_NOT_OPENNED;
    } else if (data->opened != 2) {
        return ISO_FILE_IS_NOT_DIR;
    }
    
    /* return the first child and free it */
    if (data->data.content == NULL) {
        return 0; /* EOF */
    }
    
    children = (struct child_list*)data->data.content;
    *child = children->file;
    cdata = (ImageFileSourceData*)(*child)->data;
    
    /* set the ref to the parent */
    cdata->parent = src;
    iso_file_source_ref(src);
    
    /* free the first element of the list */
    data->data.content = children->next;
    free(children);

    return ISO_SUCCESS;
}

/**
 * Read the destination of a symlink. You don't need to open the file
 * to call this.
 * 
 * @param buf 
 *     allocated buffer of at least bufsiz bytes. 
 *     The dest. will be copied there, and it will be NULL-terminated
 * @param bufsiz
 *     characters to be copied. Destination link will be truncated if
 *     it is larger than given size. This include the \0 character.
 * @return 
 *     1 on success, < 0 on error
 *      Error codes:
 *         ISO_FILE_ERROR
 *         ISO_NULL_POINTER
 *         ISO_WRONG_ARG_VALUE -> if bufsiz <= 0
 *         ISO_FILE_IS_NOT_SYMLINK
 *         ISO_MEM_ERROR
 *         ISO_FILE_BAD_PATH
 *         ISO_FILE_DOESNT_EXIST
 * 
 */
static
int ifs_readlink(IsoFileSource *src, char *buf, size_t bufsiz)
{
    char *dest;
    size_t len;
    ImageFileSourceData *data;
    
    if (src == NULL || buf == NULL || src->data == NULL) {
        return ISO_NULL_POINTER;
    }
    
    if (bufsiz <= 0) {
        return ISO_WRONG_ARG_VALUE;
    }
    
    data = (ImageFileSourceData*)src->data;
    
    if (!S_ISLNK(data->info.st_mode)) {
        return ISO_FILE_IS_NOT_SYMLINK;
    }
    
    dest = (char*)data->data.content;
    len = strlen(dest);
    if (bufsiz <= len) {
        len = bufsiz - 1;
    }
    
    strncpy(buf, dest, len);
    buf[len] = '\0';
    
    return ISO_SUCCESS;
}

static
IsoFilesystem* ifs_get_filesystem(IsoFileSource *src)
{
    ImageFileSourceData *data;

    if (src == NULL) {
        return NULL;
    }

    data = src->data;
    return (IsoFilesystem*) data->fs;
}

static
void ifs_free(IsoFileSource *src)
{
    ImageFileSourceData *data;

    data = src->data;

    /* close the file if it is already openned */
    if (data->opened) {
        src->class->close(src);
    }

    /* free destination if it is a link */
    if (S_ISLNK(data->info.st_mode)) {
        free(data->data.content);
    }
    iso_filesystem_unref((IsoFilesystem*)data->fs);
    if (data->parent != NULL) {
        iso_file_source_unref(data->parent);
    }
    free(data->name);
    free(data);
}

IsoFileSourceIface ifs_class = { 
    ifs_get_path,
    ifs_get_name,
    ifs_lstat,
    ifs_stat,
    ifs_access,
    ifs_open,
    ifs_close,
    ifs_read,
    ifs_readdir,
    ifs_readlink,
    ifs_get_filesystem,
    ifs_free
};

/**
 * 
 * @return
 *      1 success, 0 record ignored (not an error, can be a relocated dir),
 *      < 0 error
 */
static
int iso_file_source_new_ifs(IsoImageFilesystem *fs, IsoFileSource *parent, 
                            struct ecma119_dir_record *record, 
                            IsoFileSource **src)
{
    int ret;
    struct stat atts;
    time_t recorded;
    _ImageFsData *fsdata;
    IsoFileSource *ifsrc = NULL;
    ImageFileSourceData *ifsdata = NULL;
    
    int namecont = 0; /* 1 if found a NM with CONTINUE flag */
    char *name = NULL;

    /* 1 if found a SL with CONTINUE flag, 
     * 2 if found a component with continue flag */
    int linkdestcont = 0; 
    char *linkdest = NULL;
    
    uint32_t relocated_dir = 0;
    
    if (fs == NULL || fs->fs.data == NULL || record == NULL || src == NULL) {
        return ISO_NULL_POINTER;
    }

    fsdata = (_ImageFsData*)fs->fs.data;
    
    memset(&atts, 0, sizeof(struct stat));
    
    /*
     * First of all, check for unsupported ECMA-119 features
     */

    /* check for unsupported multiextend */
    if (record->flags[0] & 0x80) {
        iso_msg_fatal(fsdata->messenger, LIBISO_IMG_UNSUPPORTED,
                      "Unsupported image. This image makes use of Multi-Extend"
                      " features, that are not supported at this time. If you "
                      "need support for that, please request us this feature.");
        return ISO_UNSUPPORTED_ECMA119;
    }

    /* check for unsupported interleaved mode */
    if (record->file_unit_size[0] || record->interleave_gap_size[0]) {
        iso_msg_fatal(fsdata->messenger, LIBISO_IMG_UNSUPPORTED,
              "Unsupported image. This image has at least one file recorded "
              "in interleaved mode. We don't support this mode, as we think "
              "it's not used. If you're reading this, then we're wrong :) "
              "Please contact libisofs developers, so we can fix this.");
        return ISO_UNSUPPORTED_ECMA119;
    }
    
    /*
     * Check for extended attributes, that are not supported. Note that even
     * if we don't support them, it is easy to ignore them.
     */
    if (record->len_xa[0]) {
        iso_msg_fatal(fsdata->messenger, LIBISO_IMG_UNSUPPORTED,
              "Unsupported image. This image has at least one file with "
              "Extended Attributes, that are not supported");
        return ISO_UNSUPPORTED_ECMA119;
    }

    //TODO check for other flags?
    
    /*
     * The idea is to read all the RR entries (if we want to do that and RR
     * extensions exist on image), storing the info we want from that. 
     * Then, we need some sanity checks.
     * Finally, we select what kind of node it is, and set values properly.
     */
    
    if (fsdata->rr) {
        struct susp_sys_user_entry *sue;
        SuspIterator *iter;
        

        iter = susp_iter_new(fsdata->src, record, fsdata->len_skp, 
                             fsdata->messenger);
        if (iter == NULL) {
            return ISO_MEM_ERROR;
        }
        
        while ((ret = susp_iter_next(iter, &sue)) > 0) {
            
            /* ignore entries from different version */
            if (sue->version[0] != 1)
                continue; 
            
            if (SUSP_SIG(sue, 'P', 'X')) {
                ret = read_rr_PX(sue, &atts);
                if (ret < 0) {
                    /* notify and continue */
                    iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                                  "Invalid PX entry");
                }
            } else if (SUSP_SIG(sue, 'T', 'F')) {
                ret = read_rr_TF(sue, &atts);
                if (ret < 0) {
                    /* notify and continue */
                    iso_msg_warn(fsdata->messenger, LIBISO_RR_WARNING, 
                                 "Invalid TF entry");
                }
            } else if (SUSP_SIG(sue, 'N', 'M')) {
                if (name != NULL && namecont == 0) {
                    /* ups, RR standard violation */
                    iso_msg_warn(fsdata->messenger, LIBISO_RR_WARNING, 
                                 "New NM entry found without previous"
                                 "CONTINUE flag. Ignored");
                    continue;
                }
                ret = read_rr_NM(sue, &name, &namecont);
                if (ret < 0) {
                    /* notify and continue */
                    iso_msg_warn(fsdata->messenger, LIBISO_RR_WARNING, 
                                 "Invalid NM entry");
                }
            } else if (SUSP_SIG(sue, 'S', 'L')) {
                if (linkdest != NULL && linkdestcont == 0) {
                    /* ups, RR standard violation */
                    iso_msg_warn(fsdata->messenger, LIBISO_RR_WARNING, 
                                 "New SL entry found without previous"
                                 "CONTINUE flag. Ignored");
                    continue;
                }
                ret = read_rr_SL(sue, &linkdest, &linkdestcont);
                if (ret < 0) {
                    /* notify and continue */
                    iso_msg_warn(fsdata->messenger, LIBISO_RR_WARNING, 
                                 "Invalid SL entry");
                }
            } else if (SUSP_SIG(sue, 'R', 'E')) {
                /* 
                 * this directory entry refers to a relocated directory.
                 * We simply ignore it, as it will be correctly handled
                 * when found the CL
                 */
                susp_iter_free(iter);
                free(name);
                return 0; /* it's not an error */
            } else if (SUSP_SIG(sue, 'C', 'L')) {
                /*
                 * This entry is a placeholder for a relocated dir.
                 * We need to ignore other entries, with the exception of NM.
                 * Then we create a directory node that represents the 
                 * relocated dir, and iterate over its children. 
                 */
                relocated_dir = iso_read_bb(sue->data.CL.child_loc, 4, NULL);
                if (relocated_dir == 0) {
                    iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                                  "Invalid SL entry, no child location");
                    ret = ISO_WRONG_RR;
                    break;
                }
            } else if (SUSP_SIG(sue, 'P', 'N')) {
                ret = read_rr_PN(sue, &atts);
                if (ret < 0) {
                    /* notify and continue */
                    iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                                  "Invalid PN entry");
                }
            } else if (SUSP_SIG(sue, 'S', 'F')) {
                iso_msg_sorry(fsdata->messenger, LIBISO_RR_UNSUPPORTED, 
                              "Sparse files not supported.");
                ret = ISO_UNSUPPORTED_RR;
                break;
            } else if (SUSP_SIG(sue, 'R', 'R')) {
                /* TODO I've seen this RR on mkisofs images. what's this? */
                continue;
            } else {
                iso_msg_hint(fsdata->messenger, LIBISO_SUSP_UNHANLED, 
                    "Unhandled SUSP entry %c%c.", sue->sig[0], sue->sig[1]);
            }
        }
        
        susp_iter_free(iter);
        
        /* check for RR problems */
        
        if (ret < 0) {
            iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                          "Error parsing RR entries");
        } else if (!relocated_dir && atts.st_mode == (mode_t) 0 ) {
            iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, "Mandatory Rock "
               "Ridge PX entry is not present or it contains invalid values.");
            ret = ISO_WRONG_RR;
        } else {
            /* ensure both name and link dest are finished */
            if (namecont != 0) {
                iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                              "Incomplete RR name, last NM entry continues");
                ret = ISO_WRONG_RR;
            }
            if (linkdestcont != 0) {
                iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                    "Incomplete link destination, last SL entry continues");
                ret = ISO_WRONG_RR;
            }
        }
        
        if (ret < 0) {
            free(name);
            return ret;
        }
           
        /* convert name to needed charset */
        if (strcmp(fsdata->input_charset, fsdata->local_charset) && name) {
            /* we need to convert name charset */
            char *newname = NULL;
            ret = strconv(name, fsdata->input_charset, fsdata->local_charset,
                          &newname);
            if (ret < 0) {
                iso_msg_sorry(fsdata->messenger, LIBISO_CHARSET_ERROR, 
                    "Charset conversion error. Can't convert %s from %s to %s",
                    name, fsdata->input_charset, fsdata->local_charset);
                free(newname);
            } else {
                free(name);
                name = newname;
            }
        }
        
        /* convert link destination to needed charset */ 
        if (strcmp(fsdata->input_charset, fsdata->local_charset) && linkdest) {
            /* we need to convert name charset */
            char *newlinkdest = NULL;
            ret = strconv(linkdest, fsdata->input_charset, 
                          fsdata->local_charset, &newlinkdest);
            if (ret < 0) {
                iso_msg_sorry(fsdata->messenger, LIBISO_CHARSET_ERROR, 
                    "Charset conversion error. Can't convert %s from %s to %s",
                    linkdest, fsdata->input_charset, fsdata->local_charset);
                free(newlinkdest);
            } else {
                free(linkdest);
                linkdest = newlinkdest;
            }
        }
        
    } else {
        /* RR extensions are not read / used */
        atts.st_mode = fsdata->mode;
        atts.st_gid = fsdata->gid;
        atts.st_uid = fsdata->uid;
        if (record->flags[0] & 0x02) 
            atts.st_mode |= S_IFDIR;
        else
            atts.st_mode |= S_IFREG;
    }
    
    /* 
     * if we haven't RR extensions, or no NM entry is present,
     * we use the name in directory record
     */
    if (!name) {
        size_t len;
        
        if (record->len_fi[0] == 1 && record->file_id[0] == 0) {
            /* "." entry, we can call this for root node, so... */
            if (!(atts.st_mode & S_IFDIR)) {
                iso_msg_sorry(fsdata->messenger, LIBISO_WRONG_IMG, 
                              "Wrong ISO file name. \".\" not dir");
                return ISO_WRONG_ECMA119;
            }
        } else {
            name = fsdata->get_name((char*)record->file_id, record->len_fi[0]);
            if (name == NULL) {
                iso_msg_sorry(fsdata->messenger, LIBISO_WRONG_IMG, 
                              "Can't retrieve file name");
                return ISO_WRONG_ECMA119;
            }
            
            /* remove trailing version number */
            len = strlen(name);
            if (len > 2 && name[len-2] == ';' && name[len-1] == '1') {
                if (len > 3 && name[len-3] == '.') {
                    /* 
                     * the "." is mandatory, so in most cases is included only
                     * for standard compliance
                     */
                    name[len-3] = '\0';
                } else {
                    name[len-2] = '\0';
                }
            }
        }
    }

    if (relocated_dir) {
        
        /*
         * We are dealing with a placeholder for a relocated dir.
         * Thus, we need to read attributes for this directory from the "." 
         * entry of the relocated dir.
         */
        uint8_t buffer[BLOCK_SIZE];
        
        ret = fsdata->src->read_block(fsdata->src, relocated_dir, buffer);
        if (ret < 0) {
            return ret;
        }
        
        ret = iso_file_source_new_ifs(fs, parent, (struct ecma119_dir_record*)
                                      buffer, src);
        if (ret <= 0) {
            return ret;
        }
        
        /* but the real name is the name of the placeholder */
        ifsdata = (ImageFileSourceData*) (*src)->data;
        ifsdata->name = name;
        return ISO_SUCCESS;
    }

    if (fsdata->rr != RR_EXT_112) {
        /*
         * Only RRIP 1.12 provides valid inode numbers. If not, it is not easy
         * to generate those serial numbers, and we use extend block instead.
         * It BREAKS POSIX SEMANTICS, but its suitable for our needs
         */
        atts.st_ino = (ino_t) iso_read_bb(record->block, 4, NULL);
        if (fsdata->rr == 0) {
            atts.st_nlink = 1;
        }
    }
    
    /* 
     * if we haven't RR extensions, or a needed TF time stamp is not present,
     * we use plain iso recording time
     */
    recorded = iso_datetime_read_7(record->recording_time);
    if (atts.st_atime == (time_t) 0) {
        atts.st_atime = recorded;
    }
    if (atts.st_ctime == (time_t) 0) {
        atts.st_ctime = recorded;
    }
    if (atts.st_mtime == (time_t) 0) {
        atts.st_mtime = recorded;
    }
    
    /* the size is read from iso directory record */
    atts.st_size = iso_read_bb(record->length, 4, NULL);
    
    /* Fill last entries */
    atts.st_dev = fsdata->id;
    atts.st_blksize = BLOCK_SIZE;
    atts.st_blocks = div_up(atts.st_size, BLOCK_SIZE);
    
    //TODO more sanity checks!!
    if (S_ISLNK(atts.st_mode) && (linkdest == NULL)) {
        iso_msg_sorry(fsdata->messenger, LIBISO_RR_ERROR, 
                      "Link without destination.");
        free(name);
        return ISO_WRONG_RR;
    }
    
    /* ok, we can now create the file source */
    ifsdata = calloc(1, sizeof(ImageFileSourceData));
    if (ifsdata == NULL) {
        ret = ISO_MEM_ERROR;
        goto ifs_cleanup;
    }
    ifsrc = calloc(1, sizeof(IsoFileSource));
    if (ifsrc == NULL) {
        ret = ISO_MEM_ERROR;
        goto ifs_cleanup;
    }
    
    /* fill data */
    ifsdata->fs = fs;
    iso_filesystem_ref((IsoFilesystem*)fs);
    if (parent != NULL) {
        ifsdata->parent = parent;
        iso_file_source_ref(parent);
    }
    ifsdata->info = atts;
    ifsdata->name = name;
    ifsdata->block = iso_read_bb(record->block, 4, NULL);
    
    if (S_ISLNK(atts.st_mode)) {
        ifsdata->data.content = linkdest;
    }
    
    ifsrc->class = &ifs_class;
    ifsrc->data = ifsdata;
    ifsrc->refcount = 1;
    
    *src = ifsrc;
    return ISO_SUCCESS;
    
ifs_cleanup: ;
    free(name);
    free(linkdest);
    free(ifsdata);
    free(ifsrc);
    return ret;
}

static
int ifs_get_root(IsoFilesystem *fs, IsoFileSource **root)
{
    int ret;
    _ImageFsData *data;
    uint8_t buffer[BLOCK_SIZE];

    if (fs == NULL || fs->data == NULL || root == NULL) {
        return ISO_NULL_POINTER;
    }

    data = (_ImageFsData*)fs->data;

    /* open the filesystem */
    ret = ifs_fs_open((IsoImageFilesystem*)fs);
    if (ret < 0) {
        return ret;
    }

    /* read extend for root record */
    ret = data->src->read_block(data->src, data->iso_root_block, buffer);
    if (ret < 0) {
        ifs_fs_close((IsoImageFilesystem*)fs);
        return ret;
    }

    /* get root attributes from "." entry */
    ret = iso_file_source_new_ifs((IsoImageFilesystem*)fs, NULL,
                                   (struct ecma119_dir_record*) buffer, root);

    ifs_fs_close((IsoImageFilesystem*)fs);
    return ret;
}

/**
 * Find a file inside a node.
 * 
 * @param file
 *     it is not modified if requested file is not found
 * @return
 *     1 success, 0 not found, < 0 error
 */
static
int ifs_get_file(IsoFileSource *dir, const char *name, IsoFileSource **file)
{
    int ret;
    IsoFileSource *src;

    ret = iso_file_source_open(dir);
    if (ret < 0) {
        return ret;
    }
    while ((ret = iso_file_source_readdir(dir, &src)) == 1) {
        char *fname = iso_file_source_get_name(src);
        if (!strcmp(name, fname)) {
            free(fname);
            *file = src;
            ret = ISO_SUCCESS;
            break;
        }
        free(fname);
        iso_file_source_unref(src);
    }
    iso_file_source_close(dir);
    return ret;
}

static
int ifs_get_by_path(IsoFilesystem *fs, const char *path, IsoFileSource **file)
{
    int ret;
    _ImageFsData *data;
    IsoFileSource *src;
    char *ptr, *brk_info, *component;

    if (fs == NULL || fs->data == NULL || path == NULL || file == NULL) {
        return ISO_NULL_POINTER;
    }
    
    if (path[0] != '/') {
        /* only absolute paths supported */
        return ISO_FILE_BAD_PATH;
    }

    data = (_ImageFsData*)fs->data;

    /* open the filesystem */
    ret = ifs_fs_open((IsoImageFilesystem*)fs);
    if (ret < 0) {
        return ret;
    }
    
    ret = ifs_get_root(fs, &src);
    if (ret < 0) {
        return ret;
    }
    if (!strcmp(path, "/")) {
        /* we are looking for root */
        *file = src;
        ret = ISO_SUCCESS;
        goto get_path_exit;
    }

    ptr = strdup(path);
    if (ptr == NULL) {
        iso_file_source_unref(src);
        ret = ISO_MEM_ERROR;
        goto get_path_exit;
    }
    
    component = strtok_r(ptr, "/", &brk_info);
    while (component) {
        IsoFileSource *child = NULL;
        
        ImageFileSourceData *fdata;
        fdata = src->data;
        if (!S_ISDIR(fdata->info.st_mode)) {
            ret = ISO_FILE_BAD_PATH;
            break;
        }

        ret = ifs_get_file(src, component, &child);
        iso_file_source_unref(src);
        if (ret <= 0) {
            break;
        }
        
        src = child;
        component = strtok_r(NULL, "/", &brk_info);
    }

    free(ptr);
    if (ret < 0) {
        iso_file_source_unref(src);
    } else if (ret == 0) {
        ret = ISO_FILE_DOESNT_EXIST;
    } else {
        *file = src;
    }

    get_path_exit:;
    ifs_fs_close((IsoImageFilesystem*)fs);
    return ret;
}

unsigned int ifs_get_id(IsoFilesystem *fs)
{
    return ISO_IMAGE_FS_ID;
}

static
int ifs_fs_open(IsoImageFilesystem *fs)
{
    _ImageFsData *data;

    if (fs == NULL || fs->fs.data == NULL) {
        return ISO_NULL_POINTER;
    }

    data = (_ImageFsData*)fs->fs.data;

    if (data->open_count == 0) {
        /* we need to actually open the data source */
        int res = data->src->open(data->src);
        if (res < 0) {
            return res;
        }
    }
    ++data->open_count;
    return ISO_SUCCESS;
}

static
int ifs_fs_close(IsoImageFilesystem *fs)
{
    _ImageFsData *data;

    if (fs == NULL || fs->fs.data == NULL) {
        return ISO_NULL_POINTER;
    }

    data = (_ImageFsData*)fs->fs.data;

    if (--data->open_count == 0) {
        /* we need to actually close the data source */
        return data->src->close(data->src);
    }
    return ISO_SUCCESS;
}

static
void ifs_fs_free(IsoFilesystem *fs)
{
    IsoImageFilesystem *ifs;
    _ImageFsData *data;

    ifs = (IsoImageFilesystem*)fs;
    data = (_ImageFsData*) fs->data;

    /* close data source if already openned */
    if (data->open_count > 0) {
        data->src->close(data->src);
    }

    /* free our ref to datasource */
    iso_data_source_unref(data->src);

    /* free volume atts */
    free(data->volset_id);
    free(data->volume_id);
    free(data->publisher_id);
    free(data->data_preparer_id);
    free(data->system_id);
    free(data->application_id);
    free(data->copyright_file_id);
    free(data->abstract_file_id);
    free(data->biblio_file_id);

    free(data->input_charset);
    free(data->local_charset);
    free(data);
}

/**
 * Read the SUSP system user entries of the "." entry of the root directory, 
 * indentifying when Rock Ridge extensions are being used.
 */
static 
int read_root_susp_entries(_ImageFsData *data, uint32_t block)
{
    int ret;
    unsigned char buffer[2048];
    struct ecma119_dir_record *record;
    struct susp_sys_user_entry *sue;
    SuspIterator *iter;

    ret = data->src->read_block(data->src, block, buffer);
    if (ret < 0) {
        return ret;
    }
    
    /* record will be the "." directory entry for the root record */
    record = (struct ecma119_dir_record *)buffer;
    
    /*
     * TODO 
     * SUSP specification claims that for CD-ROM XA the SP entry
     * is not at position BP 1, but at BP 15. Is that used?
     * In that case, we need to set info->len_skp to 15!!
     */

    iter = susp_iter_new(data->src, record, data->len_skp, data->messenger);
    if (iter == NULL) {
        return ISO_MEM_ERROR;
    }
    
    /* first entry must be an SP system use entry */
    ret = susp_iter_next(iter, &sue);
    if (ret < 0) {
        /* error */
        susp_iter_free(iter);
        return ret;
    } else if (ret == 0 || !SUSP_SIG(sue, 'S', 'P') ) {
        iso_msg_debug(data->messenger, "SUSP/RR is not being used.");
        susp_iter_free(iter);
        return ISO_SUCCESS;
    }
    
    /* it is a SP system use entry */
    if (sue->version[0] != 1 || sue->data.SP.be[0] != 0xBE
        || sue->data.SP.ef[0] != 0xEF) {
    
        iso_msg_sorry(data->messenger, LIBISO_SUSP_WRONG, "SUSP SP system use "
            "entry seems to be wrong. Ignoring Rock Ridge Extensions.");
        susp_iter_free(iter);
        return ISO_SUCCESS;
    }
    
    iso_msg_debug(data->messenger, "SUSP/RR is being used.");
    
    /*
     * The LEN_SKP field, defined in IEEE 1281, SUSP. 5.3, specifies the
     * number of bytes to be skipped within each System Use field.
     * I think this will be always 0, but given that support this standard
     * feature is easy...
     */
    data->len_skp = sue->data.SP.len_skp[0];
    
    /*
     * Ok, now search for ER entry.
     * Just notice that the attributes for root dir are read elsewhere.
     * 
     * TODO if several ER are present, we need to identify the position of
     *      what refers to RR, and then look for corresponding ES entry in
     *      each directory record. I have not implemented this (it's not used,
     *      no?), but if we finally need it, it can be easily implemented in
     *      the iterator, transparently for the rest of the code.
     */
    while ((ret = susp_iter_next(iter, &sue)) > 0) {
        
        /* ignore entries from different version */
        if (sue->version[0] != 1)
            continue; 
        
        if (SUSP_SIG(sue, 'E', 'R')) {
               
            if (data->rr_version) {
                iso_msg_warn(data->messenger, LIBISO_SUSP_MULTIPLE_ER, 
                    "More than one ER has found. This is not supported. "
                    "It will be ignored, but can cause problems. "
                    "Please notify us about this.");
            }

            /* 
             * it seems that Rock Ridge can be identified with any
             * of the following 
             */
            if ( sue->data.ER.len_id[0] == 10 && 
                 !strncmp((char*)sue->data.ER.ext_id, "RRIP_1991A", 10) ) {
                
                iso_msg_debug(data->messenger, 
                              "Suitable Rock Ridge ER found. Version 1.10.");
                data->rr_version = RR_EXT_110;
                
            } else if ( (sue->data.ER.len_id[0] == 10 && 
                    !strncmp((char*)sue->data.ER.ext_id, "IEEE_P1282", 10)) 
                 || (sue->data.ER.len_id[0] == 9 && 
                    !strncmp((char*)sue->data.ER.ext_id, "IEEE_1282", 9)) ) {
                 
                iso_msg_debug(data->messenger, 
                              "Suitable Rock Ridge ER found. Version 1.12.");
                data->rr_version = RR_EXT_112;
                //TODO check also version?
            } else {
                iso_msg_warn(data->messenger, LIBISO_SUSP_MULTIPLE_ER, 
                    "Not Rock Ridge ER found.\n"
                    "That will be ignored, but can cause problems in "
                    "image reading. Please notify us about this");
            }
        }
    }
    
    susp_iter_free(iter);   
    
    if (ret < 0) { 
        return ret;
    }
    
    return ISO_SUCCESS;
}

static
int read_pvm(_ImageFsData *data, uint32_t block)
{
    int ret;
    struct ecma119_pri_vol_desc *pvm;
    struct ecma119_dir_record *rootdr;
    uint8_t buffer[BLOCK_SIZE];

    /* read PVM */
    ret = data->src->read_block(data->src, block, buffer);
    if (ret < 0) {
        return ret;
    }

    pvm = (struct ecma119_pri_vol_desc *)buffer;

    /* sanity checks */
    if (pvm->vol_desc_type[0] != 1 || pvm->vol_desc_version[0] != 1
            || strncmp((char*)pvm->std_identifier, "CD001", 5)
            || pvm->file_structure_version[0] != 1) {

        return ISO_WRONG_PVD;
    }

    /* ok, it is a valid PVD */

    /* fill volume attributes  */
    data->volset_id = strcopy((char*)pvm->vol_set_id, 128);
    data->volume_id = strcopy((char*)pvm->volume_id, 32);
    data->publisher_id = strcopy((char*)pvm->publisher_id, 128);
    data->data_preparer_id = strcopy((char*)pvm->data_prep_id, 128);
    data->system_id = strcopy((char*)pvm->system_id, 32);
    data->application_id = strcopy((char*)pvm->application_id, 128);
    data->copyright_file_id = strcopy((char*)pvm->copyright_file_id, 37);
    data->abstract_file_id = strcopy((char*)pvm->abstract_file_id, 37);
    data->biblio_file_id = strcopy((char*)pvm->bibliographic_file_id, 37);

    data->nblocks = iso_read_bb(pvm->vol_space_size, 4, NULL);

    rootdr = (struct ecma119_dir_record*) pvm->root_dir_record;
    data->pvd_root_block = iso_read_bb(rootdr->block, 4, NULL);

    /*
     * TODO
     * PVM has other things that could be interesting, but that don't have a 
     * member in IsoImage, such as creation date. In a multisession disc, we 
     * could keep the creation date and update the modification date, for 
     * example.
     */

    return ISO_SUCCESS;
}

int iso_image_filesystem_new(IsoDataSource *src, struct iso_read_opts *opts,
                             struct libiso_msgs *messenger,
                             IsoImageFilesystem **fs)
{
    int ret;
    uint32_t block;
    IsoImageFilesystem *ifs;
    _ImageFsData *data;
    uint8_t buffer[BLOCK_SIZE];

    if (src == NULL || opts == NULL || fs == NULL) {
        return ISO_NULL_POINTER;
    }

    data = calloc(1, sizeof(_ImageFsData));
    if (data == NULL) {
        return ISO_MEM_ERROR;
    }

    ifs = calloc(1, sizeof(IsoImageFilesystem));
    if (ifs == NULL) {
        free(data);
        return ISO_MEM_ERROR;
    }

    /* get our ref to IsoDataSource */
    data->src = src;
    iso_data_source_ref(src);
    data->open_count = 0;

    /* get an id for the filesystem */
    data->id = ++fs_dev_id;
    
    /* fill data from opts */
    data->gid = opts->gid;
    data->uid = opts->uid;
    data->mode = opts->mode & ~S_IFMT;
    data->messenger = messenger;
    
    setlocale(LC_CTYPE, "");
    data->local_charset = strdup(nl_langinfo(CODESET));
    if (data->local_charset == NULL) {
        ret = ISO_MEM_ERROR;
        goto fs_cleanup;
    }
    if (opts->input_charset != NULL) {
        data->input_charset = strdup(opts->input_charset);
    } else {
        data->input_charset = strdup(data->local_charset);
    }
    if (data->input_charset == NULL) {
        ret = ISO_MEM_ERROR;
        goto fs_cleanup;
    }

    ifs->open = ifs_fs_open;
    ifs->close = ifs_fs_close;

    ifs->fs.data = data;
    ifs->fs.refcount = 1;
    ifs->fs.get_root = ifs_get_root;
    ifs->fs.get_by_path = ifs_get_by_path;
    ifs->fs.free = ifs_fs_free;
    ifs->fs.get_id = ifs_get_id;

    /* read Volume Descriptors and ensure it is a valid image */

    /* 1. first, open the filesystem */
    ifs_fs_open(ifs);

    /* 2. read primary volume description */
    ret = read_pvm(data, opts->block + 16);
    if (ret < 0) {
        goto fs_cleanup;
    }

    /* 3. read next volume descriptors */
    block = opts->block + 17;
    do {
        ret = src->read_block(src, block, buffer);
        if (ret < 0) {
            /* cleanup and exit */
            goto fs_cleanup;
        }
        switch (buffer[0]) {
        case 0:
            /* 
             * This is a boot record 
             * Here we handle el-torito
             */
            //TODO add support for El-Torito
            iso_msg_hint(data->messenger, LIBISO_UNSUPPORTED_VD,
                         "El-Torito extensions not supported yet");
            break;
        case 2:
            /* suplementary volume descritor */
            {
                struct ecma119_sup_vol_desc *sup;
                struct ecma119_dir_record *root;
                
                sup = (struct ecma119_sup_vol_desc*)buffer;
                if (sup->esc_sequences[0] == 0x25 && 
                    sup->esc_sequences[1] == 0x2F &&
                    (sup->esc_sequences[2] == 0x40 ||
                     sup->esc_sequences[2] == 0x43 ||
                     sup->esc_sequences[2] == 0x45) ) {
                    
                    /* it's a Joliet Sup. Vol. Desc. */
                    data->joliet = 1;
                    root = (struct ecma119_dir_record*)sup->root_dir_record;
                    data->svd_root_block = iso_read_bb(root->block, 4, NULL);

                    //TODO maybe we can set the IsoImage attribs from this
                    //descriptor
                } else {
                    iso_msg_hint(data->messenger, LIBISO_UNSUPPORTED_VD,
                        "Unsupported (not Joliet) Sup. Vol. Desc found.");
                }
            }
            break;
        case 255:
            /* 
             * volume set terminator
             * ignore, as it's checked in loop end condition
             */
            break;
        default:
            {
                iso_msg_hint(data->messenger, LIBISO_UNSUPPORTED_VD,
                             "Ignoring Volume descriptor %x.", buffer[0]);
            }
            break;
        }
        block++;
    } while (buffer[0] != 255);

    /* 4. check if RR extensions are being used */
    ret = read_root_susp_entries(data, data->pvd_root_block);
    if (ret < 0) {
        return ret;
    }
    
    /* user doesn't want to read RR extensions */
    if (opts->norock) {
        data->rr = RR_EXT_NO;
    } else {
        data->rr = data->rr_version;
    }

    /* select what tree to read */
    if (data->rr) {
        /* RR extensions are available */
        if (!opts->nojoliet && opts->preferjoliet && data->joliet) {
            /* if user prefers joliet, that is used */
            iso_msg_debug(data->messenger, "Reading Joliet extensions.");
            data->get_name = ucs2str;
            data->rr = RR_EXT_NO;
            data->iso_root_block = data->svd_root_block;
        } else {
            /* RR will be used */
            iso_msg_debug(data->messenger, "Reading Rock Ridge extensions.");
            data->iso_root_block = data->pvd_root_block;
            data->get_name = strcopy;
        }
    } else {
        /* RR extensions are not available */
        if (!opts->nojoliet && data->joliet) {
            /* joliet will be used */
            iso_msg_debug(data->messenger, "Reading Joliet extensions.");
            data->get_name = ucs2str;
            data->iso_root_block = data->svd_root_block;
        } else {
            /* default to plain iso */
            iso_msg_debug(data->messenger, "Reading plain ISO-9660 tree.");
            data->iso_root_block = data->pvd_root_block;
            data->get_name = strcopy;
        }
    }

    /* and finally return. Note that we keep the DataSource opened */

    *fs = ifs;
    return ISO_SUCCESS;

    fs_cleanup: ;
    ifs_fs_free((IsoFilesystem*)ifs);
    free(ifs);
    return ret;
}

static
int image_builder_create_node(IsoNodeBuilder *builder, IsoImage *image,
                              IsoFileSource *src, IsoNode **node)
{
    int result;
    struct stat info;
    IsoNode *new;
    char *name;
    ImageFileSourceData *data;

    if (builder == NULL || src == NULL || node == NULL || src->data == NULL) {
        return ISO_NULL_POINTER;
    }

    data = (ImageFileSourceData*)src->data;

    name = iso_file_source_get_name(src);

    /* get info about source */
    result = iso_file_source_lstat(src, &info);
    if (result < 0) {
        return result;
    }

    new = NULL;
    switch (info.st_mode & S_IFMT) {
    case S_IFREG:
        {
            /* source is a regular file */
            IsoStream *stream;
            IsoFile *file;
            result = iso_file_source_stream_new(src, &stream);
            if (result < 0) {
                free(name);
                return result;
            }
            /* take a ref to the src, as stream has taken our ref */
            iso_file_source_ref(src);
            file = calloc(1, sizeof(IsoFile));
            if (file == NULL) {
                free(name);
                iso_stream_unref(stream);
                return ISO_MEM_ERROR;
            }

            /* the msblock is taken from the image */
            file->msblock = data->block;
            
            /* 
             * and we set the sort weight based on the block on image, to
             * improve performance on image modifying.
             */ 
            file->sort_weight = INT_MAX - data->block;

            file->stream = stream;
            file->node.type = LIBISO_FILE;
            new = (IsoNode*) file;
        }
        break;
    case S_IFDIR:
        {
            /* source is a directory */
            new = calloc(1, sizeof(IsoDir));
            if (new == NULL) {
                free(name);
                return ISO_MEM_ERROR;
            }
            new->type = LIBISO_DIR;
        }
        break;
    case S_IFLNK:
        {
            /* source is a symbolic link */
            char dest[PATH_MAX];
            IsoSymlink *link;

            result = iso_file_source_readlink(src, dest, PATH_MAX);
            if (result < 0) {
                free(name);
                return result;
            }
            link = malloc(sizeof(IsoSymlink));
            if (link == NULL) {
                free(name);
                return ISO_MEM_ERROR;
            }
            link->dest = strdup(dest);
            link->node.type = LIBISO_SYMLINK;
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
            special = malloc(sizeof(IsoSpecial));
            if (special == NULL) {
                free(name);
                return ISO_MEM_ERROR;
            }
            special->dev = info.st_rdev;
            special->node.type = LIBISO_SPECIAL;
            new = (IsoNode*) special;
        }
        break;
    }

    /* fill fields */
    new->refcount = 1;
    new->name = name;
    new->mode = info.st_mode;
    new->uid = info.st_uid;
    new->gid = info.st_gid;
    new->atime = info.st_atime;
    new->mtime = info.st_mtime;
    new->ctime = info.st_ctime;

    new->hidden = 0;

    new->parent = NULL;
    new->next = NULL;

    *node = new;
    return ISO_SUCCESS;
}

/**
 * Create a new builder, that is exactly a copy of an old builder, but where
 * create_node() function has been replaced by image_builder_create_node.
 */
int iso_image_builder_new(IsoNodeBuilder *old, IsoNodeBuilder **builder)
{
    IsoNodeBuilder *b;

    if (builder == NULL) {
        return ISO_NULL_POINTER;
    }

    b = malloc(sizeof(IsoNodeBuilder));
    if (b == NULL) {
        return ISO_MEM_ERROR;
    }

    b->refcount = 1;
    b->create_file_data = old->create_file_data;
    b->create_node_data = old->create_node_data;
    b->create_file = old->create_file;
    b->create_node = image_builder_create_node;
    b->free = old->free;

    *builder = b;
    return ISO_SUCCESS;
}

int iso_image_import(IsoImage *image, IsoDataSource *src,
                     struct iso_read_opts *opts, 
                     struct iso_read_image_features *features)
{
    int ret;
    IsoImageFilesystem *fs;
    IsoFilesystem *fsback;
    IsoNodeBuilder *blback;
    IsoDir *oldroot;
    IsoFileSource *newroot;
    
    if (image == NULL || src == NULL || opts == NULL) {
        return ISO_NULL_POINTER;
    }
    
    ret = iso_image_filesystem_new(src, opts, image->messenger, &fs);
    if (ret < 0) {
        return ret;
    }
    
    /* get root from filesystem */
    ret = fs->fs.get_root((IsoFilesystem*)fs, &newroot);
    if (ret < 0) {
        return ret;
    }
    
    /* backup image filesystem, builder and root */
    fsback = image->fs;
    blback = image->builder;
    oldroot = image->root;
    
    /* create new builder */
    ret = iso_image_builder_new(blback, &image->builder);
    if (ret < 0) {
        goto import_revert;
    }
    
    image->fs = (IsoFilesystem*)fs;

    /* create new root, and set root attributes from source */
    ret = iso_node_new_root(&image->root);
    if (ret < 0) {
        goto import_revert;
    }
    {
        struct stat info;
        
        /* I know this will not fail */
        iso_file_source_lstat(newroot, &info);
        image->root->node.mode = info.st_mode;
        image->root->node.uid = info.st_uid;
        image->root->node.gid = info.st_gid;
        image->root->node.atime = info.st_atime;
        image->root->node.mtime = info.st_mtime;
        image->root->node.ctime = info.st_ctime;
    }    

    /* recursively add image */
    ret = iso_add_dir_src_rec(image, image->root, newroot);
    
    iso_node_builder_unref(image->builder);
    
    /* error during recursive image addition? */
    if (ret <= 0) {
        goto import_revert;
    }
    
    /* free old root */
    iso_node_unref((IsoNode*)oldroot);
    
    /* recover backed fs and builder */
    image->fs = fsback;
    image->builder = blback;
    
    {
        _ImageFsData *data;
        data = fs->fs.data;

        /* set volume attributes */
        iso_image_set_volset_id(image, data->volset_id);
        iso_image_set_volume_id(image, data->volume_id);
        iso_image_set_publisher_id(image, data->publisher_id);
        iso_image_set_data_preparer_id(image, data->data_preparer_id);
        iso_image_set_system_id(image, data->system_id);
        iso_image_set_application_id(image, data->application_id);
        iso_image_set_copyright_file_id(image, data->copyright_file_id);
        iso_image_set_abstract_file_id(image, data->abstract_file_id);
        iso_image_set_biblio_file_id(image, data->biblio_file_id);
                
        if (features != NULL) {
            features->hasJoliet = data->joliet;
            features->hasRR = data->rr_version != 0;
            features->size = data->nblocks;
        }
    }
    
    ret = ISO_SUCCESS;
    goto import_cleanup;
    
    import_revert:;
    
    image->root = oldroot;
    image->fs = fsback;
    
    import_cleanup:;
    
    iso_file_source_unref(newroot);
    fs->close(fs);
    iso_filesystem_unref((IsoFilesystem*)fs);
    
    return ret;
}
