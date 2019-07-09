/* ddb-plugin.c - functions to allow a program run as a ddb plugin
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
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <ddb.h>
#include "ddb-private.h"
#include "ddb-remote.h"

struct ddb_plugin_s {
    ddb_device_t * dev;
    ddb_block_t * rblocks;
    ddb_block_t * wblocks;
    unsigned char ** cblocks;
    FILE * F_in;
    FILE * F_out;
    int block_size;
    int rwmax;
    const char * action;
    const char * name;
    const char * type;
    const char * aux_name;
    const char * aux_type;
    ddb_block_t blocks[0];
};

/* receive connection greeting, open device and send back reply */
ddb_plugin_t * ddb_plugin_init(FILE * F_in, FILE * F_out) {
    conn_open_t conn;
    conn_result_t result;
    ddb_device_t * dev = NULL;
    ddb_plugin_t * res = NULL;
    char * type = NULL, * name = NULL;
    off_t total_size = 0, blocks_present = 0, blocks_allocated = 0;
    size_t block_size = 0;
    int flags, cf, type_size, name_size, action_size, sve = EINVAL, rwmax;
    int aux_type_size, aux_name_size;
    errno = EINVAL;
    if (fread(&conn, sizeof(conn), 1, F_in) < 1)
	return NULL;
    errno = EINVAL;
    if (be64toh(conn.magic) != CONN_OPEN_MAGIC) return 0;
    total_size = be64toh(conn.total_size);
    block_size = be32toh(conn.block_size);
    cf = be32toh(conn.flags);
    switch (cf & CONN_OPEN_ACCESS) {
	case CONN_OPEN_RDONLY: flags = O_RDONLY; break;
	case CONN_OPEN_WRONLY: flags = O_WRONLY; break;
	case CONN_OPEN_RDWR: flags = O_RDWR; break;
	default: errno = EINVAL; goto error;
    }
    if (cf & CONN_OPEN_CREAT) flags |= O_CREAT;
    if (cf & CONN_OPEN_EXCL) flags |= O_EXCL;
    type_size = be32toh(conn.type_size);
    name_size = be32toh(conn.name_size);
    action_size = be32toh(conn.action_size);
    aux_type_size = be32toh(conn.aux_type_size);
    aux_name_size = be32toh(conn.aux_name_size);
    rwmax = be32toh(conn.rwmax);
    cf = ddb_copy_block(rwmax);
    if (cf < rwmax) rwmax = cf;
    if (type_size >= 0) {
	type = malloc(1 + type_size);
	if (! type) goto error;
	errno = EINVAL;
	if (fread(type, type_size, 1, F_in) < 1) goto error;
	type[type_size] = 0;
    }
    if (name_size >= 0) {
	name = malloc(1 + name_size);
	if (! name) goto error;
	errno = EINVAL;
	if (fread(name, name_size, 1, F_in) < 1) goto error;
	name[name_size] = 0;
    }
    if (action_size >= 0) {
	size_t extra = action_size + 1;
	if (name) extra += name_size + 1;
	if (type) extra += type_size + 1;
	if (aux_type_size >= 0) extra += aux_type_size + 1;
	if (aux_name_size >= 0) extra += aux_name_size + 1;
	res = malloc(sizeof(ddb_plugin_t) + extra);
	if (res) {
	    char * ptr;
	    res->rblocks = NULL;
	    res->wblocks = NULL;
	    res->cblocks = NULL;
	    ptr = (void *)&res->blocks;
	    res->action = ptr;
	    errno = EINVAL;
	    if (fread(ptr, action_size, 1, F_in) < 1) goto error;
	    ptr[action_size] = 0;
	    ptr += 1 + action_size;
	    if (type) {
		res->type = ptr;
		strcpy(ptr, type);
		ptr += 1 + type_size;
	    } else {
		res->type = NULL;
	    }
	    if (name) {
		res->name = ptr;
		strcpy(ptr, name);
		ptr += 1 + name_size;
	    } else {
		res->name = NULL;
	    }
	    if (aux_type_size >= 0) {
		res->aux_type = ptr;
		if (fread(ptr, aux_type_size, 1, F_in) < 1) goto error;
		ptr[aux_type_size] = 0;
		ptr += 1 + aux_type_size;
	    } else {
		res->aux_type = NULL;
	    }
	    if (aux_name_size >= 0) {
		res->aux_name = ptr;
		if (fread(ptr, aux_name_size, 1, F_in) < 1) goto error;
		ptr[aux_name_size] = 0;
		ptr += 1 + aux_name_size;
	    } else {
		res->aux_name = NULL;
	    }
	    res->dev = NULL;
	    res->block_size = 0;
	    res->rwmax = rwmax;
	    res->F_in = F_in;
	    res->F_out = F_out;
	} else {
	    sve = errno;
	}
    } else {
	dev = ddb_device_open(name, type, block_size, flags, total_size);
	if (dev) {
	    ddb_device_info_t info;
	    int supp = (1 << REQ_CLOSE);
	    ddb_device_info(dev, &info);
	    block_size = info.block_size;
	    total_size = info.total_size;
	    blocks_present = info.blocks_present;
	    blocks_allocated = info.blocks_allocated;
	    if (dev->ops->read) supp |= (1 << REQ_READ);
	    if (dev->ops->write) supp |= (1 << REQ_WRITE);
	    if (dev->ops->info) supp |= (1 << REQ_INFO);
	    if (dev->ops->print) supp |= (1 << REQ_PRINT);
	    if (dev->ops->has_block) supp |= (1 << REQ_HAS_BLOCK);
	    if (dev->ops->blocks) supp |= (1 << REQ_BLOCKS);
	    if (dev->ops->range) supp |= (1 << REQ_RANGE);
	    if (dev->ops->has_blocks) supp |= (1 << REQ_HAS_BLOCKS);
	    if (dev->ops->flush) supp |= (1 << REQ_FLUSH);
	    if (dev->ops->iterate) supp |= (1 << REQ_ITERATE);
	    if (dev->ops->report) supp |= (1 << REQ_REPORT);
	    result.error = htobe32(0);
	    result.supp = htobe32(supp);
	    if (block_size >= MIN_BLOCK_SIZE && block_size <= MAX_BLOCK_SIZE) {
		/* extra space needed for each block */
		size_t extra = 2 * (sizeof(ddb_block_t) + block_size)
			     + sizeof(unsigned char *) + DDB_CHECKSUM_LENGTH;
		res = malloc(sizeof(ddb_plugin_t) + rwmax * extra);
		if (res) {
		    unsigned char * ptr;
		    int i;
		    res->rblocks = res->blocks;
		    res->wblocks = &res->rblocks[rwmax];
		    res->cblocks = (void *)&res->wblocks[rwmax];
		    ptr = (void *)&res->cblocks[rwmax];
		    res->dev = dev;
		    res->block_size = block_size;
		    res->rwmax = rwmax;
		    for (i = 0; i < rwmax; i++) {
			res->rblocks[i].buffer = ptr;
			ptr += block_size;
			res->wblocks[i].buffer = ptr;
			ptr += block_size;
			res->cblocks[i] = ptr;
			ptr += DDB_CHECKSUM_LENGTH;
		    }
		    res->F_in = F_in;
		    res->F_out = F_out;
		    res->action = NULL;
		    res->aux_type = NULL;
		    res->aux_name = NULL;
		} else {
		    sve = errno;
		}
	    }
	} else {
	    sve = errno;
	}
    }
    if (! res) {
	result.error = htobe32(ddb_encode_errno(errno));
	result.supp = htobe32(0);
    }
    result.rwmax = htobe32(rwmax);
    result.magic = htobe64(CONN_OPEN_MAGIC);
    result.total_size = htobe64(total_size);
    result.blocks_present = htobe64(blocks_present);
    result.blocks_allocated = htobe64(blocks_allocated);
    result.block_size = htobe32(block_size);
    errno = EINVAL;
    if (fwrite(&result, sizeof(result), 1, F_out) < 1) goto error;
    if (fflush(F_out) == EOF) goto error;
    if (res) return res;
    errno = sve;
error:
    sve = errno;
    if (res) free(res);
    if (dev) ddb_device_close(dev);
    if (type) free(type);
    if (name) free(name);
    errno = sve;
    return NULL;
}

/* receive a list of blocks */
static ddb_blocklist_t * receive_blocks(FILE * F_in, int * data_size) {
    ddb_blocklist_t * ls = ddb_blocklist_new();
    int sve;
    if (! ls) return NULL;
    while (1) {
	blocks_request_t br;
	off_t start, end;
	errno = EINVAL;
	if (*data_size < sizeof(br))
	    goto error;
	(*data_size) -= sizeof(br);
	if (fread(&br, sizeof(br), 1, F_in) < 1)
	    goto error;
	start = be64toh(br.start);
	end = be64toh(br.end);
	if (start < 1) break;
	if (ddb_blocklist_add(ls, start, end) < 0) goto error;
    }
    return ls;
error:
    sve = errno;
    ddb_blocklist_free(ls);
    errno = sve;
    return NULL;
}

static int send_info(FILE * F_out, const ddb_device_info_t * info) {
    info_result_t ir;
    ir.flags = htobe32(info->flags);
    ir.block_size = htobe32(info->block_size);
    ir.total_size = htobe64(info->total_size);
    ir.num_blocks = htobe64(info->num_blocks);
    ir.blocks_present = htobe64(info->blocks_present);
    ir.blocks_allocated = htobe64(info->blocks_allocated);
    ir.mtime = htobe64(info->mtime);
    ir.multi_device = htobe32(info->multi_device);
    if (info->name)
	ir.name_size = htobe32(strlen(info->name));
    else
	ir.name_size = htobe32(0);
    if (fwrite(&ir, sizeof(ir), 1, F_out) < 1)
	return 0;
    if (info->name)
	if (fwrite(info->name, strlen(info->name), 1, F_out) < 1)
	    return 0;
    return 1;
}

static int send_line_i(int indent, const char * line, void * _F) {
    print_request_t pr;
    FILE * F_out = _F;
    int len = strlen(line);
    pr.indent = htobe32(indent);
    pr.verbose = htobe32(len);
    if (fwrite(&pr, sizeof(pr), 1, F_out) < 1) return -1;
    if (fwrite(line, len, 1, F_out) < 1) return -1;
    return 1;
}

static int send_line(const char * line, void * _F) {
    return send_line_i(0, line, _F);
}

/* wait for a request, execute it, return 0 if this was a close request,
 * 1 if there will be more requests, and -1 on error */
int ddb_plugin_run(ddb_plugin_t * pi) {
    ddb_device_info_t info;
    struct sigaction old_pipe, new_pipe;
    request_t rr;
    ddb_blocklist_t * rblocks = NULL, * sblocks = NULL;
    ddb_device_t * dev = pi->dev;
    FILE * F_in = pi->F_in, * F_out = pi->F_out;
    off_t block = -1;
    int data_size, request, data_sent, res_pipe = 0, ok = -1, sve;
    int verbose = 0, indent = 0, flags = 0, nblocks = 0, rwdata_size = 0;
    int status = 0, data_back = 0, send_print = 0, away = 0, send_iter = 0;
    int send_report = 0, is_read_maybe;
    if (pi->action) {
	/* run the action which was already requested instead of waiting
	 * for a request */
	if (pi->block_size) return 0; /* we've already ran the action... */
	pi->block_size = 1;
	// XXX report is not supported, would need something to send
	// XXX the file back to caller
	ok = ddb_action(pi->name, pi->type, pi->action,
			pi->aux_name, pi->aux_type, 0, NULL, NULL);
	if (ok >= 0) return 0;
	return -1;
    }
    memset(&new_pipe, 0, sizeof(new_pipe));
    new_pipe.sa_handler = SIG_IGN;
    new_pipe.sa_flags = 0;
    sigemptyset(&new_pipe.sa_mask);
    if (sigaction(SIGPIPE, &new_pipe, &old_pipe) >= 0)
	res_pipe = 1;
    errno = EINVAL;
    if (fread(&rr, sizeof(rr), 1, F_in) < 1)
	goto error;
    request = be32toh(rr.request);
    data_size = be32toh(rr.data_size);
    data_sent = be32toh(rr.data_sent);
    if (data_sent & DATA_BLOCKS) {
	rblocks = receive_blocks(F_in, &data_size);
	if (! rblocks) goto error;
    }
    if (data_sent & DATA_BLOCK) {
	rw_request_t hr;
	errno = EINVAL;
	if (data_size < sizeof(hr))
	    goto error;
	data_size -= sizeof(hr);
	if (fread(&hr, sizeof(hr), 1, F_in) < 1)
	    goto error;
	block = be64toh(hr.block);
    }
    if (data_sent & DATA_PRINT) {
	print_request_t pr;
	errno = EINVAL;
	if (data_size < sizeof(pr))
	    goto error;
	data_size -= sizeof(pr);
	if (fread(&pr, sizeof(pr), 1, F_in) < 1)
	    goto error;
	verbose = be32toh(pr.verbose);
	indent = be32toh(pr.indent);
    }
    if (data_sent & DATA_RW) {
	rw_spec_t sp;
	errno = EINVAL;
	if (data_size < sizeof(sp))
	    goto error;
	data_size -= sizeof(sp);
	if (fread(&sp, sizeof(sp), 1, F_in) < 1)
	    goto error;
	flags = be32toh(sp.flags);
	nblocks = be32toh(sp.nblocks);
	rwdata_size = be32toh(sp.data_size);
	if (data_sent & DATA_WRITE) {
	    int i;
	    errno = EINVAL;
	    if (rwdata_size < 0 || rwdata_size > pi->block_size) goto error;
	    if (nblocks < 0) goto error;
	    if (nblocks > pi->rwmax) goto error;
	    for (i = 0; i < nblocks; i++) {
		rw_request_t hr;
		errno = EINVAL;
		if (data_size < sizeof(hr))
		    goto error;
		data_size -= sizeof(hr);
		if (fread(&hr, sizeof(hr), 1, F_in) < 1)
		    goto error;
		pi->wblocks[i].block = be64toh(hr.block);
		pi->wblocks[i].result = 0;
		pi->wblocks[i].error = 0;
		errno = EINVAL;
		if (data_size < rwdata_size)
		    goto error;
		if (fread(pi->wblocks[i].buffer, rwdata_size, 1, F_in) < 1)
		    goto error;
		data_size -= rwdata_size;
	    }
	}
	if (data_sent & DATA_READ) {
	    int i;
	    errno = EINVAL;
	    if (rwdata_size < 0 || rwdata_size > pi->block_size) goto error;
	    if (nblocks < 0) goto error;
	    if (nblocks > pi->rwmax) goto error;
	    for (i = 0; i < nblocks; i++) {
		rw_request_t hr;
		errno = EINVAL;
		if (data_size < sizeof(hr))
		    goto error;
		data_size -= sizeof(hr);
		if (fread(&hr, sizeof(hr), 1, F_in) < 1)
		    goto error;
		pi->rblocks[i].block = be64toh(hr.block);
		pi->rblocks[i].result = 0;
		pi->rblocks[i].error = 0;
		if (data_sent & DATA_CHKSUM) {
		    errno = EINVAL;
		    if (DDB_CHECKSUM_LENGTH > pi->block_size) goto error;
		    if (data_size < DDB_CHECKSUM_LENGTH) goto error;
		    data_size -= DDB_CHECKSUM_LENGTH;
		    if (fread(pi->rblocks[i].buffer, DDB_CHECKSUM_LENGTH,
			      1, F_in) < 1)
			goto error;
		    /* copy the data in case the device doesn't support
		     * DDB_READ_MAYBE */
		    memcpy(pi->cblocks[i], pi->rblocks[i].buffer,
			   DDB_CHECKSUM_LENGTH);
		}
	    }
	}
    }
    errno = EINVAL;
    if (data_size > 0) goto error;
    data_size = 0;
    switch (request) {
	case REQ_READ:
	    if (! (data_sent & DATA_RW)) goto error;
	    if (! (data_sent & DATA_READ)) goto error;
	    is_read_maybe = flags & DDB_READ_MAYBE;
	    if (is_read_maybe && ! (data_sent & DATA_CHKSUM))
		goto error;
	    if (nblocks < 0 || nblocks > pi->rwmax) goto error;
	    status =
		1 + ddb_device_read_multi(dev, nblocks, pi->rblocks, flags);
	    data_back |= DATA_READ;
	    data_size += nblocks * sizeof(rw_result_t);
	    if (status > 0) {
		char zeros[rwdata_size];
		int i;
		memset(zeros, 0, rwdata_size);
		for (i = 0; i < nblocks; i++) {
		    if (pi->rblocks[i].result < 0) {
			pi->rblocks[i].result = RESULT_ERROR;
		    } else if (pi->rblocks[i].result == 0) {
			if (is_read_maybe) {
			    pi->rblocks[i].result = RESULT_EQUAL;
			} else {
			    pi->rblocks[i].result = RESULT_ERROR;
			    pi->rblocks[i].error = EINVAL;
			}
		    } else if (is_read_maybe &&
			       ddb_checksum_check(pi->rblocks[i].buffer,
						  rwdata_size,
						  pi->cblocks[i])) {
			pi->rblocks[i].result = RESULT_EQUAL;
		    } else if (memcmp(zeros, pi->rblocks[i].buffer,
				      rwdata_size) == 0) {
			pi->rblocks[i].result = RESULT_ZEROS;
		    } else {
			pi->rblocks[i].result = RESULT_DATA;
			data_size += rwdata_size;
		    }
		}
	    }
	    break;
	case REQ_WRITE:
	    if (! (data_sent & DATA_RW)) goto error;
	    if (! (data_sent & DATA_WRITE)) goto error;
	    if (nblocks < 0 || nblocks > pi->rwmax) goto error;
	    status =
		1 + ddb_device_write_multi(dev, nblocks, pi->wblocks);
	    data_back |= DATA_WRITE;
	    data_size += nblocks * sizeof(rw_result_t);
	    break;
	case REQ_INFO:
	    ddb_device_info(dev, &info);
	    data_back |= DATA_INFO;
	    data_size += sizeof(info_result_t);
	    if (info.name) data_size += strlen(info.name);
	    status = 1;
	    break;
	case REQ_PRINT:
	    send_print = 1;
	    status = 1;
	    break;
	case REQ_HAS_BLOCK:
	    status = ddb_device_has_block(dev, block);
	    if (status < 0)
		status = 0;
	    else if (status == 0)
		status = 1;
	    else
		status = 2;
	    break;
	case REQ_BLOCKS:
	    sblocks = ddb_device_blocks(dev);
	    if (sblocks) status = 1;
	    break;
	case REQ_RANGE:
	    sblocks = ddb_device_copy_blocks(dev);
	    if (sblocks) status = 1;
	    break;
	case REQ_HAS_BLOCKS:
	    errno = EINVAL;
	    if  (! rblocks) goto error;
	    sblocks = ddb_device_has_blocks(dev, rblocks);
	    if (sblocks) status = 1;
	    break;
	case REQ_FLUSH:
	    status = ddb_device_flush(dev);
	    if (status < 0)
		status = 0;
	    else
		status = 1;
	    break;
	case REQ_CLOSE:
	    status = ddb_device_close(dev);
	    if (status < 0)
		status = 0;
	    else
		status = 1;
	    away = 1;
	    pi->dev = NULL;
	    break;
	case REQ_ITERATE:
	    send_iter = 1;
	    status = 1;
	    break;
	case REQ_REPORT:
	    send_report = 1;
	    status = 1;
	    break;
    }
    rr.request = htobe32(status);
    rr.data_size = htobe32(data_size);
    rr.data_sent = htobe32(data_back);
    if (fwrite(&rr, sizeof(rr), 1, F_out) < 1) goto error;
    if (status > 0) {
	if (data_back & DATA_INFO) {
	    if (! send_info(F_out, &info)) goto error;
	}
	if (data_back & DATA_WRITE) {
	    rw_result_t rr;
	    int i;
	    for (i = 0; i < nblocks; i++) {
		rr.result = htobe32(pi->wblocks[i].result);
		rr.error = htobe32(ddb_encode_errno(pi->wblocks[i].error));
		if (fwrite(&rr, sizeof(rr), 1, F_out) < 1) goto error;
	    }
	}
	if (data_back & DATA_READ) {
	    rw_result_t rr;
	    int i;
	    for (i = 0; i < nblocks; i++) {
		rr.result = htobe32(pi->rblocks[i].result);
		rr.error = htobe32(ddb_encode_errno(pi->rblocks[i].error));
		if (fwrite(&rr, sizeof(rr), 1, F_out) < 1) goto error;
		if (pi->rblocks[i].result == RESULT_DATA)
		    if (fwrite(pi->rblocks[i].buffer, rwdata_size, 1, F_out) < 1)
			goto error;
	    }
	}
	if (send_print) {
	    print_request_t pr;
	    pr.indent = htobe32(ddb_device_info_print(dev, indent, send_line_i,
						      F_out, verbose));
	    pr.verbose = htobe32(-1);
	    if (fwrite(&pr, sizeof(pr), 1, F_out) < 1) goto error;
	}
	if (send_report) {
	    print_request_t pr;
	    pr.indent = htobe32(ddb_device_report(dev, send_line, F_out));
	    pr.verbose = htobe32(-1);
	    if (fwrite(&pr, sizeof(pr), 1, F_out) < 1) goto error;
	}
	if (sblocks) {
	    status = ddb_blocklist_iterate(sblocks, ddb_send_blocks, F_out);
	    if (ddb_send_blocks(-1, status, F_out) < 0)
		goto error;
	}
	if (send_iter) {
	    status = ddb_device_block_iterate(dev, ddb_send_blocks, F_out);
	    if (ddb_send_blocks(-1, status, F_out) < 0)
		goto error;
	}
    }
    if (fflush(F_out) == EOF) goto error;
    if (away)
	ok = 0;
    else
	ok = 1;
error:
    sve = errno;
    if (res_pipe)
	sigaction(SIGPIPE, &old_pipe, NULL);
    if (rblocks) ddb_blocklist_free(rblocks);
    if (sblocks) ddb_blocklist_free(sblocks);
    errno = sve;
    return ok;
}

/* deallocate anything allocated by ddb_plugin_init() */
void ddb_plugin_exit(ddb_plugin_t * pi) {
    if (pi->dev) ddb_device_close(pi->dev);
    free(pi);
}

