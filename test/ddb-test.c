/* test/ddb-test.c - helper function to test lists
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
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ddb.h>
#include "ddb-test.h"

int failed = 0, ntests = 0, verbose = 0;
static const char * name = "";

void test_init(int argc, char * argv[]) {
    ntests = failed = verbose = 0;
    name = "";
    ddb_device_configuration(DDB_CONFIG_SYSTEM|DDB_CONFIG_CLEAR, "config");
    ddb_device_configuration(DDB_CONFIG_USER|DDB_CONFIG_CLEAR, NULL);
    if (argc < 1) return;
    name = rindex(argv[0], '/');
    if (name)
	name++;
    else
	name = argv[0];
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v') {
	verbose = 1;
	printf("TEST %s\n", name);
    }
}

typedef struct {
    va_list nums;
    int complete;
    int place;
    off_t rs, re, fs, fe;
} compare_t;

static const char * errs[] = {
    "List element differs",
    "Extra elements in list",
    "Internal error, invalid list",
    "Missing elements in list",
};
#define n_errs (sizeof(errs) / sizeof(errs[0]))

static int compare_next(off_t fs, off_t fe, void * _compare) {
    compare_t * compare = _compare;
    off_t rs, re;
    if (compare->complete) return -2;
    rs = va_arg(compare->nums, int);
    if (rs < 0) {
	compare->complete = 1;
	return -2;
    }
    re = va_arg(compare->nums, int);
    if (re < 0) {
	compare->complete = 1;
	return -3;
    }
    compare->place++;
    if (rs == fs || re == fe) return 0;
    compare->rs = rs;
    compare->fs = fs;
    compare->re = re;
    compare->fe = fe;
    return -1;
}

void compare_list(const ddb_blocklist_t * ls, ...) {
    compare_t compare;
    int err;
    va_start(compare.nums, ls);
    compare.complete = 0;
    compare.place = 0;
    err = -ddb_blocklist_iterate(ls, compare_next, &compare);
    if (err == 0 && ! compare.complete) {
	int n = va_arg(compare.nums, int);
	if (n >= 0) err = 4;
    }
    va_end(compare.nums);
    ++ntests;
    if (err) {
	printf("FAILED %3d: list does not match (%s, place = %d",
		ntests, err > 0 && err <= n_errs ? errs[err - 1] : "Unknown error",
		compare.place);
	if (err == 1)
	    printf(", req=%lld,%lld, found=%lld,%lld",
		    (long long)compare.rs, (long long)compare.re,
		    (long long)compare.fs, (long long)compare.fe);
	printf(")\n");
	failed++;
    } else {
	if (verbose)
	    printf("OK     %3d: list matches\n", ntests);
    }
}

void _test_int(const char * ca, int va, const char * cb, int vb) {
    ++ntests;
    if (va == vb) {
	if (verbose)
	    printf("OK     %3d: %s == %s (%d)\n", ntests, ca, cb, va);
    } else {
	failed++;
	printf("FAILED %3d: %s (%d) != %s (%d)\n", ntests, ca, va, cb, vb);
    }
}

void _test_ge(const char * ca, int va, const char * cb, int vb) {
    ++ntests;
    if (va >= vb) {
	if (verbose)
	    printf("OK     %3d: %s >= %s (%d)\n", ntests, ca, cb, va);
    } else {
	failed++;
	printf("FAILED %3d: %s (%d) < %s (%d)\n", ntests, ca, va, cb, vb);
    }
}

int test_summary(void) {
    if (verbose) {
	if (failed)
	    printf("FAILED %3d out of %d tests\n\n", failed, ntests);
	else
	    printf("OK     %3d tests\n\n", ntests);
    } else {
	if (failed)
	    printf("%-10s  FAILED %3d out of %d tests\n", name, failed, ntests);
	else
	    printf("%-10s  OK     %3d tests\n", name, ntests);
    }
    return failed ? 1 : 0;
}

static char buffer[BLOCK_SIZE], compare[BLOCK_SIZE];

static void fill_buffer(off_t block, const char * extra) {
    memset(buffer, 0, BLOCK_SIZE);
    snprintf(buffer, BLOCK_SIZE, "Block = %lld %s", (long long)block, extra);
}

void write_block(ddb_device_t * dev, off_t block, const char * extra) {
    fill_buffer(block, extra);
    test_int(ddb_device_write(dev, block, buffer), 0);
}

void read_block(ddb_device_t * dev, off_t block, const char * extra) {
    memset(compare, 0, BLOCK_SIZE);
    test_int(ddb_device_read(dev, block, compare, 0), 0);
    fill_buffer(block, extra);
    test(memcmp(compare, buffer, BLOCK_SIZE) == 0, compare, buffer);
}

void read_empty(ddb_device_t * dev, off_t block) {
    memset(compare, 0, BLOCK_SIZE);
    test_int(ddb_device_read(dev, block, compare, 0), 0);
    memset(buffer, 0, BLOCK_SIZE);
    test(memcmp(compare, buffer, BLOCK_SIZE) == 0, compare, buffer);
}

