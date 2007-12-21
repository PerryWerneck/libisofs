/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

#include "ecma119_tree.h"
#include "ecma119.h"
#include "error.h"
#include "node.h"
#include "util.h"
#include "filesrc.h"
#include "messages.h"
#include "image.h"
#include "stream.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static
int get_iso_name(Ecma119Image *img, IsoNode *iso, char **name)
{
    int ret;
    char *ascii_name;

    if (iso->name == NULL) {
        /* it is not necessarily an error, it can be the root */
        return ISO_SUCCESS;
    }

    // TODO add support for other input charset
    ret = str2ascii("UTF-8", iso->name, &ascii_name);
    if (ret < 0) {
        return ret;
    }

    // TODO add support for relaxed constraints
    if (iso->type == LIBISO_DIR) {
        if (img->iso_level == 1) {
            iso_dirid(ascii_name, 8);
        } else {
            iso_dirid(ascii_name, 31);
        }
    } else {
        if (img->iso_level == 1) {
            iso_1_fileid(ascii_name);
        } else {
            iso_2_fileid(ascii_name);
        }
    }
    *name = ascii_name;
    return ISO_SUCCESS;
}

static
int create_ecma119_node(Ecma119Image *img, IsoNode *iso, Ecma119Node **node)
{
    Ecma119Node *ecma;

    ecma = calloc(1, sizeof(Ecma119Node));
    if (ecma == NULL) {
        return ISO_MEM_ERROR;
    }

    /* take a ref to the IsoNode */
    ecma->node = iso;
    iso_node_ref(iso);

    // TODO what to do with full name? For now, not a problem, as we
    // haven't support for charset conversion. However, one we had it,
    // we need to choose whether to do it here (consumes more memory)
    // or on writting
    *node = ecma;
    return ISO_SUCCESS;
}

/**
 * Create a new ECMA-119 node representing a directory from a iso directory
 * node.
 */
static
int create_dir(Ecma119Image *img, IsoDir *iso, Ecma119Node **node)
{
    int ret;
    Ecma119Node **children;

    children = calloc(1, sizeof(void*) * iso->nchildren);
    if (children == NULL) {
        return ISO_MEM_ERROR;
    }

    ret = create_ecma119_node(img, (IsoNode*)iso, node);
    if (ret < 0) {
        free(children);
        return ret;
    }
    (*node)->type = ECMA119_DIR;
    (*node)->info.dir.nchildren = 0;
    (*node)->info.dir.children = children;
    return ISO_SUCCESS;
}

/**
 * Create a new ECMA-119 node representing a regular file from a iso file
 * node.
 */
static
int create_file(Ecma119Image *img, IsoFile *iso, Ecma119Node **node)
{
    int ret;
    IsoFileSrc *src;
    off_t size;

    size = iso_stream_get_size(iso->stream);
    if (size > (off_t)0xffffffff) {
        iso_msg_note(img->image, LIBISO_FILE_IGNORED, 
                     "File \"%s\" can't be added to image because is "
                     "greater than 4GB", iso->node.name);
        return 0;
    }

    ret = iso_file_src_create(img, iso, &src);
    if (ret < 0) {
        return ret;
    }

    ret = create_ecma119_node(img, (IsoNode*)iso, node);
    if (ret < 0) {
        /* 
         * the src doesn't need to be freed, it is free together with
         * the Ecma119Image 
         */
        return ret;
    }
    (*node)->type = ECMA119_FILE;
    (*node)->info.file = src;
    
    return ret;
}

void ecma119_node_free(Ecma119Node *node)
{
    if (node == NULL) {
        return;
    }
    if (node->type == ECMA119_DIR) {
        int i;
        for (i = 0; i < node->info.dir.nchildren; i++) {
            ecma119_node_free(node->info.dir.children[i]);
        }
        free(node->info.dir.children);
    }
    free(node->iso_name);
    iso_node_unref(node->node);
    //TODO? free(node->name);
    free(node);
}

/**
 * 
 * @return 
 *      1 success, 0 node ignored,  < 0 error
 * 
 */
static
int create_tree(Ecma119Image *image, IsoNode *iso, Ecma119Node **tree, 
                int depth, int pathlen)
{
    int ret;
    Ecma119Node *node;
    int max_path;
    char *iso_name = NULL;

    if (image == NULL || iso == NULL || tree == NULL) {
        return ISO_NULL_POINTER;
    }

    if (iso->hidden & LIBISO_HIDE_ON_RR) {
        /* file will be ignored */
        return 0;
    }
    ret = get_iso_name(image, iso, &iso_name);
    if (ret < 0) {
        return ret;
    }
    max_path = pathlen + 1 + (iso_name ? strlen(iso_name) : 0);
    if (1) { //TODO !rockridge && !relaxed_paths
        if (depth > 8 || max_path > 255) {
            iso_msg_note(image->image, LIBISO_FILE_IGNORED, 
                         "File \"%s\" can't be added, because depth > 8 "
                         "or path length over 255", iso->name);
            free(iso_name);
            return 0;
        }
    }

    switch(iso->type) {
    case LIBISO_FILE:
        ret = create_file(image, (IsoFile*)iso, &node);
        break;
    case LIBISO_SYMLINK:
        //TODO only supported with RR
        iso_msg_note(image->image, LIBISO_FILE_IGNORED, "File \"%s\" ignored. "
                     "Symlinks need RockRidge extensions.", iso->name);
        free(iso_name);
        return 0;
        break;
    case LIBISO_SPECIAL:
        iso_msg_note(image->image, LIBISO_FILE_IGNORED, "File \"%s\" ignored. "
                     "Special files need RockRidge extensions.", iso->name);
        //TODO only supported with RR
        free(iso_name);
        return 0;
        break;
    case LIBISO_BOOT:
        //TODO
        free(iso_name);
        return 0;
        break;
    case LIBISO_DIR: 
        {
            IsoNode *pos;
            IsoDir *dir = (IsoDir*)iso;
            ret = create_dir(image, dir, &node);
            if (ret < 0) {
                return ret;
            }
            pos = dir->children;
            while (pos) {
                Ecma119Node *child;
                ret = create_tree(image, pos, &child, depth + 1, max_path);
                if (ret < 0) {
                    /* error */
                    ecma119_node_free(node);
                    break;
                } else if (ret == ISO_SUCCESS) {
                    /* add child to this node */
                    int nchildren = node->info.dir.nchildren++;
                    node->info.dir.children[nchildren] = child;
                    child->parent = node;
                }
                pos = pos->next;
            }
        }
        break;
    default:
        /* should never happen */
        return ISO_ERROR;
    }
    if (ret <= 0) {
        free(iso_name);
        return ret;
    }
    node->iso_name = iso_name;
    *tree = node;
    return ISO_SUCCESS;
}

/**
 * Compare the iso name of two ECMA-119 nodes
 */
static 
int cmp_node_name(const void *f1, const void *f2)
{
    Ecma119Node *f = *((Ecma119Node**)f1);
    Ecma119Node *g = *((Ecma119Node**)f2);
    return strcmp(f->iso_name, g->iso_name);
}

/**
 * Sorts a the children of each directory in the ECMA-119 tree represented
 * by \p root, acording to the order specified in ECMA-119, section 9.3.
 */
static 
void sort_tree(Ecma119Node *root)
{
    size_t i;

    qsort(root->info.dir.children, root->info.dir.nchildren, 
          sizeof(void*), cmp_node_name);
    for (i = 0; i < root->info.dir.nchildren; i++) {
        if (root->info.dir.children[i]->type == ECMA119_DIR)
            sort_tree(root->info.dir.children[i]);
    }
}

static
int contains_name(Ecma119Node *dir, const char *name)
{
    int i;
    for (i = 0; i < dir->info.dir.nchildren; i++) {
        Ecma119Node *child = dir->info.dir.children[i];
        if (!strcmp(child->iso_name, name)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Ensures that the ISO name of each children of the given dir is unique,
 * changing some of them if needed.
 * It also ensures that resulting filename is always <= than given
 * max_name_len, including extension. If needed, the extension will be reduced,
 * but never under 3 characters.
 */
static
int mangle_dir(Ecma119Node *dir, int max_file_len, int max_dir_len)
{
    int i, nchildren;
    Ecma119Node **children;
    int need_sort = 0;
    
    nchildren = dir->info.dir.nchildren;
    children = dir->info.dir.children;
    
    for (i = 0; i < nchildren; ++i) {
        char *name, *ext;
        char full_name[40];
        int max; /* computed max len for name, without extension */
        int j = i; 
        int digits = 1; /* characters to change per name */
            
        /* first, find all child with same name */
        while (j + 1 < nchildren && 
               !cmp_node_name(children + i, children + j + 1)) {
            ++j;
        }
        if (j == i) {
            /* name is unique */
            continue;
        }
        
        /*
         * A max of 7 characters is good enought, it allows handling up to 
         * 9,999,999 files with same name. We can increment this to
         * max_name_len, but the int_pow() function must then be modified
         * to return a bigger integer.
         */
        while (digits < 8) {
            int ok, k;
            char *dot;
            int change = 0; /* number to be written */
            
            /* copy name to buffer */
            strcpy(full_name, children[i]->iso_name);
            
            /* compute name and extension */
            dot = strrchr(full_name, '.');
            if (dot != NULL && children[i]->type != ECMA119_DIR) {
                
                /* 
                 * File (not dir) with extension 
                 * Note that we don't need to check for placeholders, as
                 * tree reparent happens later, so no placeholders can be
                 * here at this time.
                 * 
                 * TODO !!! Well, we will need a way to mangle root names
                 * if we do reparent!
                 */
                int extlen;
                full_name[dot - full_name] = '\0';
                name = full_name;
                ext = dot + 1;
                
                /* 
                 * For iso level 1 we force ext len to be 3, as name
                 * can't grow on the extension space 
                 */
                extlen = (max_file_len == 12) ? 3 : strlen(ext);
                max = max_file_len - extlen - 1 - digits;
                if (max <= 0) {
                    /* this can happen if extension is too long */
                    if (extlen + max > 3) {
                        /* 
                         * reduce extension len, to give name an extra char
                         * note that max is negative or 0 
                         */
                        extlen = extlen + max - 1;
                        ext[extlen] = '\0';
                        max = max_file_len - extlen - 1 - digits;
                    } else {
                        /* 
                         * error, we don't support extensions < 3
                         * This can't happen with current limit of digits. 
                         */
                        return ISO_ERROR;
                    }
                }
                /* ok, reduce name by digits */
                if (name + max < dot) {
                    name[max] = '\0';
                }
            } else {
                /* Directory, or file without extension */
                if (children[i]->type == ECMA119_DIR) {
                    max = max_dir_len - digits;
                    dot = NULL; /* dots have no meaning in dirs */
                } else {
                    max = max_file_len - digits;
                }
                name = full_name;
                if (max < strlen(name)) {
                    name[max] = '\0';
                }
                /* let ext be an empty string */
                ext = name + strlen(name);
            }
            
            ok = 1;
            /* change name of each file */
            for (k = i; k <= j; ++k) {
                char tmp[40];
                char fmt[16];
                if (dot != NULL) {
                    sprintf(fmt, "%%s%%0%dd.%%s", digits);
                } else {
                    sprintf(fmt, "%%s%%0%dd%%s", digits);
                }
                while (1) {
                    sprintf(tmp, fmt, name, change, ext);
                    ++change;
                    if (change > int_pow(10, digits)) {
                        ok = 0;
                        break;
                    }
                    if (!contains_name(dir, tmp)) {
                        /* the name is unique, so it can be used */
                        break;
                    }
                }
                if (ok) {
                    char *new = strdup(tmp);
                    if (new == NULL) {
                        return ISO_MEM_ERROR;
                    }
                    free(children[k]->iso_name);
                    children[k]->iso_name = new;
                    /* 
                     * if we change a name we need to sort again children
                     * at the end
                     */
                    need_sort = 1;
                } else {
                    /* we need to increment digits */
                    break;
                }
            }
            if (ok) {
                break;
            } else {
                ++digits;
            }
        }
        if (digits == 8) {
            return ISO_MANGLE_TOO_MUCH_FILES;
        }
        i = j;
    }

    /*
     * If needed, sort again the files inside dir
     */
    if (need_sort) { 
        qsort(children, nchildren, sizeof(void*), cmp_node_name);
    }
    
    /* recurse */
    for (i = 0; i < nchildren; ++i) {
        int ret;
        if (children[i]->type == ECMA119_DIR) {
            ret = mangle_dir(children[i], max_file_len, max_dir_len);
            if (ret < 0) {
                /* error */
                return ret;
            }
        }
    }
    
    return ISO_SUCCESS;
}

static
int mangle_tree(Ecma119Image *img)
{
    int max_file, max_dir;
    
    // TODO take care about relaxed constraints
    if (img->iso_level == 1) {
        max_file = 12; /* 8 + 3 + 1 */
        max_dir = 8;
    } else {
        max_file = max_dir = 31;
    }
    return mangle_dir(img->root, max_file, max_dir);
}


int ecma119_tree_create(Ecma119Image *img)
{
    int ret;
    Ecma119Node *root;
    
    ret = create_tree(img, (IsoNode*)img->image->root, &root, 1, 0);
    if (ret < 0) {
        return ret;
    }
    img->root = root;
    sort_tree(root);
    
    ret = mangle_tree(img);
    if (ret < 0) {
        return ret;
    }
    
    /*
     * TODO
     * - reparent if RR
     * This must be done after mangle_tree, as name mangling may increment
     * file name length. After reparent, the root dir must be mangled again
     */
    
    return ISO_SUCCESS;
}