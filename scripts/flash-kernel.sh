#!/bin/sh
# Sign a FIT and write it into the eMMC ChromeOS kernel slot (LBA 20480 = 10 MiB).
#
#   ./flash-kernel.sh <workdir>
#
# <workdir> must contain: zImage, rk3288-veyron-mickey.dtb, fit.its, cfg.txt,
# bl.bin (4 KiB of zeros), kernel.keyblock, kernel_data_key.vbprivk
set -e
W="${1:-/root/k6}"
cd "$W"

[ -f bl.bin ] || dd if=/dev/zero of=bl.bin bs=1024 count=4 2>/dev/null

mkimage -f fit.its fit.img

vbutil_kernel --pack out.kpart \
	--keyblock kernel.keyblock \
	--signprivate kernel_data_key.vbprivk \
	--version 1 \
	--vmlinuz fit.img \
	--config cfg.txt \
	--arch arm \
	--bootloader bl.bin

vbutil_kernel --verify out.kpart --verbose | grep -E "Body verification|Data key sha1sum"

# partition is 10 MiB, the blob must fit
size=$(stat -c %s out.kpart)
if [ "$size" -gt 10485760 ]; then
	echo "kpart is $size bytes, does not fit the 10 MiB slot" >&2
	exit 1
fi

dd if=out.kpart of=/dev/mmcblk0 bs=1M seek=10 conv=fsync,notrunc
sync
echo "flashed $size bytes to /dev/mmcblk0 offset 10 MiB"
