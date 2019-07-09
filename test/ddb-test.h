/* test/ddb-test.h - helper function to test lists
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

#ifndef __DDB_TEST_H__
#define __DDB_TEST_H__ 1

extern int ntests, failed, verbose;
#define test(cond, a, b) \
    ++ntests; \
    if (cond) { \
	if (verbose) \
	    printf("OK     %3d: %s == %s\n", ntests, (a), (b)); \
    } else { \
	failed++; \
	printf("FAILED %3d: %s != %s\n", ntests, (a), (b)); \
    }

void test_init(int argc, char * argv[]);
void compare_list(const ddb_blocklist_t * ls, ...);
void _test_int(const char * ca, int va, const char * cb, int vb);
#define test_int(a, b) _test_int(#a, a, #b, b)
void _test_ge(const char * ca, int va, const char * cb, int vb);
#define test_ge(a, b) _test_ge(#a, a, #b, b)
int test_summary(void);

#define BLOCK_SIZE 512

void write_block(ddb_device_t * dev, off_t block, const char * extra);
void read_block(ddb_device_t * dev, off_t block, const char * extra);
void read_empty(ddb_device_t * dev, off_t block);

#endif /* __DDB_TEST_H__ */
