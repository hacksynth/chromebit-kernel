# Arch Linux ARM on ASUS Chromebit CS10 (veyron-mickey)

在一台 **ASUS Chromebit CS10**（RK3288 / 2GB / 16GB eMMC / 开发者模式的 Stick 电脑）上，
把 Arch Linux ARM 装进**内置 eMMC**并正常启动的完整记录。

> 结论先行：这台机器的 eMMC 前 8MB 是**永久写保护**、GPT 主头写着 `IGNOREME`，
> 而且 depthcharge **不会把 FIT 里的 ramdisk 传给内核**。
> 所以常规做法（`root=PARTUUID`、initramfs、整盘镜像盖 GPT）**全部走不通**，
> 唯一可行的是：**把 initramfs 编进内核**，由它在启动时用 loop 偏移挂载 eMMC 分区。

*English summary at the bottom.*

---

## 0. 最终成果

| 项 | 值 |
|---|---|
| 系统 | Arch Linux ARM（armv7h） |
| 位置 | 内置 eMMC `/dev/mmcblk0` 第 2 分区 |
| 内核 | `6.6.100-veyron`，交叉编译，`CONFIG_INITRAMFS_SOURCE` 内建 init |
| 根分区 | `/dev/loop0`（loop 偏移 `20971520` 字节） |
| 显示 | 1920×1080@60（CEA），干净无彩线 |
| 启动 | 无 SD、无 U 盘、无键盘，上电即进系统 |

---

## 1. 硬件与固件前提

- 设备处于**开发者模式**（`crossystem devsw_boot=1`）
- 已允许外部启动：`crossystem dev_boot_usb=1 dev_boot_signed_only=0`
  （Chromebit 没有键盘，需要自供电 USB Hub + 有线键盘；固件对 Hub 后接键盘是支持的）
- Chromebit 没有 SD 卡槽，只有**一个 USB 2.0 口**；调试期建议准备一个 U 盘系统

---

## 2. 为什么不能"直接装"

### 2.1 前 8MB 永久写保护

```console
# mmc writeprotect user get /dev/mmcblk0
Write Protect Group size in blocks/bytes: 16384/8388608
Write Protect Groups 0-0 (Blocks 0-16383), Permanent Write Protection
Write Protect Groups 1-1878 (Blocks 16384-30785535), No Write Protection
```

JEDEC 的永久写保护（`Permanent WP`）**主机无法清除**：

- `mmc writeprotect user set none/temp/pwron` → 无效
- `mmc sanitize /dev/mmcblk0` → 无效（且会擦除可写区的备份 GPT，见坑 #2）
- 唯一的 `B_PERM_WP_DIS` 必须在**设置前**预置，事后不可逆

### 2.2 主 GPT 头是 `IGNOREME`

ChromeOS 工厂把主 GPT 头签名写成 `IGNOREME`（只保护内部存储的引导区不被改动），
Linux 内核只认 `EFI PART`，会**直接丢弃整张分区表**：

```console
# 有 /dev/mmcblk0，但从来没有 /dev/mmcblk0p1、p2
# cgpt show /dev/mmcblk0
WARNING: Primary GPT header is being ignored      ← 固件用备份头
```

于是 `root=PARTUUID=...` 永远解析不到设备。

### 2.3 depthcharge 不传 FIT 的 ramdisk

把 initramfs 放进 FIT（`images/ramdisk@1` + `configurations.ramdisk`）看似可行，
但实测**内核里的 `/init` 一行都没执行**——用"明信片"机制（init 第一件事往
`LBA 20000` 写标记）验证，标记扇区全 0。

> 顺带一提：这个"明信片"手法在没有串口、屏幕又不稳定的机器上非常好用，
> 强烈建议所有裸机实验都加一个。

### 2.4 于是只剩一条路

```
固件 depthcharge
   └─ 读 eMMC LBA 20480 的内核分区（ChromeOS 格式，devkey 签名）
        └─ Linux 6.6.100 + 内建 initramfs
             └─ /init：losetup -o 20971520 /dev/loop0 /dev/mmcblk0
                  └─ mount /dev/loop0 /newroot && switch_root
                       └─ systemd
```

---

## 3. 分区布局

eMMC 的 GPT 我们用脚本重建（见 `scripts/mkgpt.py`），布局与原厂一致：

| 分区 | 起始 LBA | 大小 | 类型 | 说明 |
|---|---|---|---|---|
| p1 | 20480 | 20480 扇区 (10 MiB) | ChromeOS kernel | 内核槽，`priority=15 tries=5 successful=1` |
| p2 | 40960 | 30744543 扇区 (≈14.7 GiB) | Linux data | 根文件系统 |

> **分区大小 30744543 扇区不是 4096 字节的整数倍**（余 0.375 块），
> 格式化时必须用 4K 对齐的 `--sizelimit`，否则文件系统会比设备大 → 见坑 #4。

---

## 4. 编译内核

仓库自带 GitHub Actions 工作流（`.github/workflows/build.yml`）：

```console
gh workflow run build.yml -R <owner>/chromebit-kernel -f kver=6.6.100
gh run watch -R <owner>/chromebit-kernel
gh run download <run-id> -R <owner>/chromebit-kernel -D out
```

产物：`zImage`、`rk3288-veyron-mickey.dtb`、`modules.tar.gz`、`SHA256SUMS`（约 30 分钟）。

关键配置（`multi_v7_defconfig` 基础上）：

```
CONFIG_INITRAMFS_SOURCE="<repo>/irfs"   # 内建 init
CONFIG_LOCALVERSION="-veyron"
CONFIG_BLK_DEV_INITRD=y
CONFIG_BLK_DEV_LOOP=y
CONFIG_EXT4_FS=y                        # 必须是 y，否则 init 挂载时模块还没加载
CONFIG_KERNEL_XZ=y                      # zImage 从 11.1MB 压到 7.0MB，塞得进 10MiB 分区
CONFIG_CC_OPTIMIZE_FOR_SIZE=y
CONFIG_SECURITY_LANDLOCK=y              # pacman 7 需要
CONFIG_USER_NS=y                        # pacman 7 的 alpm 用户切换需要
```

---

## 5. 写入内核分区

ChromeOS 的内核分区是 vboot 格式，必须用 **devkey** 签名（`dev_boot_signed_only=0`
时固件接受自签名 devkey 内核）。密钥是公开的：

```console
# chromium.googlesource.com/.../tests/devkeys/kernel.keyblock
# chromium.googlesource.com/.../tests/devkeys/kernel_data_key.vbprivk
# 若被墙，可用 ?format=TEXT 取 base64 再 base64 -d
```

打包流程：

```console
# 1) FIT（只含 kernel + fdt，不要 ramdisk）
mkimage -f fit.its fit.img

# 2) 签名
vbutil_kernel --pack out.kpart \
  --keyblock kernel.keyblock \
  --signprivate kernel_data_key.vbprivk \
  --version 1 --vmlinuz fit.img --config cfg.txt \
  --arch arm --bootloader bl.bin          # bl.bin = 4KB 零，ARM 下不使用

# 3) 校验（必须看到 Body verification succeeded）
vbutil_kernel --verify out.kpart --verbose | grep -E "Body verification|Data key sha1sum"

# 4) 写入（p1 起始 = 10 MiB，注意是裸偏移，没有 /dev/mmcblk0p1）
dd if=out.kpart of=/dev/mmcblk0 bs=1M seek=10 conv=fsync,notrunc
```

`cfg.txt`：

```
console=tty0 init=/sbin/init root=/dev/loop0 rw \
video=HDMI-A-1:1920x1080@60 drm_kms_helper.poll=0
```

`fit.its` 要点：`kernel@1` 用 `kernel_noload` / `compression = "none"`（zImage 自解压），
`configurations` 里 `compatible = "google,veyron-mickey"`（固件按它选 DTB）。

---

## 6. 写入根文件系统

```console
# 4K 对齐的 sizelimit！否则 bad geometry
LOOP=$(losetup -f --show -o $((40960*512)) --sizelimit $((30744536*512)) /dev/mmcblk0)
mkfs.ext4 -F -b 4096 -L ArchRoot -m 1 $LOOP
mount $LOOP /mnt

# 灌系统（可以直接从现成的 Arch SD 卡整盘拷）
tar -cf - --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run \
    --exclude=./tmp --exclude=./media --exclude=./mnt --one-file-system . | tar -xf - -C /mnt

# 新内核的模块（⚠️ 解压到 usr/lib/modules，绝对不要解到 /lib！见坑 #3）
mkdir -p /mnt/usr/lib/modules
tar xzf modules.tar.gz -C /tmp && cp -a /tmp/lib/modules/6.6.100-veyron /mnt/usr/lib/modules/
depmod -b /mnt 6.6.100-veyron
```

`/mnt/etc/fstab` 里**不要**写 root 条目（root 由 initramfs 挂载，写 PARTUUID 反而会报错）：

```
# root 由内建 initramfs 通过 /dev/loop0 挂载
```

---

## 7. 踩坑记录

### 坑 #1：`IGNOREME` GPT → 内核看不到分区

- **现象**：`lsblk` 只有 `mmcblk0` 没有任何子分区；`partx -s` 却能列出分区
- **原因**：主 GPT 头签名是 `IGNOREME`（工厂保护），内核只认 `EFI PART`
- **走不通的解法**：改签名（前 8MB 永久写保护，写不进去）
- **解法**：不依赖分区表 —— 内建 initramfs + loop 偏移挂载

### 坑 #2：`mmc sanitize` 擦掉了备份 GPT

- **现象**：执行 sanitize 后固件直接进恢复模式（"Chrome 操作系统不存在或已损坏"）
- **原因**：`sanitize` 在闪存层擦除整个用户区，把磁盘末尾的**备份 GPT** 也清了；
  而固件正是靠备份头（主头是 `IGNOREME`）来定位分区的
- **解法**：用脚本从零重建备份 GPT（`scripts/mkgpt.py`），写 `LBA last-32..last-1`
  的分区项数组 + `LBA last` 的备份头，属性写 `priority=15 tries=5 successful=1`
- **教训**：别在 Chromebook 的内部存储上跑 sanitize

### 坑 #3：解压模块把 `/lib` 符号链接覆盖成目录

- **现象**：启动卡在 `init: exec /sbin/init` 失败（阶段标记 `14-exec-fail`）
- **原因**：Arch 的 `/lib` 是 `usr/lib` 的符号链接；`tar xzf ... -C /mnt` 把 `./lib/`
  解成了真实目录，于是动态链接器 `/lib/ld-linux-armhf.so.3` 找不到 → exec ENOENT
- **解法**：`rm -rf /mnt/lib && ln -s usr/lib /mnt/lib`，模块放 `usr/lib/modules/`
- **验证**：`chroot /mnt /usr/lib/systemd/systemd --version` 能打印版本才算正常

### 坑 #4：ext4 4K 对齐 → `bad geometry`

- **现象**：`EXT4-fs (loop0): bad geometry: block count 3843072 exceeds size of device (3843067 blocks)`
- **原因**：分区 30744543 扇区 × 512 = 15741206016 字节，不是 4096 的整数倍，
  `mkfs` 向上取整，文件系统比设备大 5 个块
- **解法**：格式化时 `--sizelimit $((30744536*512))`（8 扇区对齐），
  这样 fs ≤ 分区，init 用完整分区大小挂载也没问题

### 坑 #5：depthcharge 不传 FIT 里的 ramdisk

- **现象**：内核启动后日志停在早期，`/init` 完全没执行
- **验证**：init 第一件事往 `LBA 20000` 写 `DSHSTAGE xx`，重启后读该扇区 → 全 0
- **解法**：`CONFIG_INITRAMFS_SOURCE` 把 initramfs 编进内核主体

### 坑 #6：`video=` 指定的模式在 6.6 上黑屏

- **现象**：系统完全正常（帧缓冲里有正确的登录提示），显示器有同步但画面全黑
- **原因**：`video=HDMI-A-1:1280x720@60` 选的时序在这版驱动/显示器上出不了图
- **解法**：`modetest -M rockchip -s <conn>:#6`（1920×1080@60 **CEA**，注意不是 #5 的
  DMT 时序，`#3` 的 1080p@120 更不行）验证通过后，把 `video=` 改成 `1920x1080@60`

### 坑 #7：7.1.6 内核 HDMI 彩线

- **现象**：720p/1024×768/1080p 都有横向彩色细线闪烁，1080p 还会丢掉同步黑屏
- **定位**：把 `/dev/fb0` 存下来转 PNG，发现**帧缓冲内容完全干净** → 是传输层（TMDS）问题
- **结论**：换 **6.6.100** 内核 + 1080p60 CEA 时序后画面纯净

### 坑 #8：`pacman` 报 Landlock / alpm 沙箱错误

- **现象**：`error: restricting filesystem access failed because Landlock is not supported`
- **解法**：内核补 `CONFIG_SECURITY_LANDLOCK=y`、`CONFIG_USER_NS=y`
  （临时可用 `pacman --disable-sandbox`）

### 坑 #9：`pacman -Syu` 覆盖内核分区

- **现象**：升级 `linux-armv7-chromebook` 时，包内 post_install 会
  `dd if=/boot/vmlinux.kpart of=/dev/mmcblk0 seek=0`，把内核分区刷成不带 `video=` 的版本
- **解法**：`/etc/pacman.conf` 加 `IgnorePkg = linux-armv7 linux-armv7-chromebook`，
  换内核一律走上文第 5 节的手工流程

### 坑 #10：`modetest -w` 设不了连接器属性

- **现象**：`modetest -w 45:46:8`（改 max bpc）报 `CONNECTOR 45 has no 46 property`
- **解法**：这个构建的 `-w` 有 bug，改用 libdrm 自己写工具，或直接用 `-s` 换模式验证

---

## 8. 救援

如果 eMMC 起不来（进恢复模式 / 黑屏无网络）：

1. 插上 U 盘或 SD 卡（需提前做好可启动的系统）
2. 断电 → 上电 → 在开发者模式画面按 `Ctrl+U`
3. 进入救援系统后重做第 5、6 节

读启动阶段的"明信片"：

```console
dd if=/dev/mmcblk0 bs=512 skip=20000 count=1 | tr -d '\0'    # 例：DSHSTAGE 11-mount-fail
```

---

## 9. 仓库结构

```
.github/workflows/build.yml   交叉编译（Actions）
init/init.c                   内建 init：loop 偏移挂载 + switch_root + 阶段标记
scripts/mkgpt.py              重建 eMMC 备份 GPT
scripts/mkfit.its             FIT 描述模板
README.md                     本文
```

---

## English summary

The ASUS Chromebit CS10 (veyron-mickey) has two hard limits: the first 8 MiB of its
eMMC is **permanently write-protected** (JEDEC permanent WP, uncleareable) and the
factory primary GPT header reads `IGNOREME`, which the Linux kernel discards — so
`root=PARTUUID` can never resolve. Google's depthcharge also does **not** hand a FIT
ramdisk to the kernel (verified with a marker sector: `/init` never ran).

The only working path is a **kernel with a built-in initramfs** whose `/init` maps the
root partition to `/dev/loop0` with an explicit offset and `switch_root`s into it.
Everything is signed with the public ChromeOS devkeys and written to the ChromeOS
kernel slot at LBA 20480.

Details, pitfalls and recovery steps are documented above in Chinese.
