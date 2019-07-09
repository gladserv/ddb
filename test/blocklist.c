/* test/blocklist.c - testing functions for ddb_blocklist_*
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
#include <errno.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ddb.h>
#include "ddb-test.h"

int main(int argc, char * argv[]) {
    ddb_blocklist_t * ls = ddb_blocklist_new(), * fl[3];
    const ddb_blocklist_t * ml[3];
    test_init(argc, argv);
    test_int(ddb_blocklist_count(ls), 0);
    /* add a new range to empty list and check */
    test_int(ddb_blocklist_add(ls, 42, 99), 0);
    test_int(ddb_blocklist_count(ls), 58);
    compare_list(ls, 42, 99, -1);
    test_int(ddb_blocklist_has(ls, 39), 0);
    test_int(ddb_blocklist_has(ls, 42), 1);
    test_int(ddb_blocklist_has(ls, 99), 1);
    test_int(ddb_blocklist_has(ls, 100), 0);
    /* add another disjoint range before the first one */
    test_int(ddb_blocklist_add(ls, 3, 16), 0);
    test_int(ddb_blocklist_count(ls), 72);
    compare_list(ls, 3, 16, 42, 99, -1);
    /* add something which joins second range to the left */
    test_int(ddb_blocklist_add(ls, 40, 41), 0);
    test_int(ddb_blocklist_count(ls), 74);
    compare_list(ls, 3, 16, 40, 99, -1);
    /* ditto, with 1 element overlap */
    test_int(ddb_blocklist_add(ls, 38, 40), 0);
    test_int(ddb_blocklist_count(ls), 76);
    compare_list(ls, 3, 16, 38, 99, -1);
    /* ditto, with more overlap */
    test_int(ddb_blocklist_add(ls, 35, 40), 0);
    test_int(ddb_blocklist_count(ls), 79);
    compare_list(ls, 3, 16, 35, 99, -1);
    /* now add something past the end again */
    test_int(ddb_blocklist_add(ls, 135, 142), 0);
    test_int(ddb_blocklist_count(ls), 87);
    compare_list(ls, 3, 16, 35, 99, 135, 142, -1);
    /* and add something joining second range to the right */
    test_int(ddb_blocklist_add(ls, 100, 101), 0);
    test_int(ddb_blocklist_count(ls), 89);
    compare_list(ls, 3, 16, 35, 101, 135, 142, -1);
    /* now with 1 element overlap */
    test_int(ddb_blocklist_add(ls, 101, 103), 0);
    test_int(ddb_blocklist_count(ls), 91);
    compare_list(ls, 3, 16, 35, 103, 135, 142, -1);
    /* and more overlap */
    test_int(ddb_blocklist_add(ls, 101, 105), 0);
    test_int(ddb_blocklist_count(ls), 93);
    compare_list(ls, 3, 16, 35, 105, 135, 142, -1);
    /* something to the left of first range */
    test_int(ddb_blocklist_add(ls, 1, 5), 0);
    test_int(ddb_blocklist_count(ls), 95);
    compare_list(ls, 1, 16, 35, 105, 135, 142, -1);
    /* join first two ranges together... */
    test_int(ddb_blocklist_add(ls, 9, 45), 0);
    test_int(ddb_blocklist_count(ls), 113);
    compare_list(ls, 1, 105, 135, 142, -1);
    /* add two more ranges at end */
    test_int(ddb_blocklist_add(ls, 200, 203), 0);
    test_int(ddb_blocklist_count(ls), 117);
    compare_list(ls, 1, 105, 135, 142, 200, 203, -1);
    test_int(ddb_blocklist_add(ls, 190, 193), 0);
    test_int(ddb_blocklist_count(ls), 121);
    compare_list(ls, 1, 105, 135, 142, 190, 193, 200, 203, -1);
    /* While we have a long list, try a few sublists */
    ml[0] = fl[0] = ddb_blocklist_sub(ls, 42, 192);
    test_int(ddb_blocklist_count(ml[0]), 75);
    compare_list(ml[0], 42, 105, 135, 142, 190, 192, -1);
    ml[1] = fl[1] = ddb_blocklist_sub(ls, 142, 999);
    test_int(ddb_blocklist_count(ml[1]), 9);
    compare_list(ml[1], 142, 142, 190, 193, 200, 203, -1);
    ml[2] = fl[2] = ddb_blocklist_sub(ls, 0, 35);
    test_int(ddb_blocklist_count(ml[2]), 35);
    compare_list(ml[2], 1, 35, -1);
    /* and now join them all together to test multiple overlaps */
    test_int(ddb_blocklist_add(ls, 42, 202), 0);
    test_int(ddb_blocklist_count(ls), 203);
    compare_list(ls, 1, 203, -1);
    /* that will be all for this one */
    ddb_blocklist_free(ls);
    /* try a union */
    ls = ddb_blocklist_union(3, ml);
    test_int(ddb_blocklist_count(ls), 115);
    compare_list(ls, 1, 35, 42, 105, 135, 142, 190, 192, 202, 203, -1);
    ddb_blocklist_free(ls);
    /* and a couple of intersections */
    ls = ddb_blocklist_intersect(3, ml);
    test_int(ddb_blocklist_count(ls), 0);
    compare_list(ls, -1);
    ddb_blocklist_free(ls);
    ls = ddb_blocklist_intersect(2, ml);
    test_int(ddb_blocklist_count(ls), 4);
    compare_list(ls, 142, 142, 190, 192, -1);
    test_int(ddb_blocklist_has(ls, 139), 0);
    test_int(ddb_blocklist_has(ls, 142), 1);
    test_int(ddb_blocklist_has(ls, 143), 0);
    test_int(ddb_blocklist_has(ls, 189), 0);
    test_int(ddb_blocklist_has(ls, 190), 1);
    test_int(ddb_blocklist_has(ls, 192), 1);
    test_int(ddb_blocklist_has(ls, 193), 0);
    ddb_blocklist_free(ls);
    ddb_blocklist_free(fl[0]);
    ddb_blocklist_free(fl[1]);
    ddb_blocklist_free(fl[2]);
    return test_summary();
}

