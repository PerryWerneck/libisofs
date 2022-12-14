/*
 * Very simple program to show how to grow an iso image.
 */

#include "libisofs.h"
#include "libburn/libburn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>

static IsoDataSource *libburn_data_source_new(struct burn_drive *d);

void usage(char **argv)
{
    printf("%s DISC DIRECTORY\n", argv[0]);
}

int main(int argc, char **argv)
{
    int result;
    IsoImage *image;
    IsoDataSource *src;
    struct burn_source *burn_src;
    struct burn_drive_info *drives;
    struct burn_drive *drive;
    unsigned char buf[32 * 2048];
    IsoWriteOpts *opts;
    int ret = 0;
    IsoReadImageFeatures *features;
    uint32_t ms_block;
    IsoReadOpts *ropts;
	
    if (argc < 3) {
        usage(argv);
        return 1;
    }

    iso_init();
    iso_set_msgs_severities("NEVER", "ALL", "");
    
    /* create the image context */
    result = iso_image_new("volume_id", &image);
    if (result < 0) {
        printf ("Error creating image\n");
        return 1;
    }
    iso_tree_set_follow_symlinks(image, 0);
    iso_tree_set_ignore_hidden(image, 0);

    if (!burn_initialize()) {
        err(1, "Can't init libburn");
    }
    burn_msgs_set_severities("NEVER", "SORRY", "libburner : ");
    
    if (burn_drive_scan_and_grab(&drives, argv[1], 0) != 1) {
        err(1, "Can't open device. Are you sure it is a valid drive?\n");
    }
    
    drive = drives[0].drive;

#ifdef ISO_GROW_CHECK_MEDIA
    {
        /* some check before going on */
        enum burn_disc_status state;
        int pno;
        char name[80];
        
        state = burn_disc_get_status(drive);
        burn_disc_get_profile(drive, &pno, name);
        
        /*
         * my drives report BURN_DISC_BLANK on a DVD+RW with data.
         * is that correct?
         */
        if ( (pno != 0x1a) /*|| (state != BURN_DISC_FULL)*/ ) {
            printf("You need to insert a DVD+RW with some data.\n");
            printf("Profile: %x, state: %d.\n", pno, state);
            ret = 1;
            goto exit_cleanup;
        }
    }
#endif
    
    /* create the data source to accesss previous image */
    src = libburn_data_source_new(drive);
    if (src == NULL) {
        printf("Can't create data source.\n");
        ret = 1;
        goto exit_cleanup;
    }
    
    /* import previous image */
    ret = iso_read_opts_new(&ropts, 0);
    if (ret < 0) {
        fprintf(stderr, "Error creating read options\n");
        return 1;
    }
    result = iso_image_import(image, src, ropts, &features);
    iso_data_source_unref(src);
    if (result < 0) {
        printf ("Error importing previous session %d\n", result);
        return 1;
    }
    iso_read_opts_free(ropts);

    iso_tree_set_replace_mode(image, ISO_REPLACE_IF_NEWER);

    /* add new dir */
    result = iso_tree_add_dir_rec(image, iso_image_get_root(image), argv[2]);
    if (result < 0) {
        printf ("Error adding directory %d\n", result);
        return 1;
    }

    /* generate a multisession image with new contents */
    result = iso_write_opts_new(&opts, 1);
    if (result < 0) {
        printf("Cant create write opts, error %d\n", result);
        return 1;
    }

    /* round up to 32kb aligment = 16 block */
    ms_block = ((iso_read_image_features_get_size(features) + 15) / 16 ) * 16;
    iso_write_opts_set_ms_block(opts, ms_block);
    iso_write_opts_set_appendable(opts, 1);
    iso_write_opts_set_overwrite_buf(opts, buf);

    iso_read_image_features_destroy(features);

    result = iso_image_create_burn_source(image, opts, &burn_src);
    if (result < 0) {
        printf("Cant create image, error %d\n", result);
        return 1;
    }
    
    iso_write_opts_free(opts);
    
    /* a. write the new image */
    printf("Adding new data...\n");
    {
        struct burn_disc *target_disc;
        struct burn_session *session;
        struct burn_write_opts *burn_options;
        struct burn_track *track;
        struct burn_progress progress;
        char reasons[BURN_REASONS_LEN];
        
        target_disc = burn_disc_create();
        session = burn_session_create();
        burn_disc_add_session(target_disc, session, BURN_POS_END);
        
        track = burn_track_create();
        burn_track_set_source(track, burn_src);
        burn_session_add_track(session, track, BURN_POS_END);
        
        burn_options = burn_write_opts_new(drive);
        burn_drive_set_speed(drive, 0, 0);
        burn_write_opts_set_underrun_proof(burn_options, 1);
        
        /* mmm, check for 32K alignment? */
        burn_write_opts_set_start_byte(burn_options, ms_block * 2048);
        
        if (burn_write_opts_auto_write_type(burn_options, target_disc,
                    reasons, 0) == BURN_WRITE_NONE) {
            printf("Failed to find a suitable write mode:\n%s\n", reasons);
            ret = 1;
            goto exit_cleanup;
        }
        
        /* ok, write the new track */
        burn_disc_write(burn_options, target_disc);
        burn_write_opts_free(burn_options);
        
        while (burn_drive_get_status(drive, NULL) == BURN_DRIVE_SPAWNING)
            usleep(1002);
        
        while (burn_drive_get_status(drive, &progress) != BURN_DRIVE_IDLE) {
            printf("Writing: sector %d of %d\n", progress.sector, progress.sectors);
            sleep(1);
        }
        
    }
    
    /* b. write the new vol desc */
    printf("Writing the new vol desc...\n");
    ret = burn_random_access_write(drive, 0, (char*)buf, 32*2048, 0);
    if (ret != 1) {
        printf("Ups, new vol desc write failed\n");
    }
    
    iso_image_unref(image);
	
exit_cleanup:;
    burn_drive_release(drives[0].drive, 0);
    burn_finish();
    iso_finish();
    
    exit(ret);
}

static int 
libburn_ds_read_block(IsoDataSource *src, uint32_t lba, uint8_t *buffer)
{
    struct burn_drive *d;
    off_t data_count;
    
    d = (struct burn_drive*)src->data;
    
    if ( burn_read_data(d, (off_t) lba * (off_t) 2048, (char*)buffer, 
                        2048, &data_count, 0) < 0 ) {
         return -1; /* error */
    }
        
    return 1;
}

static
int libburn_ds_open(IsoDataSource *src)
{
    /* nothing to do, device is always opened */
    return 1;
}

static
int libburn_ds_close(IsoDataSource *src)
{
    /* nothing to do, device is always opened */
    return 1;
}
    
static void 
libburn_ds_free_data(IsoDataSource *src)
{
    /* nothing to do */
}
 
static IsoDataSource *
libburn_data_source_new(struct burn_drive *d)
{
    IsoDataSource *ret;
    
    ret = malloc(sizeof(IsoDataSource));
    ret->version = 0;
    ret->refcount = 1;
    ret->read_block = libburn_ds_read_block;
    ret->open = libburn_ds_open;
    ret->close = libburn_ds_close;
    ret->free_data = libburn_ds_free_data;
    ret->data = d;
    return ret;
}
