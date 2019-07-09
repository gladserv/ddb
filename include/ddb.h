/* ddb.h - device handling library for ddb
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

#ifndef __DDB__
#define __DDB__ 1

/* see ddb_blocklist_new(3) */

/* opaque type for a block list */
typedef struct ddb_blocklist_s ddb_blocklist_t;

/* create a new empty list */
ddb_blocklist_t * ddb_blocklist_new(void);

/* read a blocklist from a file; leaves the file position just after the
 * end of list */
ddb_blocklist_t * ddb_blocklist_load(FILE * F);

/* read a blocklist from a file; leaves the file position to end of file */
ddb_blocklist_t * ddb_blocklist_read(FILE * F);

/* add a range to a list */
int ddb_blocklist_add(ddb_blocklist_t * ls, off_t start, off_t end);

/* create a new list with all the blocks in ls where start <= block <= end */
ddb_blocklist_t * ddb_blocklist_sub(const ddb_blocklist_t * ls,
		off_t start, off_t end);

/* create a new list with all the blocks which appear in at least one
 * of the lists passed as arguments */
ddb_blocklist_t * ddb_blocklist_union(int nls,
		const ddb_blocklist_t * ls[nls]);

/* create a new list with all the blocks which appear in every one
 * of the lists passed as arguments */
ddb_blocklist_t * ddb_blocklist_intersect(int nls,
		const ddb_blocklist_t * ls[nls]);

/* save blocklist to a file; leaves file position after list */
int ddb_blocklist_save(const ddb_blocklist_t * ls, FILE * F);

/* print blocklist to a file; leaves file position after list */
int ddb_blocklist_print(const ddb_blocklist_t * ls, FILE * F);

/* number of blocks in list (a range start..end counts as
 * end-start+1 blocks) */
off_t ddb_blocklist_count(const ddb_blocklist_t * ls);

/* check if the list contain a block */
int ddb_blocklist_has(const ddb_blocklist_t * ls, off_t block);

/* call func(arg, start, end) for each disjoint range start..end of blocks
 * in list; if the list is empty, it never calls func */
int ddb_blocklist_iterate(const ddb_blocklist_t * ls,
		int (*func)(off_t, off_t, void *), void * arg);

/* free and invalidate the list */
void ddb_blocklist_free(ddb_blocklist_t * ls);

/* see ddb_device_open(3) */

/* opaque type for a device */
typedef struct ddb_device_s ddb_device_t;

/* flags values for ddb_device_configuration */
#define DDB_CONFIG_SYSTEM 0x00
#define DDB_CONFIG_USER   0x01
#define DDB_CONFIG_CLEAR  0x04

/* add a configuration file to the list which the program will search */
int ddb_device_configuration(int flags, const char * path);

/* open device from name; if flags specifies creation, then block_size
 * must be a positive number; if flags specifies an existing device and
 * the block size can be determined automatically, block_size can be 0,
 * but if it is positive it will be used to check consistency */
ddb_device_t * ddb_device_open(const char * name,
		const char * type, size_t block_size,
		int flags, off_t total_size);

/* open a device which includes the devices provided as subdevices; this
 * function is normally called internally by ddb_device_open() but is
 * provided for plugins which may need to do a similar job; after a
 * successful call, the devices passed as arguments will be included in
 * the result device and must not be used again (or freed) by the caller */
ddb_device_t * ddb_device_open_multi(size_t extra, int ndevs,
		ddb_device_t * devs[ndevs], int flags);

/* complete all pending I/O */
int ddb_device_flush(ddb_device_t * dev);

/* complete all pending I/O, then free and invalidate the device */
int ddb_device_close(ddb_device_t * dev);

/* open a "remote"-like device to a pipe */
ddb_device_t * ddb_device_pipe(FILE * F_in, FILE * F_out, pid_t pid,
		int flags, size_t block_size, off_t total_size,
		const char * name, const char * type);

/* if device supports sending a verbose report on close, send that out */
int ddb_device_report(ddb_device_t * dev,
		int (*func)(const char *, void *), void * arg);

/* see ddb_device_read(3) */

/* block specification for the "checksum" and "multi" functions below */
#define DDB_CHECKSUM_LENGTH 32

#define DDB_READ_BLOCK     0x0001
#define DDB_READ_CHECKSUM  0x0002
#define DDB_READ_DATA_MASK (DDB_READ_BLOCK|DDB_READ_CHECKSUM)
#define DDB_READ_ZEROFILL  0x0010
#define DDB_READ_MAYBE     0x0020

typedef struct ddb_block_s {
    off_t         block;
    int           result;
    int           error;
    void          * buffer;
} ddb_block_t;

/* read a single block */
int ddb_device_read(ddb_device_t * dev, off_t block, void * buffer, int flags);

/* read multiple blocks */
int ddb_device_read_multi(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks], int flags);

/* write a single block */
int ddb_device_write(ddb_device_t * dev, off_t block, const void * buffer); 

/* write multiple blocks */
int ddb_device_write_multi(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks]);

/* see ddb_device_info(3) */

/* device information structure */
typedef struct ddb_device_info_s {
    const char    * name;
    const char    * type;
    int           flags; /* same as open() etc */
    int           is_remote;
    size_t        block_size;
    off_t         total_size;
    off_t         num_blocks;
    off_t         blocks_present;
    off_t         blocks_allocated;
    time_t        mtime;
    int           multi_device;
    ddb_device_t  **devs;
} ddb_device_info_t;

/* return information about a device */
void ddb_device_info(ddb_device_t * dev, ddb_device_info_t * info);

/* produces human-readable information about a device, possibly including
 * more information than the structure filled in by ddb_device_info() */
int ddb_device_info_print(ddb_device_t * dev, int level,
		int (*func)(int, const char *, void *), void * arg,
		int verbose);

/* checks if a block is present in the device */
int ddb_device_has_block(ddb_device_t * dev, off_t block);

/* return the list of blocks in the device; the caller will need to free
 * the list after using it */
ddb_blocklist_t * ddb_device_blocks(ddb_device_t * dev);

/* return the list of blocks one needs to copy from the device; this
 * includes blocks not actually present in most cases */
ddb_blocklist_t * ddb_device_copy_blocks(ddb_device_t * dev);

/* create a block list containing all blocks in the list provided which
 * are present in the device; this is the same as intersecting blocks
 * with ddb_device_blocks(dev) */
ddb_blocklist_t * ddb_device_has_blocks(ddb_device_t * dev,
		const ddb_blocklist_t * blocks);

/* call func(start, end, arg) for each disjoint range start..end of blocks
 * present in the device */
int ddb_device_block_iterate(ddb_device_t * dev,
		int (*func)(off_t, off_t, void *), void * arg);

/* see ddb_action(3) */

/* perform a special action on a device; some devices may not support
 * special actions, see the documentation for the device type */
int ddb_action(const char * name, const char * type, const char * action,
		const char * aux_name, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg);

/* see ddb_copy(3) */

int ddb_copy_block(int block);

/* what to copy */
typedef struct {
    const char * src_name;
    ddb_device_t * src;
    const char * dst_name;
    ddb_device_t * dst;
    int write_dst;
    off_t total_size;
    off_t total_blocks;
    int block_size;
    const char * input_list;
    int max_passes;
    const char * checkpoint_file;
    int checkpoint_interval;
    void (*progress_function)(void *, const char *);
    void * progress_arg;
    int progress_interval;
    int progress_sleep;
    int extra_report;
    const char * machine_progress_file;
    int machine_progress_interval;
    const char * output_list;
    int output_each_pass;
    int flush_interval;
    int use_checksums;
    int skip_identical;
    const char * copied_list;
} ddb_copy_t;

/* copy data from src to dst and return 1 if all copied, 0 if some need to
 * be retried and -1 if a fatal error occurred */
int ddb_copy(ddb_copy_t * W);

/* plugin programs use this */

typedef struct ddb_plugin_s ddb_plugin_t;

/* receive connection greeting, open device and send back reply */
ddb_plugin_t * ddb_plugin_init(FILE * F_in, FILE * F_out);

/* wait for a request, execute it, return 0 if this was a close request,
 * 1 if there will be more requests, and -1 on error */
int ddb_plugin_run(ddb_plugin_t * pi);

/* deallocate anything allocated by ddb_plugin_init() */
void ddb_plugin_exit(ddb_plugin_t * pi);

/* when running a program to perform a device specific action, we ask to
 * return these special error codes; anything else will be considered a
 * generic error */
#define DDB_ACTION_OK        0
#define DDB_ACTION_ENOENT  248
#define DDB_ACTION_EISDIR  249
#define DDB_ACTION_ENOTDIR 250
#define DDB_ACTION_EACCES  251
#define DDB_ACTION_EPERM   252
#define DDB_ACTION_EROFS   253
#define DDB_ACTION_EINVAL  254

#endif /* __DDB__ */
