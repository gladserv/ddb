/* test/cfile.c - testing configuration file handling
 *
 * note that unlike other tests this tests some private functions
 * from the library, rather than public functions
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
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ddb.h>
#include "../lib/ddb-private.h"
#include "ddb-test.h"

#define NUM_BLOCKS 1024

typedef struct {
    int type;
    int nargs;
    const char * program;
    const char * args[2];
} prep_t;

typedef struct {
    int type;
    int nargs;
    const char * module;
    const char * function;
    const char * args[2];
} conn_t;

/* "prepare" we expect from first result */
static prep_t prepare_1[] = {
    { ddb_prepare_run,  1, "prog42", { "arg", NULL } },
    { ddb_prepare_load, 0, "module42", { NULL, NULL } },
    { 0, -1, }
};

/* "connect" we expect from first result */
static conn_t connect_1r[] = {
    { ddb_connect_open, 0, "/dev/test42r", NULL, { NULL, NULL } },
    { ddb_connect_open, 0, "/dev/test42", "typer", { NULL, NULL } },
    { ddb_connect_tcp, 2, "server", "1234", { "name42", "type" } },
    { ddb_connect_call, 1, "module42", "function", { "arg", NULL } },
    { ddb_connect_acall, 1, "module42", "function", { "arg", NULL } },
    { 0, -1, }
};
static conn_t connect_1w[] = {
    { ddb_connect_open, 0, "/dev/test42w", NULL, { NULL, NULL } },
    { ddb_connect_open, 0, "/dev/test42", "typew", { NULL, NULL } },
    { ddb_connect_tcp, 2, "server", "1234", { "name42", "type" } },
    { ddb_connect_call, 1, "module42", "function", { "arg", NULL } },
    { ddb_connect_acall, 1, "module42", "function", { "arg", NULL } },
    { 0, -1, }
};

/* "close" we expect from first result */
static prep_t close_1[] = {
    { ddb_prepare_run,  1, "cprog42", { "arg", NULL } },
    { 0, -1, }
};

/* "retry_prepare" we expect from first result */
static prep_t retry_prepare_1[] = {
    { ddb_prepare_run,  1, "rprog42", { "arg", NULL } },
    { ddb_prepare_load, 0, "rmodule42", { NULL, NULL } },
    { 0, -1, }
};

/* "retry_connect" we expect from first result */
static conn_t retry_connect_1r[] = {
    { ddb_connect_open, 0, "/dev/test42", "typer", { NULL, NULL } },
    { ddb_connect_tcp, 2, "rserver", "1234", { "name42", "typename42" } },
    { ddb_connect_call, 1, "rmodule42", "function", { "arg", NULL } },
    { ddb_connect_acall, 1, "rmodule42", "function", { "arg", NULL } },
    { 0, -1, }
};
static conn_t retry_connect_1w[] = {
    { ddb_connect_open, 0, "/dev/test42", "typew", { NULL, NULL } },
    { ddb_connect_tcp, 2, "rserver", "1234", { "name42", "typename42" } },
    { ddb_connect_call, 1, "rmodule42", "function", { "arg", NULL } },
    { ddb_connect_acall, 1, "rmodule42", "function", { "arg", NULL } },
    { 0, -1, }
};

/* "retry_close" we expect from first result */
static prep_t retry_close_1[] = {
    { ddb_prepare_run,  1, "rcprog42", { "arg", NULL } },
    { 0, -1, }
};

/* "prepare" we expect from second result */
static prep_t prepare_2e[] = {
    { ddb_prepare_load, 0, "module42e", { NULL, NULL } },
    { 0, -1, }
};
static prep_t prepare_2n[] = {
    { ddb_prepare_load, 0, "module42n", { NULL, NULL } },
    { 0, -1, }
};
static prep_t prepare_2a[] = {
    { ddb_prepare_load, 0, "module42a", { NULL, NULL } },
    { 0, -1, }
};

/* "prepare" we expect from third result */
static prep_t prepare_31[] = {
    { ddb_prepare_load, 0, "m1", { NULL, NULL } },
    { 0, -1, }
};
static prep_t prepare_32[] = {
    { ddb_prepare_load, 0, "m2", { NULL, NULL } },
    { 0, -1, }
};
static prep_t prepare_33[] = {
    { ddb_prepare_load, 0, "m3", { NULL, NULL } },
    { 0, -1, }
};

static inline int eq(const char * a, const char * b) {
    if (! a) return ! b;
    if (! b) return ! a;
    return 0 == strcmp(a, b);
}

static void check_prepare(const ddb_prepare_t * prepare, const prep_t * P) {
    const char * err = NULL;
    while (prepare && P->nargs >= 0) {
	int i;
	if (! err && prepare->type != P->type) err = "type differ";
	if (! err && prepare->nargs != P->nargs) err = "nargs differ";
	if (! err && ! eq(prepare->program, P->program)) err = "program differ";
	for (i = 0; ! err && i < P->nargs; i++)
	    if (! eq(prepare->args[i], P->args[i])) err = "arg differ";
	prepare = prepare->next;
	P++;
    }
    if (! err) {
	if (prepare && P->nargs < 0) err = "prepare longer than P";
	if (! prepare && P->nargs >= 0) err = "P longer than prepare";
    }
    test(err == NULL, err, "");
}

static void check_connect(const ddb_connect_t * connect, const conn_t * C) {
    const char * err = NULL;
    while (connect && C->nargs >= 0) {
	int i;
	if (! err && connect->type != C->type) err = "type differ";
	if (! err && connect->nargs != C->nargs) err = "nargs differ";
	if (! err && ! eq(connect->module, C->module)) err = "module differ";
	if (! err && ! eq(connect->function, C->function)) err = "function differ";
	for (i = 0; ! err && i < C->nargs; i++)
	    if (! eq(connect->args[i], C->args[i])) err = "arg differ";
	connect = connect->next;
	C++;
    }
    if (! err) {
	if (connect && C->nargs < 0) err = "connect longer than P";
	if (! connect && C->nargs >= 0) err = "P longer than connect";
    }
    test(err == NULL, err, "");
}

int main(int argc, char * argv[]) {
    ddb_remote_t * descr;
    test_init(argc, argv);
    descr = ddb_read_configuration("name42", "type", DDB_MODE_RO);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_1);
    check_connect(descr->connect, connect_1r);
    check_prepare(descr->close, close_1);
    test_int(descr->retry_max, 5);
    test_int(descr->retry_delay, 42);
    check_prepare(descr->retry_prepare, retry_prepare_1);
    check_connect(descr->retry_connect, retry_connect_1r);
    check_prepare(descr->retry_close, retry_close_1);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name42", "type", DDB_MODE_RW);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_1);
    check_connect(descr->connect, connect_1w);
    check_prepare(descr->close, close_1);
    test_int(descr->retry_max, 5);
    test_int(descr->retry_delay, 42);
    check_prepare(descr->retry_prepare, retry_prepare_1);
    check_connect(descr->retry_connect, retry_connect_1w);
    check_prepare(descr->retry_close, retry_close_1);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name42", "xtype", DDB_MODE_EXCL);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_2e);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name42", "xtype", DDB_MODE_RW);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_2n);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name42", "xtype", DDB_MODE_ACT);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_2a);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name-1", "type", DDB_MODE_ACT);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_31);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name-1", NULL, DDB_MODE_ACT);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_31);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name-2", "utype", DDB_MODE_ACT);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_32);
    ddb_remote_free(descr);
    descr = ddb_read_configuration("name-3", NULL, DDB_MODE_ACT);
    if (! descr) goto error;
    check_prepare(descr->prepare, prepare_33);
    ddb_remote_free(descr);
    return test_summary();
error:
    perror("FATAL ERROR, cannot continue testing");
    return 1;
}

