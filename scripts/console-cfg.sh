#!/bin/sh
# console-cfg.sh - 引导命令行里 console= 的**唯一**来源。
# 被 scripts/build-iso.sh(isolinux.cfg) 与 scripts/gen-fatboot.sh(syslinux.cfg)
# 用 `. file` 引入, 设出 PARLZ_CONSOLE_ARGS / PARLZ_CONSOLE_MODE。
#
# 内核规则: cmdline 里**最后**一个 console= 才成为 /dev/console(系统控制台),
# 前面的 console= 只收 printk。我们的启动 body 把 0/1/2 重指 /dev/console,
# 所以"人在哪块屏上敲键盘"完全由这个顺序决定, 两个方向不能同时要:
#
#   PARLZ_CONSOLE=vga(默认, 交付/VMware/真机)
#       console=ttyS0,115200 console=tty0   → /dev/console = tty0
#       屏幕既看得到也**能输入**。串口那一路仍然收 printk(宿主抓内核日志)。
#   PARLZ_CONSOLE=serial(QEMU -nographic 自动化)
#       console=tty0 console=ttyS0,115200   → /dev/console = ttyS0
#       宿主从 stdio 喂输入、收用户空间输出; -nographic 下没有可输入的
#       屏幕, 所以跑 ISO/磁盘自启的验收脚本必须用这一序(见 iso-disk-e2e.sh)。
#   PARLZ_VGA=0(无 VGA 版, 发布号后缀 -UN\V)
#       console=ttyS0,115200                → 只有串口, 顺序无意义。
#
# 交付物一律 vga 序; serial 序只是自动化用的旁支产物, 别拿它当发布 ISO。
case "${PARLZ_VGA:-1}" in
    0)  PARLZ_CONSOLE_ARGS="console=ttyS0,115200"
        PARLZ_CONSOLE_MODE="serial-only" ;;
    *)  case "${PARLZ_CONSOLE:-vga}" in
            serial) PARLZ_CONSOLE_ARGS="console=tty0 console=ttyS0,115200"
                    PARLZ_CONSOLE_MODE="serial" ;;
            vga)    PARLZ_CONSOLE_ARGS="console=ttyS0,115200 console=tty0"
                    PARLZ_CONSOLE_MODE="vga" ;;
            *)  echo "console-cfg: PARLZ_CONSOLE 只认 vga|serial, 收到 '$PARLZ_CONSOLE'" >&2
                exit 1 ;;
        esac ;;
esac
export PARLZ_CONSOLE_ARGS PARLZ_CONSOLE_MODE
