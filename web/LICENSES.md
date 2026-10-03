# ParlzOS 授权分层说明 / License layering

> 这份文件解释**谁适用哪份许可证**。许可证正文本身在 [`LICENSE`](LICENSE)
> （与 [`PARLZ.LICENSE`](PARLZ.LICENSE) 逐字相同，二者是同一份文本的两个名字）。
>
> This file explains *which license applies to which part*. The license text
> itself is [`LICENSE`](LICENSE) (byte-identical to [`PARLZ.LICENSE`](PARLZ.LICENSE)).

## 1. 内核：`linux-7.2.5/` → GNU GPL-2.0-only（不可换）

| 项 | 说明 |
|---|---|
| 覆盖范围 | `linux-7.2.5/` 整棵树，**包括** Parlz 自己加进内核的文件（`include/linux/parlz.h`、`include/linux/parlz-release.h`、`arch/x86/kernel/parlz.c`、`init/main.c` 里的 `parlz_boot_init()` 调用、`arch/x86/boot/setup.ld` 的填充修复、`arch/x86/configs/parlz_defconfig` 等） |
| 许可证 | GNU General Public License version 2, **不含 later 选项**（`GPL-2.0-only`），头文件用 `SPDX-License-Identifier: GPL-2.0` |
| 全文在哪 | `linux-7.2.5/COPYING`；交付镜像内 `/usr/share/licenses/linux-kernel/COPYING`；引导分区 `COPYING.TXT` |
| 为什么换不成 PARLZ.LICENSE | 这份源码的权利**只以 GPLv2 为条件**授予；修改版是衍生作品，分发时必须继续按 GPLv2 授权给接收者，且 GPLv2 §6 禁止附加限制。上游著作权人数量庞大，改授权这件事没有法律基础。`PARLZ.LICENSE` 第 1.1.1 / 12.2.1 / 12.3.1 条写的就是这条边界。 |
| 分发义务 | 分发 `vmlinuz` / ISO / 磁盘镜像 = 分发内核目标代码，须按 GPLv2 §3 随附**完整对应源码**（含本项目对内核的修改）或书面要约。源码见 `www.parlz.com/git`，与二进制同一发布号（`cat /etc/parlz-release`、`uname -r`、`/proc/version` 三处同源）。 |

内核树里还有少量其它授权的片段（uapi 头文件的 Linux-syscall-note、固件等），
以每个文件头部的 SPDX 标识为准。

## 2. Parlz 自有代码 → `PARLZ.LICENSE`（Version 1.7，全文见 `LICENSE`）

版权持有人：**JGZ_YES**（所在地：中国广东省深圳市）。许可证正文的版权行与
第十五条（法律适用 / 争议管辖 / 诉讼语言）已按此署名，不再是模板占位符。

覆盖：`userland/`（不含 `userland/nano/`、`userland/busybox/` 等上游目录）、
`scripts/`、`web/`（官网与演示终端）、文档（`README.md`、`AGENTS.md`、`STATUS.md`、
本文件）、`NTCLKS-main/` 的发行版封装层（该子仓库本体是 Apache-2.0）。

- 强 copyleft 式开源许可：可自由使用、修改、分发（含商业），衍生作品须继续开源。
- **商标**：第 5 条。本许可证不授予 "Parlz" 名称与标识的使用权，也不允许暗示背书。

## 3. 上游组件：各自的原许可证

镜像与 pm 包里包含的上游件一律保持原许可证，`PARLZ.LICENSE` 不覆盖它们；
逐件全文随交付介质分发在 `/usr/share/licenses/<组件>/`（索引见同目录 `README`）。

| 组件 | 版本 | 许可证 | 介质内文本 |
|---|---|---|---|
| Linux 内核 | 7.2.5 | GPL-2.0-only（+ Linux-syscall-note） | `/usr/share/licenses/linux-kernel/COPYING` |
| BusyBox | 静态编译 | GPL-2.0 | `/usr/share/licenses/busybox/COPYING` |
| SYSLINUX / ISOLINUX | 6.04 | GPL-2.0-or-later | `/usr/share/licenses/syslinux/COPYING` |
| GNU bash | 5.3 | GPL-3.0-or-later | `/usr/share/licenses/bash/COPYING` |
| nano | 8.4 | GPL-3.0-or-later | `/usr/share/licenses/nano/COPYING` |
| wget | 1.6 | GPL-3.0-or-later | `/usr/share/licenses/wget/COPYING` |
| glibc | 2.39 | LGPL-2.1-or-later | `/usr/share/licenses/glibc/COPYING.LIB` |
| libxcrypt | 4.4.36（构建期静态链） | LGPL-2.1-or-later | 同上 |
| OpenSSL / PazeSSL | 3.5.8 | Apache-2.0 | `/usr/share/licenses/openssl/LICENSE.txt` |
| curl | 8.22.0 | curl（MIT/X 系） | `/usr/share/licenses/curl/COPYRIGHT` |
| miniz（tinfl 子集） | 3.02 的子集 | Miniz license（MIT 风格） | `/usr/share/licenses/miniz/LICENSE` |
| GCC（`gcc.pm`） | 13 | GPL-3.0-or-later WITH GCC-exception-3.1 | 随包 |
| LLVM / clang（`clang.pm`） | 18.1.3 | Apache-2.0 WITH LLVM-exception | 随包 |
| LeonOS 4（`NTCLKS-main/`） | 4.x | Apache-2.0 | `NTCLKS-main/LICENSE` |

> **静态链接提示**：用户空间全部静态链接，因此分发二进制时须能提供上述上游件的
> 对应源码（GPL-3 的 bash/nano/wget、LGPL-2.1 的 glibc/libxcrypt）。本项目自带的
> `userland/` 与构建脚本全在仓库里，上游件按表里的 tarball / 上游地址取得。

## 4. 介质上放在哪（同一份文本的三个落点）

| 位置 | 内容 |
|---|---|
| FAT16 引导分区 | `LICENSE.TXT`（分层说明的介质副本）+ `COPYING.TXT`（GPLv2 全文，与 `vmlinuz` 同分区） |
| ISO 根 | `LICENSE.TXT` + `/usr/share/licenses/**`（rootfs 平铺带过去） |
| 已安装根 | `/usr/share/licenses/<组件>/*`、`/usr/share/licenses/parlz/{PARLZ.LICENSE,LICENSES.md}`、`/LICENSE.TXT`；`/etc/parlz-release` 里有 `license:` / `license-dir:` / `source-url:` |

## 5. 冲突时以谁为准

上游组件的许可证优先于 `PARLZ.LICENSE`；内核部分的 GPLv2 优先于本文件与
`PARLZ.LICENSE`。任何具体文件若与本文件不一致，以该文件头部的 SPDX 标识为准，
并请以 issue 形式报告这种不一致。
