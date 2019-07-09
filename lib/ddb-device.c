/* ddb-device.c - generic device access for ddb
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

#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <ddb.h>
#include "ddb-private.h"

/* maximum output line for "info print" using ddb_print_line() */
#define MAX_INFO_LINE 256

static int devices_opened = 0;

int ddb_devices_opened(void) {
    return devices_opened;
}

int ddb_type_is(const char * type, const char * required) {
     if (! type) return 1;
     if (strcmp(type, required) != 0) return 0;
     return 1;
}

/* like ddb_device_open() but does not read configuration directories
 * to determine how to open a device */
ddb_device_t * ddb_device_open_local(const char * name,
		const char * type, size_t block_size,
		int flags, off_t total_size)
{
    struct stat sbuff;
    int is_reg, is_meta, is_dir, is_lvm, is_dev, is_error, want_meta;
    int is_packed;
    errno = EINVAL;
    if (! name || ! *name)
	return NULL;
    /* check for normal types */
    is_reg = ddb_type_is(type, DDB_TYPE_REG);
    is_meta = ddb_type_is(type, DDB_TYPE_META);
    is_dir = ddb_type_is(type, DDB_TYPE_DIR);
    is_lvm = ddb_type_is(type, DDB_TYPE_LVM);
    is_dev = ddb_type_is(type, DDB_TYPE_DEV);
    is_error = type && ddb_type_is(type, DDB_TYPE_ERR);
    is_packed = type && ddb_type_is(type, DDB_TYPE_PACK);
    want_meta = type ? (is_meta ? 2 : 0) : 1;
    devices_opened = 1;
    /* check for error device */
    if (is_error)
	return ddb_device_open_error(name, block_size, flags, total_size);
    /* check for LVM volumes */
    if (is_lvm) {
	if (ddb_is_lvm(name, flags) > 0)
	    return ddb_device_open_lvm(name, block_size, flags, total_size);
	if (type) {
	    errno = EINVAL;
	    return NULL;
	}
    }
    /* see if the device is a sequence with optional timestamp */
    if (is_dir || is_packed) {
	int has_type = type ? 1 : 0;
	if (ddb_is_dir(name, is_packed, has_type, flags) > 0)
	    return ddb_device_open_dir(name, is_packed, has_type,
				       block_size, flags, total_size);
	if (type) {
	    errno = EINVAL;
	    return NULL;
	}
    }
    /* or failing that try to open the device directly */
    if (! (is_reg || is_meta || is_dev)) {
	errno = EINVAL;
	return NULL;
    }
    if (stat(name, &sbuff) < 0) {
	if (errno != ENOENT || ! (flags & O_CREAT))
	    return NULL;
	if (is_reg || is_meta) {
	    /* file does not exist and we are going to create it; to
	     * prevent a race condition we'll check that we are actually
	     * creating it */
	    flags |= O_EXCL;
	    return ddb_device_open_image(name, want_meta, block_size,
					 flags, total_size);
	}
	errno = ENOENT;
	return NULL;
    }
    if (S_ISREG(sbuff.st_mode) && (is_reg || is_meta))
	return ddb_device_open_image(name, want_meta, block_size,
				     flags, total_size);
    if (S_ISBLK(sbuff.st_mode) && is_dev)
	return ddb_device_open_image(name, 0, block_size, flags, total_size);
    errno = EINVAL;
    return NULL;
}

/* open device from name; if flags specifies creation, then block_size
 * must be a positive number; if flags specifies an existing device and
 * the block size can be determined automatically, block_size can be 0,
 * but if it is positive it will be used to check consistency */
ddb_device_t * ddb_device_open(const char * name,
		const char * type, size_t block_size,
		int flags, off_t total_size)
{
    ddb_device_t * dev;
    ddb_remote_t * descr;
    int mode;
    errno = EINVAL;
    if (! name || ! *name)
	return NULL;
    /* see if configuration tells us something */
    if ((flags & O_ACCMODE) == O_RDONLY)
	mode = DDB_MODE_RO;
    else if ((flags & O_CREAT) && (flags & O_EXCL))
	mode = DDB_MODE_EXCL;
    else
	mode = DDB_MODE_RW;
    devices_opened = 1;
    descr = ddb_read_configuration(name, type, mode);
    if (descr) {
	if (descr->block_size > 0) {
	    if (block_size > 0 && block_size != descr->block_size) {
		ddb_remote_free(descr);
		errno = EINVAL;
		return NULL;
	    }
	    block_size = descr->block_size;
	} else {
	    descr->block_size = block_size;
	}
	dev = ddb_device_open_remote(descr, flags, total_size);
	if (! dev) {
	    int sve = errno;
	    ddb_remote_free(descr);
	    errno = sve;
	}
	return dev;
    }
    /* was there an error in configuration? */
    if (errno) return NULL;
    /* then it must be a local device */
    return ddb_device_open_local(name, type, block_size, flags, total_size);
}

/* open a device which includes the devices provided as subdevices; this
 * function is normally called internally by ddb_device_open() but is
 * provided for plugins which may need to do a similar job; after a
 * successful call, the devices passed as arguments will be included in
 * the result device and must not be used again (or freed) by the caller */
ddb_device_t * ddb_device_open_multi(size_t extra, int ndevs,
		ddb_device_t * devs[ndevs], int flags)
{
    ddb_device_t * res;
    int i;
    if (ndevs < 0 || extra < 0) {
	errno = EINVAL;
	return NULL;
    }
    res = malloc(sizeof(ddb_device_t) + ndevs * sizeof(ddb_device_t *) + extra);
    if (! res) return NULL;
    res->ops = NULL;
    if (ndevs > 0) {
	res->info = devs[0]->info;
	res->info.devs = (void *)&res[1];
    } else {
	memset(&res->info, 0, sizeof(res->info));
	res->info.devs = NULL;
	res->info.mtime = 0;
    }
    res->info.name = "";
    res->info.type = NULL;
    res->info.multi_device = ndevs;
    res->info.blocks_allocated = 0;
    if (extra == 0)
	res->extra = NULL;
    else if (ndevs < 1)
	res->extra = &res[1];
    else
	res->extra = &res->info.devs[ndevs];
    for (i = 0; i < ndevs; i++) {
	res->info.devs[i] = devs[i];
	if (res->info.mtime < devs[i]->info.mtime)
	    res->info.mtime = devs[i]->info.mtime;
	if (devs[i]->info.is_remote)
	    res->info.is_remote = 1;
	res->info.blocks_allocated += devs[i]->info.blocks_allocated;
    }
    /* caller will likely need to add to res->info, and they will also need
     * to set up res->ops */
    return res;
}

/* perform a device-specific action */
int ddb_action(const char * name, const char * type, const char * action,
		const char * aux_name, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg)
{
    ddb_remote_t * descr;
    if (! name || ! *name || ! type || ! *type || ! action || ! *action) {
	errno = EINVAL;
	return -1;
    }
    /* see if configuration tells us something */
    descr = ddb_read_configuration(name, type, DDB_MODE_ACT);
    if (descr)
	return ddb_action_remote(descr, action, aux_name, aux_type,
				 freq, report, arg);
    /* was there an error in configuration? */
    if (errno) return -1;
    /* the only type with supported actions is "sequence" */
    if (ddb_type_is(type, DDB_TYPE_DIR))
	return ddb_action_dir(name, 0, action, aux_name, aux_type,
				 freq, report, arg);
    if (ddb_type_is(type, DDB_TYPE_PACK))
	return ddb_action_dir(name, 1, action, aux_name, aux_type,
				 freq, report, arg);
    errno = ENOENT;
    return -1;
}

#define has_op(op) (dev->ops && dev->ops->op)
#define check_op(op) if (! has_op(op)) { errno = EINVAL; return -1; }

/* read a single block */
int ddb_device_read(ddb_device_t * dev, off_t block, void * buffer, int flags) {
    ddb_block_t B;
    B.block = block;
    B.result = 0;
    B.error = 0;
    B.buffer = buffer;
    ddb_device_read_multi(dev, 1, &B, flags);
    if (B.result > 0)
	return 0;
    if (B.result == 0 && (flags & DDB_READ_MAYBE))
	return 1;
    errno = B.error;
    return -1;
}

/* read multiple blocks */
int ddb_device_read_multi(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks], int flags)
{
    check_op(read)
    return dev->ops->read(dev, nblocks, blocks, flags);
}

/* write a single block */
int ddb_device_write(ddb_device_t * dev, off_t block, const void * buffer) {
    ddb_block_t B;
    B.block = block;
    B.result = 0;
    B.error = 0;
    B.buffer = (void *)buffer;
    ddb_device_write_multi(dev, 1, &B);
    if (B.result > 0)
	return 0;
    errno = B.error;
    return -1;
}

/* write multiple blocks */
int ddb_device_write_multi(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks])
{
    check_op(write)
    return dev->ops->write(dev, nblocks, blocks);
}

/* return information about a device */
void ddb_device_info(ddb_device_t * dev, ddb_device_info_t * info) {
    *info = dev->info;
    info->type = dev->ops->type;
    if (has_op(info))
	dev->ops->info(dev, info);
}

/* internal function to produce info print */
int ddb_device_info_print_internal(ddb_device_t * dev, int level,
		int (*func)(int, const char *, void *), void * arg,
		int verbose)
{
    ddb_device_info_t info = dev->info;
    int rv, n;
    if (has_op(info))
	dev->ops->info(dev, &info);
#define prn(...) \
    rv = ddb_print_line(level, func, arg, __VA_ARGS__); \
    if (rv < 0) return rv;
    if (info.name) {
	prn("name: %s", info.name);
    }
#if 0
    switch (info.flags & O_ACCMODE) {
	case O_RDONLY : prn("access: read-only"); break;
	case O_WRONLY : prn("access: write-only"); break;
	case O_RDWR   : prn("access: read-write"); break;
	default       : prn("access: UNKNOWN"); break;
    }
#endif
    if (dev->ops->type) {
	prn("type: %s", dev->ops->type);
    }
    prn("block-size: %ld", (long)info.block_size);
    prn("total-size: %lld", (long long)info.total_size);
    prn("num-blocks: %lld", (long long)info.num_blocks);
    prn("blocks-present: %lld", (long long)info.blocks_present);
    prn("blocks-allocated: %lld", (long long)info.blocks_allocated);
    if (info.mtime) {
	char mtime[64];
	strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M:%S %Z",
		 localtime(&info.mtime));
	prn("modified: %lld (%s)", (long long)info.mtime, mtime);
    }
    prn("multi-device: %d", info.multi_device);
    for (n = 0; n < info.multi_device; n++) {
	prn("device: %d", n);
	rv = ddb_device_info_print(info.devs[n], level + 1,
				   func, arg, verbose);
	if (rv < 0) return rv;
    }
#undef prn
    return 0;
}

/* produces human-readable information about a device, possibly including
 * more information than the structure filled in by ddb_device_info() */
int ddb_device_info_print(ddb_device_t * dev, int level,
		int (*func)(int, const char *, void *), void * arg,
		int verbose)
{
    if (has_op(print))
	return dev->ops->print(dev, level, func, arg, verbose);
    return ddb_device_info_print_internal(dev, level, func, arg, verbose);
}

/* checks if a block is present in the device */
int ddb_device_has_block(ddb_device_t * dev, off_t block) {
    if (has_op(has_block))
	return dev->ops->has_block(dev, block);
    return 1;
}

/* all blocks the device can contain, including holes */
static ddb_blocklist_t * all_blocks(ddb_device_t * dev) {
    ddb_blocklist_t * res;
    int sve;
    res = ddb_blocklist_new();
    if (! res) return res;
    if (ddb_blocklist_add(res, 0, dev->info.num_blocks - 1) >= 0)
	return res;
    sve = errno;
    ddb_blocklist_free(res);
    errno = sve;
    return NULL;
}

/* return the list of blocks in the device; the caller will need to free
 * the list after using it */
ddb_blocklist_t * ddb_device_blocks(ddb_device_t * dev) {
    if (has_op(blocks))
	return dev->ops->blocks(dev);
    return all_blocks(dev);
}

/* return the list of blocks one needs to copy from the device; this
 * includes blocks not actually present in most cases */
ddb_blocklist_t * ddb_device_copy_blocks(ddb_device_t * dev) {
    if (has_op(range))
	return dev->ops->range(dev);
    return all_blocks(dev);
}

/* create a block list containing all blocks in the list provided which
 * are present in the device; this is the same as intersecting blocks
 * with ddb_device_blocks(dev) */
ddb_blocklist_t * ddb_device_has_blocks(ddb_device_t * dev,
		const ddb_blocklist_t * blocks)
{
    ddb_blocklist_t * r1, * r2;
    const ddb_blocklist_t * ls[2];
    int sve;
    if (has_op(has_blocks))
	return dev->ops->has_blocks(dev, blocks);
    r1 = ddb_device_blocks(dev);
    if (! r1) return NULL;
    ls[0] = r1;
    ls[1] = blocks;
    r2 = ddb_blocklist_intersect(2, ls);
    sve = errno;
    ddb_blocklist_free(r1);
    errno = sve;
    return r2;
}

/* call func(start, end, arg) for each disjoint range start..end of blocks
 * present in the device */
int ddb_device_block_iterate(ddb_device_t * dev,
		int (*func)(off_t, off_t, void *), void * arg)
{
    if (has_op(iterate))
	return dev->ops->iterate(dev, func, arg);
    return func(0, dev->info.num_blocks - 1, arg);
}

/* complete all pending I/O */
int ddb_device_flush(ddb_device_t * dev) {
    if (has_op(flush))
	return dev->ops->flush(dev);
    return 0;
}

/* if device supports sending a verbose report on close, send that out */
int ddb_device_report(ddb_device_t * dev,
		int (*func)(const char *, void *), void * arg)
{
    if (has_op(report))
	return dev->ops->report(dev, func, arg);
    return 0;
}

/* complete all pending I/O, then free and invalidate the device */
int ddb_device_close(ddb_device_t * dev) {
    int ok = 0, sve = 0, i;
    if (has_op(close)) {
	ok = dev->ops->close(dev);
	sve = errno;
    }
    for (i = 0; i < dev->info.multi_device; i++) {
	int c = ddb_device_close(dev->info.devs[i]);
	if (c < 0 && ok >= 0) {
	    sve = errno;
	    ok = c;
	}
    }
    free(dev);
    errno = sve;
    return ok;
}

/* fill device information */
void ddb_fill_info(ddb_device_t * dev, off_t total_size, size_t block_size,
		off_t blocks_present, time_t mtime, blkcnt_t allocated)
{
    off_t blocks_allocated = (off_t)512 * (off_t)allocated;
    dev->info.name = dev->name;
    dev->info.type = dev->ops ? dev->ops->type : NULL;
    dev->info.block_size = block_size;
    dev->info.num_blocks = total_size / block_size;
    if (total_size % block_size)
	dev->info.num_blocks++;
    dev->info.total_size = total_size;
    if (blocks_present < 0)
	dev->info.blocks_present = dev->info.num_blocks;
    else
	dev->info.blocks_present = blocks_present;
    dev->info.blocks_allocated = blocks_allocated / (off_t)block_size;
    if (blocks_allocated % (off_t)block_size)
	dev->info.blocks_allocated++;
    dev->info.mtime = mtime;
    dev->info.multi_device = 0;
    dev->info.is_remote = 0;
}

/* helper functions to read/write a full block */
int ddb_read_block(int fd, off_t where, size_t length,
		void * _buffer, int zerofill)
{
    unsigned char * buffer = _buffer;
    if (where >= 0 && lseek(fd, where, SEEK_SET) < 0) return 0;
    while (length > 0) {
	ssize_t nr = read(fd, buffer, length);
	if (nr < 0) return 0;
	if (nr == 0) {
	    if (zerofill) {
		memset(buffer, 0, length);
		return 1;
	    } else {
		errno = EBADF;
		return 0;
	    }
	}
	buffer += nr;
	length -= nr;
    }
    return 1;
}

int ddb_write_block(int fd, off_t where, size_t length, const void * _buffer) {
    unsigned const char * buffer = _buffer;
    if (where >= 0 && lseek(fd, where, SEEK_SET) < 0) return 0;
    while (length > 0) {
	ssize_t nr = write(fd, buffer, length);
	if (nr < 0) return 0;
	if (nr == 0) {
	    errno = EBADF;
	    return 0;
	}
	buffer += nr;
	length -= nr;
    }
    return 1;
}

/* generic info print function */
int ddb_print_line(int level, int (*func)(int, const char *, void *),
		void * arg, const char * fmt, ...)
{
    char line[MAX_INFO_LINE];
    va_list values;
    va_start(values, fmt);
    vsnprintf(line, sizeof(line), fmt, values);
    va_end(values);
    return func(level, line, arg);
}

/* generic progress print function - arg is expected to be a file */
void ddb_progress_print(void * arg, const char * line) {
    if (! arg) return;
    fprintf(arg, "%s", line);
    fflush(arg);
}

/* copy device operations */
void ddb_copy_ops(ddb_device_operations_t * dst, const ddb_device_t * cond,
		const ddb_device_operations_t * src)
{
#define __ops(name) dst->name = cond->ops->name ? src->name : NULL
    dst->type = cond->ops->type;
    __ops(read);
    __ops(write);
    __ops(info);
    __ops(print);
    __ops(has_block);
    __ops(blocks);
    __ops(range);
    __ops(has_blocks);
    __ops(iterate);
    __ops(flush);
    __ops(report);
    dst->close = src->close;
#undef __ops
}

