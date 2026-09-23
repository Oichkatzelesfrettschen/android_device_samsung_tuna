#!/system/bin/sh
# Prepares the dgs partition for the TrustZone secure storage. A mounted
# /tee with its smc directory sets vendor.tee_fs.ready, which starts
# smc_pa_wvdrm and tf_daemon. A partition still blank from the factory is
# formatted here and reported through vendor.tee_fs.formatted; init then
# mounts it, creates /tee/smc and reboots, because tf_daemon hangs when
# started on a filesystem created in the same boot.

DEVICE="/dev/block/platform/omap/omap_hsmmc.0/by-name/dgs"

# SHA-1 of the 4 MiB partition as it leaves the factory, all zero bytes.
EXPECTED_HASH="2bccbd2f38f15c13eb7d5a89fd9d85f595e23bc3"

log_to_kernel() {
    echo "tee-fs-setup: $*" > /dev/kmsg
}

if [ -e /tee/smc ]; then
    log_to_kernel "/tee is initialized for SMC"
    setprop vendor.tee_fs.ready true
    exit 0
fi

actual_hash="$(/system/bin/sha1sum "${DEVICE}")"
if [ "${actual_hash}" != "${EXPECTED_HASH}  ${DEVICE}" ]; then
    log_to_kernel "unexpected hash '${actual_hash}', leaving the partition alone; SMC stays off"
    exit 0
fi

if /system/bin/mke2fs -t ext4 -b 4096 "${DEVICE}" > /dev/kmsg 2>&1; then
    log_to_kernel "formatted dgs for SMC"
    setprop vendor.tee_fs.formatted true
else
    log_to_kernel "formatting dgs failed; SMC stays off"
fi
