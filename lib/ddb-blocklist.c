/* ddb-blocklist.c - handle block lists for ddb
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
#include <malloc.h>
#include <errno.h>
#include <stdint.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ddb.h>

/* store a range of blocks; note that this will form a sorted list, but
 * we may decide to replace it with a tree or some other structure */
typedef struct range_s range_t;
struct range_s {
    range_t * next, * prev;
    off_t start;
    off_t end;
};

/* opaque type for a block list */
struct ddb_blocklist_s {
    range_t * first, * last;
    off_t count;
};

/* store a blocklist on disk, binary format */
typedef struct {
    int64_t start;
    int64_t end;
} __attribute__((packed)) disk_blocklist_t;

#define BLOCKLIST_MAGIC 0x426c6f636b4c7374LL /* "BlockLst" */

/* create a new empty list */
ddb_blocklist_t * ddb_blocklist_new(void) {
    ddb_blocklist_t * ls = malloc(sizeof(ddb_blocklist_t));
    if (! ls) return NULL;
    ls->first = ls->last = NULL;
    ls->count = 0;
    return ls;
}

/* used to load a blocklist */
static inline int getlistrec(FILE * F, off_t * start, off_t * end) {
    disk_blocklist_t bl;
    errno = EINVAL;
    if (fread(&bl, sizeof(bl), 1, F) < 1) return 0;
    *start = be64toh(bl.start);
    *end = be64toh(bl.end);
    return 1;
}

/* load a blocklist from a file */
ddb_blocklist_t * ddb_blocklist_load(FILE * F) {
    ddb_blocklist_t * res = ddb_blocklist_new();
    range_t * append = NULL;
    off_t nrecs, magic, count, last;
    int sv;
    if (! res) return NULL;
    if (! getlistrec(F, &magic, &nrecs)) goto error;
    if (magic != BLOCKLIST_MAGIC) {
	errno = EINVAL;
	goto error;
    }
    last = 0;
    for (count = 0; count < nrecs; count++) {
	range_t * range;
	off_t start, end;
	if (! getlistrec(F, &start, &end)) goto error;
	if (start < last || end < start) goto invalid;
	last = end + 2; /* end + 1 would allow joinable ranges */
	range = malloc(sizeof(range_t));
	if (! range) goto error;
	range->prev = append;
	range->next = NULL;
	range->start = start;
	range->end = end;
	res->count += end - start + 1;
	if (append)
	    append->next = range;
	else
	    res->first = range;
	append = range;
    }
    res->last = append;
    if (! getlistrec(F, &magic, &count)) goto error;
    if (magic == BLOCKLIST_MAGIC && count == nrecs)
	return res;
invalid:
    errno = EINVAL;
error:
    sv = errno;
    ddb_blocklist_free(res);
    errno = sv;
    return NULL;
}

ddb_blocklist_t * ddb_blocklist_read(FILE * F) {
    char line[128];
    ddb_blocklist_t * res = ddb_blocklist_new();
    range_t * append = NULL;
    long long last = 0;
    int sv;
    if (! res) return NULL;
    while (fgets(line, sizeof(line), F)) {
	range_t * range;
	long long start, end;
	int nf = sscanf(line, "%lld:%lld", &start, &end);
	if (nf < 1) goto invalid;
	if (nf < 2) end = start;
	if (start < last || end < start) goto invalid;
	last = end + 2; /* end + 1 would allow joinable ranges */
	range = malloc(sizeof(range_t));
	if (! range) goto error;
	range->prev = append;
	range->next = NULL;
	range->start = start;
	range->end = end;
	res->count += end - start + 1;
	if (append)
	    append->next = range;
	else
	    res->first = range;
	append = range;
    }
    res->last = append;
    return res;
invalid:
    errno = EINVAL;
error:
    sv = errno;
    ddb_blocklist_free(res);
    errno = sv;
    return NULL;
}

/* used to save a blocklist */
static inline int sendlistrec(FILE * F, off_t start, off_t end) {
    disk_blocklist_t bl;
    bl.start = htobe64(start);
    bl.end = htobe64(end);
    errno = EINVAL;
    if (fwrite(&bl, sizeof(bl), 1, F) < 1) return 0;
    return 1;
}

/* save blocklist to a file; leaves file position after list */
int ddb_blocklist_save(const ddb_blocklist_t * ls, FILE * F) {
    range_t * range = ls->first;
    off_t count = 0;
    while (range) {
	count++;
	range = range->next;
    }
    if (! sendlistrec(F, BLOCKLIST_MAGIC, count)) return -1;
    range = ls->first;
    while (range) {
	if (! sendlistrec(F, range->start, range->end)) return -1;
	range = range->next;
    }
    if (! sendlistrec(F, BLOCKLIST_MAGIC, count)) return -1;
    return 0;
}

/* print blocklist to a file; leaves file position after list */
int ddb_blocklist_print(const ddb_blocklist_t * ls, FILE * F) {
    range_t * range = ls->first;
    while (range) {
	if (range->start == range->end) {
	    if (fprintf(F, "%lld\n", (long long)range->start) < 0)
		return -1;
	} else {
	    if (fprintf(F, "%lld:%lld\n",
			(long long)range->start, (long long)range->end) < 0)
		return -1;
	}
	range = range->next;
    }
    return 0;
}

/* add a range to list if not already present; return 1 on success, 0
 * on error */
static int add_range(ddb_blocklist_t * ls, off_t start, off_t end) {
    range_t * range = ls->first, * nr;
    while (range) {
	/* if new range already completely present, nothing to do */
	if (range->start <= start && range->end >= end)
	    return 1;
	/* if the new range is just before this one, we can extend it;
	 * ditto if there's a partial overlap of the end of the new range
	 * with the start of this one */
	if (start < range->start && end + 1 >= range->start) {
	    ls->count += range->start - start;
	    range->start = start;
	    /* there will be no overlap or joining with the previous block,
	     * because we'd have done that at the previous iteration if
	     * that were the case */
	    /* if we added all required blocks, that will be all */
	    if (end <= range->end) return 1;
	    /* otherwise we need to try the next trick */
	    start = range->end + 1;
	}
	/* if the new range is just after this one, we can extend it;
	 * ditto if there's a partial overlap of the start of the new range
	 * with the end of this one */
	if (start <= range->end + 1 && end >= range->start) {
	    ls->count += end - range->end;
	    range->end = end;
	    /* and the range may now join or overlap the next one(s) */
	    while (range->next && range->next->start <= range->end + 1) {
		/* join ranges together */
		range_t * next = range->next;
		off_t newend = range->end;
		if (newend < next->end)
		    newend = next->end;
		ls->count += newend - range->end;
		range->end = newend;
		/* and now remove next as it's fully included in range */
		ls->count -= next->end - next->start + 1;
		range->next = next->next;
		if (next->next)
		    next->next->prev = range;
		else
		    ls->last = range;
		free(next);
	    }
	    return 1;
	}
	/* finally, it may be we've gone too far and we need to add a new
	 * one before this one */
	if (range->start > end) break;
	range = range->next;
    }
    /* make a new range, before range (or at end of list if range is NULL) */
    nr = malloc(sizeof(range_t));
    if (! nr) return 0;
    nr->prev = nr->next = NULL;
    nr->start = start;
    nr->end = end;
    ls->count += end - start + 1;
    if (range) {
	/* nr goes just before range */
	nr->next = range;
	nr->prev = range->prev;
	if (nr->prev)
	    nr->prev->next = nr;
	else
	    ls->first = nr;
    } else if (ls->last) {
	/* nr goes at end of list */
	ls->last->next = nr;
	nr->prev = ls->last;
	ls->last = nr;
    } else {
	/* list was empty, so now it has 1 range */
	ls->first = ls->last = nr;
    }
    return 1;
}

/* add a range to a list */
int ddb_blocklist_add(ddb_blocklist_t * ls, off_t start, off_t end) {
    /* find where to add the block */
    return add_range(ls, start, end) ? 0 : -1;
}

/* create a new list with all the blocks in ls where start <= block <= end */
ddb_blocklist_t * ddb_blocklist_sub(const ddb_blocklist_t * ls,
		off_t start, off_t end)
{
    ddb_blocklist_t * ls_new = ddb_blocklist_new();
    range_t * range = ls->first;
    if (! ls_new) return NULL;
    while (range) {
	off_t rs = range->start, re = range->end;
	if (rs < start) rs = start;
	if (re > end) re = end;
	if (rs <= re && ! add_range(ls_new, rs, re)) {
	    int sv = errno;
	    ddb_blocklist_free(ls_new);
	    errno = sv;
	    return NULL;
	}
	range = range->next;
    }
    return ls_new;
}

/* create a new list with all the blocks which appear in at least one
 * of the lists passed as arguments */
ddb_blocklist_t * ddb_blocklist_union(int nls,
		const ddb_blocklist_t * ls[nls])
{
    ddb_blocklist_t * ls_new = ddb_blocklist_new();
    int n;
    if (! ls_new) return NULL;
    /* we just add each list in turn; there are better ways of doing this
     * and we'll leave them for a future version */
    for (n = 0; n < nls; n++) {
	range_t * range = ls[n]->first;
	while (range) {
	    if (! add_range(ls_new, range->start, range->end)) {
		int sv = errno;
		ddb_blocklist_free(ls_new);
		errno = sv;
		return NULL;
	    }
	    range = range->next;
	}
    }
    return ls_new;
}

/* create a new list with all the blocks which appear in every one
 * of the lists passed as arguments */
ddb_blocklist_t * ddb_blocklist_intersect(int nls,
		const ddb_blocklist_t * ls[nls])
{
    ddb_blocklist_t * ls_new = ddb_blocklist_new();
    if (! ls_new) return NULL;
    if (nls > 0) {
	range_t * ranges[nls];
	off_t rs = 0, max = -1;
	int n, more;
	/* find start of first range in intersection; this is the highest
	 * "first block" of all lists; we also find the last possible block
	 * in the intersected list */
	for (n = 0, more = 1; n < nls && more; n++) {
	    ranges[n] = ls[n]->first;
	    if (ranges[n]) {
		if (rs < ranges[n]->start) rs = ranges[n]->start;
		if (ls[n]->last && (max < 0 || max > ls[n]->last->end))
		    max = ls[n]->last->end;
	    } else {
		more = 0;
	    }
	}
	while (more && rs <= max) {
	    /* take the range [start..max] and intersect it with all the
	     * lists; this will give a new block which we add to the result,
	     * or maybe will give an empty result in which case we're done */
	    off_t re = max;
	    for (n = 0; n < nls && more; n++) {
		/* advance ranges[n] until it intersect [rs..max] */
		while (ranges[n] && ranges[n]->end < rs)
		    ranges[n] = ranges[n]->next;
		if (ranges[n]) {
		    if (re > ranges[n]->end)
			re = ranges[n]->end;
		} else {
		    more = 0;
		}
	    }
	    /* if we are adding this block, do so... */
	    if (more && rs <= re && ! add_range(ls_new, rs, re)) {
		int sv = errno;
		ddb_blocklist_free(ls_new);
		errno = sv;
		return NULL;
	    }
	    /* and now try again with a block starting at rs >= re + 1 */
	    rs = re + 1;
	    for (n = 0; n < nls && more; n++) {
		/* advance ranges[n] until it intersect [rs..max] */
		while (ranges[n] && ranges[n]->end < rs)
		    ranges[n] = ranges[n]->next;
		if (ranges[n]) {
		    if (rs < ranges[n]->start) rs = ranges[n]->start;
		} else {
		    more = 0;
		}
	    }
	}
    }
    return ls_new;
}

/* number of blocks in list (a range start..end counts as
 * end-start+1 blocks) */
off_t ddb_blocklist_count(const ddb_blocklist_t * ls) {
    return ls->count;
}

/* check if the list contain a block */
int ddb_blocklist_has(const ddb_blocklist_t * ls, off_t block) {
    range_t * range =  ls->first;
    while (range && range->end < block)
	range = range->next;
    if (range && range->start <= block && range->end >= block)
	return 1;
    return 0;
}

/* call func(arg, start, end) for each disjoint range start..end of blocks
 * in list; if the list is empty, it never calls func */
int ddb_blocklist_iterate(const ddb_blocklist_t * ls,
		int (*func)(off_t, off_t, void *), void * arg)
{
    range_t * range = ls->first;
    while (range) {
	int res = func(range->start, range->end, arg);
	if (res < 0) return res;
	range = range->next;
    }
    return 0;
}

/* free and invalidate the list */
void ddb_blocklist_free(ddb_blocklist_t * ls) {
    while (ls->first) {
	range_t * next = ls->first->next;
	free(ls->first);
	ls->first = next;
    }
    free(ls);
}

