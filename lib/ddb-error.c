/* ddb-error.c - access a device emulating random errors
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
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <ddb.h>
#include "ddb-private.h"

typedef struct {
    ddb_device_operations_t ops;
    off_t errors;
    unsigned int error_level;
} extra_t;

#define extra(dev) ((extra_t *)((dev)->extra))

static int error_read(ddb_device_t * dev,
		int nblocks, ddb_block_t blocks[nblocks], int flags)
{
    ddb_device_t * sub = dev->info.devs[0];
    int ok;
    if (sub->ops->read)
	ok = sub->ops->read(sub, nblocks, blocks, flags);
    else
	ok = ddb_device_read_multi(sub, nblocks, blocks, flags);
    if (ok >= 0) {
	int b;
	for (b = 0; b < nblocks; b++) {
	    if (blocks[b].result < 0) continue;
	    if (rand() >= extra(dev)->error_level) continue;
	    blocks[b].result = 0;
	    blocks[b].error = EIO;
	    extra(dev)->errors++;
	    ok--;
	}
    }
    return ok;
}

static int error_write(ddb_device_t * dev,
		int nblocks, ddb_block_t blocks[nblocks])
{
    ddb_block_t revblocks[nblocks];
    ddb_device_t * sub = dev->info.devs[0];
    int bmap[nblocks], b, count, ok;
    for (b = count = 0; b < nblocks; b++) {
	if (rand() < extra(dev)->error_level) {
	    blocks[b].result = 0;
	    blocks[b].error = EIO;
	    extra(dev)->errors++;
	} else {
	    bmap[count] = b;
	    revblocks[count] = blocks[b];
	    count++;
	}
    }
    if (sub->ops->write)
	ok = sub->ops->write(sub, count, revblocks);
    else
	ok = ddb_device_write_multi(sub, count, revblocks);
    if (ok < 0) return ok;
    for (b = 0; b < count; b++) {
	blocks[bmap[b]].result = revblocks[b].result;
	blocks[bmap[b]].error = revblocks[b].error;
    }
    return ok;
}

static void error_info(ddb_device_t * dev, ddb_device_info_t * info) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->info)
	sub->ops->info(sub, info);
    else
	ddb_device_info(sub, info);
}

static int error_print(ddb_device_t * dev, int level,
		int (*f)(int, const char *, void *), void * arg, int v)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->print)
	return sub->ops->print(sub, level, f, arg, v);
    errno = ENOSYS;
    return -1;
}

static int error_report(ddb_device_t * dev,
		int (*f)(const char *, void *), void * arg)
{
    char errors[32];
    ddb_device_t * sub = dev->info.devs[0];
    int ok = 0, fo;
    if (sub->ops->report)
	ok = sub->ops->report(sub, f, arg);
    snprintf(errors, sizeof(errors), "Errors triggered: %lld",
	     (long long)extra(dev)->errors);
    fo = f(errors, arg);
    if (fo < 0 && ok >= 0) ok = fo;
    return ok;
}

static int error_has_block(ddb_device_t * dev, off_t block) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->has_block)
	return sub->ops->has_block(sub, block);
    errno = ENOSYS;
    return -1;
}

static ddb_blocklist_t * error_blocks(ddb_device_t * dev) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->blocks)
	return sub->ops->blocks(sub);
    errno = ENOSYS;
    return NULL;
}

static ddb_blocklist_t * error_range(ddb_device_t * dev) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->range)
	return sub->ops->range(sub);
    errno = ENOSYS;
    return NULL;
}

static ddb_blocklist_t * error_has_blocks(ddb_device_t * dev,
		const ddb_blocklist_t * blocks)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->has_blocks)
	return sub->ops->has_blocks(sub, blocks);
    errno = ENOSYS;
    return NULL;
}

static int error_iterate(ddb_device_t * dev,
		int (*f)(off_t, off_t, void *), void * arg)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->iterate)
	return sub->ops->iterate(sub, f, arg);
    errno = ENOSYS;
    return -1;
}

static int error_flush(ddb_device_t * dev) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->flush)
	return sub->ops->flush(sub);
    return 0;
}

static ddb_device_operations_t error_ops = {
    .type       = NULL,
    .read       = error_read,
    .write      = error_write,
    .info       = error_info,
    .print      = error_print,
    .has_block  = error_has_block,
    .blocks     = error_blocks,
    .range      = error_range,
    .has_blocks = error_has_blocks,
    .iterate    = error_iterate,
    .flush      = error_flush,
    .report     = error_report,
    .close      = NULL,
};

ddb_device_t * ddb_device_open_error(const char * name,
		size_t block_size, int flags, off_t total_size)
{
    char namebuff[1 + strlen(name)];
    char *subtype = NULL, *subname = NULL, *ep;
    ddb_device_t * sub = NULL, * dev = NULL;
    double d;
    long v;
    unsigned int level = (unsigned int)RAND_MAX + 1;
    strcpy(namebuff, name);
    subname = index(namebuff, ':');
    if (! subname) {
	errno = EINVAL;
	return NULL;
    }
    /* see if this is a valid error level */
    *subname++ = 0;
    errno = 0;
    v = strtol(namebuff, &ep, 0);
    if (errno) return NULL;
    if (v < 0 || v >= 100) {
	errno = ERANGE;
	return NULL;
    }
    /* calculate error level as double to avoid overflows */
    d = (double)v * (double)level / 100.0;
    level = (unsigned int)d;
    /* and now see if we have type:name */
    ep = index(subname, ':');
    if (ep) {
	*ep++ = 0;
	subtype = subname;
	subname = ep;
    }
    sub = ddb_device_open(subname, subtype, block_size, flags, total_size);
    if (! sub) return NULL;
    dev = ddb_device_open_multi(sizeof(extra_t), 1, &sub, flags);
    if (! dev) {
	int sve = errno;
	ddb_device_close(sub);
	errno = sve;
	return NULL;
    }
    dev->ops = &extra(dev)->ops;
    ddb_copy_ops(&extra(dev)->ops, sub, &error_ops);
    extra(dev)->ops.type = DDB_TYPE_ERR;
    extra(dev)->ops.report = error_ops.report;
    extra(dev)->errors = 0;
    extra(dev)->error_level = level;
    return dev;
}

