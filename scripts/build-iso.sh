#!/bin/sh
# build-iso.sh - 构建可引导的 Parlz 安装 ISO(ISO9660 + El Torito + 内嵌内核)。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/build-iso.sh
#
# 引导方式:
#   SeaBIOS -boot d 读 ISO 的 El Torito 引导记录 → isolinux 引导扇区
#   (/usr/lib/ISOLINUX/isolinux.bin)→ 在 ISO 里找 /isolinux.cfg →
#   执行 linux /vmlinuz + APPEND console 参数。vmlinuz 内嵌 rootfs
#   (CONFIG_INITRAMFS_SOURCE),自带 init/shell/install。
#   init 检测 /install.d 且无 install-done 标记 => 自动跑安装器。
#
# 依赖: xorriso、isolinux 包(/usr/lib/ISOLINUX/isolinux.bin)
# 产物: /mnt/f/Linux/Parlz/images/parlz-install.iso
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

K=/home/jgzyes/parlz-kernel
US=/home/jgzyes/parlz-userland
IMG=/mnt/f/Linux/Parlz/images
ISO_SRC=/tmp/parlz-iso
ISOLINUX_BIN=/usr/lib/ISOLINUX/isolinux.bin
# 控制台序 + 输出路径都可被验收脚本改写(见 scripts/console-cfg.sh 与
# iso-disk-e2e.sh): 默认 vga 序、覆盖到 images/ 下的正式产物。
. /mnt/f/Linux/Parlz/scripts/console-cfg.sh
ISO_OUT=${PARLZ_ISO_OUT:-$IMG/parlz-install.iso}
BOOTFAT_IN=${PARLZ_BOOTFAT_IN:-$IMG/parlz-bootfat.img}

# --- 校验内核与 isolinux.bin 都在 ---
[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage,先 build-kernel"; exit 1; }
[ -f "$ISOLINUX_BIN" ] || {
    echo "缺 $ISOLINUX_BIN,先: apt-get install isolinux"; exit 1; }

echo ">>> [1] 准备 ISO 数据区(rootfs 平铺 + vmlinuz + isolinux.cfg)"
rm -rf "$ISO_SRC"; mkdir -p "$ISO_SRC"
# rootfs 解包到 ISO 数据区**外面**: 解包目录若在 $ISO_SRC 里, 整份 rootfs
# 会被打进 ISO 两遍(实测 ISO 190 MB → 95 MB)。
rm -rf "$ISO_SRC.unpack"; mkdir -p "$ISO_SRC.unpack"
(cd "$ISO_SRC.unpack" && zcat "$US/initramfs.cpio.gz" | cpio -idm 2>/dev/null)
cp -a "$ISO_SRC.unpack/." "$ISO_SRC/" 2>/dev/null || true
rm -rf "$ISO_SRC.unpack"
# vmlinuz(内嵌 rootfs 的 bzImage)放 ISO 根;isolinux.cfg 引用它
cp -f "$IMG/parlz-bzImage" "$ISO_SRC/vmlinuz"
cp -f "$IMG/parlz-bzImage" "$ISO_SRC/boot/vmlinuz" 2>/dev/null || true
# isolinux.cfg(El Torito 引导器读的配置)
# rdinit=/sbin/init: PID 1 用 busybox-init(与磁盘自启路径一致) —— 它读
# /etc/inittab 的 sysinit 跑 parlz-boot.sh → body(挂 CD/装盘/登录/shell)。
# root=/dev/vda2: 若目标盘已装好(ext2 根在位), body 会挂上并 pivot 到
# 已安装的根; 空盘时该设备不存在, 挂载失败无害, 照常走安装流程。
# 控制台参数来自 scripts/console-cfg.sh(与 gen-fatboot.sh 同一份, 不许各说各话):
#   默认 vga 序 = console=ttyS0,115200 console=tty0 → /dev/console=tty0,
#   VMware/真机屏幕**既能看也能敲**(tty0 在最后是关键, 内核只认最后一个
#   console= 当系统控制台)。串口那一路照样收 printk, 宿主仍能抓内核日志。
#   PARLZ_CONSOLE=serial → 串口当 /dev/console, 只给 QEMU -nographic 验收用。
#   PARLZ_VGA=0 → 无 tty0 的版(发布号后缀 -UN\V)。
CONSOLE_ARGS="$PARLZ_CONSOLE_ARGS"
# 整条 APPEND 只有**一份**。BIOS 那条写进 isolinux.cfg,UEFI 那条写进 ESP 里的
# syslinux.cfg(gen-efi-esp.sh 收 PARLZ_APPEND),下面的判据也用它 ——
# 两条引导路不许各说各话(控制台序一旦分叉, 就是"UEFI 起来但没人能输")。
BOOT_APPEND="$CONSOLE_ARGS rdinit=/sbin/init root=/dev/vda2 rootdelay=2"
case "$PARLZ_CONSOLE_MODE" in
    vga)          echo "    控制台: 串口 printk + /dev/console=tty0(VMware/真机可键盘输入)" ;;
    serial)       echo "    控制台: /dev/console=ttyS0(串口序, QEMU -nographic 自动化专用)" ;;
    serial-only)  echo "    控制台: 仅串口(PARLZ_VGA=0, 无 VGA 版)" ;;
esac
# PROMPT 1 + TIMEOUT 50: 让引导器**自报家门**。以前是 PROMPT 0/TIMEOUT 1(完全
# 静默), 结果 VMware 里"跳到 0x7C00 之后一屏死寂"根本分不清是 isolinux 没起来
# 还是内核没接上控制台。现在能看到这两行 SAY 就说明 El Torito + isolinux.bin +
# ldlinux.c32 这段是通的, 看不到就是引导镜像那段的问题(定位维度直接劈成两半)。
# 控制台序来自 console-cfg.sh; 下面 SAY 两行**必须 ASCII** —— isolinux 用 INT10
# 逐字符打到 VGA 文本模式, 中文在这条链上只会是乱码(内核起来之后才有 UTF-8)。
cat > "$ISO_SRC/isolinux.cfg" <<CFG
PROMPT 1
TIMEOUT 50
DEFAULT parlz
SAY ">> Parlz: ISOLINUX took over (this line = El Torito + isolinux.bin OK)"
SAY ">> Parlz: loading /vmlinuz in 5s (Enter boots now, console= order decides who gets keyboard)"
LABEL parlz
LINUX /vmlinuz
APPEND $BOOT_APPEND
CFG
grep -n APPEND "$ISO_SRC/isolinux.cfg" | sed 's/^/    /'
# 引导分区镜像放进 ISO: 安装时 install 从安装介质取它(整盘 pwrite 到
# 目标盘 LBA 2048)。body 会把 /dev/sr0 挂到 /cdrom, install 按
# /cdrom/boot/fat16.img 找到。注意镜像含 vmlinuz, 不能进 initramfs(自引用
# 膨胀), 但 ISO 里放一份没问题。
mkdir -p "$ISO_SRC/boot"
# 嵌进 ISO 的引导分区镜像(install 装盘时从这里取, 决定**装出来的盘**用什么
# 控制台序、跑哪一版内核)。没显式给 PARLZ_BOOTFAT_IN 时(即出交付介质)要过两条:
#   ① APPEND 与本次控制台序一致 —— 这份文件是共享产物, 验收脚本会用串口序覆盖
#      它(PARLZ_BOOTFAT_OUT), 谁最后写的就是谁的序; 放过一次"交付 ISO 里嵌着
#      串口序镜像", 用户从 ISO 装完盘、屏幕一片黑(输出全在串口)。
#   ② 里面的 vmlinuz 就是本轮内核 —— 只看①会漏掉"序对但内核是上一轮的"
#      (踩过: e2e 明明跑在旧 initramfs 上, 判据却像在验新代码)。
# 判据一律用**内容**, 不用时间戳。
if [ -z "${PARLZ_BOOTFAT_IN:-}" ] && [ -f "$BOOTFAT_IN" ]; then
    _bf_md=$(mcopy -i "$BOOTFAT_IN" ::/vmlinuz - 2>/dev/null | md5sum | awk '{print $1}')
    if ! grep -aqF "APPEND $BOOT_APPEND" "$BOOTFAT_IN" \
       || [ "$_bf_md" != "$(md5sum "$IMG/parlz-bzImage" | awk '{print $1}')" ]; then
        echo "    引导分区镜像过期(序或内核不对), 按 $PARLZ_CONSOLE_MODE 重生成..."
        PARLZ_BOOTFAT_OUT="$BOOTFAT_IN" sh /mnt/f/Linux/Parlz/scripts/gen-fatboot.sh \
            || { echo "gen-fatboot 重生成失败"; exit 1; }
    fi
fi
if [ -f "$BOOTFAT_IN" ]; then
  cp -f "$BOOTFAT_IN" "$ISO_SRC/boot/fat16.img"
  echo "    引导镜像 -> ISO /boot/fat16.img ($(wc -c < "$BOOTFAT_IN") 字节, 取自 $BOOTFAT_IN)"
else
  echo "    警告: 缺 $BOOTFAT_IN(先跑 build-kernel.sh), ISO 将无法装到磁盘"
fi
# isolinux.bin 也要拷进数据区(-b 指 ISO 内的相对路径 /isolinux.bin,
# SeaBIOS 读它作 El Torito 引导扇区)
cp -f "$ISOLINUX_BIN" "$ISO_SRC/isolinux.bin"
# isolinux 运行时模块(必须与引导扇区同级,ldlinux.c32 是核心模块,
# 缺失就 "press a key to retry")
SYS_MOD=/usr/lib/syslinux/modules/bios
for m in ldlinux.c32 libcom32.c32 libutil.c32; do
  [ -f "$SYS_MOD/$m" ] && cp -f "$SYS_MOD/$m" "$ISO_SRC/$m" || \
    echo "警告: $SYS_MOD/$m 缺失(isolinux 引导会失败)"
done

echo ">>> [2] 生成可引导 ISO(xorriso El Torito: -b isolinux.bin)"
# El Torito 可引导 ISO(xorriso -as mkisofs 的 -b 方式):
#   -b <bootimg>          El Torito 引导镜像(指 ISO 内的 /isolinux.bin,
#                        已拷进 ISO_SRC 数据区,故用相对路径 /isolinux.bin)
#   -c <catalog>          El Torito 引导目录名
#   -no-emul-boot         非软驱模拟
#   -boot-load-size 4     **isolinux 官方配方就是 4 扇区**(Debian/通用安装盘都一样):
#                        第一阶段只占一个扇区, 剩下的 isolinux 自己用 INT13 读。
#                        不写这项 xorriso 会按整档记账(实测 Ldsiz=76) —— SeaBIOS
#                        宽容照跑, VMware 的 BIOS 严格, 现场就是 "Booting from
#                        0000:7c00" 之后一屏死寂。所以这里写 4, 并在下面验它。
#   SeaBIOS -boot d 读 ISO 的 El Torito 记录 → isolinux → /isolinux.cfg
#   → linux /vmlinuz。isolinux.cfg 放 ISO 根,isolinux 默认找它。
# xorriso 要**新建**输出文件, 可目标正被 VMware 当 CD 挂着时既删不掉也重命名不了
# (9p 上 unlink/rename 报 Permission denied, 但原地写是允许的)。所以一律先写到
# 临时名, 内容验完再"发布"到目标: mv → 原地覆盖 → 都不行才另存 busy-<时分秒>。
ISO_TARGET="$ISO_OUT"
TMP_ISO="${ISO_TARGET}.tmpbuild"
# --- 关于 UEFI(2026-09-28 试过,结论记在这里,别再走一遍)---------------
# 这张盘是 **BIOS 专用**(El Torito + isolinux)。固件设成 UEFI 时它起不来。
# 试过给 xorriso 加第二条 EFI 引导项(`-e /boot/esp.img`,ESP 镜像由
# scripts/gen-efi-esp.sh 生成,里面是 syslinux.efi + ldlinux.e64 + 内核):
#   - `-as mkisofs` 下 `-b` 与 `-e` **互相顶替**,最后只剩一条引导项(实测
#     report_el_torito 只列 UEFI,BIOS 那条消失)—— 也就是"加了 UEFI 就把
#     VMware 那条能用的路弄断了",绝对不接受;
#   - `-boot_image any partition_table=on efi_boot_part=…` 走的是 isohybrid
#     的 GPT 追加分区路线,xorriso 1.5.6 能写出来,但 El Torito 仍只有一条。
# 正解是 **grub-mkrescue**(见 scripts/build-uefi-iso.sh):它天生出
# BIOS+UEFI 双引导。交付 ISO 暂不替换,等那条路把装盘 e2e 全跑通再说。
rm -f "$TMP_ISO"
xorriso -as mkisofs \
  -o "$TMP_ISO" \
  -b /isolinux.bin \
  -c isolinux.catalog \
  -no-emul-boot \
  -boot-load-size 4 \
  -r -V PARLZ \
  "$ISO_SRC" 2>&1 | tail -8
[ -f "$TMP_ISO" ] || { echo "xorriso 生成 ISO 失败"; exit 1; }
ISO_OUT="$TMP_ISO"   # 下面第 [3] 步的判据统一读这个变量

echo ">>> [3] 验证 ISO(El Torito 记录 + APPEND 控制台参数)"
ls -la "$ISO_OUT"
FILEINFO=$(file "$ISO_OUT"); echo "  $FILEINFO"
echo "=== ISO 内容(根目录) ==="
# 注意: xorriso 没有 -oslist 这个命令(那是老 isoinfo 的写法), 用它只会得到
# 空清单 → 后面所有断言假失败。列内容用 -indev + -find/-ls。
xorriso -indev "$ISO_OUT" -ls / 2>/dev/null | head -15 || true

# 三条硬判据, 任一不满足就退出非 0: 别把"看起来生成了"的 ISO 发出去。
FAIL=""
case "$FILEINFO" in
    *bootable*) echo "  OK: file 报 (bootable)";;
    *) FAIL="$FAIL 无bootable标记";;
esac
# El Torito 引导记录单独查: 它是 ISO **系统区**里的结构, 不是文件系统条目,
# 文件清单里永远列不到 isolinux.catalog —— 拿文件清单断言它必然假失败。
ECTORITO=$(xorriso -indev "$ISO_OUT" -report_el_torito plain 2>/dev/null || true)
printf '%s\n' "$ECTORITO" | grep -aq 'El Torito boot img' \
    || FAIL="$FAIL 无 El Torito 引导镜像记录"
printf '%s\n' "$ECTORITO" | grep -a '^El Torito' | sed 's/^/  /'
# 加载扇区数必须是 4(VMware BIOS 对这一项严格, 见上面 -boot-load-size 的注释)
LDSIZ=$(printf '%s\n' "$ECTORITO" | awk '/El Torito boot img/{print $(NF-1)}')
if [ "$LDSIZ" = "4" ]; then
    echo "  OK: El Torito load secs = 4(isolinux 规范值)"
else
    FAIL="$FAIL El Torito load secs=$LDSIZ(应为 4, -boot-load-size 没生效?)"
fi
# 引导镜像本体要真等于 isolinux.bin(至少头 2048 字节一致), 不然只是文件名对了
IMG_LBA=$(printf '%s\n' "$ECTORITO" | awk '/El Torito boot img/{print $NF}')
if [ -n "$IMG_LBA" ] && [ "$(dd if="$ISO_OUT" bs=2048 skip="$IMG_LBA" count=1 2>/dev/null | md5sum)" \
     = "$(dd if="$ISOLINUX_BIN" bs=2048 count=1 2>/dev/null | md5sum)" ]; then
    echo "  OK: 引导镜像 LBA $IMG_LBA 的头 2048 字节 == isolinux.bin"
else
    FAIL="$FAIL 引导镜像内容与 isolinux.bin 不符"
fi
# 数据区文件全清单(-find 递归; 名称用单引号包住, 子串匹配不受影响)
LIST=$(xorriso -indev "$ISO_OUT" -find / 2>/dev/null || true)
# 数据区里实际需要的文件: 引导器 + 运行时模块 + 内核 + 配置
for need in isolinux.bin ldlinux.c32 vmlinuz isolinux.cfg; do
    printf '%s\n' "$LIST" | grep -aqi "$need" || FAIL="$FAIL 缺$need"
done
# 引导镜像在不在(装到磁盘要用它), 缺失只警告不失败: 允许纯 shell 会话产 ISO
printf '%s\n' "$LIST" | grep -aqi 'fat16.img' || echo "  警告: ISO 内无 boot/fat16.img(装盘会缺引导镜像)"

# isolinux.cfg 是明文躺在数据区里的, 直接 grep ISO 就能确认控制台参数落地
# (顺序最容易在这里静默失效 —— tty0 排前还是排后决定了谁能输入)。
# 判据用 console-cfg.sh 算出的**整行**, 与写进去的那行同源, 不会各说各话。
APPEND_EXPECT="APPEND $CONSOLE_ARGS rdinit=/sbin/init root=/dev/vda2 rootdelay=2"
if grep -aqF "$APPEND_EXPECT" "$ISO_OUT"; then
    echo "  OK: $APPEND_EXPECT"
    echo "      (/dev/console = 最后一个 console= → 模式 $PARLZ_CONSOLE_MODE)"
else
    echo "  ISO 里的 APPEND 行:"
    grep -a 'APPEND' "$ISO_OUT" | head -3 | sed 's/^/      /'
    FAIL="$FAIL APPEND 与开关不一致(期望: $APPEND_EXPECT)"
fi
[ -z "$FAIL" ] || { echo "build-iso: FAIL ->$FAIL"; exit 1; }

# ---- 内容合格, 发布到正式名 ----
DEGRADED=0
if mv -f "$TMP_ISO" "$ISO_TARGET" 2>/dev/null; then
    ISO_OUT="$ISO_TARGET"
elif cat "$TMP_ISO" > "$ISO_TARGET" 2>/dev/null \
     && [ "$(wc -c < "$TMP_ISO")" = "$(wc -c < "$ISO_TARGET")" ]; then
    # 目标删不掉但能原地写(VMware 正挂着它的典型情形): 正式名照样更新。
    echo "  注: $ISO_TARGET 无法改名/删除, 已用**原地覆盖**写入新内容"
    echo "      (VMware 里正在读的那次会话不受影响, 下次冷启动读到的就是新版)"
    ISO_OUT="$ISO_TARGET"; rm -f "$TMP_ISO"
else
    ALT="${ISO_TARGET%.iso}.busy-$(date +%H%M%S).iso"
    mv -f "$TMP_ISO" "$ALT" 2>/dev/null || true
    ISO_OUT="$ALT"; DEGRADED=1
    echo "!!! 写不进 $ISO_TARGET(既删不掉也不能原地覆盖) —— 新内容在 $ALT"
    echo "!!! 若上面报大小不符, $ISO_TARGET 可能已被写坏: VM 设置里把 CD/DVD 的"
    echo "!!! Connected 取消(或关掉 VMware)后重跑本脚本即可恢复"
fi

# 内容合格但没写成正式目标 → 仍然失败, 别让调用方以为交付文件已更新。
if [ "$DEGRADED" = 1 ]; then
    echo "build-iso: $ISO_TARGET 没被更新(仍是旧文件), 新内容在 $ISO_OUT"
    exit 1
fi
echo "=== build-iso done ==="
echo "  ISO: $ISO_OUT (El Torito + isolinux, /dev/console=$PARLZ_CONSOLE_MODE)"
echo "  引导: SeaBIOS -boot d 读 El Torito → isolinux → /vmlinuz → shell → install"
echo "  运行: sh /mnt/f/Linux/Parlz/scripts/run-iso.sh"
