/* ddb-info.c - information about ddb images/devices
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

static const char * source_type = NULL;
ddb_device_t * source = NULL;
static int clear_user_cfg = DDB_CONFIG_CLEAR;
static int clear_system_cfg = DDB_CONFIG_CLEAR;
static int block_size = 0;
static int list_blocks = 0;
static int do_help = 0;
static int do_licence = 0;
static int do_version = 0;

static void usage(FILE * F) {
    fprintf(F,
"Usage: %s [OPTIONS] SOURCE [SOURCE]...\n"
"-b              Show complete list of blocks present in SOURCE\n"
"-B BLOCK_SIZE   Specify the block size, if required by the SOURCE\n"
"-h              Print this helpful message and exit\n"
"-k DIRECTORY    Overrides default user configuration directory\n"
"                ($HOME/%s or if defined $%s)\n"
"-K DIRECTORY    Overrides default system configuration directory\n"
"                (%s or if defined $%s)\n"
"-l              Print program's licence and exit\n"
"-t TYPE         Specify that SOURCE has the given TYPE, if autodetection fails\n"
"-v              Print program's version information and exit\n"
	    , progname,
	    ddb_default_config(), ddb_override_config(),
	    ddb_default_sysconfig(), ddb_override_sysconfig());
}

#define store_int(var, min, max) \
    if (! ddb_store_int(opt, optarg, &var, min, max)) return 0
#define store_cfg(clr, which) \
    if (! ddb_store_cfg(&clr, which)) return 0
static int parse_arguments(int argc, char * argv[]) {
    int opt, narg;
    opterr = 0;
    while ((opt = getopt(argc, argv, ":bB:hk:K:lt:v")) != -1) {
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
		list_blocks = 1;
		break;
	    case 'B' :
		store_int(block_size, MIN_BLOCK_SIZE, MAX_BLOCK_SIZE);
		break;
	    case 'h' :
		do_help = 1;
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
	    case 't' :
		source_type = optarg;
		break;
	    case 'v' :
		do_version = 1;
		break;
	}
    }
    if (do_licence || do_version || do_help) return 1;
    narg = argc - optind;
    if (narg < 1) {
	usage(stderr);
	return 0;
    }
    return 1;
}

static int prn(int level, const char * line, void * arg) {
    printf("%*s%s\n", level, "", line);
    return 1;
}

int main(int argc, char * argv[]) {
    int argn;
    progname = rindex(argv[0], '/');;
    if (progname) progname++; else progname = argv[0];
    if (! parse_arguments(argc, argv)) return 1;
    if (do_version || do_licence) {
	printf("ddb-info %s\n", ddb_version);
	if (do_licence) putchar('\n');
    }
    if (do_help) usage(stdout);
    if (do_licence)
	puts(ddb_licence);
    if (do_licence || do_version || do_help)
	return 0;
    for (argn = optind; argn < argc; argn++) {
	ddb_device_t * source =
	    ddb_device_open(argv[argn], source_type,
			    (size_t)block_size, O_RDONLY, (off_t)0);
	if (! source) {
	    perror(argv[argn]);
	    return 2;
	}
	ddb_device_info_print(source, 0, prn, NULL, list_blocks);
	ddb_device_close(source);
	printf("\n");
    }
    return 0;
}

