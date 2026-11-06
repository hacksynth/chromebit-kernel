# Arch Linux ARM on ASUS Chromebit CS10 (veyron-mickey)

[![build](https://github.com/hacksynth/chromebit-kernel/actions/workflows/build.yml/badge.svg)](https://github.com/hacksynth/chromebit-kernel/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**[中文说明](README.md)**

Running Arch Linux ARM on an ASUS **Chromebit CS10** (Rockchip RK3288, 2 GB RAM,
16 GB eMMC, dev-mode firmware) — installed to the **internal eMMC**, no SD card,
no USB stick, no keyboard at boot.

> TL;DR: the first 8 MiB of this eMMC is **permanently write-protected** and the
> factory primary GPT header reads `IGNOREME`, so the Linux kernel discards the
> partition table and `root=PARTUUID=` can never work. On top of that,
> depthcharge **does not pass a FIT ramdisk** to the kernel. The only path that
> works is a kernel with a **built-in initramfs** whose `/init` maps the root
> partition to `/dev/loop0` with an explicit offset and `switch_root`s into it.

---

## Result

| | |
|---|---|
| OS | Arch Linux ARM (armv7h) |
| Storage | internal eMMC `/dev/mmcblk0` partition 2 |
| Kernel | `6.6.100-veyron`, cross-compiled, `CONFIG_INITRAMFS_SOURCE` embedded init |
| Root | `/dev/loop0` (loop offset `20971520` bytes) |
| Display | 1920×1080@60 (CEA timing), clean, no artifacts |
| Boot | power on → system, nothing else attached |

![test pattern](docs/images/04-test-pattern.jpg)

---

## Why the obvious approaches fail

### 1. Permanent write protection on the first 8 MiB

```console
# mmc writeprotect user get /dev/mmcblk0
Write Protect Group size in blocks/bytes: 16384/8388608
Write Protect Groups 0-0 (Blocks 0-16383), Permanent Write Protection
```

JEDEC *permanent* write protection cannot be removed by the host:
`mmc writeprotect user set none/temp/pwron` has no effect, and `mmc sanitize`
does not remove it either (it does erase the *backup GPT*, see pitfall 2).
`B_PERM_WP_DIS` must be set before the protection is applied — impossible afterwards.

### 2. Primary GPT header is `IGNOREME`

The factory writes `IGNOREME` as the primary GPT signature (to keep the internal
boot area immutable). Linux only accepts `EFI PART` and drops the whole table:

```console
# there is /dev/mmcblk0 but never /dev/mmcblk0p1 / p2
# cgpt show /dev/mmcblk0
WARNING: Primary GPT header is being ignored     # firmware falls back to the backup
```

So `root=PARTUUID=...` can never resolve, and the GPT cannot be rewritten
because LBA 0–16383 is read-only.

### 3. depthcharge does not hand FIT ramdisks to the kernel

Putting an initramfs into the FIT (`images/ramdisk@1`) looks promising, but the
kernel's `/init` never ran. Verified with a "postcard": the init's very first
action writes a marker to LBA 20000; after a boot attempt the sector was still
all zero.

> The postcard trick is worth repeating on any headless board: no serial console,
> no reliable display, and still a definitive answer about how far boot got.

### 4. The only way left

```
depthcharge firmware
   └─ loads the kernel slot at eMMC LBA 20480 (ChromeOS format, devkey signed)
        └─ Linux 6.6.100 with a built-in initramfs
             └─ /init: losetup -o 20971520 /dev/loop0 /dev/mmcblk0
                  └─ mount /dev/loop0 /newroot && switch_root
                       └─ systemd
```

---

## Partition layout

Rebuilt with `scripts/mkgpt.py` (the firmware reads the **backup** GPT):

| part | start LBA | size | type | notes |
|---|---|---|---|---|
| p1 | 20480 | 20480 sectors (10 MiB) | ChromeOS kernel | `priority=15 tries=5 successful=1` |
| p2 | 40960 | 30744543 sectors (≈14.7 GiB) | Linux data | root filesystem |

`30744543 × 512` is **not** a multiple of 4096 — format with a 4K-aligned
sizelimit or ext4 ends up larger than the partition (see pitfall 4).

---

## Build the kernel

```console
gh workflow run build.yml -R <owner>/chromebit-kernel -f kver=6.6.100
gh run watch -R <owner>/chromebit-kernel
gh run download <run-id> -R <owner>/chromebit-kernel -D out
```

Artifacts: `zImage`, `rk3288-veyron-mickey.dtb`, `modules.tar.gz`, `SHA256SUMS`
(~30 min on a hosted runner). No secrets required; fork and dispatch.

Key options on top of `multi_v7_defconfig`:

```
CONFIG_INITRAMFS_SOURCE="<repo>/irfs"   # embedded init
CONFIG_LOCALVERSION="-veyron"
CONFIG_BLK_DEV_INITRD=y
CONFIG_BLK_DEV_LOOP=y
CONFIG_EXT4_FS=y                        # must be y: the init cannot load modules
CONFIG_KERNEL_XZ=y                      # 11.1 MB -> 7.0 MB, fits the 10 MiB slot
CONFIG_CC_OPTIMIZE_FOR_SIZE=y
CONFIG_SECURITY_LANDLOCK=y              # pacman 7 sandbox
CONFIG_USER_NS=y                        # pacman 7 alpm user switch
```

---

## Flash the kernel slot

ChromeOS kernel slots are vboot-format and must be signed with the **public
ChromeOS devkeys** (accepted when `dev_boot_signed_only=0`):

- `tests/devkeys/kernel.keyblock`
- `tests/devkeys/kernel_data_key.vbprivk`

(from `chromium.googlesource.com/chromiumos/platform/vboot_reference`, append
`?format=TEXT` for base64 if raw is blocked.)

```console
mkimage -f scripts/mkfit.its fit.img        # kernel + fdt only, no ramdisk

vbutil_kernel --pack out.kpart \
  --keyblock kernel.keyblock --signprivate kernel_data_key.vbprivk \
  --version 1 --vmlinuz fit.img --config cfg.txt \
  --arch arm --bootloader bl.bin            # bl.bin = 4 KiB zeroes

vbutil_kernel --verify out.kpart --verbose | grep "Body verification"

dd if=out.kpart of=/dev/mmcblk0 bs=1M seek=10 conv=fsync,notrunc
```

`cfg.txt`:

```
console=tty0 init=/sbin/init root=/dev/loop0 rw \
video=HDMI-A-1:1920x1080@60 drm_kms_helper.poll=0
```

`scripts/flash-kernel.sh` wraps the whole thing.

---

## Write the root filesystem

```console
LOOP=$(losetup -f --show -o $((40960*512)) --sizelimit $((30744536*512)) /dev/mmcblk0)
mkfs.ext4 -F -b 4096 -L ArchRoot -m 1 $LOOP
mount $LOOP /mnt

# copy an existing system (easiest: a bootable Arch SD card)
tar -cf - --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run \
    --exclude=./tmp --exclude=./media --exclude=./mnt --one-file-system . | tar -xf - -C /mnt

# modules -> usr/lib/modules, never into /lib (see pitfall 3)
tar xzf modules.tar.gz -C /tmp
cp -a /tmp/lib/modules/6.6.100-veyron /mnt/usr/lib/modules/
depmod -b /mnt 6.6.100-veyron
```

Do **not** put a root entry in `/mnt/etc/fstab` — the initramfs mounts root.

---

## Pitfalls

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | no `/dev/mmcblk0p*` at all | primary GPT header `IGNOREME` + 8 MiB permanent WP | embedded initramfs + loop offset |
| 2 | firmware boots to "OS missing" recovery | `mmc sanitize` erased the backup GPT | rebuild it with `scripts/mkgpt.py` |
| 3 | `init: exec /sbin/init` fails (marker `14-exec-fail`) | `tar -C /mnt` turned the `/lib → usr/lib` symlink into a real directory, dynamic linker gone | `rm -rf /mnt/lib && ln -s usr/lib /mnt/lib`, modules into `usr/lib/modules` |
| 4 | `bad geometry: block count … exceeds size of device` | partition size not a multiple of 4 KiB, mkfs rounds up | format with `--sizelimit $((30744536*512))` |
| 5 | `/init` never runs | depthcharge drops FIT ramdisks | `CONFIG_INITRAMFS_SOURCE` |
| 6 | black screen, monitor has sync | `video=…1280x720@60` cannot be driven by this driver/monitor combo | verify with `modetest -s <conn>:#6` (1080p60 **CEA**), then use `1920x1080@60` |
| 7 | colored horizontal lines / sync loss | 7.1.6 HDMI output on this board | use 6.6.100 + 1080p60 CEA |
| 8 | `pacman: Landlock is not supported` | kernel lacks Landlock/UserNS | enable `CONFIG_SECURITY_LANDLOCK`, `CONFIG_USER_NS` (or `--disable-sandbox`) |
| 9 | kernel slot silently overwritten | `linux-armv7-chromebook` post_install dd's `/boot/vmlinux.kpart` | `IgnorePkg = linux-armv7 linux-armv7-chromebook` |
| 10 | `modetest -w` refuses every property | broken `-w` in that build | use `-s` mode switching or write a tiny libdrm tool |

Diagnosing the display: dump `/dev/fb0` and convert it to PNG. If the
framebuffer is clean while the screen is not, the problem is transmission
(TMDS/PHY), not rendering.

---

## Recovery

If the eMMC install stops booting:

1. insert a bootable USB/SD card,
2. power-cycle and press **Ctrl+U** at the dev-mode screen,
3. boot the rescue system and repeat the flash/rootfs steps.

Read the postcard marker:

```console
dd if=/dev/mmcblk0 bs=512 skip=20000 count=1 | tr -d '\0'   # e.g. DSHSTAGE 11-mount-fail
```

---

## Repository layout

```
.github/workflows/build.yml   cross-compile (Actions)
init/init.c                   embedded init: loop offset mount + switch_root + markers
scripts/mkgpt.py              rebuild the eMMC backup GPT
scripts/mkfit.its             FIT description template
scripts/flash-kernel.sh       sign + write the kernel slot
docs/images/                  screenshots from the process
```

---

## Verified on

- ASUS Chromebit CS10 (`google,veyron-mickey`), ChromeOS 86 dev-mode firmware,
  `dev_boot_usb=1`, `dev_boot_signed_only=0`
- Linux 6.6.100 (LTS), `multi_v7_defconfig` + options above
- Arch Linux ARM armv7h userland
