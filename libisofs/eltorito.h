/*
 * Copyright (c) 2007 Vreixo Formoso
 * Copyright (c) 2010 Thomas Schmitt
 *
 * This file is part of the libisofs project; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 
 * or later as published by the Free Software Foundation. 
 * See COPYING file for details.
 */

/**
 * Declare El-Torito related structures.
 * References:
 *  "El Torito" Bootable CD-ROM Format Specification Version 1.0 (1995)
 */

#ifndef LIBISO_ELTORITO_H
#define LIBISO_ELTORITO_H

#include "ecma119.h"
#include "node.h"

/**
 * A node that acts as a placeholder for an El-Torito catalog.
 */
struct Iso_Boot
{
    IsoNode node;
};

/* Not more than 32 so that all entries fit into 2048 bytes */
#define Libisofs_max_boot_imageS 32

struct el_torito_boot_catalog {
    IsoBoot *node; /* node of the catalog */

    /* ts B00419 */
    int num_bootimages;
    struct el_torito_boot_image *bootimages[Libisofs_max_boot_imageS];
                                                  /* [0]= default boot image */

    /* ts B00419 */
    /* Weight value for image sorting */
    int sort_weight;
};

struct el_torito_boot_image {
    IsoFile *image;

    unsigned int bootable:1; /**< If the entry is bootable. */
    /**
     * isolinux options
     * bit 0 -> whether to patch image
     * bit 1 -> whether to put built-in isolinux 3.72 isohybrid-MBR into image
     *          System Area (deprecated)
     */
    unsigned int isolinux_options:2;
    unsigned char type; /**< The type of image */
    unsigned char partition_type; /**< type of partition for HD-emul images */
    short load_seg; /**< Load segment for the initial boot image. */
    short load_size; /**< Number of sectors to load. */

    /* ts B00419 */
    /* Byte 1 of Validation Entry or Section Header Entry:
       0= 80x86, 1= PowerPC, 2= Mac, 0xef= EFI */
    uint8_t platform_id;
    uint8_t id_string[28];
    uint8_t selection_crit[20];
};

/** El-Torito, 2.1 */
struct el_torito_validation_entry {
    uint8_t header_id           BP(1, 1);
    uint8_t platform_id         BP(2, 2);
    uint8_t reserved            BP(3, 4);
    uint8_t id_string           BP(5, 28);
    uint8_t checksum            BP(29, 30);
    uint8_t key_byte1           BP(31, 31);
    uint8_t key_byte2           BP(32, 32);
};

/** El-Torito, 2.2 */
struct el_torito_default_entry {
    uint8_t boot_indicator      BP(1, 1);
    uint8_t boot_media_type     BP(2, 2);
    uint8_t load_seg            BP(3, 4);
    uint8_t system_type         BP(5, 5);
    uint8_t unused1             BP(6, 6);
    uint8_t sec_count           BP(7, 8);
    uint8_t block               BP(9, 12);
    uint8_t unused2             BP(13, 32);
};

/** El-Torito, 2.3 */
struct el_torito_section_header {
    uint8_t header_indicator    BP(1, 1);
    uint8_t platform_id         BP(2, 2);
    uint8_t num_entries         BP(3, 4);
    uint8_t id_string           BP(5, 32);
};

/** El-Torito, 2.4 */
struct el_torito_section_entry {
    uint8_t boot_indicator      BP(1, 1);
    uint8_t boot_media_type     BP(2, 2);
    uint8_t load_seg            BP(3, 4);
    uint8_t system_type         BP(5, 5);
    uint8_t unused1             BP(6, 6);
    uint8_t sec_count           BP(7, 8);
    uint8_t block               BP(9, 12);
    uint8_t selec_criteria      BP(13, 13);
    uint8_t vendor_sc           BP(14, 32);
};

void el_torito_boot_catalog_free(struct el_torito_boot_catalog *cat);

/**
 * Create a IsoFileSrc for writing the el-torito catalog for the given
 * target, and add it to target. If the target already has a src for the
 * catalog, it just returns.
 */
int el_torito_catalog_file_src_create(Ecma119Image *target, IsoFileSrc **src);

/**
 * Create a writer for el-torito information.
 */
int eltorito_writer_create(Ecma119Image *target);

#endif /* LIBISO_ELTORITO_H */
