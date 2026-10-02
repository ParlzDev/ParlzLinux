# AGENTS.md

本文件面向在此仓库工作的 AI 编码代理。人类用户请看 [README.md](README.md)。

## 项目性质

**Parlz 是基于 Linux 7.2.5 源码就地改造的操作系统内核**（`linux-7.2.5/` 就是改造后的源码树，
不是只读参考）。改造方式是直接编辑 Linux 源码：加自有的标识层、启用所需子系统、
在构建系统中注入修复。用户空间、构建脚本、安装介质在仓库其余目录。

用户明确要求过：**直接改 Linux 代码，不要另建一套参考实现**。

## 环境（重要，先读这里）

主机是 Windows，实际编译在 **WSL** 内完成。

> **2026-09-25 起构建实例为 `Ubuntu-24.04`**（原 `Ubuntu-26.04` 虚盘损坏
> `WSL_E_DISK_CORRUPTED` → 实例注销）。所有数据在 `F:\` 盘，`Ubuntu-24.04`
> 侧重建了 `/home/jgzyes/{parlz-kernel,parlz-userland}`。全链构建脚本
> `scripts/build-2404.sh`。旧 26.04 的已配置内核 `.config` 备份在
> `linux-7.2.5/.config-2404`。

| 用途 | 位置 |
|------|------|
| 工作区（本仓库） | `F:\Linux\Parlz`，WSL 内为 `/mnt/f/Linux/Parlz` |
| 内核编译副本 | WSL 内 `/home/jgzyes/parlz-kernel`（**必须**用 WSL 本地盘，9p 跨盘编译极慢） |
| 用户空间构建 | WSL 内 `/home/jgzyes/parlz-userland` |
| qemu / xorriso / gcc | WSL 24.04 `/usr/bin`（`qemu-system-x86_64` 8.2.2、gcc 13.3.0） |
| MinGW64 | `F:\Path\MinGW64\bin`（备用，当前主流程未用） |
| clang/LLVM | `F:\Path\clang+llvm-23.1.1-x86_64-pc-windows-msvc\bin`（备用） |

### 调用 WSL 的正确姿势

从 Git Bash 调用时，`wsl -e` 会把参数交给 Windows 侧解析，导致 `/mnt/...` 被拼错。
**长命令一律写成 `.sh` 文件放进仓库，再用 `wsl -d Ubuntu-24.04 -e sh /mnt/f/Linux/Parlz/scripts/xxx.sh` 执行**，
或者用 `wsl -d Ubuntu-24.04 -e bash -c '...'` 且命令内**不要**出现未转义的 `$()`、`(`、`'`。

装包需要 root，用：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "DEBIAN_FRONTEND=noninteractive apt-get install -y <pkg>"
```

> **验收脚本正在跑的时候不要编辑它。** dash/sh 是**按字节偏移增量读取**脚本的，
> 中途改内容会让后面的行从错误的偏移开始执行 —— 实测报出
> `uefi-e2e.sh: 82: uefi-e2e: FAIL ->: not found` 这种"源码里根本没有这句"的错，
> 结论整轮作废（看起来像产品 FAIL，其实是脚本被自己改坏了）。
> 要改判据就先停掉后台任务，改完再重跑。

### 已安装依赖（24.04）

`gcc make bison flex cpio bc libelf-dev libssl-dev xorriso qemu-system-x86 isolinux syslinux-utils cmake`

构建引导/安装链还额外需要（缺了会以 WARNING 跳过对应产物，磁盘就装不出来）：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "DEBIAN_FRONTEND=noninteractive apt-get install -y \
  syslinux dosfstools mtools bzip2 libncurses-dev dpkg-dev rpm createrepo-c"
```

> 2026-09-30: 这条里原来的 `libarchive-dev pkg-config` 是给上游 OPKG 静态链接用的，
> OPKG 移除后不再需要；换成 `dpkg-dev rpm createrepo-c`（下面四个包管理器验收脚本的
> 对照工具）。`createrepo-c` 包给的可执行名是 **`createrepo_c`**（带下划线）。

- `syslinux`: `/usr/lib/syslinux/mbr/mbr.bin`(MBR 引导码)+ `syslinux --install`(引导分区 VBR/ldlinux.sys)
- `dosfstools`: `mkfs.vfat`(引导分区)+ `dosfsck`(校验)
- `mtools`: `mcopy`/`mdir`/`mmd`(往 FAT 镜像里放文件；注意 mtools 会交互询问，
  脚本里已 `exec </dev/null` 防挂死)
- `bzip2`: busybox 的 `scripts/mkconfigs` 硬依赖，缺了 busybox 编不出来
- `libncurses-dev`: nano 编译依赖
- 包管理器验收还需要（都是**宿主侧对照工具**，不进交付物；缺了 `*-verify.sh`
  会直接退出并点名缺哪个）：`dpkg-deb`+`apt-ftparchive`（dpkg-dev，造真 .deb 与
  Debian `Packages` 索引）、`rpmbuild`+`rpm`+`rpm2cpio`（造真 .rpm 并当判据）、
  `createrepo_c`（造 repodata）、`cpio`、`python3`（起 http 站点 + 造篡改 fixture）
- **产工具链包还需要**（`scripts/build-toolchain.sh` 的取材来源，缺了产出来的包
  只能编 C 静态、C++ 与动态链接全缺）：`g++`（`cc1plus` —— 没有它 `g++` 这个
  文件根本进不了包，guest 里表现为 `g++: not found`）与 `libstdc++-13-dev`
  （`libstdc++.a`/`libstdc++.so`/`libstdc++exp.a` 在 Ubuntu 落在
  **`/usr/lib/gcc/x86_64-linux-gnu/<版本>/`**，不在 `/usr/lib/x86_64-linux-gnu/`，
  按后者去找会静默拷空）。版本跟随宿主探测到的 `$GV`，别写死 13。

### 24.04 构建陷阱（vs 旧 26.04）

- `make` 会触发 `syncconfig` 交互提示（新符号 `INITRAMFS_ROOT_UID`）：
  **先 `make ARCH=x86_64 olddefconfig </dev/null`，编译全程 `</dev/null`**。
- `CONFIG_INITRAMFS_SOURCE` 写进 `.config`（指向 `/home/jgzyes/parlz-kernel/rootfs.cpio.gz`）。
- 缺 OpenSSL stage / paze.a：curl/wget 退回明文 HTTP（HTTPS 待重建）。

## 构建与运行

```bash
# 全量构建(24.04): 依次跑 userland → kernel → ISO(改任何 userland 源码都走这条)
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-userland.sh   # 用户空间 -> images/parlz-initramfs
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-kernel.sh     # 内核 -> images/parlz-bzImage
                                                                                 #  + 末尾生成 images/parlz-bootfat.img(引导分区镜像)
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-iso.sh        # 安装盘 -> images/parlz-install.iso

# 增量快速路径(只改 login.c/sh.c/inittab/boot 脚本时): 重编这两个 + 重打
# rootfs + 编内核 + 生成引导镜像 + 打 ISO。改了 install.c/mount.c/cpfs.c 等
# 其它源码时**不要**用它(新代码不会进 initramfs), 走上面的全量链。
wsl -d Ubuntu-24.04 -u root -e bash /mnt/f/Linux/Parlz/scripts/build-2404.sh

# 运行
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot.sh        # 交互（Ctrl-A X 退出 QEMU）
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-verify.sh # 非交互，检查启动标记(加 login.skip)

# 安装到磁盘
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-install.sh  # 进系统 shell, 手动敲 install(推荐)
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/run-iso.sh      # ① ISO 装盘(全自动)
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-disk.sh    # ② 从装好的盘启动

# 装好的整盘产物在 images/parlz-installed-disk.img(512 MiB, 拿去就能 -boot c)。
# 它是 /home/jgzyes/parlz-disk.img 的字节拷贝 —— **可写工作盘留在 WSL 本地盘**,
# 9p 跨盘跑磁盘 I/O 很慢(装一次盘要写 64 MiB 引导分区 + 50 MiB rootfs);
# 重装完再同步一份到 images/ 即可:
#   cp /home/jgzyes/parlz-disk.img /mnt/f/Linux/Parlz/images/parlz-installed-disk.img
# 那份产物必须用 virtio 挂(盘里 syslinux.cfg 写死 root=/dev/vda2, -hda 会变 sda)
#
# ★ 但**别直接 cp 工作盘当交付快照**(2026-09-28 踩过): 验收脚本装出来的盘是
#   串口序 + 已经首启过(tester/parlz123 是脚本喂的), 拿去 VMware 开机 = 一片黑 +
#   别人的账号。交付快照要用这条脚本重做(交付序 + 不首启 + 自检):
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/refresh-shipped-disk.sh"
        # 交付序引导镜像装盘 → sync → 从盘里读回 syslinux.cfg/vmlinuz 复核
        # → 确认没有 /etc/parlz-auth、没有 /install.d → 同步到 images/

# 安装/自启端到端验收(非交互, PASS/FAIL 一目了然)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/verify-user-install.sh"
        # 在系统内装盘四阶段: guest 里喂 install → 磁盘自启换根 → 首启建用户
        # → 已安装根的 shell 真跑命令 → 宿主复核分区 2(改装盘链路就跑这条)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/iso-disk-e2e.sh"
        # ISO 装盘 → 磁盘自启(同一套判据)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/disk-e2e.sh"
        # 引导镜像挂附加只读盘当来源
# 秒级宿主侧检查(不起 QEMU; 改 cpfs/install 前先导一遍)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/cpfs-oracle.sh"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/check-installed-root.sh /home/jgzyes/parlz-disk.img"

# 引导排查辅助(QEMU -nographic 看不到 syslinux 的 VGA 输出时)
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/dbg-vga.sh       # dump VGA 文本缓冲

# 网络/终端/登录三条专项验收(改 ifc.c / sh.c / login.c 就跑对应这条)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/ifc-verify.sh"
        # 三用例: DHCP(断 resolv.conf 被改写 + 默认路由进 FIB + ping 真通)、
        #          静态旁支(parlz.ip= 一轮配好)、-nic none(10s 放弃不卡启动)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/ctrl-c-verify.sh"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/job-control-verify.sh"
        # 断的是"bash 里那两条 job control 告警不许出现",不是断提示语
wsl -d Ubuntu-24.04 -u root -e bash -c "python3 /mnt/f/Linux/Parlz/scripts/login-pty-test.py"
        # 秒级、不起 QEMU: 9 个用例, 含"43 位长口令""旧明文迁移""多账户(登 bob 必须是 bob)"
wsl -d Ubuntu-24.04 -u root -e bash -c "python3 /mnt/f/Linux/Parlz/scripts/user-cmd-test.py"
        # user add/rm/upd 的参数式与交互式两条路; 判据是"账户表 + 用 login 真登一次"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/login-refuse-verify.sh"
        # 真 QEMU: 3 次错口令必须被拦在 shell 外(旧代码在这必红), 正确凭证放行且 $USER=认证账户

# UEFI 那条引导路(BIOS 交付盘不受影响)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/build-uefi-iso.sh"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/uefi-e2e.sh"
        # 同一张 hybrid ISO 在 OVMF(q35) 与 SeaBIOS(pc) 下各起一次,
        # 断到 Parlz boot ready + 发布号逐字同源 + ifc dhcp 落地
```

Windows 侧双击 `build.bat`（全量构建）/ `install.bat`（装到磁盘）/ `run.bat`
（从磁盘启动）。

**在系统内装盘**（`boot-install.sh` + guest 里 `install`）依赖两个 cmdline 开关：
`install.skip=1` 让 body 不自动装（留 shell 给用户），`parlz.bootimg=/dev/vdb`
告诉 install 引导镜像在哪。注意 `install` 必须在 shell 里能解析到**我们自己的**
安装器而不是 busybox 的 `install`(拷贝文件工具) —— 见 `gen-busybox-links.sh`
的跨目录重名检查（PATH 里 /usr/bin 在 /bin 之前，busybox 软链会盖住真命令）。

## 内核构建的三个必踩陷阱

这些是实测踩过的，改动构建流程时务必保留修复：

### 1. `setup.ld` 的 `.bstext` 填充值（GCC 15 + binutils 2.46）

`arch/x86/boot/setup.ld` 里 `.bstext` 段默认用 `=0xffffffff` 填充。在新工具链下
`header.o` 的 `.bstext` 段内容为空，导致 `setup.bin` 前 495 字节全 `0xFF`，
SeaBIOS 跳到 `0x7C00` 执行 `0xFF` 卡死在 "Booting from ROM..."。

修复：填充改 `=0x00`；并且每次生成 `setup.bin` 后向偏移 0 注入
`EB 6A`（`jmp start_of_setup`）+ NOP 填充。见 `scripts/sync-setup.sh` 与 `build-kernel.sh` 第 6 步。

### 2. netfilter 在 GCC 15 下编译失败

`net/netfilter/xt_TCPMSS.c` 找不到生成的 `xt_tcpmss.h`。在 defconfig 中关闭
`CONFIG_NETFILTER` / `CONFIG_XTABLES` 规避。

### 3. 必需的子系统开关

`parlz_defconfig` 基于 `x86_64_defconfig`，额外打开：
`CONFIG_SERIAL_8250(_CONSOLE)`、`CONFIG_VGA_CONSOLE`、`CONFIG_DEVTMPFS(_MOUNT)`、
`CONFIG_TMPFS`、`CONFIG_BLK_DEV_INITRD`、`CONFIG_EXT4_FS`、`CONFIG_ISO9660_FS`、
`CONFIG_MSDOS_FS`、`CONFIG_VFAT_FS`、`CONFIG_SCSI`、`CONFIG_ATA`、`CONFIG_VIRTIO_*`、`CONFIG_PCI`。

注意 `CONFIG_VIRTIO` 及其依赖（`VIRTIO_PCI_LIB`、`PCI`）是 **select 出来的、不会出现在菜单里**，
必须直接写进 `.config` 再 `make olddefconfig`，否则 QEMU 的 virtio 磁盘不会出现 `/dev/vda`。

## 安装器（`userland/install.c`）的几个硬约束

### 分区重扫：`BLKRRPART` 不可用，用 `BLKPG_ADD_PARTITION`

写 MBR 后内核不会自动重扫分区表（virtio 设备只在插入时扫一次）。
本内核没有 `/sys/block/<disk>/rescan` 属性，`BLKRRPART` ioctl 返回 `EFAULT`。

可行路径：`BLKPG` ioctl + `BLKPG_ADD_PARTITION` 手动注册分区 1。
内核 UAPI 头 `linux/blkpg.h` 在静态用户空间不可用（含 `__user`），
需在 `install.c` 内联定义 `blkpg_partition` / `blkpg_ioctl_arg`。

**不要用 `system()` 调用外部命令** —— 自写的 `userland/sh.c` 不是 POSIX shell
（不支持 `if`/`for`/`$()`/`${}`），`system()` 会失败。一律 `fork` + `execv`。

### `mkfs` 的 ext2 超块偏移（最容易写错的地方）

内核 `fs/ext4/ext4.h` 的 `ext4_super_block` 布局与直觉不符，必须严格对齐：

| 字段 | 偏移 | 说明 |
|------|------|------|
| `s_log_block_size` | 24 | |
| `s_log_cluster_size` | 28 | **容易漏**，漏了后面全部错位 |
| `s_blocks_per_group` | 32 | |
| `s_clusters_per_group` | 36 | **容易漏** |
| `s_inodes_per_group` | 40 | |
| `s_mtime` / `s_wtime` | 44 / 48 | |
| `s_mnt_count` / `s_max_mnt_count` | 52 / 54 | `__le16` |
| `s_magic` | **56** | 不是 52 |
| `s_first_ino` | 84 | |
| `s_inode_size` | **88** | 不是 196 |

其他强约束：

- **超级块固定在分区字节偏移 1024**。4K 块时它在块 0 内偏移 1024，**不是块 1**。
- 4K 块时 `s_first_data_block = 0`（1K 块才是 1）。
- 内核强校验 `s_inodes_count == groups * s_inodes_per_group`，不满足直接
  `Can't find ext4 filesystem`（错误信息有误导性）。
- group descriptor 里 `bg_block_bitmap` 在偏移 0、`bg_inode_bitmap` 在偏移 4（顺序与命名直觉相反）；
  `bg_free_blocks(12)`/`bg_free_inodes(14)`/`bg_used_dirs(16)` 都是 `__le16`。
- 每组头部占 `1 + gd_blocks` 块（超级块 + GDT），随后才是 block_bitmap、inode_bitmap、inode 表。
- 目录条目在 rev 0 下 `file_type` 字段保留为 0。

验证手段（比 QEMU 快得多）：对普通文件直接跑 `mkfs`，再用宿主机工具检查。

```bash
truncate -s 100M /tmp/t.img
/home/jgzyes/parlz-userland/build/bin/mkfs /tmp/t.img 100
dumpe2fs -h /tmp/t.img     # 应当完全无报错
e2fsck -fn /tmp/t.img      # 只应有计数类告警
```

## 代码约定

### 仓库与 `git/`（2026-09-29 起）

工作区**本身不放 `.git`**（工具链与验收脚本都按普通目录树在用），仓库数据在 `git/parlz.git`：

```bash
sh scripts/git-init-repo.sh     # 没有就建，有就 add + 该提交就提交（MSG=… 可指定说明）
sh scripts/web-git-export.sh    # 导出官网仓库浏览器的数据 → web/git/（生成物，不进 git）
```

- 任何 git 操作都要显式给 `--git-dir=git/parlz.git --work-tree=.`；
  **不要**在工作区 `git init` 生成 `.git/`。
- 作者信息用 `-c user.name/-c user.email` 逐条命令传，**不要改使用者的 git 配置**；
  仓库配置里 `core.autocrlf=false` 是硬要求（内核源码树的 LF 不能被改写）。
- 进仓库的内容由根目录 `.gitignore` 的**白名单**决定：`/*` 全排除，再逐项 `!/目录/` 放行。
  ⚠ **gitignore 不支持行尾注释** —— 把说明写在模式同一行会让整条模式永远匹配不上
  （第一版就是这样把整棵 `linux-7.2.5/` 漏掉，仓库里只剩 339 个文件）。
  历次调试留下的暂存目录（`um/ csonly/ cx/ g4/ bin/ mroot/ nanocheck/ …`）与自带 `.git` 的
  `TLS-SSH/` 必须继续排除：`git add -A` 撞上里面的 Windows 重解析点会 `Function not implemented`。
- **产物不进仓库**：`images/`、`output/`、`web/feed/`、`web/rootfs/`、`web/downloads/`、`*.iso`、`*.img`。

- 内核侧改动保持 Linux 风格：`// SPDX-License-Identifier: GPL-2.0`、tab 缩进、
  `__init` 标注、内核日志用 `pr_*`。
- Parlz 自有代码（`userland/`、`scripts/`）注释和输出用中文，与现有文件一致。
- Parlz 标识层只有三处：`include/linux/parlz.h`、`arch/x86/kernel/parlz.c`、
  以及 `head64.c` 中 `start_kernel()` 前的一行 `parlz_boot_init()` 调用。保持克制。
- 用户空间一律静态链接、`-fno-pie`（CMake 里已设），保证 initramfs 内自包含。
- `userland/CMakeLists.txt` 里目标名 **不能叫 `install`**（CMake 保留字），
  用 `parlz-install` 再 `POST_BUILD` 拷贝成 `install`。

## 调试路径

1. 内核不启动 → 看 `serial file:...` 日志；"Booting from ROM..." 后卡住 = 陷阱 1。
2. `Unable to mount root fs` → `CONFIG_BLK_DEV_INITRD` 没开。
3. QEMU 里 `/dev/vda` 不存在 → virtio/PCI 配置缺失，见陷阱 3。
4. 装系统失败 → 按 `install` 的五步输出定位；`mkfs` 报错先跑 dumpe2fs 验证。
5. 串口无输出 → 确认 `console=ttyS0,115200` 与 `CONFIG_SERIAL_8250_CONSOLE`。
6. 卡在 `Username:` / `Parlz login:` 提示 → 登录认证（`/etc/parlz-auth` 凭证，
   首次进入需设置用户名+密码）。自动化脚本在 QEMU `-append` 加 `login.skip=1`
   跳过；`boot-verify.sh` / `login-verify.sh` 等非交互验证脚本走此参数。
7. SeaBIOS 打到 `Booting from Hard Disk...` 之后是 **`Missing operating system.`**
   → 这句出自 MBR 里的 **syslinux `mbr.bin`**（不是 SeaBIOS，也不是我们的
   `install`）：它按活动分区读回 VBR，却没认到 `EB xx 90` + `0x55AA`。最常见来源
   是**装到一半失败的半成品盘** —— MBR/分区表已写、引导分区一个字节没落盘
   （LBA 2048 全零）。一眼定案：
   `dd if=盘 bs=1 skip=$((2048*512+510)) count=2 | od -An -tx1` 应为 `55 aa`。
   同一段代码另两条消息：读扇区失败 = `Boot error`；找不到 `0x80` 活动分区 =
   `Invalid partition table`。`boot-disk.sh` 已在起 QEMU 前做这项检查并直接退出。

## 当前进度

- 内核启动 + 用户空间 shell：**已验证通过**
- 登录认证（`login` 命令：首次设置用户名/密码、之后登录校验，`/etc/parlz-auth`
  持久化，`login.skip=1` 自动化放行）：**已验证**（pty 三路径 + QEMU 标记全过）
- ext4/iso9660/vfat 内核支持、virtio 磁盘、MBR 分区、ext2 格式化：**已验证**
- **安装到磁盘 + 磁盘自启：已验证通过**（`scripts/iso-disk-e2e.sh`：
  ISO 引导 → 从 CD 取引导镜像 → 写 MBR/引导分区/ext2 根 → 重启裸引导 →
  MBR → VBR → SYSLINUX → 内核 → 挂载已安装的 `/dev/vda2` → `pivot_root`
  换真根 → 登录。另 `scripts/disk-e2e.sh` 走"附加只读盘当镜像源"路径）。
- **在运行中的系统里装盘（方式一）：已验证通过**（`scripts/verify-user-install.sh`
  四阶段：guest shell 里喂 `install` → 磁盘自启换根 → 首启建用户 →
  **已安装根里的 shell 真跑命令** → 宿主挂回分区 2 复核）。
- 2026-09-27 修完的四个真实缺陷（都在"装完却起不来"链路上，见下两节）
- 2026-09-27 第二轮：装过去的**根不完整**（cpfs 丢 384 条软链 + 递归了
  `/cdrom` 把整张 ISO 灌进目标），另加 `install` 不拒"往正挂着当根的盘上装"、
  body 把 FAT16 引导分区当根候选 —— 四处全修，见"cpfs 拷 rootfs 的保真要求"

## 磁盘自举的四个必踩陷阱（2026-09-27 实测）

改动安装/引导链路时务必保留这些修复，任一处回退都会静默起不来。

### 1. MBR 必须用 syslinux `mbr.bin`，不要自写

自写 MBR 踩了三个坑，最后统一改用 `/usr/lib/syslinux/mbr/mbr.bin`
（`build-userland.sh` 生成 `userland/mbrbin.h`，install 只填分区表与魔数）：

- INT13 **没有 `AH=0x50` 读扇区功能**。扩展读是 `AH=0x42`，且扇区数/
  缓冲/LBA 全由 16 字节 DAP（`DS:SI` 指向）描述；按寄存器传参必然失败。
- 不能清零 `DL`：BIOS 把启动盘号放在 `DL`，`xor dx,edx; mov dl,0` 会让
  INT13 去读 0 号盘（软驱），VBR 也收到错的 `DL`。
- **最隐蔽**：把 VBR 读进 `0x7C00` 会覆盖**正在执行的 MBR 本体**（MBR 就在
  `0x7C00`），INT13 返回后继续跑的是 VBR 的字节 → 跑飞。现象是 SeaBIOS
  打印 `Booting from Hard Disk...` 之后完全无输出。syslinux mbr.bin 先把
  自身 `rep movsw` 搬到 `0x0600`，再从安全位置读 VBR 并远跳——这才是正解。

### 2. 分区表 CHS 字段要按真实几何算

`install.c` 的 `WRITE_CHS` 必须按 255 磁头/63 扇道折算 `head/track`，
不能写 `0xFE/0xFF/0xFF` 占位——syslinux mbr.bin 会读活动分区的 CHS 找
VBR，占位值会让它报 `Missing operating system`（LBA 字段正确也没用）。

### 3. 引导分区镜像必须由 mkfs.vfat + syslinux + mcopy 生成

`gen-fatboot.sh` 曾用 Python 逐字节手搓 FAT16，根目录被内核读成乱码，
且 VBR 是 mkfs 的 `This is not a bootable disk` 桩——永远起不来。
现在：`dd` → `mkfs.vfat -F 16 -s 2` → `syslinux --install`（写真正的
VBR + `ldlinux.sys`；两者带 hidden/system 属性，`mdir` 要 `-a` 才可见）
→ `mcopy` 放 `vmlinuz`/`syslinux.cfg`/`libcom32.c32`/`libutil.c32`。
脚本末尾有三重独立 oracle：`dosfsck`、VBR 特征串、宿主 `mount` 后
md5 比对镜像内 vmlinuz 与内核产物。

### 4. install 不能内嵌引导镜像（自引用膨胀）

镜像里有 `vmlinuz`，`vmlinuz` 里嵌着 initramfs，initramfs 里又有
`install` 自己——内嵌会让 `install` 体积 = 镜像 = `vmlinuz` = `install`，
每轮构建涨一截，直到 `gen-fatboot` 报 `cluster overflow` / 内核
`do_populate_rootfs` 写挂。现在 `install` 只内嵌 `BOOTFAT_IMAGE_BYTES`
元数据，运行时按序找镜像（`install.c open_boot_image`）：

1. `PARLZ_BOOT_IMG`（cmdline `parlz.bootimg=`，body 导出）
2. `/boot/fat16.img`；3. `/cdrom/boot/fat16.img`（ISO 安装介质）
4. 自动扫描：非目标盘、开头 `EB ?? 90` + `55AA` 的块设备

### 另外三个让"装完停在 initramfs"的坑

- **ext2 inode 字段偏移**：`i_links_count` 在 **26**、`i_blocks_lo` 在 **28**
  （不是 22/24）。写错会让 `lost+found` 的 `links=0` 且 `i_dtime` 非 0，
  内核/e2fsck 视其为"已删除但仍被引用"的 inode，连带上报位图差异、
  目录计数错、根链接数错；修完 `e2fsck -fn` 完全干净。
- **`mount` 要支持 `-t type` / `-o opts`**：启动脚本写的是
  `mount -t ext4 /dev/vda2 /mnt`，旧 `mount` 只认位置式
  （`mount src dst type`），于是 src 被当成 `-t`，mount(2) 必然失败。
  挂 CD 还要 `-o ro`——CD-ROM 只读，不带 `MS_RDONLY` 时内核按可写打开
  会被 sr 驱动拒绝（内核日志 `Can't open blockdev`）。
- **`pivot_root` 的 `PUT_OLD` 必须已存在**：`cpfs` 拷 rootfs 时跳过
  `/mnt`，需先 `mkdir -p /mnt/oldroot`，否则直接 ENOENT。换根后还要在
  新根重挂 proc/sys/dev/tmp（旧挂载点留在 oldroot）。
- **busybox-init 的 sysinit 子进程没有控制台**：body 开头必须先把
  0/1/2 重指 `/dev/console`，而且**顺序是先挂 devtmpfs 再重指**——
  反了的话 `/dev/console` 还不存在，重指失败，整个 body（含 install）
  的输出全丢，串口上只剩 `login.c` 自己的输出，看着像"install 没跑"。
- **busybox applet 软链会盖住 userland 真命令**：PATH 是
  `/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin`，真命令都在 `/bin`，
  busybox 软链大量落在 `/usr/bin` → 同名时 busybox 抢先（实测：guest 里
  敲 `install` 出来的是 busybox 的拷贝文件工具，不是安装器）。
  `gen-busybox-links.sh` 已加**跨目录**重名检查：任一命令目录里已有同名
  真文件就不建该 applet 软链。

### cpfs 拷 rootfs 的保真要求（2026-09-27 实测）

`install` 的第 3b 步用 `userland/cpfs.c` 把当前根（=initramfs 根）整棵
写进分区 2 的 ext2。两条铁律，破了都会"装成功但系统不能用"：

- **符号链接必须照拷**：initramfs 里 384 个软链 vs 80 个真文件 ——
  `/bin/busybox`、`/bin/sh→ash`、`/usr/bin/*` 全是软链，丢了等于命令表清空。
  ext2 快链（目标 <60 字节）把串直接放进 inode 的 `i_block` 那 60 字节且
  **`i_blocks` 必须留 0** —— 内核 `__ext2_read_inode` 就是按 `i_blocks==0`
  区分快/慢链的；≥60 字节才分配数据块并置 `i_blocks=8`。
- **挂载点只能建成同名空目录，绝不能递归**：判据是 `st_dev != 根的 st_dev`
  （外加顶层名字表 `proc sys dev tmp run mnt cdrom`）。踩过：装盘时 ISO 已挂在
  `/cdrom`，cpfs 把它当普通目录往下拷 —— 64 MiB 的 `fat16.img` + 37 MiB 的
  `vmlinuz` 一起灌，中途失败，目标分区里只剩 `cdrom` 和 `lost+found`，
  而串口日志照样打 `pivot_root OK`（换个空根也照打），看着像装好了。

配套的两个小坑（同一链路）：

- **`install` 要拒绝往已挂载的盘上装**：在系统内敲 `install` 时，目标盘可能
  正挂着当根（`cpfs` 读的就是当前根 → 边跑边重写自己脚下的盘）。
  `install.c target_mounted()` 扫 `/proc/mounts`，命中 `/dev/vda`、`/dev/vda1`、
  `/dev/vda2` 即拒绝并提示从安装介质启动。
- **body 挑根只认 ext2/ext4，且候选别用 `ls` 去 parse**：`parlz-boot-body.sh`
  早先 `ls /dev | grep -E '^(vd|sd)[a-z]+[0-9]+$' | head -1` 取第一个分区当根
  候选，两处错：
  1. **`busybox ls` 在 tty 上按多列排版**（body 的 stdout 就是 `/dev/console`，
     是 tty！）—— 一行里挤着 `vda vda1 vda2`，`^…$` 正则一行都匹配不上，
     候选**恒为空**。所以不带 `root=` 的路径（boot-install.sh 那套）从来没
     认出过已安装的盘，只在 syslinux.cfg 显式给 `root=/dev/vda2` 时才碰巧能用。
     取设备名一律用 shell 全局展开：`for _n in /dev/vd[a-z]*[0-9]; do [ -b "$_n" ] && …`。
  2. 装好的盘上第一个分区是 **FAT16 引导分区**，把它当根候选挂上后 pivot 不了
     （留在 initramfs），还会让上面那条 `target_mounted()` 守卫误判"目标盘已被
     挂载"而拒绝重装。现在逐个分区试 `ext4`/`ext2`，谁挂得上谁当根。
  另外 **devtmpfs 是逐个、异步注册分区节点的**：内核日志已打 `vda: vda1 vda2`，
  `/dev` 里当时可能只有 `vda1`，所以"取候选 + 逐个试挂"要整轮重试（实测几轮
  ×2s 足够），连整盘节点都没有（真没插盘）时立刻退出别白等。

### 装盘会话缺安装介质：怎么一眼定案（2026-09-27 用户实测）

引导镜像（FAT16 引导分区的内容）刻意**不进 initramfs**（自引用膨胀），所以
`install` 必须有外部来源。用户在实际会话里敲 `install` 撞到"找不到引导镜像"，
按下面的表就能立刻判读：

| 启动方式 | 镜像来源 | 能不能装 |
|---|---|---|
| `install.bat` / `scripts/boot-install.sh` | 只读盘 `/dev/vdb` + cmdline `parlz.bootimg=/dev/vdb` | ✅（推荐） |
| `scripts/boot.sh`（默认） | 同上（`PARLZ_NO_IMG=1` 时没有） | ✅ |
| `run-iso.sh` / 真机 ISO | ISO 挂到 `/cdrom`，取 `/cdrom/boot/fat16.img` | ✅ |
| `run.bat` / `boot-disk.sh` | **无**（它就是"纯磁盘自启"验收，等价真机按下电源键） | ❌ 装不了，属设计内 |

`install.c open_boot_image()` 的探测顺序：`$PARLZ_BOOT_IMG` → `/boot/fat16.img`
等静态路径 → 已挂载 ISO 里的 `boot/fat16.img`（扫 `/proc/mounts`，挂载点不限）
→ 非目标盘的 FAT16 引导盘。失败时现在会打一行**块设备清单**
（`当前块设备: sr0(1023 MiB, 只读) vda(512 MiB)`），一眼看出"这个会话没挂第二块盘"。

两个必须留意的实现细节：

- 自动探测**遍历 `/sys/block` 而不是 `/dev`**：`/dev` 里的节点由 devtmpfs
  异步创建，装盘那一刻可能还没有 —— 拿 `/dev` 当清单会漏掉已经挂好的镜像盘。
  缺节点时用 `/sys/block/<n>/dev` 的 `maj:min` 自己 `mknod`（`ensure_dev_node()`）。
- `PARLZ_BOOT_IMG` 指向的设备打不开时，也先补节点再重试，别直接放弃。

### 提示符与作业控制的两个真相（省得下次误判成"env 丢了"）

- `bash` 提示符里 `\u` 取的是**有效 uid**（→ `root`），`\h` 取的是
  `gethostname()`；自研 `parlz-sh` 用的是 `$USER`/`$HOSTNAME`。所以
  `a@parlz:~> bash` 之后变成 `root@...` **不是环境变量没继承**（`sh.c`
  的 `execve` 一直把 `environ` 传下去）。`(none)` 才是真问题：没人调过
  `sethostname` → body 现在开头 `busybox hostname parlz` 设一次。
- 从 body 起的 shell 里再跑 `bash` 曾打印
  `cannot set terminal process group` / `no job control in this shell`。
  两层原因，两层都已修：
  1. busybox-init 的 `::sysinit` 子进程**不是 session leader**，打开
     `/dev/console` 不会成为控制终端 → `sh.c` 交互启动时自己
     `setsid()` + `ioctl(TIOCSCTTY)` + `dup2(0/1/2)` 接管（**不要**只把 body
     末尾改成 `exec setsid ...` —— busybox `setsid` 在需要 fork 时父进程立刻
     `EXIT_SUCCESS` 返回，源码里那条 TODO 就是没做 waitpid，sysinit 会被判定
     提前结束）。
  2. 光有 ctty 还不够：子进程留在 shell 的进程组里，bash 自己
     `setpgid`/`tcsetpgrp` 抢不到前台。现在 `sh.c` 每条前台命令（整条管道算
     一组）都放进独立进程组并 `tcsetpgrp` 指过去，命令返回后收回 ——
     `setpgid` 父子各调一次是消竞态（子进程一旦 exec，父侧再 setpgid 就
     EACCES）。判据见 `scripts/job-control-verify.sh`：两条告警**不许出现**，
     且 bash 里 Ctrl+C 仍要拿到 130。
- Ctrl+C 之后 `$?` 必须是 `128+信号`（130/137），不是 0：`sh.c` 里单命令和
  管道两处状态换算都要走 `WIFSIGNALED` 分支。

### 发布（release）

发布号是**单一来源、三处一致**的，格式就是用户要的形态：

```
<时区>+<构建时间>+<构建人>+<阶段>.<大版本>.<小版本>
CST+0800+20260927-163908+jgzyes@parlz.com+rc.0.1
```

| 出现位置 | 形态 |
|---|---|
| `/proc/version`、`uname -r` | `Linux version 7.2.5-<ID> (jgzyes@parlz.com) ...` |
| 内核启动横幅 | `Parlz release <ID>`（第一行仍是 `Parlz 0.1.0 on x86_64`，验收脚本按它 grep） |
| `/etc/parlz-release` | `version: <ID>` + 各字段（装进磁盘后跟着盘走） |

生成物两份，都别手改：仓库根的 `.parlz-release`（各字段，给 shell 读）与
`linux-7.2.5/include/linux/parlz-release.h`（给内核读）。
`build-userland.sh` / `build-kernel.sh` 会自动读它们：前者写 `/etc/parlz-release`，
后者 `scripts/config --set-str CONFIG_LOCALVERSION "-parlz-<ID>"` 并把
`KBUILD_BUILD_USER/HOST/TIMESTAMP` 设成 `jgzyes` / `parlz.com` / 构建时间戳
（`linux_banner` 里的 `(user@host)` 与末尾日期就来自这三个环境变量）。
没有 `.parlz-release` 时是日常增量构建，用头文件里的 `dev.0.1` 默认串。

```bash
# 出一条 rc 发布(默认 stage=rc major=0 minor=1) → 产物收进 output/
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/make-release.sh rc 0 1"
#   只快跑一遍(跳过装盘端到端):  SKIP_E2E=1 ...
#   保留上一次发布的文件:        KEEP_OUTPUT=1 ...
```

八步，一步不过就整体失败（`output/` 里不会留下半成品）：
① 生成发布号(`.parlz-release` + `parlz-release.h` + `.pm-release`)
② userland(裁剪 + `/etc/parlz-release`) ③ 内核(+引导镜像) ④ ISO
⑤ **版本自检**：真起一次 QEMU，断言横幅/`/proc/version`/`uname -r`/
`/etc/parlz-release`/`pm --version` 五处同源
⑥ 装盘四阶段端到端 ⑦ 收产物 + 生成 feed ⑧ **镜像站端到端**：起
`pm-server`，guest 里 `pm available` → `pm install core` → 被裁的
`nano`/`awk` 恢复且真跑出结果。

三个坑：

- **UTS_LEN 只有 64 字节**（含 NUL）。`7.2.5-parlz-<ID>` 现在 60 字符，加字段
  （比如把主机名也塞进 ID）很容易越界，越界会被截成半个版本号 ——
  `make-release.sh` 和 `build-kernel.sh` 里都有硬长度检查挡着。
- **标识层文件要同步进编译副本**：`/home/jgzyes/parlz-kernel` 是一次性
  `cp -a` 出来的，改完仓库里的 `parlz.h`/`parlz.c`/`parlz-release.h` 不同步就
  会编出旧横幅（`build-kernel.sh` 第 1 步现在每次同步这四个文件）。
- 校验发布号**别 grep `bzImage`**（gzip 过的镜像里搜不到字符串），要看
  `/home/jgzyes/parlz-kernel/vmlinux` 里的 `linux_banner`，或者直接起 QEMU
  看 `/proc/version`（`make-release.sh` 的第 5 步两头都做了）。
- **发布号里的反斜杠（VGA 位 `-F\V` / `-UN\V`）会被吃掉两层，`CONFIG_LOCALVERSION` 根本装不了它**。
  实测：`scripts/config --set-str CONFIG_LOCALVERSION "…\\V"` 之后一次 `make olddefconfig`
  能把**整个符号解析成空串**（kconfig 的字符串词法把 `\` 当转义符处理）。所以发布号改走
  **`make KERNELRELEASE=…` 命令行变量** —— Makefile 里 `ifeq ($(origin KERNELRELEASE),file)`
  不成立时 `filechk_kernel.release` 就直接 `echo` 我们给的值，既绕开 kconfig 也绕开
  `setlocalversion`。这条链上有两层在吃反斜杠：`filechk` 里那句 `echo`（dash 的 builtin
  解一次）+ C 字符串字面量再解一次，**实测 8 个进去、1 个出来**。层数跟 `/bin/sh` 是哪个
  实现有关，所以 `build-kernel.sh` 不写死数字：它把候选值逐层加倍，每一层都真编译一个
  `puts(UTS_RELEASE)` 的小程序与发布号**逐字比对**，取第一个相等的；五层内找不到就失败退出。
- **版本判据一律 `grep -F`（定值串）**。用 BRE 的话 `\V` 等价于 `V`，反斜杠全丢了判据照样
  绿 —— 上一轮"五处同源"自检就是这么放过了 `-FV`（`uname -r` 与 `/etc/parlz-release`
  已经不一致，五项里却只有一项报红）。现在 `build-kernel.sh` 编完还会
  `grep -aF "Linux version $REL_FULL" vmlinux` 硬卡一道，`make-release.sh` 的
  第 3/5 步也都改成了 `-qaF`。
- **加了 VGA 位后 `7.2.5-parlz-<ID>` 会超 `UTS_LEN`（64 含 NUL）**：现在
  UTS 用 `7.2.5-<ID>`（58~59 字符）。`make-release.sh` 与 `build-kernel.sh`
  都有 `${#REL_FULL} -gt 63` 硬失败检查，别把它放宽。

### 控制台：谁当 /dev/console（VMware 黑屏 → 卡住无键入的根因）

`isolinux.cfg`（ISO，`build-iso.sh`）与 `syslinux.cfg`（磁盘，`gen-fatboot.sh`）
的 `APPEND` **都来自 `scripts/console-cfg.sh`**（唯一来源，别再在各自脚本里拼串）。
规则：

- 内核给**所有** `console=` 设备发 printk，但**最后**一个才是 `/dev/console`；
  我们的启动 body 把 0/1/2 重指 `/dev/console` —— 所以"屏幕看得见但敲不了键盘"
  和"完全黑屏"是同一个顺序问题的两个方向。
- **交付介质默认 `PARLZ_CONSOLE=vga`** = `console=ttyS0,115200 console=tty0`
  → `/dev/console=tty0`：VMware/真机屏幕**既能看也能输入**，串口那一路照样收
  printk（宿主还能用 `-serial file:...` 留日志）。
- **`PARLZ_CONSOLE=serial`** = `console=tty0 console=ttyS0,115200`
  → `/dev/console=ttyS0`：只给 **QEMU `-nographic`** 的验收脚本用 —— 它没有可输入
  的屏幕，用户空间输出/键盘必须在串口上，否则 `iso-disk-e2e.sh` 抓到的串口日志
  是空的（装盘其实成功，判据却全灭）。
- 因此跑 `-nographic` 的脚本**不碰交付 ISO**，改用自己的串口序旁支产物：
  `scripts/serial-media.sh` 里的 `parlz_ensure_serial_media [force]` →
  `images/parlz-install.e2e.iso` + `images/parlz-bootfat.e2e.img`
  （`iso-disk-e2e.sh` 每次 force 重建；`run-iso.sh` 按需重建，
  `PARLZ_USE_DELIVERY_ISO=1` 可强行用交付那份但会看不见安装过程）。
  路径覆盖靠 `PARLZ_ISO_OUT` / `PARLZ_BOOTFAT_OUT` / `PARLZ_BOOTFAT_IN`。
- **旁支产物的判据必须是"内容 + 与本轮内核一致"两条，不能只看控制台序**
  （2026-09-28 踩过）：`parlz_ensure_serial_media` 早先只 grep 串口序 APPEND，
  而那份 `.e2e.img` 是改 body **之前**生成的 —— 序对、内核旧，于是
  `verify-user-install.sh` 的磁盘自启跑在旧 initramfs 上：新的 `login` 写出新
  格式账户表，旧 body 却还在 `head -1` 取用户名，`$USER` 成了文件第一行的注释。
  现在两处都加了 `mcopy … ::/vmlinuz | md5sum` 与本轮 `parlz-bzImage` 比对
  （`serial-media.sh`、以及 `build-iso.sh` 里"交付 ISO 嵌的引导分区镜像"那段）。
  **凡是用共享产物当输入的地方，都要问一句"它可能被谁最后写过"**。
- 同理，`verify-user-install.sh` / `disk-e2e.sh` 现在显式 `source
  serial-media.sh` 用串口序引导镜像，不再直接用 `images/parlz-bootfat.img`
  —— 那个文件是**交付序**（build-kernel 末尾生成），拿它跑 `-nographic`
  会"装得上、看不见"。
- **`PARLZ_VGA=0`** 出无 VGA 版（只有 `console=ttyS0,115200`），发布号后缀 `-UN\V`；
  默认带 VGA，后缀 `-F\V`。`make-release.sh` 把 `PARLZ_VGA` export 给两个打包脚本，
  **开关和产物内容不允许各说各话**：`build-iso.sh` 用 `console-cfg.sh` 算出的
  **整行 APPEND** 去 `grep -aF` ISO 里的实际内容，对不上直接失败。
- 只写 `console=ttyS0` 时 VGA 根本不注册 → isolinux 打完 `Booting the kernel`
  之后屏幕一片黑（看着像卡死，其实系统在跑）。
- VMware 默认往往没有虚拟串口，要在 VM 设置里加 Serial Port（COM1 → 输出到文件），
  否则串口那一路没地方去（不影响 VGA 显示与输入）。
- `boot-disk.sh` 起 QEMU 前会 grep 盘里的 `APPEND` 行，打印 `/dev/console` 落在哪；
  显示的是 vga 序时，串口窗口看不到 shell 属正常，按提示用 `run-iso.sh` 重装一份
  串口序的盘即可。（注意 grep 整块盘，不要只 `dd` 开头几个扇区：`syslinux.cfg` 挨在
  29 MB 的 `vmlinuz` 之后，窗口太小会读不到、提示就永远不触发。）

### El Torito 的 load sectors 必须是 4（VMware 死在 `Booting from 0000:7c00` 的根因）

`xorriso -as mkisofs ... -b /isolinux.bin -c isolinux.catalog -no-emul-boot
**-boot-load-size 4**`。这是 isolinux 官方配方：第一阶段只占一个扇区，其余由 isolinux
自己用 INT13 读。**不写这项 xorriso 会按整档记账**（实测 `Ldsiz=76`）—— SeaBIOS 宽容、
照样引导，**VMware 的 BIOS 直接死住**，屏上只剩 `Booting from 0000:7c00`，一行输出都
没有。也就是说 QEMU 那条路永远暴露不了这个偏差。`build-iso.sh` 现在两条硬判据：
`El Torito boot img` 行的 load secs 必须 `== 4`，且镜像头 2048 字节的 md5 必须等于
`/usr/lib/ISOLINUX/isolinux.bin` 的头 2048 字节（防止"文件名对了、内容不对"）。

### 引导器要自报家门（分不清断在哪一段时最省时间）

`isolinux.cfg`（ISO）与 `syslinux.cfg`（磁盘）都写：

```
PROMPT 1
TIMEOUT 50
SAY ">> Parlz: ISOLINUX took over (this line = El Torito + isolinux.bin OK)"
```

`SAY` 的内容**必须是 ASCII** —— 它走 INT10 逐字符打到 VGA 文本模式，那会儿还没有
UTF-8 控制台，中文只会是乱码。判读方式：屏上看不见 `took over` 那行 = "BIOS → 引导
镜像 → `ldlinux.c32`" 那一段的事；看见了却仍不往下走 = 内核/控制台那一段的事。

### ISO 被虚拟机占用时怎么写进正式名

9p 上被 VMware 当 CD 开着的 ISO：`rm`/`rename` 报 `Permission denied`，**但原地写是
允许的**。所以 `build-iso.sh` 一律先写 `<目标>.tmpbuild`，内容全部验通之后再发布：
`mv` → 失败就 `cat 临时 > 目标` 原地覆盖（再比一次大小）→ 都不行才另存
`*.busy-<时分秒>.iso` 并**以失败退出**（绝不能让 `make-release.sh` 拿着上一轮的旧 ISO
当本次产物）。用户侧仍建议**换文件名**重新指 VM：Windows 可能按旧句柄缓存介质内容，
原地覆盖过的那张在下一次冷启动前不一定是干净的。


### UEFI 引导：xorriso 加第二条引导项会把 BIOS 那条顶掉（2026-09-28 实测）

交付 ISO/磁盘一直是 **BIOS 专用**（ISO = El Torito + isolinux，磁盘 = MBR +
syslinux VBR）。固件设成 UEFI 时（VMware 新建虚机默认 UEFI、近十年真机也是）
启动项里根本看不到这张盘。试过两条路：

- **给 isolinux 的 ISO 加第二条 EFI 引导项 —— 走不通**。`xorriso -as mkisofs`
  下 `-b`（BIOS）与 `-e`（EFI）**互相顶替**，实测 `-report_el_torito plain`
  里只剩一条（先 `-b` 后 `-e` 就只剩 UEFI，反之只剩 BIOS）；native 的
  `-boot_image any partition_table=on efi_boot_part=…` 能写出 isohybrid 的
  GPT 追加 ESP 分区，但 El Torito 仍然只有一条。**这种"加了 UEFI 就把 VMware
  那条能用的路断了"的改法一律不接受**，`build-iso.sh` 里那一段注释就是记录这事。
- **正解 = `grub-mkrescue`**（`scripts/build-uefi-iso.sh`）。它天生出 hybrid：
  实测 `boot img 1 BIOS (Ldsiz=4) + boot img 2 UEFI`，同一张盘在
  OVMF(q35) 与 SeaBIOS(pc) 下**都**跑到 `Parlz boot ready`。
  验收：`scripts/uefi-e2e.sh`（两种固件各断 GRUB 菜单标题 / Parlz 横幅 /
  发布号与 `.parlz-release` 同源 / boot ready / DHCP 落地）。

要点：

- GRUB 的 BIOS 项 load sectors 也是 4，与上面那条 VMware 规则不冲突；
  但**这张 hybrid 盘还没在 VMware 里实测过**，所以它现在只是并列产物
  （`images/parlz-install-hybrid.iso`），交付 ISO 仍是 isolinux 那份。
  要换交付介质，先在 VMware 里 BIOS/UEFI 各起一次。
- UEFI 下没有 isolinux 的 `SAY`，"自报家门"靠 GRUB 菜单标题
  `menuentry "Parlz booting (console= order decides who gets keyboard)"`。
- **验收必须让 GRUB 自己也走串口**：`grub.cfg` 里的
  `terminal_input/output serial` 只在 `PARLZ_CONSOLE=serial` 时写 —— 内核
  cmdline 的串口序与 GRUB 的输出目标是两回事，只改前者会"内核起来了但
  引导器一段看不见"。
- 内核/initramfs 不复制第二份：`linux /vmlinuz`（initramfs 已嵌在 bzImage 里），
  `APPEND` 串仍由 `console-cfg.sh` 单点生成，`build-uefi-iso.sh` 末尾用
  `grep -aF` 验它真落进了 ISO。

### `ifc` 配网：不许"静默长等"（2026-09-27 用户实测"卡在等 link up"）

`parlz-boot-body.sh` 是**同步**调 `/bin/ifc auto …` 的 —— ifc 拖多久，login 就等多久。
所以 `userland/ifc.c` 里任何等待都必须：**有界**（有上限）、**有判据**（不靠猜）、
**有声**（每秒/每几轮打一行）。三条都已落实，改这块时别退回去：

- **等 link 用 `wait_running()`**：读 `/sys/class/net/<if>/carrier`（读不到退回
  `ioctl IFF_RUNNING`），**不要**去订阅 netlink 的 `RTMGRP_LINK` 事件等 link up ——
  事件可能早于订阅就发生了，等不到就是干等。首轮最多 5 s、之后每轮 1.5 s，
  carrier 一上立刻继续。
- **`IFF_UP ≠ link up`**：`link_up()` 只是把 IFF_UP 置上；驱动（e1000/virtio）的
  `Link is Up` 是**异步**的，而网关路由要走 `fib_check_nh`，设备没 RUNNING 就是
  `ENETDOWN`。所以配 IP 前先等 carrier，网关那一步再等一次（别用 `sleep(3)` 盲等）。
- **`auto` 模式没有接口就早退**：十轮看不到非 lo 接口（如 QEMU `-nic none`）直接
  放弃 + 提示手动配，别把启动卡满 60 轮。
- 收尾的失败信息要带**真实原因**（`bail` 变量），别再出现"10 s 就放弃却打印
  ‘60s 内配置失败’"这种自相矛盾的日志。

验证脚本：`scripts/ifc-verify.sh`（三个用例真起 QEMU，判据都断**终态**，不看
`ifc` 自己打的 OK）：A `ifc dhcp` 一轮拿到 10.0.2.15/24 之后，guest 里
`cat /etc/resolv.conf` 必须是 DHCP 那份（有 10.0.2.3、**没有**烘进去的 8.8.8.8
兜底）、`/proc/net/route` 有默认路由、`ping 10.0.2.2` 真收到回包；B 静态旁支
（cmdline `parlz.ip=`）必须 **round 1** 就 `carrier up → addr OK`；C `-nic none`
必须 10 s 放弃、≤25 s 到 boot ready。改 `ifc.c` 后请把 userland→kernel→ISO
全量链走一遍（它属于"其它源码"，**不能**用 `build-2404.sh` 快速路），再跑这个脚本。

### 交付介质的网络身份：不许焊 QEMU 的东西（2026-09-28 自查）

`10.0.2.15 / 10.0.2.2 / 10.0.2.3 / 10.0.2.2:8765` 这一串是 **QEMU user-NAT 的
约定**，一度同时出现在四处，其中三处在交付物里：

| 位置 | 原来 | 现在 |
|---|---|---|
| `parlz-boot-body.sh` | `ifc auto 10.0.2.15 255.255.255.0 10.0.2.2` 焊死 | 默认 `ifc dhcp`；cmdline `parlz.ip=/parlz.netmask=/parlz.gw=` 才走静态；`net.skip` 整个跳过 |
| `/etc/resolv.conf` | 只有 `nameserver 10.0.2.3` | `10.0.2.3 → 8.8.8.8 → 1.1.1.1` + `options timeout:1 attempts:1`，DHCP 起来后由 `ifc` 覆盖 |
| `/etc/pm/feeds.conf` | `http://10.0.2.2:8765`（开发机） | `http://www.parlz.com/feed`（官网即镜像站） |
| body 里 `export PM_FEED` | **无条件**导出，把上面那份盖掉 | 只在 cmdline `parlz.feed=` 时导出 |

危害不是"配不上"而是**配上了但谁也连不通**，还打 `init: network up`；用户侧
表现是 `pm` 拉不到包 / `curl` 域名解析失败，离真正的失败点隔了好几层。

- **`ifc dhcp` 是自写的**（`userland/ifc.c`，AF_PACKET 裸帧 + 混杂模式，自己组
  IP/UDP 头、算 IP 校验和、UDP 校验和按 RFC 768 置 0）。不走 UDP 套接字是因为
  客户端此刻还没有地址，广播收发要靠 `0.0.0.0/8` + broadcast 路由撑着，行为随
  内核版本变。时间上界照旧：选接口 ≤10 s，每轮 DISCOVER/REQUEST 各 3 s、共 2 轮。
- QEMU user-NAT **自带 DHCP**，所以自动化拿回来的还是 10.0.2.15/24 —— 换默认路径
  不影响任何既有判据。
- `resolv.conf` 里那行 `options timeout:1 attempts:1` 是给 glibc 解析器的：默认
  5 s×2 次才换下一个 nameserver，第一个不可达时 `curl` 会表现成"看着像卡死"。
- **`ifc dhcp` 每次成功都会整份重写 `/etc/resolv.conf`**（没有 `resolv.conf.tail`
  那种合并机制）—— 手工编辑的文件活不过下一次启动。要给用户自定义留地方的话，
  先加合并再改这里，别悄悄保留覆盖行为。
- `ping.c` 自己发 UDP 查询，早先**只取第一个** nameserver（于是"ping 域名不通但
  curl 通"）；现在按 resolv.conf 逐个问，并读 `options timeout:/attempts:`。
- 改网络这块的回归全在 `ifc-verify.sh`，别只跑 `boot-verify.sh`。

### 登录与账户：多账户 + 失败不放行（2026-09-28）

**账户表与散列只在 `userland/parlzauth.c` 里实现一份**，`login`（认证）与
`user`（增删改）都调它 —— 文件格式、散列参数、明文兼容规则不许各写一套。

`/etc/parlz-auth` 格式：

```
# 注释行会被跳过
用户名:$6$…       每行一个账户
```

- 散列是 libxcrypt 的 SHA-512 crypt（`crypt_r`，`CMakeLists.txt` 给两个目标都
  加了 `-lcrypt`；静态链一个 `.a` 就够）。理由很实际：**装好的整盘 IMG 是官网上
  的下载物**，明文口令会跟着盘一起分发，0600 挡不住拿到盘文件的人。
- **旧格式（第 1 行用户名、第 2 行口令）必须一直读得懂**：已经分发出去的
  `images/parlz-installed-disk.img`（tester/parlz123）就是那种。明文或旧格式在
  成功登录时**当场迁移**成 `名:$6$…`；写失败（只读根）不拦登录。
- 命名规则见 `pa_name_ok`：不能含 `:`、空白、控制字符，不能以 `#` 开头
  （否则会被当成注释行吃掉），长度 < 32。
- `pa_save` 是**先写 `.new` 再 rename**：中途掉电不会把凭证文件写空
  （写空 = 谁都登不进去）；并且 `fsync` + 收尾 `sync()` —— 不给的话"设完密码
  立刻断电/kill"那次写入只在页缓存里（实测：验收脚本 kill QEMU 后宿主挂盘复核，
  文件根本不在；用户侧是"设了密码，下次启动又要重设"）。
  验收脚本因此也要**先让 guest `sync` 再 kill**（`verify-user-install.sh` 阶段 2）。

**认证失败绝不放行**（用户报的缺陷：3 次输错照样进 shell）。`parlz-boot-body.sh`
现在看 `login` 的退出码：非 0 → 打一行"认证失败,2 秒后重试"并**重新弹认证，
永远不进 shell**；非 tty（自动化）与 `login.skip` 路径下 login 直接返回 0，
不会卡在重试上。

**$USER 取的是"认证过的那个人"**：login 是子进程，`setenv` 传不回 body，所以它
把用户名写进 `/tmp/.parlz-login-user`（`PA_WHOAMI_PATH`），body 读出来导出后
立刻删。以前 body 用 `head -1 /etc/parlz-auth`——单账户时看着对，多账户下必然
张冠李戴。

回归（三个层次，改 login/user 都要跑）：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "python3 /mnt/f/Linux/Parlz/scripts/login-pty-test.py"   # 9 用例
wsl -d Ubuntu-24.04 -u root -e bash -c "python3 /mnt/f/Linux/Parlz/scripts/user-cmd-test.py"    # 11 用例
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/login-refuse-verify.sh             # 真 QEMU
```

- 前两个是宿主 pty 测试，秒级不起 QEMU：`login-pty-test` 覆盖首设/登录/3 次拒绝/
  43 位长口令/旧明文迁移/多账户（登 bob 必须是 bob）；`user-cmd-test` 覆盖
  add/rm/upd 的**参数式与交互式**两条路，判据是"账户表内容 + 用 login 真登一次"
  （旧口令必须失效、新口令必须可用），而不是看命令自己打的字。
- `login-refuse-verify.sh` 是端到端的：把预置账户表塞进 initramfs 副本（**把
  bob 放第一行**），真起 QEMU 连喂 3 次错口令 → 断"拒绝登录"出现且此刻日志里
  **没有** `Parlz shell`，再喂正确凭证 → 断进去了且 `echo WHO=$USER` 是 alice。
  旧代码在②必红。
- 写这类 pty 测试的两个坑：`pa_read_pass` 进 raw 模式用 `TCSAFLUSH`，**会把已经
  排队的输入丢掉** —— 必须"看到提示再发下一行"，一次把三行全塞进去会死等到超时；
  `waitpid` 更不能无超时地等（拒绝后 login 还会再问两轮，只喂一轮它就停在那儿，
  整条测试会挂死）。

### parlz-sh 的词法：引号 / 分句 / 赋值（2026-09-28 自检）

六个真缺陷，全部有回归用例（`scripts/sh-semantics-test.sh`，宿主侧秒级；
**旧二进制 11/25 PASS**，是这些 bug 的直接证据）：

| 现象 | 根因 | 现在 |
|---|---|---|
| `echo "a  b"` 打出 `a b`（空格被折叠） | 引号先被**删掉**再按空白切词 | 引号是定界符：引号内的空白、`|`、`;` 都是普通字符 |
| `sh -c "echo x"` 里 x 成了独立参数 | 同上 | 一个引号段 = 一个 argv |
| `echo "a|b"` 被当管道劈开 | 同上 | 引号内的 `|` 不切段 |
| `X=hello` 之后 `echo $X` 是空的 | `putenv` 存了**指向局部 buf 的指针**，函数一返回就悬空；且只认大写名字 | 行首赋值立刻 `malloc` 一份 `putenv`（本行可见 + 活过本行），名字大小写都认 |
| 交互里 `echo a; echo b` 把整串当 echo 的参数 | `;` 只在 `sh -c`/脚本里切，交互路径不切；而且那边是**盲切** | 新增 `run_cmds()`：交互 / `sh -c` / 脚本三处共用，**只切引号外**的 `;`（`awk 'BEGIN{print 1; print 2}'` 不再被劈） |
| `cat < f` 空输出 | 两处: `apply_redir` 里 `dup2(fd,0)` 之后又 `dup2(3,0)` 把管道读端换回去; **而且 `cat` 无参数时根本不读 stdin**(POSIX 把缺省操作数当 `-`) | 输入重定向真把**子进程** stdin 换成文件; `cat` 无参数 = 读 stdin |
| `sh -c 'ls /nope'` 退出码永远是 0 | `run_cmds` 的返回值被“要不要退出 shell”和“最后一条命令的状态”两用 | `sh_exit_req` 单独传“退出”, 返回值 = 最后一条命令的退出码(`exit N` 也认) |
| `rm -rf` 既不递归也不 force | 选项解析写成 `opts[0]=0; o=1;` —— 先把首字节写成 NUL 再从 1 开始填字母, `opts` 永远是空串 | 改成 `snprintf(opts, …, argv[i]+1)` |
| `grep` 无匹配也返回 0; `grep -c` 单文件打 `name:count` | 选中行数没传出来 | 无选中 → 1, 出错 → 2(与 GNU 一致); `-c` 只在多文件时带文件名 |
| 串口行尾的 `\r` 变成一条空命令(`sh: : not found`) | 解析器只把 `\n` 当行尾 | `run_line`/`run_cmds`/分词器都把 `\r` 当空白 |

另外把分词器的"就地压缩"改成了独立词缓冲（`tbuf`）：就地写会把词的终止 NUL
压在分隔符上，于是后面整行被吃掉（`echo hi` 只剩一个词 —— 这个 bug 差点当成
"新解析器全坏"）。改 `sh.c` 的词法/重定向后，先跑语义套件再起 QEMU。

### NTCLKS 发行版（ParlzOS on ntclks，2026-09-27 新增）

`NTCLKS-main/` 是 **LeonOS 4 的独立 Ring-0 内核仓库**（Apache-2.0）：产物只有内核侧六件
（`kernel.sys`、`kernel.debug`、`loader.elf`、5 个 `.drv`、`kerneldebug.sys`），
**不含用户态、镜像与打包目标** —— 发行版这一层全是我们自己写的：

    scripts/build-ntclks-distro.sh   # 编内核 → installer-root → GRUB 多引导2 ISO → 冒烟引导
    scripts/run-ntclks.sh            # 起 QEMU（UEFI/OVMF）

四个必踩（都实测过）：

1. **只支持 UEFI + SATA**。
   - **UEFI**：loader 是 64 位代码，MB2 在 BIOS 下按 32 位保护模式交接 —— 一跳过去就
     triple fault 复位：GRUB 菜单正常、`boot` 之后串口/VGA 全无输出，QEMU monitor 里
     `info registers` 看到的是复位后的 SeaBIOS 状态。loader 头部带 EFI64 入口标签
     （tag 9）并要求 EFI 系统表，上游本来就是 UEFI 引导。
   - **SATA**：QEMU 用 `-machine q35`（它的盘/光驱在 AHCI 控制器上，实测日志
     `ahci=0:31.2 port=2`）；默认的 `pc` 机器那套 IDE 不在支持范围。
   - 磁盘根还要 **GPT + ESP** 布局：内核探测磁盘时找的是 "AHCI/IDE/NVMe GPT ESP/root
     filesystem"，裸 ext2 镜像（无分区表）不会被当成根（只有 ramdisk 模块那条路不需要）。
2. **GRUB cmdline 要给 `mode=live`（或 `mode=installer`）**：内核只在
   `cmdline_has("mode=live"/"mode=installer")` 时才去挂 installer-root，不给的话驱动目录
   永远"不存在"（`[driver] no /usr/lib/leonos/drivers directory`）。
3. **installer-root 只有 ext2 或 FAT32 两种**（FAT16 不行）：`storage_mount_ramdisk_root()`
   按偏移 1080 的幻数 `0x53EF` 认 ext2，否则按 FAT32 解析；源码头注释写明"新介质用 ext2
   以保住大小写"（`/usr/lib/leonos/drivers` 大小写敏感）。
4. **子模块 `third_party/kconfig-frontends` 两个坑**：
   - git 版**不带** `libs/parser/hconf.gperf`（只有发布 tarball 才有），而
     `tools/build/kconfig-frontends.sh` 按"老修订有 gperf 输入"去校验它 —— 该修订其实把
     关键字表内联成了 `kconf_id.c` 的手写数组（`yconf.y` 直接 `#include`），放一个满足
     校验的桩即可（发行版脚本里会自动补）；
   - 在 Windows 侧 `git clone` 会带 CRLF，`./bootstrap` 直接 "not found"
     （shebang 成了 `#!/bin/sh\r`）—— 用 `-c core.autocrlf=false`，或在 WSL 侧统一
     `sed -i 's/\r$//'`（脚本里会检测并清理）。

版本号：NTCLKS 的版本工具只认 `kernel_name`/`release_version`，且后者严格
`major.minor.patch` —— 因此给它加了可选字段 **`release_suffix`**（校验：非空、≤32、
无空白/控制字符，拼进 `LEONOS_KERNEL_VERSION`）。于是内核自己报的是
`parlz-ntclks 4.7.2-K/RNT`；整机发行号 = 仓库 `.parlz-release` 的 ID 再加 `-K/RNT`
（**文件名**里 `/` 与 `\` 都换成 `-`，Windows 上 `\` 是路径分隔符）。ISO 里另放
`/parlz-release` 记录身份。

用户态契约：内核注册的是 **"Linux x86_64 syscall ABI"**，所以发行版的 `/sbin/init` 直接放
我们的静态 BusyBox；同时它启动时会**自己 mkdir** `include/leonos/layout.h` 那套目录
（`/run/leonos`、`/usr/lib/leonos/{drivers,apps,tests}`、`/var/lib/leonos` …），
`userland_prepare_runtime()` 失败就打印 `runtime directory initialization failed` 然后
`kernel_idle_loop()`（现象：内核日志刷完就静止）。

当前验证到的程度：UEFI → GRUB → loader → 内核（带 `-K/RNT`）→ 挂上 ext2 installer-root
（`fs=ext2`）→ 从 `/usr/lib/leonos/drivers` 加载 `serial.drv`/`e1000.drv`/`mouse.drv`
成功（`ac97.drv`/`es1371.drv` 在没挂声卡的 QEMU 里报 -19 属正常）。**尚缺**：让
`/sbin/init`(busybox) 真正跑起来 —— 卡在 `userland_prepare_runtime()` 的 mkdir 返回 -2
（我们的 mke2fs 镜像上），这是下一步要啃的点。

### 默认系统瘦身与 pm feed（2026-09-27 第三轮）

一条名单三处消费，别各写一份：`userland/pm-trim.list`。

- `gen-busybox-links.sh` 命中的 applet **不建软链**，实际裁掉的名字写进
  rootfs 的 `/etc/pm/trimmed-links`（形如 `bin/bc`、`usr/bin/dc`）。
- `build-userland.sh` 的 `[4.5/5]` 把名单里的 **userland 真二进制**移进
  `$US/trim-stage` 并从 rootfs 删掉，记进 `/etc/pm/trimmed-binaries`。
  `KEEP` 里保着 `install` —— 那是我们的装盘安装器，只是与 busybox 的
  拷贝 applet 同名（gen-busybox-links 的跨目录重名保护已经挡住了 applet）。
  `KEEP` 里还保着 `dpkg rpm apt yum` —— 名单里有 `dpkg`/`rpm` 这两个名字是给
  busybox applet 的历史条目，现在撞上了我们自己的包管理器：不保就把 `/bin/dpkg`
  `/bin/rpm` 裁走，`apt`/`yum` 找不到后端只能报错，而 `core.pm` 里又多一份同名包。
- `build-pm-feed.sh` 照这两份清单打 `core.pm`（软链 + 真二进制），
  写 `Packages` 索引，产出地即镜像站根目录。

三条必须守住的性质：

1. **裁的是命令名，不是能力**：`/sbin/busybox` 本体留在默认系统里，
   `busybox <命令>` 永远可用，启动脚本按绝对路径调它 → 裁剪不会让启动链路断。
   反过来说：`core.pm` **不含** `/sbin/busybox`，所以
   `pm remove core` 能干净退回瘦身前状态而不会把本体删掉。
2. **启动脚本不能裸用被裁的命令**：`tr`（读 install-done 标记）、
   `seq`（安装看门狗循环）踩过 —— 一律写 `/bin/busybox tr` 这种绝对调用，
   或干脆换成 shell 计数循环。
3. **名单里的 `[` 与 `[[` 不能用 `case *" $app "*` 判成员**：它们在模式里是
   字符类，会直接语法错。用逐词比较或 `comm` 比对。

自研 `parlz-sh` 的语法边界：**只差 `&&` / `$()` / `if`-`for` 这些真没做的**，
引号、分句、重定向、变量这些基本语义在 2026-09-28 都修对了（下面"parlz-sh 的
词法"一节）。写 guest 端断言仍要选"单条命令 + 输出里有确定串"的形态，且
**不能只 grep 路径名** —— 命令回显本身就含路径，会假通过（要匹配
`[0-9]+ /bin/curl` 这种带前导数字的行）。

**PM 的默认源永远是官方镜像站 `http://www.parlz.com/feed`**（用户 2026-09-28 明确
要求）。三处必须一致，改任何一处都要三处一起看：

| 位置 | 作用 |
|---|---|
| `/etc/pm/feeds.conf`（`build-userland.sh` 烘进盘） | 盘上的默认源 |
| `userland/init.c` 里的 `setenv("PM_FEED", …)` | **PM_FEED 环境变量优先于 feeds.conf**，所以这个默认值一写错，盘上那份就是摆设（踩过：它写着开发机 `10.0.2.2:8765`；注意正常启动是 busybox-init，这段只在 `rdinit=/init` 时生效，但默认值仍必须对） |
| `userland/pm.c` 的 `FALLBACK_BASES[]` 第一项 | feeds.conf 被删/写坏时 `pm install core` 仍能从官网装回来 |
| **包体里不许带 `feeds.conf`** | `build-pm-feed.sh` 打包 pm.pm 时**故意不收入** `/etc/pm/feeds.conf` —— 装包不得改写机器的源配置。以前它被打了进去，于是"任何一次旧包安装都会把源改回旧地址"（实测：官网 feed 里那份 9-27 的 pm.pm 带着开发机地址，`pm install pm` 之后 `pm install core` 就去连 `10.0.2.2:8765`）。脚本里有反向断言：pm.pm 里出现 `feeds.conf` 直接失败 |

**官网那棵 feed 是"部署物"，改了默认源/包结构后要重新部署**（`output/feed` →
站点根；本地自测用 `PM_REPO=output/feed sh scripts/pm-server.sh`）。改完先跑
`scripts/pm-site-verify.sh`，它会分别断：盘上 feeds.conf、`pm install pm` 的下载源、
`pm install core` 的下载源、以及 `output/feed/pm.pm` 里**不含** feeds.conf。

站点走 `http→https` 301，所以 guest 必须**带 TLS**（`scripts/build-openssl.sh`
产出的 stage 在 `build-userland.sh` 里被 curl/wget/pm 自动链接；缺了它们会静默退回
明文 HTTP，官网就装不了）。端到端验收：`scripts/pm-site-verify.sh`（断盘上
feeds.conf、断 PM_FEED 没被开发机地址盖住、真从官网 `pm install pm` 并比对版本号）。
换源只走 `parlz.feed=` cmdline 或用户自己 export PM_FEED。

PM 自己的版本号与 ParlzOS 的号**各自独立**：`pm+<大>.<小>-<阶段>+<第几版>`，
阶段 `R`/`RC`/`B`/`A`（例 `pm+1.1-RC+1`）。来源是仓库根 `.pm-release`，由
`make-release.sh` 生成 → `build-userland.sh` 经 CMake
`-DPARLZ_PM_VERSION` 编进 `/bin/pm`（`pm --version` 打印）→ `build-pm-feed.sh`
拿它写 `Packages` 的版本列。发布的第 5 步会在 guest 里断言 `pm --version`。

`pm.c` 装包的两个硬规则（否则"装得回来却跑不了"）：覆盖普通文件前
**先 `unlink` 再 create** —— 就地 `O_TRUNC` 一个正在运行的可执行文件会
`ETXTBSY`（`pm install pm` 自更新必撞）；写完要**显式 `chmod`** ——
`O_CREAT` 的 mode 只在创建时生效，就地覆盖会留下旧文件的 0644，
装回来的 `/bin/nano` 就没执行位。

### 包管理器：dpkg / apt / rpm / yum（2026-09-30 换成自研移植）

上游 OPKG 与它的 `ppm`/`opkg` 委托层**已移除**（源码留在
`userland/legacy-backup/*.disabled-20260930`，构建脚本在
`scripts/legacy-backup/`，都没直接删）。换成的四个都是自己写的静态二进制，
共用一份 `userland/pkgcore.c/h`：ar / ustar+GNU+pax tar / newc cpio 的读、
gzip 解压（`miniz_tinfl.c`）、安全落盘、Debian 与 RPM 两套版本比较、SHA-256。

分工：**底层**（`dpkg`、`rpm`）负责解包落盘与状态库，**高层**（`apt`、`yum`）
只负责源、索引、依赖求解、下载校验，最后 `fork+execv` 调底层（`apt` 用
`--dpkg`、`yum` 用 `--rpm` 指路径，测试才指得到自己编的那份）。状态库是纯文本：

| 管理器 | 状态库 | 备注 |
|---|---|---|
| `dpkg` | `/var/lib/dpkg/status` + `info/<pkg>.{list,control,md5sums,conffiles,preinst,postinst,prerm,postrm}` | 卸载按 `.list` 反序删；`-P` 才清配置与条目 |
| `rpm` | `/var/lib/rpm/installed/<NVRA>.{meta,list}` + `scriptlets/<NVRA>.{prein,postin,preun,postun}` | 卸载脚本**装时落库**，否则 `-e` 时无处可寻 |

九条实测出来的必踩点：

1. **`.deb` 是 ar 档**：成员名在 GNU ar 里带 `/` 后缀（`control.tar.gz/`），比较
   名要先剥；成员 2 字节对齐。只支持 gzip 与不压缩 tar —— `.xz`/`.zst` 必须在
   **动手解之前**判掉，否则"不是 gzip 就当未压缩 tar 往下走"，最后的报错变成
   看不出根因的 `control.tar 里没有 control 文件`。
2. **tar 头**：`mode` 在 100、`size` 在 124（GNU base-256 时最高位为 1，按大端
   二进制读）、`typeflag` 在 156、`linkname` 157、ustar `prefix` 345 要拼回名字。
   写成员时**别漏 `m.mode = mode`** —— 漏了就把 0755 的脚本装成 0644，
   而"文件内容对、权限错"这种包最容易只测内容。
3. **cpio newc**：头 110 字节，`mode`@14 / `filesize`@54 / `nlink`@38 /
   `namesize`@94；名字紧跟头并按 4 补齐、数据再按 4 补齐。成员名 `./usr/...`
   归一时**要把 `./` 与后面多余的 `/` 一起跳过** —— 老的 `memmove(name, name+2)`
   把前导 `/` 留了下来，于是 `rpm -qpl` 打出 `usr/...`（缺斜杠）而安装没事。
4. **RPM 头**：lead 魔数是 `ed ab ee db`；每段 16 字节 = 魔数(4) +
   **reserved(4)** + 条目数(4) + 值区长度(4)。把 reserved 当条目数读会得到
   `nindex=0`，整段解析静默变空。索引项 = tag/type/offset/count(各 4，大端)，
   `offset` 相对**值区起点**，整数数组还要按类型对齐（int16→2、int32→4、int64→8）。
   type 语义：`6=串`、`7=BIN`、**`8=串数组但 count 是字节数`**（按 count 次数取
   名字只会捞到第一个）、`9=i18n 数组(count 是串数)`。
   **负载紧跟主 header 值区末尾，不补齐** —— 只有"签名段→主 header 段"之间才
   8 字节对齐；多补一次就会把 gzip 流头几字节切掉，报出
   `负载标称 gzip 但没有 gzip 魔数(文件被截断?)` 这种看着像包损坏的假错。
5. **标签号别靠记忆**：`scripts/rpmhdr.py` 把真包的标签表全打一遍，配
   `rpm -qp --qf '%{NAME}...'` 现核。已核实：1000/1001/1002 = NAME/VERSION/
   RELEASE、1004/1005 = SUMMARY/DESCRIPTION、1009 SIZE、1014 LICENSE、1022 ARCH、
   1023..1026 = PREIN/POSTIN/PREUN/POSTUN、1085..1088 是它们的解释器、
   1116/1117/1118 = DIRINDEXES/BASENAMES/DIRNAMES、1124..1126 负载格式。
   文件清单不靠这些标签，**直接取负载 cpio 的成员**（那才是"包里到底有什么"）。
6. **造 fixture 的两个技巧**：`rpmbuild --target i686` 在 x86_64 宿主上直接
   `No compatible architectures found for build`，所以"架构不符"的包用
   `scripts/rpmsetarch.py` 把真包的 ARCH **等长原地替换**（产物仍是真包；注意
   上游 `rpm` 会因为 header 摘要不符而拒绝读它 —— 这条正好当反向证据，
   也说明本实现"不验摘要"是明说的差别）。`dpkg-deb` 默认写 xz，测试要显式
   `-Zgzip`，另留一份 `-Zxz` 验证"拒绝而不是当空包"。
7. **stanza 切分不要用 `strtok("\n\n")`**：分隔字符集里的两个 `\n` 会并成一个，
   于是切出来的是**行**不是段 —— dpkg 的 status 变成只剩 `Package: x` 一行，
   Version/Status 全丢，症状是 `-l` 没有 `ii`、`-r` 认为没装过、一个文件都不删。
   `apt`/`yum` 读索引同理，都自己按"空行"逐行切。
   另一个同族错：`fld()` 已经吃掉了冒号后的空格，再去找 `" install ok installed"`
   （带前导空格）永远匹配不上，表现成"刚装完就 remove 却说没装过"。
8. **落盘三条硬规则**（与 `pm` 同源）：覆盖前先 `unlink` 再 create（就地
   `O_TRUNC` 正在运行的可执行文件 = `ETXTBSY`）、写完**显式 `chmod`**、
   成员名逐组件拒绝 `..`。而且"失败要干净"：`preinst`/`%pre` 非 0 或摘要不符时
   必须一个文件都不落、状态库里不写条目 —— 验收里这类反例是必需的，
   否则"解不动就当空包"的偷懒实现照样全绿。
9. **信任闸门与完整性**：四个管理器**都不验 GPG**（没实现，不暗示支持）。
   替代手段是显式开关：apt 需要源行 `[trusted=yes]` 或 `--allow-unauthenticated`，
   yum 需要 `gpgcheck=0` 或 `--nogpgcheck`，否则拒绝安装。下载一律按索引声明的
   `Size` + `SHA256` 核对，不符就删缓存且不交给底层。SHA-256 是自己实现的
   （`pkgcore.c`），K 表曾经抄错一个字节（`0xe9b5c5a5` 应为 `0xe9b5dba5`）——
   靠 `scripts/sha256-consts-check.py`（从素数立方根现算 K/IV）与
   `apt-verify.sh` 里"对 `sha256sum` 逐字节比 13 种长度"这两道抓住。

**apt/yum 默认没有任何源**：官网 `www.parlz.com/feed` 是 `.pm` 格式，不是
deb/rpm 仓库，硬指过去只会"取不到索引"。`/etc/apt/sources.list` 与
`/etc/yum.repos.d/README` 里只放注释示例。`pm` 的默认源仍然是官网镜像站，
这条不许动（见上一节三处一致的要求）。

验收（都在宿主秒级/分钟级，不起 QEMU；判据的对照物是上游工具）：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/dpkg-verify.sh <dpkg 路径>"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/rpm-verify.sh  <rpm  路径> <dpkg 路径?>"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/apt-verify.sh  <apt 路径> <dpkg 路径>"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/yum-verify.sh  <yum 路径> <rpm 路径>"
```

它们用 `dpkg-deb`/`apt-ftparchive`/`rpmbuild`/`createrepo_c` 造真包真仓库，
再拿宿主 `rpm -qpl`、`rpm -qlp`、`sha256sum` 的输出与我们的逐行 `cmp`。
脚本跑在 `sh`(dash) 上：**不许出现 `<(...)`**（会被 eval 报语法错，实测踩过）。
注意这些脚本会 `rm -rf` 自己的 `/tmp/parlz-*-test` 目录，改判据前先停后台任务。

### 工具链包（gcc.pm / clang.pm）的版本探测

`build-toolchain.sh` 与 `build-pm-packages.sh` 以前把 `gcc-15`/`llvm-21`
焊死在脚本里（那是旧 WSL 26.04 实例的包），换到 24.04 直接产不出包。
现在两头都自动探测：

- `build-toolchain.sh`：`GV` 取 `/usr/lib/gcc/x86_64-linux-gnu/<纯数字>` 最新版，
  `LV` 取 `/usr/lib/llvm-<N>` 最新版；目录名、`G15/L15` 路径、自检全部跟版本走。
  24.04 上是 **gcc-13 + llvm-18**（llvm 需 `apt-get install -y clang llvm`）。
- `build-pm-packages.sh`：从 `toolchain-pack/opt/` 反推 `GV/LV`，包名成
  `gcc-<GV>.pm` / `clang-llvm-<LV>.pm`，manifest 的版本列取
  `gcc -dumpversion` 与 `clang --version` 的实际输出。
- guest 侧不写死路径：`parlz-boot-body.sh` 按 `/opt/toolchain/gcc-*`、
  `llvm-*`、`/usr/include/c++/*` 探测后再设 `CPATH`/`LD_LIBRARY_PATH`/`C_INCLUDE_PATH`。
- `build-userland.sh` 的工具链打包段同样用 `$TCG_NAME/$TCL_NAME` 探测；
  它的 `ld.so.conf` heredoc 必须保持**不带引号**的 `<<TCEOF`，否则变量不展开、
  写进盘的是字面 `$TCG_NAME`。
- 默认 **不**把工具链烘进 initramfs（`PARLZ_TC_BUNDLED=1` 才打），装它走
  `pm install gcc` / `pm install clang`。

### 工具链"动态编译"的七个真坑（2026-10-01 实测）

`pm install gcc/clang` 之后 guest 里 `gcc a.c` 到底能不能编出**能跑**的动态产物，
取决于下面这些 —— 每一条都有对应判据，改打包链先跑
`scripts/toolchain-dyn-verify.sh`（把**真包** cpio 解进空目录当 guest 根、
`chroot` + `env -i` 裸敲四个编译器，判据取产物运行输出与 `readelf` 的
`DT_NEEDED`/`PT_INTERP`）。宿主自检看不到这些坑，**因为宿主自己什么都有**。

1. **`-lc` 找的是 `libc.so` 这个名字，不是 `libc.so.6`**。包里以前只有运行时的
   `libc.so.6` 与静态的 `libc.a` → ld 静默落回 `libc.a`：产物带着 `-pie` 的
   `Scrt1.o` 和 `PT_INTERP` 却是静态 glibc，**一跑就 SEGV(rc=139)**，
   `dlopen` 那条还会先打 "Using 'dlopen' in statically linked applications"。
   所以 `libc.so`/`libm.so`/`libstdc++.so`/`libgcc_s.so`/`libpthread.so`/
   `libdl.so`/`librt.so` 这些**不带版本的 dev 名字是动态编译的开关**，
   现在由 `build-toolchain.sh` 的 dev 层生成（内容一律写相对名
   `GROUP ( libc.so.6 libc_nonshared.a AS_NEEDED ( /lib64/ld-linux-x86-64.so.2 ) )`，
   绝对路径只能对宿主或 guest 一边）。
2. **`ld.so` 没有 `/etc/ld.so.cache`（guest 里就没跑过 ldconfig）**，它只认编译进
   自己的默认目录：`/lib`、`/usr/lib`、`/lib64` 加 multiarch 的
   `/lib/x86_64-linux-gnu`、`/usr/lib/x86_64-linux-gnu`。所以包里那几棵
   **软链树**才是"不设 `LD_LIBRARY_PATH` 也能跑"的关键；`/etc/ld.so.conf.d/*.conf`
   在这台机器上是纯装饰（没有读者）。boot 脚本导出的 `LD_LIBRARY_PATH`/`CPATH`
   从此只是给旧镜像兜底，**不许**把它当修好的证据。
3. **`cp -a` 把宿主的相对软链原样搬进包树就断了**。Ubuntu 的
   `/usr/lib/gcc/x86_64-linux-gnu/<V>/libstdc++.so` 是
   `../../../x86_64-linux-gnu/libstdc++.so.6`，`libgomp.so`/`libasan.so` 同理 →
   包内变悬空：`-lstdc++` 找不到、`-fopenmp`/`-fsanitize=address` 全废，
   而且对悬空链做 `>` 重定向会**直接 ENOENT 把构建打死**（第一版就死在这）。
   正解：在宿主上把链解引用到真 soname、把**档案**收进 `lib/`、包内改成
   指向 guest 落点的绝对单跳链或相对名 GROUP。
4. **悬空软链要在打包装箱时挡**。`build-pm-packages.sh` 的 `no_dangle` 按
   **guest 视角**（把链目标拼到包根下）解析 `usr/bin` `bin` `lib*`
   `usr/lib/{gcc,x86_64-linux-gnu}` 与包内 `opt/*/{bin,lib*,usr/lib/*}`。
   这一闸门抓出的第一个真 bug 就是 `/usr/bin/g++` 指向一个没被打进包的档案
   —— 现场表现是 guest 里 `g++: not found`，看着像"包没装上"。
   宿主的 `test -e` 测不出来（链目标是 guest 路径，宿主上另有个同名真目录）。
5. **`g++`/`cc1plus` 得有出处**：宿主不装 `g++` 就没有 `cc1plus`，
   `c++` 会被 alternatives 指到 clang 上（包里那个 140 KB 的 `c++` 其实是
   clang++），于是"C++ 支持"根本不存在。`libstdc++-*-dev` 也要装，
   它给的是 `<gcc 私目录>/libstdc++.{a,so}`，**不在** `/usr/lib/x86_64-linux-gnu/`。
6. **包内每个可执行/共享库的每条 `DT_NEEDED` 都得在包里有档案**。
   `build-toolchain.sh` 结尾两条自检就是断这个（依赖闭包补齐 + 缺档即失败）；
   抓到的例子：后加入的 `readelf` 需要 `libctf-nobfd.so.0`，而 ldd 收集发生在
   它被拷进包**之前** → guest 里 `readelf` 一起来就
   `cannot open shared object file`，验收套件的 9 条 readelf 判据全红。
   顺手剪掉非 x86_64 的 clang 运行时（`*-i386.so` 要的是 i386 的
   `ld-linux.so.2`，在纯 x86_64 的内核上是死档，留着只会报假红）。
7. **`cpio` 的完整性判据不能用退出码**：被截断的归档 `cpio -t`/`-i` **仍返回 0**，
   只在 stderr 打 `premature end of file`；而成功时它也往 stderr 打
   `N blocks` → 判错要认 `cpio:` 前缀，认"stderr 非空"会把好包判死。
   另外 `cpio -t` **不打印 `TRAILER!!!`**（以前 `grep -cv '^TRAILER'` 是空操作）。
   真踩过：1.5 GB 打包刚结束 WSL 实例被重启，页缓存里的全丢了，`gcc-13.pm`
   从 546 MB 变 60 MB、`Packages` 变 0 字节，而日志当时已经打过"546M"。
   现在 `pack_pm` 写完 `sync` + 比对成员数与源树 + 认 `cpio:` 报错。

**验证链**：`build-toolchain.sh`（两条闭包自检）→ `build-pm-packages.sh`
（悬空链闸门 + 归档完整性）→ `scripts/toolchain-dyn-verify.sh`（真包 chroot，
动态/静态/`-fPIC -shared`/`dlopen`/C++ 异常+线程/`-no-pie` 全判终态）→
`scripts/pm-verify.sh`（真 QEMU 里 `pm install gcc clang` 后裸命令编译运行）。
`userland/tooltest.sh.in` 与 `scripts/pm-verify.sh` 里那些 `-l:libc.so.6`
`-Wl,-rpath` `--gcc-toolchain=` 的补丁已经删掉：**带着补丁测出来的"能编译"
证明不了包自包含**。

两条与"注入式自测脚本"有关的规矩（都踩过）：

- **钩子在 `userland/parlz-boot-body.sh` 里执行**（`run_hook /tooltest.sh 900`
  与 `run_hook /pmtest.sh 1500`），不在 `init.c` 里。正常启动是 busybox-init +
  body（rootfs 里没有 `/init`，内核回落 `/sbin/init`），`userland/init.c` 只是
  busybox 缺位时的兜底 PID1 —— 以前钩子只写在 init.c，body 那侧打一句
  `"present (run manually)"` 就完了，于是 `pm-verify.sh` / `toolchain-verify.sh`
  **从切 busybox-init 起就不可能通过**（跑满超时、日志里只有那句 announce）。
- **guest 侧判据不许依赖被裁的命令**：`pm-trim.list` 里有
  `head tail wc sed sort uniq strings file od stat tr seq cpio timeout env …`，
  它们在 `pm install core` 之前**不存在**。用 `$(head -2 f)` 取错误信息会得到
  空串，FAIL 就退化成"rc=1，什么也没说"；`[ "$(wc -c < f)" -gt 20 ]` 会因空串
  比较而假红。诊断用 `cat`，存在性用 `[ -s ]`/`[ -x ]`，计时用
  `/bin/busybox sleep`。同理 `cp dir/`、`ln -sf` 这类自研命令的语义不要混进
  工具链判据 —— 要拷进目录就直接 `-o 目标全路径`。

### 交付物的许可证与源码义务（2026-10-02）

授权是**分层**的，不要"一键换成 PARLZ.LICENSE"：内核部分（`linux-7.2.5/` 整树，
**包括** Parlz 加进去的 `include/linux/parlz.h`、`arch/x86/kernel/parlz.c`、
`init/main.c` 的一行钩子、`setup.ld` 填充修复、`parlz_defconfig`）始终是
**GPL-2.0-only**，不能被重新授权；PARLZ.LICENSE 只覆盖内核之外的自有部分。
仓库根 `LICENSE` 是这份分层说明，`PARLZ.LICENSE` 第 1.1.1 / 12.2.1 / 12.3.1 条
（中英文各一份）写的就是这条边界。

分发 ISO/IMG = 分发内核目标代码，所以介质上必须能翻出许可证文本。落点：

| 位置 | 内容 | 谁负责 |
|---|---|---|
| FAT16 引导分区 `LICENSE.TXT` + `COPYING.TXT` | 分层说明 + GPLv2 全文（vmlinuz 就在同一分区） | `scripts/gen-fatboot.sh`（带挂载复核） |
| rootfs `/usr/share/licenses/<组件>/` | 10 个上游件 + `parlz/{PARLZ.LICENSE,LICENSE}` + `README` 索引 | `scripts/build-userland.sh`（缺目录即失败） |
| rootfs `/LICENSE.TXT` | 介质副本说明（ISO 根与已安装根都有它） | 同上，取自 `third_party/licenses/PARLZ-MEDIA-LICENSE.txt` |
| `/etc/parlz-release` | `license:` / `license-dir:` / `source-url:` 三行 | 同上 |

四条实测出来的坑：

- **许可证文本要从仓库取**，不要在构建时现拷宿主的 `/usr/share/common-licenses` ——
  换一台机器就悄悄少文件，而"没随二进制复现许可证"是 GPLv2 §3 的硬违规，
  不会有任何报错。文本由 `scripts/vendor-licenses.sh` 一次性收进
  `third_party/licenses/`，并**打印出** `web/system.js` 里 `LICENSES` 字面量该写的字节数。
- **改了 rootfs 布局必须走 `build-userland.sh` 全链**：`build-2404.sh` 那条快速路
  只是把 `$U/root` **现有内容**重新 cpio 一遍，不会执行 build-userland 里的定制步骤
  （许可证拷贝、`/etc/parlz-release` 生成都在那里）。实测：快速路出来的盘
  `/usr/share/licenses` 里只有 bash 一份、`/LICENSE.TXT` 不存在，而构建照样报
  `BUILD_2404_DONE` —— 看着绿，其实交付物是旧的。
- **字节数 ≠ 字符数**：文本里有中文，JS 的 `String.length` 是 UTF-16 码元
  （介质说明 1130），盘上是 UTF-8 字节（1772）。演示里登记的尺寸判据要用
  `Buffer.byteLength` 与 `fs.statSync().size` 比，用 `length` 会假红。
- **演示站的 `LICENSES` 条目必须逐条对仓库真文件**（`scripts/web-demo-test.js`
  里"授权:"那批判据）。手写一份字节数一定会漂：bash 那份早先手工存的 GPLv3
  与 Ubuntu 的那份差 2 字节，判据当场抓出来。

### 装盘验收的判据（别再被假绿骗）

`pivot_root OK` / `Parlz boot ready` / `Username:` 都**不能**证明装好的系统能用。
`scripts/verify-user-install.sh` 的四阶段判据是：装盘日志里有 `rootfs copied`
（cpfs 真成功，不是兜底）→ 磁盘自启换根 → 首启建用户后**已安装根里的 shell
能跑命令**（`echo installed-root-ok $USER` 回显 `installed-root-ok tester`，
顺带证明换过去的是可写的真盘根）→ 宿主把分区 2 挂回来核对文件/软链/权限
（`scripts/check-installed-root.sh`）。
改 cpfs / install / 引导链之后，至少跑这两条秒级宿主检查，再跑 QEMU：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/cpfs-oracle.sh"
#   解包 initramfs 当源根 -> mkfs+cpfs 到镜像 -> e2fsck + 挂载逐条比对
#   (路径集合/软链目标/可执行位/挂载点未递归)。比 QEMU 快得多, 先过这关
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/verify-user-install.sh"
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/iso-disk-e2e.sh"
```

磁盘自启的 `syslinux.cfg` 故意**不带** `login.skip`（真机首启就该设密码），
所以自动化脚本得自己把凭证喂进串口 —— `scripts/guest-first-login.sh` 配合
`qemu -nographic < 命名管道` 做这件事（等 `Username:` 出现再写 用户/密码/确认）。

两个跟"断电"有关的验收细节（2026-09-28 补）：

- 阶段 2 是**直接 kill QEMU**（等价拔电源），所以喂完最后一条断言要先让 guest
  `sync`（`verify-user-install.sh` 里 `/bin/busybox sync` + `echo SYNCED` 等回显）
  再 kill —— 不给的话宿主侧阶段 3 看到的是**页缓存没落盘的旧盘面**（踩过：刚写的
  `/etc/parlz-auth` 在宿主复核时"不存在"，看着像凭证没写成）。
- `check-installed-root.sh` 的 e2fsck 判据**放行计数类告警**
  （`Free inodes/blocks count wrong (…, counted=…)` + `Fix? no`）—— 那是被 kill
  的 guest 没把超级块计数写回的必然结果，`e2fsck -p` 一条就修好、不丢数据；
  其余任何输出（位图差异、链接数错、目录项错）**仍然算问题**，别把这条放行扩大化。
  它同时断言"`/etc/parlz-auth` 只要在盘上就必须是 `$6$` 散列"（明文回归的直接判据）。



