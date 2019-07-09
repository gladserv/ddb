/* test/image.c - testing functions for "image" and "meta" devices
 *
 * Copyright (c) 2018 Claudio Calvelli <ddb@gladserv.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. If the program is modified in any way, a line must be added to the
 *    above copyright notice to state that such modification has occurred.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS \"AS IS\" AND
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ddb.h>
#include "ddb-test.h"

#define NUM_BLOCKS 2048

int main(int argc, char * argv[]) {
    ddb_blocklist_t * ls;
    ddb_device_t * dev;
    int i;
    test_init(argc, argv);
    /* create directory tmp and ignore any errors */
    mkdir("tmp", 0777);
    /* create new image in tmp */
    unlink("tmp/img1");
    dev = ddb_device_open("tmp/img1", "image", (size_t)BLOCK_SIZE,
			  O_RDWR|O_CREAT, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    read_empty(dev, 42);
    write_block(dev, 42, "");
    write_block(dev, 1042, "");
    read_block(dev, 42, "");
    read_block(dev, 1042, "");
    test_int(ddb_device_close(dev), 0);
    unlink("tmp/img1");
    dev = ddb_device_open("tmp/img1", "meta", (size_t)BLOCK_SIZE,
			  O_RDWR|O_CREAT, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, -1);
    ddb_blocklist_free(ls);
    ls = ddb_device_copy_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    write_block(dev, 42, "");
    write_block(dev, 1042, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 42, 42, 1042, 1042, -1);
    ddb_blocklist_free(ls);
    ls = ddb_device_copy_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 42, "");
    read_block(dev, 1042, "");
    write_block(dev, 42, "second");
    write_block(dev, 1042, "urgle");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 42, 42, 1042, 1042, -1);
    ddb_blocklist_free(ls);
    ls = ddb_device_copy_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 42, "second");
    read_block(dev, 1042, "urgle");
    /* test adding blocks to the end of the last range */
    write_block(dev, 1043, "");
    write_block(dev, 1044, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 42, 42, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 1043, "");
    read_block(dev, 1044, "");
    /* this device stores 20 metadata entries per block, with the current
     * implementation, so add 18 more */
    for (i = 0; i < 18; i++)
	write_block(dev, i * 10, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 20, 20, 30, 30, 40, 40, 42, 42,
		     50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100,
		     110, 110, 120, 120, 130, 130, 140, 140, 150, 150,
		     160, 160, 170, 170, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    for (i = 0; i < 18; i++)
	read_block(dev, i * 10, "");
    /* and now force to split it; this will go into the second half */
    write_block(dev, 142, "split");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 20, 20, 30, 30, 40, 40, 42, 42,
		     50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100,
		     110, 110, 120, 120, 130, 130, 140, 140, 142, 142,
		     150, 150, 160, 160, 170, 170, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 142, "split");
    /* second half will have 11 elements, write 9 more */
    for (i = 0; i < 9; i++)
	write_block(dev, i * 10 + 500, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 20, 20, 30, 30, 40, 40, 42, 42,
		     50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100,
		     110, 110, 120, 120, 130, 130, 140, 140, 142, 142,
		     150, 150, 160, 160, 170, 170, 500, 500, 510, 510,
		     520, 520, 530, 530, 540, 540, 550, 550, 560, 560,
		     570, 570, 580, 580, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    for (i = 0; i < 9; i++)
	read_block(dev, i * 10 + 500, "");
    /* add a new block to the second half, which will split, and make
     * the new block appear in the first half of the second half... */
    write_block(dev, 105, "new split");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 20, 20, 30, 30, 40, 40, 42, 42,
		     50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100,
		     105, 105, 110, 110, 120, 120, 130, 130, 140, 140,
		     142, 142, 150, 150, 160, 160, 170, 170, 500, 500,
		     510, 510, 520, 520, 530, 530, 540, 540, 550, 550,
		     560, 560, 570, 570, 580, 580, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 105, "new split");
    /* now test adding blocks to an inner range */
    read_empty(dev, 43);
    read_empty(dev, 44);
    write_block(dev, 43, "");
    write_block(dev, 44, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 20, 20, 30, 30, 40, 40, 42, 44,
		     50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100,
		     105, 105, 110, 110, 120, 120, 130, 130, 140, 140,
		     142, 142, 150, 150, 160, 160, 170, 170, 500, 500,
		     510, 510, 520, 520, 530, 530, 540, 540, 550, 550,
		     560, 560, 570, 570, 580, 580, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 43, "");
    read_block(dev, 44, "");
    read_empty(dev, 45);
    /* test again adding blocks to the end of the last range */
    write_block(dev, 106, "");
    write_block(dev, 107, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 20, 20, 30, 30, 40, 40, 42, 44,
		     50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100,
		     105, 107, 110, 110, 120, 120, 130, 130, 140, 140,
		     142, 142, 150, 150, 160, 160, 170, 170, 500, 500,
		     510, 510, 520, 520, 530, 530, 540, 540, 550, 550,
		     560, 560, 570, 570, 580, 580, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 106, "");
    read_block(dev, 107, "");
    /* now force another block split, between blocks... the first metadata
     * block will have 11 elements now, so make it 20 */
    for (i = 0; i < 3; i++) {
	write_block(dev, i + 12, "");
	write_block(dev, i + 22, "");
	write_block(dev, i + 32, "");
    }
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 12, 14, 20, 20, 22, 24, 30, 30,
		     32, 34, 40, 40, 42, 44, 50, 50, 60, 60, 70, 70,
		     80, 80, 90, 90, 100, 100, 105, 107, 110, 110,
		     120, 120, 130, 130, 140, 140, 142, 142, 150, 150,
		     160, 160, 170, 170, 500, 500, 510, 510, 520, 520,
		     530, 530, 540, 540, 550, 550, 560, 560, 570, 570,
		     580, 580, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    for (i = 0; i < 3; i++) {
	read_block(dev, i + 12, "");
	read_block(dev, i + 22, "");
	read_block(dev, i + 32, "");
    }
    /* and add this new block */
    write_block(dev, 81, "");
    write_block(dev, 82, "");
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, 0, 10, 10, 12, 14, 20, 20, 22, 24, 30, 30,
		     32, 34, 40, 40, 42, 44, 50, 50, 60, 60, 70, 70,
		     80, 82, 90, 90, 100, 100, 105, 107, 110, 110,
		     120, 120, 130, 130, 140, 140, 142, 142, 150, 150,
		     160, 160, 170, 170, 500, 500, 510, 510, 520, 520,
		     530, 530, 540, 540, 550, 550, 560, 560, 570, 570,
		     580, 580, 1042, 1044, -1);
    ddb_blocklist_free(ls);
    read_block(dev, 81, "");
    read_block(dev, 82, "");
    test_int(ddb_device_close(dev), 0);
    unlink("tmp/img1");
    return test_summary();
error:
    perror("FATAL ERROR, cannot continue testing");
    return 1;
}

