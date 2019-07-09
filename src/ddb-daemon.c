/* ddb-daemon.c - access remote ddb device
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

static int clear_user_cfg = DDB_CONFIG_CLEAR;
static int clear_system_cfg = DDB_CONFIG_CLEAR;
static int do_help = 0;
static int do_licence = 0;
static int do_version = 0;

static void usage(FILE * F) {
    fprintf(F,
"Usage: %s [OPTIONS]\n"
"-h              Print this helpful message and exit\n"
"-k DIRECTORY    Overrides default user configuration directory\n"
"                ($HOME/%s or if defined $%s)\n"
"-K DIRECTORY    Overrides default system configuration directory\n"
"                (%s or if defined $%s)\n"
"-l              Print program's licence and exit\n"
"-v              Print program's version information and exit\n"
	    , progname,
	    ddb_default_config(), ddb_override_config(),
	    ddb_default_sysconfig(), ddb_override_sysconfig());
}

#define store_cfg(clr, which) \
    if (! ddb_store_cfg(&clr, which)) return 0
static int parse_arguments(int argc, char * argv[]) {
    int opt, narg;
    opterr = 0;
    while ((opt = getopt(argc, argv, ":hk:K:lqv")) != -1) {
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
	    case 'v' :
		do_version = 1;
		break;
	}
    }
    if (do_licence || do_version || do_help) return 1;
    narg = argc - optind;
    if (narg != 0) {
	usage(stderr);
	return 0;
    }
    return 1;
}

int main(int argc, char * argv[]) {
    ddb_plugin_t * pi;
    int ok;
    progname = rindex(argv[0], '/');;
    if (progname) progname++; else progname = argv[0];
    if (! parse_arguments(argc, argv)) return 1;
    if (do_version || do_licence) {
	printf("ddb-daemon %s\n", ddb_version);
	if (do_licence) putchar('\n');
    }
    if (do_help) usage(stderr);
    if (do_licence)
	puts(ddb_licence);
    if (do_licence || do_version || do_help)
	return 0;
    pi = ddb_plugin_init(stdin, stdout);
    if (! pi) {
	perror(progname);
	return 2;
    }
    ok = ddb_plugin_run(pi);
    while (ok > 0)
	ok = ddb_plugin_run(pi);
    ddb_plugin_exit(pi);
    return ok < 0 ? 3 : 0;
}

