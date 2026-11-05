# chromebit-kernel

Cross-compiled `linux-armv7` for the ASUS Chromebit CS10 (`google,veyron-mickey`),
with a **built-in initramfs** that mounts the eMMC root partition.

## Why

The Veyron eMMC has its first 8 MiB permanently write-protected and the factory
GPT primary header there contains `IGNOREME`, so a stock kernel discards the
partition table and `root=PARTUUID=...` cannot resolve. depthcharge does not pass
a FIT ramdisk to the kernel either, so the only way to reach the root filesystem
is an initramfs compiled into the kernel image: `init/init.c` maps the root
partition (LBA 40960, 30744543 sectors) to `/dev/loop0` and `switch_root`s.

## Build

Actions -> `build-kernel` -> Run workflow (kernel version input, default 6.6.100).

Artifacts: `zImage`, `rk3288-veyron-mickey.dtb`, `modules.tar.gz`, `SHA256SUMS`.

## Deploy (on the Chromebit)

1. unpack `modules.tar.gz` into the target root at `/lib/modules/<ver>` and run `depmod <ver>`
2. build a FIT (zImage + dtb), sign it with the ChromeOS devkeys
   (`vbutil_kernel --pack ... --arch arm`), write it to the 16 MiB kernel slot
3. cmdline: `console=tty0 init=/sbin/init root=/dev/loop0 rw video=HDMI-A-1:1280x720@60 drm_kms_helper.poll=0`
