/* ddb-dir.c - "directory" access, with full and incremental images
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
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <ddb.h>
#include "ddb-private.h"

#define TS_LEN 24

static const char file_magic[] = "DDB SEQUENCE META\n";

static const char packed_magic[] = "DDB PACK";

/* information used to split a timestamp into components */
static struct {
    char separator;
    int length;
} ts_components[] = {
    {   0, 4 },
    { '-', 2 },
    { '-', 2 },
    { ':', 2 },
    { ':', 2 },
    { ':', 2 },
    { 0, 0 }
};

/* packed sequence header - on disk */
typedef struct {
    char magic[8];
    int32_t file_no;
    int32_t block_size;
    int64_t file_size;
    int64_t timestamp;
    int64_t total_size;
} __attribute((packed)) disk_header_t;

/* packed sequence header - in memory */
typedef struct {
    off_t start;
    off_t length;
    time_t timestamp;
} file_t;

typedef struct {
    int num_files;
    int added_file;
    file_t files[0];
} header_t;

/* data to be found in a metadata file */
typedef struct {
    off_t total_size;
    size_t block_size;
    time_t full_mtime;
    int save_meta;
    int dlen;
    int update_mtime;
    char * tname;
    header_t * packed;
    FILE * checksums;
    int packfd;
    char dname[0];
} meta_t;

static int read_header(int fd, meta_t * meta) {
    disk_header_t dh;
    off_t next_file, file_size, total_size;
    size_t block_size;
    int num_files, sve;
    if (! ddb_read_block(fd, 0, sizeof(dh), &dh, 0)) return 0;
    errno = EINVAL;
    if (strncmp(dh.magic, packed_magic, 8) != 0) return 0;
    block_size = be32toh(dh.block_size);
    if (block_size < MIN_BLOCK_SIZE) return 0;
    if (block_size > MAX_BLOCK_SIZE) return 0;
    if (block_size < sizeof(dh)) return 0;
    if (meta && meta->block_size > 0 && meta->block_size != block_size)
	return 0;
    total_size = be64toh(dh.total_size);
    if (total_size < 1) return 0;
    if (meta && meta->total_size > 0 && meta->total_size != total_size)
	return 0;
    if (be32toh(dh.file_no) != 0) return 0;
    if (be64toh(dh.timestamp) < 0) return 0;
    /* check_name just wants a yes/no so return a fake pointer to it for yes */
    if (! meta) return 1;
    meta->packed = NULL;
    /* OK, now count files and see if things add up */
    file_size = be64toh(dh.file_size);
    next_file = 0;
    num_files = 0;
    while (file_size >= 0) {
	num_files++;
	next_file += block_size + file_size;
	if (file_size % block_size)
	    next_file += block_size - (file_size % block_size);
	if (! ddb_read_block(fd, next_file, sizeof(dh), &dh, 0)) return 0;
	errno = EINVAL;
	if (num_files != be32toh(dh.file_no)) return 0;
	if (block_size != be32toh(dh.block_size)) return 0;
	if (be64toh(dh.timestamp) < 0) return 0;
	file_size = be64toh(dh.file_size);
	if (file_size < 0) break;
    }
    /* OK, we found num_files correct file headers */
    meta->packed = malloc(sizeof(header_t) + (num_files + 1) * sizeof(file_t));
    if (! meta->packed) return 0;
    meta->packed->num_files = num_files;
    meta->packed->added_file = 0;
    next_file = 0;
    for (num_files = 0; num_files < meta->packed->num_files; num_files++) {
	time_t timestamp;
	if (! ddb_read_block(fd, next_file, sizeof(dh), &dh, 0)) goto error;
	errno = EINVAL;
	if (num_files != be32toh(dh.file_no)) goto error;
	if (block_size != be32toh(dh.block_size)) goto error;
	timestamp = be64toh(dh.timestamp);
	if (timestamp < 0) goto error;
	file_size = be64toh(dh.file_size);
	if (file_size < 0) goto error;
	next_file += block_size;
	meta->packed->files[num_files].start = next_file;
	meta->packed->files[num_files].length = file_size;
	meta->packed->files[num_files].timestamp = timestamp;
	next_file += file_size;
	if (file_size % block_size)
	    next_file += block_size - (file_size % block_size);
    }
    meta->packed->files[num_files].start = next_file;
    meta->total_size = total_size;
    meta->block_size = block_size;
    meta->save_meta = 1;
    return 1;
error:
    sve = errno;
    free(meta->packed);
    meta->packed = 0;
    errno = sve;
    return 0;
}

static void parse_name(const char * name, int * has_type,
		       int * len, char ts[TS_LEN], int * last)
{
    const char * slash = rindex(name, '/');
    int namelen = strlen(name), tsp = 0;
    if (ts) ts[0] = 0;
    if (last) *last = 0;
    *len = namelen;
    if (! slash) return;
    *len = slash - name;
    slash++;
    if (! *slash) {
	*has_type = 1;
	if (ts) goto not_ts;
	return;
    }
    if (last) {
	if (strcmp(slash, "last") == 0) {
	    *last = 1;
	    return;
	}
    }
    if (ts) {
	int com = 0;
	while (ts_components[com].length && *slash) {
	    int len = 0, p;
	    if (ts_components[com].separator) {
		if (*slash != ts_components[com].separator) goto not_ts;
		slash++;
	    }
	    while (slash[len] &&
		   isdigit(slash[len]) &&
		   len < ts_components[com].length)
		len++;
	    if (len < 1) goto not_ts;
	    if (ts_components[com].separator)
		ts[tsp++] = ts_components[com].separator;
	    for (p = len; p < ts_components[com].length; p++)
		ts[tsp++] = '0';
	    for (p = 0; p < len; p++)
		ts[tsp++] = *slash++;
	    com++;
	}
	if (*slash) goto not_ts;
	while (ts_components[com].length) {
	    int p;
	    if (com) ts[tsp++] = ts_components[com].separator;
	    for (p = 0; p < ts_components[com].length; p++)
		ts[tsp++] = '9';
	    com++;
	}
	ts[tsp] = 0;
	return;
    }
not_ts:
    *len = namelen;
    if (ts) {
	int com = 0;
	tsp = 0;
	while (ts_components[com].length) {
	    int p;
	    if (ts_components[com].separator)
		ts[tsp++] = ts_components[com].separator;
	    for (p = 0; p < ts_components[com].length; p++)
		ts[tsp++] = '9';
	    com++;
	}
	ts[tsp] = 0;
    }
    return;
}

static int check_name(const char * name, int is_packed, int has_type,
		int creating, char dn[], int * len, char * ts, int * last)
{
    struct stat sbuff;
    int try;
    /* try parsing the name as is, and if that fails, and we are looking
     * for a timestamp, try also without the timestamp, in case we are
     * given a path looking like a timestamp */
    for (try = 0; try < (ts ? 2 : 1); try++) {
	parse_name(name, &has_type, len, try ? NULL : ts, last);
	strncpy(dn, name, *len);
	dn[*len] = 0;
	if (stat(dn, &sbuff) < 0) {
	    if (errno == ENOENT && creating && has_type) return 1;
	    continue;
	}
	if (is_packed) {
	    int fd, ok;
	    if (! S_ISREG(sbuff.st_mode)) continue;
	    fd = open(dn, O_RDONLY);
	    if (fd < 0) continue;
	    ok = read_header(fd, NULL);
	    close(fd);
	    if (! ok) continue;
	    return 2;
	}
	if (! S_ISDIR(sbuff.st_mode)) continue;
	/* now see that the required files are present */
	strcpy(dn + *len, "/meta");
	if (stat(dn, &sbuff) < 0) continue;
	if (! S_ISREG(sbuff.st_mode)) continue;
	strcpy(dn + *len, "/full");
	if (stat(dn, &sbuff) < 0) continue;
	if (! S_ISREG(sbuff.st_mode)) continue;
	/* looks like it may be a sequence */
	dn[*len] = 0;
	return 2;
    }
    return 0;
}

static int load_meta(const char * mf, meta_t * meta) {
    int maglen = strlen(file_magic);
    char magic[1 + maglen];
    long long block_size, total_size, mtime;
    FILE * F = fopen(mf, "r");
    int sve;
    if (! F) return 0;
    errno = EINVAL;
    if (fread(magic, maglen, 1, F) < 1) goto out;
    magic[maglen] = 0;
    if (strcmp(magic, file_magic) != 0) goto invalid;
    if (fscanf(F, "%lld %lld %lld\n", &block_size, &total_size, &mtime) < 3)
	goto invalid;
    if (block_size < MIN_BLOCK_SIZE || block_size > MAX_BLOCK_SIZE) goto invalid;
    if (total_size < 1) goto invalid;
    if (mtime < 1) goto invalid;
    meta->block_size = block_size;
    meta->total_size = total_size;
    meta->full_mtime = mtime;
    meta->update_mtime = 0;
    meta->save_meta = 0;
    fclose(F);
    return 1;
invalid:
    errno = EINVAL;
out:
    sve = errno;
    fclose(F);
    errno = sve;
    return 0;
}

static int save_meta(const char * mf, const meta_t * meta) {
    FILE * F = fopen(mf, "w");
    time_t mtime;
    int sve;
    if (! F) return 0;
    mtime = meta->update_mtime ? time(NULL) : meta->full_mtime;
    if (fprintf(F, "%s\n", file_magic) < 0) goto out;
    if (fprintf(F, "%lld %lld %lld\n",
		(long long)meta->block_size, (long long)meta->total_size,
		(long long)mtime) < 0)
	goto out;
    if (fclose(F) < 0) return 0;
    return 1;
out:
    sve = errno;
    fclose(F);
    errno = sve;
    return 0;
}

static int save_meta_packed(const meta_t * meta) {
    errno = ENOSYS; return -1; // XXX
}

int ddb_is_dir(const char * name, int is_packed, int has_type, int flags) {
    char dn[6 + strlen(name)];
    int len;
    if ((flags & O_ACCMODE) == O_RDONLY) {
	char ts[TS_LEN];
	return check_name(name, is_packed, has_type, 0, dn, &len, ts, NULL);
    }
    if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) {
	int last;
	return check_name(name, is_packed, has_type,
			  flags & O_CREAT, dn, &len, NULL, &last);
    }
    return 0;
}

/* filter incremental backup images */
static int scan_filter(const struct dirent * ent) {
    int len, com, pos;
    if (ent->d_name[0] == '\0' || ent->d_name[0] == '.')
	return 0;
    len = strlen(ent->d_name);
    if (len != 24) return 0;
    if (strncmp(ent->d_name, "incr-", 5) != 0) return 0;
    for (com = 0, pos = 5; ts_components[com].length; com++) {
	int d;
	if (com)
	    if (ent->d_name[pos++] != ts_components[com].separator)
		return 0;
	for (d = 0; d < ts_components[com].length; d++)
	    if (! isdigit(ent->d_name[pos++]))
		return 0;
    }
    return 1;
}

/* sort function; we need this to be independent on the user's locale
 * setting, to get a repeatable result, so can't use alphasort();
 * we also sort in reverse because it makes ddb_device_open_dir()
 * slightly easier */
static int scan_sort(const struct dirent ** a, const struct dirent ** b) {
    return strcmp((*b)->d_name, (*a)->d_name);
}

#define meta(dev) ((meta_t *)(dev)->extra)

static int dir_read(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks], int flags)
{
    ddb_block_t cb[nblocks];
    int who_has[nblocks], i, source[nblocks], res = 0;
    if (dev->info.multi_device < 1) {
	errno = EINVAL;
	return -1;
    }
    /* if we have a checksums file, shortcut a DDB_READ_CHECKSUM */
    if (meta(dev)->checksums && (flags & DDB_READ_CHECKSUM)) {
	for (i = 0; i < nblocks; i++) {
	    off_t block = blocks[i].block * (off_t)DDB_CHECKSUM_LENGTH;
	    int nr;
	    blocks[i].result = -1;
	    if (fseeko(meta(dev)->checksums, block, SEEK_SET) < 0) {
		blocks[i].error = errno;
		continue;
	    }
	    nr = fread(blocks[i].buffer, DDB_CHECKSUM_LENGTH,
		      1, meta(dev)->checksums);
	    if (nr <= 0) {
		blocks[i].error = nr == 0 ? EINVAL : errno;
		continue;
	    }
	    blocks[i].result = 1;
	    blocks[i].error = 0;
	    res++;
	}
	return res;
    }
    /* for each block, we need to determine who has it; the initial full
     * backup always has the block, but an incremental one can have a
     * newer version */
    for (i = 0; i < nblocks; i++) {
	off_t block = blocks[i].block;
	int try = dev->info.multi_device;
	who_has[i] = 0;
	while (try > 0) {
	    try--;
	    if (ddb_device_has_block(dev->info.devs[try], block)) {
		who_has[i] = try;
		break;
	    }
	}
    }
    /* now we try to get requests to the same device together */
    for (i = 0; i < dev->info.multi_device; i++) {
	int b, count = 0;
	for (b = 0; b < nblocks; b++) {
	    if (who_has[b] == i) {
		cb[count] = blocks[b];
		source[count] = b;
		count++;
	    }
	}
	if (count < 1) continue;
	res += ddb_device_read_multi(dev->info.devs[i], count, cb, flags);
	for (b = 0; b < count; b++)
	    blocks[source[b]] = cb[b];
    }
    return res;
}

static int dir_write(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks])
{
    int ok;
    if (dev->info.multi_device < 1) {
	errno = EINVAL;
	return -1;
    }
    /* we always write to the last device */
    ok = ddb_device_write_multi(dev->info.devs[dev->info.multi_device - 1],
				nblocks, blocks);
    if (ok < 0) return ok;
    /* if we have a checksum file, need to update it */
    if (meta(dev)->checksums) {
	unsigned char chk[DDB_CHECKSUM_LENGTH];
	int i;
	for (i = 0; i < nblocks; i++) {
	    off_t block = blocks[i].block * (off_t)DDB_CHECKSUM_LENGTH;
	    if (blocks[i].result < 1) continue;
	    ddb_checksum_block(blocks[i].buffer, meta(dev)->block_size, chk);
	    if (fseeko(meta(dev)->checksums, block, SEEK_SET) >= 0 &&
		fwrite(chk, DDB_CHECKSUM_LENGTH, 1, meta(dev)->checksums) > 0)
		    continue;
	    /* not updating the checksum counts as write error */
	    ok--;
	    blocks[i].result = -1;
	    blocks[i].error = errno;
	}
    }
    return ok;
}

static int dir_flush(ddb_device_t * dev) {
    int ok;
    if ((dev->info.flags & O_ACCMODE) == O_RDONLY) return 0;
    if (dev->info.multi_device < 1) {
	errno = EINVAL;
	return -1;
    }
    ok = ddb_device_flush(dev->info.devs[dev->info.multi_device - 1]);
    if (meta(dev)->checksums) {
	int sve = errno;
	if (fflush(meta(dev)->checksums) == EOF) {
	    if (ok < 0)
		errno = sve;
	    else
		ok = -1;
	}
    }
    return ok;
}

static int dir_close(ddb_device_t * dev) {
    int err = dir_flush(dev) < 0 ? errno : 0;
    if (err == 0 && meta(dev)->save_meta) {
	if (meta(dev)->packed) {
	    /* save metadata of last device */
	    if (! save_meta_packed(meta(dev)))
		err = errno;
	    if (close(meta(dev)->packfd) < 0 && err == 0)
		err = errno;
	} else {
	    strcpy(meta(dev)->tname + meta(dev)->dlen, "/.meta.tmp");
	    if (! save_meta(meta(dev)->tname, meta(dev)))
		err = errno;
	    if (err == 0) {
		strcpy(meta(dev)->dname + meta(dev)->dlen, "/meta");
		if (rename(meta(dev)->tname, meta(dev)->dname) < 0)
		    err = errno;
	    }
	    if (err != 0) unlink(meta(dev)->tname);
	    if (meta(dev)->checksums) {
		if (fclose(meta(dev)->checksums) < 0 && err == 0)
		    err = errno;
	    }
	}
    }
    if (err == 0) return 0;
    errno = err;
    return -1;
}

static const ddb_device_operations_t dir_ops = {
    .type       = DDB_TYPE_DIR,
    .read       = dir_read,
    .write      = dir_write,
    .info       = NULL,
    .print      = NULL,
    .has_block  = NULL,
    .blocks     = NULL,
    .range      = NULL, /* meaning all blocks */
    .has_blocks = NULL,
    .iterate    = NULL,
    .flush      = dir_flush,
    .report     = NULL,
    .close      = dir_close,
};

ddb_device_t * ddb_device_open_dir(const char * path, int is_packed,
		int has_type, size_t block_size, int flags, off_t total_size)
{
    char dn[30 + strlen(path)], ts[TS_LEN + 5];
    const char * tslimit = NULL;
    ddb_device_t ** sub = NULL, * dev;
    struct dirent ** namelist = NULL;
    meta_t meta;
    size_t extra;
    int len, sve, ndev = 0, nents = 0, last = 0, ok = 0;
    meta.packed = NULL;
    meta.packfd = -1;
    if ((flags & O_ACCMODE) == O_RDONLY) {
	strcpy(ts, "incr-");
	tslimit = ts;
	ok = check_name(path, is_packed, has_type, 0, dn, &len, ts + 5, NULL);
    } else if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) {
	ok = check_name(path, is_packed, has_type,
			flags & O_CREAT, dn, &len, NULL, &last);
    }
    if (! ok) {
	errno = EINVAL;
	return NULL;
    }
    meta.checksums = NULL;
    if (ok < 2) {
	/* need to create a new one */
	if (block_size < MIN_BLOCK_SIZE || total_size < 1) {
	    errno = EINVAL;
	    return NULL;
	}
	sub = malloc(sizeof(ddb_device_t *));
	if (! sub) return NULL;
	meta.block_size = block_size;
	meta.total_size = total_size;
	meta.full_mtime = time(NULL);
	meta.save_meta = 1;
	meta.update_mtime = 1;
	if (is_packed) {
	    if (block_size < sizeof(disk_header_t)) {
		errno = EINVAL;
		goto error;
	    }
	    meta.packfd = open(dn, O_RDWR | O_CREAT | O_EXCL, 0600);
	    if (meta.packfd < 0) goto error;
	    meta.packed->files[0].start = (off_t)block_size;
	    meta.packed->files[0].length = -1;
	    meta.packed->files[0].timestamp = meta.full_mtime;
	    meta.save_meta = 1;
	    sub[0] = ddb_device_open_image_fd(dn, meta.packfd,
					      block_size, O_RDWR, total_size,
					      (off_t)block_size);
	} else {
	    if (mkdir(dn, 0700) < 0) goto error;
	    strcpy(dn + len, "/full");
	    sub[0] = ddb_device_open_image(dn, 0, block_size,
					   O_RDWR | O_CREAT | O_EXCL, total_size);
	}
	if (! sub[0]) goto error;
	sub[0]->info.name = NULL;
	ndev = 1;
    } else {
	/* open an existing one */
	time_t now = time(NULL);
	int writing = (flags & O_ACCMODE) != O_RDONLY;
	int mknew = writing && ! last;
	int subflags;
	if (is_packed) {
	    if (block_size < sizeof(disk_header_t)) {
		errno = EINVAL;
		goto error;
	    }
	    meta.packfd = open(dn, O_RDWR);
	    if (meta.packfd < 0) goto error;
	    if (! read_header(meta.packfd, &meta)) goto error;
	} else {
	    strcpy(dn + len, "/meta");
	    if (! load_meta(dn, &meta)) goto error;
	    if (block_size > 0 && meta.block_size != block_size) goto invalid;
	    if (total_size > 0 && meta.total_size != total_size) goto invalid;
	    strcpy(dn + len, "/checksum");
	    meta.checksums = fopen(dn, writing ? "r+" : "r");
	    if (writing && ! meta.checksums) unlink(dn); /* it'll be out of date */
	    dn[len] = 0;
	    nents = scandir(dn, &namelist, scan_filter, scan_sort);
	}
	if (nents < 0) goto error;
	block_size = meta.block_size;
	total_size = meta.total_size;
	sub = malloc(sizeof(ddb_device_t *) * (1 + mknew + nents));
	if (! sub) goto error;
	subflags = (nents == 0 && writing && last) ? O_RDWR : O_RDONLY;
	if (is_packed) {
	    sub[0] = ddb_device_open_image_fd(dn, meta.packfd,
					      block_size, subflags, total_size,
					      meta.packed->files[0].start);
	} else {
	    strcpy(dn + len, "/full");
	    sub[0] = ddb_device_open_image(dn, 0, block_size,
					   subflags, total_size);
	}
	if (! sub[0]) goto error;
	sub[0]->info.name = NULL;
	ndev = 1;
	while (nents > 0) {
	    nents--;
	    subflags = (nents == 0 && writing && last) ? O_RDWR : O_RDONLY;
	    if (is_packed) {
		sub[ndev] =
		    ddb_device_open_image_fd(dn, meta.packfd,
					     block_size, subflags, total_size,
					     meta.packed->files[ndev].start);
	    } else {
		strcpy(dn + len + 1, namelist[nents]->d_name);
		free(namelist[nents]);
		if (tslimit && strcmp(dn + len + 1, tslimit) > 0) continue;
		sub[ndev] = ddb_device_open_image(dn, 2, block_size,
						  subflags, total_size);
	    }
	    if (! sub[ndev]) goto error;
	    sub[ndev]->info.name = NULL;
	    ndev++;
	}
	if (namelist) free(namelist);
	namelist = NULL;
	if (mknew) {
	    if (is_packed) {
		meta.packed->added_file = 1;
		meta.save_meta = 1;
		sub[ndev] =
		    ddb_device_open_image_fd(dn, meta.packfd,
					     block_size, O_RDWR, total_size,
					     meta.packed->files[ndev].start);
	    } else {
		strftime(dn + len, sizeof(dn) - len, "/incr-%Y-%m-%d:%H:%M:%S",
			 gmtime(&now));
		sub[ndev] = ddb_device_open_image(dn, 2, block_size,
						  O_RDWR | O_CREAT | O_EXCL,
						  total_size);
	    }
	    if (! sub[ndev]) goto error;
	    sub[ndev]->info.name = NULL;
	    ndev++;
	} else if (writing) {
	    meta.save_meta = 1;
	    meta.update_mtime = 0;
	}
    }
    /* OK, now we have opened all devices in the directory, so... */
    sub[0]->info.mtime = meta.full_mtime;
    extra = sizeof(meta_t) + 2 * len + 20;
    dev = ddb_device_open_multi(extra, ndev, sub, flags);
    if (! dev) goto error;
    free(sub);
    dev->ops = &dir_ops;
    meta.dlen = len;
    meta.tname = meta(dev)->dname + len + 6;
    dev->info.blocks_allocated++; /* for the metadata file */
    memcpy(dev->extra, &meta, sizeof(meta_t));
    dn[len] = 0;
    strcpy(meta(dev)->dname, dn);
    strcpy(meta(dev)->tname, dn);
    return dev;
invalid:
    errno = EINVAL;
error:
    sve = errno;
    if (sub) {
	while (ndev > 0) {
	    ndev--;
	    ddb_device_close(sub[ndev]);
	}
	free(sub);
    }
    if (namelist) {
	while (nents > 0) {
	    nents--;
	    free(namelist[nents]);
	}
	free(namelist);
    }
    if (meta.packed) free(meta.packed);
    if (meta.packfd >= 0) close(meta.packfd);
    errno = sve;
    return NULL;
}

/* join full and first incremental backup; this is never called on a packed
 * image */
static int action_join(const char * path, int freq,
		void (*report)(void *, const char *), void * arg)
{
    ddb_copy_t W;
    char dn[30 + strlen(path)], in[sizeof(dn)], tn[sizeof(dn)];
    meta_t meta;
    ddb_device_t * full = NULL, * incr = NULL;
    struct dirent ** namelist = NULL;
    int len, nents = 0, sve;
    int ok = check_name(path, 0, 1, 0, dn, &len, NULL, NULL);
    if (! ok || ok < 2) {
	errno = EINVAL;
	return -1;
    }
    /* scan directory for the first incremental backup, if any */
    nents = scandir(dn, &namelist, scan_filter, scan_sort);
    if (nents < 0) return -1;
    if (nents < 1) {
	free(namelist);
	errno = EINVAL;
	return -1;
    }
    /* get metadata */
    strcpy(dn + len, "/meta");
    if (! load_meta(dn, &meta)) goto error;
    /* get name of incremental device */
    strcpy(dn + len + 1, namelist[nents - 1]->d_name);
    strcpy(in, dn);
    /* and we might as well free namelist now */
    while (nents > 0) {
	nents--;
	free(namelist[nents]);
    }
    free(namelist);
    namelist = NULL;
    /* open incremental device readonly */
    incr = ddb_device_open_image(in, 2, meta.block_size,
				 O_RDONLY, meta.total_size);
    if (! incr) return -1;
    ddb_set_image_sparse(incr);
    /* and open the full device readwrite */
    strcpy(dn + len + 1, "full");
    full = ddb_device_open_image(dn, 0, meta.block_size,
				 O_RDWR, meta.total_size);
    if (! full) goto error;
    /* now copy data */
    W.src_name = "join";
    W.src = incr;
    W.dst_name = "full";
    W.dst = full;
    W.write_dst = 1;
    W.total_size = meta.total_size;
    W.total_blocks = incr->info.num_blocks;
    W.block_size = meta.block_size;
    W.input_list = NULL;
    W.max_passes = 2;
    W.checkpoint_file = NULL;
    W.checkpoint_interval = 0;
    W.progress_function = report;
    W.progress_arg = arg;
    W.progress_interval = freq;
    W.progress_sleep = 0;
    W.extra_report = 1;
    W.output_list = NULL;
    W.output_each_pass = 0;
    W.flush_interval = 120;
    W.use_checksums = 0;
    W.skip_identical = 0;
    W.copied_list = NULL;
    if (! ddb_copy(&W)) goto error;
    ok = ddb_device_close(full);
    full = NULL;
    if (ok < 0) goto error;
    /* update mtime to match the first incremental we are about to remove */
    meta.full_mtime = incr->info.mtime;
    meta.update_mtime = 0;
    strcpy(dn + len, "/meta");
    strcpy(tn, dn);
    strcpy(tn + len, "/.meta.tmp");
    ok = save_meta(tn, &meta);
    if (ok && rename(tn, dn) < 0) ok = 0;
    if (! ok) {
	sve = errno;
	unlink(tn);
	errno = sve;
	goto error;
    }
    ok = ddb_device_close(incr);
    incr = NULL;
    if (ok < 0) goto error;
    /* all data copied, remove first incremental */
    unlink(in);
    return 0;
error:
    sve = errno;
    if (full)
	ddb_device_close(full);
    if (incr)
	ddb_device_close(incr);
    if (namelist) {
	while (nents > 0) {
	    nents--;
	    free(namelist[nents]);
	}
	free(namelist);
    }
    errno = sve;
    return -1;
}


/* Add a "checksum" file to a sequence; this is never called on a packed
 * image */
static int action_checksum(const char * path, int freq,
		void (*report)(void *, const char *), void * arg)
{
    char dn[30 + strlen(path)], tn[sizeof(dn)], repbuff[80];
    ddb_block_t block;
    ddb_device_t * dev;
    FILE * C = NULL;
    time_t next_report = time(NULL) + freq;
    int len, rm_it = 0, sve;
    int ok = check_name(path, 0, 1, 0, dn, &len, NULL, NULL);
    if (! ok || ok < 2) {
	errno = EINVAL;
	return -1;
    }
    dn[len] = 0;
    dev = ddb_device_open_dir(dn, 0, 1, 0, O_RDONLY, 0);
    if (! dev) return -1;
    strcpy(tn, dn);
    strcpy(dn + len, "/checksum");
    strcpy(tn + len, "/.checksum.tmp");
    ok = -1;
    C = fopen(tn, "w");
    if (! C) goto out;
    rm_it = 1;
    if (meta(dev)->checksums) {
	/* we want to recalculate them, not to use the old ones... */
	fclose(meta(dev)->checksums);
	meta(dev)->checksums = NULL;
    }
    /* now read all blocks and checksum them */
    for (block.block = 0; block.block < dev->info.num_blocks; block.block++) {
	unsigned char chk[dev->info.block_size];
	time_t now;
	block.buffer = chk;
	if (dir_read(dev, 1, &block, DDB_READ_CHECKSUM) < 0)
	    goto out;
	if (block.result < 0) {
	    errno = block.error;
	    goto out;
	}
	if (block.result < 1) {
	    errno = EINVAL;
	    goto out;
	}
	if (fwrite(chk, DDB_CHECKSUM_LENGTH, 1, C) < 1) goto out;
	if (! report) continue;
	now = time(NULL);
	if (now < next_report) continue;
	next_report = now + freq;
	snprintf(repbuff, sizeof(repbuff), "\rchk %lld / %lld %.2f%%\r",
		 (long long)block.block + 1, (long long)dev->info.num_blocks,
		 100.0 * (double)(block.block + 1)
		       / (double)dev->info.num_blocks);
	report(arg, repbuff);
    }
    if (report) {
	snprintf(repbuff, sizeof(repbuff), "\rchk %lld / %lld %.2f%%\r",
		 (long long)block.block + 1, (long long)dev->info.num_blocks,
		 100.0 * (double)(block.block + 1)
		       / (double)dev->info.num_blocks);
	report(arg, repbuff);
    }
    if (fclose(C) < 0) goto out;
    C = NULL;
    if (rename(tn, dn) < 0) goto out;
    ok = 1;
    rm_it = 0;
out:
    sve = errno;
    if (report) report(arg, "\n");
    ddb_device_close(dev);
    if (C) fclose(C);
    if (rm_it) unlink(tn);
    errno = sve;
    return ok;
}

/* copy a sequence or packed */
static int action_copy(const char * path, int is_packed,
		const char * to_path, int to_packed, int freq,
		void (*report)(void *, const char *), void * arg)
{
    errno = ENOSYS; return -1; // XXX
}

/* perform a device action */
int ddb_action_dir(const char * path, int is_packed, const char * action,
		const char * aux_path, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg)
{
    errno = EINVAL;
    if (! action || ! *action) return -1;
    if (strcmp(action, "join") == 0) {
	if (is_packed) return -1;
	return action_join(path, freq, report, arg);
    }
    if (strcmp(action, "checksum") == 0) {
	if (is_packed) return -1;
	return action_checksum(path, freq, report, arg);
    }
    if (strcmp(action, "copy") == 0) {
	if (! aux_path || ! *aux_path) return -1;
	if (! aux_type || ! *aux_type) return -1;
	if (ddb_type_is(aux_type, DDB_TYPE_DIR))
	    return action_copy(path, is_packed, aux_path, 0, freq, report, arg);
	if (ddb_type_is(aux_type, DDB_TYPE_PACK))
	    return action_copy(path, is_packed, aux_path, 1, freq, report, arg);
	return -1;
    }
    errno = ENOENT;
    return -1;
}

