/*******************************************************************************
   earthsoft PT3 Linux driver

   This program is free software; you can redistribute it and/or modify it
   under the terms and conditions of the GNU General Public License,
   version 3, as published by the Free Software Foundation.

   This program is distributed in the hope it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   The full GNU General Public License is included in this distribution in
   the file called "COPYING".

 *******************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

static unsigned short
get_lfsr(int isdb, int index)
{
	return (unsigned short)((1 + 2 * isdb + index) * 12345);
}

int
main(int argc, char * const argv[])
{
	unsigned short lfsr, buf;
	int fd, i;

	if (argc != 4) {
		printf("Usage : %s target_file isdb tuner_no\n", argv[0]);
		printf("\tisdb\t\tS=0 T=1\n");
		printf("\ttuner_no\t0 or 1\n");
		return EXIT_SUCCESS;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd <= 0) {
		printf("can not open file %s.\n", argv[1]);
		return EXIT_SUCCESS;
	}

	lfsr = get_lfsr(atoi(argv[2]), atoi(argv[3]));
	while (read(fd, &buf, sizeof(buf))) {
		if (buf != lfsr) {
			printf("check NG! 0x%x\n", lseek(fd, 0, SEEK_CUR));
			goto last;
		}
		lfsr = (lfsr >> 1) ^ (-(lfsr & 1) & 0xb400);
	}
	printf("check OK.\n");
last:
	close(fd);

	return EXIT_SUCCESS;
}
