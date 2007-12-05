
/* libiso_msgs
   Message handling facility of libisofs.
   Copyright (C) 2006-2007 Thomas Schmitt <scdbackup@gmx.net>,
   provided under GPL
*/


/*
  *Never* set this macro outside libiso_msgs.c !
  The entrails of the message handling facility are not to be seen by
  the other library components or the applications.
*/
#ifdef LIBISO_MSGS_H_INTERNAL


#ifndef LIBISO_MSGS_SINGLE_THREADED
#include <pthread.h>
#endif


struct libiso_msgs_item {

 double timestamp;
 pid_t process_id;
 int driveno;

 int severity;
 int priority;

 /* Apply for your developer's error code range at
      libburn-hackers@pykix.org
    Report introduced codes in the list below. */
 int error_code;

 char *msg_text;
 int os_errno;
  
 struct libiso_msgs_item *prev,*next;

};


struct libiso_msgs {

 struct libiso_msgs_item *oldest;
 struct libiso_msgs_item *youngest;
 int count;

 int queue_severity;
 int print_severity;
 char print_id[81];
 
#ifndef LIBISO_MSGS_SINGLE_THREADED
 pthread_mutex_t lock_mutex;
#endif


};

#endif /* LIBISO_MSGS_H_INTERNAL */


#ifndef LIBISO_MSGS_H_INCLUDED
#define LIBISO_MSGS_H_INCLUDED 1


#ifndef LIBISO_MSGS_H_INTERNAL


                          /* Public Opaque Handles */

/** A pointer to this is a opaque handle to a message handling facility */
struct libiso_msgs;

/** A pointer to this is a opaque handle to a single message item */
struct libiso_msgs_item;

#endif /* ! LIBISO_MSGS_H_INTERNAL */


                            /* Public Macros */


/* Registered Severities */

/* It is well advisable to let applications select severities via strings and
   forwarded functions libiso_msgs__text_to_sev(), libiso_msgs__sev_to_text().
   These macros are for use by libdax/libburn only.
*/

/** Use this to get messages of any severity. Do not use for submitting.
*/
#define LIBISO_MSGS_SEV_ALL                                          0x00000000

/** Debugging messages not to be visible to normal users by default
*/
#define LIBISO_MSGS_SEV_DEBUG                                        0x10000000

/** Update of a progress report about long running actions
*/
#define LIBISO_MSGS_SEV_UPDATE                                       0x20000000

/** Not so usual events which were gracefully handled
*/
#define LIBISO_MSGS_SEV_NOTE                                         0x30000000

/** Possibilities to achieve a better result
*/
#define LIBISO_MSGS_SEV_HINT                                         0x40000000

/** Warnings about problems which could not be handled optimally
*/
#define LIBISO_MSGS_SEV_WARNING                                      0x50000000

/** Non-fatal error messages indicating that parts of the action failed
    but processing will/should go on
*/
#define LIBISO_MSGS_SEV_SORRY                                        0x60000000

/** An error message which puts the whole operation of libdax in question
*/
#define LIBISO_MSGS_SEV_FATAL                                        0x70000000

/** A message from an abort handler which will finally finish libburn
*/
#define LIBISO_MSGS_SEV_ABORT                                        0x71000000

/** A severity to exclude resp. discard any possible message.
    Do not use this severity for submitting.
*/
#define LIBISO_MSGS_SEV_NEVER                                        0x7fffffff


/* Registered Priorities */

/* Priorities are to be used by libburn/libdax only. */

#define LIBISO_MSGS_PRIO_ZERO                                        0x00000000 
#define LIBISO_MSGS_PRIO_LOW                                         0x10000000 
#define LIBISO_MSGS_PRIO_MEDIUM                                      0x20000000
#define LIBISO_MSGS_PRIO_HIGH                                        0x30000000
#define LIBISO_MSGS_PRIO_TOP                                         0x7ffffffe

/* Do not use this priority for submitting */
#define LIBISO_MSGS_PRIO_NEVER                                       0x7fffffff


                            /* Public Functions */

               /* Calls initiated from inside libdax/libburn */


/** Create new empty message handling facility with queue.
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return >0 success, <=0 failure
*/
int libiso_msgs_new(struct libiso_msgs **m, int flag);


/** Destroy a message handling facility and all its eventual messages.
    The submitted pointer gets set to NULL.
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 for success, 0 for pointer to NULL
*/
int libiso_msgs_destroy(struct libiso_msgs **m, int flag);


/** Submit a message to a message handling facility.
    @param driveno libdax drive number. Use -1 if no number is known.
    @param error_code  Unique error code. Use only registered codes. See below.
                   The same unique error_code may be issued at different
                   occasions but those should be equivalent out of the view
                   of a libdax application. (E.g. "cannot open ATA drive"
                   versus "cannot open SCSI drive" would be equivalent.)
    @param severity The LIBISO_MSGS_SEV_* of the event.
    @param priority The LIBISO_MSGS_PRIO_* number of the event.
    @param msg_text Printable and human readable message text.
    @param os_errno Eventual error code from operating system (0 if none)
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 on success, 0 on rejection, <0 for severe errors
*/
int libiso_msgs_submit(struct libiso_msgs *m, int driveno, int error_code,
                       int severity, int priority, char *msg_text, 
                       int os_errno, int flag);


     /* Calls from applications (to be forwarded by libdax/libburn) */


/** Convert a registered severity number into a severity name
    @param flag Bitfield for control purposes:
      bit0= list all severity names in a newline separated string
    @return >0 success, <=0 failure
*/
int libiso_msgs__sev_to_text(int severity, char **severity_name,
                             int flag);


/** Convert a severity name into a severity number,
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return >0 success, <=0 failure
*/
int libiso_msgs__text_to_sev(char *severity_name, int *severity,
                             int flag);


/** Set minimum severity for messages to be queued (default
    LIBISO_MSGS_SEV_ALL) and for messages to be printed directly to stderr
    (default LIBISO_MSGS_SEV_NEVER).
    @param print_id A text of at most 80 characters to be printed before
                    any eventually printed message (default is "libdax: ").
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return always 1 for now
*/
int libiso_msgs_set_severities(struct libiso_msgs *m, int queue_severity,
                           int print_severity, const char *print_id, int flag);


/** Obtain a message item that has at least the given severity and priority.
    Usually all older messages of lower severity are discarded then. If no
    item of sufficient severity was found, all others are discarded from the
    queue.
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 if a matching item was found, 0 if not, <0 for severe errors
*/
int libiso_msgs_obtain(struct libiso_msgs *m, struct libiso_msgs_item **item,
                       int severity, int priority, int flag);


/** Destroy a message item obtained by libiso_msgs_obtain(). The submitted
    pointer gets set to NULL.
    Caution: Copy eventually obtained msg_text before destroying the item,
             if you want to use it further.
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 for success, 0 for pointer to NULL, <0 for severe errors
*/
int libiso_msgs_destroy_item(struct libiso_msgs *m,
                             struct libiso_msgs_item **item, int flag);


/** Obtain from a message item the three application oriented components as
    submitted with the originating call of libiso_msgs_submit().
    Caution: msg_text becomes a pointer into item, not a copy.
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 on success, 0 on invalid item, <0 for servere errors
*/
int libiso_msgs_item_get_msg(struct libiso_msgs_item *item, 
                             int *error_code, char **msg_text, int *os_errno,
                             int flag);


/** Obtain from a message item the submitter identification submitted
    with the originating call of libiso_msgs_submit().
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 on success, 0 on invalid item, <0 for servere errors
*/
int libiso_msgs_item_get_origin(struct libiso_msgs_item *item, 
                            double *timestamp, pid_t *process_id, int *driveno,
                            int flag);


/** Obtain from a message item severity and priority as submitted
    with the originating call of libiso_msgs_submit().
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 on success, 0 on invalid item, <0 for servere errors
*/
int libiso_msgs_item_get_rank(struct libiso_msgs_item *item, 
                              int *severity, int *priority, int flag);


#ifdef LIDBAX_MSGS_________________


                      /* Registered Error Codes */


Format: error_code  (LIBISO_MSGS_SEV_*,LIBISO_MSGS_PRIO_*) = explanation
If no severity or priority are fixely associates, use "(,)".

------------------------------------------------------------------------------
Range "libiso_msgs"        :  0x00000000 to 0x0000ffff

 0x00000000 (ALL,ZERO)     = Initial setting in new libiso_msgs_item
 0x00000001 (DEBUG,ZERO)   = Test error message
 0x00000002 (DEBUG,ZERO)   = Debugging message


------------------------------------------------------------------------------
Range "elmom"              :  0x00010000 to 0x0001ffff



------------------------------------------------------------------------------
Range "scdbackup"          :  0x00020000 to 0x0002ffff

 Acessing and defending drives:

 0x00020001 (SORRY,LOW)    = Cannot open busy device
 0x00020002 (SORRY,HIGH)   = Encountered error when closing drive
 0x00020003 (SORRY,HIGH)   = Could not grab drive
 0x00020004 (NOTE,HIGH)    = Opened O_EXCL scsi sibling
 0x00020005 (SORRY,HIGH)   = Failed to open device
 0x00020006 (FATAL,HIGH)   = Too many scsi siblings
 0x00020007 (NOTE,HIGH)    = Closed O_EXCL scsi siblings
 0x00020008 (SORRY,HIGH)   = Device busy. Failed to fcntl-lock
           
 General library operations:

 0x00020101 (WARNING,HIGH) = Cannot find given worker item
 0x00020102 (SORRY,HIGH)   = A drive operation is still going on
 0x00020103 (WARNING,HIGH) = After scan a drive operation is still going on
 0x00020104 (SORRY,HIGH)   = NULL pointer caught
 0x00020105 (SORRY,HIGH)   = Drive is already released
 0x00020106 (SORRY,HIGH)   = Drive is busy on attempt to close
 0x00020107 (SORRY,HIGH)   = Drive is busy on attempt to shut down library
 0x00020108 (SORRY,HIGH)   = Drive is not grabbed on disc status inquiry
 0x00020108 (FATAL,HIGH)   = Could not allocate new drive object
 0x00020109 (FATAL,HIGH)   = Library not running
 0x0002010a (FATAL,HIGH)   = Unsuitable track mode
 0x0002010b (FATAL,HIGH)   = Burn run failed
 0x0002010c (FATAL,HIGH)   = Failed to transfer command to drive
 0x0002010d (DEBUG,HIGH)   = Could not inquire TOC
 0x0002010e (FATAL,HIGH)   = Attempt to read ATIP from ungrabbed drive
 0x0002010f (DEBUG,HIGH)   = SCSI error condition on command
 0x00020110 (FATAL,HIGH)   = Persistent drive address too long
 0x00020111 (FATAL,HIGH)   = Could not allocate new auxiliary object
 0x00020112 (SORRY,HIGH)   = Bad combination of write_type and block_type
 0x00020113 (FATAL,HIGH)   = Drive capabilities not inquired yet
 0x00020114 (SORRY,HIGH)   = Attempt to set ISRC with bad data
 0x00020115 (SORRY,HIGH)   = Attempt to set track mode to unusable value
 0x00020116 (FATAL,HIGH)   = Track mode has unusable value
 0x00020117 (FATAL,HIGH)   = toc_entry of drive is already in use
 0x00020118 (DEBUG,HIGH)   = Closing track
 0x00020119 (DEBUG,HIGH)   = Closing session
 0x0002011a (NOTE,HIGH)    = Padding up track to minimum size
 0x0002011b (FATAL,HIGH)   = Attempt to read track info from ungrabbed drive
 0x0002011c (FATAL,HIGH)   = Attempt to read track info from busy drive
 0x0002011d (FATAL,HIGH)   = SCSI error on write
 0x0002011e (SORRY,HIGH)   = Unsuitable media detected
 0x0002011f (SORRY,HIGH)   = Burning is restricted to a single track
 0x00020120 (NOTE,HIGH)    = FORMAT UNIT ignored
 0x00020121 (FATAL,HIGH)   = Write preparation setup failed
 0x00020122 (FATAL,HIGH)   = SCSI error on format_unit
 0x00020123 (SORRY,HIGH)   = DVD Media are unsuitable for desired track type
 0x00020124 (SORRY,HIGH)   = SCSI error on set_streaming
 0x00020125 (SORRY,HIGH)   = Write start address not supported
 0x00020126 (SORRY,HIGH)   = Write start address not properly aligned
 0x00020127 (NOTE,HIGH)    = Write start address is ...
 0x00020128 (FATAL,HIGH)   = Unsupported inquiry_type with mmc_get_performance
 0x00020129 (SORRY,HIGH)   = Will not format media type
 0x0002012a (FATAL,HIGH)   = Cannot inquire write mode capabilities
 0x0002012b (FATAL,HIGH)   = Drive offers no suitable write mode with this job
 0x0002012c (SORRY,HIGH)   = Too many logical tracks recorded
 0x0002012d (FATAL,HIGH)   = Exceeding range of permissible write addresses
 0x0002012e (NOTE,HIGH)    = Activated track default size
 0x0002012f (SORRY,HIGH)   = SAO is restricted to single fixed size session
 0x00020130 (SORRY,HIGH)   = Drive and media state unsuitable for blanking
 0x00020131 (SORRY,HIGH)   = No suitable formatting type offered by drive
 0x00020132 (SORRY,HIGH)   = Selected format is not suitable for libburn
 0x00020133 (SORRY,HIGH)   = Cannot mix data and audio in SAO mode
 0x00020134 (NOTE,HIGH)    = Defaulted TAO to DAO
 0x00020135 (SORRY,HIGH)   = Cannot perform TAO, job unsuitable for DAO
 0x00020136 (SORRY,HIGH)   = DAO burning restricted to single fixed size track
 0x00020137 (HINT,HIGH)    = TAO would be possible
 0x00020138 (FATAL,HIGH)   = Cannot reserve track
 0x00020139 (SORRY,HIGH)   = Write job parameters are unsuitable
 0x0002013a (FATAL,HIGH)   = No suitable media detected
 0x0002013b (DEBUG,HIGH)   = SCSI command indicates host or driver error
 0x0002013c (SORRY,HIGH)   = Malformed capabilities page 2Ah received
 0x0002013d (DEBUG,LOW)    = Waiting for free buffer space takes long time
 0x0002013e (SORRY,HIGH)   = Timeout with waiting for free buffer. Now disabled
 0x0002013f (DEBUG,LOW)    = Reporting total time spent with waiting for buffer


 libiso_audioxtr:
 0x00020200 (SORRY,HIGH)   = Cannot open audio source file
 0x00020201 (SORRY,HIGH)   = Audio source file has unsuitable format
 0x00020202 (SORRY,HIGH)   = Failed to prepare reading of audio data
 

------------------------------------------------------------------------------
Range "vreixo"              :  0x00030000 to 0x0003ffff

 General:
 0x00031001 (SORRY,HIGH)    = Cannot read file (ignored)
 0x00031002 (FATAL,HIGH)    = Cannot read file (operation canceled)
 0x00031003 (FATAL,HIGH)    = File doesnt exist
 0x00031004 (FATAL,HIGH)    = Read access denied

 Image reading:
 0x00031000 (FATAL,HIGH)    = Unsupported ISO-9660 image
 0x00031001 (HINT,MEDIUM)   = Unsupported Vol Desc that will be ignored
 0x00031002 (FATAL,HIGH)    = Damaged ISO-9660 image
 0x00031003 (SORRY,HIGH)    = Cannot read previous image file
 
 Rock-Ridge:
 0x00030101 (HINT,MEDIUM)   = Unsupported SUSP entry that will be ignored
 0x00030102 (SORRY,HIGH)    = Wrong/damaged SUSP entry
 0x00030103 (WARNING,MEDIUM)= Multiple SUSP ER entries where found
 0x00030111 (SORRY,HIGH)    = Unsupported RR feature
 0x00030112 (SORRY,HIGH)    = Error in a Rock Ridge entry
 
 El-Torito:
 0x00030201 (HINT,MEDIUM)   = Unsupported Boot Vol Desc that will be ignored
 0x00030202 (SORRY,HIGH)    = Wrong El-Torito catalog
 0x00030203 (HINT,MEDIUM)   = Unsupported El-Torito feature
 0x00030204 (SORRY,HIGH)    = Invalid file to be an El-Torito image
 0x00030205 (WARNING,MEDIUM)= Cannot properly patch isolinux image
 0x00030206 (WARNING,MEDIUM)= Copying El-Torito from a previous image without
                              enought info about it
 
 Joliet:
 0x00030301 (NOTE,MEDIUM)   = Unsupported file type for Joliet tree
 
 
------------------------------------------------------------------------------

#endif /* LIDBAX_MSGS_________________ */



#ifdef LIBISO_MSGS_H_INTERNAL

                             /* Internal Functions */


/** Lock before doing side effect operations on m */
static int libiso_msgs_lock(struct libiso_msgs *m, int flag);

/** Unlock after effect operations on m are done */
static int libiso_msgs_unlock(struct libiso_msgs *m, int flag);


/** Create new empty message item.
    @param link Previous item in queue
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return >0 success, <=0 failure
*/
static int libiso_msgs_item_new(struct libiso_msgs_item **item, 
                                struct libiso_msgs_item *link, int flag);

/** Destroy a message item obtained by libiso_msgs_obtain(). The submitted
    pointer gets set to NULL.
    @param flag Bitfield for control purposes (unused yet, submit 0)
    @return 1 for success, 0 for pointer to NULL
*/
static int libiso_msgs_item_destroy(struct libiso_msgs_item **item, int flag);


#endif /* LIBISO_MSGS_H_INTERNAL */


#endif /* ! LIBISO_MSGS_H_INCLUDED */