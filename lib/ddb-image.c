/* ddb-image.c - "image" access (image file or real device) for ddb
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

#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <time.h>
#include <ddb.h>
#include "ddb-private.h"

static const char magic[8] = "DDB META";
static const int version_min = 0;
static const int version_max = 0;

/* image header - on disk */
typedef struct {
    char magic[8];
    int64_t total_size;
    int64_t blocks_present;
    int64_t data_end;
    int32_t block_size;
    int32_t version;
    int64_t mtime;
    int64_t metadata;
    char _padding[8];
} __attribute((packed)) disk_header_t;

/* image header - in memory */
typedef struct {
    off_t total_size;
    off_t total_blocks;
    off_t blocks_present;
    off_t data_end;
    off_t metadata;
    size_t block_size;
    size_t last_block;
    time_t mtime;
    int meta_size;
} header_t;

/* metadata summary cache */
typedef struct metadata_summary_s metadata_summary_t;
struct metadata_summary_s {
    metadata_summary_t * next;
    off_t pos;
    off_t first;
    off_t last;
};

/* metadata block header - on disk */
typedef struct {
    int64_t next;
    int32_t count;
} __attribute((packed)) disk_metadata_header_t;

/* metadata block header - in memory */
typedef struct {
    metadata_summary_t * summary;
    off_t next;
    int count;
} metadata_header_t;

/* metadata block data - on disk */
typedef struct {
    int64_t start;
    int64_t end;
    int64_t pos;
} __attribute((packed)) disk_metadata_data_t;

/* metadata block data - in memory */
typedef struct {
    off_t start;
    off_t end;
    off_t pos;
} metadata_data_t;

/* extra information for this device */
typedef struct {
    int fd;
    int has_metadata;
    int flush_file_header;
    int flush_metadata;
    off_t offset;
    header_t header;
    metadata_header_t metadata_header;
    metadata_data_t * metadata_data;
    metadata_summary_t * metadata_summary;
    unsigned char * metadata_buffer;
    unsigned char * block_buffer;
    unsigned char _buffer[0];
} image_t;

/* used to print device information */
typedef struct {
    int level;
    int (*func)(int, const char *, void *);
    void * arg;
} infoprint_t;

/* used to generate block lists */
typedef struct {
    const ddb_blocklist_t * list;
    ddb_blocklist_t * res;
} makelist_t;

/* convert in-memory image header to on-disk */
static void header_encode(const header_t * mh, disk_header_t * dh) {
    memset(dh, 0, sizeof(*dh));
    strncpy(dh->magic, magic, sizeof(dh->magic));
    dh->version = htobe32(version_max);
    dh->total_size = htobe64(mh->total_size);
    dh->blocks_present = htobe64(mh->blocks_present);
    dh->data_end = htobe64(mh->data_end);
    dh->block_size = htobe32(mh->block_size);
    dh->mtime = htobe64(mh->mtime);
    dh->metadata = htobe64(mh->metadata);
}

/* fill derived image header fields */
static void fill_derived_header(header_t * mh) {
    mh->total_blocks = mh->total_size / mh->block_size;
    mh->last_block = mh->total_size % mh->block_size;
    if (mh->last_block)
	mh->total_blocks++;
    else
	mh->last_block = mh->block_size;
    /* number of data elements in a metadata block */
    mh->meta_size = (mh->block_size - sizeof(disk_metadata_header_t))
		  / sizeof(disk_metadata_data_t);
}

/* convert on-disk image header to in-memory */
static int header_decode(const disk_header_t * dh, header_t * mh,
		off_t file_size)
{
    int v;
    if (strncmp(dh->magic, magic, 8) != 0) return 0;
    v = be32toh(dh->version);
    if (v < version_min || v > version_max) return 0;
    mh->total_size = be64toh(dh->total_size);
    if (mh->total_size < 1) return 0;
    mh->block_size = be32toh(dh->block_size);
    if (mh->block_size < 512) return 0;
    mh->blocks_present = be64toh(dh->blocks_present);
    if (mh->blocks_present < 0) return 0;
    mh->data_end = be64toh(dh->data_end);
    if (mh->data_end < 0) return 0;
    if (mh->data_end > file_size) return 0;
    mh->metadata = be64toh(dh->metadata);
    if (mh->metadata < 0) return 0;
    if (mh->metadata >= file_size) return 0;
    mh->mtime = be64toh(dh->mtime);
    fill_derived_header(mh);
    if (mh->blocks_present > mh->total_blocks) return 0;
    return 1;
}

/* write current buffered metadata to disk */
static int write_metadata(image_t * img) {
    int ok = 1, err = 0;
    if (img->flush_metadata) {
	disk_metadata_header_t dmh;
	unsigned char * dest = img->metadata_buffer;
	int n;
	dmh.count = htobe32(img->metadata_header.count);
	dmh.next = htobe64(img->metadata_header.next);
	memcpy(dest, &dmh, sizeof(dmh));
	dest += sizeof(dmh);
	for (n = 0; n < img->metadata_header.count; n++) {
	    disk_metadata_data_t dmd;
	    dmd.start = htobe64(img->metadata_data[n].start);
	    dmd.end = htobe64(img->metadata_data[n].end);
	    dmd.pos = htobe64(img->metadata_data[n].pos);
	    memcpy(dest, &dmd, sizeof(dmd));
	    dest += sizeof(dmd);
	}
	n = dest - img->metadata_buffer;
	if (n < img->header.block_size)
	    memset(dest, 0, img->header.block_size - n);
	if (ddb_write_block(img->fd, img->metadata_header.summary->pos,
			    img->header.block_size, img->metadata_buffer)) {
	    img->flush_metadata = 0;
	} else {
	    err = errno;
	    ok = 0;
	}
    }
    if (img->flush_file_header) {
	disk_header_t dh;
	size_t pos = img->offset;
	header_encode(&img->header, &dh);
	if (ddb_write_block(img->fd, pos, sizeof(dh), &dh)) {
	    img->flush_file_header = 0;
	} else {
	    err = errno;
	    ok = 0;
	}
	pos += sizeof(dh);
	if (pos < img->header.block_size) {
	    char buffer[4096];
	    memset(buffer, 0, sizeof(buffer));
	    while (pos < img->header.block_size) {
		size_t diff = img->header.block_size - pos;
		if (diff > sizeof(buffer)) diff = sizeof(buffer);
		ddb_write_block(img->fd, pos, diff, buffer);
		pos += diff;
	    }
	}
    }
    errno = err;
    return ok;
}

/* read metadata block at position */
static int read_metadata(image_t * img, metadata_summary_t * summary) {
    disk_metadata_header_t dmh;
    const unsigned char * src = img->metadata_buffer;
    int n;
    if (img->metadata_header.summary && img->metadata_header.summary == summary)
	return 1;
    if (! write_metadata(img)) return 0;
    img->metadata_header.summary = NULL;
    if (! ddb_read_block(img->fd, summary->pos, img->header.block_size,
			 img->metadata_buffer, 0))
	return 0;
    memcpy(&dmh, src, sizeof(dmh));
    src += sizeof(dmh);
    img->metadata_header.count = be32toh(dmh.count);
    if (img->metadata_header.count < 1) {
	errno = EINVAL;
	return 0;
    }
    img->metadata_header.next = be64toh(dmh.next);
    for (n = 0; n < img->metadata_header.count; n++) {
	disk_metadata_data_t dmd;
	memcpy(&dmd, src, sizeof(dmd));
	src += sizeof(dmd);
	img->metadata_data[n].start = be64toh(dmd.start);
	img->metadata_data[n].end = be64toh(dmd.end);
	img->metadata_data[n].pos = be64toh(dmd.pos);
    }
    img->metadata_header.summary = summary;
    return 1;
}

#define image(dev) ((image_t *)(dev)->extra)

/* free metadata summary cache */
static void free_summary(image_t * img) {
    while (img->metadata_summary) {
	metadata_summary_t * going = img->metadata_summary;
	img->metadata_summary = going->next;
	free(going);
    }
}

/* read metadata summary cache */
static int read_summary(image_t * img) {
    off_t pos = img->header.metadata;
    metadata_summary_t * curr = NULL;
    img->metadata_summary = NULL;
    while (pos > 0) {
	disk_metadata_header_t dmh;
	disk_metadata_data_t dmd;
	const unsigned char * src = img->metadata_buffer;
	metadata_summary_t * prev = curr;
	int count;
	if (! ddb_read_block(img->fd, pos, img->header.block_size,
			     img->metadata_buffer, 0))
	    return 0;
	curr = malloc(sizeof(metadata_summary_t));
	if (! curr) return 0;
	curr->next = NULL;
	memcpy(&dmh, src, sizeof(dmh));
	src += sizeof(dmh);
	curr->pos = pos;
	pos = be64toh(dmh.next);
	count = be32toh(dmh.count);
	memcpy(&dmd, src, sizeof(dmd));
	curr->first = be64toh(dmd.start);
	if (count > 1) {
	    src += (count - 1) * sizeof(dmd);
	    memcpy(&dmd, src, sizeof(dmd));
	}
	curr->last = be64toh(dmd.end);
	if (prev)
	    prev->next = curr;
	else
	    img->metadata_summary = curr;
    }
    return 1;
}

/* calculate block position given metadata information */
static off_t block_position(const metadata_data_t * md, off_t block,
		size_t block_size)
{
    block -= md->start;
    block *= (off_t)block_size;
    return block + md->pos;
}

/* see if an image with metadata contains a block, and return where we
 * can find it */
static off_t meta_block_position(ddb_device_t * dev, off_t block) {
    int n;
    /* the normal assumption is sequential access from first to last
     * block, because that's what we have when we copy etc; so... */
    if (! image(dev)->metadata_header.summary ||
	block < image(dev)->metadata_header.summary->first ||
	block > image(dev)->metadata_header.summary->last)
    {
	/* we don't have the right cached block, use the summary cache
	 * to find one */
	metadata_summary_t * summ = image(dev)->metadata_summary;
	if (! summ) {
	    if (! read_summary(image(dev)))
		return -1;
	    summ = image(dev)->metadata_summary;
	    if (! summ) return 0; /* file is really empty */
	}
	if (image(dev)->metadata_header.summary &&
	    block > image(dev)->metadata_header.summary->last)
		summ = image(dev)->metadata_header.summary;
	while (summ) {
	    /* if we've gone past then the block is not present */
	    if (block < summ->first) return 0;
	    if (block <= summ->last) break;
	    summ = summ->next;
	}
	if (! summ) return 0;
	/* we've found where it would be, read that metadata */
	if (! write_metadata(image(dev))) return -1;
	if (! read_metadata(image(dev), summ)) return -1;
    }
    /* OK, we know the block is in the range covered by the cached metadata
     * block, find it if it's present */
    /* now search the correct block */
    for (n = 0; n < image(dev)->metadata_header.count; n++)
	if (image(dev)->metadata_data[n].start <= block &&
	    image(dev)->metadata_data[n].end >= block)
		return block_position(&image(dev)->metadata_data[n],
				      block, dev->info.block_size);
    /* it is not there */
    return 0;
}

/* if possible, extend a metadata range to include a new block */
static int extend_metadata(image_t * img, off_t block, off_t pos) {
    int n;
    for (n = 0; n < img->metadata_header.count; n++) {
	if (img->metadata_data[n].end + 1 != block)
	    continue;
	if (pos != block_position(&img->metadata_data[n],
				  block, img->header.block_size))
	    continue;
	img->metadata_data[n].end++;
	if (n + 1 == img->metadata_header.count)
	    img->metadata_header.summary->last = img->metadata_data[n].end;
	img->flush_metadata = 1;
	return 1;
    }
    return 0;
}

/* add a block; if we can, append to an existing metadata range, if not
 * create a new range; as for the data itself it can only extend the file
 * so we know where it'll be... */
static off_t add_new_block(ddb_device_t * dev, off_t block) {
    metadata_summary_t * summ, * new;
    off_t new_pos = image(dev)->header.data_end;
    int n;
    /* first check if we can just extend the cached metadata block */
    if (image(dev)->metadata_header.summary &&
	block >= image(dev)->metadata_header.summary->first &&
	block <= image(dev)->metadata_header.summary->last + 1 &&
	extend_metadata(image(dev), block, new_pos))
	    goto extend_file;
    /* search through the summary cache to find one we can use */
    summ = image(dev)->metadata_summary;
    if (! summ) {
	if (! read_summary(image(dev)))
	    return -1;
	summ = image(dev)->metadata_summary;
	if (! summ) {
	    summ = malloc(sizeof(metadata_summary_t));
	    if (! summ) return -1;
	    summ->next = NULL;
	    summ->first = block;
	    summ->last = block;
	    summ->pos = new_pos;
	    image(dev)->metadata_summary = summ;
	    image(dev)->metadata_header.summary = summ;
	    image(dev)->metadata_header.count = 0;
	    image(dev)->metadata_header.next = 0;
	    image(dev)->header.metadata = new_pos;
	    image(dev)->flush_file_header = 1;
	    image(dev)->flush_metadata = 1;
	    new_pos += dev->info.block_size;
	    goto extend_block;
	}
    }
    if (image(dev)->metadata_header.summary &&
	block > image(dev)->metadata_header.summary->last)
	    summ = image(dev)->metadata_header.summary;
    while (summ && block > summ->last && summ->next)
	summ = summ->next;
    /* we found one, read it */
    if (! write_metadata(image(dev))) return -1;
    if (! read_metadata(image(dev), summ)) return -1;
    /* do we have space? */
    if (image(dev)->metadata_header.count < image(dev)->header.meta_size)
	goto extend_block;
    /* maybe it can go at the start of the next block */
    if (block > summ->last && summ->next && block <= summ->next->last) {
	summ = summ->next;
    if (! write_metadata(image(dev))) return -1;
	if (! write_metadata(image(dev))) return -1;
	if (! read_metadata(image(dev), summ)) return -1;
	if (image(dev)->metadata_header.count < image(dev)->header.meta_size)
	    goto extend_block;
    }
    /* we don't have space in the current block */
    new = malloc(sizeof(metadata_summary_t));
    if (! new) return -1;
    new->next = summ->next;
    new->pos = new_pos;
    summ->next = new;
    /* we don't have space in the current block; if we were trying to add
     * a data block after the end of it, just start a new empty block */
    if (block > summ->last) {
	/* adding data after the end of this metadata block: we create
	 * a new block after it */
	off_t next = image(dev)->metadata_header.next;
	summ = new;
	summ->first = summ->last = block;
	image(dev)->metadata_header.next = new_pos;
	image(dev)->flush_metadata = 1;
	if (! write_metadata(image(dev))) return 1;
	image(dev)->metadata_header.summary = summ;
	image(dev)->metadata_header.count = 0;
	image(dev)->metadata_header.next = next;
	image(dev)->flush_metadata = 1;
	new_pos += dev->info.block_size;
    } else {
	/* adding a data block in the middle of a full metadata block:
	 * split the metadata block in half */
	metadata_data_t * data = image(dev)->metadata_data;
	int count = image(dev)->metadata_header.count, ok;
	int n1 = count / 2, n2 = count - n1;
	new->last = summ->last;
	summ->last = data[n1 - 1].end;
	new->first = data[n1].start;
	/* write new second half at end of file */
	image(dev)->metadata_header.summary = new;
	image(dev)->metadata_header.count = n2;
	image(dev)->metadata_data = &data[n1];
	image(dev)->flush_metadata = 1;
	ok = write_metadata(image(dev));
	image(dev)->metadata_header.summary = summ;
	image(dev)->metadata_header.count = count;
	image(dev)->metadata_data = data;
	if (! ok) {
	    image(dev)->flush_metadata = 1;
	    return -1;
	}
	/* and now overwrite original block with first half */
	image(dev)->metadata_header.next = new_pos;
	image(dev)->metadata_header.count = n1;
	image(dev)->flush_metadata = 1;
	if (! write_metadata(image(dev))) return 1;
	/* if we need the second block, reread it */
	if (image(dev)->metadata_data[n1].end < block)
	    if (! read_metadata(image(dev), new))
		return -1;
	/* data will take the block after the new metadata block */
	new_pos += dev->info.block_size;
    }
extend_block:
    /* we have space to add 1 range to this metadata block */
    n = image(dev)->metadata_header.count++;
    while (n > 0 && image(dev)->metadata_data[n - 1].end > block) {
	image(dev)->metadata_data[n] = image(dev)->metadata_data[n - 1];
	n--;
    }
    image(dev)->metadata_data[n].start = block;
    image(dev)->metadata_data[n].end = block;
    image(dev)->metadata_data[n].pos = new_pos;
    image(dev)->metadata_header.summary->first =
	image(dev)->metadata_data[0].start;
    image(dev)->metadata_header.summary->last =
	image(dev)->metadata_data[image(dev)->metadata_header.count - 1].end;
    image(dev)->flush_metadata = 1;
extend_file:
    /* we have a place, and even some metadata pointing at it */
    image(dev)->header.data_end = new_pos + dev->info.block_size;
    image(dev)->header.blocks_present++;
    image(dev)->flush_file_header = 1;
    /* try to make sure the file size matches the header. */
    if (ftruncate(image(dev)->fd, image(dev)->header.data_end) < 0)
	return -1;
    return new_pos;
}

/* see if an image with metadata contains a block */
static int meta_has_block(ddb_device_t * dev, off_t block) {
    off_t pos = meta_block_position(dev, block);
    if (pos < 0) return -1;
    if (pos == 0) return 0;
    return 1;
}

/* read blocks, with or without metadata */
static int image_read(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks], int flags)
{
    int n, ok = 0, zf = flags & DDB_READ_ZEROFILL;
    int ck = (flags & DDB_READ_DATA_MASK) == DDB_READ_CHECKSUM;
    for (n = 0; n < nblocks; n++) {
	size_t len = image(dev)->header.block_size;
	off_t pos;
	if (blocks[n].block == image(dev)->header.total_blocks - 1) {
	    len = image(dev)->header.last_block;
	    /* if last block is shorter than the rest, zero fill the
	     * bit which we won't read, to help comparing blocks */
	    if (len < image(dev)->header.block_size) {
		char * dst = blocks[n].buffer;
		memset(dst + len, 0, image(dev)->header.block_size - len);
	    }
	}
	if (image(dev)->has_metadata) {
	    pos = meta_block_position(dev, blocks[n].block);
	    if (pos < 0) goto read_failed;
	    if (pos == 0) {
		memset(blocks[n].buffer, 0, len);
		goto read_ok;
	    }
	} else {
	    pos = blocks[n].block * (off_t)image(dev)->header.block_size
		+ image(dev)->offset;
	}
	if (ddb_read_block(image(dev)->fd, pos, len, blocks[n].buffer, zf)) {
	read_ok:
	    ok++;
	    blocks[n].error = 0;
	    blocks[n].result = 1;
	    if (ck) {
		unsigned char checksum[DDB_CHECKSUM_LENGTH];
		ddb_checksum_block(blocks[n].buffer, len, checksum);
		memcpy(blocks[n].buffer, checksum, DDB_CHECKSUM_LENGTH);
	    }
	} else {
	read_failed:
	    blocks[n].error = errno;
	    blocks[n].result = -1;
	}
    }
    return ok;
}

/* write blocks, with or without metadata */
static int image_write(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[nblocks])
{
    int n, ok = 0;
    for (n = 0; n < nblocks; n++) {
	size_t len = image(dev)->header.block_size;
	off_t pos;
	if (blocks[n].block == image(dev)->header.total_blocks - 1)
	    len = image(dev)->header.last_block;
	if (image(dev)->has_metadata) {
	    pos = meta_block_position(dev, blocks[n].block);
	    if (pos < 0) goto write_failed;
	    if (pos == 0) {
		/* block not present, we must add it... */
		pos = add_new_block(dev, blocks[n].block);
		if (pos < 0) goto write_failed;
	    }
	} else {
	    pos = blocks[n].block * (off_t)image(dev)->header.block_size
		+ image(dev)->offset;
	}
	if (ddb_write_block(image(dev)->fd, pos, len, blocks[n].buffer)) {
	    ok++;
	    blocks[n].error = 0;
	    blocks[n].result = 1;
	} else {
	write_failed:
	    blocks[n].error = errno;
	    blocks[n].result = -1;
	}
    }
    return ok;
}

/* iterate over all blocks present in device */
static int meta_iterate(ddb_device_t * dev,
		int (*func)(off_t, off_t, void *), void * arg)
{
    metadata_summary_t * summ = image(dev)->metadata_summary;
    off_t start = -1, end = -1;
    if (! write_metadata(image(dev))) return -1;
    if (! summ) {
	if (! read_summary(image(dev)))
	    return -1;
	summ = image(dev)->metadata_summary;
    }
    while (summ) {
	int n;
	if (! read_metadata(image(dev), summ)) return -1;
	for (n = 0; n < image(dev)->metadata_header.count; n++) {
	    off_t rs = image(dev)->metadata_data[n].start;
	    off_t re = image(dev)->metadata_data[n].end;
	    if (end >= 0) {
		int ok;
		if (end + 1 == rs) {
		    end = re;
		    continue;
		}
		ok = func(start, end, arg);
		if (ok < 0) return ok;
	    }
	    start = rs;
	    end = re;
	}
	summ = summ->next;
    }
    if (end < 0) return 0;
    return func(start, end, arg);
}

/* print information about a range of blocks */
static int meta_print_range(off_t start, off_t end, void * _prn) {
    infoprint_t * prn = _prn;
    if (end == start)
	return ddb_print_line(prn->level, prn->func, prn->arg,
			      "block-range: %lld", (long long)start);
    else
	return ddb_print_line(prn->level, prn->func, prn->arg,
			      "block-range: %lld:%lld",
			      (long long)start, (long long)end);
}

static void meta_info(ddb_device_t * dev, ddb_device_info_t * info) {
    info->blocks_present = image(dev)->header.blocks_present;
}

/* print (if verbose) a complete list of blocks present */
static int meta_print(ddb_device_t * dev, int level,
		int (*func)(int, const char *, void *), void * arg,
		int verbose)
{
    infoprint_t prn;
    int ok = ddb_device_info_print_internal(dev, level, func, arg, verbose);
    if (ok < 0) return ok;
    if (! verbose) return 0;
    prn.level = level;
    prn.func = func;
    prn.arg = arg;
    return meta_iterate(dev, meta_print_range, &prn);
}

/* add a range to a list of blocks */
static int meta_add_range(off_t start, off_t end, void * _ml) {
    makelist_t * ml = _ml;
    const ddb_blocklist_t * add[2];
    ddb_blocklist_t * tmp, * range;
    if (! ml->list)
	return ddb_blocklist_add(ml->res, start, end);
    range = ddb_blocklist_sub(ml->list, start, end);
    if (! range) return -1;
    add[0] = ml->res;
    add[1] = range;
    tmp = ddb_blocklist_union(2, add);
    if (! tmp) {
	int sve = errno;
	ddb_blocklist_free(range);
	errno = sve;
	return -1;
    }
    ddb_blocklist_free(range);
    ddb_blocklist_free(ml->res);
    ml->res = tmp;
    return 0;
}

/* get sub-list of blocks */
static ddb_blocklist_t * meta_has_blocks(ddb_device_t * dev,
		const ddb_blocklist_t * list)
{
    makelist_t ml;
    int sve;
    ml.list = list;
    ml.res = ddb_blocklist_new();
    if (! ml.res) return NULL;
    if (meta_iterate(dev, meta_add_range, &ml) >= 0)
	return ml.res;
    sve = errno;
    ddb_blocklist_free(ml.res);
    errno = sve;
    return NULL;
}

/* get list of blocks */
static ddb_blocklist_t * meta_blocks(ddb_device_t * dev) {
    return meta_has_blocks(dev, NULL);
}

/* flush metadata to disk */
static int meta_flush(ddb_device_t * dev) {
    if (! write_metadata(image(dev))) return -1;
    /* we may want to fsync() or fdatasync() */
    return 0;
}

/* close device */
static int image_close(ddb_device_t * dev) {
    int ok = 0, err = 0;
    if (dev->ops && dev->ops->flush && dev->ops->flush(dev) < 0) {
	err = errno;
	ok = -1;
    }
    if (image(dev)->fd >= 0 && close(image(dev)->fd) < 0 && ok >= 0) {
	err = errno;
	ok = -1;
    }
    free_summary(image(dev));
    errno = err;
    return ok;
}

static const ddb_device_operations_t meta_ops = {
    .type       = DDB_TYPE_META,
    .read       = image_read,
    .write      = image_write,
    .info       = meta_info,
    .print      = meta_print,
    .has_block  = meta_has_block,
    .blocks     = meta_blocks,
    .range      = NULL, /* meaning all blocks */
    .has_blocks = meta_has_blocks,
    .iterate    = meta_iterate,
    .flush      = meta_flush,
    .report     = NULL,
    .close      = image_close,
};

static const ddb_device_operations_t sparse_ops = {
    .type       = DDB_TYPE_META,
    .read       = image_read,
    .write      = image_write,
    .info       = meta_info,
    .print      = meta_print,
    .has_block  = meta_has_block,
    .blocks     = meta_blocks,
    .range      = meta_blocks,
    .has_blocks = meta_has_blocks,
    .iterate    = meta_iterate,
    .flush      = meta_flush,
    .report     = NULL,
    .close      = image_close,
};

static const ddb_device_operations_t image_ops = {
    .type       = DDB_TYPE_REG,
    .read       = image_read,
    .write      = image_write,
    .info       = NULL,
    .print      = NULL,
    .has_block  = NULL,
    .blocks     = NULL,
    .has_blocks = NULL,
    .iterate    = NULL,
    .flush      = NULL,
    .report     = NULL,
    .close      = image_close,
};

static const ddb_device_operations_t dev_ops = {
    .type       = DDB_TYPE_DEV,
    .read       = image_read,
    .write      = image_write,
    .info       = NULL,
    .print      = NULL,
    .has_block  = NULL,
    .blocks     = NULL,
    .has_blocks = NULL,
    .iterate    = NULL,
    .flush      = NULL,
    .report     = NULL,
    .close      = image_close,
};

/* open an image using an existing file descriptor */
static ddb_device_t * open_image_fd(const char * path, int fd,
		int want_metadata, size_t block_size, int flags,
		off_t total_size, off_t offset)
{
    struct stat sbuff;
    ddb_device_t * dev;
    image_t img;
    off_t size, blocks_present = -1;
    time_t mtime = 0;
    size_t devsize, metasize;
    img.fd = fd;
    img.offset = offset;
    size = lseek(img.fd, 0, SEEK_END);
    if (size < 0) return NULL;
    /* we don't allow 0-size images... */
    if (size <= offset && (! (flags & O_CREAT) || total_size < 1)) {
	errno = EINVAL;
	return NULL;
    }
    size -= offset;
    if (total_size >= 0) {
	if (size == 0 && (flags & O_CREAT)) {
	    if (want_metadata > 1) {
		/* create initial metadata for a new image */
		if (block_size < 512) {
		    errno = EINVAL;
		    return NULL;
		}
		img.header.total_size = total_size;
		img.header.block_size = block_size;
		img.header.blocks_present = 0;
		img.header.mtime = time(NULL);
		img.header.data_end = (off_t)block_size + offset;
		img.header.metadata = 0;
		fill_derived_header(&img.header);
		img.flush_file_header = 1;
		/* and clear the remaining data */
		img.metadata_header.summary = NULL;
		img.flush_metadata = 0;
		if (! write_metadata(&img)) return NULL;
	    } else {
		/* extend device to required size */
		if (ftruncate(img.fd, offset + total_size) < 0) return NULL;
	    }
	    size = total_size;
	}
    }
    /* OK, size matches, now look for presence of metadata */
    if (fstat(img.fd, &sbuff) < 0) return NULL;
    if (mtime == 0) mtime = sbuff.st_mtime;
    img.metadata_summary = NULL;
    img.has_metadata = 0;
    if (S_ISREG(sbuff.st_mode) && size >= sizeof(disk_header_t)) {
	disk_header_t dh;
	if (ddb_read_block(img.fd, offset, sizeof(dh), &dh, 0)) {
	    if (header_decode(&dh, &img.header, size)) {
		img.has_metadata = 1;
		size = img.header.total_size;
		if (block_size > 0 && block_size != img.header.block_size) {
		    /* block size mismatch */
		    errno = EINVAL;
		    return NULL;
		}
		img.metadata_header.summary = NULL;
		img.flush_metadata = 0;
		img.flush_file_header = 0;
		block_size = img.header.block_size;
		blocks_present = img.header.blocks_present;
		mtime = img.header.mtime;
	    }
	}
    }
    /* now we have either the file stat or the metadata, check consistency */
    errno = EINVAL;
    if (want_metadata > 1 && ! img.has_metadata)
	return NULL;
    if (want_metadata < 1 && img.has_metadata)
	return NULL;
    if (total_size > 0 && size != total_size)
	return NULL;
    if (block_size < 512)
	return NULL;
    /* for an images without metadata we make up a metadata header because
     * it contains useful things */
    if (! img.has_metadata) {
	img.header.total_size = size;
	img.header.block_size = block_size;
	img.header.mtime = mtime;
	fill_derived_header(&img.header);
	img.header.blocks_present = img.header.total_blocks;
	img.header.data_end = offset + size;
	img.metadata_buffer = NULL;
    }
    /* OK, allocate memory and that'll be it */
    devsize = sizeof(image_t) + block_size;
    metasize = 0;
    if (img.has_metadata) {
	metasize = sizeof(metadata_data_t) * img.header.meta_size;
	devsize += block_size + metasize;
    }
    dev = ddb_device_open_multi(devsize, 0, NULL, flags);
    if (! dev) return NULL;
    memcpy(dev->extra, &img, sizeof(img));
    if (img.has_metadata) {
	image(dev)->metadata_data = (void *)image(dev)->_buffer;
	image(dev)->block_buffer = image(dev)->_buffer + metasize;
	image(dev)->metadata_buffer = image(dev)->block_buffer + block_size;
    } else {
	image(dev)->block_buffer = image(dev)->_buffer;
	image(dev)->metadata_buffer = NULL;
	image(dev)->metadata_data = NULL;
    }
    dev->name = path;
    if (img.has_metadata) {
	dev->ops = &meta_ops;
    } else if (S_ISREG(sbuff.st_mode)) {
	dev->ops = &image_ops;
    } else {
	dev->ops = &dev_ops;
    }
    ddb_fill_info(dev, size, block_size, blocks_present,
		  mtime, sbuff.st_blocks);
    return dev;
}

/* open an image with metadata using an existing file descriptor */
ddb_device_t * ddb_device_open_image_fd(const char * path, int fd,
		size_t block_size, int flags, off_t total_size, off_t offset)
{
    return open_image_fd(path, fd, 1, block_size, flags, total_size, offset);
}

/* open "image" */
ddb_device_t * ddb_device_open_image(const char * path,
		int want_metadata, size_t block_size,
		int flags, off_t total_size)
{
    ddb_device_t * dev;
    int fd, sve;
#ifdef O_CLOEXEC
    fd = open(path, flags|O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
#else
    fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    sve = fcntl(fd, F_GETFD);
    if (sve < 0) goto error;
    if (fcntl(fd, F_SETFD, sve|FD_CLOEXEC) < 0) goto error;
#endif
    dev = open_image_fd(path, fd, want_metadata, block_size,
			flags, total_size, (off_t)0);
    if (dev) return dev;
#ifndef O_CLOEXEC
error:
#endif
    sve = errno;
    if (fd >= 0) close(fd);
    errno = sve;
    return NULL;
}

/* mark an image for sparse copy */
void ddb_set_image_sparse(ddb_device_t * dev) {
    if (dev->ops == &meta_ops)
	dev->ops = &sparse_ops;
}

/* get image placement in file */
void ddb_image_placement(const ddb_device_t * dev, off_t * start, off_t * end) {
    *start = image(dev)->offset;
    *end = image(dev)->header.data_end;
}

