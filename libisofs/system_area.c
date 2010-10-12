/*
 * Copyright (c) 2008 Vreixo Formoso
 * Copyright (c) 2010 Thomas Schmitt
 *
 * This file is part of the libisofs project; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 
 * or later as published by the Free Software Foundation. 
 * See COPYING file for details.
 */

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "libisofs.h"
#include "system_area.h"
#include "eltorito.h"
#include "filesrc.h"

#include <string.h>
#include <stdio.h>


/*
 * Create a MBR for an isohybrid enabled ISOLINUX boot image.
 * See libisofs/make_isohybrid_mbr.c
 * Deprecated.
 */
int make_isohybrid_mbr(int bin_lba, int *img_blocks, char *mbr, int flag);

/*
 * The New ISOLINUX MBR Producer.
 * Be cautious with changing parameters. Only few combinations are tested.
 *
 */
int make_isolinux_mbr(uint32_t *img_blocks, uint32_t boot_lba,
                      uint32_t mbr_id, int head_count, int sector_count,
                      int part_offset, int part_number, int fs_type,
                      uint8_t *buf, int flag);


/*
 * @param flag bit0= img_blocks is start address rather than end address:
                     do not subtract 1
 */
static
void iso_compute_cyl_head_sec(uint32_t *img_blocks, int hpc, int sph,
                              uint32_t *end_lba, uint32_t *end_sec,
                              uint32_t *end_head, uint32_t *end_cyl, int flag)
{
    uint32_t secs;

    /* Partition table unit is 512 bytes per sector, ECMA-119 unit is 2048 */
    if (*img_blocks >= 0x40000000)
      *img_blocks = 0x40000000 - 1;        /* truncate rather than roll over */
    if (flag & 1)
        secs = *end_lba = *img_blocks * 4;            /* first valid 512-lba */
    else
        secs = *end_lba = *img_blocks * 4 - 1;         /* last valid 512-lba */
    *end_cyl = secs / (sph * hpc);
    secs -= *end_cyl * sph * hpc;
    *end_head = secs / sph;
    *end_sec = secs - *end_head * sph + 1;   /* Sector count starts by 1 */
    if (*end_cyl >= 1024) {
        *end_cyl = 1023;
        *end_head = hpc - 1;
        *end_sec = sph;
    }
}


/* This is the gesture of grub-mkisofs --protective-msdos-label as explained by
   Vladimir Serbinenko <phcoder@gmail.com>, 2 April 2010, on grub-devel@gnu.org
   "Currently we use first and not last entry. You need to:
    1) Zero-fill 446-510
    2) Put 0x55, 0xAA into 510-512
    3) Put 0x80 (for bootable partition), 0, 2, 0 (C/H/S of the start), 0xcd
      (partition type), [3 bytes of C/H/S end], 0x01, 0x00, 0x00, 0x00 (LBA
      start in little endian), [LBA end in little endian] at 446-462
   "

   "C/H/S end" means the CHS address of the last block in the partition.
   It seems that not "[LBA end in little endian]" but "number of blocks"
   should go into bytes 458-461. But with a start lba of 1, this is the
   same number.
   See also http://en.wikipedia.org/wiki/Master_boot_record

   flag   bit0= do not write 0x55, 0xAA to 510,511
          bit1= do not mark partition as bootable
*/
static
int make_grub_msdos_label(uint32_t img_blocks, uint8_t *buf, int flag)
{
    uint8_t *wpt;
    uint32_t end_lba, end_sec, end_head, end_cyl;
    int sph = 63, hpc = 255, i;

    iso_compute_cyl_head_sec(&img_blocks, hpc, sph,
                             &end_lba, &end_sec, &end_head, &end_cyl, 0);

    /* 1) Zero-fill 446-510 */
    wpt = buf + 446;
    memset(wpt, 0, 64);

    if (!(flag & 1)) {
        /* 2) Put 0x55, 0xAA into 510-512 (actually 510-511) */
        buf[510] = 0x55;
        buf[511] = 0xAA;
    }
    if (!(flag & 2)) {
      /* 3) Put 0x80 (for bootable partition), */
      *(wpt++) = 0x80;
    } else {
      *(wpt++) = 0;
    }

    /* 0, 2, 0 (C/H/S of the start), */
    *(wpt++) = 0;
    *(wpt++) = 2;
    *(wpt++) = 0;

    /* 0xcd (partition type) */
    *(wpt++) = 0xcd;

    /* [3 bytes of C/H/S end], */
    *(wpt++) = end_head;
    *(wpt++) = end_sec | ((end_cyl & 0x300) >> 2);
    *(wpt++) = end_cyl & 0xff;
    

    /* 0x01, 0x00, 0x00, 0x00 (LBA start in little endian), */
    *(wpt++) = 0x01;
    *(wpt++) = 0x00;
    *(wpt++) = 0x00;
    *(wpt++) = 0x00;

    /* [LBA end in little endian] */
    for (i = 0; i < 4; i++)
       *(wpt++) = (end_lba >> (8 * i)) & 0xff;

    /* at 446-462 */
    if (wpt - buf != 462) {
        fprintf(stderr,
        "libisofs: program error in make_grub_msdos_label: \"assert 462\"\n");
        return ISO_ASSERT_FAILURE;
    }
    return ISO_SUCCESS;
}


/* @param flag bit0= zeroize partitions entries 2, 3, 4
*/
static
int iso_offset_partition_start(uint32_t img_blocks, uint32_t partition_offset,
                               int sph_in, int hpc_in, uint8_t *buf, int flag)
{
    uint8_t *wpt;
    uint32_t end_lba, end_sec, end_head, end_cyl;
    uint32_t start_lba, start_sec, start_head, start_cyl;
    int sph = 63, hpc = 255, i;

    if (sph_in > 0)
      sph = sph_in;
    if (hpc_in > 0)
      hpc = hpc_in;
    iso_compute_cyl_head_sec(&partition_offset, hpc, sph,
                           &start_lba, &start_sec, &start_head, &start_cyl, 1);
    iso_compute_cyl_head_sec(&img_blocks, hpc, sph,
                             &end_lba, &end_sec, &end_head, &end_cyl, 0);
    wpt = buf + 446;

    /* Let pass only legal bootability values */
    if (*wpt != 0 && *wpt != 0x80)
        (*wpt) = 0;
    wpt++;

    /* C/H/S of the start */
    *(wpt++) = start_head;
    *(wpt++) = start_sec | ((start_cyl & 0x300) >> 2);
    *(wpt++) = end_cyl & 0xff;

    /* (partition type) */
    wpt++;

    /* 3 bytes of C/H/S end */
    *(wpt++) = end_head;
    *(wpt++) = end_sec | ((end_cyl & 0x300) >> 2);
    *(wpt++) = end_cyl & 0xff;
    
    /* LBA start in little endian */
    for (i = 0; i < 4; i++)
       *(wpt++) = (start_lba >> (8 * i)) & 0xff;

    /* Number of sectors in partition, little endian */
    end_lba = end_lba - start_lba + 1;
    for (i = 0; i < 4; i++)
       *(wpt++) = (end_lba >> (8 * i)) & 0xff;

    if (wpt - buf != 462) {
        fprintf(stderr,
    "libisofs: program error in iso_offset_partition_start: \"assert 462\"\n");
        return ISO_ASSERT_FAILURE;
    }

    if (flag & 1) /* zeroize the other partition entries */
        memset(wpt, 0, 3 * 16);

    return ISO_SUCCESS;
}


/* This function was implemented according to a byte map which was derived
   by Thomas Schmitt from
   cdrkit-1.1.10/genisoimage/boot-mips.c by Steve McIntyre which is based
   on work of Florian Lohoff and Thiemo Seufer who possibly learned from
   documents of MIPS Computer Systems, Inc. and Silicon Graphics Computer
   Systems, Inc.
   This function itself is entirely under copyright (C) 2010 Thomas Schmitt.
*/
static int make_mips_volume_header(Ecma119Image *t, uint8_t *buf, int flag)
{
    char *namept, *name_field;
    uint32_t num_cyl, idx, blocks, num, checksum;
    off_t image_size;
    static uint32_t bps = 512, spt = 32;

    memset(buf, 0, 16 * BLOCK_SIZE);

    image_size = t->curblock * 2048;

    /* 0 -   3 | 0x0be5a941 | Magic number */
    iso_msb(buf, 0x0be5a941, 4);

    /* 28 -  29 |  num_cyl_l | Number of usable cylinder, lower two bytes */
    /* >>> Shall i rather orund up ? */
    num_cyl = image_size / (bps * spt);
    iso_msb(buf + 28, num_cyl & 0xffff, 2);

    /* 32 -  33 |          1 | Number of tracks per cylinder */
    iso_msb(buf + 32, 1, 2);

    /* 35 -  35 |  num_cyl_h | Number of usable cylinders, high byte */
    buf[35] = (num_cyl >> 16) & 0xff;
    
    /* 38 -  39 |         32 | Sectors per track */
    iso_msb(buf + 38, spt, 2);

    /* 40 -  41 |        512 | Bytes per sector */
    iso_msb(buf + 40, bps, 2);

    /* 44 -  47 | 0x00000034 | Controller characteristics */
    iso_msb(buf + 44, 0x00000034, 4);

    /*  72 -  87 | ========== | Volume Directory Entry 1 */
    /*  72 -  79 |  boot_name | Boot file basename */
    /*  80 -  83 | boot_block | ISO 9660 LBA of boot file * 4 */
    /*  84 -  87 | boot_bytes | File length in bytes */
    /*  88 - 311 |          0 | Volume Directory Entries 2 to 15 */
    for (idx = 0; idx < t->catalog->num_bootimages; idx++) {

        /* >>> skip non-MIPS boot images */;

        namept = (char *) iso_node_get_name(
                               (IsoNode *) t->catalog->bootimages[idx]->image);
        name_field = (char *) (buf + (72 + 16 * idx));
        strncpy(name_field, namept, 8);
        iso_msb(buf + (72 + 16 * idx) + 8,
                t->bootsrc[idx]->sections[0].block * 4, 4);

        /* >>> shall i really round up to 2048 ? */
        iso_msb(buf + (72 + 16 * idx) + 12,
                ((t->bootsrc[idx]->sections[0].size + 2047) / 2048 ) * 2048,
                4);
    }

    /* 408 - 411 |  part_blks | Number of 512 byte blocks in partition */
    blocks = (image_size + bps - 1) / bps;
    iso_msb(buf + 408, blocks, 4);
    /* 416 - 419 |          0 | Partition is volume header */
    iso_msb(buf + 416, 0, 4);

    /* 432 - 435 |  part_blks | Number of 512 byte blocks in partition */
    iso_msb(buf + 432, blocks, 4);
    iso_msb(buf + 444, 6, 4);

    /* 504 - 507 |   head_chk | Volume header checksum  
                                The two's complement of bytes 0 to 503 read
                                as big endian unsigned 32 bit:
                                  sum(32-bit-words) + head_chk == 0
    */
    checksum = 0;
    for (idx = 0; idx < 504; idx += 4) {
        num = iso_read_msb(buf + idx, 4);
        /* Addition modulo a natural number is commutative and associative.
           Thus the inverse of a sum is the sum of the inverses of the addends.
        */
        checksum -= num;
    }
    iso_msb(buf + 504, checksum, 4);

    return 1;
}


int iso_write_system_area(Ecma119Image *t, uint8_t *buf)
{
    int ret, int_img_blocks, sa_type;
    uint32_t img_blocks;

    if ((t == NULL) || (buf == NULL)) {
        return ISO_NULL_POINTER;
    }

    /* set buf to 0s */
    memset(buf, 0, 16 * BLOCK_SIZE);

    sa_type = (t->system_area_options >> 2) & 0x3f;
    img_blocks = t->curblock;
    if (t->system_area_data != NULL) {
        /* Write more or less opaque boot image */
        memcpy(buf, t->system_area_data, 16 * BLOCK_SIZE);

    } else if (sa_type == 0 && t->catalog != NULL &&
               (t->catalog->bootimages[0]->isolinux_options & 0x0a) == 0x02) {
        /* Check for isolinux image with magic number of 3.72 and produce
           an MBR from our built-in template. (Deprecated since 31 Mar 2010)
        */
        if (img_blocks < 0x80000000) {
            int_img_blocks= img_blocks;
        } else {
            int_img_blocks= 0x7ffffff0;
        }
        ret = make_isohybrid_mbr(t->bootsrc[0]->sections[0].block,
                                 &int_img_blocks, (char*)buf, 0);
        if (ret != 1) {
            /* error, it should never happen */
            return ISO_ASSERT_FAILURE;
        }
        return ISO_SUCCESS;
    }
    if (sa_type == 0 && (t->system_area_options & 1)) {
        /* Write GRUB protective msdos label, i.e. a simple partition table */
        ret = make_grub_msdos_label(img_blocks, buf, 0);
        if (ret != ISO_SUCCESS) /* error should never happen */
            return ISO_ASSERT_FAILURE;
    } else if(sa_type == 0 && (t->system_area_options & 2)) {
        /* Patch externally provided system area as isohybrid MBR */
        if (t->catalog == NULL || t->system_area_data == NULL) {
            /* isohybrid makes only sense together with ISOLINUX boot image
               and externally provided System Area.
            */
            return ISO_ISOLINUX_CANT_PATCH;
        }
        ret = make_isolinux_mbr(&img_blocks, t->bootsrc[0]->sections[0].block,
                                (uint32_t) 0, 64, 32, 0, 1, 0x17, buf, 1);
        if (ret != 1)
            return ret;
    } else if(sa_type == 1) {
        ret = make_mips_volume_header(t, buf, 0);
        if (ret != ISO_SUCCESS) /* error should never happen */
            return ISO_ASSERT_FAILURE;
    } else if(t->partition_offset > 0) {
        /* Write a simple partition table. */
        ret = make_grub_msdos_label(img_blocks, buf, 2);
        if (ret != ISO_SUCCESS) /* error should never happen */
            return ISO_ASSERT_FAILURE;
    }

    if (t->partition_offset > 0) {
        /* Adjust partition table to partition offset */
        img_blocks = t->curblock;                  /* value might be altered */
        ret = iso_offset_partition_start(img_blocks, t->partition_offset,
                                         t->partition_secs_per_head,
                                         t->partition_heads_per_cyl, buf, 1);
        if (ret != ISO_SUCCESS) /* error should never happen */
            return ISO_ASSERT_FAILURE;
    }

    return ISO_SUCCESS;
}
