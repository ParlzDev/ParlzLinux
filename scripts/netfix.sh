#!/bin/bash
# netfix.sh - 给 WSL 内核副本 .config 启用网络子系统(virtio-net + UDP/TCP + DNS)
CFG=/home/jgzyes/parlz-kernel
C=$CFG/.config

add() {
    local key="$1" val="$2"
    if grep -qE "^# ${key}=" "$C"; then
        sed -i "s/^# ${key}= not set/${key}=${val}/" "$C"
    elif ! grep -qE "^${key}=" "$C"; then
        echo "${key}=${val}" >> "$C"
    fi
}

add CONFIG_NET y
add CONFIG_INET y
add CONFIG_VIRTIO_NET y
add CONFIG_UDP y
add CONFIG_TCP y
# NETFILTER 保持关闭(GCC 15 编译陷阱,见 AGENTS.md)
add CONFIG_NETFILTER n
add CONFIG_XTABLES n

make -C "$CFG" olddefconfig > /tmp/netfix-olddef.log 2>&1
echo "olddef rc=$?"
grep -E "^CONFIG_(NET|INET|VIRTIO_NET|UDP|TCP|NETFILTER|XTABLES)=" "$C"
