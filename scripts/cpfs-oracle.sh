#!/bin/sh
# cpfs-oracle.sh - 宿主机上验证 cpfs 的 rootfs 保真拷贝(不起 QEMU, 秒级)。
#
# 做法: 把 images/parlz-initramfs 解包当"源根", 对一个新 mkfs 的 ext2 镜像
# 以该根为 cwd 跑 cpfs(与 install 的调用方式一致: chdir 到根 + 整盘偏移写),
# 然后挂载结果, 与源根逐项比对。ISO/磁盘 e2e 之前先过这道关。
#
# 覆盖的真实缺陷:
#   - 符号链接被丢(busybox applet 全是指向 /sbin/busybox 的软链, 384 个)
#   - 递归进挂载点 /cdrom 把整张安装 ISO 当 rootfs 拷(中途失败, 目标只剩半个根)
#   - 快链(目标 <60 字节, 存 inode 内联区)与慢链(占一个数据块)两条路径
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/cpfs-oracle.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

WL=/home/jgzyes/parlz-userland
SRC=/mnt/f/Linux/Parlz/images/parlz-initramfs
W=/home/jgzyes/cpfs-oracle
ROOT=$W/root
MNT=$W/mnt
SIZE_MB=${SIZE_MB:-460}

# cpfs 的分区大小由 tot_sec 反推(root_sec = tot_sec - 83969), 必须与
# mkfs 按文件尺寸算出的块数一致, 否则位图/inode 表几何对不上。
BLOCKS=$((SIZE_MB * 1024 * 1024 / 4096))
TOT=$((BLOCKS * 8 + 83969))

echo "=== [1/5] 同步并构建 cpfs/mkfs ==="
for f in cpfs.c mkfs.c; do
    cp /mnt/f/Linux/Parlz/userland/$f $WL/src/$f
done
cd $WL && cmake --build build --target cpfs --target mkfs -j"$(nproc)" 2>&1 \
    | grep -E "error|Built target" || true
[ -x $WL/build/bin/cpfs ] || { echo "FAIL: cpfs 没编出来"; exit 1; }

echo "=== [2/5] 解包 initramfs 当源根($SIZE_MB MiB 目标分区) ==="
rm -rf $W && mkdir -p $ROOT $MNT
gzip -dc "$SRC" | (cd $ROOT && cpio -idm --quiet 2>/dev/null)
# 挂载点残留:解包出来的 /proc /sys /dev 等本应空, 清一遍确保干净
for d in proc sys dev tmp run mnt cdrom; do
    rm -rf $ROOT/$d && mkdir -p $ROOT/$d
done
# 真挂一个 tmpfs 到**不在名字表里**的目录并塞入文件: 只有靠 st_dev
# 判挂载点的实现才不会把它拷进目标(cpfs 之后 umount, 两边都是空目录)
mkdir -p $ROOT/oracle-mnt
mount -t tmpfs tmpfs $ROOT/oracle-mnt
mkdir -p $ROOT/oracle-mnt/subdir_probe
echo "不该被拷进目标盘" > $ROOT/oracle-mnt/subdir_probe/hide.txt
LONG=$(printf 'a%.0s' $(seq 1 80))
ln -sf "$LONG" $ROOT/oracle-slow-link
ln -sf /bin/busybox $ROOT/oracle-fast-link
N_SRC_LINK=$(find $ROOT -type l | wc -l)
N_SRC_ALL=$(find $ROOT -mindepth 1 | wc -l)
echo "源根: $N_SRC_ALL 条目, $N_SRC_LINK 软链, $(find $ROOT -type f | wc -l) 普通文件"

echo "=== [3/5] mkfs + cpfs ==="
rm -f $W/root2.img
truncate -s "${SIZE_MB}M" $W/root2.img
$WL/build/bin/mkfs $W/root2.img $SIZE_MB >/dev/null || { echo "FAIL: mkfs"; exit 1; }
( cd $ROOT && $WL/build/bin/cpfs $W/root2.img 0 $TOT ) || { echo "FAIL: cpfs 退出非 0"; exit 1; }
umount $ROOT/oracle-mnt

echo "=== [4/5] e2fsck ==="
E2=$(e2fsck -fn $W/root2.img 2>&1); echo "$E2" | tail -8
FAIL=""
echo "$E2" | grep -qiE "wrong|deleted|inconsistent|duplicate|illegal|untraced|inode bitmap|blocks count|Magic" \
    && FAIL="$FAIL e2fsck有告警"

echo "=== [5/5] 挂载复核 ==="
mount -o loop $W/root2.img $MNT || { echo "FAIL: 宿主挂载失败"; exit 1; }
# 路径集合逐条比对(源根里挂载点都是空目录, 应与目标 1:1)
find $ROOT -mindepth 1 ! -name install.d | sed "s|^$ROOT/||" | sort > $W/src.paths
( cd $MNT && find . -mindepth 1 ! -path './lost+found*' ) | sed 's|^\./||' | sort > $W/dst.paths
if ! diff -q $W/src.paths $W/dst.paths >/dev/null; then
    echo "FAIL: 条目集合不一致(前 10 处差异)"
    diff $W/src.paths $W/dst.paths | head -10
    FAIL="$FAIL 条目集合"
else
    echo "OK: $(wc -l < $W/dst.paths) 个条目路径完全一致"
fi
# 软链: 数量 + 逐条目标
find $MNT -type l -printf '%p %l\n' | sed "s|$MNT/||" | sort > $W/dst.links
( cd $ROOT && find . -type l -printf '%p %l\n' ) | sed 's|^\./||' | sort > $W/src.links
if ! diff -q $W/src.links $W/dst.links >/dev/null; then
    echo "FAIL: 软链不一致(源 $(wc -l < $W/src.links) / 目标 $(wc -l < $W/dst.links), 前 5 处)"
    diff $W/src.links $W/dst.links | head -10
    FAIL="$FAIL 软链"
else
    echo "OK: $N_SRC_LINK 条软链目标逐条一致(含快链/慢链)"
fi
# 可执行位 + 真通过软链读到的内容
for p in bin/parlz-sh bin/install bin/cpfs bin/login sbin/busybox; do
    [ -x "$MNT/$p" ] || { echo "FAIL: $p 缺失或不可执行"; FAIL="$FAIL $p"; }
done
if cmp -s $ROOT/bin/parlz-sh $MNT/bin/parlz-sh; then
    echo "OK: /bin/parlz-sh 内容一致"
else
    echo "FAIL: /bin/parlz-sh 内容不一致"; FAIL="$FAIL parlz-sh内容"
fi
[ "$(readlink $MNT/bin/busybox)" = "../sbin/busybox" ] \
    && echo "OK: 快链 /bin/busybox 可解析" \
    || { echo "FAIL: /bin/busybox 软链解析异常"; FAIL="$FAIL busybox链"; }
cmp -s $ROOT/sbin/busybox $MNT/bin/busybox \
    && echo "OK: 经软链读到 /sbin/busybox 内容一致" \
    || { echo "FAIL: 经软链读 busybox 不一致"; FAIL="$FAIL 软链内容"; }
[ "$(readlink $MNT/oracle-slow-link)" = "$LONG" ] \
    && echo "OK: 慢链(80 字节目标)完整" \
    || { echo "FAIL: 慢链目标不等于 80 字节 a"; FAIL="$FAIL 慢链"; }
[ -d $MNT/dev ] && [ -z "$(ls -A $MNT/dev)" ] \
    && echo "OK: /dev 作为空目录存在(挂载点未递归)" \
    || { echo "FAIL: /dev 不是空目录"; FAIL="$FAIL dev"; }
# st_dev 判挂载点: 运行期挂在 oracle-mnt 的 tmpfs 内容不该进目标盘
[ -d $MNT/oracle-mnt ] && [ -z "$(ls -A $MNT/oracle-mnt)" ] \
    && echo "OK: 真挂载点 oracle-mnt 只建空目录(按 st_dev 识别, 未递归)" \
    || { echo "FAIL: oracle-mnt 被递归拷贝或没建目录"; FAIL="$FAIL 挂载点递归"; }
[ ! -e $MNT/install.d ] && echo "OK: 目标根无 install.d(不会自触发重装)" \
    || { echo "FAIL: 目标根还有 install.d"; FAIL="$FAIL install.d"; }

sync; umount $MNT
echo ""
if [ -z "$FAIL" ]; then
    echo "cpfs-oracle: PASS"
    exit 0
fi
echo "cpfs-oracle: FAIL ->$FAIL"
exit 1
