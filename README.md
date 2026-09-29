# Parlz

一个基于 **Linux 7.2.5 内核源码就地改造**而成的操作系统。
Parlz 直接修改 Linux 源码（不是参考、不是旁挂），在启动路径、标识层和构建配置上
注入自己的身份，编译为可在 QEMU 中运行的 x86_64 内核，并配有自研的静态用户空间、
文件系统工具和命令行安装盘。

## 当前状态

| 能力 | 状态 |
|------|------|
| 内核编译（x86_64，GCC 15） | ✅ 通过 |
| QEMU 启动到命令行 shell | ✅ 通过 |
| 源码标识层（`Parlz 0.1.0 on x86_64`） | ✅ 通过 |
| ext4 / iso9660 / vfat 文件系统支持 | ✅ 已启用并验证 |
| virtio 磁盘识别（`/dev/vda`） | ✅ 通过 |
| MBR 分区表写入（`0x55AA` + 类型 `0x83`） | ✅ 通过 |
| 内置 ext2 格式化（`mkfs`，`dumpe2fs` 校验通过） | ✅ 通过 |
| 安装盘 `parlz-install.iso` | ✅ 已生成（可 dd 到 U 盘 / 刻盘在真机引导） |
| 安装到磁盘 + 从磁盘自启 | ✅ 全链路通过（MBR→VBR→SYSLINUX→内核→挂 `/dev/vda2`→`pivot_root` 换真根→首启建用户→已安装根里的 shell 可用；`scripts/verify-user-install.sh` 四阶段 PASS） |
| 装过去的根完整（384 条软链 + 挂载点不递归） | ✅ `scripts/cpfs-oracle.sh`（宿主秒级）+ `scripts/check-installed-root.sh`（挂回分区 2 复核） |
| 发布（rc）与版本同源 | ✅ `CST+0800+<构建时间>+jgzyes@parlz.com+rc.0.1-F\V`（VGA+串口双路）—— `/proc/version`、`uname -r`、启动横幅、`/etc/parlz-release`、`pm --version` 同源，产物在 `output/` |

已实测的启动输出：

```
Parlz 0.1.0 on x86_64 (Linux 7.2.5 base)
Linux version 7.2.5 (jgzyes@JGZYES) ... #9 PREEMPT_DYNAMIC
Unpacking initramfs...
devtmpfs: initialized
Run /init as init process
=== Parlz user-space ===
Parlz shell 0.1 - 'help' for commands, 'exit' to quit
parlz>
```

## 快速开始

前置：Windows + WSL2，发行版 `Ubuntu-24.04`（2026-09-25 起；旧 `Ubuntu-26.04`
虚盘损坏已注销），已装
`gcc make bison flex cpio bc libelf-dev libssl-dev xorriso qemu-system-x86 cmake`
以及构建引导/安装链需要的
`syslinux dosfstools mtools bzip2 libncurses-dev libarchive-dev pkg-config`
（完整清单与说明见 [AGENTS.md](AGENTS.md)）。
内核源码已复制到 WSL 本地盘：

```bash
cp -a /mnt/f/Linux/Parlz/linux-7.2.5 /home/jgzyes/parlz-kernel
```

### 构建

```bash
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/build-2404.sh
```

或在 Windows 资源管理器里双击 `build.bat`。产物在 `images/`。

### 运行

```bash
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot.sh
```

双击 `run.bat` 等效。退出 QEMU 按 `Ctrl-A` 再按 `X`。

### 安装到磁盘

**方式一：进系统里自己装**（推荐先玩这个 —— 想要哪块盘、什么时候装由你定）

```bash
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-install.sh
```

`boot-install.sh` 会挂进来两块盘：目标盘（guest 里是 `/dev/vda`，可写）+ 引导
镜像（`/dev/vdb`，只读），cmdline 带 `install.skip=1`（**不自动装**）。进去后是
普通 shell，自己敲：

```
install             # 自动探测第一块可写盘(排除只读的 /dev/vdb)并安装
install /dev/vda    # 或显式指定目标盘
reboot              # 装完重启, 本次会话直接进入装好的系统
```

install 的六步输出：写 MBR → 写引导分区（syslinux VBR + `ldlinux.sys` +
`vmlinuz`）→ 分区 2 建 ext2 → `cpfs` 拷入整个 rootfs（含 384 条软链，
挂载点只建空目录）→ 写 install-done 标记 → `=== install complete ===`。
目标盘若正挂着当根（`/dev/vda1`、`/dev/vda2` 在 `/proc/mounts` 里），
`install` 直接拒绝 —— 不能边跑边重写自己脚下的盘；重装请从安装介质启动。

引导镜像（FAT16 引导分区的内容）不嵌进 initramfs（会自引用膨胀），所以
**装盘的那个会话必须能取到它**。哪次会话有介质：

| 启动方式 | 镜像来源 | 能装？ |
|---|---|---|
| `install.bat` / `scripts/boot-install.sh` | 只读盘 `/dev/vdb`（cmdline `parlz.bootimg=/dev/vdb`） | ✅ 推荐 |
| `scripts/boot.sh` | 同上（`PARLZ_NO_IMG=1` 时没有） | ✅ |
| `install.bat` 用 ISO / `run-iso.sh` | ISO 挂到 `/cdrom`，取 `/cdrom/boot/fat16.img` | ✅ |
| `run.bat` / `boot-disk.sh` | 无介质（这就是"真机按下电源键"的自启验收） | ❌ |

取不到镜像时 `install` 会打一行当前块设备清单（形如
`当前块设备: sr0(1023 MiB, 只读) vda(512 MiB)`），据此即可判断是"没挂第二块盘"
还是"设备在但节点没出来"。

**方式二：用 ISO 当安装介质（全自动）**

```bash
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/run-iso.sh   # ① 挂 ISO+空盘, 自动装
wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-disk.sh # ② 从磁盘启动(纯 MBR→VBR→syslinux)
```

Windows 侧双击：`install.bat` = 方式一（进系统里自己装），`run.bat` = 从装好的盘启动。
方式二① 没有 .bat，直接跑 `run-iso.sh`。
默认磁盘是 `/home/jgzyes/parlz-disk.img`（512 MiB，`PARLZ_DISK` 改路径，
`PARLZ_DISK_MB` 改大小，`PARLZ_FRESH=1` 从零重装）。方式二启动后内核会挂载
`/dev/vda2` 并 `pivot_root` 到**磁盘上装好的根**，然后进入登录。

`images/parlz-install.iso` 也可以直接 `dd` 到 U 盘 / 刻盘，在真机上引导安装。

`images/parlz-installed-disk.img` 是**已经装好的整盘镜像**（512 MiB：MBR +
FAT16 引导分区 + ext2 根 + 全量 rootfs），拿去就能启：

```bash
qemu-system-x86_64 -m 1024M -nographic \
  -drive file=images/parlz-installed-disk.img,if=virtio,format=raw -boot c
```

必须用 **virtio** 挂（盘里的 `syslinux.cfg` 写死 `root=/dev/vda2`，`-hda`
会变成 `/dev/sda` 而挂不上根）。`dd` 到真盘同理：
`dd if=images/parlz-installed-disk.img of=/dev/sdX bs=4M conv=fsync`。
账号是装盘验收时建的 `tester / parlz123`；要换成自己的盘就用 `install.bat`
（或 `PARLZ_FRESH=1 … boot-install.sh`）重装，装好的盘再用
`cp /home/jgzyes/parlz-disk.img images/parlz-installed-disk.img` 更新这份产物。

非交互验收脚本（自动跑完并打印 PASS/FAIL）：

```bash
# 秒级(不用 QEMU): 解包 initramfs 当源根 -> cpfs 到镜像 -> e2fsck + 挂载逐条比对
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/cpfs-oracle.sh"
# 在系统内装盘 -> 磁盘自启 -> 首启建用户 -> 已安装根 shell 跑命令 -> 宿主复核根完整性
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/verify-user-install.sh"
# ISO 全自动装盘 -> 磁盘自启(同一套判据)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/iso-disk-e2e.sh"
# 只复核"装到磁盘的那个根"(可用于任何一块装过的盘)
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/check-installed-root.sh /home/jgzyes/parlz-disk.img"
```

## 目录结构

```
Parlz/
├── linux-7.2.5/                     # Linux 源码（已就地改造为 Parlz）
│   ├── include/linux/parlz.h        #   新增：Parlz 标识头
│   ├── arch/x86/kernel/parlz.c      #   新增：标识层实现
│   ├── arch/x86/kernel/head64.c     #   修改：start_kernel() 前调用 parlz_boot_init()
│   ├── arch/x86/kernel/Makefile     #   修改：obj-y += parlz.o
│   ├── arch/x86/configs/parlz_defconfig  # 新增：Parlz 专用 defconfig
│   └── arch/x86/boot/setup.ld       #   修改：.bstext 填充（GCC 15 修复）
├── userland/                        # 用户空间（CMake + GCC，静态链接）
│   ├── init.c                       #   PID 1，挂 proc/sys/devtmpfs，检测 /install.d
│   ├── sh.c                         #   最小 shell（内建 + execvp 回退 + 脚本/-c 模式；/bin/bash 软链到它）
│   ├── mkfs.c                       #   内置 ext2 格式化
│   ├── install.c                    #   安装器（MBR + 分区注册 + 拷贝 + 引导扇区）
│   ├── fdisk.c / mknod.c            #   MBR 创建 / 设备节点创建
│   ├── cat.c dmesg.c free.c mount.c umount.c boot.c
│   └── CMakeLists.txt
├── scripts/                         # 全部构建/运行脚本
│   ├── make-release.sh              #   出发布: 生成发布号 → 全链构建 → 自检 → 收进 output/
│   ├── build-userland.sh / build-kernel.sh / build-iso.sh   # 全量构建三步
│   ├── run-iso.sh / boot-disk.sh    #   安装到磁盘 / 从磁盘启动
│   ├── verify-user-install.sh / iso-disk-e2e.sh / disk-e2e.sh  # 安装链验收
│   ├── cpfs-oracle.sh / check-installed-root.sh  #  宿主侧秒级复核(不起 QEMU)
│   ├── ifc-verify.sh / ctrl-c-verify.sh / job-control-verify.sh / login-pty-test.py
│   │                              #   配网(含 ifc dhcp)/ Ctrl+C / 终端前台权 / 登录认证
│   ├── build-uefi-iso.sh / uefi-e2e.sh
│   │                              #   UEFI 那条引导路(GRUB 双固件 ISO)与它的端到端验收
│   ├── gen-efi-esp.sh             #   FAT ESP 镜像(syslinux.efi + 内核),磁盘 UEFI 用
│   ├── build-2404.sh / console-cfg.sh / serial-media.sh
│   ├── boot.sh / boot-verify.sh / gen-fatboot.sh
│   ├── guest-first-login.sh         #   验收脚本替 guest 完成首启设密码
│   └── sync-setup.sh / mkfs-test.sh #   工具链修复 / mkfs 诊断
├── images/                          # 产物
│   ├── parlz-bzImage                #   内核
│   ├── parlz-initramfs              #   用户空间 cpio.gz
│   ├── parlz-bootfat.img            #   引导分区镜像(64 MiB FAT16, 含 syslinux+vmlinuz)
│   ├── parlz-install.iso            #   安装盘
│   └── parlz-installed-disk.img     #   已装好的整盘(512 MiB): MBR+引导分区+ext2 根
├── output/                          # 发布产物(按发布号命名 + VERSION.txt + SHA256SUMS)
│   ├── parlz-rc.0.1-<构建时间>-{bzImage,initramfs,bootfat.img,install.iso,installed-disk.img}
│   ├── VERSION.txt                  #   发布号与各字段(时区/构建时间/构建人/阶段.大.小)
│   ├── RELEASE-NOTES.md             #   这一版有什么、怎么跑、已知限制
│   └── SHA256SUMS.txt
├── build.bat / install.bat / run.bat # Windows 一键: 构建 / 装盘 / 从盘启动
├── AGENTS.md                        # 面向 AI 代理的工程说明
└── README.md
```

## 内核改造点

| 文件 | 改动 |
|------|------|
| `include/linux/parlz.h` | 新增。`parlz_banner`、`parlz_running()`、`parlz_boot_init()` |
| `arch/x86/kernel/parlz.c` | 新增。打印 `Parlz 0.1.0 on x86_64` |
| `arch/x86/kernel/head64.c` | 修改。`start_kernel()` 前插入 `parlz_boot_init()` |
| `arch/x86/kernel/Makefile` | 修改。`obj-y += parlz.o` |
| `arch/x86/configs/parlz_defconfig` | 新增。串口控制台、devtmpfs、ext4/iso9660/vfat、virtio 磁盘 |
| `arch/x86/boot/setup.ld` | 修改。`.bstext` 填充 `0xffffffff` → `0x00` |

`vmlinux` 中可验证的标识符号：`parlz_banner`、`parlz_boot_init`、字符串 `Parlz 0.1.0`。

## 用户空间命令

默认镜像只装启动/装盘必需的东西：`userland/pm-trim.list` 里约 260 个命令名
（`awk`、`nano`、`curl`/`wget`、`tar`、`sort`/`head`/`tail`/`wc`、`tree`、`file`、
`free`、`dmesg`、`fdisk`/`mkfs`、`ping`、`w3m`、`pweb`、`audio`、`ppm`、`boot`、`vi` …）
**不在**默认系统里，`pm install core` 一条命令补齐；`/sbin/busybox` 本体保留，
任何时候都能 `busybox <命令>` 直接调。装好的盘上 `cat /etc/pm/trimmed-links`、
`/etc/pm/trimmed-binaries` 就是完整清单。

### 默认系统自带

| 命令 | 功能 |
|------|------|
| `init` | PID 1：挂载 proc/sysfs/devtmpfs/tmpfs，有 `/install.d` 则自动安装（有 LBA 1 install-done 标记则跳过） |
| `sh`（`/bin/parlz-sh`） | 自研最小 shell：内建 `ls` `echo` `mount` `pwd` `clear` `export`，管道 `\|`/重定向 `> >> <`/`$?`/`$VAR`/`${VAR}`/`VAR=val cmd` 前缀；外部命令走 `execvp`；支持 `sh <script>`；交互：上下键命令历史 + 逐字符即时回显（无缓冲） |
| `bash` | 移植的 GNU bash（提示符由 `/root/.bashrc` 的 `\u@\h:\w\$` 决定） |
| `install` | 安装器：syslinux 双分区 MBR → 引导分区写入 FAT16 镜像（内含 vmlinuz + EFI/BOOT）→ 分区 2 ext2 → `cpfs` 拷 rootfs → install-done 标记；目标盘已挂载时拒绝安装 |
| `cpfs` | 把当前根（=initramfs 根）整棵写进分区 2 的 ext2：保留权限位、照拷符号链接（ext2 快/慢链）、挂载点只建同名空目录 |
| `pm` | 包管理器（`pm --version` = `pm+1.1-RC+1`）：`available` / `install <名\|本地.pm>` / `remove` / `list`；**默认源就是官方镜像站 `http://www.parlz.com/feed`**（盘上 `/etc/pm/feeds.conf` 写的就是它，索引 `<base>/Packages`）；自建/离线镜像改 `/etc/pm/feeds.conf` 或用 `PM_FEED` |
| `login` | 多账户认证：无账户时首次设置，之后按 `/etc/parlz-auth` 校验；**认证失败不放行**（重试，不进 shell），`login.skip=1` 自动化放行 |
| `user` | 账户管理：`user add [名] [口令]` / `user rm [名]` / `user upd [名] [口令]`，缺参数则交互式；口令存 `$6$`（SHA-512 crypt）散列，不存明文 |
| `mount` / `umount` | 挂载（`-t type` / `-o opts` / 位置式都支持）/ 卸载 |
| `cat` / `grep` / `sed` / `df` / `ps` | 查看 / 检索 / 流编辑 / 挂载盘容量 / 进程列表（`/proc`、`statvfs`） |
| `cp` / `mv` / `rm` / `mkdir` / `ln` / `chmod` / `mknod` | 文件与节点操作（自研真二进制，`mv` 跨设备退化 cp+rm，`chmod` 走 nftw 递归） |
| `ifc` / `ifconfig` / `which` | 网络配置（`ioctl`+netlink 双路）/ 命令定位 / 别名 |
| `opkg` / `ppm` | OPKG 上游后端与委托层（`.ipk` 解包安装到 `/` + 登记 `/var/lib/opkg/`，DEFLATE 用 miniz tinfl 纯 C 解） |
| busybox 保留的 applet | `ls` `dd` `sleep` `reboot` `halt` `poweroff` `date` `kill` `uname` `hostname` 等，以软链落在 `/bin`、`/usr/bin` |

### `pm install core` 补回

| 组 | 命令 |
|---|---|
| 文本处理 | `awk` `sort` `head` `tail` `wc` `uniq` `tr` `cut` `join` `paste` `split` `od` `hexdump` `strings` `nl` `tsort` `more` `expr` `seq` |
| 编辑/查看 | `nano` `vi` `w3m` `tree` `file` `stat` `watch` |
| 网络 | `curl` `wget` `ping` `nc` `telnet` `tftp` `nslookup` `logger` `syslogd` `klogd` |
| 归档压缩 | `tar` `gzip` `gunzip` `zcat` `bzip2` `unzip` `xz` `lzma` `cpio` |
| 磁盘/内存 | `fdisk` `mkfs`（内置 ext2 格式化）`mke2fs` `mkfs.vfat` `mkswap` `swapon` `swapoff` `blkid` `fsck` `shred` `free` `dmesg` `top` `truncate` |
| Parlz 自有 | `pweb`（Web 服务）`ppm`（委托包管理）`audio`（mp3/wav/flac 播放）`boot`（reboot/halt/poweroff） |
| 编译器 | 不在本包里，另需 `pm install gcc` / `pm install clang` |

## 启动流程

1. SeaBIOS 加载 `parlz-bzImage`，执行实模式 setup
2. 解压到 1 MiB，进入 64 位 `x86_64_start_kernel`
3. `parlz_boot_init()` 打印 `Parlz 0.1.0 on x86_64 (Linux 7.2.5 base)`
4. `start_kernel()` 初始化 CPU / mm / sched / fs / net
5. `rest_init()` 创建 PID 1 与 kthreadd
6. 解压 `parlz-initramfs`（newc cpio.gz）为 rootfs
7. `Run /init as init process` → 挂载虚拟文件系统 → exec `/bin/sh`
8. 进入 `parlz>` 命令行

## 发布与版本号

一条命令出一条发布（默认 `rc`，也可 `alpha` / `beta` / `release`）：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/make-release.sh rc 0 1"
```

发布号格式 = **时区 + 构建时间 + 构建人 + 阶段.大版本.小版本 + VGA 位**，例如
`CST+0800+20260927-184500+jgzyes@parlz.com+rc.0.1-F\V`。VGA 位取 `-F\V`
（默认：控制台 = VGA 屏幕 + 串口双路）或 `-UN\V`（`PARLZ_VGA=0`，只串口）。
同一个号在四处一致，
脚本会真起一次 QEMU 断言：

| 位置 | 长相 |
|---|---|
| `/proc/version`、`uname -r` | `Linux version 7.2.5-CST+0800+…+rc.0.1-F\V (jgzyes@parlz.com) … Sun Sep 27 18:45:00 CST 2026` |
| 内核启动横幅 | `Parlz release CST+0800+…+rc.0.1-F\V` |
| `/etc/parlz-release`（装进盘里跟着走） | `version: …` + `kernel-release: 7.2.5-parlz-…` |
| `output/VERSION.txt` | 同上 + 阶段/构建人/VGA 位/工具链 |

控制台默认 `console=ttyS0,115200 console=tty0`（ISO 的 `isolinux.cfg` 与磁盘的
`syslinux.cfg` 同一套，顺序由 `scripts/console-cfg.sh` 单点决定）：内核把**最后**一个
`console=` 当 `/dev/console`，启动脚本又把 0/1/2 重指到它 —— 所以 tty0 放在最后，
VMware/真机的屏幕**既能看也能敲**（ttyS0 在前只收内核 printk，宿主照样能抓日志）。
只在 QEMU `-nographic` 里跑自动化时才需要反过来（那里没有可输入的屏幕）：
`PARLZ_CONSOLE=serial`；验收脚本自己另导一份 `images/parlz-install.e2e.iso`，不动交付那份。
只给 `console=ttyS0` 时 VGA 上不会有输出，表现就是 isolinux 打完
`Booting the kernel` 之后一片黑。要出不带 VGA 的版本：`PARLZ_VGA=0 sh
scripts/make-release.sh …`，发布号自动带 `-UN\V` 后缀。VMware 里想同时收串口日志，
记得给虚拟机加 Serial Port（COM1，输出到文件），VMware 默认常常没有。

产物收在 `output/`（含 `VERSION.txt`、`SHA256SUMS.txt`、`RELEASE-NOTES.md`）。
发布流程细节与三个坑见 [AGENTS.md](AGENTS.md) 的"发布（release）"一节。

## 官网（`web/`，本地起）

纯黑极简的静态站，**六个页面**：`index.html`（版本号、下载、循环敲代码的展示块）、
`packages.html`（包列表）、`download.html`（当前版本 + 历代版本 + 校验）、
`license.html`（许可证与引用：自有代码 GPL-2.0-or-later，上游组件逐项列许可/版本/出处）、
`git.html`（**自托管 Git 仓库**：两个克隆入口、仓库内容、只读发布方式、上游与许可）
与 `sim.html`（**整页模拟机**，见下）。导航顺序固定为
`首页 · 软件包 · 下载 · 许可证 · 仓库 · 模拟机`（模拟机始终排在页面链接之后，六页逐字一致）。
**就是 html + css + js**：`web/` 下 `boot.js`（首屏引导）+ `style.css` + `i18n.js` +
`settings.js`（主题/语言）+ `app.js`（页面部件）+ `router.js`（AJAX 翻页）被六个页面直接引用，
`codeshow.js` 只给首页那块代码、`system.js`/`term.js`/`sim.js` 只给模拟机，
没有构建、没有模板、不依赖任何运行时（更不含 Python）——双击任意一个 `.html` 也能打开
（file:// 下浏览器拦 `fetch`，翻页自动退回整页跳转，见"翻页是 AJAX"）。

**明暗主题**：右上角 `◐ theme` 切换，默认深色、未选过时跟随系统。**十一种语言**：
右上角下拉切 English / 简体中文 / 繁體中文 / Português / Français / Español /
Bahasa Indonesia / हिन्दी / বাংলা / العربية / اردو，未选过时按浏览器语言
（`zh-TW`/`zh-HK`/`zh-MO`/`zh-Hant*`→zh-Hant，其余 `zh*`→zh-CN、`pt*`→pt、
`fr*`→fr、`es*`→es、`hi*`→hi、`bn*`→bn、`id*`/`in*`→id、`ur*`→ur、`ar*`→ar，
其余 en-US）。两项选择都**同时**写 localStorage 与 URL hash（`#lang=fr&theme=light`），
并且每次变更都把站内所有 `.html` 链接改成带这个 hash 的 —— 这是跨页面/跨标签、以及
file:// 下浏览器禁 localStorage 时唯一的同步通道；`storage` 事件与 1s 轮询是兜底。

页面 HTML 里烘的是中文，换语言在 body 末尾由 `i18n.js` 完成（`data-i18n` 文本 /
`data-i18n-html` 含标记 / `data-i18n-attr` 属性三种标记；每语言 129 条词条（`git.html` 那页占 16 条），
en-US 兼作缺词兜底），换语言前正文先 `visibility:hidden`（避免闪一下错语言）；
即使 `app.js` 被拦或加载失败，也有 2.5s 定时器 + 纯 CSS 超时两层兜底把内容放出来，
不会只剩半页。

复杂文字与 RTL 的显示兼容（`style.css` 末尾那一节，别删）：

- 阿拉伯语/乌尔都语整页 `dir="rtl"`（`boot.js` 与 `ParlzI18n.apply` 都设），
  但**终端与 `pre` 锁 ltr**（内核日志、命令行、sha256 左起读），下划线扫过方向、
  表格/内边距全走逻辑属性（`text-align: start/end`、`padding-inline`）；
- 这四种语言（ar/ur/hi/bn）**一律不用 letter-spacing**：阿拉伯/乌尔都连着写、
  天城文/孟加拉文有合字，加字距会把字拆开；
- 等宽字体栈末尾加了复杂文字回退（Win10 的 Nirmala UI、Ubuntu 的
  Noto Devanagari/Bengali/Arabic 与 Lohit、macOS 的 Geeza Pro）。**Ubuntu 上若这些
  文字显示成方块**，装一下 `fonts-noto-core`（`apt-get install -y fonts-noto-core`）——
  Firefox 靠系统字体渲染这三种脚本，页面自己不带 webfont（要保住双击 file:// 也能看）。

**那台"终端"不是回显假数据，是一台模拟真机**（`web/system.js`，只随 `sim.html` 加载）：

- **真 VFS**：`/bin` 的 77 项、`/usr/bin` 的 14 项、`/etc` 每个文件的内容、路径/类型/字节数、
  软链目标，全部照真机 rootfs（`/home/jgzyes/parlz-userland/root`）抄；`/proc/version`
  与 `/etc/parlz-release` 逐字节同源。
- **真 parlz-sh 语法**：照 `userland/sh.c` 复刻 —— 引号是定界符（引号内空格/`|`/`;` 都是
  普通字符）、只在引号外切 `;` 与 `|`（到多 5 个管道）、重定向 `>`/`>>`/`<` 且**只作用管道
  最后一段**、行首赋值是 putenv 语义（活过本行）、`$VAR`/`${VAR}`/`$?`、`cd` 失败也返回 0、
  `&&`/`$()`/glob/反引号一律不支持……连怪癖都照搬。
- **真装包，默认源 = 官网**：演示里的 pm 照真机 `pm.c` 的规则取源 —— `$PM_FEED`
  （种子环境里就是 `http://www.parlz.com/feed`，与 `init.c` 一致）> `/etc/pm/feeds.conf`
  第一行。`pm install core` 会**真去官网下载 core.pm 并解包**（真 newc cpio 解析，见
  `parseCpio`）。装完 `/bin/nano` 就是包里那 **1,583,288 字节的真 ELF** ——
  `file`/`sha256sum`/`od`/`strings` 拿到的是真字节（实测 sha256 与真包成员逐字节一致），
  322 条 busybox 软链也照真机建好。"被裁的命令装 core 之前没有、装完能用"这条性质是真的。
  > 官网 `/feed/*` 只发 `Access-Control-Allow-Origin` 的缺失会让**跨源**页面（比如本地
  > `python3 -m http.server` 起的这份）拉不到，此时自动退回**本站同源镜像 `feed/`**
  > （就是镜像站本体，同一份文件）并**明说用了哪个源**；站点部署到 `https://www.parlz.com/`
  > 之后是同源，直连官网零配置。想让本地预览也直连官网，在官网 nginx 上加一行
  > `add_header Access-Control-Allow-Origin *;`（只对 `/feed/` 开即可）。
- **大包走真清单**：gcc.pm(454.7 MiB) / clang.pm(966.8 MiB) 浏览器里不下载实体，改读
  `web/feed/gcc.list` / `clang.list`（真清单：11773 / 7389 个成员，含 165 / 152 条软链目标，
  由 `scripts/web-pkg-manifest.py` 从真包流式导出）登记成员。
- **`gcc` 是子集编译器**：把 `printf/puts + return` 的 C 编成**真 ELF64**（合法头 + PT_LOAD +
  真 x86-64 指令：write(2) + exit(2)），跑它的是内置的极小 x86-64 解释器（只认这条码路）。
  即 `gcc hello.c -o hello && ./hello` 能真出结果、退出码也真。
- **`/bin` 与 `/usr/bin` 的真 ELF 全都有实现**：77 + 14 个默认名字（含 `chown`/`dd`/`kill`/
  `ifup`/`route`/`adduser`/`chroot`/`time`/`setsid`/`mknod`…），加上 `core.pm` 装回来的 20 个
  真二进制（`awk` 有 -F/模式/NR/变量子集，`fdisk` 打的是本演示真的分区布局）与 gcc/clang 的
  名字。busybox 的 322 个 applet 软链里**常用的一大批**（`xargs`/`diff`/`cmp`/`xxd`/`cksum`/
  `lsof`/`lsblk`/`mountpoint`/`sysctl`/`chattr`/`ipcalc`/`pstree`… 共 59 个）也真实现，
  长尾仍是**如实说明**（不假装成功）。
- **bash 能真跑**：`web/system.js` 里带一个真解析的 bash 子集 —— `&&` `||` `;` `|` `&>`
  重定向（含 `2>`）引号/转义 `$VAR` `${VAR:-x}` `$?` `$#` `$@` `$((算术))` `$(命令替换)`
  \`反引号\` if/for/while/until/case/函数/`{ }`/`( )`/`local`/`export`/`read`/`source`。
  无参 `bash` 会**切进 bash 模式**（提示符变 `root@parlz:~#`，`exit` 回来）；`sh` 照真机
  `shx.c` 判别（无参+tty→parlz-sh，带参→bash）；`ash`/`busybox <applet>` 也都能用。
- **`./xxx` 与绝对路径调用都行**：`./cat`、`/bin/cat`、`/usr/bin/pwd` 都会按名字派发到实现；
  自带 `gcc` 编出来的 ELF 交给内置解释器跑；真机上那些 ELF 则走等价实现。
- **真文件字节落在本地**：`web/rootfs/` 下 37 个真文件（31.7 MiB，`scripts/web-rootfs-sync.sh`
  从构建产物同步）。命令碰到 `src` 标记的文件会**按需取字节**（只取一次），所以
  `sha256sum /bin/cat` 与宿主 `sha256sum web/rootfs/bin/cat` 逐字节一致、`file`/`od`/`strings`/
  `cp` 拿到的都是真内容（实测 bash 2,653,248 B / cat 820,624 B 都对得上）。

**整页模拟机（`sim.html`）**：同一台机器，但终端占满整页、并且是**真终端** ——
`web/term.js` 是个极简 ANSI 终端（光标定位/清屏/滚动/SGR 颜色/备用屏/回看），
于是这些全屏程序真的能跑：`nano 文件`（全屏编辑、`^O` 保存、`^X` 退出、`^W` 搜索、
`^K`/`^U` 剪切粘贴，存完 `cat` 就是刚敲的内容）、`less/more` 文件（翻页、`/` 搜索）、
`top`（实时刷新、`q` 退出）、`watch -n 1 命令`。首页导航和 hero 下方都有入口；
这一页的逻辑在 `web/sim.js`（原来是 sim.html 的内联脚本，AJAX 翻页后内联体不执行，必须成文件）。

**翻页是 AJAX（`web/router.js`）**：点站内链接不再整页硬跳 —— 取新页 HTML → 淡出 → 换内容 →
淡入，顶上有一条细进度线。四条撑住这套设计的规矩，改动时别破：

- **导航栏 `.top` 是常驻的**：六个页面这一块的标记逐字一致，router 只换 `<main>` 里除 `.top`
  之外的孩子。所以主题按钮/语言下拉上挂的监听器不会因翻页失效，`aria-current` 由 router 更新。
- **每页独有的东西要跟着补**：`<head>` 里首页那块代码框用的 `@import` 字体（`ensureHeadAssets`）、
  该页独有的 `<script>`（首页 `codeshow.js`、模拟机 `system.js`+`term.js`+`sim.js`，`ensureDeps`）。
  少补一样就是"翻过去看着像没加载全"。
- **离开一页必须调它的收尾函数**：首页的打字是一条 `setTimeout` 链、模拟机的按键钩子挂在
  **document** 上（终端销毁后 `readKey` 挂住不放，否则 nano 的 `if (k == null) continue` 会空转烧 CPU）。
  新部件一律写成 `(root) => 收尾`，不许在脚本加载时自己跑一遍。
- **fetch 拿不到就退回整页跳转**（`canRoute=false` + `location.href`）：file:// 双击时浏览器拦
  `fetch`，宁可像以前那样跳，也不能点了没反应。URL 用 `pushState` 维护（file:// 下 Chrome 会抛，
  忽略即可），`scrollRestoration` 设 manual，back/forward 由 `popstate` 复原并回到原来滚动位置。
- **静态表不许挂 `.on` 动画**：`html.js tbody tr { opacity: 0 }` 那类"等 JS 加 .on"的规则
  只准写在 JS 真会填的表上（现在只有 `#pkgs`）。烘在 HTML 里的表（许可证 12 行、仓库 1 行）
  没人给 `.on` —— 没开"减弱动态效果"的浏览器上那就是整块空白。

### 自托管 Git 仓库（`git.html` + 仓库根 `git/`）

`git/` 是**要发布的裸库放哪儿**的约定目录（见 `git/README.md`），`git.html` 是它对外的说明页，
风格与其它页完全一致（同一套 CSS、同样的 hero/浮现/表/代码块）。两个克隆入口指向同一份裸库：

```
https://www.parlz.com/git/parlz.git     ← 主地址（与本站同域，不用新证书）
https://git.os.parlz.com/parlz.git      ← 子域入口
```

- **只读镜像**：谁都能 `clone`/`fetch`；写入只有服务器本机与 SSH 一条路（`git push --mirror`）。
- **产物不进仓库**：ISO / IMG / `.pm` 走 `web/downloads/` 与 `web/feed/`，塞进 git 对象库会让
  一次 clone 拖掉整个 GB。
- **服务端当前没配**（用户明确不配）：子域 DNS/TLS 与 `/git/` 走 smart-HTTP 的 location 都没上，
  在那之前 `git clone` 这两个地址是连不通的 —— 页面上就照实写了"待上线地址"这一句，
  别把它删成光鲜的地址列表（`scripts/web-demo-test.js` 有断言拦着）。

**手机端首屏（2026-09-29 修"手机上加载不出来/资源太慢"）**：慢的不是我们的代码，是
**字体表挡住了首次绘制**。实测 zeoseven 那两份 `result.css`：`198` = 140 KB（br 后 **47.7 KB**、
175 个 `unicode-range` 分片）、`184` = 150 KB（gzip 后 **49.3 KB**、172 片），每个分片 ~58 KB，
而 CDN 只给 `Cache-Control: max-age=600`（十分钟就过期）。原先它们用 `@import` 挂在 `style.css`
之前 —— **导入表必须到齐才允许绘制**，于是手机上要等 HTML → style.css → 这 97 KB 字体 CSS
才出第一像素；而我方整站 JS+CSS gzip 起来才 54 KB，字体表反而是它的两倍。改法（**一个字体都没换**，
只是不再让它挡渲染）：

- 两份字体 CSS 改成各页 `<head>` 里的**非阻塞 link**：`media="print" onload="this.media='all'"`，
  先用 `--ui`/`--mono` 里的系统字体把内容画出来，字体到货再 swap；`style.css` 里**不许再有 `@import`**。
- `<link rel="preconnect" … crossorigin>` 提前跟字体域名握手。
- 模拟机整屏从 `100vh` 改 **`100dvh`**（保留 `100vh` 兜底）：`100vh` 是含被地址栏挡住那一条的
  **大视口**，而这一页 `overflow: hidden`，提示符正好落在看不见也滚不到的地方。
- 字体非阻塞之后，终端先按系统字体量好字符格、字体后到就会错位 → `term.js` 听 `document.fonts`
  的 `ready`/`loadingdone`，到货重新 `fit()`（销毁时摘掉监听）。
- `i18n.js`（115 KB / gz 29 KB）到货前正文先 `visibility:hidden`，时限从 **1.6s 压到 0.6s**：
  慢网络上"读不到内容"比"闪一下中文"严重得多。
- 翻页与取 `feed/Packages` 的 `fetch` 缓存策略按域名分：只有本地预览（localhost / 127. / `[::1]` /
  10.0.2.）用 `no-cache`，线上走浏览器缓存 —— 线上每翻一页都回源等于白送一趟往返。
- `web/feed/*.pm`（core 44 MB、pm 7.6 MB、gcc 455 MB、clang 967 MB）与 `web/rootfs/`（45 MB）
  都是**按需**取的，不进首屏；手机上跑 `pm install core` / `sha256sum /bin/pms` 要下真字节，
  慢网络会等一会儿，这是"演示真装包"的代价，不是加载失败。
- **验证提醒**：别在隐藏标签页里判断动画有没有跑 —— Chrome 对隐藏页的深层 `setTimeout` 链做
  intensive throttling（约 1 次/分钟），代码框"停住"是节流不是 bug；要断状态机就照
  `scripts/web-demo-test.js` 那样把待执行的定时器抽出来自己推。

浏览器跑不了 x86-64 ELF，所以"执行"由 JS 等价实现承担；拿不到实体字节时相关命令会
**如实说明**而不是假装。状态只在本次会话 —— 刷新页面 = 重新开机（翻页进/出模拟机也算重新开机）。
加语言/改页面不牵动它；改了包或 rootfs 要重出这两样：

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/web-rootfs-sync.sh"   # 真 ELF 落本地
wsl -d Ubuntu-24.04 -u root -e bash -c "cd /mnt/f/Linux/Parlz && \
  python3 scripts/web-pkg-manifest.py web/feed/gcc.pm web/feed/gcc.list && \
  python3 scripts/web-pkg-manifest.py web/feed/clang.pm web/feed/clang.list"
node scripts/web-demo-test.js   # 秒级回归(宿主, 不起浏览器)：224 条断言
```

站本身兼作 pm 镜像，对外地址就是 **`www.parlz.com/feed/*.pm`**：
`web/feed/` 即完整 feed（`Packages` + 四个 `.pm`，与 `output/feed/` 是**硬链接**，
不占第二份空间）。盘上 `/etc/pm/feeds.conf` 默认写的是 `http://www.parlz.com/feed`
（站点对 http 发 301 跳 https，`pm` 会跟随），直接写
`https://www.parlz.com/feed` 也可以，两者都能装。`web/downloads/` 同理硬链接了**当前**的 ISO 与整盘
IMG，另加了**历代版本**留档（首版 175626 与 190002-r1 的 ISO/IMG，见 `download.html`
的"历代版本"一节），在正式域名下即 `https://www.parlz.com/downloads/…`。
页面取数据/下载用的是**相对路径**，因此本地预览与线上同一套代码都能跑。

```bash
wsl -d Ubuntu-24.04 -u root -e bash -c "cd /mnt/f/Linux/Parlz/web && python3 -m http.server 4173 --bind 127.0.0.1"
# 浏览器开 http://localhost:4173/
```

注意：`web/downloads/` 的 151 MiB / 512 MiB 与 `gcc.pm`(455 M)/`clang.pm`(967 M)
都超过 Qoder Sites 的单文件上限（50 MiB），**这份站只适合本地/自建服务器**；
真要发到托管平台，这些大文件得换外链。

## 已知限制

- 磁盘安装引导链（syslinux MBR + FAT16 引导分区 + ext2 根 + cpfs 全量 rootfs）
  已在 WSL QEMU 下实测：SeaBIOS（Legacy）从装好的盘自启 → 挂 `/dev/vda2` →
  `pivot_root` 换到磁盘上的真根 → 首启设用户名/密码 → shell 可用。
  已知限制：UEFI/OVMF 路径缺 `syslinux.efi`（`gen-fatboot.sh` 会跳过并提示，
  Legacy 不受影响）；重装要在未挂载该盘的会话里做（`install` 会拒绝往
  正挂着当根的盘上装，需从 `boot-install.sh`/ISO 起的会话执行）
- 从启动脚本拉起的 shell 里再起 `bash`，会打印 `cannot set terminal process group`
  + `no job control in this shell`：busybox-init 的 `::sysinit` 子进程不是 session
  leader，`/dev/console` 成不了它的控制终端。`bash` 功能正常（只是 `Ctrl-Z`/`%`
  这类作业控制不可用）；要根治得把 inittab 的那一项换成
  `::respawn:/bin/busybox cttyhack /bin/parlz-sh`（牵动 init 链，暂未做）
- shell 是极简实现：支持管道/重定向/`$?`/`$VAR`/`export`/`VAR=val cmd`/`-c 'code'`，但仍无 `if`/`for`/`while` 控制结构、`&` 后台、`??` 通配。`/bin/sh` 与 `/bin/bash` 是两个不同实现：前者是自研 `parlz-sh`（提示符 `$USER@$HOSTNAME:pwd>`），后者是移植的 GNU bash（提示符由 `/root/.bashrc` 的 `\u@\h:\w\$` 决定，`\u` 取 euid、`\h` 取 `gethostname()`，所以显示 `root@parlz:~#`）
- 自研 ext2 格式化与 `e2fsck` 尚有个位数计数类告警（不影响挂载）
- curl DNS 在 QEMU user NAT（WSL）下 10.0.2.3 的 DNS 代理不通，HTTPS 链接已确认、端到端握手待真实 TLS 服务器
- 网络为 QEMU user NAT（`10.0.2.15`），无真实 NIC 直通；无多用户、无权限模型（一切以 root 运行）

## 工程说明

改这个仓库前请先读 [AGENTS.md](AGENTS.md) —— 里面记录了 WSL 调用方式、
GCC 15 工具链的三个必踩陷阱、以及 ext2 超级块/组描述符的精确字节偏移。

## 许可

内核部分沿用 Linux 的 GPL-2.0。Parlz 自有代码同样以 GPL-2.0 发布。
