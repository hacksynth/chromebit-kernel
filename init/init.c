/*
 * Minimal initramfs init for ASUS Chromebit CS10 (rk3288 veyron-mickey).
 *
 * Why this exists:
 *   The Veyron eMMC ships with a permanently write-protected first 8 MiB, and
 *   ChromeOS' factory GPT header there reads "IGNOREME". A stock Linux kernel
 *   therefore refuses the partition table, so root=PARTUUID cannot resolve.
 *   depthcharge does not hand a FIT ramdisk to the kernel either, so the
 *   initramfs is compiled into the kernel image and does the work here:
 *   bind the eMMC user area at the root partition offset to /dev/loop0 and
 *   switch_root into it.
 *
 * Root layout on the eMMC (fixed by the frozen factory GPT):
 *   partition 1 : ChromeOS kernel slot, 20480 sectors @ LBA 20480
 *   partition 2 : ext4 root,              LBA 40960 .. 30785502
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <linux/loop.h>

#define ROOT_OFFSET  (40960ULL * 512ULL)
#define ROOT_SIZE    (30744543ULL * 512ULL)
#define MARK_OFFSET  (20000LL * 512LL)

static void msg(const char *s) { ssize_t r = write(1, s, strlen(s)); (void)r; }

/* Leave a breadcrumb in a scratch sector so a failed boot can be diagnosed
 * from another system afterwards. */
static void mark(const char *what) {
    int fd = open("/dev/mmcblk0", O_RDWR);
    if (fd < 0) return;
    char buf[512];
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "DSHSTAGE %s", what);
    ssize_t r = pwrite(fd, buf, sizeof(buf), MARK_OFFSET);
    (void)r;
    fsync(fd);
    close(fd);
}

int main(void) {
    int c, i, ffd, lfd;

    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755); mkdir("/newroot", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, "");
    c = open("/dev/console", O_RDWR);
    if (c >= 0) { dup2(c, 0); dup2(c, 1); dup2(c, 2); }
    mount("proc", "/proc", "proc", 0, "");
    mount("sysfs", "/sys", "sysfs", 0, "");

    mark("01-start");
    msg("init: wait mmcblk0\n");
    for (i = 0; i < 100; i++) {
        if (access("/dev/mmcblk0", F_OK) == 0) break;
        usleep(100000);
    }
    if (access("/dev/mmcblk0", F_OK) != 0) { mark("02-no-mmc"); for (;;) pause(); }
    mark("03-mmc-ok");
    if (access("/dev/loop0", F_OK) != 0) { mark("04-no-loop"); for (;;) pause(); }

    ffd = open("/dev/mmcblk0", O_RDWR);
    if (ffd < 0) { mark("05-open-mmc-fail"); for (;;) pause(); }
    lfd = open("/dev/loop0", O_RDWR);
    if (lfd < 0) { mark("06-open-loop-fail"); for (;;) pause(); }
    if (ioctl(lfd, LOOP_SET_FD, ffd) < 0) { mark("07-setfd-fail"); for (;;) pause(); }
    mark("08-setfd-ok");

    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    info.lo_offset = ROOT_OFFSET;
    info.lo_sizelimit = ROOT_SIZE;
    if (ioctl(lfd, LOOP_SET_STATUS64, &info) < 0) { mark("09-status-fail"); for (;;) pause(); }
    mark("10-status-ok");

    if (mount("/dev/loop0", "/newroot", "ext4", 0, "") < 0) { mark("11-mount-fail"); for (;;) pause(); }
    mark("12-mount-ok");

    mkdir("/newroot/dev", 0755); mkdir("/newroot/proc", 0755); mkdir("/newroot/sys", 0755);
    mount("/dev", "/newroot/dev", 0, MS_MOVE, 0);
    mount("/proc", "/newroot/proc", 0, MS_MOVE, 0);
    mount("/sys", "/newroot/sys", 0, MS_MOVE, 0);
    chdir("/newroot");
    mount(".", "/", 0, MS_MOVE, 0);
    chroot(".");
    chdir("/");

    mark("13-before-exec");
    msg("init: exec /sbin/init\n");
    execl("/sbin/init", "/sbin/init", (char *)0);
    mark("14-exec-fail");
    for (;;) pause();
}
