/* ddb-private.h - definitions for use by the ddb library
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

#ifndef __DDB_PRIVATE__
#define __DDB_PRIVATE__ 1

#define DEFAULT_BLOCK_SIZE 4096
#define MIN_BLOCK_SIZE 512
#define MAX_BLOCK_SIZE (16 * 1024 * 1024)
#if MIN_BLOCK_SIZE < DDB_CHECKSUM_LENGTH
#error Minimum block size too small
#endif

extern const char * const ddb_version;
extern const char * const ddb_licence;

/* operations on a device; these correspond to the device functions
 * in <ddb.h> but ddb-device.c may emulate any missing ones */
typedef struct {
    const char * type;
    int (*read)(ddb_device_t *, int, ddb_block_t [], int);
    int (*write)(ddb_device_t *, int, ddb_block_t []);
    void (*info)(ddb_device_t *, ddb_device_info_t *);
    int (*print)(ddb_device_t *, int,
	int (*)(int, const char *, void *), void *, int);
    int (*has_block)(ddb_device_t *, off_t);
    ddb_blocklist_t * (*blocks)(ddb_device_t *);
    ddb_blocklist_t * (*range)(ddb_device_t *);
    ddb_blocklist_t * (*has_blocks)(ddb_device_t *, const ddb_blocklist_t *);
    int (*iterate)(ddb_device_t *, int (*)(off_t, off_t, void *), void *);
    int (*report)(ddb_device_t *, int (*)(const char *, void *), void *);
    int (*flush)(ddb_device_t *);
    int (*close)(ddb_device_t *);
} ddb_device_operations_t;

/* opaque type for a device */
struct ddb_device_s {
    const char * name;
    const ddb_device_operations_t * ops;
    ddb_device_info_t info;
    void * extra;
    /* modules can have something following the end of this structure */
};

/* a list of tasks to prepare for a device connection */
typedef struct ddb_prepare_s ddb_prepare_t;
struct ddb_prepare_s {
    ddb_prepare_t * next;
    enum {
	ddb_prepare_load,
	ddb_prepare_run
    } type;
    int nargs;
    void * loaded;
    const char * program; /* or module */
    const char * args[0];
};

/* a list of tasks to connect to a device; first one which work will
 * be used */
typedef struct ddb_connect_s ddb_connect_t;
struct ddb_connect_s {
    ddb_connect_t * next;
    enum {
	ddb_connect_open,
	ddb_connect_tcp,
	ddb_connect_pipe,
	ddb_connect_call,
	ddb_connect_acall
    } type;
    int nargs;
    const char * module; /* or devname */
    const char * function; /* or devtype */
    const char * args[0]; /* function args / program + args / host + port */
};

/* a complete description of a "remote" device */
typedef struct {
    const char * name;
    ddb_prepare_t * prepare;
    ddb_connect_t * connect;
    ddb_prepare_t * close;
    int retry_max;
    int retry_delay;
    ddb_prepare_t * retry_prepare;
    ddb_connect_t * retry_connect;
    ddb_prepare_t * retry_close;
    int block_size;
} ddb_remote_t;

int ddb_devices_opened(void);

/* helper functions to open predefined device types */
int ddb_type_is(const char * type, const char * required);
#define DDB_TYPE_DIR "sequence"
#define DDB_TYPE_DEV "device"
#define DDB_TYPE_REG "image"
#define DDB_TYPE_META "meta"
#define DDB_TYPE_LVM "lvm"
#define DDB_TYPE_PACK "packed"
#define DDB_TYPE_ERR "error"
int ddb_is_dir(const char * name, int is_packed, int has_type, int flags);
ddb_device_t * ddb_device_open_dir(const char * path, int is_packed,
		int has_type, size_t block_size, int flags, off_t total_size);
ddb_device_t * ddb_device_open_image(const char * path,
		int want_metadata, size_t block_size,
		int flags, off_t total_size);
ddb_device_t * ddb_device_open_image_fd(const char * path, int fd,
		size_t block_size, int flags, off_t total_size, off_t offset);
void ddb_set_image_sparse(ddb_device_t * dev);
void ddb_image_placement(const ddb_device_t * dev, off_t * start, off_t * end);
ddb_device_t * ddb_device_open_remote(ddb_remote_t * descr,
		int flags, off_t total_size);
int ddb_is_lvm(const char * name, int flags);
ddb_device_t * ddb_device_open_lvm(const char * name,
		size_t block_size, int flags, off_t total_size);
ddb_device_t * ddb_device_open_error(const char * name,
		size_t block_size, int flags, off_t total_size);

/* and to perform actions */
int ddb_action_dir(const char * path, int is_packed, const char * action,
		const char * aux_path, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg);
int ddb_action_remote(ddb_remote_t * descr, const char * action,
		const char * aux_name, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg);

/* like ddb_device_open() but does not read configuration directories
 * to determine how to open a device */
ddb_device_t * ddb_device_open_local(const char * name,
		const char * type, size_t block_size,
		int flags, off_t total_size);

/* a remote device may need this after it finishes */
void ddb_remote_free(ddb_remote_t * descr);

/* when opening a device this may be required */
#define DDB_MODE_RO   1
#define DDB_MODE_RW   2
#define DDB_MODE_EXCL 3
#define DDB_MODE_ACT  4
ddb_remote_t * ddb_read_configuration(const char * name,
		const char * type, int mode);
void ddb_fill_info(ddb_device_t * dev, off_t total_size, size_t block_size,
		off_t blocks_present, time_t mtime, blkcnt_t allocated);

/* functions to read/write a whole block from a file/pipe/socket; some
 * OSs need that, some don't */
int ddb_read_block(int fd, off_t where, size_t length,
		void * buffer, int zerofill);
int ddb_write_block(int fd, off_t where, size_t length, const void * buffer);

/* and to calculate checksums... */
void ddb_checksum_block(const void * buffer, size_t length,
		unsigned char checksum[DDB_CHECKSUM_LENGTH]);
int ddb_checksum_check(const void * buffer, size_t length,
		const unsigned char checksum[DDB_CHECKSUM_LENGTH]);

/* generic info print function */
int ddb_print_line(int level, int (*func)(int, const char *, void *),
		void * arg, const char * fmt, ...);

/* generic progress print function - arg is expected to be a file */
void ddb_progress_print(void * arg, const char * line);

/* configuration defaults */
const char * ddb_default_config(void);
const char * ddb_override_config(void);
const char * ddb_default_sysconfig(void);
const char * ddb_override_sysconfig(void);

/* used to parse command-line options */
extern const char * progname;
int ddb_store_int(int opt, const char * a, int * var, int min, int max);
int ddb_store_cfg(int * clr, int which);

/* copy device operations */
void ddb_copy_ops(ddb_device_operations_t * dst, const ddb_device_t * cond,
		const ddb_device_operations_t * src);

/* internal function to produce info print */
int ddb_device_info_print_internal(ddb_device_t * dev, int level,
		int (*func)(int, const char *, void *), void * arg,
		int verbose);

#endif /* __DDB_PRIVATE__ */
