/* ddb-copy.c - copy data from a device to another
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
#include <limits.h>
#include <string.h>
#include <memory.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <endian.h>
#include <ddb.h>
#include "ddb-private.h"

/* number of blocks we copy in one go */
#define MAX_COPY_BLOCK 16384
static int copy_block = 32;
static int can_change_copy_block = 1;

/* checkpoint header on disk */
typedef struct {
    int64_t magic;
    int64_t total_size;
    int64_t pass_size;
    int64_t blocks_read;
    int64_t read_errors;
    int64_t blocks_written;
    int64_t blocks_skipped;
    int64_t checksum_equal;
    int64_t write_errors;
    int32_t block_size;
    int32_t pass;
} __attribute__((packed)) checkpoint_header_t;

#define CHECKPOINT_MAGIC 0x43686b506f696e74LL /* "ChkPoint" */

typedef struct {
    ddb_block_t * rblocks;
    ddb_block_t * wblocks;
    ddb_copy_t * what;
    char ** buffers;
    ddb_blocklist_t * to_copy;
    ddb_blocklist_t * to_retry;
    ddb_blocklist_t * copied;
    off_t total_to_copy;
    off_t pass_size;
    off_t blocks_read;
    off_t read_errors;
    off_t blocks_written;
    off_t blocks_skipped;
    off_t checksum_equal;
    off_t write_errors;
    time_t next_flush;
    time_t next_report;
    time_t next_checkpoint;
    time_t next_machine_report;
    int pass;
    int progress_clear;
    int rcount;
    int use_checksums;
} copy_context_t;

static int load_checkpoint(ddb_copy_t * W, copy_context_t * C) {
    checkpoint_header_t ch, cf;
    FILE * F = fopen(W->checkpoint_file, "r");
    int ok = -1;
    if (! F) return 0;
    /* load checkpoint file and check it's compatible with device and options */
    if (fread(&ch, sizeof(ch), 1, F) < 1) goto out;
    if (be64toh(ch.magic) != CHECKPOINT_MAGIC) goto out;
    if (be64toh(ch.total_size) != W->total_size) goto out;
    if (be32toh(ch.block_size) != W->block_size) goto out;
    C->pass = be32toh(ch.pass);
    if (C->pass >= W->max_passes) C->pass = W->max_passes - 1;
    C->pass_size = be64toh(ch.pass_size);
    C->blocks_read = be64toh(ch.blocks_read);
    C->read_errors = be64toh(ch.read_errors);
    if (C->pass_size < 0 ||
        C->blocks_read < 0 ||
	C->read_errors < 0 ||
	C->blocks_read + C->read_errors > W->total_blocks)
	    goto out;
    C->blocks_written = be64toh(ch.blocks_written);
    C->blocks_skipped = be64toh(ch.blocks_skipped);
    C->checksum_equal = be64toh(ch.checksum_equal);
    C->write_errors = be64toh(ch.write_errors);
    if (C->blocks_written < 0 ||
	C->blocks_skipped < 0 ||
	C->checksum_equal < 0 ||
	C->checksum_equal > C->blocks_skipped ||
	C->write_errors < 0 ||
	C->blocks_written + C->blocks_skipped + C->write_errors > W->total_blocks)
	    goto out;
    if (C->to_copy) ddb_blocklist_free(C->to_copy);
    C->to_copy = ddb_blocklist_load(F);
    if (! C->to_copy) goto out;
    if (C->to_retry) ddb_blocklist_free(C->to_retry);
    C->to_retry = ddb_blocklist_load(F);
    if (! C->to_retry) goto out;
    if (C->copied) ddb_blocklist_free(C->copied);
    C->copied = ddb_blocklist_load(F);
    if (! C->copied) goto out;
    if (fread(&cf, sizeof(cf), 1, F) < 1) goto out;
    if (memcmp(&cf, &ch, sizeof(ch)) != 0) goto out;
    ok = 1;
out:
    fclose(F);
    return ok;
}

static void write_checkpoint(const ddb_copy_t * W, const copy_context_t * C) {
    checkpoint_header_t ch;
    const char * slash = rindex(W->checkpoint_file, '/');
    char tmpname[6 + strlen(W->checkpoint_file)];
    FILE * F;
    if (slash) {
	int pl = slash - W->checkpoint_file + 1;
	strncpy(tmpname, W->checkpoint_file, pl);
	tmpname[pl] = '.';
	strcpy(tmpname + pl + 1, W->checkpoint_file + pl);
	strcat(tmpname, ".tmp");
    } else {
	strcpy(tmpname, ".");
	strcat(tmpname, W->checkpoint_file);
	strcat(tmpname, ".tmp");
    }
    F = fopen(tmpname, "w");
    if (! F) return;
    ch.magic = htobe64(CHECKPOINT_MAGIC);
    ch.total_size = htobe64(W->total_size);
    ch.pass_size = htobe64(C->pass_size);
    ch.blocks_read = htobe64(C->blocks_read);
    ch.read_errors = htobe64(C->read_errors);
    ch.blocks_written = htobe64(C->blocks_written);
    ch.blocks_skipped = htobe64(C->blocks_skipped);
    ch.checksum_equal = htobe64(C->checksum_equal);
    ch.write_errors = htobe64(C->write_errors);
    ch.block_size = htobe32(W->block_size);
    ch.pass = htobe32(C->pass);
    if (fwrite(&ch, sizeof(ch), 1, F) < 1) goto error;
    if (ddb_blocklist_save(C->to_copy, F) < 0) goto error;
    if (ddb_blocklist_save(C->to_retry, F) < 0) goto error;
    if (ddb_blocklist_save(C->copied, F) < 0) goto error;
    if (fwrite(&ch, sizeof(ch), 1, F) < 1) goto error;
    if (fclose(F) < 0) goto remove;
    if (rename(tmpname, W->checkpoint_file) < 0) goto remove;
    return;
error:
    fclose(F);
remove:
    unlink(tmpname);
}

static void write_machine_report(time_t now, const ddb_copy_t * W,
		const copy_context_t * C)
{
    const char * slash = rindex(W->machine_progress_file, '/');
    char tmpname[6 + strlen(W->machine_progress_file)];
    FILE * F;
    if (slash) {
	int pl = slash - W->machine_progress_file + 1;
	strncpy(tmpname, W->machine_progress_file, pl);
	tmpname[pl] = '.';
	strcpy(tmpname + pl + 1, W->machine_progress_file + pl);
	strcat(tmpname, ".tmp");
    } else {
	strcpy(tmpname, ".");
	strcat(tmpname, W->machine_progress_file);
	strcat(tmpname, ".tmp");
    }
    F = fopen(tmpname, "w");
    if (! F) return;
    if (fprintf(F, "%lld %lld %lld %lld %lld %lld %lld %ld %d\n",
		(long long)now, (long long)C->total_to_copy,
		(long long)C->blocks_read, (long long)C->read_errors,
		(long long)C->blocks_written, (long long)C->blocks_skipped,
		(long long)C->write_errors, (long)W->src->info.block_size,
		C->pass) < 0)
	goto error;
    if (fclose(F) < 0) goto remove;
    if (rename(tmpname, W->machine_progress_file) < 0) goto remove;
    return;
error:
    fclose(F);
remove:
    unlink(tmpname);
}

static const char * ts(void) {
    static char buffer[10];
    time_t now = time(NULL);
    strftime(buffer, sizeof(buffer), "%H:%M:%S", localtime(&now));
    return buffer;
}

static void progress_report(const ddb_copy_t * W, copy_context_t * C, int nl) {
    char progbuff[256];
    double percent;
    int clr = 0;
    if (! W->progress_function) return;
    percent = 100.0 * (double)(C->blocks_read + C->read_errors)
		    / (double)C->pass_size;
    clr += snprintf(progbuff + clr, sizeof(progbuff) - clr,
		    "%s %.2f%% %lld rd + %lld er",
		    ts(), percent, (long long)C->blocks_read,
		    (long long)C->read_errors);
    if (W->dst) {
	if (C->use_checksums) {
	    if (W->write_dst)
		clr += snprintf(progbuff + clr, sizeof(progbuff) - clr,
				"; %lld wr + %lld(%lld) sk + %lld er",
				(long long)C->blocks_written,
				(long long)C->blocks_skipped,
				(long long)C->checksum_equal,
				(long long)C->write_errors);
	    else
		clr += snprintf(progbuff + clr, sizeof(progbuff) - clr,
				"; %lld(%lld) eq + %lld ne + %lld er",
				(long long)C->blocks_skipped,
				(long long)C->checksum_equal,
				(long long)C->blocks_written,
				(long long)C->write_errors);
	} else {
	    if (W->write_dst)
		clr += snprintf(progbuff + clr, sizeof(progbuff) - clr,
				"; %lld wr + %lld sk + %lld er",
				(long long)C->blocks_written,
				(long long)C->blocks_skipped,
				(long long)C->write_errors);
	    else
		clr += snprintf(progbuff + clr, sizeof(progbuff) - clr,
				"; %lld eq + %lld ne + %lld er",
				(long long)C->blocks_skipped,
				(long long)C->blocks_written,
				(long long)C->write_errors);
	}
    }
    if (clr < C->progress_clear)
	snprintf(progbuff + clr, sizeof(progbuff) - clr,
		 "%.*s", C->progress_clear - clr, "");
    if (nl) {
	C->progress_clear = 0;
	snprintf(progbuff + clr, sizeof(progbuff) - clr, "\n");
    } else {
	C->progress_clear = clr;
	snprintf(progbuff + clr, sizeof(progbuff) - clr, "\r");
    }
    W->progress_function(W->progress_arg, progbuff);
}

static int add_block(ddb_blocklist_t * ls, off_t block) {
    if (ddb_blocklist_add(ls, block, block) >= 0) return 1;
    perror("malloc");
    return 0;
}

static int copy_blocks(ddb_copy_t * W, copy_context_t * C) {
    time_t now;
    int bnum, wcount, cmpmap[copy_block];
    if (C->rcount < 1) goto finish;
    /* first, if they asked to skip identical, and we are using checksums,
     * read checksums and compare */
    if (C->use_checksums) {
	if (W->src->info.is_remote) {
	    int wend;
	    /* read checksums from destination */
	    ddb_device_read_multi(W->dst, C->rcount, C->rblocks, DDB_READ_CHECKSUM);
	    wcount = 0;
	    /* if anything had a read error on destination add to the
	     * final list as we'll want to write always */
	    for (bnum = 0; bnum < C->rcount; bnum++) {
		if (C->rblocks[bnum].result > 0) continue;
		C->wblocks[wcount].block = C->rblocks[bnum].block;
		C->wblocks[wcount].buffer = C->buffers[wcount];
		wcount++;
	    }
	    if (wcount > 0) {
		int wprev = wcount;
		ddb_device_read_multi(W->src, wcount, C->wblocks, DDB_READ_BLOCK);
		wcount = 0;
		for (bnum = 0; bnum < wprev; bnum++) {
		    if (C->wblocks[bnum].result <= 0) {
			C->read_errors++;
			if (! add_block(C->to_retry, C->wblocks[bnum].block))
			    return 0;
		    } else {
			if (wcount != bnum)
			    C->wblocks[wcount] = C->wblocks[bnum];
			wcount++;
		    }
		}
	    }
	    wend = wcount;
	    /* and then send the "OK" checksums to source and ask to send
	     * any data which doesn't match */
	    for (bnum = 0; bnum < C->rcount; bnum++) {
		if (C->rblocks[bnum].result <= 0) continue;
		C->wblocks[wend] = C->rblocks[bnum];
		wend++;
	    }
	    if (wend > wcount) {
		int wbase = wcount;
		ddb_device_read_multi(W->src, wend - wbase, &C->wblocks[wbase],
				      DDB_READ_BLOCK | DDB_READ_MAYBE);
		for (bnum = wbase; bnum < wend; bnum++) {
		    if (C->wblocks[bnum].result < 0) {
			C->read_errors++;
			if (! add_block(C->to_retry, C->wblocks[bnum].block))
			    return 0;
		    } else if (C->wblocks[bnum].result == 0) {
			/* source stated that data matches */
			C->blocks_skipped++;
			C->checksum_equal++;
			C->blocks_read++;
		    } else {
			/* we'll copy this block */
			if (wcount != bnum)
			    C->wblocks[wcount] = C->wblocks[bnum];
			C->blocks_read++;
			wcount++;
		    }
		}
	    }
	    if (wcount < 1) goto finish;
	    if (! W->write_dst) goto finish;
	    /* we just need to skip to write these wcount blocks */
	    goto write_dst;
	}
	/* read checksums from both source and destination */
	for (bnum = 0; bnum < C->rcount; bnum++) {
	    C->wblocks[bnum].block = C->rblocks[bnum].block;
	    C->wblocks[bnum].buffer = C->buffers[bnum];
	}
	ddb_device_read_multi(W->src, C->rcount, C->rblocks, DDB_READ_CHECKSUM);
	ddb_device_read_multi(W->dst, C->rcount, C->wblocks, DDB_READ_CHECKSUM);
	for (bnum = wcount = 0; bnum < C->rcount; bnum++) {
	    if (C->rblocks[bnum].result > 0) {
		if (C->wblocks[bnum].result > 0) {
		    if (memcmp(C->rblocks[bnum].buffer, C->wblocks[bnum].buffer,
			       DDB_CHECKSUM_LENGTH) == 0)
		    {
			/* checksum matches, we'll skip this */
			C->blocks_skipped++;
			C->checksum_equal++;
			C->blocks_read++;
			continue;
		    }
		}
	    } else {
		/* read source failed, we'll retry next pass */
		C->read_errors++;
		if (! add_block(C->to_retry, C->rblocks[bnum].block))
		    return 0;
		continue;
	    }
	    /* we'll need to read the full data */
	    C->rblocks[wcount].block = C->rblocks[bnum].block;
	    wcount++;
	}
	C->rcount = wcount;
	if (C->rcount < 1) goto finish;
    }
    /* any remaining block will need to be copied in full, so
     * first read all these blocks */
    ddb_device_read_multi(W->src, C->rcount, C->rblocks, DDB_READ_BLOCK);
    for (bnum = wcount = 0; bnum < C->rcount; bnum++) {
	if (C->rblocks[bnum].result > 0) {
	    /* we need to write/check this block */
	    C->wblocks[wcount].block = C->rblocks[bnum].block;
	    if (! C->use_checksums && (W->skip_identical || ! W->write_dst))
		C->wblocks[wcount].buffer = C->buffers[wcount];
	    else
		C->wblocks[wcount].buffer = C->rblocks[bnum].buffer;
	    cmpmap[wcount] = bnum;
	    wcount++;
	    C->blocks_read++;
	} else {
	    /* we'll need to retry this */
	    C->read_errors++;
	    if (! add_block(C->to_retry, C->rblocks[bnum].block))
		return 0;
	}
    }
    if (wcount < 1) goto finish;
    if (! W->dst) goto finish;
    /* OK, we have a list of block to either write or compare; if we
     * have skip_identical != 0 or write_dst == 0 and we didn't already
     * determine with checksums that the data differs, we read destination */
    if (! C->use_checksums && (W->skip_identical || ! W->write_dst)) {
	int diff;
	ddb_device_read_multi(W->dst, wcount, C->wblocks, DDB_READ_BLOCK);
	for (bnum = diff = 0; bnum < wcount; bnum++) {
	    int rblock = cmpmap[bnum];
	    void * rdata = C->rblocks[rblock].buffer;
	    if (C->wblocks[bnum].result > 0 &&
		memcmp(C->wblocks[bnum].buffer, rdata, W->block_size) == 0)
	    {
		/* data is identical */
		C->blocks_skipped++;
	    } else {
		/* data differs */
		C->wblocks[diff].block = C->rblocks[rblock].block;
		C->wblocks[diff].buffer = rdata;
		diff++;
	    }
	}
	wcount = diff;
	if (wcount < 1) goto finish;
    }
    /* we have some data to write to destination */
write_dst:
    if (! W->write_dst) {
	C->blocks_written += wcount;
	goto finish;
    }
    ddb_device_write_multi(W->dst, wcount, C->wblocks);
    for (bnum = 0; bnum < wcount; bnum++) {
	if (C->wblocks[bnum].result > 0) {
	    C->blocks_written++;
	} else {
	    C->write_errors++;
	    if (! add_block(C->to_retry, C->wblocks[bnum].block))
		return 0;
	}
    }
    /* all done, check for periodic tasks */
finish:
    now = time(NULL);
    if (W->progress_interval > 0 && now >= C->next_report) {
	progress_report(W, C, 0);
	if (W->progress_sleep > 0) {
	    sleep(W->progress_sleep);
	    now = time(NULL);
	}
	C->next_report = now + W->progress_interval;
    }
    if (W->dst && W->write_dst && now >= C->next_flush) {
	C->next_flush = now + W->flush_interval;
	if (ddb_device_flush(W->dst) < 0) {
	    perror(W->dst_name);
	    return 0;
	}
    }
    if (W->checkpoint_file && now >= C->next_checkpoint) {
	C->next_checkpoint = now + W->checkpoint_interval;
	write_checkpoint(W, C);
    }
    if (W->machine_progress_file  &&
	W->machine_progress_interval > 0 &&
	now >= C->next_machine_report)
    {
	C->next_machine_report = now + W->machine_progress_interval;
	write_machine_report(now, W, C);
    }
    C->rcount = 0;
    return 1;
}

static int copy_range(off_t start, off_t end, void * _C) {
    copy_context_t * C = _C;
    ddb_copy_t * W = C->what;
    start--;
    while (start < end) {
	start++;
	C->rblocks[C->rcount].block = start;
	C->rblocks[C->rcount].result = 0;
	C->rblocks[C->rcount].error = 0;
	C->rcount++;
	if (C->rcount >= copy_block)
	    if (! copy_blocks(W, C))
		return -1;
    }
    return 0;
}

static int write_block_list(const char * name, const copy_context_t * C) {
    FILE * F;
    const char * slash = rindex(name, '/');
    char tmpname[6 + strlen(name)];
    int ok;
    if (slash) {
	int pl = slash - name + 1;
	strncpy(tmpname, name, pl);
	tmpname[pl] = '.';
	strcpy(tmpname + pl + 1, name + pl);
	strcat(tmpname, ".tmp");
    } else {
	strcpy(tmpname, ".");
	strcat(tmpname, name);
	strcat(tmpname, ".tmp");
    }
    F = fopen(tmpname, "w");
    if (! F) return 0;
    ok = ddb_blocklist_print(C->to_copy, F) >= 0;
    if (fclose(F) == EOF) ok = 0;
    if (ok && rename(tmpname, name) >= 0) return 1;
    unlink(tmpname);
    return ok;
}

int ddb_copy(ddb_copy_t * W) {
    ddb_block_t C_rblocks[copy_block], C_wblocks[copy_block];
    copy_context_t C;
    char * C_buffers[copy_block];
    char * buffer = malloc(2 * copy_block * W->block_size), * bptr;
    off_t count_to_copy;
    int resuming = 0, i, ok = -1;
    can_change_copy_block = 0;
    C.rblocks = C_rblocks;
    C.wblocks = C_wblocks;
    C.buffers = C_buffers;
    C.to_copy = C.to_retry = C.copied = NULL;
    if (! buffer) {
	perror("malloc");
	goto out;
    }
    C.what = W;
    bptr = buffer;
    for (i = 0; i < copy_block; i++) {
	C.rblocks[i].buffer = bptr;
	bptr += W->block_size;
	C.buffers[i] = bptr;
	bptr += W->block_size;
    }
    C.blocks_read = C.read_errors = C.blocks_written =
	C.blocks_skipped = C.checksum_equal = C.write_errors = 0;
    C.copied = ddb_blocklist_new();
    C.use_checksums = W->dst && W->skip_identical && W->use_checksums &&
		      (W->src->info.is_remote || W->dst->info.is_remote);
    if (! C.copied) goto out;
    /* get blocks to copy */
    if (W->input_list) {
	FILE * F = fopen(W->input_list, "r");
	if (! F) {
	    perror(W->input_list);
	    goto out;
	}
	C.to_copy = ddb_blocklist_read(F);
	if (! C.to_copy) {
	    perror(W->input_list);
	    fclose(F);
	    goto out;
	}
	fclose(F);
	// XXX check it's valid for device
    } else {
	C.to_copy = ddb_device_copy_blocks(W->src);
	if (! C.to_copy) {
	    perror(W->src_name);
	    goto out;
	}
    }
    count_to_copy = ddb_blocklist_count(C.to_copy);
    C.pass = 0;
    C.rcount = 0;
    C.progress_clear = 0;
    C.total_to_copy = count_to_copy;
    if (W->checkpoint_file) {
	int loaded = load_checkpoint(W, &C);
	if (loaded < 0) goto out;
	resuming = loaded;
	C.next_checkpoint = time(NULL) + W->checkpoint_interval;
    }
    C.next_flush = time(NULL) + W->flush_interval;
    C.next_machine_report = time(NULL) + W->machine_progress_interval;
    while (C.pass < W->max_passes && count_to_copy > 0) {
	/* to_retry may have been loaded from checkpoint */
	if (! C.to_retry) {
	    C.to_retry = ddb_blocklist_new();
	    if (! C.to_retry) {
		perror("malloc");
		goto out;
	    }
	}
	if (W->progress_function) {
	    char progbuff[256];
	    const char * todo = 
		W->dst ? (W->write_dst ? "copy" : "check") : "read";
	    if (resuming) {
		snprintf(progbuff, sizeof(progbuff),
			 "%s %s: %s: resume pass %d, %lld of %lld blocks to %s\n",
			 ts(), progname, W->src_name, C.pass,
			 (long long)(count_to_copy - C.blocks_read - C.read_errors),
			 (long long)count_to_copy, todo);
	    } else {
		C.pass++;
		snprintf(progbuff, sizeof(progbuff),
			 "%s %s: %s: start pass %d, %lld blocks to %s\n",
			 ts(), progname, W->src_name, C.pass,
			 (long long)count_to_copy, todo);
	    }
	    W->progress_function(W->progress_arg, progbuff);
	    C.pass_size = count_to_copy;
	    C.progress_clear = 0;
	    C.next_report = time(NULL) + W->progress_interval;
	}
	/* now do the pass... */
	C.rcount = 0;
	if (ddb_blocklist_iterate(C.to_copy, copy_range, &C) < 0)
	    goto out;
	if (C.rcount > 0)
	    if (! copy_blocks(W, &C))
		goto out;
	/* and prepare for next pass */
	ddb_blocklist_free(C.to_copy);
	C.to_copy = C.to_retry;
	C.to_retry = NULL;
	count_to_copy = ddb_blocklist_count(C.to_copy);
	if (W->progress_function) {
	    char progbuff[256];
	    const char * done = 
		W->dst ? (W->write_dst ? "copied" : "are equal") : "read";
	    off_t differ = W->write_dst ? count_to_copy : C.blocks_written;
	    int cnt;
	    if (C.progress_clear > 0) {
		snprintf(progbuff, sizeof(progbuff),
			 "%.*s", C.progress_clear, "");
		W->progress_function(W->progress_arg, progbuff);
		C.progress_clear = 0;
	    }
	    if (W->extra_report)
		progress_report(W, &C, 1);
	    if (differ)
		cnt = snprintf(progbuff, sizeof(progbuff) - 1,
			       "%s %s: %s: end pass %d, %lld blocks %s, %lld blocks %s",
			       ts(), progname, W->src_name, C.pass,
			       (long long)C.blocks_read, done,
			       (long long)differ, W->write_dst ? "to retry" : "differ");
	    else
		cnt = snprintf(progbuff, sizeof(progbuff) - 1,
			       "%s %s: %s: end pass %d, all %lld blocks %s",
			       ts(), progname, W->src_name, C.pass,
			       (long long)C.blocks_read, done);
	    if (C.read_errors)
		cnt += snprintf(progbuff + cnt, sizeof(progbuff) - cnt - 1,
				", %lld read errors",
				(long long)C.read_errors);
	    if (C.write_errors)
		cnt += snprintf(progbuff + cnt, sizeof(progbuff) - cnt - 1,
				", %lld write errors",
				(long long)C.write_errors);
	    snprintf(progbuff + cnt, sizeof(progbuff) - cnt, "\n");
	    W->progress_function(W->progress_arg, progbuff);
	}
	C.blocks_read = C.read_errors = C.blocks_written =
	    C.blocks_skipped = C.checksum_equal = C.write_errors = 0;
	if (W->output_list && W->output_each_pass)
	    if (! write_block_list(W->output_list, &C))
		perror(W->output_list);
    }
    if (W->output_list && ! W->output_each_pass)
	if (! write_block_list(W->output_list, &C))
	    perror(W->output_list);
    if (W->copied_list)
	if (! write_block_list(W->copied_list, &C))
	    perror(W->copied_list);
    ok = count_to_copy ? 0 : 1;
out:
    if (buffer) free(buffer);
    if (C.to_copy) ddb_blocklist_free(C.to_copy);
    if (C.to_retry) ddb_blocklist_free(C.to_retry);
    if (C.copied) ddb_blocklist_free(C.copied);
    return ok;
}

int ddb_copy_block(int block) {
    if (block > 0 && block < MAX_COPY_BLOCK && can_change_copy_block) {
	if (ddb_devices_opened())
	    can_change_copy_block = 0;
	else
	    copy_block = block;
    }
    return copy_block;
}

