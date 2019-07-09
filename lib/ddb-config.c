/* ddb-config.c - configuration files for ddb
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

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <ctype.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <regex.h>
#include <ddb.h>
#include "ddb-private.h"

/* compiled-in defaults */
#define DEFAULT_CONFIG ".ddb"
#define ENV_CONFIG "DDB_CONFIG"
#define DEFAULT_SYSCONFIG "/etc/ddb"
#define ENV_SYSCONFIG "DDB_SYSCONFIG"

/* undocumented line size limit; may become documented at some point */
#define LINE_SIZE 4096
#define MAX_ARGS 128

/* hold configuration */
typedef struct configuration_s configuration_t;
struct configuration_s {
    configuration_t * next;
    char path[0];
};

/* assignment in configuration file */
typedef struct assign_item_s assign_item_t;
struct assign_item_s {
    assign_item_t * next;
    int varlen;
    int vallen;
    const char * variable;
    const char * value;
    char _buffer[0];
};

typedef struct {
    assign_item_t * first;
} assign_t;

/* temporary holding up to 2 strings + a list */
typedef struct {
    int nargs, len1, len2;
    const char * val1, * val2;
    struct {
	int len;
	int explen;
	const char * val;
    } arg[MAX_ARGS];
} temporary_t;

static configuration_t * cfg_sys = NULL, * cfg_usr = NULL;
static configuration_t * end_cfg_sys = NULL, * end_cfg_usr = NULL;
int cfg_sys_default = 1, cfg_usr_default = 1;

/* add a configuration file to the list which the program will search */
int ddb_device_configuration(int flags, const char * path) {
    configuration_t ** list, ** end, * new;
    int * use_default;
    if ((flags & (DDB_CONFIG_USER | DDB_CONFIG_SYSTEM)) == DDB_CONFIG_SYSTEM) {
	list = &cfg_sys;
	end = &end_cfg_sys;
	use_default = &cfg_sys_default;
    } else {
	list = &cfg_usr;
	use_default = &cfg_usr_default;
	end = &end_cfg_usr;
    }
    if (flags & DDB_CONFIG_CLEAR) {
	*use_default = 0;
	while (*list) {
	    configuration_t * going = *list;
	    *list = going->next;
	    free(going);
	    *list = NULL;
	}
	*end = NULL;
    }
    if (! path) return 0;
    new = malloc(sizeof(configuration_t) + 1 + strlen(path));
    if (! new) return -1;
    new->next = NULL;
    strcpy(new->path, path);
    if (*end)
	(*end)->next = new;
    else
	*list = new;
    *end = new;
    return 0;
}

/* generic free on a list */
#define generic_free(name, what, extra) \
static void name(what * list) { \
    while (list) { \
	what * going = list; \
	list = going->next; \
	extra \
	free(going); \
    } \
}

/* assignment list functions */
generic_free(free_assign_item, assign_item_t, )

static void free_assign(assign_t * assign) {
    free_assign_item(assign->first);
    assign->first = NULL;
}

static int expand_assign(const assign_t * assign, const char * val,
		int ulen, char * res)
{
#define store_char(c) if (res) res[elen] = c; elen++
    int elen = 0;
    while (*val && ulen > 0) {
	if (*val == '$' && ulen > 1) {
	    const char * vname = val + 1;
	    int varlen, paren, rlen = ulen - 1;
	    if (*vname == '{' && rlen > 2) {
		vname++;
		rlen--;
		paren = 1;
	    } else {
		paren = 0;
	    }
	    varlen = 0;
	    while (varlen < rlen &&
		    vname[varlen] &&
		    (vname[varlen] == '_' || isalnum(vname[varlen])))
		varlen++;
	    if (varlen > 0 && (vname[varlen] == '}' || ! paren)) {
		/* do variable substitution */
		assign_item_t * list = assign->first;
		while (list) {
		    if (list->varlen == varlen &&
			strncmp(list->variable, vname, varlen) == 0)
		    {
			if (res)
			    strncpy(&res[elen], list->value, list->vallen);
			elen += list->vallen;
		    }
		    list = list->next;
		}
		val = vname + varlen + paren;
		continue;
	    }
	}
	store_char(*val);
	val++;
	ulen--;
    }
    if (res) res[elen] = '\0';
#undef store_char
    return elen;
}

static void remove_assign(assign_t * assign, const char * var) {
    assign_item_t * list = assign->first, * prev = NULL;
    while (list) {
	if (strcmp(list->variable, var) == 0) {
	    assign_item_t * going = list;
	    list = list->next;
	    if (prev)
		prev->next = list;
	    else
		assign->first = list;
	    free(going);
	} else {
	    prev = list;
	    list = list->next;
	}
    }
}

static int store_assign(assign_t * assign, const char * var,
		const char * val, int ulen)
{
    assign_item_t * new;
    char * ptr;
    int elen, varlen = strlen(var);
    elen = expand_assign(assign, val, ulen, NULL);
    new = malloc(sizeof(assign_item_t) + varlen + elen + 2);
    if (! new) return 0;
    ptr = new->_buffer;
    new->varlen = varlen;
    new->vallen = elen;
    new->variable = ptr;
    strcpy(ptr, var);
    ptr += 1 + strlen(var);
    new->value = ptr;
    expand_assign(assign, val, ulen, ptr);
    remove_assign(assign, var);
    new->next = assign->first;
    assign->first = new;
    return 1;
}

/* prepare list functions */
generic_free(free_prepare, ddb_prepare_t,
	     if (going->loaded) dlclose(going->loaded););

static int store_prepare(const assign_t * assign, ddb_prepare_t ** add,
		int type, temporary_t * temp)
{
    ddb_prepare_t * new;
    void * ptr;
    char * buffer;
    size_t size = 0, offset = sizeof(ddb_prepare_t);
    int n, exp1 = 0;
    if (temp->val1) {
	exp1 = expand_assign(assign, temp->val1, temp->len1, NULL);
	size += 1 + exp1;
    }
    for (n = 0; n < temp->nargs; n++) {
	temp->arg[n].explen =
	    expand_assign(assign, temp->arg[n].val, temp->arg[n].len, NULL);
	offset += sizeof(const char *);
	size += 1 + temp->arg[n].explen;
    }
    ptr = malloc(offset + size);
    if (! ptr) return 0;
    new = ptr;
    buffer = ptr;
    buffer += offset;
    new->next = NULL;
    new->type = type;
    new->nargs = temp->nargs;
    new->loaded = NULL;
    if (temp->val1) {
	new->program = buffer;
	expand_assign(assign, temp->val1, temp->len1, buffer);
	buffer += 1 + exp1;
    } else {
	new->program = NULL;
    }
    for (n = 0; n < temp->nargs; n++) {
	new->args[n] = buffer;
	expand_assign(assign, temp->arg[n].val, temp->arg[n].len, buffer);
	buffer += 1 + temp->arg[n].explen;
    }
    if (*add) {
	ddb_prepare_t * last = *add;
	while (last->next)
	    last = last->next;
	last->next = new;
    } else {
	*add = new;
    }
    return 1;
}

/* connect list functions */
generic_free(free_connect, ddb_connect_t, );

static int store_connect(const assign_t * assign, ddb_remote_t * descr,
		int in_retry, int type, temporary_t * temp)
{
    ddb_connect_t * new, * add;
    void * ptr;
    char * buffer;
    size_t size = 0, offset = sizeof(ddb_connect_t);
    int n, exp1 = 0, exp2 = 0;
    if (temp->val1) {
	exp1 = expand_assign(assign, temp->val1, temp->len1, NULL);
	size += 1 + exp1;
    }
    if (temp->val2) {
	exp2 = expand_assign(assign, temp->val2, temp->len2, NULL);
	size += 1 + exp2;
    }
    for (n = 0; n < temp->nargs; n++) {
	temp->arg[n].explen =
	    expand_assign(assign, temp->arg[n].val, temp->arg[n].len, NULL);
	offset += sizeof(const char *);
	size += 1 + temp->arg[n].explen;
    }
    ptr = malloc(offset + size);
    if (! ptr) return 0;
    new = ptr;
    buffer = ptr;
    buffer += offset;
    new->next = NULL;
    new->type = type;
    new->nargs = temp->nargs;
    if (temp->val1) {
	new->module = buffer;
	expand_assign(assign, temp->val1, temp->len1, buffer);
	buffer += 1 + exp1;
    } else {
	new->module = NULL;
    }
    if (temp->val2) {
	new->function = buffer;
	expand_assign(assign, temp->val2, temp->len2, buffer);
	buffer += 1 + exp2;
    } else {
	new->function = NULL;
    }
    for (n = 0; n < temp->nargs; n++) {
	new->args[n] = buffer;
	expand_assign(assign, temp->arg[n].val, temp->arg[n].len, buffer);
	buffer += 1 + temp->arg[n].explen;
    }
    add = in_retry ? descr->retry_connect : descr->connect;
    if (add) {
	while (add->next)
	    add = add->next;
	add->next = new;
    } else if (in_retry) {
	descr->retry_connect = new;
    } else {
	descr->connect = new;
    }
    return 1;
}

/* ddb_remote_t functions */
static void init_remote(ddb_remote_t * descr) {
    descr->prepare = NULL;
    descr->connect = NULL;
    descr->close = NULL;
    descr->retry_prepare = NULL;
    descr->retry_connect = NULL;
    descr->retry_close = NULL;
    descr->retry_max = 0;
    descr->retry_delay = 0;
    descr->block_size = 0;
}

static void free_remote(const ddb_remote_t * descr) {
    free_prepare(descr->prepare);
    free_connect(descr->connect);
    free_prepare(descr->close);
    free_prepare(descr->retry_prepare);
    free_connect(descr->retry_connect);
    free_prepare(descr->retry_close);
}

void ddb_remote_free(ddb_remote_t * descr) {
    free_remote(descr);
    free(descr);
}

/* helper function to match keywords */
static inline int keyword(char ** lp, const char * kw) {
    int len = strlen(kw);
    if (strncmp(*lp, kw, len) != 0) return 0;
    (*lp) += len;
    while (**lp && isspace(**lp)) (*lp)++;
    return 1;
}

/* helper function to match a quoted string or regex */
static inline int getquoted(char ** lp, const char * qb, const char * qe) {
    char * dp = *lp, * sp = *lp;
    const char * quote;
    int len = 0;
    while (*sp && isspace(*sp)) sp++;
    if (! *sp) return -1;
    /* see if the string is quoted */
    quote = strchr(qb, *sp);
    if (quote) {
	int pos = quote - qb;
	quote = qe + pos;
	sp++;
    }
    while (1) {
	/* end of input inside quote? */
	if (quote && ! *sp) return 1;
	/* quoted character gets copied unchanged */
	if (*sp == '\\') {
	    sp++;
	    if (! *sp) return -1;
	    *dp++ = *sp++;
	    len++;
	    continue;
	}
	/* end of string? */
	if (quote ? (*sp == *quote) : (*sp == '\0' || isspace(*sp))) {
	    if (quote) sp++;
	    while (*sp && isspace(*sp)) sp++;
	    *lp = sp;
	    *dp = 0;
	    return len;
	}
	*dp++ = *sp++;
	len++;
    }
}
#define getstring(lp) getquoted(&(lp), "'\"", "'\"")
#define getregex(lp) getquoted(&(lp), "'\"/", "'\"/")

static int getint(char ** lp, int * val) {
    char * ep;
    long res;
    errno = 0;
    res = strtol(*lp, &ep, 0);
    if (errno == ERANGE && (res == LONG_MAX || res == LONG_MIN))
	return 0;
    if (errno != 0 && res == 0)
	return 0;
    if (res < INT_MIN || res > INT_MAX) {
	errno = ERANGE;
	return 0;
    }
    if (ep == *lp) {
	errno = EINVAL;
	return 0;
    }
    *val = res;
    *lp = ep;
    while (**lp && isspace(**lp)) (*lp)++;
    return 1;
}

static inline void init_temporary(temporary_t * temp) {
    temp->len1 = temp->len2 = temp->nargs = 0;
    temp->val1 = temp->val2 = NULL;
}

/* scan configuration file */
static int config_file(ddb_remote_t ** dp, const char * dir,
		const char * file, const char * name, const char * type,
		int mode)
{
    char line[LINE_SIZE];
    char fn[strlen(dir) + strlen(file) + 2];
    ddb_remote_t descr;
    FILE * F;
    assign_t assign;
    int in_match = 1, use_this = 0, in_retry = 0, lineno = 0, sve;
    *dp = NULL;
    assign.first = NULL;
    descr.name = name;
    init_remote(&descr);
    strcpy(fn, dir);
    strcat(fn, "/");
    strcat(fn, file);
    F = fopen(fn, "r");
    if (! F) return 1;
    while (fgets(line, sizeof(line), F)) {
	char * lp = line;
	int len;
	lineno++;
	while (*lp && isspace(*lp)) lp++;
	len = strlen(lp);
	while (len > 0 && isspace(lp[len - 1])) len--;
	if (len < 1) continue;
	lp[len] = 0;
	if (*lp == '#') continue;
	if (keyword(&lp, "match")) {
	    /* "match" mode type regexp {assign} */
	    regmatch_t rm[10];
	    regex_t rc;
	    const char * re;
	    int err, n;
	    if (! in_match) {
		free_assign(&assign);
		/* if the previous match matched, we've found what to do */
		if (use_this) {
		    fclose(F);
		    *dp = malloc(sizeof(ddb_device_t));
		    if (*dp) {
			**dp = descr;
		    } else {
			int sve = errno;
			free_remote(&descr);
			errno = sve;
		    }
		    return 1;
		}
		free_remote(&descr);
		init_remote(&descr);
		in_match = in_retry = use_this = 0;
	    }
	    in_match = 1;
	    if (use_this) continue;
	    /* mode */
	    re = lp;
	    if (getstring(lp) < 0) goto invalid;
	    if (strcmp(re, "any") == 0) {
		/* always matches */
	    } else if (strcmp(re, "ro") == 0) {
		if (mode != DDB_MODE_RO) continue;
	    } else if (strcmp(re, "rw") == 0) {
		if (mode != DDB_MODE_RW && mode != DDB_MODE_EXCL) continue;
	    } else if (strcmp(re, "excl") == 0) {
		if (mode != DDB_MODE_EXCL) continue;
	    } else if (strcmp(re, "nonexcl") == 0) {
		if (mode != DDB_MODE_RW) continue;
	    } else if (strcmp(re, "action") == 0) {
		if (mode != DDB_MODE_ACT) continue;
	    } else {
		goto invalid;
	    }
	    /* see if they've requested a match on type */
	    if (*lp == '-') {
		/* type must not be present */
		if (type) continue;
		lp++;
		while (*lp && isspace(*lp)) lp++;
	    } else if (*lp == '=' || *lp == '?' || *lp == '!') {
		char mode = *(lp++);
		/* type must be present for "=" and "!" */
		if (mode != '?' && ! type) continue;
		while (*lp && isspace(*lp)) lp++;
		/* get the regex and see if it matches */
		re = lp;
		if (getregex(lp) < 0) goto invalid;
		if (type) {
		    int matches;
		    err = regcomp(&rc, re, REG_EXTENDED);
		    if (err) goto invalid;
		    matches = regexec(&rc, type, 10, rm, 0) == 0;
		    regfree(&rc);
		    if (mode == '!') {
			if (matches) continue;
		    } else {
			if (! matches) continue;
		    }
		}
	    } else {
		/* invalid type specification */
		goto invalid;
	    }
	    /* see if this has a regex, and does it match */
	    re = lp;
	    if (getregex(lp) < 0) goto invalid;
	    err = regcomp(&rc, re, REG_EXTENDED);
	    if (err) goto invalid;
	    if (regexec(&rc, name, 10, rm, 0) != 0) {
		/* no match, try another one */
		regfree(&rc);
		continue;
	    }
	    /* match, store capture groups as variables */
	    use_this = 1;
	    free_assign(&assign);
	    for (n = 0; n < 10; n++) {
		char var[2];
		if (rm[n].rm_so < 0) continue;
		snprintf(var, sizeof(var), "%d", n);
		if (store_assign(&assign, var,
				 name + rm[n].rm_so, rm[n].rm_eo - rm[n].rm_so))
		    continue;
		n = errno;
		regfree(&rc);
		fclose(F);
		free_assign(&assign);
		free_remote(&descr);
		errno = n;
		return 1;
	    }
	    regfree(&rc);
	    while (*lp) {
		char * eq;
		re = lp;
		if (getstring(lp) < 0) goto invalid;
		eq = index(re, '=');
		if (! eq) goto invalid;
		*eq++ = 0;
		if (! store_assign(&assign, re, eq, strlen(eq)))
		    goto error;
	    }
	    continue;
	}
	in_match = 0;
	if (! use_this) continue;
	if (keyword(&lp, "load")) {
	    /* "load" module */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    if (! store_prepare(&assign, in_retry ? &descr.retry_prepare
					          : &descr.prepare,
				ddb_prepare_load, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "run")) {
	    /* "run" program {args} */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    while (*lp && *lp != '>' && temp.nargs < MAX_ARGS) {
		temp.arg[temp.nargs].val = lp;
		temp.arg[temp.nargs].len = getstring(lp);
		if (temp.arg[temp.nargs].len < 0) goto invalid;
		temp.nargs++;
	    }
	    if (! store_prepare(&assign, in_retry ? &descr.retry_prepare
					          : &descr.prepare,
			        ddb_prepare_run, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "close")) {
	    /* "close" program {args} */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    while (*lp && *lp != '>' && temp.nargs < MAX_ARGS) {
		temp.arg[temp.nargs].val = lp;
		temp.arg[temp.nargs].len = getstring(lp);
		if (temp.arg[temp.nargs].len < 0) goto invalid;
		temp.nargs++;
	    }
	    if (! store_prepare(&assign, in_retry ? &descr.retry_close
					          : &descr.close,
			        ddb_prepare_run, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "open")) {
	    /* "open" devname [devtype] */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    temp.val2 = lp;
	    temp.len2 = getstring(lp);
	    if (temp.len2 < 0) {
		temp.val2 = NULL;
		temp.len2 = 0;
	    }
	    if (! store_connect(&assign, &descr, in_retry, ddb_connect_open, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "connect")) {
	    /* "connect" devname devtype|"-" host port */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    temp.val2 = lp;
	    temp.len2 = getstring(lp);
	    if (temp.len2 < 0) goto invalid;
	    if (temp.len2 == 1 && temp.val2[0] == '-') {
		temp.len2 = 0;
		temp.val2 = NULL;
	    }
	    temp.arg[0].val = lp;
	    temp.arg[0].len = getstring(lp);
	    if (temp.arg[0].len < 0) goto invalid;
	    temp.arg[1].val = lp;
	    temp.arg[1].len = getstring(lp);
	    if (temp.arg[1].len < 0) goto invalid;
	    temp.nargs = 2;
	    if (! store_connect(&assign, &descr, in_retry, ddb_connect_tcp, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "pipe")) {
	    /* "pipe" devname devtype|"-" program {args} */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    temp.val2 = lp;
	    temp.len2 = getstring(lp);
	    if (temp.len2 < 0) goto invalid;
	    if (temp.len2 == 1 && temp.val2[0] == '-') {
		temp.len2 = 0;
		temp.val2 = NULL;
	    }
	    while (*lp && temp.nargs < MAX_ARGS) {
		temp.arg[temp.nargs].val = lp;
		temp.arg[temp.nargs].len = getstring(lp);
		if (temp.arg[temp.nargs].len < 0) goto invalid;
		temp.nargs++;
	    }
	    if (temp.nargs < 1) goto invalid;
	    if (! store_connect(&assign, &descr, in_retry, ddb_connect_pipe, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "call")) {
	    /* "call" module function {args} */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    temp.val2 = lp;
	    temp.len2 = getstring(lp);
	    if (temp.len2 < 0) goto invalid;
	    while (*lp && temp.nargs < MAX_ARGS) {
		temp.arg[temp.nargs].val = lp;
		temp.arg[temp.nargs].len = getstring(lp);
		if (temp.arg[temp.nargs].len < 0) goto invalid;
		temp.nargs++;
	    }
	    if (! store_connect(&assign, &descr, in_retry, ddb_connect_call, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "acall")) {
	    /* "acall" module function {args} */
	    temporary_t temp;
	    init_temporary(&temp);
	    temp.val1 = lp;
	    temp.len1 = getstring(lp);
	    if (temp.len1 < 0) goto invalid;
	    temp.val2 = lp;
	    temp.len2 = getstring(lp);
	    if (temp.len2 < 0) goto invalid;
	    while (*lp && temp.nargs < MAX_ARGS) {
		temp.arg[temp.nargs].val = lp;
		temp.arg[temp.nargs].len = getstring(lp);
		if (temp.arg[temp.nargs].len < 0) goto invalid;
		temp.nargs++;
	    }
	    if (! store_connect(&assign, &descr, in_retry, ddb_connect_acall, &temp))
		goto error;
	    continue;
	}
	if (keyword(&lp, "retry")) {
	    /* "retry" num_times delay */
	    if (in_retry) goto invalid;
	    in_retry = 1;
	    if (! getint(&lp, &descr.retry_max)) goto error;
	    if (! getint(&lp, &descr.retry_delay)) goto error;
	    continue;
	}
	if (keyword(&lp, "block")) {
	    /* "retry" num_times delay */
	    if (in_retry) goto invalid;
	    in_retry = 1;
	    if (! getint(&lp, &descr.block_size)) goto error;
	    continue;
	}
	goto invalid;
    }
    fclose(F);
    free_assign(&assign);
    /* if the last match matched, return it */
    if (use_this) {
	*dp = malloc(sizeof(ddb_device_t));
	if (*dp) {
	    **dp = descr;
	} else {
	    int sve = errno;
	    free_remote(&descr);
	    errno = sve;
	}
	return 1;
    }
    free_remote(&descr);
    return 0;
invalid:
    // XXX so we could do some better error reporting...
    fprintf(stderr, "%s.%d: invalid line\n", fn, lineno);
    errno = EINVAL;
error:
    sve = errno;
    fclose(F);
    free_assign(&assign);
    free_remote(&descr);
    errno = sve;
    return 1;
}

/* configuration file filter */
static int scan_filter(const struct dirent * ent) {
    int len;
    if (ent->d_name[0] == '\0' || ent->d_name[0] == '.')
	return 0;
    len = strlen(ent->d_name);
    if (len < 5) return 0;
    if (strcmp(ent->d_name + len - 4, ".ddb") != 0)
	return 0;
    return 1;
}

/* sort function; we need this to be independent on the user's locale
 * setting, to get a repeatable result, so can't use alphasort();
 * we also sort in reverse because it makes config_dir() slightly easier */
static int scan_sort(const struct dirent ** a, const struct dirent ** b) {
    return strcmp((*b)->d_name, (*a)->d_name);
}

/* scan configuration directory */
static int config_dir(ddb_remote_t ** descr, const char * path,
		const char * name, const char * type, int mode)
{
    struct dirent ** namelist;
    int nents;
    if (path[0] == '-' && ! path[1]) return 0;
    *descr = NULL;
    nents = scandir(path, &namelist, scan_filter, scan_sort);
    if (nents < 0)
	return errno = ENOENT ? 0 : 1;
    while (nents-- > 0) {
	if (config_file(descr, path, namelist[nents]->d_name, name, type, mode))
	{
	    int sv = errno;
	    free(namelist[nents]);
	    while (nents-- > 0)
		free(namelist[nents]);
	    free(namelist);
	    errno = sv;
	    return 1;
	}
	free(namelist[nents]);
    }
    free(namelist);
    return 0;
}

/* scan user configuration directory */
static int config_udir(ddb_remote_t ** descr, const char * path,
		const char * name, const char * type, int mode)
{
    if (path[0] == '-' && ! path[1]) return 0;
    if (path[0] != '/') {
	const char * home = getenv("HOME");
	if (home) {
	    char np[strlen(home) + strlen(path) + 2];
	    strcpy(np, home);
	    strcat(np, "/");
	    strcat(np, path);
	    return config_dir(descr, np, name, type, mode);
	}
    }
    return config_dir(descr, path, name, type, mode);
}

/* scan all possible configuration */
ddb_remote_t * ddb_read_configuration(const char * name,
		const char * type, int mode)
{
    configuration_t * cfg;
    if (cfg_usr_default) {
	ddb_remote_t * res;
	const char * path = getenv(ENV_CONFIG);
	if (! path) path = DEFAULT_CONFIG;
	if (config_udir(&res, path, name, type, mode))
	    return res;
    }
    cfg = cfg_usr;
    while (cfg) {
	ddb_remote_t * res;
	if (config_udir(&res, cfg->path, name, type, mode))
	    return res;
	cfg = cfg->next;
    }
    if (cfg_sys_default) {
	ddb_remote_t * res;
	const char * path = getenv(ENV_SYSCONFIG);
	if (! path) path = DEFAULT_SYSCONFIG;
	if (config_dir(&res, path, name, type, mode))
	    return res;
    }
    cfg = cfg_sys;
    while (cfg) {
	ddb_remote_t * res;
	if (config_dir(&res, cfg->path, name, type, mode))
	    return res;
	cfg = cfg->next;
    }
    /* not found */
    errno = 0;
    return NULL;
}

/* return configuration defaults */
const char * ddb_default_config(void) {
    return DEFAULT_CONFIG;
}

const char * ddb_override_config(void) {
    return ENV_CONFIG;
}

const char * ddb_default_sysconfig(void) {
    return DEFAULT_SYSCONFIG;
}

const char * ddb_override_sysconfig(void) {
    return ENV_SYSCONFIG;
}

