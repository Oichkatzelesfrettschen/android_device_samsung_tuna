#!/system/bin/sh
# Formats the dgs partition for the TrustZone secure storage the first time
# it is found empty, then reboots: tf_daemon hangs when started on a
# filesystem created in the same boot. An initialized /tee sets
# init.tee_fs.ready, which starts smc_pa_wvdrm and tf_daemon.

DEVICE="/dev/block/platform/omap/omap_hsmmc.0/by-name/dgs"

# SHA-1 of the 4 MiB partition as it leaves the factory, all zero bytes.
EXPECTED_HASH="2bccbd2f38f15c13eb7d5a89fd9d85f595e23bc3"

log_to_kernel() {
    echo "tee-fs-setup: $*" > /dev/kmsg
}

create_tee_fs() {
    /system/bin/mke2fs -t ext4 -b 4096 "${DEVICE}" || return 1
    mount -t ext4 "${DEVICE}" /tee || return 1
    mkdir /tee/smc || return 1
    chmod 0770 /tee/smc || return 1
    chown drmrpc:drmrpc /tee/smc || return 1
    restorecon -R /tee/smc || return 1
}

if [ -e /tee/smc ]; then
    log_to_kernel "/tee is already initialized for SMC"
    setprop init.tee_fs.ready true
    exit 0
fi

actual_hash="$(/system/bin/sha1sum "${DEVICE}")"
if [ "${actual_hash}" != "${EXPECTED_HASH}  ${DEVICE}" ]; then
    log_to_kernel "unexpected hash '${actual_hash}', leaving the partition alone; SMC stays off"
    exit 0
fi

if create_tee_fs > /dev/kmsg 2>&1; then
    log_to_kernel "initialized /tee for SMC, rebooting"
    mount -t ext4 -o remount,ro /tee
    reboot
else
    log_to_kernel "initialization of /tee for SMC failed; SMC stays off"
fi
