// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <iconv.h>

#include "exfat_ondisk.h"
#include "exfat_tools.h"

#define EXFAT_GET_LABEL	1
#define EXFAT_SET_LABEL	2

static void usage(void)
{
	fprintf(stderr, "Usage: label.exfat\n");
	fprintf(stderr, "\t<device name>			Get volume label\n");
	fprintf(stderr, "\t<device name> <new label string>	Set volume label\n");

	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	struct exfat_blk_dev bd;
	struct exfat_user_input ui;
	struct pbr *ppbr;
	int ret, flags, label_len;
	char label[22] = {0};
	struct exfat_dentry ed;

	if (argc < 2 || argc > 3) {
		usage();
		return -1;
	}

	label_len = strlen(argv[2]);
	if (label_len > VOLUME_LABEL_MAX_LEN) {
		exfat_msg(EXFAT_ERROR, "Volume label string is too long\n");
		return -1;
	}
	strncpy(label, argv[2], label_len);

	memset(ui.dev_name, 0, 255);
	strncpy(ui.dev_name, argv[1], 255);

	if (argc == 2) {
		ui->writeable = false;
		flags = EXFAT_GET_LABEL;
	} else {
		ui->writeable = true;
		flags = EXFAT_SET_LABEL;
	}

	printf("exfat-tools version : %s\n", EXFAT_TOOLS_VERSION);
	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	ppbr = malloc(sizeof(struct pbr));
	if (!ppbr) {
		exfat_msg(EXFAT_ERROR, "Cannot allocate pbr: out of memory\n");
		return -1;
	}
	memset(ppbr, 0, sizeof(struct pbr));

	ret = exfat_read_sector(bd, ppbr, 0);
	if (ret < 0)
		goto out;

	root_entry_off = (ppbr->bsx->root_start_clu << ppbr->bsx->sect_per_clus_bits) << ppbr->bsx->sect_size_bits;

	read_bytes = exfat_read(bd->dev_fd, &ed, sizeof(ed), root_entry_off);
	if (read_bytes != sizeof(ed))
		goto out;

	if (flags == EXFAT_GET_LABEL) {
		ret = exfat_convert_utf16s_to_char(ed.vol_label, ed.vol_char_cnt, label, label_len);
		if (ret < 0)
			goto out;

		exfat_msg(EXFAT_INFO, "Volume label : %s\n", label);

	} else {
		ret = exfat_convert_char_to_utf16(label, label_len, ed.vol_label, ed.vol_char_cnt);
		if (ret < 0)
			goto out;

		nbytes = exfat_write(bd->dev_fd, ed, sizeof(ed), root_entry_off);
		if (nbytes != sizeof(ed))
			goto out;
		exfat_msg(EXFAT_INFO, "Set Volume label complete!!\n");
	}

out:
	free(ppbr);
	return 0;
}
