#!/bin/sh
# 直接复算: 读 root/boot/vmlinuz 真实大小, 按当前几何算数据循环 off 序列
# 验证是否有 off+512 > TOTAL, 以及最后写位置
V=/home/jgzyes/parlz-userland/root/boot/vmlinuz
python3 - <<PY
import os
VML = "$V"
sz = os.path.getsize(VML)
TOTAL_S = 81920
TOTAL = TOTAL_S * 512
SPC = 2
RESERVED, NUM_FAT, ROOTDIR, VBR_FS16 = 1028, 2, 8, 192
CLU = SPC * 512
DATA0 = RESERVED + NUM_FAT * VBR_FS16 + ROOTDIR
NCLUS = (TOTAL - DATA0 * 512) // CLU      # 当前生成器公式(NCLUS 修正后)
data_end_cluster = NCLUS - 1
print("vmlinuz size=%d  ncl=%d" % (sz, (sz + CLU - 1) // CLU))
# 模拟文件分配(ldlinux/libcom32/libutil/vmlinuz/syslinux.cfg/EFI/EFI_BOOT + EFI 模块)
# 简化: 只看 vmlinuz 的簇号
first_vml = 2 + 3  # ldlinux+libcom32+libutil 各 1 簇
ncl_vml = max(1, (sz + CLU - 1) // CLU)
off0 = (DATA0 + ((first_vml - 2) % NCLUS) * SPC) * 512
last = off0 + (ncl_vml - 1) * SPC * 512
print("vmlinuz first=%d off0=%d last_off=%d (TOTAL=%d, 越界=%s)" %
      (first_vml, off0, last, TOTAL, last + SPC * 512 > TOTAL))
PY
