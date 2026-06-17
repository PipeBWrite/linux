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
sudo make install && sudo reboot
