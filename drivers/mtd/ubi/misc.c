/*
 * Copyright (c) International Business Machines Corp., 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Author: Artem Bityutskiy (Битюцкий Артём)
 */

/* Here we keep miscellaneous functions which are used all over the UBI code */

#include "ubi.h"

#ifdef CONFIG_MTD_UBI_BEB_RESERVE_ONFI
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#endif

/**
 * calc_data_len - calculate how much real data is stored in a buffer.
 * @ubi: UBI device description object
 * @buf: a buffer with the contents of the physical eraseblock
 * @length: the buffer length
 *
 * This function calculates how much "real data" is stored in @buf and returnes
 * the length. Continuous 0xFF bytes at the end of the buffer are not
 * considered as "real data".
 */
int ubi_calc_data_len(const struct ubi_device *ubi, const void *buf,
		      int length)
{
	int i;

	ubi_assert(!(length & (ubi->min_io_size - 1)));

	for (i = length - 1; i >= 0; i--)
		if (((const uint8_t *)buf)[i] != 0xFF)
			break;

	/* The resulting length must be aligned to the minimum flash I/O size */
	length = ALIGN(i + 1, ubi->min_io_size);
	return length;
}

/**
 * ubi_check_volume - check the contents of a static volume.
 * @ubi: UBI device description object
 * @vol_id: ID of the volume to check
 *
 * This function checks if static volume @vol_id is corrupted by fully reading
 * it and checking data CRC. This function returns %0 if the volume is not
 * corrupted, %1 if it is corrupted and a negative error code in case of
 * failure. Dynamic volumes are not checked and zero is returned immediately.
 */
int ubi_check_volume(struct ubi_device *ubi, int vol_id)
{
	void *buf;
	int err = 0, i;
	struct ubi_volume *vol = ubi->volumes[vol_id];

	if (vol->vol_type != UBI_STATIC_VOLUME)
		return 0;

	buf = vmalloc(vol->usable_leb_size);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < vol->used_ebs; i++) {
		int size;

		if (i == vol->used_ebs - 1)
			size = vol->last_eb_bytes;
		else
			size = vol->usable_leb_size;

		err = ubi_eba_read_leb(ubi, vol, i, buf, 0, size, 1);
		if (err) {
			if (mtd_is_eccerr(err))
				err = 1;
			break;
		}
	}

	vfree(buf);
	return err;
}

/**
 * ubi_calculate_rsvd_pool - calculate how many PEBs must be reserved for bad
 * eraseblock handling.
 * @ubi: UBI device description object
 */
void ubi_calculate_reserved(struct ubi_device *ubi)
{
#ifdef CONFIG_MTD_UBI_BEB_RESERVE_ONFI

	struct mtd_info *master;
	struct nand_chip *nand;
	uint32_t part_start_block;
	uint32_t part_end_block;
	uint32_t part_start_lun;
	uint32_t part_end_lun;

	/* If the MTD is not NAND flash. */
	if (ubi->mtd->type != MTD_NANDFLASH)
		goto ubi_calculate_reserved_fallback;

	/* If the erase size is not a power of two. Note that this is only
	   necessary to avoid 64-bit division, which isn't supported on ARM. */
	if (!ubi->mtd->erasesize_shift)
		goto ubi_calculate_reserved_fallback;

	master = mtd_partition_master(ubi->mtd);

	/* If the MTD is not an MTD partition, not sure if this is possible. */
	if (!master)
		goto ubi_calculate_reserved_fallback;

	nand = master->priv;

	/* If the MTD doesn't have an underlying NAND device, or if it doesn't
	   support ONFI. */
	if (!nand || !nand->onfi_version)
		goto ubi_calculate_reserved_fallback;

	/* Get the start and end of the partition in erase blocks. */
	part_start_block = mtd_partition_offset(ubi->mtd)
			   >> ubi->mtd->erasesize_shift;
	part_end_block = (ubi->mtd->size >> ubi->mtd->erasesize_shift)
			 + part_start_block - 1;

	/* Get the start and end LUNs of the partition. */
	part_start_lun = part_start_block / nand->onfi_params.blocks_per_lun;
	part_end_lun = part_end_block / nand->onfi_params.blocks_per_lun;

	/* Look up the bad blocks per unit and multiply by the number of units
	   that the partition spans. */
	ubi->beb_rsvd_level = nand->onfi_params.bb_per_lun *
			      (part_end_lun - part_start_lun + 1);
	return;

ubi_calculate_reserved_fallback:
	/* Not NAND or no ONFI, fall through. */
#endif
	ubi->beb_rsvd_level = ubi->good_peb_count/100;
	ubi->beb_rsvd_level *= CONFIG_MTD_UBI_BEB_RESERVE;
	if (ubi->beb_rsvd_level < MIN_RESEVED_PEBS)
		ubi->beb_rsvd_level = MIN_RESEVED_PEBS;
}

/**
 * ubi_check_pattern - check if buffer contains only a certain byte pattern.
 * @buf: buffer to check
 * @patt: the pattern to check
 * @size: buffer size in bytes
 *
 * This function returns %1 in there are only @patt bytes in @buf, and %0 if
 * something else was also found.
 */
int ubi_check_pattern(const void *buf, uint8_t patt, int size)
{
	int i;

	for (i = 0; i < size; i++)
		if (((const uint8_t *)buf)[i] != patt)
			return 0;
	return 1;
}
