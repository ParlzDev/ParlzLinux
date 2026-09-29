#!/bin/sh
# pm-guest-sim.sh - 宿主侧 guest 模拟器: 验证 pm.c 的 install_cpio_mem
# 走位公式。用与 pm.c 完全相同的走位逻辑(独立 C 单测, 不写盘)解析
# gcc-15.2.pm / clang-llvm-21.1.pm, 真实条目计数与宿主 cpio 解包基准比对。
# 不污染宿主 /usr, 不依赖 unshare。比 QEMU 快几个量级。
#
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/pm-guest-sim.sh
# 前提: /home/jgzyes/pm-repo/{gcc-15.2.pm,clang-llvm-21.1.pm} 已生成
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

US=/home/jgzyes/parlz-userland
REPO=/home/jgzyes/pm-repo
SIM=/home/jgzyes/pm-guest-sim
ROOT=$SIM/root

mkdir -p $SIM
# 1) 与 pm.c 完全一致的走位公式 C 单测(宿主编译, 只读包不写盘)
cat > $SIM/walk_test.c <<'CEOF'
/* 与 userland/pm.c install_cpio_mem 循环完全一致(独立编译验证, 不写盘):
 *
 * newc cpio 头 110 字节: magic "070701"+pad 到 8, 8 字符 ASCII hex 大端
 * 字段。宿主 oracle 穷举 + gcc/clang/mini 三包全成员走位定死:
 *   mode=rdhex(off+14), fsize=rdhex(off+54), namesize=rdhex(off+94)
 * 走位: off+=110; off+=namesize; off=(off+3)&~3; off+=(fsize+3)&~3。
 * 包尾 0 填充区读出的"头"magic 非 070701 → 干净退出; 真实 TRAILER! 头
 * (namesize=11 "TRAILER!!!\0") 判为终止符不计真实条目。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned rdhex(const unsigned char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10u;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10u;
        else d = 0;
        v = v * 16u + d;
    }
    return v;
}
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pkg> <cpio_extracted_real>\n", argv[0]);
        return 2;
    }
    long host_real = atol(argv[2]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "open %s 失败\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "读包失败 len=%ld\n", len);
        return 1;
    }
    fclose(f);
    long off = 0, n_real = 0, n_dir = 0, n_lnk = 0, n_trailer = 0;
    int bad = 0;
    long real_start;
    while (off + 110 <= len) {
        int magic_ok = 1;
        for (int i = 0; i < 6; i++)
            if (b[off + i] != ((const unsigned char *)"070701")[i])
                magic_ok = 0;
        if (!magic_ok)
            break; /* 包尾 0 填充区, 干净退出 */
        unsigned mode     = rdhex(b + off + 14);
        unsigned fsize    = rdhex(b + off + 54);
        unsigned namesize = rdhex(b + off + 94);
        off += 110;
        if (namesize == 0)
            break; /* 包尾填充垃圾头 */
        if (namesize > 512 || off + namesize > len) { bad = 1; break; }
        char nm[512]; unsigned nl = namesize < 511 ? namesize : 511;
        if (nl) memcpy(nm, b + off, nl);
        nm[nl] = 0;
        /* 与 pm.c 一致: 去尾部 NUL 后判 TRAILER(GNU cpio 名区实际
         * 11 字节 "TRAILER!!!\0", 去 0 后 "TRAILER!!" 前缀匹配 TRAILER!) */
        for (unsigned i = 0; i < nl; i++)
            if (nm[i] == 0) { nl = i; break; }
        nm[nl] = 0;
        off += namesize;
        off = (off + 3) & ~3L;
        if (!strncmp(nm, "TRAILER!", 8) && fsize == 0) {
            n_trailer++;
            break;
        }
        n_real++;
        unsigned t = mode & 0xF000;
        if (t == 0x4000) n_dir++;
        else if (t == 0xA000) n_lnk++;
        off += (fsize + 3) & ~3L;
    }
    printf("walk_test: 真实条目=%ld (dir=%ld lnk=%ld), TRAILER=%ld, 坏头=%d\n",
           n_real, n_dir, n_lnk, n_trailer, bad);
    if (n_real != host_real || bad) {
        printf("walk_test: 基准不符(真实 %ld vs cpio 解包 %ld, 坏头=%d)\n",
               n_real, host_real, bad);
        return 1;
    }
    printf("walk_test: 对齐基准通过 (真实条目 %ld == cpio 解包 %ld)%s\n",
           n_real, host_real, n_trailer ? ", TRAILER 正常" : "");
    return 0;
}
CEOF
gcc -O1 -o $SIM/walk_test $SIM/walk_test.c || { echo "walk_test 编译失败"; exit 1; }

TOTAL_OK=1
for PKG in $REPO/gcc-15.2.pm $REPO/clang-llvm-21.1.pm; do
  [ -f "$PKG" ] || { echo "缺 $PKG, 先跑 scripts/build-pm-packages.sh"; exit 1; }
  B=$(basename $PKG .pm)
  # 宿主 cpio 解包到独立目录, 真实条目数作基准(不含 TRAILER)
  rm -rf $ROOT; mkdir -p $ROOT
  cpio -i -D $ROOT < "$PKG" 2>/dev/null
  HOST_REAL=$(find $ROOT -mindepth 1 | wc -l)
  echo ">>> $B: 宿主 cpio 解包真实条目 $HOST_REAL"
  $SIM/walk_test "$PKG" "$HOST_REAL" > $SIM/$B.walk.out 2>&1
  WRC=$?
  echo ">>> walk_test (rc=$WRC):"
  cat $SIM/$B.walk.out
  [ "$WRC" -eq 0 ] || TOTAL_OK=0
  rm -rf $ROOT
done

if [ "$TOTAL_OK" -eq 1 ]; then
  echo "PM_GUEST_SIM_OK (pm.c 走位公式与宿主 cpio 解包真实条目数全部一致)"
else
  echo "PM_GUEST_SIM_FAIL (某包 walk 与 cpio 解包基准不符, 见 .walk.out)"
  exit 1
fi
