/* ddb-sequence.c - operations on a sequence of backups
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
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <ddb.h>
#include <ddb-private.h>

#define TYPE "sequence"

static const char * sequence_name = NULL;
static const char * sequence_type = TYPE;
static int clear_user_cfg = DDB_CONFIG_CLEAR;
static int clear_system_cfg = DDB_CONFIG_CLEAR;
static int operation = 0;
static int machine = 0;
static int quiet = 0;
static int do_help = 0;
static int do_licence = 0;
static int do_version = 0;

#define OP_INFO      0x0001
#define OP_FULL      0x0002
#define OP_JOIN      0x0004
#define OP_CHECKSUM  0x0008

static void usage(FILE * F) {
    fprintf(F,
"Usage: %s [OPTIONS] SEQUENCE\n"
"-c              Create checksum cache for a sequence\n"
"-h              Print this helpful message and exit\n"
"-i              Show information about full and incremental images\n"
"                (default if no operation is selected)\n"
"-I              Show full information (like \"-i\" but with more detail)\n"
"-j              Join full and first incremental backup into a newer\n"
"                full backup\n"
"-k DIRECTORY    Overrides default user configuration directory\n"
"                ($HOME/%s or if defined $%s)\n"
"-K DIRECTORY    Overrides default system configuration directory\n"
"                (%s or if defined $%s)\n"
"-l              Print program's licence and exit\n"
"-m              Machine-readable output\n"
"-q              Quiet: omit progress reports for \"-j\"\n"
"-t TYPE         Specify device type (default: %s)\n"
"-v              Print program's version information and exit\n"
	    , progname,
	    ddb_default_config(), ddb_override_config(),
	    ddb_default_sysconfig(), ddb_override_sysconfig(),
	    TYPE);
}

#define store_cfg(clr, which) \
    if (! ddb_store_cfg(&clr, which)) return 0
static int parse_arguments(int argc, char * argv[]) {
    int opt, narg;
    opterr = 0;
    while ((opt = getopt(argc, argv, ":chiIjk:K:lmqt:v")) != -1) {
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
	    case 'c' :
		operation |= OP_CHECKSUM;
		break;
	    case 'h' :
		do_help = 1;
		break;
	    case 'i' :
		operation |= OP_INFO;
		break;
	    case 'I' :
		operation |= OP_FULL;
		break;
	    case 'j' :
		operation |= OP_JOIN;
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
	    case 'm' :
		machine = 1;
		break;
	    case 'q' :
		quiet = 1;
		break;
	    case 't' :
		sequence_type = optarg;
		break;
	    case 'v' :
		do_version = 1;
		break;
	}
    }
    if (do_licence || do_version || do_help) return 1;
    narg = argc - optind;
    if (narg != 1) {
	usage(stderr);
	return 0;
    }
    sequence_name = argv[optind];
    if (! operation) operation = OP_INFO;
    return 1;
}

static char * hu(off_t num, char * buffer, size_t len) {
    double fnum = (double)num / 1024.0;
    const char * names = "kMGTEP";
    int unit = 0;
    while (names[unit + 1] && fnum > 1024.0) {
	fnum /= 1024.0;
	unit++;
    }
    snprintf(buffer, len - 1, "%.2f", fnum);
    len = strlen(buffer);
    while (len > 0 && buffer[len - 1] == '0') len--;
    if (len > 0 && buffer[len - 1] == '.') len--;
    buffer[len++] = names[unit];
    buffer[len] = 0;
    return buffer;
}

static int show_info(void) {
    char ts[32];
    ddb_device_info_t info;
    ddb_device_t **devs, * close, * sequence =
	ddb_device_open(sequence_name, sequence_type,
			(size_t)0, O_RDONLY, (off_t)0);
    int i;
    if (! sequence) {
	perror(sequence_name);
	return 0;
    }
    close = sequence;
    ddb_device_info(sequence, &info);
    devs = info.devs;
    /* it could be an indirect device opening a sequence, so... */
    while (info.multi_device == 1 && (! info.type || strcmp(info.type, TYPE) != 0)) {
	sequence = devs[0];
	ddb_device_info(sequence, &info);
	devs = info.devs;
    }
    if (! info.type || strcmp(info.type, TYPE) != 0 ||
	info.multi_device < 1 || ! devs)
    {
	fprintf(stderr, "%s: invalid sequence\n", sequence_name);
	ddb_device_close(close);
	return 0;
    }
    if (machine)
	printf("name %s\n", sequence_name);
    else
	printf("%s:\n", sequence_name);
    if (operation & OP_FULL) {
	if (machine) {
	    printf("block-size %ld\n", (long)info.block_size);
	    printf("total-blocks %lld\n", (long long)info.blocks_allocated);
	} else {
	    printf("Block size: %ld\n", (long)info.block_size);
	    printf("Total size: %s (%lld blocks)\n",
		   hu(info.blocks_allocated * (off_t)info.block_size,
		      ts, sizeof(ts)),
		   (long long)info.blocks_allocated);
	}
    }
    printf("\n");
    for (i = 0; i < info.multi_device; i++) {
	ddb_device_info_t subinfo;
	ddb_device_info(devs[i], &subinfo);
	if (machine)
	    snprintf(ts, sizeof(ts), "%lld", (long long)subinfo.mtime);
	else
	    strftime(ts, sizeof(ts),
		     "%Y-%m-%d %H:%M:%S %Z", localtime(&subinfo.mtime));
	if (i) {
	    if (machine) {
		printf("incremental %s\n", ts);
		if (operation & OP_FULL)
		    printf("blocks %lld\n", (long long)subinfo.blocks_present);
	    } else {
		printf("Incremental: %s\n", ts);
		if (operation & OP_FULL)
		    printf("  Changes: %s (%lld blocks)\n",
			   hu(subinfo.blocks_present * (off_t)subinfo.block_size,
			      ts, sizeof(ts)),
			   (long long)subinfo.blocks_present);
	    }
	} else {
	    if (machine) {
		printf("full %s\n", ts);
		if (operation & OP_FULL)
		    printf("blocks %lld\n", (long long)subinfo.num_blocks);
	    } else {
		printf("Full backup: %s\n", ts);
		if (operation & OP_FULL)
		    printf("  Total size: %s (%lld blocks)\n",
			   hu(subinfo.total_size, ts, sizeof(ts)),
			   (long long)subinfo.num_blocks);
	    }
	}
	if (operation & OP_FULL) {
	    if (machine)
		printf("allocated %lld\n", (long long)subinfo.blocks_allocated);
	    else
		printf("  File size: %s (%lld blocks)\n",
		       hu(subinfo.blocks_allocated * (off_t)subinfo.block_size,
			  ts, sizeof(ts)),
		       (long long)subinfo.blocks_allocated);
	}
	printf("\n");
    }
    ddb_device_close(close);
    return 1;
}

static int do_action(const char * action) {
    if (quiet) {
	if (ddb_action(sequence_name, sequence_type, action,
		       NULL, NULL, 1000, NULL, NULL) < 0)
	    return 0;
    } else {
	if (ddb_action(sequence_name, sequence_type, action,
		       NULL, NULL, 2, ddb_progress_print, stdout) < 0)
	    return 0;
    }
    return 1;
}

int main(int argc, char * argv[]) {
    progname = rindex(argv[0], '/');;
    if (progname) progname++; else progname = argv[0];
    if (! parse_arguments(argc, argv)) return 1;
    if (do_version || do_licence) {
	printf("ddb-sequence %s\n", ddb_version);
	if (do_licence) putchar('\n');
    }
    if (do_help) usage(stdout);
    if (do_licence)
	puts(ddb_licence);
    if (do_licence || do_version || do_help)
	return 0;
    if (operation & (OP_INFO | OP_FULL))
	if (! show_info()) return 2;
    if (operation & OP_JOIN) {
	if (! do_action("join")) return 2;
	if (operation & (OP_INFO | OP_FULL))
	    if (! show_info()) return 2;
    }
    if (operation & OP_CHECKSUM) {
	if (! do_action("checksum")) return 2;
	if (operation & (OP_INFO | OP_FULL))
	    if (! show_info()) return 2;
    }
    return 0;
}

