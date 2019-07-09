/* ddb-util.c - utility functions
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
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ddb.h>
#include "ddb-private.h"

const char * const ddb_version = "1.0 03/02/2018";
const char * const ddb_licence =
"Copyright (c) 2018 Claudio Calvelli <ddb@gladserv.com>\n"
"All rights reserved.\n"
"\n"
"Redistribution and use in source and binary forms, with or without\n"
"modification, are permitted provided that the following conditions\n"
"are met:\n"
"\n"
"1. Redistributions of source code must retain the above copyright\n"
"   notice, this list of conditions and the following disclaimer.\n"
"2. Redistributions in binary form must reproduce the above copyright\n"
"   notice, this list of conditions and the following disclaimer in the\n"
"   documentation and/or other materials provided with the distribution.\n"
"3. If the program is modified in any way, a line must be added to the\n"
"   above copyright notice to state that such modification has occurred.\n"
"\n"
"THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS \"AS IS\" AND\n"
"ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
"IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR\n"
"PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS\n"
"BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\n"
"CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\n"
"SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS\n"
"INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN\n"
"CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\n"
"ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF\n"
"THE POSSIBILITY OF SUCH DAMAGE.\n";

const char * progname = "";

int ddb_store_int(int opt, const char * a, int * var, int min, int max) {
    char * ep;
    long v;
    errno = 0;
    v = strtol(a, &ep, 0);
    if ((errno == ERANGE && (v == LONG_MIN || v == LONG_MAX)) ||
	(errno != 0 && v == 0))
    {
	fprintf(stderr, "%s: -%c %s:", progname, opt, a);
	perror(NULL);
	return 0;
    }
    if (ep == a) {
	fprintf(stderr, "%s: -%c %s: not a number\n", progname, opt, a);
	return 0;
    }
    if (*ep) {
	fprintf(stderr, "%s: -%c %s: extra characters after number\n",
		progname, opt, a);
	return 0;
    }
    if (v < min || v > max) {
	fprintf(stderr, "%s: -%c %s: value out of range\n", progname, opt, a);
	return 0;
    }
    *var = v;
    return 1;
}

int ddb_store_cfg(int * clr, int which) {
    if (ddb_device_configuration(which | *clr, optarg) < 0) {
	perror("malloc");
	return 0;
    }
    *clr = 0;
    return 1;
}

