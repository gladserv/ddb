/* ddb.c - distributed device backup
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
#include <ddb-private.h>

#define DEFAULT_CHECKPOINT_INTERVAL 60
#define DEFAULT_MACHINE_PROGRESS_INTERVAL 120
#define DEFAULT_PROGRESS_INTERVAL 2
#define DEFAULT_FLUSH_INTERVAL 120
#define DEFAULT_MAX_PASSES 10

static ddb_copy_t W;
static const char * src_type = NULL;
static const char * dst_type = NULL;
static int default_copy_block = 0;
static int clear_user_cfg = DDB_CONFIG_CLEAR;
static int clear_system_cfg = DDB_CONFIG_CLEAR;
static int dst_exclusive = 0;
static int device_report = 0;
static int do_help = 0;
static int do_licence = 0;
static int do_version = 0;

static void init_copy(void) {
    W.src_name = NULL;
    W.src = NULL;
    W.dst_name = NULL;
    W.dst = NULL;
    W.write_dst = 1;
    W.total_size = 0;
    W.total_blocks = 0;
    W.block_size = DEFAULT_BLOCK_SIZE;
    W.input_list = NULL;
    W.max_passes = DEFAULT_MAX_PASSES;
    W.checkpoint_file = NULL;
    W.checkpoint_interval = DEFAULT_CHECKPOINT_INTERVAL;
    W.progress_function = ddb_progress_print;
    W.progress_arg = stdout;
    W.progress_interval = DEFAULT_PROGRESS_INTERVAL;
    W.machine_progress_file = NULL;
    W.machine_progress_interval = 0;
    W.extra_report = 0;
    W.output_list = NULL;
    W.output_each_pass = 0;
    W.flush_interval = DEFAULT_FLUSH_INTERVAL;
    W.use_checksums = 1;
    W.skip_identical = 1;
    W.copied_list = NULL;
}

/* checkpoint header on disk */
typedef struct {
    int64_t magic;
    int64_t total_size;
    int64_t blocks_read;
    int64_t read_errors;
    int64_t blocks_written;
    int64_t blocks_skipped;
    int64_t write_errors;
    int32_t block_size;
    int32_t pass;
} __attribute__((packed)) checkpoint_header_t;

#define CHECKPOINT_MAGIC 0x43686b506f696e74LL /* "ChkPoint" */

static void usage(FILE * F) {
    fprintf(F,
"Usage: %s [OPTIONS] SOURCE [DESTINATION]\n"
"-b BLOCK_SIZE   Change block size from default %d\n"
"-c [INTERVAL:]CHECKPOINT\n"
"                Checkpoint file: see documentation\n"
"-C              Do not use checksums to determine if some data is already\n"
"                present on DESTINATION, instead compare the full data\n"
"-e              Provides an extra per-device report at end, if supported\n"
"-f FILENAME     At end of processing, write to FILENAME the list of blocks\n"
"                which were copied from SOURCE to DESTINATION or which were\n"
"                already present in DESTINATION; if there is no DESTINATION\n"
"                the list of blocks which could be read from SOURCE\n"
"-F INTERVAL     Flush data to DESTINATION every INTERVAL seconds; use\n"
"                \"-F 0\" to disable; default is %d seconds\n"
"-h              Print this helpful message and exit\n"
"-i FILENAME     Before starting, read list of blocks to copy from FILENAME;\n"
"                default is to copy all blocks present in SOURCE\n"
"-k DIRECTORY    Overrides default user configuration directory\n"
"                ($HOME/%s or if defined $%s)\n"
"-K DIRECTORY    Overrides default system configuration directory\n"
"                (%s or if defined $%s)\n"
"-l              Print program's licence and exit\n"
"-n              Open DESTINATION readonly and report what would be written to\n"
"                it; this is incompatible with \"-w\"; implies \"-p 1\"\n"
"-o FILENAME     At end of processing, write the list of blocks which could not\n"
"                be copied to FILENAME\n"
"-O              With \"-o\", write the list after each pass\n"
"-p PASSES       Maximum number of passes (retries) before a block is considered\n"
"                completely unreadable; default is %d\n"
"-q              Omit per-pass and progress messages (but still obey \"-s\")\n"
"-r INTERVAL     Interval between progress reports, default is %d seconds;\n"
"                use \"-r 0\" to disable progress reports\n"
"-R              Show an extra progress report before the \"end pass\" message\n"
"-s INTERVAL     Sleep INTERVAL seconds after progress reports; default 0\n"
"-S NUMBER       Size of the copy buffer, default %d\n"
"-t TYPE         If creating DESTINATION, make it with type TYPE (see\n"
"                documentation); if it already exist, check it has this type\n"
"-T TYPE         Specify that SOURCE has the given TYPE, if autodetection fails\n"
"-v              Print program's version information and exit\n"
"-x              Exclusive: if DESTINATION already exist, stop with an error\n"
"-w              Write unconditionally to DESTINATION even if the data is\n"
"                already resent and identical; incompatible with \"-n\";\n"
"                see documentation about using this option\n"
	    , progname, DEFAULT_BLOCK_SIZE, DEFAULT_FLUSH_INTERVAL,
	    ddb_default_config(), ddb_override_config(),
	    ddb_default_sysconfig(), ddb_override_sysconfig(),
	    DEFAULT_MAX_PASSES, DEFAULT_PROGRESS_INTERVAL, default_copy_block);
}

static int open_devices(void) {
    ddb_device_info_t info;
    W.src = ddb_device_open(W.src_name, src_type,
			    (size_t)W.block_size, O_RDONLY, (off_t)0);
    if (! W.src) {
	perror(W.src_name);
	return 0;
    }
    ddb_device_info(W.src, &info);
    W.total_size = info.total_size;
    W.total_blocks = info.num_blocks;
    if (W.dst_name) {
	int flags;
	if (W.write_dst) {
	    flags = W.skip_identical ? O_RDWR : O_WRONLY;
	    flags |= O_CREAT;
	    if (dst_exclusive) flags |= O_EXCL;
	} else {
	    flags = O_RDONLY;
	}
	W.dst = ddb_device_open(W.dst_name, dst_type,
				(size_t)W.block_size, flags, W.total_size);
	if (! W.dst) {
	    perror(W.dst_name);
	    return 0;
	}
    }
    return 1;
}

static int send_line(const char * line, void * arg) {
    puts(line);
    return 0;
}

static int close_devices(void) {
    if (device_report) {
	ddb_device_report(W.src, send_line, NULL);
	if (W.dst) ddb_device_report(W.dst, send_line, NULL);
    }
    ddb_device_close(W.src);
    if (W.dst && ddb_device_close(W.dst) < 0) {
	perror(W.dst_name);
	return 0;
    }
    return 1;
}

#define store_int(var, min, max) \
    if (! ddb_store_int(opt, optarg, &var, min, max)) return 0
#define store_cfg(clr, which) \
    if (! ddb_store_cfg(&clr, which)) return 0
static int parse_arguments(int argc, char * argv[]) {
    int opt, narg, rwmax;
    init_copy();
    opterr = 0;
    while ((opt = getopt(argc, argv,
			 ":b:c:Cdf:F:hi:k:K:lno:Op:P:qr:Rs:S:t:T:vxw")) != -1)
    {
	char * colon;
	switch (opt) {
	    case '?' :
		fprintf(stderr, "%s: invalid option \"-%c\"\n",
			progname, argv[optind - 1][1]);
		usage(stderr);
		return 0;
	    case ':' :
		fprintf(stderr, "%s: option \"-%c\" requires an argument\n",
			progname, argv[optind - 1][1]);
		return 0;
	    case 'b' :
		store_int(W.block_size, MIN_BLOCK_SIZE, MAX_BLOCK_SIZE);
		break;
	    case 'c' :
		colon = index(optarg, ':');
		if (colon) {
		    int l = colon - optarg, i, ok = l > 0;
		    char tmp[l + 1];
		    for (i = 0; i < l; i++) {
			if (! isdigit(optarg[i])) {
			    ok = 0;
			    break;
			}
			tmp[i] = optarg[i];
		    }
		    if (ok) {
			tmp[l] = 0;
			if (! ddb_store_int(opt, tmp, &W.checkpoint_interval,
					    5, INT_MAX))
			    return 0;
			optarg = colon + 1;
		    }
		}
		W.checkpoint_file = optarg;
		break;
	    case 'C' :
		W.use_checksums = 0;
		break;
	    case 'd' :
		device_report = 1;
		break;
	    case 'f' :
		W.copied_list = optarg;
		break;
	    case 'F' :
		store_int(W.flush_interval, 0, INT_MAX);
		break;
	    case 'h' :
		do_help = 1;
		break;
	    case 'i' :
		W.input_list = optarg;
		break;
	    case 'k' :
		store_cfg(clear_user_cfg, DDB_CONFIG_USER);
		break;
	    case 'K' :
		store_cfg(clear_system_cfg, DDB_CONFIG_SYSTEM);
		break;
	    case 'l' :
		do_licence = 1;
		break;
	    case 'n' :
		W.write_dst = 0;
		break;
	    case 'o' :
		W.output_list = optarg;
		break;
	    case 'O' :
		W.output_each_pass = 1;
		break;
	    case 'p' :
		store_int(W.max_passes, 1, INT_MAX);
		break;
	    case 'P' :
		colon = index(optarg, ':');
		if (colon) {
		    int l = colon - optarg, i, ok = l > 0;
		    char tmp[l + 1];
		    for (i = 0; i < l; i++) {
			if (! isdigit(optarg[i])) {
			    ok = 0;
			    break;
			}
			tmp[i] = optarg[i];
		    }
		    if (ok) {
			tmp[l] = 0;
			if (! ddb_store_int(opt, tmp,
					    &W.machine_progress_interval,
					    5, INT_MAX))
			    return 0;
			optarg = colon + 1;
		    }
		} else {
		    W.machine_progress_interval =
			DEFAULT_MACHINE_PROGRESS_INTERVAL;
		}
		W.machine_progress_file = optarg;
		break;
	    case 'q' :
		W.progress_function = NULL;
		break;
	    case 'r' :
		store_int(W.progress_interval, 0, INT_MAX);
		break;
	    case 'R' :
		W.extra_report = 1;
		break;
	    case 's' :
		store_int(W.progress_sleep, 0, INT_MAX);
		break;
	    case 'S' :
		store_int(rwmax, 1, INT_MAX);
		ddb_copy_block(rwmax);
		break;
	    case 't' :
		dst_type = optarg;
		break;
	    case 'T' :
		src_type = optarg;
		break;
	    case 'v' :
		do_version = 1;
		break;
	    case 'x' :
		dst_exclusive = 1;
		break;
	    case 'w' :
		W.skip_identical = 0;
		break;
	}
    }
    if (do_licence || do_version || do_help) return 1;
    narg = argc - optind;
    if (narg < 1 || narg > 2) {
	usage(stderr);
	return 0;
    }
    W.src_name = argv[optind];
    if (narg > 1) W.dst_name = argv[optind + 1];
    if (W.skip_identical == 0 && W.write_dst == 0) {
	fprintf(stderr, "%s: cannot use both \"-w\" and \"-n\"\n", progname);
	return 0;
    }
    if (! W.skip_identical) W.use_checksums = 0;
    if (W.dst && ! W.write_dst) W.max_passes = 1;
    return 1;
}

int main(int argc, char * argv[]) {
    int ok;
    progname = rindex(argv[0], '/');;
    if (progname) progname++; else progname = argv[0];
    default_copy_block = ddb_copy_block(0);
    if (! parse_arguments(argc, argv)) return 1;
    if (do_version || do_licence) {
	printf("ddb %s\n", ddb_version);
	if (do_licence) putchar('\n');
    }
    if (do_help) usage(stdout);
    if (do_licence)
	puts(ddb_licence);
    if (do_licence || do_version || do_help)
	return 0;
    if (! open_devices()) return 2;
    ok = ddb_copy(&W);
    if (ok < 0) return 2;
    if (! close_devices()) return 2;
    return ok ? 0 : 3;
}

