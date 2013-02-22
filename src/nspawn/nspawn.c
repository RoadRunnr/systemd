/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <getopt.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <termios.h>
#include <sys/signalfd.h>
#include <grp.h>
#include <linux/fs.h>
#include <sys/un.h>
#include <sys/socket.h>

#include <systemd/sd-daemon.h>

#include "log.h"
#include "util.h"
#include "mkdir.h"
#include "macro.h"
#include "audit.h"
#include "missing.h"
#include "cgroup-util.h"
#include "strv.h"
#include "path-util.h"
#include "loopback-setup.h"
#include "sd-id128.h"
#include "dev-setup.h"
#include "fdset.h"
#include "build.h"
#include "fileio.h"

typedef enum LinkJournal {
        LINK_NO,
        LINK_AUTO,
        LINK_HOST,
        LINK_GUEST
} LinkJournal;

static char *arg_directory = NULL;
static char *arg_user = NULL;
static char **arg_controllers = NULL;
static char *arg_uuid = NULL;
static bool arg_private_network = false;
static char ***arg_network_if = NULL;
static bool arg_read_only = false;
static bool arg_boot = false;
static LinkJournal arg_link_journal = LINK_AUTO;
static uint64_t arg_retain =
        (1ULL << CAP_CHOWN) |
        (1ULL << CAP_DAC_OVERRIDE) |
        (1ULL << CAP_DAC_READ_SEARCH) |
        (1ULL << CAP_FOWNER) |
        (1ULL << CAP_FSETID) |
        (1ULL << CAP_IPC_OWNER) |
        (1ULL << CAP_KILL) |
        (1ULL << CAP_LEASE) |
        (1ULL << CAP_LINUX_IMMUTABLE) |
        (1ULL << CAP_NET_BIND_SERVICE) |
        (1ULL << CAP_NET_BROADCAST) |
        (1ULL << CAP_NET_RAW) |
        (1ULL << CAP_SETGID) |
        (1ULL << CAP_SETFCAP) |
        (1ULL << CAP_SETPCAP) |
        (1ULL << CAP_SETUID) |
        (1ULL << CAP_SYS_ADMIN) |
        (1ULL << CAP_SYS_CHROOT) |
        (1ULL << CAP_SYS_NICE) |
        (1ULL << CAP_SYS_PTRACE) |
        (1ULL << CAP_SYS_TTY_CONFIG) |
        (1ULL << CAP_SYS_RESOURCE) |
        (1ULL << CAP_SYS_BOOT) |
        (1ULL << CAP_AUDIT_WRITE) |
        (1ULL << CAP_AUDIT_CONTROL);

static int help(void) {

        printf("%s [OPTIONS...] [PATH] [ARGUMENTS...]\n\n"
               "Spawn a minimal namespace container for debugging, testing and building.\n\n"
               "  -h --help               Show this help\n"
               "  --version               Print version string\n"
               "  -D --directory=NAME     Root directory for the container\n"
               "  -b --boot               Boot up full system (i.e. invoke init)\n"
               "  -u --user=USER          Run the command under specified user or uid\n"
               "  -C --controllers=LIST   Put the container in specified comma-separated cgroup hierarchies\n"
               "     --uuid=UUID          Set a specific machine UUID for the container\n"
               "     --private-network    Disable network in container\n"
               "     --network-if         Add a virtual interface to the container\n"
               "     --read-only          Mount the root directory read-only\n"
               "     --capability=CAP     In addition to the default, retain specified capability\n"
               "     --link-journal=MODE  Link up guest journal, one of no, auto, guest, host\n"
               "  -j                      Equivalent to --link-journal=host\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_PRIVATE_NETWORK,
                ARG_UUID,
                ARG_READ_ONLY,
                ARG_CAPABILITY,
                ARG_LINK_JOURNAL,
                ARG_NETWORK_IF
        };

        static const struct option options[] = {
                { "help",            no_argument,       NULL, 'h'                 },
                { "version",         no_argument,       NULL, ARG_VERSION         },
                { "directory",       required_argument, NULL, 'D'                 },
                { "user",            required_argument, NULL, 'u'                 },
                { "controllers",     required_argument, NULL, 'C'                 },
                { "private-network", no_argument,       NULL, ARG_PRIVATE_NETWORK },
                { "network-if",      required_argument, NULL, ARG_NETWORK_IF      },
                { "boot",            no_argument,       NULL, 'b'                 },
                { "uuid",            required_argument, NULL, ARG_UUID            },
                { "read-only",       no_argument,       NULL, ARG_READ_ONLY       },
                { "capability",      required_argument, NULL, ARG_CAPABILITY      },
                { "link-journal",    required_argument, NULL, ARG_LINK_JOURNAL    },
                { NULL,              0,                 NULL, 0                   }
        };

        int c;
        int if_cnt = 0;
        char ***p = arg_network_if = calloc(sizeof(char *), 16);

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "+hD:u:C:bj", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case 'D':
                        free(arg_directory);
                        arg_directory = canonicalize_file_name(optarg);
                        if (!arg_directory) {
                                log_error("Failed to canonicalize root directory.");
                                return -ENOMEM;
                        }

                        break;

                case 'u':
                        free(arg_user);
                        if (!(arg_user = strdup(optarg))) {
                                log_error("Failed to duplicate user name.");
                                return -ENOMEM;
                        }

                        break;

                case 'C':
                        strv_free(arg_controllers);
                        arg_controllers = strv_split(optarg, ",");
                        if (!arg_controllers) {
                                log_error("Failed to split controllers list.");
                                return -ENOMEM;
                        }
                        strv_uniq(arg_controllers);

                        break;

                case ARG_PRIVATE_NETWORK:
                        arg_private_network = true;
                        break;

                case ARG_NETWORK_IF:
                        if (if_cnt >= 16)  {
                                log_error("Too many interface specification %s", optarg);
                                return -EINVAL;
                        }
                        if (strchr(optarg, ':') == NULL) {
                                log_error("Invalid interface specification %s", optarg);
                                return -EINVAL;
                        }

                        *(p++) = strv_split(optarg, ":");
                        if_cnt++;
                        break;

                case 'b':
                        arg_boot = true;
                        break;

                case ARG_UUID:
                        arg_uuid = optarg;
                        break;

                case ARG_READ_ONLY:
                        arg_read_only = true;
                        break;

                case ARG_CAPABILITY: {
                        char *state, *word;
                        size_t length;

                        FOREACH_WORD_SEPARATOR(word, length, optarg, ",", state) {
                                cap_value_t cap;
                                char *t;

                                t = strndup(word, length);
                                if (!t)
                                        return log_oom();

                                if (cap_from_name(t, &cap) < 0) {
                                        log_error("Failed to parse capability %s.", t);
                                        free(t);
                                        return -EINVAL;
                                }

                                free(t);
                                arg_retain |= 1ULL << (uint64_t) cap;
                        }

                        break;
                }

                case 'j':
                        arg_link_journal = LINK_GUEST;
                        break;

                case ARG_LINK_JOURNAL:
                        if (streq(optarg, "auto"))
                                arg_link_journal = LINK_AUTO;
                        else if (streq(optarg, "no"))
                                arg_link_journal = LINK_NO;
                        else if (streq(optarg, "guest"))
                                arg_link_journal = LINK_GUEST;
                        else if (streq(optarg, "host"))
                                arg_link_journal = LINK_HOST;
                        else {
                                log_error("Failed to parse link journal mode %s", optarg);
                                return -EINVAL;
                        }

                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }
        }

        return 1;
}

static int mount_all(const char *dest) {

        typedef struct MountPoint {
                const char *what;
                const char *where;
                const char *type;
                const char *options;
                unsigned long flags;
                bool fatal;
        } MountPoint;

        static const MountPoint mount_table[] = {
                { "proc",      "/proc",     "proc",  NULL,       MS_NOSUID|MS_NOEXEC|MS_NODEV, true  },
                { "/proc/sys", "/proc/sys", NULL,    NULL,       MS_BIND, true                       },   /* Bind mount first */
                { NULL,        "/proc/sys", NULL,    NULL,       MS_BIND|MS_RDONLY|MS_REMOUNT, true  },   /* Then, make it r/o */
                { "sysfs",     "/sys",      "sysfs", NULL,       MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV, true  },
                { "tmpfs",     "/dev",      "tmpfs", "mode=755", MS_NOSUID|MS_STRICTATIME,     true  },
                { "/dev/pts",  "/dev/pts",  NULL,    NULL,       MS_BIND,                      true  },
                { "tmpfs",     "/dev/shm",  "tmpfs", "mode=1777", MS_NOSUID|MS_NODEV|MS_STRICTATIME, true  },
                { "tmpfs",     "/run",      "tmpfs", "mode=755", MS_NOSUID|MS_NODEV|MS_STRICTATIME, true  },
#ifdef HAVE_SELINUX
                { "/sys/fs/selinux", "/sys/fs/selinux", NULL, NULL, MS_BIND,                      false },  /* Bind mount first */
                { NULL,              "/sys/fs/selinux", NULL, NULL, MS_BIND|MS_RDONLY|MS_REMOUNT, false },  /* Then, make it r/o */
#endif
        };

        unsigned k;
        int r = 0;

        for (k = 0; k < ELEMENTSOF(mount_table); k++) {
                char _cleanup_free_ *where = NULL;
                int t;

                if (asprintf(&where, "%s/%s", dest, mount_table[k].where) < 0) {
                        log_oom();

                        if (r == 0)
                                r = -ENOMEM;

                        break;
                }

                t = path_is_mount_point(where, true);
                if (t < 0) {
                        log_error("Failed to detect whether %s is a mount point: %s", where, strerror(-t));

                        if (r == 0)
                                r = t;

                        continue;
                }

                /* Skip this entry if it is not a remount. */
                if (mount_table[k].what && t > 0)
                        continue;

                mkdir_p_label(where, 0755);

                if (mount(mount_table[k].what,
                          where,
                          mount_table[k].type,
                          mount_table[k].flags,
                          mount_table[k].options) < 0 &&
                    mount_table[k].fatal) {

                        log_error("mount(%s) failed: %m", where);

                        if (r == 0)
                                r = -errno;
                }
        }

        return r;
}

static int setup_timezone(const char *dest) {
        _cleanup_free_ char *where = NULL, *p = NULL, *q = NULL, *check = NULL, *what = NULL;
        char *z, *y;
        int r;

        assert(dest);

        /* Fix the timezone, if possible */
        r = readlink_malloc("/etc/localtime", &p);
        if (r < 0) {
                log_warning("/etc/localtime is not a symlink, not updating container timezone.");
                return 0;
        }

        z = path_startswith(p, "../usr/share/zoneinfo/");
        if (!z)
                z = path_startswith(p, "/usr/share/zoneinfo/");
        if (!z) {
                log_warning("/etc/localtime does not point into /usr/share/zoneinfo/, not updating container timezone.");
                return 0;
        }

        where = strappend(dest, "/etc/localtime");
        if (!where)
                return log_oom();

        r = readlink_malloc(where, &q);
        if (r >= 0) {
                y = path_startswith(q, "../usr/share/zoneinfo/");
                if (!y)
                        y = path_startswith(q, "/usr/share/zoneinfo/");


                /* Already pointing to the right place? Then do nothing .. */
                if (y && streq(y, z))
                        return 0;
        }

        check = strjoin(dest, "/usr/share/zoneinfo/", z, NULL);
        if (!check)
                return log_oom();

        if (access(check, F_OK) < 0) {
                log_warning("Timezone %s does not exist in container, not updating container timezone.", z);
                return 0;
        }

        what = strappend("../usr/share/zoneinfo/", z);
        if (!what)
                return log_oom();

        unlink(where);
        if (symlink(what, where) < 0) {
                log_error("Failed to correct timezone of container: %m");
                return 0;
        }

        return 0;
}

static int setup_resolv_conf(const char *dest) {
        char *where;

        assert(dest);

        if (arg_private_network && *arg_network_if == NULL)
                return 0;

        /* Fix resolv.conf, if possible */
        where = strappend(dest, "/etc/resolv.conf");
        if (!where)
                return log_oom();

        /* We don't really care for the results of this really. If it
         * fails, it fails, but meh... */
        if (mount("/etc/resolv.conf", where, "bind", MS_BIND, NULL) >= 0)
                mount("/etc/resolv.conf", where, "bind", MS_BIND|MS_REMOUNT|MS_RDONLY, NULL);

        free(where);

        return 0;
}

static int setup_boot_id(const char *dest) {
        char _cleanup_free_ *from = NULL, *to = NULL;
        sd_id128_t rnd;
        char as_uuid[37];
        int r;

        assert(dest);

        /* Generate a new randomized boot ID, so that each boot-up of
         * the container gets a new one */

        from = strappend(dest, "/dev/proc-sys-kernel-random-boot-id");
        to = strappend(dest, "/proc/sys/kernel/random/boot_id");
        if (!from || !to)
                return log_oom();

        r = sd_id128_randomize(&rnd);
        if (r < 0) {
                log_error("Failed to generate random boot id: %s", strerror(-r));
                return r;
        }

        snprintf(as_uuid, sizeof(as_uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 SD_ID128_FORMAT_VAL(rnd));
        char_array_0(as_uuid);

        r = write_one_line_file(from, as_uuid);
        if (r < 0) {
                log_error("Failed to write boot id: %s", strerror(-r));
                return r;
        }

        if (mount(from, to, "bind", MS_BIND, NULL) < 0) {
                log_error("Failed to bind mount boot id: %m");
                r = -errno;
        } else
                mount(from, to, "bind", MS_BIND|MS_REMOUNT|MS_RDONLY, NULL);

        unlink(from);
        return r;
}

static int copy_devnodes(const char *dest) {

        static const char devnodes[] =
                "null\0"
                "zero\0"
                "full\0"
                "random\0"
                "urandom\0"
                "tty\0"
                "ptmx\0";

        const char *d;
        int r = 0;
        mode_t _cleanup_umask_ u;

        assert(dest);

        u = umask(0000);

        NULSTR_FOREACH(d, devnodes) {
                struct stat st;
                char _cleanup_free_ *from = NULL, *to = NULL;

                asprintf(&from, "/dev/%s", d);
                asprintf(&to, "%s/dev/%s", dest, d);

                if (!from || !to) {
                        log_oom();

                        if (r == 0)
                                r = -ENOMEM;

                        break;
                }

                if (stat(from, &st) < 0) {

                        if (errno != ENOENT) {
                                log_error("Failed to stat %s: %m", from);
                                if (r == 0)
                                        r = -errno;
                        }

                } else if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {

                        log_error("%s is not a char or block device, cannot copy", from);
                        if (r == 0)
                                r = -EIO;

                } else if (mknod(to, st.st_mode, st.st_rdev) < 0) {

                        log_error("mknod(%s) failed: %m", dest);
                        if (r == 0)
                                r = -errno;
                }
        }

        return r;
}

static int setup_dev_console(const char *dest, const char *console) {
        struct stat st;
        char _cleanup_free_ *to = NULL;
        int r;
        mode_t _cleanup_umask_ u;

        assert(dest);
        assert(console);

        u = umask(0000);

        if (stat(console, &st) < 0) {
                log_error("Failed to stat %s: %m", console);
                return -errno;

        } else if (!S_ISCHR(st.st_mode)) {
                log_error("/dev/console is not a char device");
                return -EIO;
        }

        r = chmod_and_chown(console, 0600, 0, 0);
        if (r < 0) {
                log_error("Failed to correct access mode for TTY: %s", strerror(-r));
                return r;
        }

        if (asprintf(&to, "%s/dev/console", dest) < 0)
                return log_oom();

        /* We need to bind mount the right tty to /dev/console since
         * ptys can only exist on pts file systems. To have something
         * to bind mount things on we create a device node first, that
         * has the right major/minor (note that the major minor
         * doesn't actually matter here, since we mount it over
         * anyway). */

        if (mknod(to, (st.st_mode & ~07777) | 0600, st.st_rdev) < 0) {
                log_error("mknod() for /dev/console failed: %m");
                return -errno;
        }

        if (mount(console, to, "bind", MS_BIND, NULL) < 0) {
                log_error("Bind mount for /dev/console failed: %m");
                return -errno;
        }

        return 0;
}

static int setup_kmsg(const char *dest, int kmsg_socket) {
        char _cleanup_free_ *from = NULL, *to = NULL;
        int r, fd, k;
        mode_t _cleanup_umask_ u;
        union {
                struct cmsghdr cmsghdr;
                uint8_t buf[CMSG_SPACE(sizeof(int))];
        } control;
        struct msghdr mh;
        struct cmsghdr *cmsg;

        assert(dest);
        assert(kmsg_socket >= 0);

        u = umask(0000);

        /* We create the kmsg FIFO as /dev/kmsg, but immediately
         * delete it after bind mounting it to /proc/kmsg. While FIFOs
         * on the reading side behave very similar to /proc/kmsg,
         * their writing side behaves differently from /dev/kmsg in
         * that writing blocks when nothing is reading. In order to
         * avoid any problems with containers deadlocking due to this
         * we simply make /dev/kmsg unavailable to the container. */
        if (asprintf(&from, "%s/dev/kmsg", dest) < 0 ||
            asprintf(&to, "%s/proc/kmsg", dest) < 0)
                return log_oom();

        if (mkfifo(from, 0600) < 0) {
                log_error("mkfifo() for /dev/kmsg failed: %m");
                return -errno;
        }

        r = chmod_and_chown(from, 0600, 0, 0);
        if (r < 0) {
                log_error("Failed to correct access mode for /dev/kmsg: %s", strerror(-r));
                return r;
        }

        if (mount(from, to, "bind", MS_BIND, NULL) < 0) {
                log_error("Bind mount for /proc/kmsg failed: %m");
                return -errno;
        }

        fd = open(from, O_RDWR|O_NDELAY|O_CLOEXEC);
        if (fd < 0) {
                log_error("Failed to open fifo: %m");
                return -errno;
        }

        zero(mh);
        zero(control);

        mh.msg_control = &control;
        mh.msg_controllen = sizeof(control);

        cmsg = CMSG_FIRSTHDR(&mh);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

        mh.msg_controllen = cmsg->cmsg_len;

        /* Store away the fd in the socket, so that it stays open as
         * long as we run the child */
        k = sendmsg(kmsg_socket, &mh, MSG_DONTWAIT|MSG_NOSIGNAL);
        close_nointr_nofail(fd);

        if (k < 0) {
                log_error("Failed to send FIFO fd: %m");
                return -errno;
        }

        /* And now make the FIFO unavailable as /dev/kmsg... */
        unlink(from);
        return 0;
}

static int setup_hostname(void) {
        char *hn;
        int r = 0;

        hn = path_get_file_name(arg_directory);
        if (hn) {
                hn = strdup(hn);
                if (!hn)
                        return -ENOMEM;

                hostname_cleanup(hn);

                if (!isempty(hn))
                        if (sethostname(hn, strlen(hn)) < 0)
                                r = -errno;

                free(hn);
        }

        return r;
}

static int setup_journal(const char *directory) {
        sd_id128_t machine_id;
        char _cleanup_free_ *p = NULL, *b = NULL, *q = NULL, *d = NULL;
        char *id;
        int r;

        if (arg_link_journal == LINK_NO)
                return 0;

        p = strappend(directory, "/etc/machine-id");
        if (!p)
                return log_oom();

        r = read_one_line_file(p, &b);
        if (r == -ENOENT && arg_link_journal == LINK_AUTO)
                return 0;
        else if (r < 0) {
                log_error("Failed to read machine ID from %s: %s", p, strerror(-r));
                return r;
        }

        id = strstrip(b);
        if (isempty(id) && arg_link_journal == LINK_AUTO)
                return 0;

        /* Verify validity */
        r = sd_id128_from_string(id, &machine_id);
        if (r < 0) {
                log_error("Failed to parse machine ID from %s: %s", p, strerror(-r));
                return r;
        }

        free(p);
        p = strappend("/var/log/journal/", id);
        q = strjoin(directory, "/var/log/journal/", id, NULL);
        if (!p || !q)
                return log_oom();

        if (path_is_mount_point(p, false) > 0) {
                if (arg_link_journal != LINK_AUTO) {
                        log_error("%s: already a mount point, refusing to use for journal", p);
                        return -EEXIST;
                }

                return 0;
        }

        if (path_is_mount_point(q, false) > 0) {
                if (arg_link_journal != LINK_AUTO) {
                        log_error("%s: already a mount point, refusing to use for journal", q);
                        return -EEXIST;
                }

                return 0;
        }

        r = readlink_and_make_absolute(p, &d);
        if (r >= 0) {
                if ((arg_link_journal == LINK_GUEST ||
                     arg_link_journal == LINK_AUTO) &&
                    path_equal(d, q)) {

                        r = mkdir_p(q, 0755);
                        if (r < 0)
                                log_warning("failed to create directory %s: %m", q);
                        return 0;
                }

                if (unlink(p) < 0) {
                        log_error("Failed to remove symlink %s: %m", p);
                        return -errno;
                }
        } else if (r == -EINVAL) {

                if (arg_link_journal == LINK_GUEST &&
                    rmdir(p) < 0) {

                        if (errno == ENOTDIR) {
                                log_error("%s already exists and is neither a symlink nor a directory", p);
                                return r;
                        } else {
                                log_error("Failed to remove %s: %m", p);
                                return -errno;
                        }
                }
        } else if (r != -ENOENT) {
                log_error("readlink(%s) failed: %m", p);
                return r;
        }

        if (arg_link_journal == LINK_GUEST) {

                if (symlink(q, p) < 0) {
                        log_error("Failed to symlink %s to %s: %m", q, p);
                        return -errno;
                }

                r = mkdir_p(q, 0755);
                if (r < 0)
                        log_warning("failed to create directory %s: %m", q);
                return 0;
        }

        if (arg_link_journal == LINK_HOST) {
                r = mkdir_p(p, 0755);
                if (r < 0) {
                        log_error("Failed to create %s: %m", p);
                        return r;
                }

        } else if (access(p, F_OK) < 0)
                return 0;

        if (dir_is_empty(q) == 0) {
                log_error("%s not empty.", q);
                return -ENOTEMPTY;
        }

        r = mkdir_p(q, 0755);
        if (r < 0) {
                log_error("Failed to create %s: %m", q);
                return r;
        }

        if (mount(p, q, "bind", MS_BIND, NULL) < 0) {
                log_error("Failed to bind mount journal from host into guest: %m");
                return -errno;
        }

        return 0;
}

static int drop_capabilities(void) {
        return capability_bounding_set_drop(~arg_retain, false);
}

static int is_os_tree(const char *path) {
        int r;
        char *p;
        /* We use /bin/sh as flag file if something is an OS */

        if (asprintf(&p, "%s/bin/sh", path) < 0)
                return -ENOMEM;

        r = access(p, F_OK);
        free(p);

        return r < 0 ? 0 : 1;
}

static int setup_private_network(int pid) {

        for (char ***p = arg_network_if; p; p++) {
                char cmd[PATH_MAX];

                snprintf(cmd, sizeof(cmd), "/sbin/ip link add name %s type veth peer name %s", (*p)[0], (*p)[1]);
                system(cmd);

                snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s netns %d", (*p)[1], pid);
                system(cmd);
        }

        return 0;
}

static int process_pty(int master, pid_t pid, sigset_t *mask) {

        char in_buffer[LINE_MAX], out_buffer[LINE_MAX];
        size_t in_buffer_full = 0, out_buffer_full = 0;
        struct epoll_event stdin_ev, stdout_ev, master_ev, signal_ev;
        bool stdin_readable = false, stdout_writable = false, master_readable = false, master_writable = false;
        int ep = -1, signal_fd = -1, r;
        bool tried_orderly_shutdown = false;

        assert(master >= 0);
        assert(pid > 0);
        assert(mask);

        fd_nonblock(STDIN_FILENO, 1);
        fd_nonblock(STDOUT_FILENO, 1);
        fd_nonblock(master, 1);

        signal_fd = signalfd(-1, mask, SFD_NONBLOCK|SFD_CLOEXEC);
        if (signal_fd < 0) {
                log_error("signalfd(): %m");
                r = -errno;
                goto finish;
        }

        ep = epoll_create1(EPOLL_CLOEXEC);
        if (ep < 0) {
                log_error("Failed to create epoll: %m");
                r = -errno;
                goto finish;
        }

        /* We read from STDIN only if this is actually a TTY,
         * otherwise we assume non-interactivity. */
        if (isatty(STDIN_FILENO)) {
                zero(stdin_ev);
                stdin_ev.events = EPOLLIN|EPOLLET;
                stdin_ev.data.fd = STDIN_FILENO;

                if (epoll_ctl(ep, EPOLL_CTL_ADD, STDIN_FILENO, &stdin_ev) < 0) {
                        log_error("Failed to register STDIN in epoll: %m");
                        r = -errno;
                        goto finish;
                }
        }

        zero(stdout_ev);
        stdout_ev.events = EPOLLOUT|EPOLLET;
        stdout_ev.data.fd = STDOUT_FILENO;

        zero(master_ev);
        master_ev.events = EPOLLIN|EPOLLOUT|EPOLLET;
        master_ev.data.fd = master;

        zero(signal_ev);
        signal_ev.events = EPOLLIN;
        signal_ev.data.fd = signal_fd;

        if (epoll_ctl(ep, EPOLL_CTL_ADD, STDOUT_FILENO, &stdout_ev) < 0) {
                if (errno != EPERM) {
                        log_error("Failed to register stdout in epoll: %m");
                        r = -errno;
                        goto finish;
                }
                /* stdout without epoll support. Likely redirected to regular file. */
                stdout_writable = true;
        }

        if (epoll_ctl(ep, EPOLL_CTL_ADD, master, &master_ev) < 0 ||
            epoll_ctl(ep, EPOLL_CTL_ADD, signal_fd, &signal_ev) < 0) {
                log_error("Failed to register fds in epoll: %m");
                r = -errno;
                goto finish;
        }

        for (;;) {
                struct epoll_event ev[16];
                ssize_t k;
                int i, nfds;

                nfds = epoll_wait(ep, ev, ELEMENTSOF(ev), -1);
                if (nfds < 0) {

                        if (errno == EINTR || errno == EAGAIN)
                                continue;

                        log_error("epoll_wait(): %m");
                        r = -errno;
                        goto finish;
                }

                assert(nfds >= 1);

                for (i = 0; i < nfds; i++) {
                        if (ev[i].data.fd == STDIN_FILENO) {

                                if (ev[i].events & (EPOLLIN|EPOLLHUP))
                                        stdin_readable = true;

                        } else if (ev[i].data.fd == STDOUT_FILENO) {

                                if (ev[i].events & (EPOLLOUT|EPOLLHUP))
                                        stdout_writable = true;

                        } else if (ev[i].data.fd == master) {

                                if (ev[i].events & (EPOLLIN|EPOLLHUP))
                                        master_readable = true;

                                if (ev[i].events & (EPOLLOUT|EPOLLHUP))
                                        master_writable = true;

                        } else if (ev[i].data.fd == signal_fd) {
                                struct signalfd_siginfo sfsi;
                                ssize_t n;

                                n = read(signal_fd, &sfsi, sizeof(sfsi));
                                if (n != sizeof(sfsi)) {

                                        if (n >= 0) {
                                                log_error("Failed to read from signalfd: invalid block size");
                                                r = -EIO;
                                                goto finish;
                                        }

                                        if (errno != EINTR && errno != EAGAIN) {
                                                log_error("Failed to read from signalfd: %m");
                                                r = -errno;
                                                goto finish;
                                        }
                                } else {

                                        if (sfsi.ssi_signo == SIGWINCH) {
                                                struct winsize ws;

                                                /* The window size changed, let's forward that. */
                                                if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) >= 0)
                                                        ioctl(master, TIOCSWINSZ, &ws);
                                        } else if (sfsi.ssi_signo == SIGTERM && arg_boot && !tried_orderly_shutdown) {

                                                log_info("Trying to halt container. Send SIGTERM again to trigger immediate termination.");

                                                /* This only works for systemd... */
                                                tried_orderly_shutdown = true;
                                                kill(pid, SIGRTMIN+3);

                                        } else {
                                                r = 0;
                                                goto finish;
                                        }
                                }
                        }
                }

                while ((stdin_readable && in_buffer_full <= 0) ||
                       (master_writable && in_buffer_full > 0) ||
                       (master_readable && out_buffer_full <= 0) ||
                       (stdout_writable && out_buffer_full > 0)) {

                        if (stdin_readable && in_buffer_full < LINE_MAX) {

                                k = read(STDIN_FILENO, in_buffer + in_buffer_full, LINE_MAX - in_buffer_full);
                                if (k < 0) {

                                        if (errno == EAGAIN || errno == EPIPE || errno == ECONNRESET || errno == EIO)
                                                stdin_readable = false;
                                        else {
                                                log_error("read(): %m");
                                                r = -errno;
                                                goto finish;
                                        }
                                } else
                                        in_buffer_full += (size_t) k;
                        }

                        if (master_writable && in_buffer_full > 0) {

                                k = write(master, in_buffer, in_buffer_full);
                                if (k < 0) {

                                        if (errno == EAGAIN || errno == EPIPE || errno == ECONNRESET || errno == EIO)
                                                master_writable = false;
                                        else {
                                                log_error("write(): %m");
                                                r = -errno;
                                                goto finish;
                                        }

                                } else {
                                        assert(in_buffer_full >= (size_t) k);
                                        memmove(in_buffer, in_buffer + k, in_buffer_full - k);
                                        in_buffer_full -= k;
                                }
                        }

                        if (master_readable && out_buffer_full < LINE_MAX) {

                                k = read(master, out_buffer + out_buffer_full, LINE_MAX - out_buffer_full);
                                if (k < 0) {

                                        if (errno == EAGAIN || errno == EPIPE || errno == ECONNRESET || errno == EIO)
                                                master_readable = false;
                                        else {
                                                log_error("read(): %m");
                                                r = -errno;
                                                goto finish;
                                        }
                                }  else
                                        out_buffer_full += (size_t) k;
                        }

                        if (stdout_writable && out_buffer_full > 0) {

                                k = write(STDOUT_FILENO, out_buffer, out_buffer_full);
                                if (k < 0) {

                                        if (errno == EAGAIN || errno == EPIPE || errno == ECONNRESET || errno == EIO)
                                                stdout_writable = false;
                                        else {
                                                log_error("write(): %m");
                                                r = -errno;
                                                goto finish;
                                        }

                                } else {
                                        assert(out_buffer_full >= (size_t) k);
                                        memmove(out_buffer, out_buffer + k, out_buffer_full - k);
                                        out_buffer_full -= k;
                                }
                        }
                }
        }

finish:
        if (ep >= 0)
                close_nointr_nofail(ep);

        if (signal_fd >= 0)
                close_nointr_nofail(signal_fd);

        return r;
}

int main(int argc, char *argv[]) {
        pid_t pid = 0;
        int r = EXIT_FAILURE, k;
        char *oldcg = NULL, *newcg = NULL;
        char **controller = NULL;
        int master = -1, n_fd_passed;
        const char *console = NULL;
        struct termios saved_attr, raw_attr;
        sigset_t mask;
        bool saved_attr_valid = false;
        struct winsize ws;
        int kmsg_socket_pair[2] = { -1, -1 };
        FDSet *fds = NULL;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (arg_directory) {
                char *p;

                p = path_make_absolute_cwd(arg_directory);
                free(arg_directory);
                arg_directory = p;
        } else
                arg_directory = get_current_dir_name();

        if (!arg_directory) {
                log_error("Failed to determine path");
                goto finish;
        }

        path_kill_slashes(arg_directory);

        if (geteuid() != 0) {
                log_error("Need to be root.");
                goto finish;
        }

        if (sd_booted() <= 0) {
                log_error("Not running on a systemd system.");
                goto finish;
        }

        if (path_equal(arg_directory, "/")) {
                log_error("Spawning container on root directory not supported.");
                goto finish;
        }

        if (is_os_tree(arg_directory) <= 0) {
                log_error("Directory %s doesn't look like an OS root directory. Refusing.", arg_directory);
                goto finish;
        }

        log_close();
        n_fd_passed = sd_listen_fds(false);
        if (n_fd_passed > 0) {
                k = fdset_new_listen_fds(&fds, false);
                if (k < 0) {
                        log_error("Failed to collect file descriptors: %s", strerror(-k));
                        goto finish;
                }
        }
        fdset_close_others(fds);
        log_open();

        k = cg_get_by_pid(SYSTEMD_CGROUP_CONTROLLER, 0, &oldcg);
        if (k < 0) {
                log_error("Failed to determine current cgroup: %s", strerror(-k));
                goto finish;
        }

        if (asprintf(&newcg, "%s/nspawn-%lu", oldcg, (unsigned long) getpid()) < 0) {
                log_error("Failed to allocate cgroup path.");
                goto finish;
        }

        k = cg_create_and_attach(SYSTEMD_CGROUP_CONTROLLER, newcg, 0);
        if (k < 0)  {
                log_error("Failed to create cgroup: %s", strerror(-k));
                goto finish;
        }

        STRV_FOREACH(controller, arg_controllers) {
                k = cg_create_and_attach(*controller, newcg, 0);
                if (k < 0)
                        log_warning("Failed to create cgroup in controller %s: %s", *controller, strerror(-k));
        }

        master = posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC|O_NDELAY);
        if (master < 0) {
                log_error("Failed to acquire pseudo tty: %m");
                goto finish;
        }

        console = ptsname(master);
        if (!console) {
                log_error("Failed to determine tty name: %m");
                goto finish;
        }

        log_info("Spawning namespace container on %s (console is %s).", arg_directory, console);

        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) >= 0)
                ioctl(master, TIOCSWINSZ, &ws);

        if (unlockpt(master) < 0) {
                log_error("Failed to unlock tty: %m");
                goto finish;
        }

        if (tcgetattr(STDIN_FILENO, &saved_attr) >= 0) {
                saved_attr_valid = true;

                raw_attr = saved_attr;
                cfmakeraw(&raw_attr);
                raw_attr.c_lflag &= ~ECHO;
        }

        if (socketpair(AF_UNIX, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0, kmsg_socket_pair) < 0) {
                log_error("Failed to create kmsg socket pair");
                goto finish;
        }

        assert_se(sigemptyset(&mask) == 0);
        sigset_add_many(&mask, SIGCHLD, SIGWINCH, SIGTERM, SIGINT, -1);
        assert_se(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

        for (;;) {
                siginfo_t status;
                int pipefd[2];

                if(pipe2(pipefd, O_NONBLOCK|O_CLOEXEC) < 0) {
                        log_error("pipe2(): %m");
                        goto finish;
                }

                pid = syscall(__NR_clone, SIGCHLD|CLONE_NEWIPC|CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWUTS|(arg_private_network ? CLONE_NEWNET : 0), NULL);
                if (pid < 0) {
                        if (errno == EINVAL)
                                log_error("clone() failed, do you have namespace support enabled in your kernel? (You need UTS, IPC, PID and NET namespacing built in): %m");
                        else
                                log_error("clone() failed: %m");

                        goto finish;
                }

                if (pid == 0) {
                        /* child */
                        const char *home = NULL;
                        uid_t uid = (uid_t) -1;
                        gid_t gid = (gid_t) -1;
                        unsigned n_env = 0;
                        const char *envp[] = {
                                "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                                "container=systemd-nspawn", /* LXC sets container=lxc, so follow the scheme here */
                                NULL, /* TERM */
                                NULL, /* HOME */
                                NULL, /* USER */
                                NULL, /* LOGNAME */
                                NULL, /* container_uuid */
                                NULL, /* LISTEN_FDS */
                                NULL, /* LISTEN_PID */
                                NULL
                        };

                        envp[2] = strv_find_prefix(environ, "TERM=");
                        n_env = 3;

                        close_nointr_nofail(pipefd[1]);
                        fd_wait_for_event(pipefd[0], POLLHUP, -1);
                        close_nointr_nofail(pipefd[0]);

                        close_nointr_nofail(master);
                        master = -1;

                        if (saved_attr_valid) {
                                if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_attr) < 0) {
                                        log_error("Failed to set terminal attributes: %m");
                                        goto child_fail;
                                }
                        }

                        close_nointr(STDIN_FILENO);
                        close_nointr(STDOUT_FILENO);
                        close_nointr(STDERR_FILENO);

                        close_nointr_nofail(kmsg_socket_pair[0]);
                        kmsg_socket_pair[0] = -1;

                        reset_all_signal_handlers();

                        assert_se(sigemptyset(&mask) == 0);
                        assert_se(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);

                        k = open_terminal(console, O_RDWR);
                        if (k != STDIN_FILENO) {
                                if (k >= 0) {
                                        close_nointr_nofail(k);
                                        k = -EINVAL;
                                }

                                log_error("Failed to open console: %s", strerror(-k));
                                goto child_fail;
                        }

                        if (dup2(STDIN_FILENO, STDOUT_FILENO) != STDOUT_FILENO ||
                            dup2(STDIN_FILENO, STDERR_FILENO) != STDERR_FILENO) {
                                log_error("Failed to duplicate console: %m");
                                goto child_fail;
                        }

                        if (setsid() < 0) {
                                log_error("setsid() failed: %m");
                                goto child_fail;
                        }

                        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
                                log_error("PR_SET_PDEATHSIG failed: %m");
                                goto child_fail;
                        }

                        /* Mark everything as slave, so that we still
                         * receive mounts from the real root, but don't
                         * propagate mounts to the real root. */
                        if (mount(NULL, "/", NULL, MS_SLAVE|MS_REC, NULL) < 0) {
                                log_error("MS_SLAVE|MS_REC failed: %m");
                                goto child_fail;
                        }

                        /* Turn directory into bind mount */
                        if (mount(arg_directory, arg_directory, "bind", MS_BIND|MS_REC, NULL) < 0) {
                                log_error("Failed to make bind mount.");
                                goto child_fail;
                        }

                        if (arg_read_only)
                                if (mount(arg_directory, arg_directory, "bind", MS_BIND|MS_REMOUNT|MS_RDONLY|MS_REC, NULL) < 0) {
                                        log_error("Failed to make read-only.");
                                        goto child_fail;
                                }

                        if (mount_all(arg_directory) < 0)
                                goto child_fail;

                        if (copy_devnodes(arg_directory) < 0)
                                goto child_fail;

                        dev_setup(arg_directory);

                        if (setup_dev_console(arg_directory, console) < 0)
                                goto child_fail;

                        if (setup_kmsg(arg_directory, kmsg_socket_pair[1]) < 0)
                                goto child_fail;

                        close_nointr_nofail(kmsg_socket_pair[1]);
                        kmsg_socket_pair[1] = -1;

                        if (setup_boot_id(arg_directory) < 0)
                                goto child_fail;

                        if (setup_timezone(arg_directory) < 0)
                                goto child_fail;

                        if (setup_resolv_conf(arg_directory) < 0)
                                goto child_fail;

                        if (setup_journal(arg_directory) < 0)
                                goto child_fail;

                        if (chdir(arg_directory) < 0) {
                                log_error("chdir(%s) failed: %m", arg_directory);
                                goto child_fail;
                        }

                        if (mount(arg_directory, "/", NULL, MS_MOVE, NULL) < 0) {
                                log_error("mount(MS_MOVE) failed: %m");
                                goto child_fail;
                        }

                        if (chroot(".") < 0) {
                                log_error("chroot() failed: %m");
                                goto child_fail;
                        }

                        if (chdir("/") < 0) {
                                log_error("chdir() failed: %m");
                                goto child_fail;
                        }

                        umask(0022);

                        loopback_setup();

                        if (drop_capabilities() < 0) {
                                log_error("drop_capabilities() failed: %m");
                                goto child_fail;
                        }

                        if (arg_user) {

                                /* Note that this resolves user names
                                 * inside the container, and hence
                                 * accesses the NSS modules from the
                                 * container and not the host. This is
                                 * a bit weird... */

                                if (get_user_creds((const char**)&arg_user, &uid, &gid, &home, NULL) < 0) {
                                        log_error("get_user_creds() failed: %m");
                                        goto child_fail;
                                }

                                if (mkdir_parents_label(home, 0775) < 0) {
                                        log_error("mkdir_parents_label() failed: %m");
                                        goto child_fail;
                                }

                                if (mkdir_safe_label(home, 0775, uid, gid) < 0) {
                                        log_error("mkdir_safe_label() failed: %m");
                                        goto child_fail;
                                }

                                if (initgroups((const char*)arg_user, gid) < 0) {
                                        log_error("initgroups() failed: %m");
                                        goto child_fail;
                                }

                                if (setresgid(gid, gid, gid) < 0) {
                                        log_error("setregid() failed: %m");
                                        goto child_fail;
                                }

                                if (setresuid(uid, uid, uid) < 0) {
                                        log_error("setreuid() failed: %m");
                                        goto child_fail;
                                }
                        } else {
                                /* Reset everything fully to 0, just in case */

                                if (setgroups(0, NULL) < 0) {
                                        log_error("setgroups() failed: %m");
                                        goto child_fail;
                                }

                                if (setresgid(0, 0, 0) < 0) {
                                        log_error("setregid() failed: %m");
                                        goto child_fail;
                                }

                                if (setresuid(0, 0, 0) < 0) {
                                        log_error("setreuid() failed: %m");
                                        goto child_fail;
                                }
                        }

                        if ((asprintf((char**)(envp + n_env++), "HOME=%s", home ? home: "/root") < 0) ||
                            (asprintf((char**)(envp + n_env++), "USER=%s", arg_user ? arg_user : "root") < 0) ||
                            (asprintf((char**)(envp + n_env++), "LOGNAME=%s", arg_user ? arg_user : "root") < 0)) {
                                log_oom();
                                goto child_fail;
                        }

                        if (arg_uuid) {
                                if (asprintf((char**)(envp + n_env++), "container_uuid=%s", arg_uuid) < 0) {
                                        log_oom();
                                        goto child_fail;
                                }
                        }

                        if (fdset_size(fds) > 0) {
                                k = fdset_cloexec(fds, false);
                                if (k < 0) {
                                        log_error("Failed to unset O_CLOEXEC for file descriptors.");
                                        goto child_fail;
                                }

                                if ((asprintf((char **)(envp + n_env++), "LISTEN_FDS=%u", n_fd_passed) < 0) ||
                                    (asprintf((char **)(envp + n_env++), "LISTEN_PID=%lu", (unsigned long) getpid()) < 0)) {
                                        log_oom();
                                        goto child_fail;
                                }
                        }

                        setup_hostname();

                        if (arg_boot) {
                                char **a;
                                size_t l;

                                /* Automatically search for the init system */

                                l = 1 + argc - optind;
                                a = newa(char*, l + 1);
                                memcpy(a + 1, argv + optind, l * sizeof(char*));

                                a[0] = (char*) "/usr/lib/systemd/systemd";
                                execve(a[0], a, (char**) envp);

                                a[0] = (char*) "/lib/systemd/systemd";
                                execve(a[0], a, (char**) envp);

                                a[0] = (char*) "/sbin/init";
                                execve(a[0], a, (char**) envp);
                        } else if (argc > optind)
                                execvpe(argv[optind], argv + optind, (char**) envp);
                        else {
                                chdir(home ? home : "/root");
                                execle("/bin/bash", "-bash", NULL, (char**) envp);
                        }

                        log_error("execv() failed: %m");

                child_fail:
                        _exit(EXIT_FAILURE);
                }

                if (arg_private_network)
                        setup_private_network(pid);

                log_info("Init process in the container running as PID %d", pid);
                close_nointr_nofail(pipefd[0]);
                close_nointr_nofail(pipefd[1]);

                fdset_free(fds);
                fds = NULL;

                if (process_pty(master, pid, &mask) < 0)
                        goto finish;

                if (saved_attr_valid)
                        tcsetattr(STDIN_FILENO, TCSANOW, &saved_attr);

                r = wait_for_terminate(pid, &status);
                if (r < 0) {
                        r = EXIT_FAILURE;
                        break;
                }

                if (status.si_code == CLD_EXITED) {
                        if (status.si_status != 0) {
                                log_error("Container failed with error code %i.", status.si_status);
                                r = status.si_status;
                                break;
                        }

                        log_debug("Container exited successfully.");
                        break;
                } else if (status.si_code == CLD_KILLED &&
                           status.si_status == SIGINT) {
                        log_info("Container has been shut down.");
                        r = 0;
                        break;
                } else if (status.si_code == CLD_KILLED &&
                           status.si_status == SIGHUP) {
                        log_info("Container is being rebooted.");
                        continue;
                } else if (status.si_code == CLD_KILLED ||
                           status.si_code == CLD_DUMPED) {

                        log_error("Container terminated by signal %s.", signal_to_string(status.si_status));
                        r = EXIT_FAILURE;
                        break;
                } else {
                        log_error("Container failed due to unknown reason.");
                        r = EXIT_FAILURE;
                        break;
                }
        }

finish:
        if (saved_attr_valid)
                tcsetattr(STDIN_FILENO, TCSANOW, &saved_attr);

        if (master >= 0)
                close_nointr_nofail(master);

        close_pipe(kmsg_socket_pair);

        if (oldcg)
                cg_attach(SYSTEMD_CGROUP_CONTROLLER, oldcg, 0);

        if (newcg)
                cg_kill_recursive_and_wait(SYSTEMD_CGROUP_CONTROLLER, newcg, true);

        free(arg_directory);
        strv_free(arg_controllers);
        free(oldcg);
        free(newcg);

        fdset_free(fds);

        return r;
}
