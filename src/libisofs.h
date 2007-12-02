/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */
#ifndef LIBISO_LIBISOFS_H_
#define LIBISO_LIBISOFS_H_

#include <sys/stat.h>

typedef struct Iso_Image IsoImage;

typedef struct Iso_Node IsoNode;
typedef struct Iso_Dir IsoDir;
typedef struct Iso_Symlink IsoSymlink;
typedef struct Iso_File IsoFile;

typedef struct Iso_Dir_Iter IsoDirIter;

/**
 * Create a new image, empty.
 * 
 * The image will be owned by you and should be unref() when no more needed.
 * 
 * @param name
 *     Name of the image. This will be used as volset_id and volume_id.
 * @param image
 *     Location where the image pointer will be stored.
 * @return
 *     1 sucess, < 0 error
 */
int iso_image_new(const char *name, IsoImage **image);

/**
 * Increments the reference counting of the given image.
 */
void iso_image_ref(IsoImage *image);

/**
 * Decrements the reference couting of the given image.
 * If it reaches 0, the image is free, together with its tree nodes (whether 
 * their refcount reach 0 too, of course).
 */
void iso_image_unref(IsoImage *image);

/**
 * Get the root directory of the image.
 * No extra ref is added to it, so you musn't unref it. Use iso_node_ref()
 * if you want to get your own reference.
 */
IsoDir *iso_image_get_root(const IsoImage *image);

/**
 * Fill in the volset identifier for a image.
 */
void iso_image_set_volset_id(IsoImage *image, const char *volset_id);

/** 
 * Get the volset identifier. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_volset_id(const IsoImage *image);

/**
 * Fill in the volume identifier for a image.
 */
void iso_image_set_volume_id(IsoImage *image, const char *volume_id);

/** 
 * Get the volume identifier. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_volume_id(const IsoImage *image);

/**
 * Fill in the publisher for a image.
 */
void iso_image_set_publisher_id(IsoImage *image, const char *publisher_id);

/** 
 * Get the publisher of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_publisher_id(const IsoImage *image);

/**
 * Fill in the data preparer for a image.
 */
void iso_image_set_data_preparer_id(IsoImage *image,
                     const char *data_preparer_id);

/** 
 * Get the data preparer of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_data_preparer_id(const IsoImage *image);

/**
 * Fill in the system id for a image. Up to 32 characters.
 */
void iso_image_set_system_id(IsoImage *image, const char *system_id);

/** 
 * Get the system id of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_system_id(const IsoImage *image);

/**
 * Fill in the application id for a image. Up to 128 chars.
 */
void iso_image_set_application_id(IsoImage *image, const char *application_id);

/** 
 * Get the application id of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_application_id(const IsoImage *image);

/**
 * Fill copyright information for the image. Usually this refers
 * to a file on disc. Up to 37 characters.
 */
void iso_image_set_copyright_file_id(IsoImage *image,
                     const char *copyright_file_id);

/** 
 * Get the copyright information of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_copyright_file_id(const IsoImage *image);

/**
 * Fill abstract information for the image. Usually this refers
 * to a file on disc. Up to 37 characters.
 */
void iso_image_set_abstract_file_id(IsoImage *image,
                     const char *abstract_file_id);

/** 
 * Get the abstract information of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_abstract_file_id(const IsoImage *image);

/**
 * Fill biblio information for the image. Usually this refers
 * to a file on disc. Up to 37 characters.
 */
void iso_image_set_biblio_file_id(IsoImage *image,
                     const char *biblio_file_id);

/** 
 * Get the biblio information of a image. 
 * The returned string is owned by the image and should not be freed nor
 * changed.
 */
const char *iso_image_get_biblio_file_id(const IsoImage *image);

/**
 * Increments the reference counting of the given node.
 */
void iso_node_ref(IsoNode *node);

/**
 * Decrements the reference couting of the given node.
 * If it reach 0, the node is free, and, if the node is a directory,
 * its children will be unref() too.
 */
void iso_node_unref(IsoNode *node);

/**
 * Set the name of a node.
 * 
 * @param name  The name in UTF-8 encoding
 */
void iso_node_set_name(IsoNode *node, const char *name);

/**
 * Get the name of a node (in UTF-8).
 * The returned string belongs to the node and should not be modified nor
 * freed. Use strdup if you really need your own copy.
 */
const char *iso_node_get_name(const IsoNode *node);

/**
 * Set the permissions for the node. This attribute is only useful when 
 * Rock Ridge extensions are enabled.
 * 
 * @param mode 
 *     bitmask with the permissions of the node, as specified in 'man 2 stat'.
 *     The file type bitfields will be ignored, only file permissions will be
 *     modified.
 */
void iso_node_set_permissions(IsoNode *node, mode_t mode);

/** 
 * Get the permissions for the node 
 */
mode_t iso_node_get_permissions(const IsoNode *node);

/** 
 * Get the mode of the node, both permissions and file type, as specified in
 * 'man 2 stat'.
 */
mode_t iso_node_get_mode(const IsoNode *node);

/**
 * Set the user id for the node. This attribute is only useful when 
 * Rock Ridge extensions are enabled.
 */
void iso_node_set_uid(IsoNode *node, uid_t uid);

/**
 * Get the user id of the node.
 */
uid_t iso_node_get_uid(const IsoNode *node);

/**
 * Set the group id for the node. This attribute is only useful when 
 * Rock Ridge extensions are enabled.
 */
void iso_node_set_gid(IsoNode *node, gid_t gid);

/**
 * Get the group id of the node.
 */
gid_t iso_node_get_gid(const IsoNode *node);

/**
 * Add a new node to a dir. Note that this function don't add a new ref to
 * the node, so you don't need to free it, it will be automatically freed
 * when the dir is deleted. Of course, if you want to keep using the node
 * after the dir life, you need to iso_node_ref() it.
 * 
 * @param dir 
 *     the dir where to add the node
 * @param child 
 *     the node to add. You must ensure that the node hasn't previously added
 *     to other dir, and that the node name is unique inside the child.
 *     Otherwise this function will return a failure, and the child won't be
 *     inserted.
 * @return
 *     number of nodes in dir if succes, < 0 otherwise
 *     Possible errors:
 *         ISO_NULL_POINTER, if dir or child are NULL
 *         ISO_NODE_ALREADY_ADDED, if child is already added to other dir 
 *         ISO_NODE_NAME_NOT_UNIQUE, a node with same name already exists
 *         ISO_WRONG_ARG_VALUE, if child == dir
 */
int iso_dir_add_node(IsoDir *dir, IsoNode *child);

/**
 * Locate a node inside a given dir.
 * 
 * @param name
 *     The name of the node
 * @param node
 *     Location for a pointer to the node, it will filled with NULL if the dir 
 *     doesn't have a child with the given name.
 *     The node will be owned by the dir and shouldn't be unref(). Just call
 *     iso_node_ref() to get your own reference to the node.
 * @return 
 *     1 node found, 0 child has no such node, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if dir, node or name are NULL
 */
int iso_dir_get_node(IsoDir *dir, const char *name, IsoNode **node);

/**
 * Get the number of children of a directory.
 * 
 * @return
 *     >= 0 number of items, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if dir is NULL
 */
int iso_dir_get_nchildren(IsoDir *dir);

/**
 * Removes a child from a directory.
 * The child is not freed, so you will become the owner of the node. Later
 * you can add the node to another dir (calling iso_dir_add_node), or free
 * it if you don't need it (with iso_node_unref).
 * 
 * @return 
 *     1 on success, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if node is NULL
 *         ISO_NODE_NOT_ADDED_TO_DIR, if node doesn't belong to a dir
 */
int iso_node_take(IsoNode *node);

/**
 * Removes a child from a directory and free (unref) it.
 * If you want to keep the child alive, you need to iso_node_ref() it
 * before this call, but in that case iso_node_take() is a better
 * alternative.
 * 
 * @return 
 *     1 on success, < 0 error
 */
int iso_node_remove(IsoNode *node);

/**
 * Get an iterator for the children of the given dir.
 * 
 * You can iterate over the children with iso_dir_iter_next. When finished,
 * you should free the iterator with iso_dir_iter_free.
 * You musn't delete a child of the same dir, using iso_node_take() or
 * iso_node_remove(), while you're using the iterator. You can use 
 * iso_node_take_iter() or iso_node_remove_iter() instead.
 * 
 * You can use the iterator in the way like this
 * 
 * IsoDirIter *iter;
 * IsoNode *node;
 * if ( iso_dir_get_children(dir, &iter) != 1 ) {
 *     // handle error
 * }
 * while ( iso_dir_iter_next(iter, &node) == 1 ) {
 *     // do something with the child
 * }
 * iso_dir_iter_free(iter);
 * 
 * An iterator is intended to be used in a single iteration over the
 * children of a dir. Thus, it should be treated as a temporary object,
 * and free as soon as possible. 
 *  
 * @return
 *     1 success, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if dir or iter are NULL
 *         ISO_OUT_OF_MEM
 */
int iso_dir_get_children(const IsoDir *dir, IsoDirIter **iter);

/**
 * Get the next child.
 * Take care that the node is owned by its parent, and will be unref() when
 * the parent is freed. If you want your own ref to it, call iso_node_ref()
 * on it.
 * 
 * @return
 *     1 success, 0 if dir has no more elements, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if node or iter are NULL
 *         ISO_ERROR, on wrong iter usage, usual caused by modiying the
 *         dir during iteration
 */
int iso_dir_iter_next(IsoDirIter *iter, IsoNode **node);

/**
 * Check if there're more children.
 * 
 * @return
 *     1 dir has more elements, 0 no, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if iter is NULL
 */
int iso_dir_iter_has_next(IsoDirIter *iter);

/** 
 * Free a dir iterator.
 */
void iso_dir_iter_free(IsoDirIter *iter);

/**
 * Removes a child from a directory during an iteration, without freeing it.
 * It's like iso_node_take(), but to be used during a directory iteration.
 * The node removed will be the last returned by the iteration.
 * 
 * The behavior on two call to this function without calling iso_dir_iter_next
 * between then is undefined, and should never occur. (TODO protect against this?)
 * 
 * @return
 *     1 on succes, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if iter is NULL
 *         ISO_ERROR, on wrong iter usage, for example by call this before
 *         iso_dir_iter_next.
 */
int iso_dir_iter_take(IsoDirIter *iter);

/**
 * Removes a child from a directory during an iteration and unref() it.
 * It's like iso_node_remove(), but to be used during a directory iteration.
 * The node removed will be the last returned by the iteration.
 * 
 * The behavior on two call to this function without calling iso_tree_iter_next
 * between then is undefined, and should never occur. (TODO protect against this?)
 * 
 * @return
 *     1 on succes, < 0 error
 *     Possible errors:
 *         ISO_NULL_POINTER, if iter is NULL
 *         ISO_ERROR, on wrong iter usage, for example by call this before
 *         iso_dir_iter_next.
 */
int iso_dir_iter_remove(IsoDirIter *iter);

#define ISO_MSGS_MESSAGE_LEN 4096

/** 
 * Control queueing and stderr printing of messages from a given IsoImage.
 * Severity may be one of "NEVER", "FATAL", "SORRY", "WARNING", "HINT",
 * "NOTE", "UPDATE", "DEBUG", "ALL".
 * 
 * @param image          The image
 * @param queue_severity Gives the minimum limit for messages to be queued.
 *                       Default: "NEVER". If you queue messages then you
 *                       must consume them by iso_msgs_obtain().
 * @param print_severity Does the same for messages to be printed directly
 *                       to stderr.
 * @param print_id       A text prefix to be printed before the message.
 * @return               >0 for success, <=0 for error
 */
int iso_image_set_msgs_severities(IsoImage *img, char *queue_severity,
                                 char *print_severity, char *print_id);
/** 
 * Obtain the oldest pending message from a IsoImage message queue which has at
 * least the given minimum_severity. This message and any older message of
 * lower severity will get discarded from the queue and is then lost forever.
 * 
 * Severity may be one of "NEVER", "FATAL", "SORRY", "WARNING", "HINT",
 * "NOTE", "UPDATE", "DEBUG", "ALL". To call with minimum_severity "NEVER"
 * will discard the whole queue.
 * 
 * @param image      The image whose messages we want to obtain
 * @param error_code Will become a unique error code as listed in messages.h
 * @param msg_text   Must provide at least ISO_MSGS_MESSAGE_LEN bytes.
 * @param os_errno   Will become the eventual errno related to the message
 * @param severity   Will become the severity related to the message and
 *                   should provide at least 80 bytes.
 * @return 1 if a matching item was found, 0 if not, <0 for severe errors
 */
int iso_image_obtain_msgs(IsoImage *image, char *minimum_severity,
                          int *error_code, char msg_text[], int *os_errno,
                          char severity[]);

/**
 * Return the messenger object handle used by the given image. This handle
 * may be used by related libraries to replace their own compatible
 * messenger objects and thus to direct their messages to the libisofs
 * message queue. See also: libburn, API function burn_set_messenger().
 * 
 * @return the handle. Do only use with compatible
 */
void *iso_image_get_messenger(IsoImage *image);

#endif /*LIBISO_LIBISOFS_H_*/
