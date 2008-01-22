/*
 * Copyright (c) 2007 Vreixo Formoso
 * 
 * This file is part of the libisofs project; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. See COPYING file for details.
 */

/*
 * Message handling for libisofs
 */

#ifndef MESSAGES_H_
#define MESSAGES_H_

#include "libiso_msgs.h"

/**
 * Take and increment this variable to get a valid identifier for message
 * origin.
 */
extern int iso_message_id;

/**
 * Submit a debug message.
 */
void iso_msg_debug(int imgid, const char *fmt, ...);

/**
 * Get a textual description of an error.
 */
const char *iso_error_to_msg(int errcode);

/**
 * TODO add caused by!!
 * 
 * @return
 *      1 on success, < 0 if function must abort asap.
 */
int iso_msg_submit(int imgid, int errcode, const char *fmt, ...);

#endif /*MESSAGES_H_*/
