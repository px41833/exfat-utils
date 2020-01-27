// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@gmail.com>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <iconv.h>

#include "exfat_ondisk.h"
#include "exfat_tools.h"
#include "mkfs.h"

#if defined(__LITTLE_ENDIAN)
#define BITOP_LE_SWIZZLE        0
#elif defined(__BIG_ENDIAN)
#define BITOP_LE_SWIZZLE	(~0x7)
#endif

#define BIT_MASK(nr)            ((1) << ((nr) % 32))
#define BIT_WORD(nr)            ((nr) / 32)

static inline void set_bit(int nr, unsigned int *addr)
{
	unsigned long mask = BIT_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);

	*p  |= mask;
}

static inline void clear_bit(int nr, unsigned int *addr)
{
	unsigned long mask = BIT_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);

	*p &= ~mask;
}

static inline void set_bit_le(int nr, void *addr)
{
	set_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

static inline void clear_bit_le(int nr, void *addr)
{
	clear_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

void exfat_set_bit(struct exfat_blk_dev *bd, char *bitmap,
		unsigned int clu)
{
	int i, b;

	i = clu >> (bd->sector_size_bits + 3);
	b = clu & ((bd->sector_size << 3) - 1);

	set_bit_le(b, bitmap);
}

void exfat_clear_bit(struct exfat_blk_dev *bd, char *bitmap,
		unsigned int clu)
{
	int i, b;

	i = clu >> (bd->sector_size_bits + 3);
	b = clu & ((bd->sector_size << 3) - 1);

	clear_bit_le(b, bitmap);
}

wchar_t exfat_bad_char(wchar_t w)
{
	return (w < 0x0020)
		|| (w == '*') || (w == '?') || (w == '<') || (w == '>')
		|| (w == '|') || (w == '"') || (w == ':') || (w == '/')
		|| (w == '\\');
}

void boot_calc_checksum(unsigned char *sector, unsigned short size,
		bool is_boot_sec, __le32 *checksum)
{
	unsigned int index;

	if (is_boot_sec) {
		for (index = 0; index < size; index++) {
			if ((index == 106) || (index == 107) || (index == 112))
				continue;
			*checksum = ((*checksum & 1) ? 0x80000000 : 0) +
				(*checksum >> 1) + sector[index];
		}
	} else {
		for (index = 0; index < size; index++) {
			*checksum = ((*checksum & 1) ? 0x80000000 : 0) +
				(*checksum >> 1) + sector[index];
		}
	}
}

void show_version(void)
{
	printf("exfat-tools version : %s\n", EXFAT_TOOLS_VERSION);
	exit(EXIT_FAILURE);
}

static inline unsigned int sector_size_bits(unsigned int size)
{
	unsigned int bits = 8;

	do {
		bits++;
		size >>= 1;
	} while (size > 256);

	return bits;
}

static void exfat_set_default_cluster_size(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	if (256 * MB >= bd->size)
		ui->cluster_size = 4 * KB;
	else if (32 * GB >= bd->size)
		ui->cluster_size = 32 * KB;
	else
		ui->cluster_size = 128 * KB;
}

int exfat_get_blk_dev_info(struct exfat_user_input *ui,
		struct exfat_blk_dev *bd)
{
	int fd, ret = -1;
	long long blk_dev_size;

	fd = open(ui->dev_name, ui->writeable ? O_RDWR : O_RDONLY);
	if (fd < 0)
		return -1;

	blk_dev_size = lseek(fd, 0, SEEK_END);
	if (blk_dev_size <= 0) {
		exfat_msg(EXFAT_ERROR,
			"invalid block device size(%s) : %lld\n",
			ui->dev_name, blk_dev_size);
		ret = blk_dev_size;
		close(fd);
		goto out;
	}

	bd->dev_fd = fd;
	bd->size = blk_dev_size;
	if (!ui->cluster_size)
		exfat_set_default_cluster_size(bd, ui);

	if (ioctl(fd, BLKSSZGET, &bd->sector_size) < 0)
		bd->sector_size = DEFAULT_SECTOR_SIZE;
	bd->sector_size_bits = sector_size_bits(bd->sector_size);
	bd->num_sectors = blk_dev_size / DEFAULT_SECTOR_SIZE;
	bd->num_clusters = blk_dev_size / ui->cluster_size;

	exfat_msg(EXFAT_DEBUG, "Block device name : %s\n", ui->dev_name);
	exfat_msg(EXFAT_DEBUG, "Block device size : %lld\n", bd->size);
	exfat_msg(EXFAT_DEBUG, "Block sector size : %u\n", bd->sector_size);
	exfat_msg(EXFAT_DEBUG, "Number of the sectors : %llu\n",
		bd->num_sectors);
	exfat_msg(EXFAT_DEBUG, "Number of the clusters : %u\n",
		bd->num_clusters);

	ret = 0;
	bd->dev_fd = fd;
out:
	return ret;
}

ssize_t exfat_read(int fd, void *buf, size_t size, off_t offset)
{
	return pread(fd, buf, size, offset);
}

ssize_t exfat_write(int fd, void *buf, size_t size, off_t offset)
{
	return pwrite(fd, buf, size, offset);
}

int exfat_convert_char_to_utf16s(char *src, size_t src_len, char *dest,
		size_t dest_len)
{
	iconv_t it;
	int ret;

	it = iconv_open("UTF-16", "UTF-8");
	if (it == (iconv_t) -1) {
		exfat_msg(EXFAT_ERROR, "iconv_open failed\n");
		return -1;
	}

	ret = iconv(it, &src, &src_len, &dest, &dest_len);
	if (ret < 0) {
		exfat_msg(EXFAT_ERROR, "iconv failed : %d, errno : %d\n",
				ret, errno);
		if (errno == 7)
			exfat_msg(EXFAT_ERROR,
				  "Volume label string is too long\n");
		return -1;
	}

	iconv_close(it);

	return dest_len;
}

int exfat_convert_utf16s_to_char(char *src, size_t src_len, char *dest,
		size_t dest_len)
{
	iconv_t it;
	int ret;

	it = iconv_open("UTF-8", "UTF-16");
	if (it == (iconv_t) -1) {
		exfat_msg(EXFAT_ERROR, "iconv_open failed\n");
		return -1;
	}

	ret = iconv(it, &src, &src_len, &dest, &dest_len);
	if (ret < 0) {
		exfat_msg(EXFAT_ERROR, "iconv failed : %d, errno : %d\n",
				ret, errno);
		if (errno == 7)
			exfat_msg(EXFAT_ERROR,
				  "Volume label string is too long\n");
		return -1;
	}

	iconv_close(it);

	return dest_len;
}

int exfat_read_sector(struct exfat_blk_dev *bd, void *buf,
		unsigned int sec_off)
{
	int bytes;
	unsigned long long offset = sec_off * bd->sector_size;

	lseek(bd->dev_fd, offset, SEEK_SET);
	bytes = read(bd->dev_fd, buf, bd->sector_size);
	if (bytes != bd->sector_size) {
		exfat_msg(EXFAT_ERROR,
			"write failed, sec_off : %u, bytes : %d\n", sec_off,
			bytes);
		return -1;
	}
	return 0;
}
