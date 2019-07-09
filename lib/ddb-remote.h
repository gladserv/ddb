/* ddb-remote.h - "remote" access, via a program or plugin
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

#ifndef __DDB_REMOTE__
#define __DDB_REMOTE__ 1

/* structure used to open connection and to return status */
#define CONN_OPEN_MAGIC 0x6464622d6f70656eLL /* "ddb-open" */
#define CONN_OPEN_RDONLY 0x0001
#define CONN_OPEN_WRONLY 0x0002
#define CONN_OPEN_RDWR   0x0003
#define CONN_OPEN_ACCESS 0x0003
#define CONN_OPEN_CREAT  0x0004
#define CONN_OPEN_EXCL   0x0008
#define ERRCODE_OK            0
#define ERRCODE_ACCESS        1
#define ERRCODE_EXISTS        2
#define ERRCODE_NOENT         3
#define ERRCODE_INVALID       4
#define ERRCODE_ISDIR         5
#define ERRCODE_NOTDIR        6
#define ERRCODE_LOOP          7
#define ERRCODE_NOMEM         8
#define ERRCODE_OTHER       255
typedef struct {
    int64_t magic;
    int64_t total_size;
    int32_t flags;
    int32_t block_size;
    int32_t type_size;
    int32_t name_size;
    int32_t action_size;
    int32_t aux_type_size;
    int32_t aux_name_size;
    int32_t rwmax;
} __attribute__((packed)) conn_open_t;

/* structure used as reply from connection open */
typedef struct {
    int64_t magic;
    int64_t total_size;
    int64_t blocks_present;
    int64_t blocks_allocated;
    int32_t supp;
    int32_t block_size;
    int32_t error;
    int32_t rwmax;
} __attribute__((packed)) conn_result_t;

/* structure used to send a request and get a reply */
#define REQ_READ         1
#define REQ_WRITE        2
#define REQ_INFO         3
#define REQ_PRINT        4
#define REQ_HAS_BLOCK    5
#define REQ_BLOCKS       6
#define REQ_RANGE        7
#define REQ_HAS_BLOCKS   8
#define REQ_FLUSH        9
#define REQ_CLOSE       10
#define REQ_ITERATE     11
#define REQ_REPORT      12
#define DATA_BLOCKS 0x0001
#define DATA_BLOCK  0x0002
#define DATA_PRINT  0x0004
#define DATA_RW     0x0008
#define DATA_INFO   0x0010
#define DATA_READ   0x0020
#define DATA_WRITE  0x0040
#define DATA_CHKSUM 0x0080
typedef struct {
    int32_t request;   /* or status, 1 OK, 0 error */
    int32_t data_size; /* or error code */
    int32_t data_sent;
} __attribute__((packed)) request_t;

/* for read and write requests this is the global data */
typedef struct {
    int32_t flags;
    int32_t nblocks;
    int32_t data_size;
} __attribute__((packed)) rw_spec_t;

/* for read, write and has_block requests, this is the data which follows */
typedef struct {
    int64_t block;
} __attribute__((packed)) rw_request_t;

/* for read and write requests, this is the status data */
#define RESULT_ERROR      0
#define RESULT_ZEROS      1
#define RESULT_EQUAL      2
#define RESULT_DATA       3
typedef struct {
    int32_t result;
    int32_t error;
} __attribute__((packed)) rw_result_t;

/* for info requests, this is the data returned */
typedef struct {
    int32_t flags;
    int32_t block_size;
    int64_t total_size;
    int64_t num_blocks;
    int64_t blocks_present;
    int64_t blocks_allocated;
    int64_t mtime;
    int32_t multi_device;
    int32_t name_size;
} __attribute__((packed)) info_result_t;

/* for print requests, this is the data sent or received */
typedef struct {
    int32_t verbose; /* or length */
    int32_t indent;
} __attribute__((packed)) print_request_t;

/* for has_blocks, blocks, range and iterate, this is the data */
typedef struct {
    int64_t start;
    int64_t end;
} __attribute__((packed)) blocks_request_t;

/* for has_data this is the data */
typedef struct {
    int64_t block;
    unsigned char checksum[DDB_CHECKSUM_LENGTH];
} __attribute__((packed)) has_data_request_t;

int ddb_decode_errno(int);
int ddb_encode_errno(int);
int ddb_send_blocks(off_t, off_t, void *);

#endif /* __DDB_REMOTE__ */
