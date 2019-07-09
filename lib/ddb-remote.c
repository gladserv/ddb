/* ddb-remote.c - "remote" access, via a program or plugin
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
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ddb.h>
#include "ddb-private.h"
#include "ddb-remote.h"

static char default_modpath[] = DEFAULT_MODPATH;

typedef struct {
    ddb_device_operations_t ops;
    struct timeval reqtime;
    ddb_remote_t * descr;
    ddb_prepare_t * prepare;
    ddb_prepare_t * close;
    off_t bytes_sent;
    off_t bytes_received;
    pid_t pid;
    FILE * F_in;
    FILE * F_out;
    int requests;
    int supp;
    int rwmax;
    int retry_max;
    int retry_delay;
} extra_t;

typedef struct {
    int verbose;
    int indent;
} print_t;

typedef struct {
    int flags;
    int nblocks;
    int data_size;
} rwdata_t;

typedef struct {
    int status;
    const ddb_blocklist_t * blocks;
    const off_t * send_block;
    const print_t * send_print;
    ddb_device_info_t * recv_info;
    const rwdata_t * send_rwdata;
    ddb_block_t * send_data;
    ddb_block_t * recv_data;
    int skip_equal;
} command_t;

/* used to call send_greeting - it had so many parameters it
 * was easy to give them in the wrong order */
typedef struct {
    FILE * F_in;
    FILE * F_out;
    const char * name;
    const char * type;
    int flags;
    const char * action;
    const char * aux_name;
    const char * aux_type;
} send_greeting_in_t;
typedef struct {
    size_t block_size;
    off_t total_size;
    int supp;
    int rwmax;
    time_t mtime;
    off_t blocks_present;
    off_t blocks_allocated;
    off_t bytes_sent;
    off_t bytes_received;
} send_greeting_out_t;

#define extra(dev) ((extra_t *)((dev)->extra))

int ddb_decode_errno(int code) {
    switch (code) {
	case ERRCODE_OK: return 0;
	case ERRCODE_ACCESS: return EACCES;
	case ERRCODE_EXISTS: return EEXIST;
	case ERRCODE_NOENT: return ENOENT;
	case ERRCODE_INVALID: return EINVAL;
	case ERRCODE_ISDIR: return EISDIR;
	case ERRCODE_NOTDIR: return ENOTDIR;
	case ERRCODE_LOOP: return ELOOP;
	case ERRCODE_NOMEM: return ENOMEM;
	default: return EINVAL;
    }
}

int ddb_encode_errno(int code) {
    switch (code) {
	case 0: return ERRCODE_OK;
	case EACCES: return ERRCODE_ACCESS;
	case EEXIST: return ERRCODE_EXISTS;
	case ENOENT: return ERRCODE_NOENT;
	case EINVAL: return ERRCODE_INVALID;
	case EISDIR: return ERRCODE_ISDIR;
	case ENOTDIR: return ERRCODE_NOTDIR;
	case ELOOP: return ERRCODE_LOOP;
	case ENOMEM: return ERRCODE_NOMEM;
	default: return ERRCODE_OTHER;
    }
}

/* send connection greeting and wait for other end's reply */
static int send_greeting(const send_greeting_in_t * I,
		send_greeting_out_t * O)
{
    conn_open_t conn;
    conn_result_t result;
    int nf = 0, rwmax = ddb_copy_block(0);
    O->bytes_sent = sizeof(conn);
    O->bytes_received = sizeof(result);
    switch (I->flags & O_ACCMODE) {
	case O_RDONLY : nf |= CONN_OPEN_RDONLY; break;
	case O_WRONLY : nf |= CONN_OPEN_WRONLY; break;
	case O_RDWR : nf |= CONN_OPEN_RDWR; break;
	default: errno = EINVAL; return 0;
    }
    if (I->flags & O_CREAT) nf |= CONN_OPEN_CREAT;
    if (I->flags & O_EXCL) nf |= CONN_OPEN_EXCL;
    conn.rwmax = htobe32(rwmax);
    conn.magic = htobe64(CONN_OPEN_MAGIC);
    conn.total_size = htobe64(O->total_size);
    conn.flags = htobe32(nf);
    conn.block_size = htobe32(O->block_size);
    if (I->type) {
	int len = strlen(I->type);
	conn.type_size = htobe32(len);
	O->bytes_sent += len;
    } else {
	conn.type_size = htobe32(-1);
    }
    if (I->name) {
	int len = strlen(I->name);
	conn.name_size = htobe32(len);
	O->bytes_sent += len;
    } else {
	conn.name_size = htobe32(-1);
    }
    if (I->action) {
	int len = strlen(I->action);
	conn.action_size = htobe32(len);
	O->bytes_sent += len;
    } else {
	conn.action_size = htobe32(-1);
    }
    if (I->aux_type) {
	int len = strlen(I->aux_type);
	conn.aux_type_size = htobe32(len);
	O->bytes_sent += len;
    } else {
	conn.aux_type_size = htobe32(-1);
    }
    if (I->aux_name) {
	int len = strlen(I->aux_name);
	conn.aux_name_size = htobe32(len);
	O->bytes_sent += len;
    } else {
	conn.aux_name_size = htobe32(-1);
    }
    errno = EINVAL;
    if (fwrite(&conn, sizeof(conn), 1, I->F_out) < 1) return 0;
    if (I->type && fwrite(I->type, strlen(I->type), 1, I->F_out) < 1)
	return 0;
    if (I->name && fwrite(I->name, strlen(I->name), 1, I->F_out) < 1)
	return 0;
    if (I->action && fwrite(I->action, strlen(I->action), 1, I->F_out) < 1)
	return 0;
    if (I->aux_type && fwrite(I->aux_type, strlen(I->aux_type), 1, I->F_out) < 1)
	return 0;
    if (I->aux_name && fwrite(I->aux_name, strlen(I->aux_name), 1, I->F_out) < 1)
	return 0;
    if (fflush(I->F_out) == EOF) return 1;
    errno = EINVAL;
    memset(&result, 0, sizeof(result));
    if (fread(&result, sizeof(result), 1, I->F_in) < 1) return 0;
    errno = 0;
    if (be64toh(result.magic) != CONN_OPEN_MAGIC) return 0;
    nf = be32toh(result.error);
    if (nf != ERRCODE_OK) {
	errno = ddb_decode_errno(nf);
	return 0;
    }
    O->supp = be32toh(result.supp);
    O->rwmax = be32toh(result.rwmax);
    if (rwmax < O->rwmax) O->rwmax = rwmax;
    O->block_size = be32toh(result.block_size);
    O->total_size = be64toh(result.total_size);
    O->blocks_present = be64toh(result.blocks_present);
    O->blocks_allocated = be64toh(result.blocks_allocated);
    return 1;
}

static int local_read(ddb_device_t * dev,
		int nblocks, ddb_block_t blocks[nblocks], int flags)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->read)
	return sub->ops->read(sub, nblocks, blocks, flags);
    return ddb_device_read_multi(sub, nblocks, blocks, flags);
}

static int local_write(ddb_device_t * dev,
		int nblocks, ddb_block_t blocks[nblocks])
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->write)
	return sub->ops->write(sub, nblocks, blocks);
    return ddb_device_write_multi(sub, nblocks, blocks);
}

static void local_info(ddb_device_t * dev, ddb_device_info_t * info) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->info)
	sub->ops->info(sub, info);
    else
	ddb_device_info(sub, info);
}

static int local_print(ddb_device_t * dev, int level,
		int (*f)(int, const char *, void *), void * arg, int v)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->print)
	return sub->ops->print(sub, level, f, arg, v);
    errno = ENOSYS;
    return -1;
}

static int local_report(ddb_device_t * dev,
		int (*f)(const char *, void *), void * arg)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->report)
	return sub->ops->report(sub, f, arg);
    errno = ENOSYS;
    return -1;
}

static int local_has_block(ddb_device_t * dev, off_t block) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->has_block)
	return sub->ops->has_block(sub, block);
    errno = ENOSYS;
    return -1;
}

static ddb_blocklist_t * local_blocks(ddb_device_t * dev) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->blocks)
	return sub->ops->blocks(sub);
    errno = ENOSYS;
    return NULL;
}

static ddb_blocklist_t * local_range(ddb_device_t * dev) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->range)
	return sub->ops->range(sub);
    errno = ENOSYS;
    return NULL;
}

static ddb_blocklist_t * local_has_blocks(ddb_device_t * dev,
		const ddb_blocklist_t * blocks)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->has_blocks)
	return sub->ops->has_blocks(sub, blocks);
    errno = ENOSYS;
    return NULL;
}

static int local_iterate(ddb_device_t * dev,
		int (*f)(off_t, off_t, void *), void * arg)
{
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->iterate)
	return sub->ops->iterate(sub, f, arg);
    errno = ENOSYS;
    return -1;
}

static int local_flush(ddb_device_t * dev) {
    ddb_device_t * sub = dev->info.devs[0];
    if (sub->ops->flush)
	return sub->ops->flush(sub);
    return 0;
}

static int prepare_remote(ddb_prepare_t * prepare, int all);

static int local_close(ddb_device_t * dev) {
    int sve = errno;
    ddb_prepare_t * prepare = extra(dev)->prepare;
    ddb_prepare_t * close = extra(dev)->close;
    while (prepare) {
	if (prepare->loaded) {
	    dlclose(prepare->loaded);
	    prepare->loaded = NULL;
	}
	prepare = prepare->next;
    }
    prepare_remote(close, 1);
    errno = sve;
    return 0;
}

static ddb_device_operations_t local_ops = {
    .type       = NULL,
    .read       = local_read,
    .write      = local_write,
    .info       = local_info,
    .print      = local_print,
    .has_block  = local_has_block,
    .blocks     = local_blocks,
    .range      = local_range,
    .has_blocks = local_has_blocks,
    .iterate    = local_iterate,
    .flush      = local_flush,
    .report     = local_report,
    .close      = local_close,
};

static void init_command(command_t * cmd) {
    cmd->status = 0;
    cmd->blocks = NULL;
    cmd->send_block = NULL;
    cmd->send_print = NULL;
    cmd->recv_info = NULL;
    cmd->send_rwdata = NULL;
    cmd->send_data = NULL;
    cmd->recv_data = NULL;
    cmd->skip_equal = 0;
}

static int count_blocks(off_t start, off_t end, void * _num) {
    int * num = _num;
    (*num) += sizeof(blocks_request_t);
    return 1;
}

int ddb_send_blocks(off_t start, off_t end, void * _fd) {
    FILE * F_out = _fd;
    blocks_request_t br;
    br.start = htobe64(start);
    br.end = htobe64(end);
    errno = EINVAL;
    if (fwrite(&br, sizeof(br), 1, F_out) < 1)
	return -1;
    return 1;
}

static int receive_info(FILE * F_in, ddb_device_info_t * info) {
    static char name[1024];
    info_result_t ir;
    int name_size, bytes = sizeof(ir);
    if (fread(&ir, sizeof(ir), 1, F_in) < 1)
	return 0;
    info->flags = be32toh(ir.flags);
    info->block_size = be32toh(ir.block_size);
    info->total_size = be64toh(ir.total_size);
    info->num_blocks = be64toh(ir.num_blocks);
    info->blocks_present = be64toh(ir.blocks_present);
    info->blocks_allocated = be64toh(ir.blocks_allocated);
    info->mtime = be64toh(ir.mtime);
    info->multi_device = be32toh(ir.multi_device);
    name_size = be32toh(ir.name_size);
    info->devs = NULL;
    info->name = NULL;
    if (name_size >= 0) {
	int recv = name_size < sizeof(name) ? name_size : (sizeof(name) - 1);
	bytes += name_size;
	info->name = name;
	if (fread(name, recv, 1, F_in) < 1) return 0;
	name[recv] = 0;
	name_size -= recv;
	while (name_size > 0) {
	    char buff[128];
	    recv = name_size > sizeof(buff) ? sizeof(buff) : name_size;
	    if (fread(buff, recv, 1, F_in) < 1) return 0;
	    name_size -= recv;
	}
    }
    return bytes;
}

static int send_command(const ddb_device_t * dev, int req, command_t * cmd) {
    struct sigaction old_pipe, new_pipe;
    request_t rr;
    struct timeval stime, etime;
    int data_size = 0, res_pipe = 0, ok = 0, data_sent = 0;
    if (! (extra(dev)->supp & (1 << req))) {
	errno = -ENOSYS;
	return 0;
    }
    gettimeofday(&stime, NULL);
    memset(&new_pipe, 0, sizeof(new_pipe));
    new_pipe.sa_handler = SIG_IGN;
    new_pipe.sa_flags = 0;
    sigemptyset(&new_pipe.sa_mask);
    if (sigaction(SIGPIPE, &new_pipe, &old_pipe) >= 0)
	res_pipe = 1;
    if (cmd->blocks) {
	ddb_blocklist_iterate(cmd->blocks, count_blocks, &data_size);
	data_sent |= DATA_BLOCKS;
	count_blocks(-1, 0, &data_size);
    }
    if (cmd->send_block) {
	data_size += sizeof(rw_request_t);
	data_sent |= DATA_BLOCK;
    }
    if (cmd->send_print) {
	data_size += sizeof(print_request_t);
	data_sent |= DATA_PRINT;
    }
    if (cmd->send_rwdata && cmd->send_rwdata->nblocks > 0) {
	data_size += sizeof(rw_spec_t);
	if (cmd->send_data) {
	    data_size += cmd->send_rwdata->nblocks
		      * (cmd->send_rwdata->data_size + sizeof(rw_request_t));
	    data_sent |= DATA_WRITE;
	}
	if (cmd->recv_data) {
	    data_size += cmd->send_rwdata->nblocks * sizeof(rw_request_t);
	    data_sent |= DATA_READ;
	    if (cmd->skip_equal) {
		data_size += cmd->send_rwdata->nblocks * DDB_CHECKSUM_LENGTH;
		data_sent |= DATA_CHKSUM;
	    }
	}
	data_sent |= DATA_RW;
    }
    /* now send prepared request */
    rr.request = htobe32(req);
    rr.data_size = htobe32(data_size);
    rr.data_sent = htobe32(data_sent);
    extra(dev)->bytes_sent += data_size + sizeof(rr);
    errno = EINVAL;
    if (fwrite(&rr, sizeof(rr), 1, extra(dev)->F_out) < 1)
	goto error;
    if (data_sent & DATA_BLOCKS) {
	if (ddb_blocklist_iterate(cmd->blocks, ddb_send_blocks,
				  extra(dev)->F_out) < 0)
	    goto error;
	if (ddb_send_blocks(-1, -1, extra(dev)->F_out) < 0)
	    goto error;
    }
    if (data_sent & DATA_BLOCK) {
	rw_request_t hr;
	hr.block = htobe64(*cmd->send_block);
	errno = EINVAL;
	if (fwrite(&hr, sizeof(hr), 1, extra(dev)->F_out) < 1)
	    goto error;
    }
    if (data_sent & DATA_PRINT) {
	print_request_t pr;
	pr.verbose = htobe32(cmd->send_print->verbose);
	pr.indent = htobe32(cmd->send_print->indent);
	errno = EINVAL;
	if (fwrite(&pr, sizeof(pr), 1, extra(dev)->F_out) < 1)
	    goto error;
    }
    if (data_sent & DATA_RW) {
	rw_spec_t sp;
	int i;
	sp.flags = htobe32(cmd->send_rwdata->flags);
	sp.nblocks = htobe32(cmd->send_rwdata->nblocks);
	sp.data_size = htobe32(cmd->send_rwdata->data_size);
	errno = EINVAL;
	if (fwrite(&sp, sizeof(sp), 1, extra(dev)->F_out) < 1)
	    goto error;
	if (data_sent & DATA_WRITE) {
	    for (i = 0; i < cmd->send_rwdata->nblocks; i++) {
		rw_request_t rr;
		rr.block = htobe64(cmd->send_data[i].block);
		errno = EINVAL;
		if (fwrite(&rr, sizeof(rr), 1, extra(dev)->F_out) < 1)
		    goto error;
		errno = EINVAL;
		if (fwrite(cmd->send_data[i].buffer,
			   cmd->send_rwdata->data_size, 1,
			   extra(dev)->F_out) < 1)
		    goto error;
	    }
	}
	if (data_sent & DATA_READ) {
	    for (i = 0; i < cmd->send_rwdata->nblocks; i++) {
		rw_request_t rr;
		rr.block = htobe64(cmd->recv_data[i].block);
		errno = EINVAL;
		if (fwrite(&rr, sizeof(rr), 1, extra(dev)->F_out) < 1)
		    goto error;
		if (data_sent & DATA_CHKSUM)
		    if (fwrite(cmd->recv_data[i].buffer, DDB_CHECKSUM_LENGTH,
			       1, extra(dev)->F_out) < 1)
			goto error;
	    }
	}
    }
    if (fflush(extra(dev)->F_out) == EOF) goto error;
    /* and get reply */
    errno = EINVAL;
    if (fread(&rr, sizeof(rr), 1, extra(dev)->F_in) < 1) goto error;
    cmd->status = be32toh(rr.request);
    data_size = be32toh(rr.data_size);
    data_sent = be32toh(rr.data_sent);
    extra(dev)->bytes_received += sizeof(rr) + data_size;
    if (! cmd->status) {
	errno = ddb_decode_errno(data_size);
	goto error;
    }
    if (cmd->recv_info) {
	int bytes;
	errno = EINVAL;
	if (data_size < sizeof(info_result_t)) goto error;
	if (! (data_sent & DATA_INFO)) goto error;
	data_size -= sizeof(info_result_t);
	bytes = receive_info(extra(dev)->F_in, cmd->recv_info);
	if (! bytes)
	    goto error;
	extra(dev)->bytes_received += bytes;
    }
    if (cmd->send_rwdata && cmd->send_rwdata->nblocks > 0) {
	if (cmd->send_data) {
	    rw_result_t rr;
	    int i;
	    errno = EINVAL;
	    if (data_size < cmd->send_rwdata->nblocks * sizeof(rr))
		goto error;
	    if (! (data_sent & DATA_WRITE)) goto error;
	    data_size -= cmd->send_rwdata->nblocks * sizeof(rr);
	    for (i = 0; i < cmd->send_rwdata->nblocks; i++) {
		errno = EINVAL;
		if (fread(&rr, sizeof(rr), 1, extra(dev)->F_in) < 1) goto error;
		cmd->send_data[i].result = be32toh(rr.result);
		cmd->send_data[i].error = ddb_decode_errno(be32toh(rr.error));
	    }
	}
	if (cmd->recv_data) {
	    rw_result_t rr;
	    int i;
	    errno = EINVAL;
	    if (data_size < cmd->send_rwdata->nblocks * sizeof(rr))
		goto error;
	    data_size -= cmd->send_rwdata->nblocks * sizeof(rr);
	    if (! (data_sent & DATA_READ)) goto error;
	    for (i = 0; i < cmd->send_rwdata->nblocks; i++) {
		int result;
		errno = EINVAL;
		if (fread(&rr, sizeof(rr), 1, extra(dev)->F_in) < 1) goto error;
		result = be32toh(rr.result);
		cmd->recv_data[i].error = ddb_decode_errno(be32toh(rr.error));
		switch (result) {
		    case RESULT_ERROR :
			cmd->recv_data[i].result = -1;
			break;
		    case RESULT_ZEROS :
			memset(cmd->recv_data[i].buffer, 0,
			       cmd->send_rwdata->data_size);
			cmd->recv_data[i].result = 1;
			break;
		    case RESULT_EQUAL :
			if (cmd->skip_equal) {
			    cmd->recv_data[i].result = 0;
			} else {
			    cmd->recv_data[i].result = -1;
			    cmd->recv_data[i].error = EINVAL;
			}
			break;
		    case RESULT_DATA :
			errno = EINVAL;
			if (data_size < cmd->send_rwdata->data_size)
			    goto error;
			data_size -= cmd->send_rwdata->data_size;
			if (fread(cmd->recv_data[i].buffer,
				  cmd->send_rwdata->data_size, 1,
				  extra(dev)->F_in) < 1)
			    goto error;
			cmd->recv_data[i].result = 1;
			break;
		    default:
			cmd->recv_data[i].result = -1;
			cmd->recv_data[i].error = EINVAL;
			break;
		}
	    }
	}
    }
    if (data_size > 0) {
	errno = EINVAL;
	goto error;
    }
    ok = 1;
error:
    if (res_pipe) {
	int sve = errno;
	sigaction(SIGPIPE, &old_pipe, NULL);
	errno = sve;
    }
    gettimeofday(&etime, NULL);
    if (etime.tv_usec < stime.tv_usec) {
	etime.tv_usec += 1000000;
	etime.tv_sec--;
    }
    extra(dev)->reqtime.tv_usec += etime.tv_usec - stime.tv_usec;
    extra(dev)->reqtime.tv_sec += etime.tv_sec - stime.tv_sec;
    if (extra(dev)->reqtime.tv_usec >= 1000000) {
	extra(dev)->reqtime.tv_usec -= 1000000;
	extra(dev)->reqtime.tv_sec ++;
    }
    extra(dev)->requests++;
    return ok;
}

static int reopen_remote(ddb_device_t * dev);

static int remote_read(ddb_device_t * dev,
		int nblocks, ddb_block_t blocks[nblocks], int flags)
{
    int ok = 0;
    while (nblocks > 0) {
	command_t cmd;
	rwdata_t rw;
	int todo = nblocks < extra(dev)->rwmax ? nblocks : extra(dev)->rwmax;
	init_command(&cmd);
	rw.nblocks = todo;
	rw.flags = flags;
	if ((flags & DDB_READ_DATA_MASK) == DDB_READ_CHECKSUM) {
	    rw.data_size = DDB_CHECKSUM_LENGTH;
	    rw.flags &= ~DDB_READ_MAYBE;
	} else {
	    rw.data_size = dev->info.block_size;
	    if (flags & DDB_READ_MAYBE)
		cmd.skip_equal = 1;
	}
	cmd.send_rwdata = &rw;
	cmd.recv_data = blocks;
	if (! send_command(dev, REQ_READ, &cmd)) {
	    int i, sve = errno;
	    if (reopen_remote(dev))
		continue;
	    for (i = 0; i < nblocks; i++) {
		blocks[i].error = sve;
		blocks[i].result = -1;
	    }
	    return ok;
	}
	ok += cmd.status - 1;
	nblocks -= todo;
	blocks += todo;
    }
    return ok;
}

static int remote_write(ddb_device_t * dev,
		int nblocks, ddb_block_t blocks[nblocks])
{
    int ok = 0;
    while (nblocks > 0) {
	command_t cmd;
	rwdata_t rw;
	int todo = nblocks < extra(dev)->rwmax ? nblocks : extra(dev)->rwmax;
	init_command(&cmd);
	rw.nblocks = todo;
	rw.flags = 0;
	rw.data_size = dev->info.block_size;
	cmd.send_rwdata = &rw;
	cmd.send_data = blocks;
	if (! send_command(dev, REQ_WRITE, &cmd)) {
	    int i, sve = errno;
	    if (reopen_remote(dev))
		continue;
	    for (i = 0; i < nblocks; i++) {
		blocks[i].error = sve;
		blocks[i].result = -1;
	    }
	    return ok;
	}
	ok += cmd.status - 1;
	nblocks -= todo;
	blocks += todo;
    }
    return ok;
}

static void remote_info(ddb_device_t * dev, ddb_device_info_t * info) {
    command_t cmd;
    init_command(&cmd);
    *info = dev->info;
    cmd.recv_info = info;
    while (! send_command(dev, REQ_INFO, &cmd))
	if (! reopen_remote(dev))
	    return;
}

static int remote_print(ddb_device_t * dev, int level,
		int (*f)(int, const char *, void *), void * arg, int v)
{
    char line[1024];
    command_t cmd;
    print_t prn;
    int ok = 1;
    init_command(&cmd);
    prn.verbose = v;
    prn.indent = level;
    cmd.send_print = &prn;
    while (! send_command(dev, REQ_PRINT, &cmd))
	if (! reopen_remote(dev))
	    return -1;
    while (1) {
	print_request_t pr;
	int length, indent, blk;
	errno = EINVAL;
	if (fread(&pr, sizeof(pr), 1, extra(dev)->F_in) < 1)
	    return -1;
	extra(dev)->bytes_received += sizeof(pr);
	length = be32toh(pr.verbose);
	indent = be32toh(pr.indent);
	if (length < 0) {
	    ok = indent;
	    break;
	}
	extra(dev)->bytes_received += length;
	blk = length < sizeof(line) ? length : (sizeof(line) - 1);
	errno = EINVAL;
	if (fread(line, blk, 1, extra(dev)->F_in) < 1)
	    return -1;
	length -= blk;
	line[blk] = 0;
	if (ok >= 0) ok = f(indent, line, arg);
	while (length > 0) {
	    blk = length < sizeof(line) ? sizeof(line) - 1 : length;
	    errno = EINVAL;
	    if (fread(line, blk, 1, extra(dev)->F_in) < 1)
		return -1;
	    length -= blk;
	}
    }
    return ok;
}

static int remote_report(ddb_device_t * dev,
		int (*f)(const char *, void *), void * arg)
{
    char line[1024];
    int ok = 0, rs;
    if (extra(dev)->supp & (1 << REQ_REPORT)) {
	command_t cmd;
	init_command(&cmd);
	while (! send_command(dev, REQ_REPORT, &cmd))
	    if (! reopen_remote(dev))
		return -1;
	while (1) {
	    print_request_t pr;
	    int length, blk;
	    errno = EINVAL;
	    if (fread(&pr, sizeof(pr), 1, extra(dev)->F_in) < 1)
		return -1;
	    extra(dev)->bytes_received += sizeof(pr);
	    length = be32toh(pr.verbose);
	    if (length < 0) {
		ok = be32toh(pr.indent);
		break;
	    }
	    extra(dev)->bytes_received += length;
	    blk = length < sizeof(line) ? length : (sizeof(line) - 1);
	    errno = EINVAL;
	    if (fread(line, blk, 1, extra(dev)->F_in) < 1)
		return -1;
	    length -= blk;
	    line[blk] = 0;
	    if (ok >= 0) ok = f(line, arg);
	    while (length > 0) {
		blk = length < sizeof(line) ? sizeof(line) - 1 : length;
		errno = EINVAL;
		if (fread(line, blk, 1, extra(dev)->F_in) < 1)
		    return -1;
		length -= blk;
	    }
	}
    }
    snprintf(line, sizeof(line), "Bytes sent: %lld",
	     (long long)extra(dev)->bytes_sent);
    rs = f(line, arg);
    if (rs < 0 && ok >= 0) ok = rs;
    snprintf(line, sizeof(line), "Bytes received: %lld",
	     (long long)extra(dev)->bytes_received);
    rs = f(line, arg);
    if (rs < 0 && ok >= 0) ok = rs;
    if (extra(dev)->requests > 0) {
	double reqtime = (double)extra(dev)->reqtime.tv_sec
		       + (double)extra(dev)->reqtime.tv_usec / 1e6;
	snprintf(line, sizeof(line),
		 "Requests sent: %d in %.3fs (%.6f s/request)\n",
		 extra(dev)->requests, reqtime,
		 reqtime / (double)extra(dev)->requests);
	rs = f(line, arg);
	if (rs < 0 && ok >= 0) ok = rs;
    }
    return ok;
}

static int remote_has_block(ddb_device_t * dev, off_t block) {
    command_t cmd;
    init_command(&cmd);
    cmd.send_block = &block;
    while (! send_command(dev, REQ_HAS_BLOCK, &cmd))
	if (! reopen_remote(dev))
	    return -1;
    return cmd.status - 1;
}

static ddb_blocklist_t * receive_blocks(ddb_device_t * dev,
		int op, command_t * cmd)
{
    ddb_blocklist_t * res;
    int sve;
    res = ddb_blocklist_new();
    if (! res) return NULL;
    while (! send_command(dev, op, cmd))
	if (! reopen_remote(dev))
	    goto error;
    while (1) {
	blocks_request_t br;
	off_t start, end;
	errno = EINVAL;
	if (fread(&br, sizeof(br), 1, extra(dev)->F_in) < 1)
	    goto error;
	extra(dev)->bytes_received += sizeof(br);
	start = be64toh(br.start);
	end = be64toh(br.end);
	if (start < 0) break;
	if (ddb_blocklist_add(res, start, end) < 0) goto error;
    }
    return res;
error:
    sve = errno;
    ddb_blocklist_free(res);
    errno = sve;
    return NULL;
}

static ddb_blocklist_t * remote_blocks(ddb_device_t * dev) {
    command_t cmd;
    init_command(&cmd);
    return receive_blocks(dev, REQ_BLOCKS, &cmd);
}

static ddb_blocklist_t * remote_range(ddb_device_t * dev) {
    command_t cmd;
    init_command(&cmd);
    return receive_blocks(dev, REQ_RANGE, &cmd);
}

static ddb_blocklist_t * remote_has_blocks(ddb_device_t * dev,
		const ddb_blocklist_t * blocks)
{
    command_t cmd;
    init_command(&cmd);
    cmd.blocks = blocks;
    return receive_blocks(dev, REQ_HAS_BLOCKS, &cmd);
}

static int remote_iterate(ddb_device_t * dev,
		int (*f)(off_t, off_t, void *), void * arg)
{
    command_t cmd;
    int ok = 0;
    init_command(&cmd);
    while (! send_command(dev, REQ_ITERATE, &cmd))
	if (! reopen_remote(dev))
	    return -1;
    while (1) {
	blocks_request_t br;
	off_t start, end;
	errno = EINVAL;
	if (fread(&br, sizeof(br), 1, extra(dev)->F_in) < 1)
	    return -1;
	extra(dev)->bytes_received += sizeof(br);
	start = be64toh(br.start);
	end = be64toh(br.end);
	if (start < 0) {
	    if (ok >= 0) ok = end;
	    break;
	}
	if (ok >= 0) ok = f(start, end, arg);
    }
    return ok;
}

static int remote_flush(ddb_device_t * dev) {
    command_t cmd;
    init_command(&cmd);
    return send_command(dev, REQ_FLUSH, &cmd) ? 0 : -1;
}

static void terminate(pid_t pid) {
    int i;
    if (waitpid(pid, NULL, WNOHANG) > 0) return;
    /* wait 50ms for it to finish */
    for (i = 0; i < 50; i++) {
	usleep(1000);
	if (waitpid(pid, NULL, WNOHANG) > 0) return;
    }
    kill(pid, SIGTERM);
    if (waitpid(pid, NULL, WNOHANG) > 0) return;
    /* wait 10ms for it to get that SIGTERM */
    for (i = 0; i < 10; i++) {
	usleep(1000);
	if (waitpid(pid, NULL, WNOHANG) > 0) return;
    }
    /* not going away, ask more forcefully */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static int remote_close(ddb_device_t * dev) {
    command_t cmd;
    int ret, sve;
    init_command(&cmd);
    ret = send_command(dev, REQ_CLOSE, &cmd) ? 0 : -1;
    sve = errno;
    while (extra(dev)->prepare) {
	if (extra(dev)->prepare->loaded) {
	    dlclose(extra(dev)->prepare->loaded);
	    extra(dev)->prepare->loaded = NULL;
	}
	extra(dev)->prepare = extra(dev)->prepare->next;
    }
    prepare_remote(extra(dev)->close, 1);
    extra(dev)->close = NULL;
    if (extra(dev)->F_in) {
	fclose(extra(dev)->F_in);
	extra(dev)->F_in = NULL;
    }
    if (extra(dev)->F_out) {
	fclose(extra(dev)->F_out);
	extra(dev)->F_out = NULL;
    }
    if (extra(dev)->pid > 0) {
	terminate(extra(dev)->pid);
	extra(dev)->pid = -1;
    }
    errno = sve;
    return ret;
}

static ddb_device_operations_t remote_ops = {
    .type       = NULL,
    .read       = remote_read,
    .write      = remote_write,
    .info       = remote_info,
    .print      = remote_print,
    .has_block  = remote_has_block,
    .blocks     = remote_blocks,
    .range      = remote_range,
    .has_blocks = remote_has_blocks,
    .iterate    = remote_iterate,
    .flush      = remote_flush,
    .report     = remote_report,
    .close      = remote_close,
};

static ddb_device_t * open_local(ddb_device_t * sub, int flags,
		ddb_prepare_t * prepare, ddb_prepare_t * close)
{
    ddb_device_t * dev =
	ddb_device_open_multi(sizeof(extra_t), 1, &sub, flags);
    if (! dev) return NULL;
    dev->ops = &extra(dev)->ops;
    ddb_copy_ops(&extra(dev)->ops, sub, &local_ops);
    extra(dev)->ops.type = NULL;
    extra(dev)->prepare = prepare;
    extra(dev)->close = close;
    extra(dev)->F_in = NULL;
    extra(dev)->F_out = NULL;
    extra(dev)->pid = -1;
    extra(dev)->supp = 0;
    return dev;
}

static void set_ops_supp(ddb_device_t * dev, int supp,
		const ddb_device_operations_t * ops)
{
#define __ops(name, code) \
    extra(dev)->ops.name = (supp & (1 << code)) ? ops->name : NULL
    dev->ops = &extra(dev)->ops;
    extra(dev)->ops.type = ops->type;
    __ops(read, REQ_READ);
    __ops(write, REQ_WRITE);
    __ops(info, REQ_INFO);
    __ops(print, REQ_PRINT);
    __ops(has_block, REQ_HAS_BLOCK);
    __ops(blocks, REQ_BLOCKS);
    __ops(range, REQ_RANGE);
    __ops(has_blocks, REQ_HAS_BLOCKS);
    __ops(iterate, REQ_ITERATE);
    __ops(flush, REQ_FLUSH);
    extra(dev)->ops.report = ops->report;
    extra(dev)->ops.close = ops->close;
#undef __ops
}

ddb_device_t * ddb_device_pipe(FILE * F_in, FILE * F_out, pid_t pid,
		int flags, size_t block_size, off_t total_size,
		const char * name, const char * type)
{
    send_greeting_in_t I;
    send_greeting_out_t O;
    ddb_device_t * dev;
    O.mtime = 0;
    O.blocks_present = 0;
    O.blocks_allocated = 0;
    O.supp = 0;
    O.block_size = block_size;
    O.total_size = total_size;
    I.F_in = F_in;
    I.F_out = F_out;
    I.name = name;
    I.type = type;
    I.action = NULL;
    I.aux_name = NULL;
    I.aux_type = NULL;
    I.flags = flags;
    if (! send_greeting(&I, &O))
	return NULL;
    dev = ddb_device_open_multi(sizeof(extra_t), 0, NULL, flags);
    if (! dev) return NULL;
    ddb_fill_info(dev, O.total_size, O.block_size,
		  O.blocks_present, O.mtime, O.blocks_allocated);
    dev->info.is_remote = 1;
    set_ops_supp(dev, O.supp, &remote_ops);
    extra(dev)->descr = NULL;
    extra(dev)->prepare = NULL;
    extra(dev)->close = NULL;
    extra(dev)->F_in = F_in;
    extra(dev)->F_out = F_out;
    extra(dev)->pid = pid;
    extra(dev)->supp = O.supp;
    extra(dev)->rwmax = ddb_copy_block(0);
    extra(dev)->retry_max = 0;
    extra(dev)->retry_delay = 0;
    extra(dev)->bytes_sent = O.bytes_sent;
    extra(dev)->bytes_received = O.bytes_received;
    extra(dev)->requests = 0;
    extra(dev)->reqtime.tv_sec = 0;
    extra(dev)->reqtime.tv_usec = 0;
    return dev;
}

/* prepare for a connection */
static int prepare_remote(ddb_prepare_t * prepare, int all) {
    while (prepare) {
	int (*init)(int, const char *[]);
	const char * modpath;
	pid_t pid;
	int status;
	switch (prepare->type) {
	    case ddb_prepare_load :
		modpath = getenv("DDB_MODPATH");
		if (! modpath) modpath = default_modpath;
		{
		    const char * module = prepare->program;
		    char modname[strlen(modpath) + strlen(module) + 6];
		    sprintf(modname, "%s/%s.so", modpath, module);
		    prepare->loaded = dlopen(modname, RTLD_NOW);
		}
		if (! prepare->loaded) return 0;
		init = dlsym(prepare->loaded, "init");
		if (! init) goto invalid;
		if (! init(prepare->nargs, prepare->args) && ! all)
		    return 0;
		break;
	    case ddb_prepare_run :
		pid = fork();
		if (pid < 0) return 0;
		if (pid == 0) {
		    const char * args[2 + prepare->nargs];
		    int i;
		    args[0] = prepare->program;
		    for (i = 0; i < prepare->nargs; i++)
			args[i + 1] = prepare->args[i];
		    args[prepare->nargs + 1] = NULL;
		    execvp(prepare->program, (char * const *)args);
		    fprintf(stderr, "Cannot run %s\n", prepare->program);
		    exit(1);
		}
		while (waitpid(pid, &status, 0) < 0) {
		    if (errno != EINTR) {
			if (all) goto next;
			return 0;
		    }
		}
		if (WIFEXITED(status)) {
		    int c = WEXITSTATUS(status);
		    if (! c) break;
		    fprintf(stderr, "%s exited with status %d\n",
			    prepare->program, c);
		    goto invalid;
		}
		if (WIFSIGNALED(status)) {
		    fprintf(stderr, "%s terminated by signal %d\n",
			    prepare->program, WTERMSIG(status));
		    goto invalid;
		}
		fprintf(stderr, "%s returned invalid status %d?\n",
			prepare->program, status);
		if (! all) goto invalid;
	    next:
		break;
	}
	prepare = prepare->next;
    }
    return 1;
invalid:
    errno = EINVAL;
    return 0;
}

/* try connecting to a pipe */
static int connect_pipe(ddb_connect_t * connect, pid_t * pid,
		FILE ** F_in, FILE ** F_out)
{
    int pipe_in[2], pipe_out[2], sve;
    if (pipe(pipe_in) < 0) return 0;
    if (pipe(pipe_out) < 0) goto error_close_in;
    *pid = fork();
    if (*pid < 0) goto error_close_both;
    if (*pid == 0) {
	const char * args[1 + connect->nargs];
	int i;
	/* pipe_in/_out refers to parent's direction, so here we
	 * input from pipe_out and output to pipe_in */
	close(pipe_out[1]);
	if (dup2(pipe_out[0], 0) < 0) exit(254);
	close(pipe_out[0]);
	close(pipe_in[0]);
	if (dup2(pipe_in[1], 1) < 0) exit(254);
	close(pipe_in[1]);
	for (i = 0; i < connect->nargs; i++)
	    args[i] = connect->args[i];
	args[connect->nargs] = NULL;
	execvp(args[0], (char * const *)args);
	fprintf(stderr, "Cannot run %s\n", connect->module);
	exit(1);
    }
    close(pipe_in[1]);
    *F_in = fdopen(pipe_in[0], "r");
    if (! *F_in) goto error_close_both;
    close(pipe_out[0]);
    *F_out = fdopen(pipe_out[1], "w");
    if (*F_out) return 1;
    sve = errno;
    fclose(*F_in);
    errno = sve;
    goto error_close_in;
error_close_both:
    sve = errno;
    close(pipe_out[0]);
    close(pipe_out[1]);
    errno = sve;
error_close_in:
    sve = errno;
    close(pipe_in[0]);
    close(pipe_in[1]);
    errno = sve;
    return 0;
}

/* try connecting to TCP host and port */
static int connect_tcp(ddb_connect_t * C, FILE ** F_in, FILE ** F_out) {
    struct addrinfo hints, *res, *rp;
    const char * host = C->nargs > 0 ? C->args[0] : NULL;
    const char * port = C->nargs > 1 ? C->args[1] : NULL;
    int err;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;
    err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
	/* approximate mapping to errno value */
	switch (err) {
	    case EAI_SERVICE :
		errno = EADDRNOTAVAIL;
		break;
	    case EAI_AGAIN :
		errno = EAGAIN;
		break;
	    case EAI_FAIL :
	    case EAI_NONAME :
		errno = ENOENT;
		break;
	    case EAI_MEMORY :
		errno = ENOMEM;
		break;
	    case EAI_SYSTEM :
		break;
	    default :
		errno = EINVAL;
		break;
	}
	return 0;
    }
    for (rp = res; rp; rp = rp->ai_next) {
	int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol), sve;
	if (fd < 0) continue;
	*F_in = NULL;
	if (connect(fd, rp->ai_addr, rp->ai_addrlen) < 0) goto error;
	*F_in = fdopen(fd, "r");
	if (! *F_in) goto error;
	*F_out = fdopen(fd, "w");
	if (! *F_out) goto error;
	freeaddrinfo(res);
	return 1;
    error:
	sve = errno;
	close(fd);
	if (*F_in) fclose(*F_in);
	errno = sve;
	continue;
    }
    freeaddrinfo(res);
    return 0;
}

/* try reopening a device after an error */
static int reopen_remote(ddb_device_t * dev) {
    ddb_remote_t * descr = extra(dev)->descr;
    ddb_prepare_t * prepare;
    ddb_connect_t * connect;
    int sve = errno;
    if (! descr) return 0;
    if (extra(dev)->retry_max < 1) {
	errno = sve;
	return 0;
    }
    extra(dev)->retry_max--;
    /* close old device */
    remote_close(dev);
    /* sleep specified interval */
    if (extra(dev)->retry_delay > 0)
	sleep(extra(dev)->retry_delay);
    /* prepare to reconnect */
    prepare = descr->retry_prepare;
    connect = descr->retry_connect;
    /* prepare for connection */
    if (! prepare_remote(prepare, 0)) goto error;
    /* now find the first thing we can connect to; note that we only
     * look at "remote" types here */
    errno = EINVAL;
    while (connect) {
	send_greeting_in_t I;
	send_greeting_out_t O;
	pid_t pid = -1;
	I.F_in = I.F_out = NULL;
	switch (connect->type) {
	    case ddb_connect_open:
	    case ddb_connect_call:
	    case ddb_connect_acall:
		/* these are all local devices, so don't apply on a
		 * retry for a "real" remote */
		break;
	    case ddb_connect_pipe:
		/* try running this program */
		if (! connect_pipe(connect, &pid, &I.F_in, &I.F_out))
		    break;
		goto pipe_or_tcp;
	    case ddb_connect_tcp:
		/* open connection to this host */
		if (! connect_tcp(connect, &I.F_in, &I.F_out))
		    break;
		pid = -1;
	    pipe_or_tcp:
		/* send initial packet; note that we've removed O_EXCL
		 * from the flags as that will now fail as it's a
		 * reconnect and the destination will have been created */
		I.flags = dev->info.flags & ~O_EXCL;
		I.name = connect->module;
		I.type = connect->function;
		I.action = NULL;
		I.aux_name = NULL;
		I.aux_type = NULL;
		O.mtime = 0;
		O.blocks_present = 0;
		O.blocks_allocated = 0;
		O.total_size = 0;
		O.block_size = 0;
		O.supp = 0;
		O.rwmax = 0;
		if (! send_greeting(&I, &O))
		    goto connect_error;
		/* check consistency */
		if (O.block_size != dev->info.block_size) goto invalid;
		if (O.total_size != dev->info.total_size) goto invalid;
		if (O.supp != extra(dev)->supp) goto invalid;
		dev->info.blocks_present = O.blocks_present;
		dev->info.blocks_allocated = O.blocks_allocated;
		dev->info.mtime = O.mtime;
		extra(dev)->prepare = prepare;
		extra(dev)->close = descr->retry_close;
		extra(dev)->F_in = I.F_in;
		extra(dev)->F_out = I.F_out;
		extra(dev)->pid = pid;
		extra(dev)->bytes_sent = O.bytes_sent;
		extra(dev)->bytes_received = O.bytes_received;
		return 1;
	}
	connect = connect->next;
	continue;
    invalid:
	errno = EINVAL;
    connect_error:
	sve = errno;
	if (I.F_in) fclose(I.F_in);
	if (I.F_out) fclose(I.F_out);
	if (pid >= 0) terminate(pid);
	errno = sve;
	goto error;
    }
    /* failed to connect... */
error:
    sve = errno;
    prepare = descr->retry_prepare;
    while (prepare) {
	if (prepare->loaded) {
	    dlclose(prepare->loaded);
	    prepare->loaded = NULL;
	}
	prepare = prepare->next;
    }
    prepare_remote(descr->retry_close, 1);
    errno = sve;
    return 0;
}

ddb_device_t * ddb_device_open_remote(ddb_remote_t * descr,
		int flags, off_t total_size)
{
    ddb_prepare_t * prepare = descr->prepare;
    ddb_connect_t * connect = descr->connect;
    int sve;
    /* prepare for connection */
    if (! prepare_remote(prepare, 0)) goto error;
    /* now find the first thing we can connect to */
    errno = EINVAL;
    while (connect) {
	ddb_device_t * sub = NULL, * dev = NULL;
	FILE * F_in = NULL, * F_out = NULL;
	pid_t pid = -1;
	switch (connect->type) {
	    case ddb_connect_open:
		sub = ddb_device_open_local(connect->module, connect->function,
					    descr->block_size, flags,
					    total_size);
		if (! sub) break;
		dev = open_local(sub, flags, descr->prepare, descr->close);
		if (dev) return dev;
		goto connect_error;
	    case ddb_connect_call:
		/* find module */
		prepare = descr->prepare;
		while (prepare) {
		    ddb_prepare_t * P = prepare;
		    ddb_device_t * (*devopen)(int, const char *[]);
		    prepare = prepare->next;
		    if (strcmp(P->program, connect->module) != 0) continue;
		    /* found module, find function and run it */
		    devopen = dlsym(prepare->loaded, connect->function);
		    if (! devopen) continue;
		    sub = devopen(connect->nargs, connect->args);
		    if (! sub) continue;
		    dev = open_local(sub, flags, descr->prepare, descr->close);
		    if (dev) return dev;
		    goto connect_error;
		}
		break;
	    case ddb_connect_pipe:
		/* try running this program */
		if (! connect_pipe(connect, &pid, &F_in, &F_out))
		    break;
		goto pipe_or_tcp;
	    case ddb_connect_tcp:
		/* open connection to this host */
		if (! connect_tcp(connect, &F_in, &F_out))
		    break;
		pid = -1;
	    pipe_or_tcp:
		dev = ddb_device_pipe(F_in, F_out, pid,
				      flags, descr->block_size, total_size,
				      connect->module, connect->function);
		if (dev) {
		    extra(dev)->descr = descr;
		    extra(dev)->prepare = descr->prepare;
		    extra(dev)->close = descr->close;
		    extra(dev)->retry_max = descr->retry_max;
		    extra(dev)->retry_delay = descr->retry_delay;
		    return dev;
		}
		if (errno != 0)
		    goto connect_error;
		break;
	    case ddb_connect_acall:
		/* not used for normal device access */
		break;
	}
	connect = connect->next;
	continue;
    connect_error:
	sve = errno;
	if (sub) ddb_device_close(sub);
	if (F_in) fclose(F_in);
	if (F_out) fclose(F_out);
	if (pid >= 0) terminate(pid);
	errno = sve;
	goto error;
    }
    /* failed to connect... */
error:
    sve = errno;
    prepare = descr->prepare;
    while (prepare) {
	if (prepare->loaded) {
	    dlclose(prepare->loaded);
	    prepare->loaded = NULL;
	}
	prepare = prepare->next;
    }
    prepare_remote(descr->close, 1);
    errno = sve;
    return NULL;
}

static int action_remote(FILE * F_in, FILE * F_out, const char * action,
		const char * name, const char * type,
		const char * aux_name, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg)
{
    send_greeting_in_t I;
    send_greeting_out_t O;
    I.F_in = F_in;
    I.F_out = F_out;
    I.flags = 0;
    I.name = name;
    I.type = type;
    I.action = action;
    I.aux_name = aux_name;
    I.aux_type = aux_type;
    O.mtime = 0;
    O.blocks_present = 0;
    O.blocks_allocated = 0;
    O.total_size = 0;
    O.block_size = 0;
    O.supp = 0;
    O.rwmax = 0;
    // XXX report is not currently supported and ignored
    if (! send_greeting(&I, &O))
	return -1;
    return 0;
}

/* perform a remote action */
int ddb_action_remote(ddb_remote_t * descr, const char * action,
		const char * aux_name, const char * aux_type,
		int freq, void (*report)(void *, const char *), void * arg)
{
    ddb_prepare_t * prepare = descr->prepare;
    ddb_connect_t * connect = descr->connect;
    int sve;
    /* prepare for connection */
    if (! prepare_remote(prepare, 0)) goto error;
    /* now find the first thing we can connect to */
    errno = EINVAL;
    while (connect) {
	FILE * F_in, * F_out;
	pid_t pid;
	int ok;
	switch (connect->type) {
	    case ddb_connect_open:
		errno = 0;
		ok = ddb_action(connect->module, connect->function,
				action, aux_name, aux_type, freq, report, arg);
		if (ok >= 0) return ok;
		if (errno != EINVAL) return ok;
		break;
	    case ddb_connect_acall:
		/* find module */
		prepare = descr->prepare;
		while (prepare) {
		    ddb_prepare_t * P = prepare;
		    int (*devact)(int, const char *[], const char *,
				  const char *, const char *,
				  int, void (*)(void *, const char *), void *);
		    prepare = prepare->next;
		    if (strcmp(P->program, connect->module) != 0) continue;
		    /* found module, find function and run it */
		    devact = dlsym(prepare->loaded, connect->function);
		    if (! devact) continue;
		    errno = 0;
		    ok = devact(connect->nargs, connect->args,
				action, aux_name, aux_type, freq, report, arg);
		    if (ok >= 0) return ok;
		    if (errno != 0) return ok;
		    break;
		}
		break;
	    case ddb_connect_pipe:
		if (! connect_pipe(connect, &pid, &F_in, &F_out))
		    break;
	    pipe_or_tcp:
		ok = action_remote(F_in, F_out, action,
				   connect->module, connect->function,
				   aux_name, aux_type, freq, report, arg);
		if (F_in)
		    fclose(F_in);
		if (F_out)
		    fclose(F_out);
		if (pid > 0)
		    terminate(pid);
		if (ok >= 0) return ok;
		break;
	    case ddb_connect_tcp:
		if (! connect_tcp(connect, &F_in, &F_out))
		    break;
		goto pipe_or_tcp;
	    case ddb_connect_call:
		/* not used for actions */
		break;
	}
	connect = connect->next;
    }
    /* failed to connect... */
error:
    sve = errno;
    prepare = descr->prepare;
    while (prepare) {
	if (prepare->loaded) {
	    dlclose(prepare->loaded);
	    prepare->loaded = NULL;
	}
	prepare = prepare->next;
    }
    prepare_remote(descr->close, 1);
    errno = sve;
    return -1;
}

