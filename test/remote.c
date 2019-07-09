/* test/remote.c - testing functions for the "remote" device protocol
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/wait.h>
#include <time.h>
#include <ddb.h>
#include "ddb-test.h"

#define NUM_BLOCKS 1024

static int run_remote(FILE * in, FILE * out) {
    ddb_plugin_t * pi = ddb_plugin_init(in, out);
    int ret = 0;
    if (! pi) return 0;
    while (1) {
	int ok = ddb_plugin_run(pi);
	if (ok > 0) continue;
	if (ok == 0) ret = 1;
	break;
    }
    ddb_plugin_exit(pi);
    return ret;
}

static int open_pipes(FILE ** r, FILE ** w) {
    int fd[2];
    if (pipe(fd) < 0) return 0;
    *r = fdopen(fd[0], "r");
    if (! *r) return 0;
    *w = fdopen(fd[1], "w");
    if (! *w) return 0;
    return 1;
}

static int tcp_listen(pid_t * pid, int * port) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int fd, sve;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto error;
    if (listen(fd, 1) < 0) goto error;
    if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) < 0) goto error;
    *port = ntohs(addr.sin_port);
    *pid = fork();
    if (*pid < 0) goto error;
    if (*pid == 0) {
	FILE * F = NULL;
	int lfd = -1;
	if ((lfd = accept(fd, NULL, NULL)) < 0) goto child_error;
	close(fd);
	fd = -1;
	F = fdopen(lfd, "r+");
	if (! F) goto child_error;
	lfd = -1;
	run_remote(F, F);
	exit(0);
    child_error:
	perror("TCP");
	if (fd >= 0) close(fd);
	if (lfd >= 0) close(lfd);
	if (F) fclose(F);
	exit(1);
    }
    close(fd);
    return 1;
error:
    sve = errno;
    if (fd >= 0) close(fd);
    errno = sve;
    return 0;
}

static int f_iter(off_t start, off_t end, void * _num) {
    int * ptr = _num, num = (*ptr)++;
    off_t val;
    if (num > 1) return -1;
    val = num ? 542 : 42;
    if (start != val) return -1;
    if (end != val) return -1;
    return 0;
}

static int f_print(int level, const char * line, void * _num) {
    int * num = _num;
    if (strncmp(line, "name: ", 6) == 0) *num |= 0x01;
    if (strncmp(line, "block-size: ", 12) == 0) *num |= 0x02;
    if (strncmp(line, "total-size: ", 12) == 0) *num |= 0x04;
    if (strncmp(line, "num-blocks: ", 12) == 0) *num |= 0x08;
    if (strncmp(line, "blocks-present: ", 16) == 0) *num |= 0x10;
    if (strncmp(line, "blocks-allocated: ", 18) == 0) *num |= 0x20;
    if (strncmp(line, "modified: ", 10) == 0) *num |= 0x40;
    if (strncmp(line, "multi-device: ", 14) == 0) *num |= 0x80;
    if (strncmp(line, "block-range: 42", 15) == 0) *num |= 0x100;
    if (strncmp(line, "block-range: 542", 16) == 0) *num |= 0x200;
    return 0;
}

static int f_report(const char * line, void * _num) {
    int * num = _num;
    if (strncmp(line, "Bytes sent: ", 12) == 0) *num |= 0x01;
    if (strncmp(line, "Bytes received: ", 16) == 0) *num |= 0x02;
    return 0;
}

static int write_device(ddb_device_t * dev, const char * name) {
    ddb_blocklist_t * ls, * rs;
    ddb_device_info_t info;
    int iter;
    ls = ddb_device_copy_blocks(dev);
    if (! ls) return 0;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    ls = ddb_device_blocks(dev);
    if (! ls) return 0;
    compare_list(ls, -1);
    ddb_blocklist_free(ls);
    read_empty(dev, 42);
    write_block(dev, 42, "");
    write_block(dev, 542, "");
    read_block(dev, 42, "");
    read_block(dev, 542, "");
    ls = ddb_device_blocks(dev);
    if (! ls) return 0;
    compare_list(ls, 42, 42, 542, 542, -1);
    ddb_blocklist_free(ls);
    test_int(ddb_device_has_block(dev, 42), 1);
    test_int(ddb_device_has_block(dev, 43), 0);
    rs = ddb_blocklist_new();
    if (! rs) return 0;
    if (ddb_blocklist_add(rs, 12, 142) < 0) return 0;
    ls = ddb_device_has_blocks(dev, rs);
    if (! ls) return 0;
    ddb_blocklist_free(rs);
    compare_list(ls, 42, 42, -1);
    ddb_blocklist_free(ls);
    ddb_device_info(dev, &info);
    test_int(info.multi_device, 0);
    test_int(info.block_size, BLOCK_SIZE);
    test_int(info.total_size, BLOCK_SIZE * NUM_BLOCKS);
    test_int(info.num_blocks, NUM_BLOCKS);
    test_int(info.blocks_present, 2);
    test(strcmp(info.name, name) == 0, info.name, name);
    iter = 0;
    test_ge(ddb_device_block_iterate(dev, f_iter, &iter), 0);
    iter = 0;
    test_ge(ddb_device_info_print(dev, 0, f_print, &iter, 0), 0);
    test_int(iter, 0xff);
    iter = 0;
    test_ge(ddb_device_info_print(dev, 0, f_print, &iter, 1), 0);
    test_int(iter, 0x3ff);
    test_int(ddb_device_flush(dev), 0);
    iter = 0;
    test_int(ddb_device_report(dev, f_report, &iter), 0);
    test_int(iter, 0x03);
    test_int(ddb_device_close(dev), 0);
    return 1;
}

static int read_device(ddb_device_t * dev, const char * name) {
    ddb_blocklist_t * ls;
    ddb_device_info_t info;
    int iter;
    ls = ddb_device_copy_blocks(dev);
    if (! ls) return 0;
    compare_list(ls, 0, NUM_BLOCKS - 1, -1);
    ddb_blocklist_free(ls);
    read_empty(dev, 242);
    read_block(dev, 42, "");
    read_block(dev, 542, "");
    ls = ddb_device_blocks(dev);
    if (! ls) return 0;
    compare_list(ls, 42, 42, 542, 542, -1);
    ddb_blocklist_free(ls);
    test_int(ddb_device_has_block(dev, 42), 1);
    test_int(ddb_device_has_block(dev, 43), 0);
    if (name) {
	ddb_device_info(dev, &info);
	test_int(info.multi_device, 0);
	test_int(info.block_size, BLOCK_SIZE);
	test_int(info.total_size, BLOCK_SIZE * NUM_BLOCKS);
	test_int(info.num_blocks, NUM_BLOCKS);
	test_int(info.blocks_present, 2);
	test(strcmp(info.name, name) == 0, info.name, name);
    }
    iter = 0;
    test_ge(ddb_device_block_iterate(dev, f_iter, &iter), 0);
    iter = 0;
    test_ge(ddb_device_info_print(dev, 0, f_print, &iter, 0), 0);
    test_int(iter, 0xff);
    iter = 0;
    test_ge(ddb_device_info_print(dev, 0, f_print, &iter, 1), 0);
    test_int(iter, 0x3ff);
    test_int(ddb_device_flush(dev), 0);
    iter = 0;
    test_int(ddb_device_report(dev, f_report, &iter), 0);
    test_int(iter, name ? 0x03 : 0);
    test_int(ddb_device_close(dev), 0);
    return 1;
}

int main(int argc, char * argv[]) {
    char tcp_name[32];
    const char * orig_path = getenv("PATH");
    char new_path[10 + (orig_path ? strlen(orig_path) : 0)];
    FILE * ltr = NULL, * rtl = NULL, * lfr = NULL, * rfl = NULL;
    ddb_device_t * dev;
    pid_t remote;
    int port;
    /* set up environment for calling ddb-daemon */
    strcpy(new_path, "../src");
    if (orig_path) {
	strcat(new_path, ":");
	strcat(new_path, orig_path);
    }
    setenv("LD_LIBRARY_PATH", "../lib", 1);
    setenv("PATH", new_path, 1);
    setenv("DDB_SYSCONFIG", "-", 1);
    setenv("DDB_CONFIG", "-", 1);
    test_init(argc, argv);
    /* create directory tmp and ignore any errors */
    mkdir("tmp", 0777);
    /* now we create a "remote" image using pipes directly */
    unlink("tmp/img3");
    /* create pipes */
    if (! open_pipes(&lfr, &rtl)) goto error;
    if (! open_pipes(&rfl, &ltr)) goto error;
    /* make a "remote" process */
    remote = fork();
    if (remote < 0) goto error;
    if (remote == 0) {
	fclose(lfr);
	fclose(ltr);
	exit(run_remote(rfl, rtl) ? 0 : 1);
    }
    fclose(rfl);
    fclose(rtl);
    dev = ddb_device_pipe(lfr, ltr, remote, O_RDWR|O_CREAT,
			  BLOCK_SIZE, BLOCK_SIZE * NUM_BLOCKS,
			  "tmp/img3", "meta");
    if (! dev) goto error;
    if (! write_device(dev, "tmp/img3")) goto error;
    /* and now we create a "remote" image using our test configuration
     * to open a pipe to ddb-daemon */
    dev = ddb_device_open("tmp/img4", "test-remote", BLOCK_SIZE,
			  O_RDWR|O_CREAT, BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    if (! write_device(dev, "tmp/img4")) goto error;
    /* and then we read the first device using ddb-daemon */
    dev = ddb_device_open("tmp/img3", "test-remote", BLOCK_SIZE,
			  O_RDONLY, BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    if (! read_device(dev, "tmp/img3")) goto error;
    /* and vice versa read the second device using pipes directly */
    /* create pipes */
    if (! open_pipes(&lfr, &rtl)) goto error;
    if (! open_pipes(&rfl, &ltr)) goto error;
    /* make a "remote" process */
    remote = fork();
    if (remote < 0) goto error;
    if (remote == 0) {
	fclose(lfr);
	fclose(ltr);
	exit(run_remote(rfl, rtl) ? 0 : 1);
    }
    fclose(rfl);
    fclose(rtl);
    dev = ddb_device_pipe(lfr, ltr, remote, O_RDONLY,
			  BLOCK_SIZE, BLOCK_SIZE * NUM_BLOCKS,
			  "tmp/img4", "meta");
    if (! dev) goto error;
    if (! read_device(dev, "tmp/img4")) goto error;
    /* finally, read the first device using a TCP connection */
    if (! tcp_listen(&remote, &port)) goto error;
    snprintf(tcp_name, sizeof(tcp_name), "%d:tmp/img3", port);
    dev = ddb_device_open(tcp_name, "test-tcp", BLOCK_SIZE,
			  O_RDONLY, BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    if (! read_device(dev, "tmp/img3")) goto error;
    /* in case it's still there */
    kill(remote, SIGKILL);
    waitpid(remote, NULL, WNOHANG);
    /* now use the configuration to open a "remote" file which is really
     * "local" i.e. ddb-remote will call ddb_device_open_local */
    dev = ddb_device_open("tmp/img3", "test-local", BLOCK_SIZE,
			  O_RDONLY, BLOCK_SIZE * NUM_BLOCKS);
    if (! dev) goto error;
    if (! read_device(dev, NULL)) goto error;
    unlink("tmp/img3");
    unlink("tmp/img4");
    return test_summary();
error:
    perror("FATAL ERROR, cannot continue testing");
    return 1;
}

