#!/bin/bash

set -x

# Check if tree is clean
DIFF=$(git --no-pager diff)
STATUS=$(git status --porcelain)
if [[ -n "$DIFF" || -n "$STATUS" ]]; then
    # Print in red
    echo -e "\033[0;31mDiff:\033[0m"
    batcat --no-pager <(echo "${DIFF}")
    echo ""
    echo -e "\033[0;31mStatus:\033[0m"
    batcat --no-pager <(echo "${STATUS}")
    echo ""
    echo -e "\033[0;31mTree is dirty, please commit your changes before building\033[0m"
    exit 1
fi

offline_cpus=$(lscpu --offline --parse | grep -Eo '^[0-9]+')
if [[ -n $offline_cpus ]]; then
    while IFS= read -r cpu; do
        echo 1 | sudo tee /sys/devices/system/cpu/cpu"$cpu"/online
    done <<<"$offline_cpus"
fi

set -e
ROOT_PASSWORD=$(cat .root_password)

make LLVM=1 -j"$(nproc)"
echo -e "$ROOT_PASSWORD\n" | sudo -S make modules_install -j32
echo -e "$ROOT_PASSWORD\n" | sudo -S make install

if ! command -v kexec >/dev/null 2>&1; then
    echo -e "\033[0;31mkexec not found; install kexec-tools first\033[0m"
    exit 1
fi

kver=$(make -s kernelrelease)

VMLINUX="/boot/vmlinuz-$kver"
if [[ ! -e "$VMLINUX" ]]; then
    VMLINUX=$(ls -1 /boot/vmlinuz-"$kver"* 2>/dev/null | head -n1 || true)
fi
if [[ -z "$VMLINUX" || ! -e "$VMLINUX" ]]; then
    echo -e "\033[0;31mNo vmlinuz found for ${kver} in /boot\033[0m"
    exit 1
fi

INITRD=""
for candidate in \
    "/boot/initrd.img-$kver" \
    "/boot/initramfs-$kver.img" \
    "/boot/initramfs-$kver" \
    "/boot/initrd-$kver.img"
do
    if [[ -e "$candidate" ]]; then
        INITRD="$candidate"
        break
    fi
done

if [[ -z "$INITRD" ]]; then
    INITRD=$(ls -1 /boot/init*"$kver"* 2>/dev/null | head -n1 || true)
fi

if [[ -z "$INITRD" ]]; then
    echo -e "\033[0;31mNo initrd found for ${kver} in /boot (run update-initramfs or dracut)\033[0m"
    exit 1
fi

CMDLINE=$(sed -e 's/BOOT_IMAGE=[^ ]*//g' -e 's/^ *//' -e 's/ *$//' /proc/cmdline)

echo -e "$ROOT_PASSWORD\n" | sudo -S kexec -l "$VMLINUX" --initrd="$INITRD" --command-line="$CMDLINE"
echo -e "\033[0;33mkexec loaded; rebooting into ${kver}\033[0m"
echo -e "$ROOT_PASSWORD\n" | sudo -S kexec -e
