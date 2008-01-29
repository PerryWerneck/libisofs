/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */
#ifndef LIBISO_NODE_H_
#define LIBISO_NODE_H_

/*
 * Definitions for the public iso tree
 */

#include "libisofs.h"
#include "stream.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

/* #define LIBISO_EXTENDED_INFORMATION */
#ifdef LIBISO_EXTENDED_INFORMATION

/**
 * The extended information is a way to attach additional information to each
 * IsoNode. External applications may want to use this extension system to 
 * store application speficic information related to each node. On the other
 * side, libisofs may make use of this struct to attach information to nodes in
 * some particular, uncommon, cases, without incrementing the size of the
 * IsoNode struct.
 * 
 * It is implemented like a chained list.
 */
typedef struct iso_extended_info IsoExtendedInfo;

struct iso_extended_info {
    /**
     * Next struct in the chain. NULL if it is the last item
     */
    IsoExtendedInfo *next;
    
    /**
     * Function to handle this particular extended information. The function
     * pointer acts as an identifier for the type of the information. Structs
     * with same information type must use the same function.
     * 
     * @param data
     *     Attached data
     * @param flag
     *     What to do with the data. At this time the following values are 
     *     defined:
     *      -> 1 the data must be freed
     * @return
     *     1
     */
    int (*process)(void *data, int flag);
    
    /**
     * Pointer to information specific data.
     */
    void *data;
};

#endif

/**
 * 
 */
struct Iso_Node
{
    /*
     * Initilized to 1, originally owned by user, until added to another node.
     * Then it is owned by the parent node, so the user must take his own ref 
     * if needed. With the exception of the creation functions, none of the
     * other libisofs functions that return an IsoNode increment its 
     * refcount. This is responsablity of the client, if (s)he needs it.
     */
    int refcount;

    /** Type of the IsoNode, do not confuse with mode */
    enum IsoNodeType type;

    char *name; /**< Real name, in default charset */

    mode_t mode; /**< protection */
    uid_t uid; /**< user ID of owner */
    gid_t gid; /**< group ID of owner */

    /* TODO #00001 : consider adding new timestamps */
    time_t atime; /**< time of last access */
    time_t mtime; /**< time of last modification */
    time_t ctime; /**< time of last status change */

    int hidden; /**< whether the node will be hidden, see IsoHideNodeFlag */

    IsoDir *parent; /**< parent node, NULL for root */

    /*
     * Pointer to the linked list of children in a dir.
     */
    IsoNode *next;

#ifdef LIBISO_EXTENDED_INFORMATION
    /**
     * Extended information for the node.
     */
    IsoExtendedInfo *xinfo;
#endif
};

struct Iso_Dir
{
    IsoNode node;

    size_t nchildren; /**< The number of children of this directory. */
    IsoNode *children; /**< list of children. ptr to first child */
};

struct Iso_File
{
    IsoNode node;

    /**
     * Location of a file extent in a ms disc, 0 for newly added file
     */
    uint32_t msblock;

    /** 
     * It sorts the order in which the file data is written to the CD image.
     * Higher weighting files are written at the beginning of image 
     */
    int sort_weight;
    IsoStream *stream;
};

struct Iso_Symlink
{
    IsoNode node;

    char *dest;
};

struct Iso_Special
{
    IsoNode node;
    dev_t dev;
};

/**
 * An iterator for directory children.
 */
struct Iso_Dir_Iter
{
    const IsoDir *dir;
    IsoNode *pos;
};

int iso_node_new_root(IsoDir **root);

/**
 * Create a new IsoDir. Attributes, uid/gid, timestamps, etc are set to 
 * default (0) values. You must set them.
 * 
 * @param name
 *      Name for the node. It is not strdup() so you shouldn't use this 
 *      reference when this function returns successfully. NULL is not 
 *      allowed.
 * @param dir
 *      
 * @return
 *      1 on success, < 0 on error.
 */
int iso_node_new_dir(char *name, IsoDir **dir);

/**
 * Create a new file node. Attributes, uid/gid, timestamps, etc are set to 
 * default (0) values. You must set them.
 * 
 * @param name
 *      Name for the node. It is not strdup() so you shouldn't use this 
 *      reference when this function returns successfully. NULL is not 
 *      allowed.
 * @param stream
 *      Source for file contents. The reference is taken by the node,
 *      you must call iso_stream_ref() if you need your own ref.
 * @return
 *      1 on success, < 0 on error.
 */
int iso_node_new_file(char *name, IsoStream *stream, IsoFile **file);

/**
 * Creates a new IsoSymlink node. Attributes, uid/gid, timestamps, etc are set
 * to default (0) values. You must set them.
 * 
 * @param name
 *      name for the new symlink. It is not strdup() so you shouldn't use this 
 *      reference when this function returns successfully. NULL is not 
 *      allowed.
 * @param dest
 *      destination of the link. It is not strdup() so you shouldn't use this 
 *      reference when this function returns successfully. NULL is not 
 *      allowed.
 * @param link
 *      place where to store a pointer to the newly created link.
 * @return
 *     1 on success, < 0 otherwise
 */
int iso_node_new_symlink(char *name, char *dest, IsoSymlink **link);

/**
 * Create a new special file node. As far as libisofs concerns,
 * an special file is a block device, a character device, a FIFO (named pipe)
 * or a socket. You can choose the specific kind of file you want to add
 * by setting mode propertly (see man 2 stat).
 * 
 * Note that special files are only written to image when Rock Ridge 
 * extensions are enabled. Moreover, a special file is just a directory entry
 * in the image tree, no data is written beyond that.
 * 
 * Owner and hidden atts are taken from parent. You can modify any of them 
 * later.
 * 
 * @param name
 *      name for the new special file. It is not strdup() so you shouldn't use 
 *      this reference when this function returns successfully. NULL is not 
 *      allowed.
 * @param mode
 *      file type and permissions for the new node. Note that you can't
 *      specify any kind of file here, only special types are allowed. i.e,
 *      S_IFSOCK, S_IFBLK, S_IFCHR and S_IFIFO are valid types; S_IFLNK, 
 *      S_IFREG and S_IFDIR aren't.
 * @param dev
 *      device ID, equivalent to the st_rdev field in man 2 stat.
 * @param special
 *      place where to store a pointer to the newly created special file.
 * @return
 *     1 on success, < 0 otherwise
 */
int iso_node_new_special(char *name, mode_t mode, dev_t dev, 
                         IsoSpecial **special);

/**
 * Check if a given name is valid for an iso node.
 * 
 * @return
 *     1 if yes, 0 if not
 */
int iso_node_is_valid_name(const char *name);

/**
 * Check if a given path is valid for the destination of a link.
 * 
 * @return
 *     1 if yes, 0 if not
 */
int iso_node_is_valid_link_dest(const char *dest);

/**
 * Find the position where to insert a node
 * 
 * @param dir
 *      A valid dir. It can't be NULL
 * @param name
 *      The node name to search for. It can't be NULL
 * @param pos
 *      Will be filled with the position where to insert. It can't be NULL
 */
void iso_dir_find(IsoDir *dir, const char *name, IsoNode ***pos);

/**
 * Check if a node with the given name exists in a dir.
 * 
 * @param dir
 *      A valid dir. It can't be NULL
 * @param name
 *      The node name to search for. It can't be NULL
 * @param pos
 *      If not NULL, will be filled with the position where to insert. If the
 *      node exists, (**pos) will refer to the given node.
 * @return
 *      1 if node exists, 0 if not
 */
int iso_dir_exists(IsoDir *dir, const char *name, IsoNode ***pos);

/**
 * Inserts a given node in a dir, at the specified position.
 * 
 * @param dir
 *     Dir where to insert. It can't be NULL
 * @param node
 *     The node to insert. It can't be NULL
 * @param pos
 *     Position where the node will be inserted. It is a pointer previously
 *     obtained with a call to iso_dir_exists() or iso_dir_find(). 
 *     It can't be NULL.
 * @param replace 
 *     Whether to replace an old node with the same name with the new node.
 * @return
 *     If success, number of children in dir. < 0 on error  
 */
int iso_dir_insert(IsoDir *dir, IsoNode *node, IsoNode **pos, 
                   enum iso_replace_mode replace);

#endif /*LIBISO_NODE_H_*/