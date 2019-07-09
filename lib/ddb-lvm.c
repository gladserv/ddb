/* ddb-lvm.c - access to LVM volumes, possibly including snapshots
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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ddb.h>
#include "ddb-private.h"

typedef struct {
    char * snapsize;
    char * snapname;
    int snapname_len;
    char * lvname;
    char * vgname;
    char * fullname;
    char _buffer[0];
} name_t;

#define name(dev) ((name_t *)((dev)->extra))

/* we could use liblvm - except that this doesn't require LVM to be installed,
 * and the include files for liblvm state that the API is not an API but
 * something which can change... groan */
static int lvm_knows(const char * cmd, const char * name) {
    pid_t pid = fork();
    int status;
    if (pid < 0) return -1;
    if (pid == 0) {
	int fd = open("/dev/null", O_RDWR);
	if (fd < 0) exit(254);
	close(0);
	if (dup2(fd, 0) < 0) exit(254);
	close(1);
	if (dup2(fd, 1) < 0) exit(254);
	close(2);
	if (dup2(fd, 2) < 0) exit(254);
	close(fd);
	/* set up a sane PATH for searching the command */
	putenv("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/sbin:/opt/bin");
	execlp(cmd, cmd, "--noheadings", "-ovg_name", name, NULL);
	exit(254);
    }
    while (waitpid(pid, &status, 0) < 0)
	if (errno != EINTR) return -1;
    errno = EINVAL;
    if (! WIFEXITED(status)) return -1;
    if (WEXITSTATUS(status) == 254) return -1;
    if (WEXITSTATUS(status)) return 0;
    return 1;
}

static int run_cmd(char * const argv[]) {
    pid_t pid = fork();
    int status;
    if (pid < 0) return -1;
    if (pid == 0) {
	int fd = open("/dev/null", O_RDWR);
	if (fd < 0) exit(254);
	close(0);
	if (dup2(fd, 0) < 0) exit(254);
	close(fd);
	close(1);
	if (dup2(2, 1) < 0) exit(254);
	/* set up a sane PATH for searching the command */
	putenv("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/sbin:/opt/bin");
	execvp(argv[0], argv);
	exit(254);
    }
    while (waitpid(pid, &status, 0) < 0)
	if (errno != EINTR) return -1;
    errno = EINVAL;
    if (! WIFEXITED(status)) return -1;
    if (WEXITSTATUS(status) == 254) return -1;
    if (WEXITSTATUS(status)) return 0;
    return 1;
}

/* find the highest snapshot number for a volume, 0 if there are no
 * snapshots */
static int lvm_max_snapshot(const char * vgname, const char * lvname) {
    char name[1024];
    FILE * fp;
    int comm[2], sve, nclose = 0, lvlen = strlen(lvname), max = 0, status;
    pid_t pid;
    if (pipe(comm) < 0) return -1;
    nclose = 2;
    pid = fork();
    if (pid < 0) goto error;
    if (pid == 0) {
	int fd = open("/dev/null", O_RDWR);
	if (fd < 0) exit(254);
	close(0);
	if (dup2(fd, 0) < 0) exit(254);
	close(2);
	if (dup2(fd, 2) < 0) exit(254);
	close(fd);
	close(1);
	if (dup2(comm[1], 1) < 0) exit(254);
	close(comm[0]);
	close(comm[1]);
	execlp("lvs", "--noheadings", "-olv_name", vgname, NULL);
	exit(254);
    }
    close(comm[1]);
    nclose = 1;
    fp = fdopen(comm[0], "r");
    if (! fp) goto error;
    while (fgets(name, sizeof(name), fp)) {
	char * vp = name;
	int len, num;
	while (*vp && isspace(*vp)) vp++;
	len = strlen(vp);
	while (len > 0 && isspace(vp[len - 1])) len--;
	if (len < lvlen + 6) continue;
	if (strncmp(vp, lvname, lvlen) != 0) continue;
	if (strncmp(vp + lvlen, "-snap", 5) != 0) continue;
	vp[len] = 0;
	vp += lvlen + 5;
	len -= lvlen + 5;
	while (len > 0 && isdigit(vp[len - 1])) len--;
	if (len > 0) continue;
	num = atoi(vp);
	if (max < num) max = num;
    }
    fclose(fp);
    while (waitpid(pid, &status, 0) < 0)
	if (errno != EINTR) goto error;
    errno = EINVAL;
    if (! WIFEXITED(status)) goto error;
    if (WEXITSTATUS(status) == 254) goto error;
    if (WEXITSTATUS(status)) goto error;
    return max;
error:
    sve = errno;
    while (nclose > 0) {
	nclose--;
	close(comm[nclose]);
    }
    errno = sve;
    return -1;
}

static name_t * parse_name(const char * name, int flags) {
    name_t * lv = NULL;
    char * ptr;
    const char * lvname = NULL, * snapsize = NULL;
    size_t extra = 0;
    int vglen = 0, lvlen = 0, res, snapname_len = 0;
    errno = 0;
    while (name[vglen] && name[vglen] != '/') vglen++;
    if (vglen < 1 || ! name[vglen] || name[vglen] != '/') return NULL;
    lvname = &name[vglen + 1];
    while (lvname[lvlen] && lvname[lvlen] != '/') lvlen++;
    if (lvlen < 1 || lvname[lvlen]) return NULL;
    snapsize = rindex(lvname, ':');
    if (snapsize) {
	int i = 1;
	while (snapsize[i] && isdigit(snapsize[i])) i++;
	if (snapsize[i] &&
	    ! snapsize[i + 1] &&
	    index("mgtepMGTEP", snapsize[i]))
	{
	    lvlen = snapsize - lvname;
	    snapsize++;
	    snapname_len = lvlen + 10;
	    extra = 1 + strlen(snapsize) + snapname_len;
	} else {
	    snapsize = NULL;
	}
    }
    /* looks like it could be a volume name, set up a name_t structure */
    lv = malloc(sizeof(name_t) + 2 * vglen + lvlen + 3 + extra);
    if (! lv) return NULL;
    ptr = lv->_buffer;
    lv->fullname = ptr;
    strncpy(ptr, name, vglen);
    ptr += vglen;
    *ptr++ = '/';
    lv->lvname = ptr;
    strncpy(ptr, lvname, lvlen);
    ptr += lvlen;
    *ptr++ = 0;
    lv->vgname = ptr;
    strncpy(ptr, name, vglen);
    ptr += vglen;
    *ptr++ = 0;
    if (snapsize) {
	/* if we are taking a snapshot, we can't update the origin volume */
	if ((flags & O_ACCMODE) == O_RDONLY) {
	    errno = EINVAL;
	    goto error;
	}
	/* we won't be creating the origin volume, so pretend we don't
	 * have O_CREAT */
	flags &= ~O_CREAT;
	lv->snapsize = ptr;
	strcpy(ptr, snapsize);
	ptr += 1 + strlen(snapsize);
	lv->snapname = ptr;
	*ptr = 0;
    } else {
	lv->snapsize = NULL;
	lv->snapname = NULL;
    }
    lv->snapname_len = snapname_len;
    /* easy case is if LVM knows about it */
    res = lvm_knows("lvs", lv->fullname);
    if (res > 0) return lv;
    if (res < 0) goto error;
    if (! (flags & O_CREAT)) goto not_found;
    /* see if at least we have a valid volume group */
    res = lvm_knows("vgs", lv->vgname);
    if (res < 0) goto error;
    if (res == 0) goto not_found;
    /* we'll assume it's a valid volume, and we'll be creating it */
    return lv;
not_found:
    errno = 0;
error:
    if (lv) {
	int sve = errno;
	free(lv);
	errno = sve;
    }
    return NULL;
}

int ddb_is_lvm(const char * name, int flags) {
    name_t * lv = parse_name(name, flags);
    if (! lv) return 0;
    free(lv);
    return 1;
}

static int lvm_remove_snap(const name_t * lv) {
    char name[strlen(lv->vgname) + strlen(lv->snapname) + 2];
    char * const argv[] = { "lvremove", "-f", name, NULL };
    sprintf(name, "%s/%s", lv->vgname, lv->snapname);
    return run_cmd(argv);
}

static int lvm_read(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[], int flags)
{
    return ddb_device_read_multi(dev->info.devs[0], nblocks, blocks, flags);
}

static int lvm_write(ddb_device_t * dev, int nblocks,
		ddb_block_t blocks[])
{
    return ddb_device_write_multi(dev->info.devs[0], nblocks, blocks);
}

static int lvm_print(ddb_device_t * dev, int level,
		int (*func)(int, const char *, void *),
		void * arg, int verbose)
{
    int ok = ddb_device_info_print_internal(dev, level, func, arg, verbose);
    if (ok < 0) return ok;
    if (name(dev)->snapsize) {
	char snap[30 + strlen(name(dev)->snapname)];
	int ok;
	sprintf(snap, "snapshot-name: %s", name(dev)->snapname);
	ok = func(level, snap, arg);
	if (ok < 0) return ok;
    }
    return 0;
}

static int lvm_close(ddb_device_t * dev) {
    if (name(dev)->snapsize)
	lvm_remove_snap(name(dev));
    free(name(dev));
    return 0;
}

static const ddb_device_operations_t lvm_ops = {
    .type        = DDB_TYPE_LVM,
    .read        = lvm_read,
    .write       = lvm_write,
    .info        = NULL,
    .print       = lvm_print,
    .has_block   = NULL,
    .blocks      = NULL,
    .range       = NULL,
    .has_blocks  = NULL,
    .iterate     = NULL,
    .flush       = NULL,
    .report      = NULL,
    .close       = lvm_close,
};

static int lvm_create_snap(const name_t * lv) {
    char * const argv[] = { "lvcreate", "-s", "-n", lv->snapname, "-W", "n",
			    "-L", lv->snapsize, lv->fullname, NULL };
    return run_cmd(argv);
}

static int lvm_create_volume(const name_t * lv, off_t size) {
    char vsize[32];
    char * const argv[] = { "lvcreate", "-n", lv->lvname, "-L", vsize,
			    "-W", "n", lv->vgname, NULL };
    /* specify size in bytes, because that's the only one not subject to
     * rounding (lvm may make it bigger by rounding up to an extent */
    snprintf(vsize, sizeof(vsize), "%lldB", (long long)size);
    return run_cmd(argv);
}

static ddb_device_t * device_open(const name_t * lv, const char * lvname,
		size_t block_size, int flags, off_t total_size)
{
    char name[7 + strlen(lv->vgname) + strlen(lvname)];
    sprintf(name, "/dev/%s/%s", lv->vgname, lvname);
    return ddb_device_open_image(name, 0, block_size, flags, total_size);
}

ddb_device_t * ddb_device_open_lvm(const char * name,
		size_t block_size, int flags, off_t total_size)
{
    ddb_device_t * dev = NULL, * sub = NULL;
    name_t * lv = parse_name(name, flags);
    int sve, rmsnap = 0;
    if (! lv) {
	if (! errno) errno = EINVAL;
 	return NULL;
    }
    if (lv->snapsize) {
	int num = lvm_max_snapshot(lv->vgname, lv->lvname), max;
	if (num < 0) goto error;
	max = num + 15;
	if (max > 9999) max = 9999;
	while (++num < max) {
	    snprintf(lv->snapname, lv->snapname_len,
		     "%s-snap%04d", lv->lvname, num);
	    if (lvm_create_snap(lv) > 0) goto created;
	}
	goto invalid;
    created:
	rmsnap = 1;
	flags &= ~O_ACCMODE;
	flags |= O_RDONLY;
	sub = device_open(lv, lv->snapname, block_size, flags, total_size);
    } else {
	int can_create = flags & O_CREAT, exclusive = can_create && (flags & O_EXCL);
	/* we don't want to create a regular file... try opening without
	 * O_CREAT and if that fails we'll try creating */
	flags &= ~(O_CREAT|O_EXCL);
	if (exclusive)
	    if (lvm_create_volume(lv, total_size) < 1)
		goto error;
	sub = device_open(lv, lv->lvname, block_size, flags, total_size);
	if (! sub && can_create && ! exclusive && errno == ENOENT) {
	    if (lvm_create_volume(lv, total_size) < 1) goto error;
	    sub = device_open(lv, lv->lvname, block_size, flags, total_size);
	}
    }
    if (! sub) goto error;
    dev = ddb_device_open_multi(0, 1, &sub, flags);
    if (! dev) goto error;
    dev->ops = &lvm_ops;
    dev->extra = lv;
    return dev;
invalid:
    errno = EINVAL;
error:
    sve = errno;
    if (lv) {
	if (rmsnap) lvm_remove_snap(lv);
	free(lv);
    }
    if (dev) ddb_device_close(dev);
    if (sub) ddb_device_close(sub);
    errno = sve;
    return NULL;
}

