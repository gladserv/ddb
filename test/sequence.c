/* test/sequence.c - testing functions for "sequence" devices
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
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <ddb.h>
#include "ddb-test.h"

#define NUM_BLOCKS 16

static const char * which[NUM_BLOCKS];
static const char * full[NUM_BLOCKS], * incr1[NUM_BLOCKS], * incr2[NUM_BLOCKS];

static int is_incr_name(const struct dirent * ent) {
    int len;
    if (ent->d_name[0] == '\0' || ent->d_name[0] == '.')
	return 0;
    len = strlen(ent->d_name);
    if (len != 24) return 0;
    if (strncmp(ent->d_name, "incr-", 5) != 0) return 0;
    /* year */
    if (! isdigit(ent->d_name[5])) return 0;
    if (! isdigit(ent->d_name[6])) return 0;
    if (! isdigit(ent->d_name[7])) return 0;
    if (! isdigit(ent->d_name[8])) return 0;
    if (ent->d_name[9] != '-') return 0;
    /* month */
    if (! isdigit(ent->d_name[10])) return 0;
    if (! isdigit(ent->d_name[11])) return 0;
    if (ent->d_name[12] != '-') return 0;
    /* day */
    if (! isdigit(ent->d_name[13])) return 0;
    if (! isdigit(ent->d_name[14])) return 0;
    if (ent->d_name[15] != ':') return 0;
    /* hour */
    if (! isdigit(ent->d_name[16])) return 0;
    if (! isdigit(ent->d_name[17])) return 0;
    if (ent->d_name[18] != ':') return 0;
    /* minute */
    if (! isdigit(ent->d_name[19])) return 0;
    if (! isdigit(ent->d_name[20])) return 0;
    if (ent->d_name[21] != ':') return 0;
    /* second */
    if (! isdigit(ent->d_name[22])) return 0;
    if (! isdigit(ent->d_name[23])) return 0;
    return 1;
}

static int remove_sequence(const char * dir, int really) {
    char fb[strlen(dir) + 256], * fe;
    DIR * dp = opendir(dir);
    struct dirent * ep;
    int count = 0;
    if (! dp) return 0;
    strcpy(fb, dir);
    fe = fb + strlen(fb);
    *fe++ = '/';
    while ((ep = readdir(dp)) != NULL) {
	if (strcmp(ep->d_name, "full") == 0) goto rm_it;
	if (strcmp(ep->d_name, "meta") == 0) goto rm_it;
	if (strcmp(ep->d_name, "checksum") == 0) goto rm_it;
	if (is_incr_name(ep)) goto rm_it;
	continue;
    rm_it:
	count++;
	if (really) {
	    strcpy(fe, ep->d_name);
	    unlink(fb);
	}
    }
    closedir(dp);
    if (really) rmdir(dir);
    return count;
}

static void write_mark(ddb_device_t * dev, int block, const char * extra) {
    write_block(dev, (off_t)block, extra);
    which[block] = extra;
}

static void read_device(ddb_device_t * dev, const char * F[]) {
    int i;
    for (i = 0; i < NUM_BLOCKS; i++)
	read_block(dev, (off_t)i, F[i]);
}

static inline time_t backup_time(ddb_device_t * dev) {
    ddb_device_info_t info;
    ddb_device_info(dev, &info);
    return info.mtime;
}

static inline int all_times(ddb_device_t * dev, int s, time_t * p) {
    ddb_device_info_t info, sub;
    int i, num;
    ddb_device_info(dev, &info);
    num = info.multi_device;
    for (i = 0; i < num && i < s; i++) {
	ddb_device_info(info.devs[i], &sub);
	p[i] = sub.mtime;
    }
    return num;
}

int main(int argc, char * argv[]) {
    char tsbuffer[64];
    ddb_blocklist_t * ls;
    ddb_device_t * dev;
    time_t all[3], fulltime, incr1time, incr2time;
    int i;
    test_init(argc, argv);
    /* create directory tmp and ignore any errors */
    mkdir("tmp", 0777);
    /* create new sequence in tmp */
    remove_sequence("tmp/img2", 1);
    dev = ddb_device_open("tmp/img2", "sequence", (size_t)BLOCK_SIZE,
			  O_RDWR|O_CREAT, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    ls = ddb_device_blocks(dev);
    if (! ls) goto error;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    /* simulate a full backup */
    for (i = 0; i < NUM_BLOCKS; i++)
	write_mark(dev, i, "full");
    for (i = 0; i < NUM_BLOCKS; i++)
	full[i] = which[i];
    read_device(dev, full);
    test_int(ddb_device_close(dev), 0);
    /* we are supposed to have 2 files: meta, full */
    test_int(remove_sequence("tmp/img2", 0), 2);
    /* now reopen the sequence which will create an incremental backup */
    sleep(1); /* to make sure we can tell full from first incremental */
    dev = ddb_device_open("tmp/img2", "sequence", (size_t)BLOCK_SIZE,
			  O_RDWR, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    write_mark(dev, 5, "incr1");
    write_mark(dev, 10, "incr1");
    for (i = 0; i < NUM_BLOCKS; i++)
	incr1[i] = which[i];
    read_device(dev, incr1);
    test_int(ddb_device_close(dev), 0);
    /* we are supposed to have 3 files: meta, full, incr */
    test_int(remove_sequence("tmp/img2", 0), 3);
    /* now reopen the sequence which will create a second incremental backup */
    sleep(1); /* to make sure we get a new filename */
    dev = ddb_device_open("tmp/img2", "sequence", (size_t)BLOCK_SIZE,
			  O_RDWR, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    write_mark(dev, 3, "incr2");
    write_mark(dev, 7, "incr2");
    write_mark(dev, 10, "incr2");
    for (i = 0; i < NUM_BLOCKS; i++)
	incr2[i] = which[i];
    read_device(dev, incr2);
    test_int(ddb_device_close(dev), 0);
    /* we are supposed to have 4 files: meta, full, incr, incr */
    test_int(remove_sequence("tmp/img2", 0), 4);
    /* open the backup to get the mtimes from the metadata */
    dev = ddb_device_open("tmp/img2", "sequence", (size_t)BLOCK_SIZE,
			  O_RDONLY, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    test_int(all_times(dev, sizeof(all) / sizeof(all[0]), all), 3);
    fulltime = all[0];
    incr1time = all[1];
    incr2time = all[2];
    test_int(ddb_device_close(dev), 0);
    /* open the full backup and read it */
    strftime(tsbuffer, sizeof(tsbuffer), "tmp/img2/%Y-%m-%d:%H:%M:%S",
	     gmtime(&fulltime));
    dev = ddb_device_open(tsbuffer, "sequence", (size_t)BLOCK_SIZE,
			  O_RDONLY, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    read_device(dev, full);
    test_int(ddb_device_close(dev), 0);
    /* open the first incremental backup and read it */
    strftime(tsbuffer, sizeof(tsbuffer), "tmp/img2/%Y-%m-%d:%H:%M:%S",
	     gmtime(&incr1time));
    dev = ddb_device_open(tsbuffer, "sequence", (size_t)BLOCK_SIZE,
			  O_RDONLY, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    read_device(dev, incr1);
    test_int(ddb_device_close(dev), 0);
    /* update the last incremental backup, without writing a new one */
    dev = ddb_device_open("tmp/img2/last", "sequence", (size_t)BLOCK_SIZE,
			  O_RDWR, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    write_mark(dev, 9, "last");
    write_mark(dev, 14, "last");
    read_device(dev, which);
    test_int(ddb_device_close(dev), 0);
    /* we are supposed to have 4 files: meta, full, incr, incr */
    test_int(remove_sequence("tmp/img2", 0), 4);
    /* now join first incremental into full backup */
    test_int(ddb_action("tmp/img2", "sequence", "join",
			NULL, NULL, 0, NULL, NULL), 0);
    /* we are supposed to have 3 files: meta, full, incr */
    test_int(remove_sequence("tmp/img2", 0), 3);
    /* open the full backup and read it, but now it contains full+incr1 */
    strftime(tsbuffer, sizeof(tsbuffer), "tmp/img2/%Y-%m-%d:%H:%M:%S",
	     gmtime(&incr1time));
    dev = ddb_device_open(tsbuffer, "sequence", (size_t)BLOCK_SIZE,
			  O_RDONLY, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    read_device(dev, incr1);
    test_int(ddb_device_close(dev), 0);
    /* open the incremental backup and read it */
    strftime(tsbuffer, sizeof(tsbuffer), "tmp/img2/%Y-%m-%d:%H:%M:%S",
	     gmtime(&incr2time));
    dev = ddb_device_open(tsbuffer, "sequence", (size_t)BLOCK_SIZE,
			  O_RDONLY, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    read_device(dev, which);
    test_int(ddb_device_close(dev), 0);
    /* open the default view, last data available, and check mtimes */
    dev = ddb_device_open("tmp/img2", "sequence", (size_t)BLOCK_SIZE,
			  O_RDONLY, (off_t)BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    test_int(backup_time(dev), incr2time);
    test_int(all_times(dev, sizeof(all) / sizeof(all[0]), all), 2);
    test_int(all[0], incr1time);
    test_int(all[1], incr2time);
    test_int(ddb_device_close(dev), 0);
    /* all done */
    remove_sequence("tmp/img2", 1);
    return test_summary();
error:
    perror("FATAL ERROR, cannot continue testing");
    return 1;
}

