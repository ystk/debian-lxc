/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <dlezcano at fr.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#define _GNU_SOURCE
#include <stdio.h>
#undef _GNU_SOURCE
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <mntent.h>
#include <unistd.h>
#include <pty.h>

#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/capability.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <libgen.h>

#include "network.h"
#include "error.h"
#include "parse.h"
#include "config.h"
#include "utils.h"
#include "conf.h"
#include "log.h"
#include "lxc.h"	/* for lxc_cgroup_set() */

lxc_log_define(lxc_conf, lxc);

#define MAXHWLEN    18
#define MAXINDEXLEN 20
#define MAXMTULEN   16
#define MAXLINELEN  128

#ifndef MS_REC
#define MS_REC 16384
#endif

#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

#ifndef CAP_SETFCAP
#define CAP_SETFCAP 31
#endif

#ifndef CAP_MAC_OVERRIDE
#define CAP_MAC_OVERRIDE 32
#endif

#ifndef CAP_MAC_ADMIN
#define CAP_MAC_ADMIN 33
#endif

#ifndef PR_CAPBSET_DROP
#define PR_CAPBSET_DROP 24
#endif

extern int pivot_root(const char * new_root, const char * put_old);

typedef int (*instanciate_cb)(struct lxc_netdev *);

struct mount_opt {
	char *name;
	int clear;
	int flag;
};

struct caps_opt {
	char *name;
	int value;
};

static int instanciate_veth(struct lxc_netdev *);
static int instanciate_macvlan(struct lxc_netdev *);
static int instanciate_vlan(struct lxc_netdev *);
static int instanciate_phys(struct lxc_netdev *);
static int instanciate_empty(struct lxc_netdev *);

static  instanciate_cb netdev_conf[LXC_NET_MAXCONFTYPE + 1] = {
	[LXC_NET_VETH]    = instanciate_veth,
	[LXC_NET_MACVLAN] = instanciate_macvlan,
	[LXC_NET_VLAN]    = instanciate_vlan,
	[LXC_NET_PHYS]    = instanciate_phys,
	[LXC_NET_EMPTY]   = instanciate_empty,
};

static struct mount_opt mount_opt[] = {
	{ "defaults",   0, 0              },
	{ "ro",         0, MS_RDONLY      },
	{ "rw",         1, MS_RDONLY      },
	{ "suid",       1, MS_NOSUID      },
	{ "nosuid",     0, MS_NOSUID      },
	{ "dev",        1, MS_NODEV       },
	{ "nodev",      0, MS_NODEV       },
	{ "exec",       1, MS_NOEXEC      },
	{ "noexec",     0, MS_NOEXEC      },
	{ "sync",       0, MS_SYNCHRONOUS },
	{ "async",      1, MS_SYNCHRONOUS },
	{ "remount",    0, MS_REMOUNT     },
	{ "mand",       0, MS_MANDLOCK    },
	{ "nomand",     1, MS_MANDLOCK    },
	{ "atime",      1, MS_NOATIME     },
	{ "noatime",    0, MS_NOATIME     },
	{ "diratime",   1, MS_NODIRATIME  },
	{ "nodiratime", 0, MS_NODIRATIME  },
	{ "bind",       0, MS_BIND        },
	{ "rbind",      0, MS_BIND|MS_REC },
	{ NULL,         0, 0              },
};

static struct caps_opt caps_opt[] = {
	{ "chown",             CAP_CHOWN 	     },
	{ "dac_override",      CAP_DAC_OVERRIDE      },
	{ "dac_read_search",   CAP_DAC_READ_SEARCH   },
	{ "fowner",            CAP_FOWNER            },
	{ "fsetid",            CAP_FSETID            },
	{ "kill",              CAP_KILL              },
	{ "setgid",            CAP_SETGID            },
	{ "setuid",            CAP_SETUID            },
	{ "setpcap",           CAP_SETPCAP           },
	{ "linux_immutable",   CAP_LINUX_IMMUTABLE   },
	{ "net_bind_service",  CAP_NET_BIND_SERVICE  },
	{ "net_broadcast",     CAP_NET_BROADCAST     },
	{ "net_admin",         CAP_NET_ADMIN         },
	{ "net_raw",           CAP_NET_RAW           },
	{ "ipc_lock",          CAP_IPC_LOCK          },
	{ "ipc_owner",         CAP_IPC_OWNER         },
	{ "sys_module",        CAP_SYS_MODULE        },
	{ "sys_rawio",         CAP_SYS_RAWIO         },
	{ "sys_chroot",        CAP_SYS_CHROOT        },
	{ "sys_ptrace",        CAP_SYS_PTRACE        },
	{ "sys_pacct",         CAP_SYS_PACCT         },
	{ "sys_admin",         CAP_SYS_ADMIN         },
	{ "sys_boot",          CAP_SYS_BOOT          },
	{ "sys_nice",          CAP_SYS_NICE          },
	{ "sys_resource",      CAP_SYS_RESOURCE      },
	{ "sys_time",          CAP_SYS_TIME          },
	{ "sys_tty_config",    CAP_SYS_TTY_CONFIG    },
	{ "mknod",             CAP_MKNOD             },
	{ "lease",             CAP_LEASE             },
#ifdef CAP_AUDIT_WRITE
	{ "audit_write",       CAP_AUDIT_WRITE       },
#endif
#ifdef CAP_AUDIT_CONTROL
	{ "audit_control",     CAP_AUDIT_CONTROL     },
#endif
	{ "setfcap",           CAP_SETFCAP           },
	{ "mac_override",      CAP_MAC_OVERRIDE      },
	{ "mac_admin",         CAP_MAC_ADMIN         },
};

#if 0 /* will be reactivated with image mounting support */
static int configure_find_fstype_cb(char* buffer, void *data)
{
	struct cbarg {
		const char *rootfs;
		const char *testdir;
		char *fstype;
		int mntopt;
	} *cbarg = data;

	char *fstype;

	/* we don't try 'nodev' entries */
	if (strstr(buffer, "nodev"))
		return 0;

	fstype = buffer;
	fstype += lxc_char_left_gc(fstype, strlen(fstype));
	fstype[lxc_char_right_gc(fstype, strlen(fstype))] = '\0';

	if (mount(cbarg->rootfs, cbarg->testdir, fstype, cbarg->mntopt, NULL))
		return 0;

	/* found ! */
	umount(cbarg->testdir);
	strcpy(cbarg->fstype, fstype);

	return 1;
}

/* find the filesystem type with brute force */
static int configure_find_fstype(const char *rootfs, char *fstype, int mntopt)
{
	int i, found;

	struct cbarg {
		const char *rootfs;
		const char *testdir;
		char *fstype;
		int mntopt;
	} cbarg = {
		.rootfs = rootfs,
		.fstype = fstype,
		.mntopt = mntopt,
	};

	/* first we check with /etc/filesystems, in case the modules
	 * are auto-loaded and fall back to the supported kernel fs
	 */
	char *fsfile[] = {
		"/etc/filesystems",
		"/proc/filesystems",
	};

	cbarg.testdir = tempnam("/tmp", "lxc-");
	if (!cbarg.testdir) {
		SYSERROR("failed to build a temp name");
		return -1;
	}

	if (mkdir(cbarg.testdir, 0755)) {
		SYSERROR("failed to create temporary directory");
		return -1;
	}

	for (i = 0; i < sizeof(fsfile)/sizeof(fsfile[0]); i++) {

		found = lxc_file_for_each_line(fsfile[i],
					       configure_find_fstype_cb,
					       &cbarg);

		if (found < 0) {
			SYSERROR("failed to read '%s'", fsfile[i]);
			goto out;
		}

		if (found)
			break;
	}

	if (!found) {
		ERROR("failed to determine fs type for '%s'", rootfs);
		goto out;
	}

out:
	rmdir(cbarg.testdir);
	return found - 1;
}

static int configure_rootfs_dir_cb(const char *rootfs, const char *absrootfs,
				   FILE *f)
{
	return fprintf(f, "%s %s none rbind 0 0\n", absrootfs, rootfs);
}

static int configure_rootfs_blk_cb(const char *rootfs, const char *absrootfs,
				   FILE *f)
{
	char fstype[MAXPATHLEN];

	if (configure_find_fstype(absrootfs, fstype, 0)) {
		ERROR("failed to configure mount for block device '%s'",
			      absrootfs);
		return -1;
	}

	return fprintf(f, "%s %s %s defaults 0 0\n", absrootfs, rootfs, fstype);
}

static int configure_rootfs(const char *name, const char *rootfs)
{
	char path[MAXPATHLEN];
	char absrootfs[MAXPATHLEN];
	char fstab[MAXPATHLEN];
	struct stat s;
	FILE *f;
	int i, ret;

	typedef int (*rootfs_cb)(const char *, const char *, FILE *);

	struct rootfs_type {
		int type;
		rootfs_cb cb;
	} rtfs_type[] = {
		{ __S_IFDIR, configure_rootfs_dir_cb },
		{ __S_IFBLK, configure_rootfs_blk_cb },
	};

	if (!realpath(rootfs, absrootfs)) {
		SYSERROR("failed to get real path for '%s'", rootfs);
		return -1;
	}

	snprintf(path, MAXPATHLEN, LXCPATH "/%s/rootfs", name);

	if (mkdir(path, 0755)) {
		SYSERROR("failed to create the '%s' directory", path);
		return -1;
	}

	if (access(absrootfs, F_OK)) {
		SYSERROR("'%s' is not accessible", absrootfs);
		return -1;
	}

	if (stat(absrootfs, &s)) {
		SYSERROR("failed to stat '%s'", absrootfs);
		return -1;
	}

	for (i = 0; i < sizeof(rtfs_type)/sizeof(rtfs_type[0]); i++) {

		if (!__S_ISTYPE(s.st_mode, rtfs_type[i].type))
			continue;

		snprintf(fstab, MAXPATHLEN, LXCPATH "/%s/fstab", name);

		f = fopen(fstab, "a+");
		if (!f) {
			SYSERROR("failed to open fstab file");
			return -1;
		}

		ret = rtfs_type[i].cb(path, absrootfs, f);

		fclose(f);

		if (ret < 0) {
			ERROR("failed to add rootfs mount in fstab");
			return -1;
		}

		snprintf(path, MAXPATHLEN, LXCPATH "/%s/rootfs/rootfs", name);

		return symlink(absrootfs, path);
	}

	ERROR("unsupported rootfs type for '%s'", absrootfs);
	return -1;
}
#endif

static int setup_utsname(struct utsname *utsname)
{
	if (!utsname)
		return 0;

	if (sethostname(utsname->nodename, strlen(utsname->nodename))) {
		SYSERROR("failed to set the hostname to '%s'", utsname->nodename);
		return -1;
	}

	INFO("'%s' hostname has been setup", utsname->nodename);

	return 0;
}

static int setup_tty(const struct lxc_rootfs *rootfs,
		     const struct lxc_tty_info *tty_info)
{
	char path[MAXPATHLEN];
	int i;

	for (i = 0; i < tty_info->nbtty; i++) {

		struct lxc_pty_info *pty_info = &tty_info->pty_info[i];

		snprintf(path, sizeof(path), "%s/dev/tty%d",
			 rootfs->path ? rootfs->path : "", i + 1);

		/* At this point I can not use the "access" function
		 * to check the file is present or not because it fails
		 * with EACCES errno and I don't know why :( */

		if (mount(pty_info->name, path, "none", MS_BIND, 0)) {
			WARN("failed to mount '%s'->'%s'",
			     pty_info->name, path);
			continue;
		}
	}

	INFO("%d tty(s) has been setup", tty_info->nbtty);

	return 0;
}

static int setup_rootfs_pivot_root_cb(char *buffer, void *data)
{
	struct lxc_list	*mountlist, *listentry, *iterator;
	char *pivotdir, *mountpoint, *mountentry;
	int found;
	void **cbparm;

	mountentry = buffer;
	cbparm = (void **)data;

	mountlist = cbparm[0];
	pivotdir  = cbparm[1];

	/* parse entry, first field is mountname, ignore */
	mountpoint = strtok(mountentry, " ");
	if (!mountpoint)
		return -1;

	/* second field is mountpoint */
	mountpoint = strtok(NULL, " ");
	if (!mountpoint)
		return -1;

	/* only consider mountpoints below old root fs */
	if (strncmp(mountpoint, pivotdir, strlen(pivotdir)))
		return 0;

	/* filter duplicate mountpoints */
	found = 0;
	lxc_list_for_each(iterator, mountlist) {
		if (!strcmp(iterator->elem, mountpoint)) {
			found = 1;
			break;
		}
	}
	if (found)
		return 0;

	/* add entry to list */
	listentry = malloc(sizeof(*listentry));
	if (!listentry) {
		SYSERROR("malloc for mountpoint listentry failed");
		return -1;
	}

	listentry->elem = strdup(mountpoint);
	if (!listentry->elem) {
		SYSERROR("strdup failed");
		return -1;
	}
	lxc_list_add_tail(mountlist, listentry);

	return 0;
}

static int umount_oldrootfs(const char *oldrootfs)
{
	char path[MAXPATHLEN];
	void *cbparm[2];
	struct lxc_list mountlist, *iterator;
	int ok, still_mounted, last_still_mounted;

	/* read and parse /proc/mounts in old root fs */
	lxc_list_init(&mountlist);

	/* oldrootfs is on the top tree directory now */
	snprintf(path, sizeof(path), "/%s", oldrootfs);
	cbparm[0] = &mountlist;

	cbparm[1] = strdup(path);
	if (!cbparm[1]) {
		SYSERROR("strdup failed");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/proc/mounts", oldrootfs);

	ok = lxc_file_for_each_line(path,
				    setup_rootfs_pivot_root_cb, &cbparm);
	if (ok < 0) {
		SYSERROR("failed to read or parse mount list '%s'", path);
		return -1;
	}

	/* umount filesystems until none left or list no longer shrinks */
	still_mounted = 0;
	do {
		last_still_mounted = still_mounted;
		still_mounted = 0;

		lxc_list_for_each(iterator, &mountlist) {

			/* umount normally */
			if (!umount(iterator->elem)) {
				DEBUG("umounted '%s'", (char *)iterator->elem);
				lxc_list_del(iterator);
				continue;
			}

			still_mounted++;
		}

	} while (still_mounted > 0 && still_mounted != last_still_mounted);


	lxc_list_for_each(iterator, &mountlist) {

		/* let's try a lazy umount */
		if (!umount2(iterator->elem, MNT_DETACH)) {
			INFO("lazy unmount of '%s'", (char *)iterator->elem);
			continue;
		}

		/* be more brutal (nfs) */
		if (!umount2(iterator->elem, MNT_FORCE)) {
			INFO("forced unmount of '%s'", (char *)iterator->elem);
			continue;
		}

		WARN("failed to unmount '%s'", (char *)iterator->elem);
	}

	return 0;
}

static int setup_rootfs_pivot_root(const char *rootfs, const char *pivotdir)
{
	char path[MAXPATHLEN];
	int remove_pivotdir = 0;

	/* change into new root fs */
	if (chdir(rootfs)) {
		SYSERROR("can't chdir to new rootfs '%s'", rootfs);
		return -1;
	}

	if (!pivotdir)
		pivotdir = "mnt";

	/* compute the full path to pivotdir under rootfs */
	snprintf(path, sizeof(path), "%s/%s", rootfs, pivotdir);

	if (access(path, F_OK)) {

		if (mkdir_p(path, 0755)) {
			SYSERROR("failed to create pivotdir '%s'", path);
			return -1;
		}

		remove_pivotdir = 1;
		DEBUG("created '%s' directory", path);
	}

	DEBUG("mountpoint for old rootfs is '%s'", path);

	/* pivot_root into our new root fs */
	if (pivot_root(".", path)) {
		SYSERROR("pivot_root syscall failed");
		return -1;
	}

	if (chdir("/")) {
		SYSERROR("can't chdir to / after pivot_root");
		return -1;
	}

	DEBUG("pivot_root syscall to '%s' successful", rootfs);

	/* we switch from absolute path to relative path */
	if (umount_oldrootfs(pivotdir))
		return -1;

	/* remove temporary mount point, we don't consider the removing
	 * as fatal */
	if (remove_pivotdir && rmdir(pivotdir))
		WARN("can't remove mountpoint '%s': %m", pivotdir);

	INFO("pivoted to '%s'", rootfs);

	return 0;
}

static int setup_rootfs(const struct lxc_rootfs *rootfs)
{
	char *mpath = LXCROOTFSMOUNT;

	if (!rootfs->path)
		return 0;

	if (rootfs->mount)
		mpath = rootfs->mount;

	if (access(mpath, F_OK)) {
		SYSERROR("failed to access to '%s', check it is present",
			 mpath);
		return -1;
	}

	if (mount(rootfs->path, mpath, "none", MS_BIND|MS_REC, NULL)) {
		SYSERROR("failed to mount '%s'->'%s'", rootfs->path, mpath);
		return -1;
	}

	DEBUG("mounted '%s' on '%s'", rootfs->path, mpath);

	if (setup_rootfs_pivot_root(mpath, rootfs->pivot)) {
		ERROR("failed to setup pivot root");
		return -1;
	}

	return 0;
}

static int setup_pts(int pts)
{
	if (!pts)
		return 0;

	if (!access("/dev/pts/ptmx", F_OK) && umount("/dev/pts")) {
		SYSERROR("failed to umount 'dev/pts'");
		return -1;
	}

	if (mount("devpts", "/dev/pts", "devpts", MS_MGC_VAL, "newinstance,ptmxmode=0666")) {
		SYSERROR("failed to mount a new instance of '/dev/pts'");
		return -1;
	}

	if (access("/dev/ptmx", F_OK)) {
		if (!symlink("/dev/pts/ptmx", "/dev/ptmx"))
			goto out;
		SYSERROR("failed to symlink '/dev/pts/ptmx'->'/dev/ptmx'");
		return -1;
	}

	/* fallback here, /dev/pts/ptmx exists just mount bind */
	if (mount("/dev/pts/ptmx", "/dev/ptmx", "none", MS_BIND, 0)) {
		SYSERROR("mount failed '/dev/pts/ptmx'->'/dev/ptmx'");
		return -1;
	}

	INFO("created new pts instance");

out:
	return 0;
}

static int setup_console(const struct lxc_rootfs *rootfs,
			 const struct lxc_console *console)
{
	char path[MAXPATHLEN];
	struct stat s;

	/* We don't have a rootfs, /dev/console will be shared */
	if (!rootfs->path)
		return 0;

	snprintf(path, sizeof(path), "%s/dev/console", rootfs->path);

	if (access(path, F_OK)) {
		WARN("rootfs specified but no console found");
		return 0;
	}

	if (console->peer == -1) {
		INFO("no console output required");
		return 0;
	}

	if (stat(path, &s)) {
		SYSERROR("failed to stat '%s'", path);
		return -1;
	}

	if (chmod(console->name, s.st_mode)) {
		SYSERROR("failed to set mode '0%o' to '%s'",
			 s.st_mode, console->name);
		return -1;
	}

	if (mount(console->name, path, "none", MS_BIND, 0)) {
		ERROR("failed to mount '%s' on '%s'", console->name, path);
		return -1;
	}

	INFO("console has been setup");

	return 0;
}

static int setup_cgroup(const char *name, struct lxc_list *cgroups)
{
	struct lxc_list *iterator;
	struct lxc_cgroup *cg;
	int ret = -1;

	if (lxc_list_empty(cgroups))
		return 0;

	lxc_list_for_each(iterator, cgroups) {

		cg = iterator->elem;

		if (lxc_cgroup_set(name, cg->subsystem, cg->value))
			goto out;

		DEBUG("cgroup '%s' set to '%s'", cg->subsystem, cg->value);
	}

	ret = 0;
	INFO("cgroup has been setup");
out:
	return ret;
}

static void parse_mntopt(char *opt, unsigned long *flags, char **data)
{
	struct mount_opt *mo;

	/* If opt is found in mount_opt, set or clear flags.
	 * Otherwise append it to data. */

	for (mo = &mount_opt[0]; mo->name != NULL; mo++) {
		if (!strncmp(opt, mo->name, strlen(mo->name))) {
			if (mo->clear)
				*flags &= ~mo->flag;
			else
				*flags |= mo->flag;
			return;
		}
	}

	if (strlen(*data))
		strcat(*data, ",");
	strcat(*data, opt);
}

static int parse_mntopts(struct mntent *mntent, unsigned long *mntflags,
			 char **mntdata)
{
	char *s, *data;
	char *p, *saveptr = NULL;

	if (!mntent->mnt_opts)
		return 0;

	s = strdup(mntent->mnt_opts);
	if (!s) {
		SYSERROR("failed to allocate memory");
		return -1;
	}

	data = malloc(strlen(s) + 1);
	if (!data) {
		SYSERROR("failed to allocate memory");
		free(s);
		return -1;
	}
	*data = 0;

	for (p = strtok_r(s, ",", &saveptr); p != NULL;
	     p = strtok_r(NULL, ",", &saveptr))
		parse_mntopt(p, mntflags, &data);

	if (*data)
		*mntdata = data;
	else
		free(data);
	free(s);

	return 0;
}

static int mount_file_entries(FILE *file)
{
	struct mntent *mntent;
	int ret = -1;
	unsigned long mntflags;
	char *mntdata;

	while ((mntent = getmntent(file))) {

		mntflags = 0;
		mntdata = NULL;
		if (parse_mntopts(mntent, &mntflags, &mntdata) < 0) {
			ERROR("failed to parse mount option '%s'",
				      mntent->mnt_opts);
			goto out;
		}

		if (mount(mntent->mnt_fsname, mntent->mnt_dir,
			  mntent->mnt_type, mntflags & ~MS_REMOUNT, mntdata)) {
			SYSERROR("failed to mount '%s' on '%s'",
					 mntent->mnt_fsname, mntent->mnt_dir);
			goto out;
		}

		if ((mntflags & MS_REMOUNT) == MS_REMOUNT ||
		    ((mntflags & MS_BIND) == MS_BIND)) {

			DEBUG ("remounting %s on %s to respect bind " \
			       "or remount options",
			       mntent->mnt_fsname, mntent->mnt_dir);

			if (mount(mntent->mnt_fsname, mntent->mnt_dir,
				  mntent->mnt_type,
				  mntflags | MS_REMOUNT, mntdata)) {
				SYSERROR("failed to mount '%s' on '%s'",
					 mntent->mnt_fsname, mntent->mnt_dir);
				goto out;
			}
		}

		DEBUG("mounted %s on %s, type %s", mntent->mnt_fsname,
		      mntent->mnt_dir, mntent->mnt_type);

		free(mntdata);
	}

	ret = 0;

	INFO("mount points have been setup");
out:
	return ret;
}

static int setup_mount(const char *fstab)
{
	FILE *file;
	int ret;

	if (!fstab)
		return 0;

	file = setmntent(fstab, "r");
	if (!file) {
		SYSERROR("failed to use '%s'", fstab);
		return -1;
	}

	ret = mount_file_entries(file);

	endmntent(file);
	return ret;
}

static int setup_mount_entries(struct lxc_list *mount)
{
	FILE *file;
	struct lxc_list *iterator;
	char *mount_entry;
	int ret;

	file = tmpfile();
	if (!file) {
		ERROR("tmpfile error: %m");
		return -1;
	}

	lxc_list_for_each(iterator, mount) {
		mount_entry = iterator->elem;
		fprintf(file, "%s\n", mount_entry);
	}

	rewind(file);

	ret = mount_file_entries(file);

	fclose(file);
	return ret;
}

static int setup_caps(struct lxc_list *caps)
{
	struct lxc_list *iterator;
	char *drop_entry;
	int i, capid;

	lxc_list_for_each(iterator, caps) {

		drop_entry = iterator->elem;

		capid = -1;

		for (i = 0; i < sizeof(caps_opt)/sizeof(caps_opt[0]); i++) {

			if (strcmp(drop_entry, caps_opt[i].name))
				continue;

			capid = caps_opt[i].value;
			break;
		}

	        if (capid < 0) {
			ERROR("unknown capability %s", drop_entry);
			return -1;
		}

		DEBUG("drop capability '%s' (%d)", drop_entry, capid);

		if (prctl(PR_CAPBSET_DROP, capid, 0, 0, 0)) {
                       SYSERROR("failed to remove %s capability", drop_entry);
                       return -1;
                }

	}

	DEBUG("capabilities has been setup");

	return 0;
}

static int setup_hw_addr(char *hwaddr, const char *ifname)
{
	struct sockaddr sockaddr;
	struct ifreq ifr;
	int ret, fd;

	ret = lxc_convert_mac(hwaddr, &sockaddr);
	if (ret) {
		ERROR("mac address '%s' conversion failed : %s",
		      hwaddr, strerror(-ret));
		return -1;
	}

	memcpy(ifr.ifr_name, ifname, IFNAMSIZ);
	memcpy((char *) &ifr.ifr_hwaddr, (char *) &sockaddr, sizeof(sockaddr));

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		ERROR("socket failure : %s", strerror(errno));
		return -1;
	}

	ret = ioctl(fd, SIOCSIFHWADDR, &ifr);
	close(fd);
	if (ret)
		ERROR("ioctl failure : %s", strerror(errno));

	DEBUG("mac address '%s' on '%s' has been setup", hwaddr, ifname);

	return ret;
}

static int setup_ipv4_addr(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	struct lxc_inetdev *inetdev;
	int err;

	lxc_list_for_each(iterator, ip) {

		inetdev = iterator->elem;

		err = lxc_ipv4_addr_add(ifindex, &inetdev->addr,
					&inetdev->bcast, inetdev->prefix);
		if (err) {
			ERROR("failed to setup_ipv4_addr ifindex %d : %s",
			      ifindex, strerror(-err));
			return -1;
		}
	}

	return 0;
}

static int setup_ipv6_addr(struct lxc_list *ip, int ifindex)
{
	struct lxc_list *iterator;
	struct lxc_inet6dev *inet6dev;
	int err;

	lxc_list_for_each(iterator, ip) {

		inet6dev = iterator->elem;

		err = lxc_ipv6_addr_add(ifindex, &inet6dev->addr,
					&inet6dev->mcast, &inet6dev->acast,
					inet6dev->prefix);
		if (err) {
			ERROR("failed to setup_ipv6_addr ifindex %d : %s",
			      ifindex, strerror(-err));
			return -1;
		}
	}

	return 0;
}

static int setup_netdev(struct lxc_netdev *netdev)
{
	char ifname[IFNAMSIZ];
	char *current_ifname = ifname;
	int err;

	/* empty network namespace */
	if (!netdev->ifindex) {
		if (netdev->flags | IFF_UP) {
			err = lxc_device_up("lo");
			if (err) {
				ERROR("failed to set the loopback up : %s",
				      strerror(-err));
				return -1;
			}
			return 0;
		}
	}

	/* retrieve the name of the interface */
	if (!if_indextoname(netdev->ifindex, current_ifname)) {
		ERROR("no interface corresponding to index '%d'",
		      netdev->ifindex);
		return -1;
	}

	/* default: let the system to choose one interface name */
	if (!netdev->name)
		netdev->name = netdev->type == LXC_NET_PHYS ?
			netdev->link : "eth%d";

	/* rename the interface name */
	err = lxc_device_rename(ifname, netdev->name);
	if (err) {
		ERROR("failed to rename %s->%s : %s", ifname, netdev->name,
		      strerror(-err));
		return -1;
	}

	/* Re-read the name of the interface because its name has changed
	 * and would be automatically allocated by the system
	 */
	if (!if_indextoname(netdev->ifindex, current_ifname)) {
		ERROR("no interface corresponding to index '%d'",
		      netdev->ifindex);
		return -1;
	}

	/* set a mac address */
	if (netdev->hwaddr) {
		if (setup_hw_addr(netdev->hwaddr, current_ifname)) {
			ERROR("failed to setup hw address for '%s'",
			      current_ifname);
			return -1;
		}
	}

	/* setup ipv4 addresses on the interface */
	if (setup_ipv4_addr(&netdev->ipv4, netdev->ifindex)) {
		ERROR("failed to setup ip addresses for '%s'",
			      ifname);
		return -1;
	}

	/* setup ipv6 addresses on the interface */
	if (setup_ipv6_addr(&netdev->ipv6, netdev->ifindex)) {
		ERROR("failed to setup ipv6 addresses for '%s'",
			      ifname);
		return -1;
	}

	/* set the network device up */
	if (netdev->flags | IFF_UP) {
		int err;

		err = lxc_device_up(current_ifname);
		if (err) {
			ERROR("failed to set '%s' up : %s", current_ifname,
			      strerror(-err));
			return -1;
		}

		/* the network is up, make the loopback up too */
		err = lxc_device_up("lo");
		if (err) {
			ERROR("failed to set the loopback up : %s",
			      strerror(-err));
			return -1;
		}
	}

	DEBUG("'%s' has been setup", current_ifname);

	return 0;
}

static int setup_network(struct lxc_list *network)
{
	struct lxc_list *iterator;
	struct lxc_netdev *netdev;

	lxc_list_for_each(iterator, network) {

		netdev = iterator->elem;

		if (setup_netdev(netdev)) {
			ERROR("failed to setup netdev");
			return -1;
		}
	}

	if (!lxc_list_empty(network))
		INFO("network has been setup");

	return 0;
}

struct lxc_conf *lxc_conf_init(void)
{
	struct lxc_conf *new;

	new = 	malloc(sizeof(*new));
	if (!new) {
		ERROR("lxc_conf_init : %m");
		return NULL;
	}
	memset(new, 0, sizeof(*new));

	new->console.path = NULL;
	new->console.peer = -1;
	new->console.master = -1;
	new->console.slave = -1;
	new->console.name[0] = '\0';
	lxc_list_init(&new->cgroup);
	lxc_list_init(&new->network);
	lxc_list_init(&new->mount_list);
	lxc_list_init(&new->caps);

	return new;
}

static int instanciate_veth(struct lxc_netdev *netdev)
{
	char veth1buf[IFNAMSIZ], *veth1;
	char veth2buf[IFNAMSIZ], *veth2;
	int err;

	if (netdev->priv.veth_attr.pair)
		veth1 = netdev->priv.veth_attr.pair;
	else {
		snprintf(veth1buf, sizeof(veth1buf), "vethXXXXXX");
		veth1 = mktemp(veth1buf);
	}

	snprintf(veth2buf, sizeof(veth2buf), "vethXXXXXX");
	veth2 = mktemp(veth2buf);

	if (!strlen(veth1) || !strlen(veth2)) {
		ERROR("failed to allocate a temporary name");
		return -1;
	}

	err = lxc_veth_create(veth1, veth2);
	if (err) {
		ERROR("failed to create %s-%s : %s", veth1, veth2,
		      strerror(-err));
		return -1;
	}

	if (netdev->mtu) {
		err = lxc_device_set_mtu(veth1, atoi(netdev->mtu));
		if (!err)
			err = lxc_device_set_mtu(veth2, atoi(netdev->mtu));
		if (err) {
			ERROR("failed to set mtu '%s' for %s-%s : %s",
			      netdev->mtu, veth1, veth2, strerror(-err));
			goto out_delete;
		}
	}

	if (netdev->link) {
		err = lxc_bridge_attach(netdev->link, veth1);
		if (err) {
			ERROR("failed to attach '%s' to the bridge '%s' : %s",
				      veth1, netdev->link, strerror(-err));
			goto out_delete;
		}
	}

	netdev->ifindex = if_nametoindex(veth2);
	if (!netdev->ifindex) {
		ERROR("failed to retrieve the index for %s", veth2);
		goto out_delete;
	}

	if (netdev->flags & IFF_UP) {
		err = lxc_device_up(veth1);
		if (err) {
			ERROR("failed to set %s up : %s", veth1,
			      strerror(-err));
			goto out_delete;
		}
	}

	DEBUG("instanciated veth '%s/%s', index is '%d'",
	      veth1, veth2, netdev->ifindex);

	return 0;

out_delete:
	lxc_device_delete(veth1);
	return -1;
}

static int instanciate_macvlan(struct lxc_netdev *netdev)
{
	char peerbuf[IFNAMSIZ], *peer;
	int err;

	if (!netdev->link) {
		ERROR("no link specified for macvlan netdev");
		return -1;
	}

	snprintf(peerbuf, sizeof(peerbuf), "mcXXXXXX");

	peer = mktemp(peerbuf);
	if (!strlen(peer)) {
		ERROR("failed to make a temporary name");
		return -1;
	}

	err = lxc_macvlan_create(netdev->link, peer,
				 netdev->priv.macvlan_attr.mode);
	if (err) {
		ERROR("failed to create macvlan interface '%s' on '%s' : %s",
		      peer, netdev->link, strerror(-err));
		return -1;
	}

	netdev->ifindex = if_nametoindex(peer);
	if (!netdev->ifindex) {
		ERROR("failed to retrieve the index for %s", peer);
		lxc_device_delete(peer);
		return -1;
	}

	DEBUG("instanciated macvlan '%s', index is '%d' and mode '%d'",
	      peer, netdev->ifindex, netdev->priv.macvlan_attr.mode);

	return 0;
}

/* XXX: merge with instanciate_macvlan */
static int instanciate_vlan(struct lxc_netdev *netdev)
{
	char peer[IFNAMSIZ];
	int err;

	if (!netdev->link) {
		ERROR("no link specified for vlan netdev");
		return -1;
	}

	snprintf(peer, sizeof(peer), "vlan%d", netdev->priv.vlan_attr.vid);

	err = lxc_vlan_create(netdev->link, peer, netdev->priv.vlan_attr.vid);
	if (err) {
		ERROR("failed to create vlan interface '%s' on '%s' : %s",
		      peer, netdev->link, strerror(-err));
		return -1;
	}

	netdev->ifindex = if_nametoindex(peer);
	if (!netdev->ifindex) {
		ERROR("failed to retrieve the ifindex for %s", peer);
		lxc_device_delete(peer);
		return -1;
	}

	DEBUG("instanciated vlan '%s', ifindex is '%d'", " vlan1000",
	      netdev->ifindex);

	return 0;
}

static int instanciate_phys(struct lxc_netdev *netdev)
{
	if (!netdev->link) {
		ERROR("no link specified for the physical interface");
		return -1;
	}

	netdev->ifindex = if_nametoindex(netdev->link);
	if (!netdev->ifindex) {
		ERROR("failed to retrieve the index for %s", netdev->link);
		return -1;
	}

	return 0;
}

static int instanciate_empty(struct lxc_netdev *netdev)
{
	netdev->ifindex = 0;
	return 0;
}

int lxc_create_network(struct lxc_list *network)
{
	struct lxc_list *iterator;
	struct lxc_netdev *netdev;

	lxc_list_for_each(iterator, network) {

		netdev = iterator->elem;

		if (netdev->type < 0 || netdev->type > LXC_NET_MAXCONFTYPE) {
			ERROR("invalid network configuration type '%d'",
			      netdev->type);
			return -1;
		}

		if (netdev_conf[netdev->type](netdev)) {
			ERROR("failed to create netdev");
			return -1;
		}
	}

	return 0;
}

void lxc_delete_network(struct lxc_list *network)
{
	struct lxc_list *iterator;
	struct lxc_netdev *netdev;

	lxc_list_for_each(iterator, network) {
		netdev = iterator->elem;
		if (netdev->ifindex > 0 && netdev->type != LXC_NET_PHYS)
			lxc_device_delete_index(netdev->ifindex);
	}
}

int lxc_assign_network(struct lxc_list *network, pid_t pid)
{
	struct lxc_list *iterator;
	struct lxc_netdev *netdev;
	int err;

	lxc_list_for_each(iterator, network) {

		netdev = iterator->elem;

		/* empty network namespace, nothing to move */
		if (!netdev->ifindex)
			continue;

		err = lxc_device_move(netdev->ifindex, pid);
		if (err) {
			ERROR("failed to move '%s' to the container : %s",
			      netdev->link, strerror(-err));
			return -1;
		}

		DEBUG("move '%s' to '%d'", netdev->link, pid);
	}

	return 0;
}

int lxc_create_tty(const char *name, struct lxc_conf *conf)
{
	struct lxc_tty_info *tty_info = &conf->tty_info;
	int i;

	/* no tty in the configuration */
	if (!conf->tty)
		return 0;

	tty_info->pty_info =
		malloc(sizeof(*tty_info->pty_info)*conf->tty);
	if (!tty_info->pty_info) {
		SYSERROR("failed to allocate pty_info");
		return -1;
	}

	for (i = 0; i < conf->tty; i++) {

		struct lxc_pty_info *pty_info = &tty_info->pty_info[i];

		if (openpty(&pty_info->master, &pty_info->slave,
			    pty_info->name, NULL, NULL)) {
			SYSERROR("failed to create pty #%d", i);
			tty_info->nbtty = i;
			lxc_delete_tty(tty_info);
			return -1;
		}

		DEBUG("allocated pty '%s' (%d/%d)",
		      pty_info->name, pty_info->master, pty_info->slave);

                /* Prevent leaking the file descriptors to the container */
		fcntl(pty_info->master, F_SETFD, FD_CLOEXEC);
		fcntl(pty_info->slave, F_SETFD, FD_CLOEXEC);

		pty_info->busy = 0;
	}

	tty_info->nbtty = conf->tty;

	INFO("tty's configured");

	return 0;
}

void lxc_delete_tty(struct lxc_tty_info *tty_info)
{
	int i;

	for (i = 0; i < tty_info->nbtty; i++) {
		struct lxc_pty_info *pty_info = &tty_info->pty_info[i];

		close(pty_info->master);
		close(pty_info->slave);
	}

	free(tty_info->pty_info);
	tty_info->nbtty = 0;
}

int lxc_setup(const char *name, struct lxc_conf *lxc_conf)
{
	if (setup_utsname(lxc_conf->utsname)) {
		ERROR("failed to setup the utsname for '%s'", name);
		return -1;
	}

	if (setup_network(&lxc_conf->network)) {
		ERROR("failed to setup the network for '%s'", name);
		return -1;
	}

	if (setup_cgroup(name, &lxc_conf->cgroup)) {
		ERROR("failed to setup the cgroups for '%s'", name);
		return -1;
	}

	if (setup_mount(lxc_conf->fstab)) {
		ERROR("failed to setup the mounts for '%s'", name);
		return -1;
	}

	if (setup_mount_entries(&lxc_conf->mount_list)) {
		ERROR("failed to setup the mount entries for '%s'", name);
		return -1;
	}

	if (setup_console(&lxc_conf->rootfs, &lxc_conf->console)) {
		ERROR("failed to setup the console for '%s'", name);
		return -1;
	}

	if (setup_tty(&lxc_conf->rootfs, &lxc_conf->tty_info)) {
		ERROR("failed to setup the ttys for '%s'", name);
		return -1;
	}

	if (setup_rootfs(&lxc_conf->rootfs)) {
		ERROR("failed to set rootfs for '%s'", name);
		return -1;
	}

	if (setup_pts(lxc_conf->pts)) {
		ERROR("failed to setup the new pts instance");
		return -1;
	}

	if (setup_caps(&lxc_conf->caps)) {
		ERROR("failed to drop capabilities");
		return -1;
	}

	NOTICE("'%s' is setup.", name);

	return 0;
}