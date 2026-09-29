#!/bin/sh
# serial-media.sh - 给 QEMU -nographic 的会话准备一份**串口序**引导介质。
#
# 为什么需要: 交付介质(VMware/真机要用)默认是 vga 序 ——
#   APPEND console=ttyS0,115200 console=tty0 → /dev/console=tty0 → 屏幕能看能敲。
# 内核只把 printk 发给所有 console=, 用户空间(安装进度、登录提示、shell)只走
# /dev/console。所以拿交付 ISO 跑 QEMU -nographic(没有可输入的屏幕)会"装得上、
# 看不见、进不去"。这里另导一份 serial 序的(/dev/console=ttyS0)给自动化用,
# 交付产物 images/parlz-install.iso 不动。
#
# 用法(source, 不要直接执行):
#   . /mnt/f/Linux/Parlz/scripts/serial-media.sh
#   parlz_ensure_serial_media            # 缺了/内容不对才重建
#   parlz_ensure_serial_media force      # 无条件重建(验收脚本用)
#   qemu ... -cdrom "$PARLZ_SERIAL_ISO" ...
#
# 产物: images/parlz-bootfat.e2e.img + images/parlz-install.e2e.iso
#       (e2e = 只给端到端验收用的旁支产物, 不是发布物)
PARLZ_SERIAL_BF=${PARLZ_SERIAL_BF:-/mnt/f/Linux/Parlz/images/parlz-bootfat.e2e.img}
PARLZ_SERIAL_ISO=${PARLZ_SERIAL_ISO:-/mnt/f/Linux/Parlz/images/parlz-install.e2e.iso}

parlz_ensure_serial_media() {
    _force=${1:-}
    # 判据是**内容**不是时间戳, 而且两条都要过:
    #  ① ISO 里是串口序(旧版约定/半截产物会被这条挡下来重建);
    #  ② 引导镜像里的 vmlinuz 与本轮内核产物**逐字节一致** —— 这条是后补的:
    #     只看①会漏掉"串口序但内核是上一轮的"产物(踩过: images/parlz-bootfat.e2e.img
    #     是改过 body 之前生成的, 于是 e2e 明明跑在旧 initramfs 上, 判据却像在验新代码)。
    if [ "$_force" != "force" ] && [ -f "$PARLZ_SERIAL_ISO" ] && [ -f "$PARLZ_SERIAL_BF" ] \
       && grep -aq 'APPEND console=tty0 console=ttyS0,115200 rdinit=/sbin/init' \
             "$PARLZ_SERIAL_ISO" 2>/dev/null \
       && [ "$(mcopy -i "$PARLZ_SERIAL_BF" ::/vmlinuz - 2>/dev/null | md5sum | awk '{print $1}')" \
            = "$(md5sum /mnt/f/Linux/Parlz/images/parlz-bzImage | awk '{print $1}')" ]; then
        return 0
    fi
    echo "serial-media: 导一份串口序介质(/dev/console=ttyS0)..."
    PARLZ_CONSOLE=serial PARLZ_BOOTFAT_OUT="$PARLZ_SERIAL_BF" \
        sh /mnt/f/Linux/Parlz/scripts/gen-fatboot.sh || return 1
    PARLZ_CONSOLE=serial PARLZ_BOOTFAT_IN="$PARLZ_SERIAL_BF" \
        PARLZ_ISO_OUT="$PARLZ_SERIAL_ISO" \
        sh /mnt/f/Linux/Parlz/scripts/build-iso.sh || return 1
    grep -aq 'APPEND console=tty0 console=ttyS0,115200 rdinit=/sbin/init' \
        "$PARLZ_SERIAL_ISO" || {
        echo "serial-media: $PARLZ_SERIAL_ISO 里不是串口序, 别用它跑 -nographic"; return 1; }
    echo "serial-media: OK -> $PARLZ_SERIAL_ISO"
}
