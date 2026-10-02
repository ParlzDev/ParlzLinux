// parlz 演示终端背后的"模拟真机"：真 VFS + 真 parlz-sh 语法 + 真装包。
//
// 与真机同源（不是编的，全部取自本仓库的真机 rootfs / userland 源码）：
//   · /etc/* 的内容、/bin 与 /usr/bin 的每一项（名字/类型/字节数）、软链目标、PATH
//   · parlz-sh 的词法/重定向/退出码语义（照 userland/sh.c 复刻，连它的怪癖一起）
//   · .pm = 未压缩 newc cpio：`pm install core` 会**真去 feed 下载官网的核心包并解包**，
//     真字节进 VFS —— `file`/`sha256sum`/`od`/`strings` 拿到的就是包里的真内容
//
// 与真机不同（浏览器限制，不装成真的）：
//   · x86-64 ELF 不能在这里执行：命令行为由 JS 等价实现承担；没有实体字节的文件
//     （如 /sbin/busybox）data=null，相关命令会如实说明
//   · 超过 64 MiB 的包（gcc/clang）不下载实体，改读 feed/<pkg>.list 真清单登记成员
//   · 状态只在本次会话（刷新页面 = 重新开机）
(function (root) {
"use strict";

/* ================= 与真机同源的常量 ================= */

const REL   = "CST+0800+20260927-190002+jgzyes@parlz.com+rc.0.1-F\\V";
const UTS   = "7.2.5-" + REL;
const PMVER = "pm+1.1-RC+1";
const PARLZ_RELEASE = [
  "name: ParlzOS", "version: " + REL, "semver: 0.1.0", "stage: rc",
  "builder: jgzyes@parlz.com", "build-tz: CST+0800", "build-time: 20260927-190002",
  "kernel-release: 7.2.5-" + REL,
  // 下面三行是真机 /etc/parlz-release 里新增的授权与源码地址字段
  // (build-userland.sh 写盘), 演示站必须逐行跟着真机, 不然"与真机同内容"是假的。
  "license: mixed (kernel=GPL-2.0-only, parlz=PARLZ.LICENSE, upstream=各自; 见 /usr/share/licenses)",
  "license-dir: /usr/share/licenses",
  "source-url: https://www.parlz.com/git",
  "",
].join("\n");
// 交付介质上的"授权说明"副本 —— 与仓库里 third_party/licenses/
// PARLZ-MEDIA-LICENSE.txt 逐字相同（web-demo-test.js 有一条判据专门比这个，
// 防止演示里贴了一份旧文本）。真机上它同时出现在: 引导分区 LICENSE.TXT、
// ISO 根、已安装根 /LICENSE.TXT（同一份文件由 build-userland.sh 烘进 rootfs）。
const MEDIA_LICENSE = [
  "ParlzOS — 授权说明（介质副本 / on-media notice）",
  "",
  "本介质（引导分区 / ISO / 已安装磁盘）上的 `vmlinuz` 是**改过的 Linux 内核**。",
  "授权分三层，不可一刀切：",
  "",
  "  1) 内核部分（`linux-7.2.5/` 整树，含 Parlz 自己加进内核的",
  "     `include/linux/parlz.h`、`arch/x86/kernel/parlz.c`、`init/main.c` 的",
  "     一行钩子、`arch/x86/boot/setup.ld` 的填充修复、`parlz_defconfig` 等）",
  "        → GNU GPL-2.0-only（不含 later 选项）",
  "        → 全文：本介质 `COPYING.TXT`；已安装根 `/usr/share/licenses/linux-kernel/COPYING`",
  "",
  "  2) Parlz 自有代码（用户空间、构建脚本、官网、文档）",
  "        → PARLZ.LICENSE Version 1.7（许可证正文即仓库根的 LICENSE，两者逐字相同）",
  "        → 全文：已安装根 `/usr/share/licenses/parlz/PARLZ.LICENSE`",
  "        → 商标：该许可证第 5 条，不授予名称/标识使用权",
  "",
  "  3) 上游组件（BusyBox、SYSLINUX = GPL-2.0；bash、nano、wget = GPL-3.0；",
  "     glibc、libxcrypt = LGPL-2.1；OpenSSL = Apache-2.0；curl = 自有许可；",
  "     miniz = MIT 风格）",
  "        → 各自的原许可证，PARLZ.LICENSE 不覆盖它们",
  "        → 全文：已安装根 `/usr/share/licenses/<组件>/`，索引见同目录 README",
  "",
  "分层授权的完整说明：`/usr/share/licenses/parlz/LICENSES.md`（仓库根 `LICENSES.md`）。",
  "",
  "源码义务（GPLv2 §3 / LGPL-2.1 §6）",
  "    分发本介质 = 分发内核目标代码与静态链接的用户空间二进制，",
  "    必须能提供**完整对应源码**（含对内核的修改）与可重链接材料：",
  "",
  "        源码：https://www.parlz.com/git",
  "        发布号：`cat /etc/parlz-release`（与 `uname -r`、`/proc/version` 同源）",
  "",
  "若本说明与某个具体文件头部的 SPDX 标识不一致，以文件头部标识为准。",
  "",
].join("\n");
const PROC_VERSION = "Linux version " + UTS + " (jgzyes@parlz.com) " +  "(gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, GNU ld (GNU Binutils for Ubuntu) 2.42) " +
  "#51 SMP PREEMPT_DYNAMIC Sun Sep 27 19:00:02 CST 2026";
// 交付盘 /usr/share/licenses 的真清单: [组件目录, 文件名, 字节数]。
// 名字与字节数取自仓库里的实际文件(third_party/licenses/<组件>/ + 仓库根的
// PARLZ.LICENSE/LICENSE), 由 scripts/web-demo-test.js 逐条核对 —— 演示里
// `ls -l /usr/share/licenses/linux-kernel` 打出的尺寸必须是真机的尺寸。
// 内容不外联(data=null): `cat` 它们时演示会如实说明"没有实体字节"。
const LICENSES = [
  ["bash", "COPYING", 35147],
  ["busybox", "COPYING", 18092],
  ["curl", "COPYRIGHT", 21749],
  ["glibc", "COPYING.LIB", 26530],
  ["linux-kernel", "COPYING", 18092],
  ["miniz", "LICENSE", 1380],
  ["nano", "COPYING", 35149],
  ["openssl", "LICENSE.txt", 11358],
  ["parlz", "LICENSE", 44646],
  ["parlz", "LICENSES.md", 5124],
  ["parlz", "PARLZ.LICENSE", 44646],
  ["syslinux", "COPYING", 18092],
  ["wget", "COPYING", 35149],
];
const LICENSES_README_SIZE = 1296;      // third_party/licenses/README
const MEDIA_LICENSE_SIZE = 1845;        // PARLZ-MEDIA-LICENSE.txt = 盘上 /LICENSE.TXT
const RESOLV = "nameserver 10.0.2.3\nnameserver 8.8.8.8\nnameserver 1.1.1.1\noptions timeout:1 attempts:1\n";
const FEEDS_DEFAULT = "http://www.parlz.com/feed";
const PASSWD = "root:x:0:0:root:/root:/bin/bash\n";
const GROUP  = "root:x:0:\n";
const INITTAB = "# /etc/inittab — ParlzOS\n" +
  "::sysinit:/bin/busybox mount -t proc proc /proc\n" +
  "::sysinit:/bin/busybox mount -t sysfs sysfs /sys\n" +
  "::sysinit:/bin/busybox mount -t tmpfs tmpfs /tmp\n" +
  "::sysinit:/usr/local/bin/parlz-boot.sh\n" +
  "::ctrlaltdel:/bin/busybox killall -HUP sh\n" +
  "::shutdown:/bin/busybox mount -o remount,ro /\n";
const PARLZ_BANNER = "ParlzOS rc.0.1 — Linux 7.2.5 就地改造\n";
const BUSYBOX_SIZE = 2501752;
const NET_ADDR = "10.0.2.15", NET_GW = "10.0.2.2", NET_MASK = "255.255.255.0", NET_DNS = "10.0.2.3";

// 真机 /bin（79 项）与 /usr/bin（14 项）：[名字, 类型, 字节数, 软链目标]
const REAL_BIN = [
  ["ash","l",15,"../sbin/busybox"], ["apt","f",7625568], ["bash","f",2653248], ["busybox","l",15,"../sbin/busybox"],
  ["cat","f",820624], ["chgrp","l",15,"../sbin/busybox"], ["chmod","f",789832],
  ["chown","l",15,"../sbin/busybox"], ["clear","l",8,"parlz-sh"], ["cp","f",820464],
  ["cpfs","f",834368], ["crond","l",15,"../sbin/busybox"], ["date","l",15,"../sbin/busybox"],
  ["dd","l",15,"../sbin/busybox"], ["depmod","l",15,"../sbin/busybox"], ["df","f",951360], ["dpkg","f",874776],
  ["du","l",15,"../sbin/busybox"], ["echo","l",15,"../sbin/busybox"], ["env","l",15,"../sbin/busybox"],
  ["find","l",15,"../sbin/busybox"], ["frpc","f",1062392], ["getty","l",15,"../sbin/busybox"],
  ["grep","f",820816], ["halt","l",15,"../sbin/busybox"], ["hostname","l",15,"../sbin/busybox"],
  ["id","l",15,"../sbin/busybox"], ["ifc","f",865328], ["ifconfig","f",982040],
  ["ifdown","l",15,"../sbin/busybox"], ["ifup","l",15,"../sbin/busybox"], ["init","l",15,"../sbin/busybox"],
  ["insmod","l",15,"../sbin/busybox"], ["install","f",982928], ["ip","l",15,"../sbin/busybox"],
  ["kill","l",15,"../sbin/busybox"], ["killall","l",15,"../sbin/busybox"], ["ln","f",820520],
  ["login","f",1035048], ["ls","l",15,"../sbin/busybox"], ["mkdir","f",820592],
  ["mkfifo","l",15,"../sbin/busybox"], ["mknod","f",820696], ["mktemp","l",15,"../sbin/busybox"],
  ["modprobe","l",15,"../sbin/busybox"], ["mount","f",789792], ["mv","f",820808],
  ["openvpn","f",1062096],
  ["parlz-sh","f",1049848], ["pivot_root","l",15,"../sbin/busybox"], ["pm","f",7576952],
  ["pms","f",799072], ["poweroff","l",15,"../sbin/busybox"], ["printf","l",15,"../sbin/busybox"],
  ["ps","f",820872], ["readlink","l",15,"../sbin/busybox"], ["realpath","l",15,"../sbin/busybox"],
  ["reboot","l",15,"../sbin/busybox"], ["rm","f",820752], ["rmdir","l",15,"../sbin/busybox"],
  ["rmmod","l",15,"../sbin/busybox"], ["route","l",15,"../sbin/busybox"], ["rpm","f",853568], ["sed","f",820592],
  ["sh","f",785528], ["sleep","l",15,"../sbin/busybox"], ["stty","l",15,"../sbin/busybox"],
  ["su","l",15,"../sbin/busybox"], ["switch_root","l",15,"../sbin/busybox"], ["test","l",15,"../sbin/busybox"],
  ["time","l",15,"../sbin/busybox"], ["touch","l",15,"../sbin/busybox"], ["udhcpc","l",15,"../sbin/busybox"],
  ["umount","f",820584], ["uname","l",15,"../sbin/busybox"], ["user","f",1035120],
  ["which","f",785504], ["whoami","l",15,"../sbin/busybox"], ["yum","f",7625984],
];
const REAL_USRBIN = ["add-shell","addgroup","adduser","busybox","chroot","lsmod","mdev","pwd",
  "setsid","sha384sum","sha3sum","sha512sum","sync","users"].map((n) => [n, "l", 13, "/sbin/busybox"]);

/* ================= 字节 / 路径 / 显示工具 ================= */

const TE = new TextEncoder(), TD = new TextDecoder();
const enc = (s) => TE.encode(s);
const dec = (u8) => TD.decode(u8);
const align4 = (n) => (n + 3) & ~3;
const oct = (m) => (m & 0o7777).toString(8).padStart(4, "0");
const rwx = (m, n) => (m & 4 ? "r" : "-") + (m & 2 ? "w" : "-") + (m & 1 ? "x" : "-");
const pad = (s, n) => String(s).padEnd(n);
const human = (n) => n > 1048576 ? (n / 1048576).toFixed(1) + "M" : n > 1024 ? (n / 1024).toFixed(1) + "K" : n + "B";

/* ================= VFS ================= */

const nDir  = (mode) => ({ t: "d", mode: mode || 0o755, kids: new Map() });
const nFile = (data, mode, size) => ({ t: "f", mode: mode || 0o644, data: data || null,
                                       size: size != null ? size : (data ? data.length : 0) });
const nLink = (target) => ({ t: "l", mode: 0o777, link: target, size: target.length });

class Vfs {
  constructor() { this.root = nDir(0o755); this.mounts = []; }
  norm(path, cwd) {
    const p = path.startsWith("/") ? path : ((cwd || "/") === "/" ? "/" + path : (cwd || "/") + "/" + path);
    const out = [];
    for (const seg of p.split("/")) {
      if (!seg || seg === ".") continue;
      if (seg === "..") { out.pop(); continue; }
      out.push(seg);
    }
    return "/" + out.join("/");
  }
  walk(path, follow) {
    const parts = path === "/" ? [] : path.slice(1).split("/");
    let node = this.root, hops = 0;
    for (let i = 0; i < parts.length; i++) {
      if (!node || node.t !== "d") return null;
      node = node.kids.get(parts[i]);
      if (!node) return null;
      const last = i === parts.length - 1;
      if (node.t === "l" && (!last || follow)) {
        if (++hops > 40) return null;
        const dir = "/" + parts.slice(0, i).join("/");
        const target = node.link.startsWith("/") ? node.link : this.norm(node.link, dir);
        const rest = parts.slice(i + 1).join("/");
        return this.walk(rest ? target + "/" + rest : target, follow);
      }
    }
    return node;
  }
  stat(p)  { return this.walk(p, true); }
  lstat(p) { return this.walk(p, false); }
  parentOf(p) { const i = p.lastIndexOf("/"); return i <= 0 ? "/" : p.slice(0, i); }
  baseOf(p) { return p.slice(p.lastIndexOf("/") + 1); }
  mkdirp(path, mode) {
    let node = this.root;
    for (const seg of path.split("/")) {
      if (!seg) continue;
      let kid = node.kids.get(seg);
      if (!kid) { kid = nDir(mode || 0o755); node.kids.set(seg, kid); }
      node = kid;
    }
    return node;
  }
  dir(p) { const n = this.stat(p); return n && n.t === "d" ? n : null; }
  list(p) { const d = this.dir(p); return d ? [...d.kids.keys()] : null; }
  add(p, node) { this.mkdirp(this.parentOf(p)).kids.set(this.baseOf(p), node); return node; }
  writeFile(p, data, mode, size) {
    const u8 = typeof data === "string" ? enc(data) : data;
    this.add(p, nFile(u8, mode, size));
    return this.stat(p);
  }
  symlink(target, p) { return this.add(p, nLink(target)); }
  rmrf(p) {
    const d = this.dir(this.parentOf(p));
    return d ? d.kids.delete(this.baseOf(p)) : false;
  }
  walkAll(p, fn, depth) {
    const n = this.lstat(p);
    if (!n) return;
    fn(p, n, depth || 0);
    if (n.t === "d") for (const name of n.kids.keys())
      this.walkAll(p === "/" ? "/" + name : p + "/" + name, fn, (depth || 0) + 1);
  }
  totalSize(n) {
    if (n.t === "f") return n.size;
    if (n.t === "l") return 0;
    let s = 0;
    for (const kid of n.kids.values()) s += this.totalSize(kid);
    return s;
  }
}

/* ================= cpio（newc；.pm 就是它）================= */

function parseCpio(u8) {
  const out = [];
  const hx = (i) => parseInt(String.fromCharCode(u8[i],u8[i+1],u8[i+2],u8[i+3],u8[i+4],u8[i+5],u8[i+6],u8[i+7]), 16) || 0;
  let off = 0;
  while (off + 110 <= u8.length) {
    if (String.fromCharCode(u8[off],u8[off+1],u8[off+2],u8[off+3],u8[off+4],u8[off+5]) !== "070701") break;
    const mode = hx(off + 14), size = hx(off + 54), ns = hx(off + 94);
    if (!ns || ns > 512) break;
    let name = dec(u8.subarray(off + 110, off + 110 + ns));
    const z = name.indexOf("\0");
    if (z >= 0) name = name.slice(0, z);
    const dataAt = align4(off + 110 + ns);
    if (name.startsWith("TRAILER")) break;
    const kind = mode & 0o170000;
    const rec = { name, mode, size, kind };
    if (kind === 0o120000) rec.target = dec(u8.subarray(dataAt, dataAt + size));
    else if (kind !== 0o40000) rec.data = u8.subarray(dataAt, dataAt + size);
    out.push(rec);
    off = align4(dataAt + size);
  }
  return out;
}

/* ================= 摘要：真 SHA-256 与真 MD5（file:// 下也能用）================= */

const SHA_K = [
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];

function sha256(bytes) {
  const l = bytes.length;
  const buf = new Uint8Array((((l + 8) >> 6) + 1) << 6);
  buf.set(bytes); buf[l] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(buf.length - 8, Math.floor(l / 0x20000000));
  dv.setUint32(buf.length - 4, (l << 3) >>> 0);
  const H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
  const w = new Uint32Array(64);
  const rr = (x, n) => ((x >>> n) | (x << (32 - n))) >>> 0;
  for (let i = 0; i < buf.length; i += 64) {
    for (let j = 0; j < 16; j++) w[j] = dv.getUint32(i + j * 4);
    for (let j = 16; j < 64; j++) {
      const a = w[j-15], b = w[j-2];
      w[j] = (w[j-16] + (rr(a,7) ^ rr(a,18) ^ (a >>> 3)) + w[j-7] + (rr(b,17) ^ rr(b,19) ^ (b >>> 10))) >>> 0;
    }
    let A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], I = H[7];
    for (let j = 0; j < 64; j++) {
      const t1 = (I + (rr(E,6) ^ rr(E,11) ^ rr(E,25)) + ((E & F) ^ (~E & G)) + SHA_K[j] + w[j]) >>> 0;
      const t2 = ((rr(A,2) ^ rr(A,13) ^ rr(A,22)) + ((A & B) ^ (A & C) ^ (B & C))) >>> 0;
      I = G; G = F; F = E; E = (D + t1) >>> 0; D = C; C = B; B = A; A = (t1 + t2) >>> 0;
    }
    H[0] = (H[0]+A)>>>0; H[1] = (H[1]+B)>>>0; H[2] = (H[2]+C)>>>0; H[3] = (H[3]+D)>>>0;
    H[4] = (H[4]+E)>>>0; H[5] = (H[5]+F)>>>0; H[6] = (H[6]+G)>>>0; H[7] = (H[7]+I)>>>0;
  }
  return H.map((x) => x.toString(16).padStart(8, "0")).join("");
}

const MD5_S = [7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
               4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];
function md5(bytes) {
  const K = new Uint32Array(64);
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0;
  const l = bytes.length;
  const buf = new Uint8Array((((l + 8) >> 6) + 1) << 6);
  buf.set(bytes); buf[l] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(buf.length - 8, (l << 3) >>> 0);
  dv.setUint32(buf.length - 4, Math.floor(l / 0x20000000));
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const rol = (x, n) => ((x << n) | (x >>> (32 - n))) >>> 0;
  const M = new Uint32Array(16);
  for (let i = 0; i < buf.length; i += 64) {
    for (let j = 0; j < 16; j++) M[j] = dv.getUint32(i + j * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let j = 0; j < 64; j++) {
      let F, g;
      if (j < 16)      { F = (B & C) | (~B & D);      g = j; }
      else if (j < 32) { F = (D & B) | (~D & C);      g = (5 * j + 1) % 16; }
      else if (j < 48) { F = B ^ C ^ D;               g = (3 * j + 5) % 16; }
      else             { F = C ^ (B | ~D);            g = (7 * j) % 16; }
      const tmp = D; D = C; C = B;
      B = (B + rol((A + F + K[j] + M[g]) >>> 0, MD5_S[j])) >>> 0;
      A = tmp;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const out = new Uint8Array(16), ov = new DataView(out.buffer);
  [a0, b0, c0, d0].forEach((v, i) => ov.setUint32(i * 4, v, true));
  return [...out].map((x) => x.toString(16).padStart(2, "0")).join("");
}

/* ================= 命令实现 ================= */
// 每个实现：async (ctx, argv, io) => rc；argv[0] 是命令名（可能是软链名）。
// ctx: { vfs, env, sys, cwd, resolve(), findExec(), resolveUrl() }

const CMD = {};

const readAll = (ctx, io) => (io.input != null ? io.input : "");
function emit(io, text) { io.out(text.endsWith("\n") ? text : text + "\n"); }

function fileText(ctx, io, arg) {
  const p = ctx.resolve(arg);
  const n = ctx.vfs.stat(p);
  if (!n || n.t === "d") { io.err(arg + ": No such file or directory\n"); return null; }
  if (!n.data) { io.err(arg + ": 打不开（本演示没有该文件的实体字节）\n"); return null; }
  if (n.data.subarray(0, 512).includes(0)) {
    io.err(arg + ": 二进制文件（真机 cat 会原样吐字节；演示里用 od / strings / file 看）\n");
    return null;
  }
  return dec(n.data);
}

/* ---- 文件与文本 ---- */

CMD.ls = (ctx, argv, io) => {
  let long = false, arg = null;
  for (const a of argv.slice(1)) { if (a === "-l") long = true; else arg = a; }
  const node = ctx.vfs.stat(ctx.resolve(arg || "."));
  if (!node) { io.err(arg + ": No such file or directory\n"); return 1; }
  const color = ctx.env.NO_COLOR ? false : (ctx.env.LS_COLORS ? true : ctx.isTty);
  const paint = (s, n) => !color ? s
    : n.t === "d" ? "\x1b[34m" + s + "\x1b[0m"
    : n.t === "l" ? "\x1b[36m" + s + "\x1b[0m"
    : "\x1b[32m" + s + "\x1b[0m";
  const line1 = (name, n) => (n.t === "d" ? "d" : n.t === "l" ? "l" : "-") + " " + n.size + "  " + paint(name, n);
  if (node.t !== "d") { emit(io, long ? line1(arg, node) : paint(arg, node)); return 0; }
  for (const name of node.kids.keys()) {
    if (name === "." || name === "..") continue;
    const kid = node.kids.get(name);
    emit(io, long ? line1(name, kid) : paint(name, kid));
  }
  return 0;
};

CMD.cat = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (!a.length) { io.out(readAll(ctx, io)); return 0; }
  let rc = 0;
  for (const f of a) {
    const t = fileText(ctx, io, f);
    if (t == null) rc = 1; else io.out(t);
  }
  return rc;
};

function headTail(ctx, argv, io, which) {
  let n = 10, file = null;
  const a = argv.slice(1);
  for (let i = 0; i < a.length; i++) {
    if (a[i] === "-n" && a[i + 1]) n = parseInt(a[++i], 10) || 10;
    else if (/^-\d+$/.test(a[i])) n = parseInt(a[i].slice(1), 10);
    else file = a[i];
  }
  let text;
  if (file) { text = fileText(ctx, io, file); if (text == null) return 1; } else text = readAll(ctx, io);
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  const pick = which === "head" ? lines.slice(0, n) : lines.slice(-n);
  if (pick.length) io.out(pick.join("\n") + "\n");
  return 0;
}
CMD.head = (ctx, argv, io) => headTail(ctx, argv, io, "head");
CMD.tail = (ctx, argv, io) => headTail(ctx, argv, io, "tail");

CMD.wc = (ctx, argv, io) => {
  const flags = argv.slice(1).filter((x) => x[0] === "-").join("");
  const files = argv.slice(1).filter((x) => x[0] !== "-");
  const srcs = [];
  if (!files.length) srcs.push([null, readAll(ctx, io)]);
  else for (const f of files) { const t = fileText(ctx, io, f); if (t == null) return 1; srcs.push([f, t]); }
  for (const [name, text] of srcs) {
    const lines = text ? text.replace(/\n$/, "").split("\n").length : 0;
    const words = text.split(/\s+/).filter(Boolean).length;
    const bytes = enc(text).length;
    const tail = name ? " " + name : "";
    if (flags.includes("l")) emit(io, lines + tail);
    else if (flags.includes("w")) emit(io, words + tail);
    else if (flags.includes("c")) emit(io, bytes + tail);
    else emit(io, pad(lines, 7) + pad(words, 8) + pad(bytes, 8) + (name || ""));
  }
  return 0;
};

CMD.grep = (ctx, argv, io) => {
  let pat = null, files = [], icase = false, inv = false, count = false, num = false;
  for (const a of argv.slice(1)) {
    if (a === "-i") icase = true; else if (a === "-v") inv = true;
    else if (a === "-c") count = true; else if (a === "-n") num = true;
    else if (pat === null) pat = a; else files.push(a);
  }
  if (pat === null) { io.err("grep: 用法: grep [-i] [-v] [-c] [-n] 模式 [文件…]\n"); return 2; }
  let re;
  try { re = new RegExp(pat, icase ? "i" : ""); } catch (e) { io.err("grep: " + pat + ": 无效的正则\n"); return 2; }
  const srcs = [];
  if (!files.length) srcs.push([null, readAll(ctx, io)]);
  else for (const f of files) { const t = fileText(ctx, io, f); if (t == null) return 2; srcs.push([f, t]); }
  const multi = srcs.length > 1;
  let hit = false;
  for (const [name, text] of srcs) {
    const lines = text.split("\n");
    if (lines[lines.length - 1] === "") lines.pop();
    let c = 0;
    lines.forEach((ln, i) => {
      if (re.test(ln) !== inv) { hit = true; c++; if (!count) emit(io, (multi ? name + ":" : "") + (num ? (i + 1) + ":" : "") + ln); }
    });
    if (count) emit(io, (multi ? name + ":" : "") + c);
  }
  return hit ? 0 : 1;
};
CMD.egrep = CMD.grep; CMD.fgrep = CMD.grep;

CMD.sed = (ctx, argv, io) => {
  let quiet = false, script = null, file = null;
  for (const x of argv.slice(1)) {
    if (x === "-n") quiet = true; else if (script === null) script = x; else file = x;
  }
  if (!script) { io.err("sed: 用法: sed [-n] 's/旧/新/[g]' [文件]\n"); return 1; }
  let text;
  if (file) { text = fileText(ctx, io, file); if (text == null) return 1; } else text = readAll(ctx, io);
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  const m = script.match(/^s(.)(.*?)\1(.*?)\1([gi]*)$/);
  if (m) {
    let re;
    try { re = new RegExp(m[2], m[4].includes("g") ? "g" : ""); } catch (e) { io.err("sed: 正则错误\n"); return 1; }
    for (const ln of lines) if (!quiet) emit(io, ln.replace(re, m[3]));
    return 0;
  }
  if (script === "p") { for (const ln of lines) emit(io, ln); return 0; }
  const np = script.match(/^(\d+)p$/);
  if (np) { const i = parseInt(np[1], 10); if (lines[i - 1] !== undefined) emit(io, lines[i - 1]); return 0; }
  io.err("sed: 本演示支持 s///、p、Np（真机 /bin/sed 是完整实现）\n");
  return 1;
};

CMD.tr = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (a.length < 2) { io.err("tr: 用法: tr 集合1 集合2\n"); return 1; }
  const expand = (s) => s.replace(/(.)-(.)/g, (mm, x, y) => {
    let o = ""; for (let c = x.charCodeAt(0); c <= y.charCodeAt(0); c++) o += String.fromCharCode(c); return o;
  });
  const from = expand(a[0]), to = expand(a[1]), del = argv.includes("-d");
  let res = "";
  for (const ch of readAll(ctx, io)) {
    const i = from.indexOf(ch);
    if (del) { if (i < 0) res += ch; }
    else if (i < 0) res += ch;
    else res += to[i] !== undefined ? to[i] : to[to.length - 1];
  }
  io.out(res);
  return 0;
};

CMD.cut = (ctx, argv, io) => {
  const a = argv.slice(1);
  let delim = "\t", fields = null, file = null;
  for (let i = 0; i < a.length; i++) {
    if (a[i] === "-d") delim = a[++i];
    else if (a[i] === "-f") fields = a[++i];
    else if (a[i].startsWith("-d")) delim = a[i].slice(2);
    else if (a[i].startsWith("-f")) fields = a[i].slice(2);
    else file = a[i];
  }
  if (!fields) { io.err("cut: 需要 -f\n"); return 1; }
  let text;
  if (file) { text = fileText(ctx, io, file); if (text == null) return 1; } else text = readAll(ctx, io);
  const want = fields.split(",").map((x) => parseInt(x, 10));
  for (const ln of text.split("\n")) {
    if (!ln) continue;
    const parts = delim === "\t" ? ln.split(/\t/) : ln.split(delim);
    emit(io, want.map((i) => (parts[i - 1] !== undefined ? parts[i - 1] : "")).join(delim));
  }
  return 0;
};

CMD.sort = (ctx, argv, io) => {
  const file = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text;
  if (file) { text = fileText(ctx, io, file); if (text == null) return 1; } else text = readAll(ctx, io);
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  lines.sort(argv.includes("-n") ? (x, y) => parseFloat(x) - parseFloat(y) : undefined);
  if (lines.length) io.out(lines.join("\n") + "\n");
  return 0;
};

CMD.uniq = (ctx, argv, io) => {
  const file = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text;
  if (file) { text = fileText(ctx, io, file); if (text == null) return 1; } else text = readAll(ctx, io);
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  const count = argv.includes("-c");
  let prev = null, n = 0;
  const flush = () => { if (prev !== null) emit(io, count ? pad(n, 7) + " " + prev : prev); };
  for (const ln of lines) { if (ln === prev) n++; else { flush(); prev = ln; n = 1; } }
  flush();
  return 0;
};

function copyMove(ctx, argv, io, which) {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  const rec = argv.includes("-r") || argv.includes("-R");
  if (a.length < 2) { io.err(which + ": 缺少操作数\n"); return 1; }
  const src = ctx.resolve(a[0]);
  const srcNode = ctx.vfs.stat(src);              // cp 跟随源软链（真机 cp 也是）
  if (!srcNode) { io.err(a[0] + ": No such file or directory\n"); return 1; }
  const dstRaw = ctx.resolve(a[a.length - 1]);
  const dstNode = ctx.vfs.stat(dstRaw);
  const dst = dstNode && dstNode.t === "d" ? dstRaw + "/" + ctx.vfs.baseOf(src) : dstRaw;
  if (srcNode.t === "d" && !rec) { io.err(which + ": " + a[0] + " is a directory (用 -r)\n"); return 1; }
  const clone = (n) => n.t === "d" ? nDir(n.mode) : n.t === "l" ? nLink(n.link) : nFile(n.data ? n.data.slice() : null, n.mode, n.size);
  const rec2 = (from, to) => {
    const n = ctx.vfs.lstat(from);
    ctx.vfs.add(to, clone(n));
    if (n.t === "d") for (const name of n.kids.keys()) rec2(from + "/" + name, to + "/" + name);
  };
  rec2(src, dst);
  if (which === "mv") ctx.vfs.rmrf(src);
  return 0;
}
CMD.cp = (ctx, argv, io) => copyMove(ctx, argv, io, "cp");
CMD.mv = (ctx, argv, io) => copyMove(ctx, argv, io, "mv");

CMD.rm = (ctx, argv, io) => {
  const joined = argv.slice(1).join(" ");
  const rec = /-[a-zA-Z]*[rR]/.test(joined);
  const force = /-[a-zA-Z]*f/.test(joined);
  let rc = 0;
  for (const f of argv.slice(1).filter((x) => x[0] !== "-")) {
    const p = ctx.resolve(f), node = ctx.vfs.lstat(p);
    if (!node) { if (!force) { io.err(f + ": No such file or directory\n"); rc = 1; } continue; }
    if (node.t === "d" && !rec) { io.err("rm: " + f + " is a directory (用 -r)\n"); rc = 1; continue; }
    ctx.vfs.rmrf(p);
  }
  return rc;
};

CMD.mkdir = (ctx, argv, io) => {
  const par = argv.includes("-p");
  let rc = 0;
  for (const d of argv.slice(1).filter((x) => x[0] !== "-")) {
    const p = ctx.resolve(d);
    if (ctx.vfs.lstat(p)) { if (!par) { io.err("mkdir: " + d + ": File exists\n"); rc = 1; } continue; }
    if (!ctx.vfs.dir(ctx.vfs.parentOf(p)) && !par) { io.err("mkdir: " + d + ": No such file or directory\n"); rc = 1; continue; }
    if (par) ctx.vfs.mkdirp(p); else ctx.vfs.add(p, nDir(0o755));
  }
  return rc;
};

CMD.rmdir = (ctx, argv, io) => {
  let rc = 0;
  for (const d of argv.slice(1).filter((x) => x[0] !== "-")) {
    const p = ctx.resolve(d), n = ctx.vfs.dir(p);
    if (!n) { io.err("rmdir: " + d + ": No such file or directory\n"); rc = 1; continue; }
    if (n.kids.size) { io.err("rmdir: " + d + ": Directory not empty\n"); rc = 1; continue; }
    ctx.vfs.rmrf(p);
  }
  return rc;
};

CMD.ln = (ctx, argv, io) => {
  const sym = argv.includes("-s");
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (a.length < 2) { io.err("ln: 缺少操作数\n"); return 1; }
  const target = ctx.resolve(a[1]);
  const dst = ctx.vfs.dir(target) ? target + "/" + ctx.vfs.baseOf(a[0]) : target;
  if (sym) ctx.vfs.symlink(a[0], dst);
  else {
    const n = ctx.vfs.lstat(ctx.resolve(a[0]));
    if (!n) { io.err(a[0] + ": No such file or directory\n"); return 1; }
    ctx.vfs.add(dst, n);
  }
  return 0;
};

CMD.chmod = (ctx, argv, io) => {
  const a = argv.slice(1);
  if (a.length < 2) { io.err("chmod: 用法: chmod 0644 文件…\n"); return 1; }
  const mode = parseInt(a[0], 8);
  let rc = 0;
  for (const f of a.slice(1)) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n) { io.err("chmod: " + f + ": No such file or directory\n"); rc = 1; continue; }
    n.mode = mode;
  }
  return rc;
};

CMD.touch = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (!a.length) { io.err("touch: usage: touch <file>\n"); return 1; }
  for (const f of a) { const p = ctx.resolve(f); if (!ctx.vfs.lstat(p)) ctx.vfs.writeFile(p, new Uint8Array(0), 0o644); }
  return 0;
};

CMD.stat = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!f) { io.err("stat: 缺少操作数\n"); return 1; }
  const p = ctx.resolve(f), n = ctx.vfs.lstat(p);
  if (!n) { io.err("stat: " + f + ": No such file or directory\n"); return 1; }
  emit(io, "  File: " + p);
  emit(io, "  Size: " + n.size + "\tType: " + (n.t === "d" ? "directory" : n.t === "l" ? "symbolic link" : n.data ? "regular file" : "regular file (无实体字节, 真机是 ELF)"));
  emit(io, "Access: (" + oct(n.mode) + "/" + (n.t === "d" ? "d" : n.t === "l" ? "l" : "-") + rwx((n.mode >> 6) & 7, 1) + rwx((n.mode >> 3) & 7, 1) + rwx(n.mode & 7, 1) + ")");
  if (n.t === "l") emit(io, "  Link: -> " + n.link);
  return 0;
};

CMD.tree = (ctx, argv, io) => {
  const start = ctx.resolve(argv.slice(1).filter((x) => x[0] !== "-")[0] || ".");
  const node = ctx.vfs.lstat(start);
  if (!node) { io.err(start + ": No such file or directory\n"); return 1; }
  let dirs = 0, files = 0;
  emit(io, start);
  const walk = (n, prefix) => {
    const names = [...n.kids.keys()];
    names.forEach((name, i) => {
      const kid = n.kids.get(name), last = i === names.length - 1;
      emit(io, prefix + (last ? "└── " : "├── ") + name + (kid.t === "l" ? " -> " + kid.link : ""));
      if (kid.t === "d") { dirs++; walk(kid, prefix + (last ? "    " : "│   ")); } else files++;
    });
  };
  if (node.t === "d") walk(node, "");
  emit(io, "");
  emit(io, dirs + " directories, " + files + " files");
  return 0;
};

CMD.find = (ctx, argv, io) => {
  const a = argv.slice(1);
  const start = ctx.resolve(a[0] && a[0][0] !== "-" ? a[0] : ".");
  const ni = a.indexOf("-name");
  const pat = ni >= 0 ? a[ni + 1] : null;
  const re = pat ? new RegExp("^" + pat.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".") + "$") : null;
  ctx.vfs.walkAll(start, (p, n) => { if (!re || re.test(ctx.vfs.baseOf(p))) emit(io, p); });
  return 0;
};

CMD.du = (ctx, argv, io) => {
  const path = ctx.resolve(argv.slice(1).filter((x) => x[0] !== "-")[0] || ".");
  const n = ctx.vfs.lstat(path);
  if (!n) { io.err(path + ": No such file or directory\n"); return 1; }
  const h = argv.includes("-h");
  if (n.t !== "d") { emit(io, (h ? human(n.size) : Math.ceil(n.size / 1024)) + "\t" + path); return 0; }
  let total = 0;
  for (const [name, kid] of n.kids) {
    const size = ctx.vfs.totalSize(kid);
    total += size;
    emit(io, (h ? human(size) : Math.ceil(size / 1024)) + "\t" + name);
  }
  emit(io, (h ? human(total) : Math.ceil(total / 1024)) + "\ttotal");
  return 0;
};

CMD.df = (ctx, argv, io) => {
  emit(io, "Filesystem      1K-blocks   Used Available Use% Mounted on");
  emit(io, "/dev/vda2          524288  51200    473088  10% /");
  emit(io, "devtmpfs            32768      0     32768   0% /dev");
  emit(io, "tmpfs               65536     12     65524   0% /tmp");
  emit(io, "proc                    0      0         0    - /proc");
  return 0;
};

CMD.which = (ctx, argv, io) => {
  let rc = 1;
  for (const name of argv.slice(1).filter((x) => x[0] !== "-")) {
    const hit = ctx.findExec(name);
    if (hit) { emit(io, hit); rc = 0; }
  }
  return rc;
};

CMD.file = (ctx, argv, io) => {
  let rc = 0;
  for (const f of argv.slice(1).filter((x) => x[0] !== "-")) {
    const p = ctx.resolve(f), n = ctx.vfs.lstat(p);
    if (!n) { io.err("file: " + f + ": cannot open\n"); rc = 1; continue; }
    let kind;
    if (n.t === "d") kind = "directory";
    else if (n.t === "l") kind = "symbolic link to " + n.link;
    else if (!n.data) kind = "ELF 64-bit LSB executable, x86-64 (本演示未下载实体, " + n.size + " B)";
    else {
      const d = n.data;
      if (d.length > 18 && d[0] === 0x7f && d[1] === 0x45 && d[2] === 0x4c && d[3] === 0x46)
        kind = "ELF 64-bit LSB " + (d[16] === 2 ? "executable" : d[16] === 3 ? "shared object" : "object") +
               ", " + (d[18] === 0x3e ? "x86-64" : "x86") + ", 动态链接, " + d.length + " B";
      else if (d.length >= 6 && String.fromCharCode(d[0],d[1],d[2],d[3],d[4],d[5]) === "070701")
        kind = "ASCII cpio archive (SVR4 with no CRC), " + d.length + " B";
      else if (d[0] === 0x1f && d[1] === 0x8b) kind = "gzip compressed data";
      else kind = "ASCII text" + (d.length > 1024 ? ", " + d.length + " B" : "");
    }
    emit(io, f + ": " + kind);
  }
  return rc;
};

CMD.od = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let data;
  if (f) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n || !n.data) { io.err("od: " + f + ": 没有实体字节\n"); return 1; }
    data = n.data;
  } else data = enc(readAll(ctx, io));
  const limit = Math.min(data.length, 256);
  for (let i = 0; i < limit; i += 16)
    emit(io, i.toString(8).padStart(7, "0") + " " + [...data.subarray(i, i + 16)].map((x) => x.toString(16).padStart(2, "0")).join(" "));
  if (data.length > limit) emit(io, "* (只显示前 256 字节)");
  return 0;
};
CMD.hexdump = CMD.od;

CMD.strings = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!f) { io.err("strings: 缺少操作数\n"); return 1; }
  const n = ctx.vfs.stat(ctx.resolve(f));
  if (!n || !n.data) { io.err("strings: " + f + ": 没有实体字节\n"); return 1; }
  for (const m of dec(n.data).matchAll(/[\x20-\x7e]{5,}/g)) emit(io, m[0]);
  return 0;
};

function hashCmd(ctx, argv, io, algo) {
  const files = argv.slice(1).filter((x) => x[0] !== "-");
  if (!files.length) { io.out((algo === "md5" ? md5(enc(readAll(ctx, io))) : sha256(enc(readAll(ctx, io)))) + "  -\n"); return 0; }
  let rc = 0;
  for (const f of files) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n || n.t === "d") { io.err(f + ": No such file or directory\n"); rc = 1; continue; }
    if (!n.data) { io.err(f + ": 本演示没有实体字节（" + n.size + " B）\n"); rc = 1; continue; }
    emit(io, (algo === "md5" ? md5(n.data) : sha256(n.data)) + "  " + f);
  }
  return rc;
}
CMD.md5sum = (ctx, argv, io) => hashCmd(ctx, argv, io, "md5");
CMD.sha256sum = (ctx, argv, io) => hashCmd(ctx, argv, io, "sha256");
CMD.sha1sum = CMD.sha256sum; CMD.sha512sum = CMD.sha256sum; CMD.sha384sum = CMD.sha256sum; CMD.sha3sum = CMD.sha256sum;

CMD.base64 = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text;
  if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  if (argv.includes("-d")) {
    try {
      const bin = atob(text.trim());
      const u8 = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
      io.out(dec(u8));
    } catch (e) { io.err("base64: 输入无效\n"); return 1; }
    return 0;
  }
  let bin = "";
  for (const x of enc(text)) bin += String.fromCharCode(x);
  const b64 = btoa(bin);
  for (let i = 0; i < b64.length; i += 76) emit(io, b64.slice(i, i + 76));
  return 0;
};

CMD.nl = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text;
  if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  let i = 1;
  for (const ln of text.split("\n")) { if (!ln) continue; emit(io, pad(i++, 6) + "\t" + ln); }
  return 0;
};

CMD.tee = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const text = readAll(ctx, io);
  if (f) ctx.vfs.writeFile(ctx.resolve(f), text, 0o644);
  io.out(text);
  return 0;
};

CMD.seq = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-").map(Number);
  let from = 1, to = 1, step = 1;
  if (a.length === 1) to = a[0];
  else if (a.length === 2) { from = a[0]; to = a[1]; }
  else if (a.length >= 3) { from = a[0]; step = a[1]; to = a[2]; }
  for (let i = from; step > 0 ? i <= to : i >= to; i += step) emit(io, String(i));
  return 0;
};

CMD.pwd = (ctx, argv, io) => { emit(io, ctx.cwd); return 0; };
CMD.yes = (ctx, argv, io) => { emit(io, argv[1] || "y"); return 0; };
CMD.sleep = (ctx, argv, io) => { emit(io, "(演示环境不真的等待)"); return 0; };
CMD.clear = (ctx, argv, io) => { io.out("\x1b[H\x1b[2J"); return 0; };
CMD.help = (ctx, argv, io) => {
  io.out("  ls [dir] | ls -l | cd [dir] | echo <text> | touch <file> | mount | pwd | clear | exit\n" +
         "  ls 颜色: 目录蓝/文件绿/链接青(NO_COLOR=1 或管道时关)\n" +
         "  管道: cmd1 | cmd2 (至多 5 段)\n" +
         "  重定向: cmd > f | cmd >> f | cmd < f\n" +
         "  外部命令: nano / install / mkfs / curl / ifc / ifconfig ...\n");
  return 0;
};
CMD.echo = (ctx, argv, io) => { emit(io, argv.slice(1).join(" ")); return 0; };
CMD.printf = (ctx, argv, io) => {
  const a = argv.slice(1);
  if (!a.length) return 0;
  const fmt = a[0].replace(/\\n/g, "\n").replace(/\\t/g, "\t");
  let i = 1;
  io.out(fmt.replace(/%[sd]/g, () => (a[i] !== undefined ? a[i++] : "")));
  return 0;
};
CMD.basename = (ctx, argv, io) => { emit(io, ctx.vfs.baseOf(argv[1] || "")); return 0; };
CMD.dirname = (ctx, argv, io) => { emit(io, ctx.vfs.parentOf(ctx.resolve(argv[1] || "."))); return 0; };
CMD.realpath = (ctx, argv, io) => { emit(io, ctx.resolve(argv[1] || ".")); return 0; };
CMD.readlink = (ctx, argv, io) => {
  const n = ctx.vfs.lstat(ctx.resolve(argv[1] || ""));
  if (!n || n.t !== "l") { io.err("readlink: " + (argv[1] || "") + ": 不是软链\n"); return 1; }
  emit(io, n.link);
  return 0;
};
CMD.nproc = (ctx, argv, io) => { emit(io, "2"); return 0; };

function testImpl(ctx, argv) {
  const a = argv.slice(1);
  if (!a.length) return 1;
  if (a[0] === "-e") return ctx.vfs.stat(ctx.resolve(a[1])) ? 0 : 1;
  if (a[0] === "-d") { const n = ctx.vfs.stat(ctx.resolve(a[1])); return n && n.t === "d" ? 0 : 1; }
  if (a[0] === "-f") { const n = ctx.vfs.stat(ctx.resolve(a[1])); return n && n.t === "f" ? 0 : 1; }
  if (a[0] === "-x") { const n = ctx.vfs.stat(ctx.resolve(a[1])); return n && (n.mode & 0o111) ? 0 : 1; }
  if (a[0] === "-z") return a[1] === undefined || a[1] === "" ? 0 : 1;
  if (a[0] === "-n") return a[1] ? 0 : 1;
  if (a[1] === "=") return a[0] === a[2] ? 0 : 1;
  if (a[1] === "!=") return a[0] !== a[2] ? 0 : 1;
  if (a[1] === "-eq") return Number(a[0]) === Number(a[2]) ? 0 : 1;
  if (a[1] === "-ne") return Number(a[0]) !== Number(a[2]) ? 0 : 1;
  if (a[1] === "-gt") return Number(a[0]) > Number(a[2]) ? 0 : 1;
  if (a[1] === "-lt") return Number(a[0]) < Number(a[2]) ? 0 : 1;
  return a[0] && a[0].length ? 0 : 1;
}
CMD.test = (ctx, argv) => testImpl(ctx, argv);
CMD["["] = (ctx, argv) => testImpl(ctx, argv.slice(0, -1));

CMD.expr = (ctx, argv, io) => {
  const a = argv.slice(1);
  if (a.length === 3 && ["+","-","*","/","%"].includes(a[1])) {
    const x = Number(a[0]), y = Number(a[2]);
    const v = a[1] === "+" ? x + y : a[1] === "-" ? x - y : a[1] === "*" ? x * y : a[1] === "/" ? Math.floor(x / y) : x % y;
    emit(io, String(v));
    return v === 0 ? 1 : 0;
  }
  if (a.length === 3 && ["=","!=","<",">"].includes(a[1])) {
    const v = a[1] === "=" ? a[0] === a[2] : a[1] === "!=" ? a[0] !== a[2] : a[1] === "<" ? a[0] < a[2] : a[0] > a[2];
    emit(io, v ? "1" : "0");
    return v ? 0 : 1;
  }
  emit(io, a.join(" "));
  return a.length ? 0 : 1;
};

CMD.factor = (ctx, argv, io) => {
  const n = parseInt(argv[1], 10);
  if (!n) { io.err("factor: 需要一个整数\n"); return 1; }
  let m = n, f = 2; const fs = [];
  while (m > 1 && f * f <= m) { while (m % f === 0) { fs.push(f); m /= f; } f++; }
  if (m > 1) fs.push(m);
  emit(io, n + ": " + fs.join(" "));
  return 0;
};

CMD.cal = (ctx, argv, io) => {
  const now = new Date(), y = now.getFullYear(), mo = now.getMonth();
  const names = ["January","February","March","April","May","June","July","August","September","October","November","December"];
  emit(io, "     " + names[mo] + " " + y);
  emit(io, "Su Mo Tu We Th Fr Sa");
  const first = new Date(y, mo, 1).getDay(), days = new Date(y, mo + 1, 0).getDate();
  let row = "   ".repeat(first);
  for (let d = 1; d <= days; d++) {
    row += String(d).padStart(2, " ") + " ";
    if ((first + d) % 7 === 0) { emit(io, row.trimEnd()); row = ""; }
  }
  if (row.trim()) emit(io, row.trimEnd());
  return 0;
};

/* ---- 系统信息 ---- */

CMD.uname = (ctx, argv, io) => {
  if (argv.includes("-r")) { emit(io, UTS); return 0; }
  if (argv.includes("-a")) { emit(io, "Linux parlz " + UTS + " #51 SMP PREEMPT_DYNAMIC x86_64 GNU/Linux"); return 0; }
  emit(io, "Linux");
  return 0;
};
CMD.hostname = (ctx, argv, io) => {
  if (argv[1]) { ctx.env.HOSTNAME = argv[1]; return 0; }
  emit(io, ctx.env.HOSTNAME);
  return 0;
};
CMD.whoami = (ctx, argv, io) => { emit(io, ctx.env.USER); return 0; };
CMD.logname = CMD.whoami;
CMD.id = (ctx, argv, io) => { emit(io, "uid=0(root) gid=0(root) groups=0(root)"); return 0; };
CMD.users = (ctx, argv, io) => { emit(io, ctx.env.USER); return 0; };
CMD.date = (ctx, argv, io) => { emit(io, new Date().toString()); return 0; };
CMD.uptime = (ctx, argv, io) => { emit(io, " 20:00:00 up 0 min,  1 user,  load average: 0.00, 0.00, 0.00"); return 0; };
CMD.env = (ctx, argv, io) => { for (const k of Object.keys(ctx.env).sort()) io.out(k + "=" + ctx.env[k] + "\n"); return 0; };
CMD.printenv = (ctx, argv, io) => {
  const k = argv[1];
  if (!k) return CMD.env(ctx, argv, io);
  if (ctx.env[k] === undefined) return 1;
  emit(io, ctx.env[k]);
  return 0;
};
CMD.free = (ctx, argv, io) => {
  emit(io, "              total        used        free      shared  buff/cache   available");
  emit(io, "Mem:         503316480     9437184   481067008     1048576    12689408   489553920");
  emit(io, "Swap:                0           0           0");
  return 0;
};
CMD.ps = (ctx, argv, io) => {
  emit(io, "  PID USER       VSZ STAT COMMAND");
  const rows = procTable(ctx.sys).slice();
  rows.push({ pid: 58, user: ctx.env.USER, vsz: 1980, stat: "R", cmd: "ps" });
  for (const r of rows)
    io.out(pad(r.pid, 6) + pad(r.user, 10) + pad(r.vsz, 7) + pad(r.stat, 5) + r.cmd + "\n");
  return 0;
};
CMD.dmesg = (ctx, argv, io) => {
  for (const l of [
    "[    0.000000] " + PROC_VERSION,
    "[    0.000000] Parlz 0.1.0 on x86_64 (Linux 7.2.5 base)",
    "[    0.000000] Parlz release " + REL,
    "[    0.120000] console [tty0] enabled",
    "[    0.121000] console [ttyS0] enabled",
    "[    2.310000] virtio_blk virtio1: [vda] 1048576 512-byte logical blocks (537 MB/512 MiB)",
    "[    2.840000]  vda: vda1 vda2",
    "[    3.020000] mounted /dev/vda2 (ext4) on /mnt",
    "[    3.400000] Parlz boot ready",
  ]) emit(io, l);
  return 0;
};
CMD.mount = (ctx, argv, io) => {
  const a = argv.slice(1);
  if (a.length >= 3) {
    ctx.vfs.mounts.push([a[0], a[1], a[2], a[3] || ""]);
    if (ctx.sys.refreshProc) ctx.sys.refreshProc();
    emit(io, "mounted " + a[0] + " on " + a[1] + " (" + a[2] + ")");
    return 0;
  }
  const mp = ctx.vfs.stat("/proc/mounts");
  const text = mp && mp.data ? dec(mp.data) : "";
  for (const ln of text.split("\n")) if (ln) { emit(io, ln); emit(io, ""); }   // 真机每行后多一个空行
  return 0;
};
CMD.umount = (ctx, argv, io) => {
  const t = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const i = ctx.vfs.mounts.findIndex((m) => m[1] === t || m[0] === t);
  if (i < 0) { io.err("umount: " + t + ": not mounted\n"); return 1; }
  ctx.vfs.mounts.splice(i, 1);
  if (ctx.sys.refreshProc) ctx.sys.refreshProc();
  return 0;
};
CMD.sync = () => 0;
CMD.reboot = (ctx, argv, io) => { emit(io, "reboot: 演示环境不重启 —— 刷新页面就是重新开机"); return 0; };
CMD.poweroff = (ctx, argv, io) => { emit(io, "poweroff: 演示环境不关机 —— 刷新页面就是重新开机"); return 0; };
CMD.halt = CMD.poweroff;
CMD.ifconfig = (ctx, argv, io) => {
  const net = netState(ctx.sys);
  io.out("eth0      Link encap:Ethernet  HWaddr 52:54:00:12:34:56\n");
  if (net.up) io.out("          inet addr:" + net.addr + "  Bcast:10.0.2.255  Mask:" + net.mask + "\n");
  io.out("          " + (net.up ? "UP BROADCAST RUNNING MULTICAST" : "BROADCAST MULTICAST") + "  MTU:1500  Metric:1\n" +
         "          RX packets:31 errors:0 dropped:0 overruns:0 frame:0\n" +
         "          TX packets:19 errors:0 dropped:0 overruns:0 carrier:0\n" +
         "          collisions:0 txqueuelen:1000\n" +
         "lo        Link encap:Local Loopback\n" +
         "          inet addr:127.0.0.1  Mask:255.0.0.0\n" +
         "          UP LOOPBACK RUNNING  MTU:65536  Metric:1\n");
  return 0;
};
CMD.ip = CMD.ifconfig;

CMD.ping = (ctx, argv, io) => {
  const host = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!host) { io.err("ping: 用法: ping <主机>\n"); return 1; }
  const ip = host === "parlz" ? NET_ADDR : /^\d+\.\d+\.\d+\.\d+$/.test(host) ? host : null;
  if (!ip) { io.err("ping: bad address '" + host + "'\n"); return 1; }
  emit(io, "PING " + host + " (" + ip + "): 56 data bytes");
  for (let i = 1; i <= 3; i++) emit(io, "64 bytes from " + ip + ": seq=" + i + " ttl=64 time=0.4" + i + " ms");
  emit(io, "--- " + host + " ping statistics ---");
  emit(io, "3 packets transmitted, 3 packets received, 0% packet loss");
  return 0;
};
CMD.ping6 = CMD.ping;

CMD.ifc = (ctx, argv, io) => {
  emit(io, "ifc: auto  eth0");
  emit(io, "waiting for carrier on eth0 ... carrier up");
  emit(io, "DHCP: got " + NET_ADDR + "/24 gw " + NET_GW + " dns " + NET_DNS);
  emit(io, "ifc: network up (eth0 " + NET_ADDR + "/24)");
  return 0;
};

/* ---- 网络：curl/wget 真的发请求（同源；跨域会被浏览器拦，如实报错）---- */

CMD.curl = async (ctx, argv, io) => {
  const a = argv.slice(1);
  const head = a.includes("-I") || a.includes("-sI");
  const url = a.filter((x) => x[0] !== "-")[0];
  if (!url) { io.err("curl: 用法: curl [-I] <url>\n"); return 1; }
  const full = ctx.resolveUrl(url);
  try {
    const res = await ctx.sys.fetchImpl(full, head ? { method: "HEAD" } : undefined);
    if (head) {
      emit(io, "HTTP/1.0 " + res.status + " " + (res.statusText || "OK"));
      res.headers.forEach((v, k) => emit(io, k + ": " + v));
      io.out("\n");
      return 0;
    }
    const text = await res.text();
    io.out(text.endsWith("\n") ? text : text + "\n");
    return res.ok ? 0 : 1;
  } catch (e) {
    io.err("curl: " + url + ": 请求失败：" + e.message + "\n");
    io.err("curl: 本演示只能访问本站同源地址（浏览器的 CORS 限制）\n");
    return 7;
  }
};

CMD.wget = async (ctx, argv, io) => {
  const a = argv.slice(1);
  const oi = a.indexOf("-O");
  const url = a.filter((x) => x[0] !== "-" && x !== "-O")[0];
  if (!url) { io.err("wget: 用法: wget [-O 文件] <url>\n"); return 1; }
  try {
    const res = await ctx.sys.fetchImpl(ctx.resolveUrl(url));
    const buf = new Uint8Array(await res.arrayBuffer());
    const dest = ctx.resolve(oi >= 0 ? a[oi + 1] : ctx.vfs.baseOf(url));
    emit(io, "Connecting to " + url + " ... connected.");
    emit(io, "HTTP request sent, awaiting response... " + res.status + " " + (res.statusText || "OK"));
    emit(io, "Length: " + buf.length + " bytes  Saving to: '" + dest + "'");
    ctx.vfs.writeFile(dest, buf, 0o644);
    emit(io, "'" + dest + "' saved [" + buf.length + "/" + buf.length + "]");
    return 0;
  } catch (e) {
    io.err("wget: " + url + ": 请求失败：" + e.message + "（只能访问本站同源地址）\n");
    return 1;
  }
};

/* ---- 打包 / 压缩 / 编辑 / 真机工具 ---- */

CMD.cpio = (ctx, argv, io) => {
  const a = argv.slice(1);
  const list = a.includes("-it") || a.includes("-t");
  const extract = a.includes("-id") || (a.includes("-i") && !list);
  if (!list && !extract) { io.err("cpio: 本演示支持 cpio -it（列出）与 cpio -id（解包）\n"); return 1; }
  const members = parseCpio(enc(readAll(ctx, io)));
  if (!members.length) { io.err("cpio: 不是 newc 归档（首 6 字节应为 070701）\n"); return 1; }
  for (const m of members) {
    if (list) emit(io, m.name);
    else if (m.kind === 0o40000) ctx.vfs.mkdirp(ctx.resolve(m.name), m.mode & 0o7777);
    else if (m.kind === 0o120000) ctx.vfs.symlink(m.target, ctx.resolve(m.name));
    else ctx.vfs.add(ctx.resolve(m.name), nFile(m.data.slice(), m.mode & 0o7777, m.size));
  }
  if (!list) emit(io, members.length + " blocks");
  return 0;
};

async function gunzipImpl(ctx, argv, io) {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!f) { io.err("gunzip: 用法: gunzip <文件>\n"); return 1; }
  const n = ctx.vfs.stat(ctx.resolve(f));
  if (!n || !n.data) { io.err(f + ": 没有实体字节\n"); return 1; }
  if (typeof DecompressionStream !== "function") { io.err("gunzip: 本浏览器不支持 DecompressionStream\n"); return 1; }
  try {
    const stream = new Blob([n.data]).stream().pipeThrough(new DecompressionStream("gzip"));
    const out = new Uint8Array(await new Response(stream).arrayBuffer());
    io.out(dec(out));
    return 0;
  } catch (e) { io.err("gunzip: " + f + ": 不是 gzip 数据\n"); return 1; }
}
CMD.gunzip = gunzipImpl;
CMD.zcat = gunzipImpl;
CMD.gzip = (ctx, argv, io) => { io.err("gzip: 本演示不做压缩（gunzip/zcat 可用，走浏览器的 DecompressionStream）\n"); return 1; };

CMD.tar = (ctx, argv, io) => {
  const a = argv.slice(1);
  const file = a.find((x) => x[0] !== "-");
  if (!file || !a.some((x) => x.startsWith("-") && x.includes("t"))) {
    io.err("tar: 本演示支持 tar -tf <归档>（列出成员）\n");
    return 1;
  }
  const n = ctx.vfs.stat(ctx.resolve(file));
  if (!n || !n.data) { io.err(file + ": 没有实体字节\n"); return 1; }
  const d = n.data;
  for (let off = 0; off + 512 <= d.length;) {
    const name = dec(d.subarray(off, off + 100)).replace(/\0.*$/, "");
    if (!name) break;
    const size = parseInt(dec(d.subarray(off + 124, off + 136)).replace(/\0.*$/, "").trim(), 8) || 0;
    emit(io, name);
    off += 512 + Math.ceil(size / 512) * 512;
  }
  return 0;
};

function out2(io, ...lines) { for (const l of lines) emit(io, l); }

/* ================= nano：真全屏编辑器（要 tty；老终端里退回说明）=================
   sim.html 里是全功能的：方向键/翻页/^O 保存/^X 退出/^K 剪切/^U 粘贴/^W 搜索/^G 帮助。
   文件读写走同一个 VFS —— 存完 `cat` 得到的就是刚敲的内容。 */
class Nano {
  constructor(ctx, tty, path) {
    this.ctx = ctx; this.tty = tty; this.path = path;
    this.lines = [""];
    this.cy = 0; this.cx = 0; this.top = 0; this.left = 0;
    this.modified = false; this.message = ""; this.cut = null;
    this.prompt = null; this.rc = 0;
    if (path) {
      const n = ctx.vfs.stat(ctx.resolve(path));
      if (n && n.t === "f") {
        if (!n.data && n.src && ctx.sys.ensureData) { /* 由调用方先 ensure */ }
        if (n.data) {
          const text = dec(n.data);
          this.lines = text.replace(/\n$/, "").split("\n");
          if (!this.lines.length) this.lines = [""];
        }
      } else if (n) {
        this.message = "\"" + path + "\" 不是普通文件";
      }
    }
  }
  fit(s, w) { s = String(s); return s.length > w ? s.slice(0, w) : s + " ".repeat(w - s.length); }
  clip(s, w) { return String(s).replace(/\t/g, "        ").slice(0, w); }

  async run() {
    this.tty.write("\x1b[?1049h\x1b[?25l");
    try { await this.loop(); } finally { this.tty.write("\x1b[?1049l\x1b[?25h"); }
    return this.rc;
  }

  async loop() {
    for (;;) {
      this.draw();
      const k = await this.tty.readKey();
      if (k == null) continue;
      if (await this.handle(k)) break;
    }
  }

  async handle(k) {
    // 提示行（^O 文件名 / ^W 搜索 / 退出确认）
    if (this.prompt) return this.handlePrompt(k);
    if (k === "\x18") {                                  // ^X 退出
      if (this.modified) {
        this.prompt = { label: "Save modified buffer?  ", value: "", onDone: (v) => {
          this.prompt = null;
          const c = (v || "").trim().toLowerCase();
          if (c === "" || c.startsWith("y")) return this.saveAs(this.path || "");
          if (c.startsWith("n")) { this.rc = 0; return "quit"; }
          return null;
        }};
        return false;
      }
      return true;
    }
    if (k === "\x0f") {                                  // ^O 保存
      this.prompt = { label: "File Name to Write: ", value: this.path || "", onDone: (v) => {
        this.prompt = null;
        return this.saveAs((v || "").trim());
      }};
      return false;
    }
    if (k === "\x17") {                                  // ^W 搜索
      this.prompt = { label: "Search: ", value: "", onDone: (v) => {
        this.prompt = null;
        const term = (v || "").trim();
        if (!term) return null;
        for (let i = 0; i < this.lines.length; i++) {
          const idx = this.lines[(this.cy + i + 1) % this.lines.length].indexOf(term);
          if (idx >= 0) {
            this.cy = (this.cy + i + 1) % this.lines.length;
            this.cx = idx;
            this.message = "找到: " + term;
            this.ensureVisible();
            return null;
          }
        }
        this.message = "\"" + term + "\" 没找到";
        return null;
      }};
      return false;
    }
    if (k === "\x07") {                                  // ^G 帮助
      this.message = "方向键移动 · ^O 保存 · ^X 退出 · ^K 剪切行 · ^U 粘贴 · ^W 搜索 · ^A/^E 行首行尾 · ^Y/^V 翻页";
      return false;
    }
    if (k === "\x0b") {                                  // ^K 剪切当前行
      this.cut = this.lines.splice(this.cy, 1)[0] || "";
      if (!this.lines.length) this.lines = [""];
      this.cy = Math.min(this.cy, this.lines.length - 1);
      this.cx = 0; this.modified = true;
      this.message = "已剪切 1 行";
      return false;
    }
    if (k === "\x15") {                                  // ^U 粘贴
      if (this.cut != null) {
        this.lines.splice(this.cy, 0, this.cut);
        this.cy++; this.cx = 0; this.modified = true;
        this.message = "已粘贴 1 行";
      }
      return false;
    }
    if (k === "\x03") { this.message = "第 " + (this.cy + 1) + " 行, 第 " + (this.cx + 1) + " 列"; return false; }
    if (k === "\x01") { this.cx = 0; return false; }     // ^A 行首
    if (k === "\x05") { this.cx = this.lines[this.cy].length; return false; }   // ^E 行尾
    if (k === "\x19") { this.pageUp(); return false; }   // ^Y
    if (k === "\x16") { this.pageDown(); return false; } // ^V
    if (k === "\x0c") { this.draw(); return false; }     // ^L 重画
    // 编辑
    if (k === "\r" || k === "\n") {
      const rest = this.lines[this.cy].slice(this.cx);
      this.lines[this.cy] = this.lines[this.cy].slice(0, this.cx);
      this.lines.splice(this.cy + 1, 0, rest);
      this.cy++; this.cx = 0; this.modified = true; this.ensureVisible();
      return false;
    }
    if (k === "\x7f" || k === "\b") {
      if (this.cx > 0) {
        const ln = this.lines[this.cy];
        this.lines[this.cy] = ln.slice(0, this.cx - 1) + ln.slice(this.cx);
        this.cx--; this.modified = true;
      } else if (this.cy > 0) {
        const prev = this.lines[this.cy - 1], cur = this.lines[this.cy];
        this.cx = prev.length;
        this.lines[this.cy - 1] = prev + cur;
        this.lines.splice(this.cy, 1);
        this.cy--; this.modified = true; this.ensureVisible();
      }
      return false;
    }
    if (k === "\x1b[3~") {
      const ln = this.lines[this.cy];
      if (this.cx < ln.length) { this.lines[this.cy] = ln.slice(0, this.cx) + ln.slice(this.cx + 1); this.modified = true; }
      else if (this.cy < this.lines.length - 1) { this.lines[this.cy] += this.lines.splice(this.cy + 1, 1)[0]; this.modified = true; }
      return false;
    }
    // 移动
    if (k === "\x1b[A") { if (this.cy > 0) { this.cy--; this.clampX(); this.ensureVisible(); } return false; }
    if (k === "\x1b[B") { if (this.cy < this.lines.length - 1) { this.cy++; this.clampX(); this.ensureVisible(); } return false; }
    if (k === "\x1b[C") { if (this.cx < this.lines[this.cy].length) this.cx++; else if (this.cy < this.lines.length - 1) { this.cy++; this.cx = 0; this.ensureVisible(); } return false; }
    if (k === "\x1b[D") { if (this.cx > 0) this.cx--; else if (this.cy > 0) { this.cy--; this.cx = this.lines[this.cy].length; this.ensureVisible(); } return false; }
    if (k === "\x1b[H") { this.cx = 0; return false; }
    if (k === "\x1b[F") { this.cx = this.lines[this.cy].length; return false; }
    if (k === "\x1b[5~") { this.pageUp(); return false; }
    if (k === "\x1b[6~") { this.pageDown(); return false; }
    if (k === "\x1b") { this.message = ""; return false; }
    // 可打印字符
    if (k.length === 1 && k >= " " && k !== "\x7f") {
      const ln = this.lines[this.cy];
      this.lines[this.cy] = ln.slice(0, this.cx) + k + ln.slice(this.cx);
      this.cx++; this.modified = true;
      return false;
    }
    if (k === "\t") {
      const ln = this.lines[this.cy];
      this.lines[this.cy] = ln.slice(0, this.cx) + "\t" + ln.slice(this.cx);
      this.cx++; this.modified = true;
      return false;
    }
    return false;
  }

  handlePrompt(k) {
    if (k === "\x03" || k === "\x1b") { this.prompt = null; this.message = "已取消"; return false; }
    if (k === "\r") {
      const p = this.prompt;
      const r = p.onDone(p.value);
      return r === "quit";
    }
    if (k === "\x7f" || k === "\b") { this.prompt.value = this.prompt.value.slice(0, -1); return false; }
    if (k.length === 1 && k >= " ") { this.prompt.value += k; return false; }
    return false;
  }

  saveAs(name) {
    if (!name) { this.message = "文件名不能为空"; return null; }
    const p = this.ctx.resolve(name);
    const old = this.ctx.vfs.stat(p);
    if (old && old.t === "d") { this.message = name + " 是个目录"; return null; }
    try {
      this.ctx.vfs.writeFile(p, this.lines.join("\n") + "\n", old && old.t === "f" ? old.mode : 0o644);
    } catch (e) { this.message = "写入失败: " + e.message; return null; }
    this.path = name;
    this.modified = false;
    this.message = "[ 已写入 " + this.lines.length + " 行: " + p + " ]";
    return null;
  }
  clampX() { this.cx = Math.min(this.cx, this.lines[this.cy].length); }
  pageUp() { const h = this.pageHeight(); this.top = Math.max(0, this.top - h); this.cy = Math.max(0, this.cy - h); }
  pageDown() { const h = this.pageHeight(); this.top = Math.min(Math.max(0, this.lines.length - 1), this.top + h); this.cy = Math.min(this.lines.length - 1, this.cy + h); }
  pageHeight() { return Math.max(1, this.tty.size().rows - 3); }
  ensureVisible() {
    const h = this.pageHeight();
    if (this.cy < this.top) this.top = this.cy;
    if (this.cy >= this.top + h) this.top = this.cy - h + 1;
  }

  draw() {
    const { rows, cols } = this.tty.size();
    const body = Math.max(1, rows - 3);
    const title = "  GNU nano 8.7.1   " + (this.path || "新文件") + (this.modified ? "   Modified" : "");
    let out = "\x1b[?25l\x1b[0m\x1b[1;1H\x1b[7m" + this.fit(title, cols) + "\x1b[0m";
    if (this.cx - this.left >= cols) this.left = this.cx - cols + 1;
    if (this.cx < this.left) this.left = this.cx;
    for (let i = 0; i < body; i++) {
      const n = this.top + i;
      const txt = n < this.lines.length ? this.clip(this.lines[n], this.left + cols).slice(this.left) : "";
      out += "\x1b[" + (i + 2) + ";1H\x1b[K" + txt;
    }
    const statusRow = rows - 1;
    out += "\x1b[" + statusRow + ";1H\x1b[K\x1b[1m" + (this.prompt
      ? this.fit(this.prompt.label + this.prompt.value, cols)
      : this.fit(this.message || ("[ 第 " + (this.cy + 1) + "/" + this.lines.length + " 行, 第 " + (this.cx + 1) + " 列 ]"), cols)) + "\x1b[0m";
    const keys = this.tty.size().cols < 70
      ? "^O 存 ^X 退 ^W 搜 ^K 切 ^U 贴 ^G 帮"
      : "^G 帮助    ^O 保存    ^W 搜索    ^K 剪切    ^U 粘贴    ^X 退出";
    out += "\x1b[" + rows + ";1H\x1b[7m" + this.fit(keys, cols) + "\x1b[0m";
    const crow = 2 + (this.cy - this.top), ccol = 1 + Math.max(0, this.cx - this.left);
    out += "\x1b[" + Math.min(rows - 2, crow) + ";" + Math.min(cols, ccol) + "H\x1b[?25h";
    this.tty.write(out);
  }
}

CMD.nano = async (ctx, argv, io) => {
  const a = argv.slice(1);
  if (a.includes("--version") || a.includes("-V")) {
    out2(io, " GNU nano, version 8.7.1", " (C) 2024 the GNU nano team — 这是演示里用 JS 重写的 nano");
    return 0;
  }
  if (a.includes("--help") || a.includes("-h")) {
    out2(io, "用法: nano [选项] [[+行[,列]] 文件]",
      "  ^O 保存  ^X 退出  ^W 搜索  ^K 剪切  ^U 粘贴  ^G 帮助  ^Y/^V 翻页");
    return 0;
  }
  if (!ctx.tty) {
    const f = a.filter((x) => x[0] !== "-")[0];
    out2(io, "  GNU nano 8.7.1" + (f ? "        " + f : ""), "",
      "  这个终端不是全屏终端：nano 要在 /sim.html（整页模拟机）里跑。",
      "  本页想写文件可以用重定向：  echo hello > /tmp/a.txt", "");
    return 0;
  }
  const file = a.find((x) => x[0] !== "-" && !/^\+\d+$/.test(x));
  const at = a.find((x) => /^\+\d+/.test(x));
  if (file && ctx.sys.ensureData) await ctx.sys.ensureData(ctx.resolve(file));
  const ed = new Nano(ctx, ctx.tty, file);
  if (at) { const n = parseInt(at.slice(1), 10); if (n > 0) { ed.cy = Math.min(n - 1, ed.lines.length - 1); ed.ensureVisible(); } }
  return ed.run();
};
CMD.vi = CMD.nano; CMD.vim = CMD.nano;

CMD.w3m = (ctx, argv, io) => {
  emit(io, "w3m: 本演示不带文本浏览器（真机 /bin/w3m，1,202,896 字节静态 ELF）");
  emit(io, "     要看页面内容可以用 curl <url>");
  return 0;
};
/* ---- 分页器 / 实时视图：有 tty 就真交互，没有就整段打出来 ---- */
function pagerText(ctx, argv, io) {
  const file = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (file) return fileText(ctx, io, file);
  return readAll(ctx, io);
}

async function pager(ctx, argv, io, name) {
  const text = pagerText(ctx, argv, io);
  if (text == null) return 1;
  const tty = ctx.tty;
  if (!tty) { io.out(text.endsWith("\n") ? text : text + "\n"); return 0; }
  const lines = text.replace(/\n$/, "").split("\n");
  const { rows, cols } = tty.size();
  const page = Math.max(1, rows - 1);
  let top = 0, find = "";
  tty.write("\x1b[?1049h");
  try {
    for (;;) {
      let out = "\x1b[?25l";
      for (let i = 0; i < page; i++) {
        const n = top + i;
        out += "\x1b[" + (i + 1) + ";1H\x1b[K" + (n < lines.length ? lines[n].slice(0, cols) : "");
      }
      const pct = lines.length <= page ? "全部" : Math.round(((top + page) / lines.length) * 100) + "%";
      const st = (find ? "/" + find + "  " : "") + (name === "less" ? ":" : "") + pct +
                 (top + page >= lines.length ? "  (END)" : "  空格=下页 b=上页 q=退出 /搜索");
      out += "\x1b[" + rows + ";1H\x1b[7m" + pad(st, cols).slice(0, cols) + "\x1b[0m\x1b[?25h";
      tty.write(out);
      const k = await tty.readKey();
      if (k == null) continue;
      if (k === "q" || k === "\x03") break;
      if (k === " " || k === "f" || k === "\x1b[6~" || k === "\r") top = Math.min(Math.max(0, lines.length - page), top + page);
      else if (k === "b" || k === "\x1b[5~") top = Math.max(0, top - page);
      else if (k === "g") top = 0;
      else if (k === "G") top = Math.max(0, lines.length - page);
      else if (k === "j" || k === "\x1b[B") top = Math.min(Math.max(0, lines.length - page), top + 1);
      else if (k === "k" || k === "\x1b[A") top = Math.max(0, top - 1);
      else if (k === "/") {
        tty.write("\x1b[" + rows + ";1H\x1b[K/");
        let buf = "";
        for (;;) {
          const c = await tty.readKey();
          if (c === "\r") break;
          if (c === "\x1b" || c === "\x03") { buf = ""; break; }
          if (c === "\x7f") { buf = buf.slice(0, -1); tty.write("\b \b"); continue; }
          if (c && c.length === 1 && c >= " ") { buf += c; tty.write(c); }
        }
        find = buf;
        if (find) {
          const hit = lines.findIndex((l, i) => i >= top && l.includes(find));
          if (hit >= 0) top = Math.max(0, Math.min(hit, Math.max(0, lines.length - page)));
        }
      }
    }
  } finally { tty.write("\x1b[?1049l"); }
  return 0;
}
CMD.less = (ctx, argv, io) => pager(ctx, argv, io, "less");
CMD.more = (ctx, argv, io) => pager(ctx, argv, io, "more");

async function liveView(ctx, argv, io, kind) {
  const tty = ctx.tty;
  const a = argv.slice(1);
  const ci = a.indexOf("-n");
  const interval = ci >= 0 ? Math.max(0.2, parseFloat(a[ci + 1]) || 2) : 2;
  const cmd = a.filter((x, i) => i !== ci && i !== ci + 1).join(" ") || (kind === "top" ? "ps" : "");
  if (!cmd) { io.err(kind + ": 需要命令\n"); return 1; }
  if (!tty) {                                   // 老终端：跑一次就完
    const out = await ctx.sys.runCapture(cmd);
    io.out(out.endsWith("\n") ? out : out + "\n");
    return 0;
  }
  tty.write("\x1b[?1049h");
  try {
    for (;;) {
      const out = await ctx.sys.runCapture(cmd);
      const { rows, cols } = tty.size();
      const lines = out.replace(/\n$/, "").split("\n");
      let scr = "\x1b[?25l";
      const head = kind === "watch"
        ? "Every " + interval + "s: " + cmd + "                                     " + new Date().toTimeString().slice(0, 8) + "  (q 退出)"
        : "top — 演示进程表（q 退出）";
      scr += "\x1b[1;1H\x1b[7m" + pad(head, cols).slice(0, cols) + "\x1b[0m";
      for (let i = 0; i < rows - 2; i++) {
        scr += "\x1b[" + (i + 2) + ";1H\x1b[K" + (i < lines.length ? lines[i].slice(0, cols) : "");
      }
      scr += "\x1b[" + rows + ";1H\x1b[K\x1b[?25h";
      tty.write(scr);
      const k = await tty.readKey(kind === "watch" ? interval * 1000 : 1000);
      if (k === "q" || k === "\x03" || k === "\x18") break;
    }
  } finally { tty.write("\x1b[?1049l"); }
  return 0;
}
CMD.watch = (ctx, argv, io) => liveView(ctx, argv, io, "watch");
CMD.top = (ctx, argv, io) => liveView(ctx, argv, io, "top");

CMD.install = (ctx, argv, io) => {
  out2(io, "install: ParlzOS 装盘安装器（真机 /bin/install，982,928 字节）。",
    "         装盘要写 MBR/引导分区/ext2 真磁盘，演示环境里没有可写的块设备。");
  return 0;
};
CMD.mkfs = (ctx, argv, io) => { emit(io, "mkfs: 演示环境不格式化磁盘（真机 /bin/mkfs 自写 ext2）。"); return 0; };
CMD.login = (ctx, argv, io) => { emit(io, "login: 演示里已经以 " + ctx.env.USER + " 登录（真机走 /etc/parlz-auth）。"); return 0; };
CMD.user = (ctx, argv, io) => { emit(io, "user: 演示不提供账户管理（真机 /bin/user add/rm/upd）。"); return 0; };
// dpkg/rpm/apt/yum 四个包管理器的演示实现在下面 pkgFrontend 那一组（读同一份 feed 数据）
CMD.audio = (ctx, argv, io) => { emit(io, "audio: 演示环境没有 ALSA（真机 /bin/audio 走 /dev/snd）。"); return 0; };
CMD.pweb = (ctx, argv, io) => { emit(io, "pweb: 真机 /bin/pweb 是自研 C 静态服务器 —— 这个站就是它这一类服务器伺服的。"); return 0; };
CMD.cpfs = (ctx, argv, io) => { emit(io, "cpfs: 递归拷贝当前根到目标分区（真机装盘第 3b 步）。"); return 0; };
CMD.frpc = (ctx, argv, io) => { emit(io, "frpc: 内网穿透客户端，演示环境不联网。"); return 0; };
CMD.openvpn = (ctx, argv, io) => { emit(io, "openvpn: 需要 TUN 设备与真实网络，演示环境不可用。"); return 0; };
CMD.su = (ctx, argv, io) => { emit(io, "su: 演示里已经是 root。"); return 0; };
CMD.mknod = (ctx, argv, io) => { emit(io, "mknod: 演示的 /dev 是只读视图。"); return 0; };
CMD.mkfifo = CMD.mknod;
CMD.stty = () => 0;
CMD.who = (ctx, argv, io) => { emit(io, ctx.env.USER + "    pts/0        2026-09-28 20:00"); return 0; };
CMD.w = (ctx, argv, io) => {
  emit(io, " 20:00:00 up 0 min,  1 user,  load average: 0.00, 0.00, 0.00");
  emit(io, "USER     TTY      FROM             LOGIN@   IDLE   JCPU   PCPU WHAT");
  emit(io, ctx.env.USER + "     pts/0    -                20:00    0.00s  0.01s  0.00s parlz-sh");
  return 0;
};

/* ============ gcc：编译 C 子集 → **真 ELF64**；./a.out 由内置极小 x86-64 解释器执行 ============
   这不是"假装编译"：产出的是合法 ELF64（magic/e_type/e_machine/e_phdr/PT_LOAD 都真），
   代码段是真 x86-64 指令（write(2) + exit(2) 两个系统调用）。执行它的是本演示内置的
   极小解释器 —— 只认这条代码路径用到的 4 条指令，够跑 printf/puts + return 的子集。 */

const X86_REG = { 0: "rax", 2: "rdx", 6: "rsi", 7: "rdi" };

function buildElf(message, exitCode) {
  const msg = enc(message);
  const base = 0x400000, ehsize = 64, phentsize = 56, codeOff = ehsize + phentsize;
  const codeLen = 5 + 5 + 7 + 5 + 2 + 5 + 5 + 2;
  const msgOff = codeOff + codeLen;
  const buf = new Uint8Array(msgOff + msg.length);
  const dv = new DataView(buf.buffer);
  buf.set([0x7f, 0x45, 0x4c, 0x46, 2, 1, 1, 0], 0);
  dv.setUint16(16, 2, true); dv.setUint16(18, 0x3e, true); dv.setUint32(20, 1, true);
  dv.setBigUint64(24, BigInt(base + codeOff), true);
  dv.setBigUint64(32, BigInt(ehsize), true);
  dv.setUint16(52, ehsize, true); dv.setUint16(54, phentsize, true); dv.setUint16(56, 1, true);
  dv.setUint32(64, 1, true); dv.setUint32(68, 5, true);
  dv.setBigUint64(72, 0n, true); dv.setBigUint64(80, BigInt(base), true);
  dv.setBigUint64(88, BigInt(base), true);
  dv.setBigUint64(96, BigInt(buf.length), true); dv.setBigUint64(104, BigInt(buf.length), true);
  dv.setBigUint64(112, 0x1000n, true);
  let p = codeOff;
  buf.set([0xb8, 1, 0, 0, 0], p); p += 5;                 // mov eax, 1   (write)
  buf.set([0xbf, 1, 0, 0, 0], p); p += 5;                 // mov edi, 1   (stdout)
  buf.set([0x48, 0x8d, 0x35], p); p += 3;                 // lea rsi, [rip+rel]
  const leaAt = p; p += 4;
  buf.set([0xba], p); p += 1;
  dv.setUint32(p, msg.length, true); p += 4;              // mov edx, len
  buf.set([0x0f, 0x05], p); p += 2;                       // syscall
  buf.set([0xb8, 60, 0, 0, 0], p); p += 5;                // mov eax, 60  (exit)
  buf.set([0xbf], p); p += 1;
  dv.setUint32(p, exitCode & 0xff, true); p += 4;         // mov edi, code
  buf.set([0x0f, 0x05], p); p += 2;                       // syscall
  dv.setUint32(leaAt, (msgOff - (leaAt + 4)) >>> 0, true);
  buf.set(msg, msgOff);
  return buf;
}

// 返回退出码；认不出来返回 null
function runElf(bytes, io) {
  if (!bytes || bytes.length < 120) return null;
  if (bytes[0] !== 0x7f || bytes[1] !== 0x45 || bytes[2] !== 0x4c || bytes[3] !== 0x46) return null;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const entry = Number(dv.getBigUint64(24, true));
  const phoff = Number(dv.getBigUint64(32, true)), phnum = dv.getUint16(56, true);
  let seg = null;
  for (let i = 0; i < phnum; i++) {
    const o = phoff + i * phentsize();
    if (dv.getUint32(o, true) !== 1) continue;
    const off = Number(dv.getBigUint64(o + 8, true)), va = Number(dv.getBigUint64(o + 16, true));
    const fsz = Number(dv.getBigUint64(o + 32, true));
    if (entry >= va && entry < va + fsz) { seg = { off, va, fsz }; break; }
  }
  function phentsize() { return dv.getUint16(54, true) || 56; }
  if (!seg) return null;
  const at = (addr) => seg.off + (addr - seg.va);
  const u32 = (a) => dv.getUint32(at(a), true);
  const reg = { rax: 0, rdi: 0, rsi: 0, rdx: 0, rip: entry };
  for (let steps = 0; steps < 4096; steps++) {
    const op = bytes[at(reg.rip)];
    if (op >= 0xb8 && op <= 0xbf) {                       // mov r32, imm32
      const name = X86_REG[op - 0xb8];
      if (!name) return null;
      reg[name] = u32(reg.rip + 1);
      reg.rip += 5;
      continue;
    }
    if (op === 0x48 && bytes[at(reg.rip) + 1] === 0x8d && bytes[at(reg.rip) + 2] === 0x35) {
      reg.rsi = reg.rip + 7 + dv.getInt32(at(reg.rip) + 3, true);   // lea rsi, [rip+rel]
      reg.rip += 7;
      continue;
    }
    if (op === 0x0f && bytes[at(reg.rip) + 1] === 0x05) {           // syscall
      if (reg.rax === 1) {
        const from = at(reg.rsi);
        io.out(dec(bytes.subarray(from, from + reg.rdx)));
        reg.rip += 2;
        continue;
      }
      if (reg.rax === 60) return reg.rdi & 0xff;
      return null;
    }
    return null;                                          // 不认识的指令：不装能跑
  }
  return null;
}

CMD.gcc = (ctx, argv, io) => {
  const a = argv.slice(1);
  if (a.includes("--version")) {
    emit(io, "gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0\nCopyright (C) 2023 Free Software Foundation, Inc.");
    return 0;
  }
  if (a.includes("-dumpversion")) { emit(io, "13.3.0"); return 0; }
  if (a.includes("-dumpmachine")) { emit(io, "x86_64-linux-gnu"); return 0; }
  if (a.includes("-E")) { emit(io, "（预处理输出略：本演示的 gcc 是子集编译器）"); return 0; }
  const src = a.find((x) => /\.(c|cc|cpp)$/.test(x));
  if (!src) { io.err("gcc: 用法: gcc <源文件.c> -o <输出>\n"); return 1; }
  const oi = a.indexOf("-o");
  const out = oi >= 0 ? a[oi + 1] : "a.out";
  const text = fileText(ctx, io, src);
  if (text == null) return 1;
  if (!/int\s+main\s*\(/.test(text)) { io.err("gcc: " + src + ": 本演示只编译带 int main() 的子集\n"); return 1; }
  const parts = [];
  for (const m of text.matchAll(/(?:printf|puts)\s*\(\s*"((?:[^"\\]|\\.)*)"/g))
    parts.push(m[1].replace(/\\n/g, "\n").replace(/\\t/g, "\t").replace(/\\"/g, '"').replace(/\\\\/g, "\\"));
  const rm = text.match(/return\s+(\d+)\s*;/);
  const code = rm ? parseInt(rm[1], 10) : 0;
  const elf = buildElf(parts.join(""), code);
  ctx.vfs.writeFile(ctx.resolve(out), elf, 0o755);
  emit(io, "gcc: " + src + " → " + out + "（真 ELF64，" + elf.length + " 字节；本演示只支持 printf/puts + return 的子集）");
  return 0;
};
CMD.cc = CMD.gcc;
CMD["g++"] = CMD.gcc;
CMD["c++"] = CMD.gcc;
CMD.clang = CMD.gcc;
CMD["clang++"] = CMD.gcc;

/* ================= parlz-sh 解释器（照 userland/sh.c 复刻）================= */

const MAX_STAGES = 6, MAX_ARGV = 7, MAX_REDIR = 4;
const STATELESS_BUILTINS = ["ls", "echo", "touch", "pwd", "help", "clear", "mount"];

// 按引号外的 `;` 切段（引号内的 `;`/空白都是普通字符）
function splitSegments(line) {
  const segs = [];
  let cur = "", q = 0;
  for (const c of line) {
    if (q === 1) { cur += c; if (c === "'") q = 0; continue; }
    if (q === 2) { cur += c; if (c === '"') q = 0; continue; }
    if (c === "'" || c === '"') { q = c === "'" ? 1 : 2; cur += c; continue; }
    if (c === ";") { segs.push(cur); cur = ""; continue; }
    cur += c;
  }
  segs.push(cur);
  return segs;
}

// 按引号外的 `|` 切管道（上限 6 段；多余的并进最后一段并报错）
function splitPipes(s) {
  const parts = [];
  let cur = "", q = 0, tooMany = false;
  for (const c of s) {
    if (q === 1) { cur += c; if (c === "'") q = 0; continue; }
    if (q === 2) { cur += c; if (c === '"') q = 0; continue; }
    if (c === "'" || c === '"') { q = c === "'" ? 1 : 2; cur += c; continue; }
    if (c === "|") {
      if (parts.length + 1 >= MAX_STAGES) { tooMany = true; cur += " "; continue; }
      parts.push(cur); cur = "";
      continue;
    }
    cur += c;
  }
  parts.push(cur);
  return { stages: parts, tooMany };
}

// 分词：引号是定界符（引号内的空白/|/; 都是普通字符），空引号词整体丢弃，无转义
function tokenize(stage) {
  const words = [];
  let cur = "", inWord = false, hadSingle = false, q = 0;
  const flush = () => {
    if (inWord && !(cur === "" && !hadSingle)) words.push({ w: cur, single: hadSingle });
    cur = ""; inWord = false; hadSingle = false;
  };
  for (const c of stage) {
    if (q === 0 && (c === " " || c === "\t" || c === "\r")) { flush(); continue; }
    if (c === "'" && q !== 2) { q = q === 1 ? 0 : 1; hadSingle = true; inWord = true; continue; }
    if (c === '"' && q !== 1) { q = q === 2 ? 0 : 2; inWord = true; continue; }
    cur += c; inWord = true;
  }
  flush();
  return words;
}

// 变量展开：$NAME / ${NAME}；未定义 → 空；词里出现过单引号则整词不展开
function expand(word, env, lastRc) {
  if (word.single) return word.w;
  const s = word.w;
  let res = "", i = 0;
  while (i < s.length) {
    if (s[i] !== "$") { res += s[i++]; continue; }
    if (s[i + 1] === "?") { res += String(lastRc | 0); i += 2; continue; }   // $? → 上一条命令的退出码
    if (s[i + 1] === "{") {
      let j = i + 2, name = "";
      while (j < s.length && /[A-Za-z0-9_]/.test(s[j])) name += s[j++];
      res += env[name] !== undefined ? env[name] : "";
      if (s[j] === "}") j++;                     // 正常闭合吃掉 `}`；未闭合就留原样（真机如此）
      i = j;
      continue;
    }
    let j = i + 1, name = "";
    while (j < s.length && /[A-Za-z0-9_]/.test(s[j])) name += s[j++];
    if (!name) { res += "$"; i++; continue; }
    res += env[name] !== undefined ? env[name] : "";
    i = j;
  }
  return res;
}

class Shell {
  constructor(vfs, env, sys) {
    this.vfs = vfs; this.env = env; this.sys = sys;
    this.cwd = "/root"; this.lastRc = 0; this.isTty = true;
    this.exitRequested = false; this.exitCode = 0;
  }
  // chroot 前缀：子 shell 的绝对路径都挂到 newroot 下（chroot 命令用）
  resolve(p) {
    const abs = this.vfs.norm(p, this.cwd);
    const c = this.chroot;
    return c ? (abs === "/" ? c : c + abs) : abs;
  }
  findExec(name) {
    if (name.includes("/")) {
      const p = this.resolve(name);
      const n = this.vfs.stat(p);
      return n && n.t === "f" ? p : null;
    }
    for (const dir of String(this.env.PATH || "").split(":")) {
      const p = this.resolve((dir || ".") + "/" + name);
      const n = this.vfs.stat(p);
      if (n && n.t === "f") return p;
    }
    return null;
  }
  prompt() {
    const home = this.env.HOME || "/root";
    return (this.env.USER || "parlz") + "@" + (this.env.HOSTNAME || "parlz") + ":" +
           (this.cwd === home ? "~" : this.cwd) + "> ";
  }
  ctx() {
    const self = this;
    return {
      vfs: this.vfs, env: this.env, sys: this.sys, shell: self, isTty: this.isTty,
      get tty() { return self.sys.tty; },
      get cwd() { return self.cwd; },
      resolve: (p) => self.resolve(p),
      findExec: (n) => self.findExec(n),
      resolveUrl: (u) => self.sys.resolveUrl(u),
    };
  }

  async run(line, io) {
    const cut = String(line).split(/[\r\n]/)[0];        // 真机在第一个 CR/LF 处截断
    for (const seg of splitSegments(cut)) {
      const rc = await this.runSegment(seg, io);
      if (rc === null) continue;                        // 空段/注释：不改 $?
      this.lastRc = rc;
      if (this.exitRequested) break;
    }
    return this.lastRc;
  }

  async runSegment(seg, io) {
    let s = seg.replace(/^[ \t\r]+/, "");
    if (!s) return null;
    if (s[0] === "#") return null;                      // 段首注释

    // 行首赋值：立刻生效、活过本行、管道子进程可见（真机是 putenv）
    for (;;) {
      const m = s.match(/^([A-Za-z_][A-Za-z0-9_]*)=/);
      if (!m) break;
      let j = m[0].length, val = "", q = 0;
      while (j < s.length) {
        const c = s[j];
        if (q === 0 && (c === " " || c === "\t" || c === "|")) break;
        if (c === "'" && q !== 2) { q = q === 1 ? 0 : 1; j++; continue; }
        if (c === '"' && q !== 1) { q = q === 2 ? 0 : 2; j++; continue; }
        val += c; j++;
      }
      this.env[m[1]] = val;
      s = " ".repeat(j) + s.slice(j);
      s = s.replace(/^[ \t]+/, "");
    }

    if (!s.trim()) return null;                         // 纯赋值段：不改 $?
    const { stages, tooMany } = splitPipes(s);
    if (tooMany) io.err("sh: too many pipes (max 5)\n");
    const parsed = stages.map((st) => {
      const argv = [], redirs = [];
      const words = tokenize(st).map((w) => expand(w, this.env, this.lastRc));
      for (let i = 0; i < words.length; i++) {
        let v = words[i];
        // `>f` 与 `> f` 等价（真机两种写法都认）；缺路径的 `>` 静默丢弃
        if (v === ">" || v === ">>" || v === "<") {
          if (redirs.length >= MAX_REDIR) { io.err("sh: too many redirections\n"); continue; }
          const next = words[i + 1];
          if (next === undefined) continue;
          i++;
          redirs.push({ kind: v, path: next });
          continue;
        }
        if (v.startsWith(">") || v.startsWith("<")) {
          if (redirs.length >= MAX_REDIR) { io.err("sh: too many redirections\n"); continue; }
          if (v.startsWith(">>")) redirs.push({ kind: ">>", path: v.slice(2) });
          else if (v.startsWith(">")) redirs.push({ kind: ">", path: v.slice(1) });
          else redirs.push({ kind: "<", path: v.slice(1) });
          continue;
        }
        if (argv.length < MAX_ARGV) argv.push(v);
      }
      return { argv, redirs };
    });

    // 单段特判：stateful 内建在 shell 本体跑，重定向被静默忽略（真机如此）
    if (parsed.length === 1 && parsed[0].argv.length) {
      const name = parsed[0].argv[0];
      if (name === "cd") return this.builtinCd(parsed[0].argv, io);
      if (name === "export") return this.builtinExport(parsed[0].argv, io);
      if (name === "exit") return this.builtinExit(parsed[0].argv, io);
    }
    return this.runPipeline(parsed, io);
  }

  async runPipeline(stages, io) {
    let input = null, rc = 0;
    const lastIdx = stages.length - 1;
    for (let i = 0; i <= lastIdx; i++) {
      const st = stages[i];
      const isLast = i === lastIdx;
      let wredir = null;
      if (isLast) {
        for (const r of st.redirs) {
          if (r.kind === "<") {
            const n = this.vfs.stat(this.resolve(r.path));
            if (!n) { io.err(r.path + ": No such file or directory\n"); return 1; }
            input = n.data ? dec(n.data) : "";
          } else wredir = r;
        }
      }
      if (!st.argv.length) { input = ""; continue; }
      const capture = !isLast || !!wredir;
      let buf = "";
      const sink = {
        out: capture ? (s) => { buf += s; } : (s) => io.out(s),
        err: (s) => io.err(s),
        progress: capture ? () => {} : (s) => io.progress && io.progress(s),
        input,
      };
      rc = await this.execOne(st.argv, sink);
      if (!isLast) { input = buf; continue; }
      if (wredir) {
        const p = this.resolve(wredir.path);
        const old = wredir.kind === ">>" ? this.vfs.stat(p) : null;
        const prev = old && old.data ? dec(old.data) : "";
        try { this.vfs.writeFile(p, prev + buf, 0o644); }
        catch (e) { io.err(wredir.path + ": " + e.message + "\n"); rc = 1; }
      } else if (buf) io.out(buf);
    }
    return rc;
  }

  async execOne(argv, sink) {
    const name = argv[0];
    const sysCmds = this.sys.cmds || {};
    let impl = null;
    // 命令要读的文件：按需从 web/rootfs 取真字节（每个文件只取一次）
    if (this.sys.ensureData) {
      for (const arg of argv.slice(1)) {
        if (!arg || arg[0] === "-") continue;
        await this.sys.ensureData(this.resolve(arg));
      }
    }
    if (STATELESS_BUILTINS.includes(name) && CMD[name]) impl = CMD[name];   // 内建（无状态，管道里也能跑）
    else {
      const path = this.findExec(name);
      if (!path) {
        sink.err("sh: " + name + ": " + (name.includes("/") ? "not found or not executable" : "not found") + "\n");
        return 127;
      }
      const node = this.vfs.stat(path);
      if (name.includes("/") && !(node.mode & 0o111)) {
        sink.err("sh: " + name + ": not found or not executable\n");
        return 127;
      }
      const base = this.vfs.baseOf(path);
      impl = sysCmds[base] || CMD[base];
      if (!impl) {
        // 没有同名实现：把真字节取来，是 ELF 就交给内置的极小 x86-64 解释器
        const withData = node.data ? node : (node.src && this.sys.ensureData ? await this.sys.ensureData(path) : node);
        if (withData.data && withData.data[0] === 0x7f && withData.data[1] === 0x45) {
          const rc = runElf(withData.data, sink);
          if (rc != null) return rc;
          sink.err(base + ": 这个 ELF 用了演示解释器不认的指令（它只跑得动自带 gcc 生成的那种）\n");
          return 126;
        }
        sink.err(base + ": 演示里没有对应实现（真机 " + path + " 是可执行 ELF，" + node.size + " 字节）\n");
        return 127;
      }
    }
    let rc0 = 0;
    try { rc0 = await impl(this.ctx(), argv, sink); }
    catch (e) { sink.err(name + ": " + (e && e.message ? e.message : String(e)) + "\n"); rc0 = 1; }
    if (typeof rc0 !== "number") rc0 = 0;
    return rc0 ? (rc0 > 1 ? rc0 : 1) : 0;
  }

  builtinCd(argv, io) {
    if (argv.length < 2) { this.cwd = this.env.HOME || "/root"; return null; }   // 裸 cd 回 HOME，且不改 $?
    const target = this.resolve(argv[1]);
    const n = this.vfs.stat(target);
    if (!n || n.t !== "d") { io.err("cd: No such file or directory\n"); return 0; }  // 真机：cd 失败也是 0
    this.cwd = target;
    return 0;
  }
  builtinExport(argv, io) {
    if (argv.length < 2) return 0;
    const kv = argv[1], eq = kv.indexOf("=");
    if (eq > 0) this.env[kv.slice(0, eq)] = kv.slice(eq + 1);
    else if (this.env[kv] !== undefined) emit(io, kv + "=" + this.env[kv]);
    return 0;
  }
  builtinExit(argv, io) {
    this.exitRequested = true;
    this.exitCode = argv[1] !== undefined ? (parseInt(argv[1], 10) & 0xff) : 0;
    emit(io, "bye");
    return this.exitCode;
  }
}

/* ---- 补齐 /bin 与 /usr/bin 的其余名字 ---- */

function passwdMap(vfs) {
  const n = vfs.stat("/etc/passwd"), out = [];
  if (n && n.data) for (const ln of dec(n.data).split("\n")) {
    const c = ln.split(":");
    if (c.length > 3) out.push({ name: c[0], uid: Number(c[2]), gid: Number(c[3]), home: c[5] || "/" , shell: c[6] || "/bin/sh" });
  }
  return out;
}
function groupMap(vfs) {
  const n = vfs.stat("/etc/group"), out = [];
  if (n && n.data) for (const ln of dec(n.data).split("\n")) {
    const c = ln.split(":");
    if (c.length > 2) out.push({ name: c[0], gid: Number(c[2]) });
  }
  return out;
}
function writePasswd(vfs, rows) {
  vfs.writeFile("/etc/passwd", rows.map((r) => r.name + ":x:" + r.uid + ":" + r.gid + ":" + r.name + ":" + r.home + ":" + r.shell).join("\n") + "\n", 0o644);
}
function writeGroup(vfs, rows) {
  vfs.writeFile("/etc/group", rows.map((r) => r.name + ":x:" + r.gid + ":").join("\n") + "\n", 0o644);
}
function netState(sys) {
  if (!sys.net) sys.net = { up: true, addr: NET_ADDR, mask: NET_MASK, gw: NET_GW, dns: NET_DNS,
                            routes: [["0.0.0.0", NET_GW, "0.0.0.0", "UG", 0, 0, 0, "eth0"], ["10.0.2.0", "0.0.0.0", NET_MASK, "U", 0, 0, 0, "eth0"]] };
  return sys.net;
}
function procTable(sys) {
  if (!sys.procs) sys.procs = [ { pid: 1, user: "root", vsz: 1760, stat: "S", cmd: "/sbin/init" },
                                { pid: 2, user: "root", vsz: 0, stat: "S", cmd: "[kthreadd]" },
                                { pid: 43, user: "guest", vsz: 2296, stat: "S", cmd: "/bin/parlz-sh" } ];
  return sys.procs;
}

CMD.chown = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (a.length < 2) { io.err("chown: 用法: chown 用户[:组] 文件…\n"); return 1; }
  const [owner, group] = a[0].split(":");
  const users = passwdMap(ctx.vfs), groups = groupMap(ctx.vfs);
  const u = /^\d+$/.test(owner) ? { name: owner, uid: Number(owner) } : users.find((x) => x.name === owner);
  if (!u) { io.err("chown: invalid user: '" + owner + "'\n"); return 1; }
  let g = null;
  if (group) { g = /^\d+$/.test(group) ? { gid: Number(group) } : groups.find((x) => x.name === group); if (!g) { io.err("chown: invalid group: '" + group + "'\n"); return 1; } }
  let rc = 0;
  for (const f of a.slice(1)) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n) { io.err("chown: " + f + ": No such file or directory\n"); rc = 1; continue; }
    n.uid = u.uid;
    if (g) n.gid = g.gid;
  }
  return rc;
};
CMD.chgrp = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (a.length < 2) { io.err("chgrp: 用法: chgrp 组 文件…\n"); return 1; }
  const groups = groupMap(ctx.vfs);
  const g = /^\d+$/.test(a[0]) ? { gid: Number(a[0]) } : groups.find((x) => x.name === a[0]);
  if (!g) { io.err("chgrp: invalid group: '" + a[0] + "'\n"); return 1; }
  let rc = 0;
  for (const f of a.slice(1)) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n) { io.err("chgrp: " + f + ": No such file or directory\n"); rc = 1; continue; }
    n.gid = g.gid;
  }
  return rc;
};

CMD.dd = async (ctx, argv, io) => {
  const o = { bs: 512, count: -1, skip: 0, seek: 0 };
  for (const a of argv.slice(1)) {
    const m = /^([a-z]+)=(.*)$/.exec(a);
    if (m) o[m[1]] = /^\d+$/.test(m[2]) ? Number(m[2]) : m[2];
  }
  let src;
  if (o.if) {
    const n = await ctx.sys.ensureData(ctx.resolve(o.if));
    if (!n || !n.data) { io.err("dd: " + o.if + ": 打不开（或本演示没有实体字节）\n"); return 1; }
    src = n.data;
  } else src = enc(readAll(ctx, io));
  src = src.subarray(Math.min(o.skip * o.bs, src.length));
  const nrec = o.count >= 0 ? Math.min(o.count, Math.floor(src.length / o.bs)) : Math.floor(src.length / o.bs);
  const bytes = nrec * o.bs;
  const chunk = src.subarray(0, bytes);
  if (o.of) {
    const p = ctx.resolve(o.of);
    const old = o.seek ? ctx.vfs.stat(p) : null;
    const base = old && old.data ? old.data.slice() : new Uint8Array(0);
    const out = new Uint8Array(Math.max(base.length, o.seek * o.bs + bytes));
    out.set(base); out.set(chunk, o.seek * o.bs);
    ctx.vfs.writeFile(p, out, 0o644);
  } else if (chunk.includes(0)) {
    io.out("（二进制，不往终端刷 " + bytes + " 字节；用 of= 写文件或 od 看内容）\n");
  } else io.out(dec(chunk));
  emit(io, nrec + "+0 records in");
  emit(io, nrec + "+0 records out");
  emit(io, bytes + " bytes (" + human(bytes) + ") copied, 0.001 s, " + Math.max(1, Math.round(bytes / 1024)) + " kB/s");
  return 0;
};

CMD.mktemp = (ctx, argv, io) => {
  const dir = argv.includes("-d");
  const tpl = argv.slice(1).filter((x) => x[0] !== "-")[0] || "/tmp/tmp.XXXXXX";
  const name = tpl.replace(/X+$/, () => Math.random().toString(36).slice(2, 8).padEnd(6, "0").slice(0, 6));
  const p = ctx.resolve(name);
  if (dir) ctx.vfs.mkdirp(p, 0o700);
  else ctx.vfs.writeFile(p, new Uint8Array(0), 0o600);
  emit(io, p);
  return 0;
};

const SIGNALS = { 1: "HUP", 2: "INT", 3: "QUIT", 6: "ABRT", 9: "KILL", 11: "SEGV", 13: "PIPE", 14: "ALRM", 15: "TERM", 17: "CHLD", 18: "CONT", 19: "STOP", 20: "TSTP" };
function killImpl(ctx, argv, io, byName) {
  const a = argv.slice(1);
  if (a[0] === "-l") { emit(io, Object.keys(SIGNALS).map((k) => SIGNALS[k]).join(" ")); return 0; }
  let sig = 15, rest = a;
  if (a[0] && a[0][0] === "-" && a[0].length > 1) {
    const s = a[0].slice(1);
    sig = /^\d+$/.test(s) ? Number(s) : (Object.keys(SIGNALS).find((k) => SIGNALS[k] === s.toUpperCase()) || 15);
    rest = a.slice(1);
  }
  if (!rest.length) { io.err((byName ? "killall" : "kill") + ": 需要进程号或名字\n"); return 1; }
  const procs = procTable(ctx.sys);
  let rc = 0;
  for (const t of rest) {
    const hits = byName ? procs.filter((p) => p.cmd.split("/").pop() === t) : procs.filter((p) => p.pid === Number(t));
    if (!hits.length) { io.err((byName ? "killall: " + t + ": no process killed" : "kill: (" + t + "): No such process") + "\n"); rc = 1; continue; }
    for (const h of hits) {
      if (h.pid === 1) { io.err("kill: (1): Operation not permitted\n"); rc = 1; continue; }
      ctx.sys.procs = procs.filter((p) => p !== h);
    }
  }
  return rc;
}
CMD.kill = (ctx, argv, io) => killImpl(ctx, argv, io, false);
CMD.killall = (ctx, argv, io) => killImpl(ctx, argv, io, true);

CMD.lsmod = (ctx, argv, io) => { emit(io, "Module                  Size  Used by"); return 0; };
CMD.insmod = (ctx, argv, io) => { io.err("insmod: 本演示内核不含可加载模块（真机 CONFIG_MODULES 同样全内建）\n"); return 1; };
CMD.rmmod = (ctx, argv, io) => { io.err("rmmod: 没有已加载的模块\n"); return 1; };
CMD.modprobe = (ctx, argv, io) => { io.err("modprobe: FATAL: Module " + (argv[1] || "") + " not found in directory /lib/modules\n"); return 1; };
CMD.depmod = (ctx, argv, io) => { emit(io, "depmod: 没有 /lib/modules（全内建内核）"); return 0; };

CMD.ifdown = (ctx, argv, io) => { netState(ctx.sys).up = false; return 0; };
CMD.ifup = (ctx, argv, io) => {
  const net = netState(ctx.sys);
  net.up = true; net.addr = NET_ADDR;
  emit(io, "udhcpc: lease of " + NET_ADDR + " obtained, lease time 86400");
  return 0;
};
CMD.udhcpc = (ctx, argv, io) => {
  const net = netState(ctx.sys);
  net.up = true; net.addr = NET_ADDR;
  emit(io, "udhcpc: started, v1.36.1");
  emit(io, "udhcpc: broadcasting discover");
  emit(io, "udhcpc: broadcasting select for " + NET_ADDR + ", server 10.0.2.3");
  emit(io, "udhcpc: lease of " + NET_ADDR + " obtained from 10.0.2.3, lease time 86400");
  return 0;
};
CMD.route = (ctx, argv, io) => {
  const net = netState(ctx.sys);
  const a = argv.slice(1);
  if (a[0] === "add" || a[0] === "del") {
    const gw = a[a.indexOf("gw") + 1];
    const dev = a[a.indexOf("dev") + 1] || "eth0";
    if (a[0] === "add") net.routes.unshift(["0.0.0.0", gw || "0.0.0.0", "0.0.0.0", "UG", 0, 0, 0, dev]);
    else net.routes = net.routes.filter((r) => !(r[3] === "UG"));
    return 0;
  }
  if (a.includes("-n")) {
    emit(io, "Kernel IP routing table");
    emit(io, "Destination     Gateway         Genmask         Flags Metric Ref    Use Iface");
    for (const r of net.routes) emit(io, pad(r[0], 16) + pad(r[1], 16) + pad(r[2], 16) + pad(r[3], 7) + pad(r[4], 7) + pad(r[5], 7) + pad(r[6], 7) + r[7]);
    return 0;
  }
  emit(io, "Kernel IP routing table");
  emit(io, "Destination     Gateway         Genmask         Flags Metric Ref    Use Iface");
  for (const r of net.routes) emit(io, pad(r[0], 16) + pad(r[1], 16) + pad(r[2], 16) + pad(r[3], 7) + pad(r[4], 7) + pad(r[5], 7) + pad(r[6], 7) + r[7]);
  return 0;
};

CMD.adduser = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  const name = a[a.length - 1];
  if (!name || name === argv[0]) { io.err("adduser: 用法: adduser [-D] [-h 家目录] [-s shell] 用户名\n"); return 1; }
  const users = passwdMap(ctx.vfs);
  if (users.find((u) => u.name === name)) { io.err("adduser: user " + name + " is in use\n"); return 1; }
  const groups = groupMap(ctx.vfs);
  const gid = Math.max(1000, ...groups.map((g) => g.gid + 1));
  groups.push({ name, gid });
  writeGroup(ctx.vfs, groups);
  const home = argv.includes("-h") ? argv[argv.indexOf("-h") + 1] : "/home/" + name;
  const shell = argv.includes("-s") ? argv[argv.indexOf("-s") + 1] : "/bin/parlz-sh";
  const uid = Math.max(1000, ...users.map((u) => u.uid + 1));
  users.push({ name, uid, gid, home, shell });
  writePasswd(ctx.vfs, users);
  ctx.vfs.mkdirp(home, 0o755);
  return 0;
};
CMD.deluser = (ctx, argv, io) => {
  const name = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const users = passwdMap(ctx.vfs);
  if (!users.find((u) => u.name === name)) { io.err("deluser: 用户 " + (name || "") + " 不存在\n"); return 1; }
  writePasswd(ctx.vfs, users.filter((u) => u.name !== name));
  writeGroup(ctx.vfs, groupMap(ctx.vfs).filter((g) => g.name !== name));
  ctx.vfs.rmrf("/home/" + name);
  return 0;
};
CMD.addgroup = (ctx, argv, io) => {
  const name = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const groups = groupMap(ctx.vfs);
  if (groups.find((g) => g.name === name)) { io.err("addgroup: group " + name + " is in use\n"); return 1; }
  groups.push({ name, gid: Math.max(1000, ...groups.map((g) => g.gid + 1)) });
  writeGroup(ctx.vfs, groups);
  return 0;
};
CMD.delgroup = (ctx, argv, io) => {
  const name = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const groups = groupMap(ctx.vfs);
  if (!groups.find((g) => g.name === name)) { io.err("delgroup: 组 " + (name || "") + " 不存在\n"); return 1; }
  writeGroup(ctx.vfs, groups.filter((g) => g.name !== name));
  return 0;
};
CMD["add-shell"] = (ctx, argv, io) => {
  const sh = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!sh) { io.err("add-shell: 用法: add-shell <shell>\n"); return 1; }
  const n = ctx.vfs.stat("/etc/shells");
  const cur = n && n.data ? dec(n.data) : "# /etc/shells\n";
  if (!cur.includes(sh + "\n")) ctx.vfs.writeFile("/etc/shells", cur.replace(/\n?$/, "\n") + sh + "\n", 0o644);
  return 0;
};

CMD.setsid = async (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  if (!a.length) { io.err("setsid: 需要命令\n"); return 1; }
  return ctx.shell.execOne(a, io);
};

CMD.chroot = async (ctx, argv, io) => {
  const a = argv.slice(1);
  const root = a[0];
  if (!root) { io.err("chroot: 用法: chroot 新根 [命令]\n"); return 125; }
  const p = ctx.resolve(root);
  const n = ctx.vfs.stat(p);
  if (!n || n.t !== "d") { io.err("chroot: cannot change root directory to '" + root + "': No such file or directory\n"); return 125; }
  if (a.length < 2) { io.err("chroot: 演示里请给一个命令（真机上是 /bin/parlz-sh）\n"); return 125; }
  const sub = new Shell(ctx.vfs, ctx.env, ctx.sys);
  sub.chroot = p; sub.cwd = "/";
  sub.isTty = ctx.isTty;
  return sub.run(a.slice(1).join(" "), io);
};

CMD.time = async (ctx, argv, io) => {
  const a = argv.slice(1);
  if (!a.length) { io.err("time: 需要命令\n"); return 1; }
  const t0 = Date.now();
  const rc = await ctx.shell.execOne(a, io);
  const ms = Date.now() - t0;
  emit(io, "");
  emit(io, "real\t0m" + (ms / 1000).toFixed(3) + "s");
  emit(io, "user\t0m" + (ms / 1000 * 0.6).toFixed(3) + "s");
  emit(io, "sys\t0m" + (ms / 1000 * 0.4).toFixed(3) + "s");
  return rc;
};

CMD.mdev = (ctx, argv, io) => { emit(io, "mdev: /dev 是内核 devtmpfs 自动填的（真机 CONFIG_DEVTMPFS_MOUNT=y）"); return 0; };
CMD.getty = (ctx, argv, io) => { emit(io, "getty: 真机上由 init 起在 tty1 上；这里已经在终端里了"); return 0; };
CMD.crond = (ctx, argv, io) => { emit(io, "crond: 演示环境不起定时任务（真机上是 busybox crond）"); return 0; };
CMD.pivot_root = (ctx, argv, io) => { emit(io, "pivot_root: 需要真块设备与 mount namespace（真机装机/自启时用）"); return 1; };
CMD.switch_root = (ctx, argv, io) => { emit(io, "switch_root: 需要真块设备（真机 initramfs 换根时用）"); return 1; };
CMD.init = (ctx, argv, io) => { emit(io, "init: PID 1 已经在跑（" + (procTable(ctx.sys).find((p) => p.pid === 1) || {}).cmd + "）"); return 0; };

CMD.pms = (ctx, argv, io) => {
  const sub = argv[1] || "help";
  if (sub === "help" || sub === "--help" || sub === "-h") {
    io.out(
      "用法: pms <命令> [选项]\n" +
      "  probe                    探测邮箱服务器\n" +
      "  login                    登录（IMAP）\n" +
      "  list [页码]              列出邮件\n" +
      "  read <号>                读一封\n" +
      "  send -t <收件人> -s <主题> -b <正文>\n" +
      "  att -F <文件>            带附件\n" +
      "  search <关键词> | drafts | spam | emptyjunk | emptytrash\n" +
      "  选项: -u/--user  -p/--pass  --imap-tls  --smtp-tls  --no-starttls  --insecure\n");
    return 0;
  }
  emit(io, "pms: 演示环境没有邮箱账号也不联网（真机上是自研的 IMAP/SMTP 客户端，静态链 OpenSSL）。");
  return 1;
};

// 四个包管理器在演示里都接同一份 feed 数据（真机上 dpkg/rpm/apt/yum 读的是
// .deb/.rpm/Debian Packages/repodata，本站演示没有那些仓库）
// 版本行逐字对齐真机 `<cmd> --version` 的第一行 —— 演示不许自己编一套版本号
const PKG_VER_LINE = {
  dpkg: "dpkg -Parlz/1.0.0 (基于 pkgcore 的移植实现)",
  rpm: "RPM 包管理器(Parlz 移植实现) 1.0.0",
  apt: "apt 1.0.0-parlz (Parlz 移植实现; 解包安装由 dpkg 完成)",
  yum: "yum 1.0.0-parlz (Parlz 移植实现; 解包安装由 rpm 完成)",
};

async function pkgFrontend(ctx, argv, io, name, backend) {
  const sub = argv[1];
  if (sub === "--version" || sub === "version" || !sub) {
    emit(io, PKG_VER_LINE[name]);
    return 0;
  }
  if (sub === "update" || sub === "makecache" || sub === "list" ||
      sub === "available" || sub === "search") {
    emit(io, `（演示: ${name} ${sub} 没有真的 deb/rpm 仓库可读, 下面列的是官网 .pm 镜像站的内容）`);
    return ctx.sys.cmds.pm(ctx, ["pm", "available"], io);
  }
  if (sub === "install") {
    emit(io, `（演示: ${name} 装不了 .deb/.rpm —— 本站演示只有 .pm; 转交 pm）`);
    return ctx.sys.cmds.pm(ctx, ["pm", "install", argv[2]], io);
  }
  emit(io, `${name}: 演示里请用 pm（真机 ${name} 是自己移植的实现, 包格式与索引沿用上游公开规范, 不链接上游代码）`);
  return 0;
}

CMD.dpkg = (ctx, argv, io) => pkgFrontend(ctx, argv, io, "dpkg", "pkgcore");
CMD.apt = (ctx, argv, io) => pkgFrontend(ctx, argv, io, "apt", "dpkg");
CMD.rpm = (ctx, argv, io) => pkgFrontend(ctx, argv, io, "rpm", "pkgcore");
CMD.yum = (ctx, argv, io) => pkgFrontend(ctx, argv, io, "yum", "rpm");

CMD.user = (ctx, argv, io) => {
  const sub = argv[1], name = argv[2];
  if (sub === "list" || !sub) {
    for (const u of passwdMap(ctx.vfs)) emit(io, u.name + "  uid=" + u.uid + "  gid=" + u.gid + "  home=" + u.home);
    return 0;
  }
  if (sub === "add") {
    if (!name) { io.err("user: 用法: user add <名>\n"); return 1; }
    return CMD.adduser(ctx, ["adduser", "-D", name], io);
  }
  if (sub === "rm") {
    if (!name) { io.err("user: 用法: user rm <名>\n"); return 1; }
    return CMD.deluser(ctx, ["deluser", name], io);
  }
  if (sub === "upd") {
    emit(io, "user: 演示改不了口令 —— 真机把 SHA-512 crypt 的散列写进 /etc/parlz-auth（libxcrypt）");
    return 1;
  }
  io.err("user: 未知子命令 " + sub + "（list/add/rm/upd）\n");
  return 1;
};

CMD.busybox = async (ctx, argv, io) => {
  const a = argv.slice(1);
  // applet 表 = 现在系统里指向 busybox 的那些软链（真机就是靠建链给出 applet 的）
  const applets = [];
  for (const dir of ["/bin", "/usr/bin", "/sbin", "/usr/sbin"]) {
    const d = ctx.vfs.dir(dir);
    if (!d) continue;
    for (const [name, kid] of d.kids) {
      if (kid.t !== "l") continue;
      const target = kid.link.startsWith("/") ? kid.link : ctx.vfs.norm(kid.link, dir);
      if (target === "/sbin/busybox") applets.push(name);
    }
  }
  if (!a.length || a[0] === "--list") { for (const n of applets.sort()) emit(io, n); return 0; }
  if (a[0] === "--help") {
    emit(io, "BusyBox v1.36.1 (ParlzOS) multi-call binary.");
    emit(io, "");
    emit(io, "用法: busybox [功能 [参数]]…");
    return 0;
  }
  const impl = (ctx.sys.cmds && ctx.sys.cmds[a[0]]) || CMD[a[0]];
  if (!impl) { io.err("busybox: " + a[0] + ": applet not found\n"); return 127; }
  return impl(ctx, [a[0]].concat(a.slice(1)), io);
};

CMD["parlz-sh"] = async (ctx, argv, io) => {
  const a = argv.slice(1);
  const ci = a.indexOf("-c");
  const sub = new Shell(ctx.vfs, ctx.env, ctx.sys);
  sub.cwd = ctx.cwd; sub.isTty = ctx.isTty;
  if (ci >= 0) return sub.run(a[ci + 1] || "", io);
  const script = a.find((x) => x[0] !== "-");
  if (script) {
    const n = ctx.vfs.stat(ctx.resolve(script));
    if (!n || !n.data) { io.err("parlz-sh: " + script + ": No such file or directory\n"); return 127; }
    let rc = 0;
    for (const ln of dec(n.data).split("\n")) rc = await sub.run(ln, io);
    return rc;
  }
  emit(io, "Parlz shell (parlz-sh) —— 现在跑的就是它");
  return 0;
};

/* ---- busybox applet 补齐：常用的一批给真行为 ---- */

CMD.true = () => 0;
CMD.false = () => 1;
CMD[":"] = () => 0;
CMD.split = (ctx, argv, io) => {
  const a = argv.slice(1);
  const nLines = parseInt(a.find((x) => /^\d+$/.test(x)) || "1000", 10);
  const prefix = a.filter((x) => !/^-/.test(x) && !/^\d+$/.test(x))[0] || "x";
  const f = a.find((x) => /\.|-/.test(x) && !/^\d+$/.test(x) && x !== prefix);
  let text;
  if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  let idx = 0;
  for (let i = 0; i < lines.length; i += nLines) {
    const suffix = String.fromCharCode(97 + Math.floor(idx / 26) % 26) + String.fromCharCode(97 + idx % 26);
    ctx.vfs.writeFile(ctx.resolve(prefix + suffix), lines.slice(i, i + nLines).join("\n") + "\n", 0o644);
    idx++;
  }
  return 0;
};
CMD.truncate = (ctx, argv, io) => {
  const a = argv.slice(1);
  const si = a.indexOf("-s");
  const size = si >= 0 ? parseInt(a[si + 1], 10) : 0;
  let rc = 0;
  for (const f of a.filter((x) => x[0] !== "-" && !/^\d+$/.test(x))) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n || n.t !== "f") { io.err("truncate: " + f + ": No such file or directory\n"); rc = 1; continue; }
    const data = n.data ? n.data.slice() : new Uint8Array(0);
    const out = new Uint8Array(Math.max(0, size));
    out.set(data.subarray(0, Math.min(data.length, size)));
    ctx.vfs.writeFile(ctx.resolve(f), out, n.mode);
  }
  return rc;
};
CMD.boot = (ctx, argv, io) => { emit(io, "boot: 真机上这是引导修复工具（本页顶部的启动日志就是它之后的输出）"); return 0; };
CMD.unlink = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!f) { io.err("unlink: 缺少操作数\n"); return 1; }
  return ctx.vfs.rmrf(ctx.resolve(f)) ? 0 : (io.err("unlink: " + f + ": No such file or directory\n"), 1);
};
const lineOp = (fn, name) => (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text;
  if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  fn(lines).forEach((l) => emit(io, l));
  return 0;
};
CMD.tac = lineOp((ls) => ls.slice().reverse(), "tac");
CMD.rev = lineOp((ls) => ls.map((l) => [...l].reverse().join("")), "rev");
CMD.shuf = lineOp((ls) => ls.map((v) => [Math.random(), v]).sort((a, b) => a[0] - b[0]).map((p) => p[1]), "shuf");
CMD.fold = (ctx, argv, io) => {
  const w = argv.includes("-w") ? parseInt(argv[argv.indexOf("-w") + 1], 10) : 80;
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text; if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  for (const ln of text.split("\n")) for (let i = 0; i < ln.length; i += w) emit(io, ln.slice(i, i + w));
  return 0;
};
CMD.expand = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text; if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  io.out(text.replace(/\t/g, "        "));
  return 0;
};
CMD.unexpand = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let text; if (f) { text = fileText(ctx, io, f); if (text == null) return 1; } else text = readAll(ctx, io);
  io.out(text.replace(/ {8}/g, "\t"));
  return 0;
};
CMD.paste = (ctx, argv, io) => {
  const files = argv.slice(1).filter((x) => x[0] !== "-");
  const cols = files.map((f) => { const t = fileText(ctx, io, f); return t == null ? [] : t.replace(/\n$/, "").split("\n"); });
  const n = Math.max(0, ...cols.map((c) => c.length));
  for (let i = 0; i < n; i++) emit(io, cols.map((c) => c[i] || "").join("\t"));
  return 0;
};
CMD.cmp = (ctx, argv, io) => {
  const [a, b] = argv.slice(1).filter((x) => x[0] !== "-");
  const ta = fileText(ctx, io, a), tb = fileText(ctx, io, b);
  if (ta == null || tb == null) return 2;
  if (ta === tb) return 0;
  const la = ta.split("\n"), lb = tb.split("\n");
  for (let i = 0; i < Math.max(la.length, lb.length); i++)
    if (la[i] !== lb[i]) { emit(io, a + " " + b + " differ: byte " + ((la[i] || "").length + 1) + ", line " + (i + 1)); break; }
  return 1;
};
CMD.comm = (ctx, argv, io) => {
  const [a, b] = argv.slice(1).filter((x) => x[0] !== "-");
  const setA = new Set((fileText(ctx, io, a) || "").split("\n").filter(Boolean));
  const setB = new Set((fileText(ctx, io, b) || "").split("\n").filter(Boolean));
  for (const l of [...setA].filter((x) => !setB.has(x)).sort()) emit(io, l);
  for (const l of [...setB].filter((x) => !setA.has(x)).sort()) emit(io, "\t" + l);
  for (const l of [...setA].filter((x) => setB.has(x)).sort()) emit(io, "\t\t" + l);
  return 0;
};
CMD.diff = (ctx, argv, io) => {
  const [a, b] = argv.slice(1).filter((x) => x[0] !== "-");
  const ta = fileText(ctx, io, a), tb = fileText(ctx, io, b);
  if (ta == null || tb == null) return 2;
  if (ta === tb) return 0;
  emit(io, "--- " + a);
  emit(io, "+++ " + b);
  const la = ta.replace(/\n$/, "").split("\n"), lb = tb.replace(/\n$/, "").split("\n");
  for (let i = 0; i < Math.max(la.length, lb.length); i++) {
    if (la[i] === lb[i]) continue;
    if (la[i] !== undefined) emit(io, "-" + la[i]);
    if (lb[i] !== undefined) emit(io, "+" + lb[i]);
  }
  return 1;
};
CMD.xargs = async (ctx, argv, io) => {
  const a = argv.slice(1);
  const cmd = a.length ? a : ["echo"];
  const words = readAll(ctx, io).split(/\s+/).filter(Boolean);
  return ctx.shell.execOne(cmd.concat(words), io);
};
CMD.xxd = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let data;
  if (f) { const n = ctx.vfs.stat(ctx.resolve(f)); if (!n || !n.data) { io.err("xxd: " + f + ": 没有实体字节\n"); return 1; } data = n.data; }
  else data = enc(readAll(ctx, io));
  const lim = Math.min(data.length, 256);
  for (let i = 0; i < lim; i += 16) {
    const c = data.subarray(i, i + 16);
    const hex = [...c].map((x) => x.toString(16).padStart(2, "0")).join("");
    const txt = [...c].map((x) => (x >= 32 && x < 127 ? String.fromCharCode(x) : ".")).join("");
    emit(io, i.toString(16).padStart(8, "0") + ": " + pad(hex, 32).replace(/(.{4})(?=.)/g, "$1 ") + "  " + txt);
  }
  return 0;
};
CMD.crc32 = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let data;
  if (f) { const n = ctx.vfs.stat(ctx.resolve(f)); if (!n || !n.data) { io.err("crc32: " + f + ": 没有实体字节\n"); return 1; } data = n.data; }
  else data = enc(readAll(ctx, io));
  let c = 0xffffffff;
  for (const byte of data) { c ^= byte; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); }
  emit(io, ((c ^ 0xffffffff) >>> 0).toString(16));
  return 0;
};
CMD.cksum = (ctx, argv, io) => {
  const f = argv.slice(1).filter((x) => x[0] !== "-")[0];
  let data;
  if (f) { const n = ctx.vfs.stat(ctx.resolve(f)); if (!n || !n.data) { io.err("cksum: " + f + ": 没有实体字节\n"); return 1; } data = n.data; }
  else data = enc(readAll(ctx, io));
  let sum = 0;
  for (const b of data) sum = (sum + b) >>> 0;
  emit(io, sum + " " + data.length + (f ? " " + f : ""));
  return 0;
};
CMD.hostid = (ctx, argv, io) => { emit(io, "007f0101"); return 0; };
CMD.arch = (ctx, argv, io) => { emit(io, "x86_64"); return 0; };
CMD.tty = (ctx, argv, io) => { emit(io, "/dev/console"); return 0; };
CMD.ttysize = (ctx, argv, io) => { emit(io, "80 24"); return 0; };
CMD.usleep = (ctx, argv, io) => 0;
CMD.nologin = (ctx, argv, io) => { emit(io, "该账户当前不可登录"); return 1; };
CMD.mesg = (ctx, argv, io) => { emit(io, "is y"); return 0; };
CMD.groups = (ctx, argv, io) => { emit(io, "root"); return 0; };
CMD.dos2unix = (ctx, argv, io) => { const f = argv.slice(1).filter((x) => x[0] !== "-")[0]; const t = fileText(ctx, io, f); if (t == null) return 1; ctx.vfs.writeFile(ctx.resolve(f), t.replace(/\r\n/g, "\n"), 0o644); return 0; };
CMD.unix2dos = (ctx, argv, io) => { const f = argv.slice(1).filter((x) => x[0] !== "-")[0]; const t = fileText(ctx, io, f); if (t == null) return 1; ctx.vfs.writeFile(ctx.resolve(f), t.replace(/\n/g, "\r\n"), 0o644); return 0; };
CMD.nohup = async (ctx, argv, io) => { emit(io, "nohup: appending output to 'nohup.out'"); const a = argv.slice(1); if (!a.length) return 1; return ctx.shell.execOne(a, io); };
CMD.nice = async (ctx, argv, io) => { const a = argv.slice(1).filter((x) => !/^-n\d+$/.test(x) && x[0] !== "-"); if (!a.length) { emit(io, "0"); return 0; } return ctx.shell.execOne(a, io); };
CMD.renice = (ctx, argv, io) => { emit(io, "renice: 演示里没有真调度优先级"); return 0; };
CMD.timeout = async (ctx, argv, io) => { const a = argv.slice(1).filter((x) => x[0] !== "-"); a.shift(); if (!a.length) return 1; return ctx.shell.execOne(a, io); };
CMD.flock = async (ctx, argv, io) => { const a = argv.slice(1).filter((x) => x[0] !== "-"); a.shift(); if (!a.length) return 1; return ctx.shell.execOne(a, io); };
CMD.pidof = (ctx, argv, io) => {
  const name = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const hits = procTable(ctx.sys).filter((p) => p.cmd.split("/").pop() === name);
  if (!hits.length) return 1;
  emit(io, hits.map((h) => h.pid).join(" "));
  return 0;
};
CMD.pgrep = (ctx, argv, io) => {
  const name = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const hits = procTable(ctx.sys).filter((p) => p.cmd.includes(name));
  if (!hits.length) return 1;
  for (const h of hits) emit(io, String(h.pid));
  return 0;
};
CMD.pkill = (ctx, argv, io) => killImpl(ctx,["killall"].concat(argv.slice(1)), io, true);
CMD.pstree = (ctx, argv, io) => {
  emit(io, "init(1)-+-kthreadd(2)");
  emit(io, "        `-parlz-sh(43)");
  return 0;
};
CMD.pwdx = (ctx, argv, io) => { emit(io, String(argv[1] || 43) + ": " + ctx.cwd); return 0; };
CMD.lsof = (ctx, argv, io) => {
  emit(io, "COMMAND    PID USER   FD   TYPE DEVICE SIZE/OFF NODE NAME");
  emit(io, "parlz-sh    43 root  cwd    DIR  0,72     4096    2 " + ctx.cwd);
  emit(io, "parlz-sh    43 root    0u   CHR    5,0      0t0    1 /dev/console");
  emit(io, "parlz-sh    43 root    1u   CHR    5,0      0t0    1 /dev/console");
  return 0;
};
CMD.lsblk = (ctx, argv, io) => {
  emit(io, "NAME   MAJ:MIN RM  SIZE RO TYPE MOUNTPOINTS");
  emit(io, "vda    254:0    0  512M  0 disk ");
  emit(io, "├─vda1 254:1    0   64M  0 part /boot");
  emit(io, "└─vda2 254:2    0  448M  0 part /");
  return 0;
};
CMD.fdisk = (ctx, argv, io) => {
  const dev = argv.slice(1).filter((x) => x[0] !== "-")[0] || "/dev/vda";
  emit(io, "Disk " + dev + ": 512 MiB, 536870912 bytes, 1048576 sectors");
  emit(io, "Units: sectors of 1 * 512 = 512 bytes");
  emit(io, "");
  emit(io, "Device     Boot Start    End Sectors  Size Id Type");
  emit(io, dev + "1  *     2048 133119  131072   64M  e W95 FAT16 (LBA)");
  emit(io, dev + "2      133120 1048574  915454  447M 83 Linux");
  return 0;
};
CMD.blkid = (ctx, argv, io) => {
  emit(io, "/dev/vda1: LABEL=\"PARLZBOOT\" UUID=\"1234-ABCD\" TYPE=\"vfat\" PARTUUID=\"00000000-01\"");
  emit(io, "/dev/vda2: LABEL=\"parlz-root\" UUID=\"3f8b1c2e-7a41-4d9b-8e55-2c6f0a1d4b77\" TYPE=\"ext4\" PARTUUID=\"00000000-02\"");
  return 0;
};
CMD.mountpoint = (ctx, argv, io) => {
  const p = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const q = ctx.resolve(p);
  if (ctx.vfs.mounts.some((m) => m[1] === q)) { emit(io, q + " is a mountpoint"); return 0; }
  if (argv.includes("-q")) return 1;
  emit(io, q + " is not a mountpoint");
  return 1;
};
CMD["run-parts"] = async (ctx, argv, io) => {
  const dir = ctx.resolve(argv.slice(1).filter((x) => x[0] !== "-")[0] || "");
  const d = ctx.vfs.dir(dir);
  if (!d) { io.err("run-parts: " + dir + ": No such file or directory\n"); return 1; }
  for (const name of [...d.kids.keys()].sort()) {
    const n = ctx.vfs.lstat(dir + "/" + name);
    if (n && n.t === "f" && (n.mode & 0o111)) await ctx.shell.execOne([dir + "/" + name], io);
  }
  return 0;
};
CMD["remove-shell"] = (ctx, argv, io) => {
  const sh = argv.slice(1).filter((x) => x[0] !== "-")[0];
  const n = ctx.vfs.stat("/etc/shells");
  if (n && n.data) ctx.vfs.writeFile("/etc/shells", dec(n.data).split("\n").filter((l) => l && l !== sh).join("\n") + "\n", 0o644);
  return 0;
};
CMD.chattr = (ctx, argv, io) => {
  const a = argv.slice(1);
  const flags = a.filter((x) => x[0] === "+" || x[0] === "-");
  let rc = 0;
  for (const f of a.filter((x) => x[0] !== "+" && x[0] !== "-")) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n) { io.err("chattr: " + f + ": No such file or directory\n"); rc = 1; continue; }
    for (const fl of flags) {
      const set = fl[0] === "+";
      for (const ch of fl.slice(1)) {
        const cur = n.attr || "--------------e-------";
        const i = "suSaclDdijtTeEx".indexOf(ch);
        if (i < 0) continue;
        n.attr = cur.slice(0, i) + (set ? ch : "-") + cur.slice(i + 1);
      }
    }
  }
  return rc;
};
CMD.lsattr = (ctx, argv, io) => {
  let rc = 0;
  for (const f of argv.slice(1).filter((x) => x[0] !== "-")) {
    const n = ctx.vfs.stat(ctx.resolve(f));
    if (!n) { io.err("lsattr: " + f + ": No such file or directory\n"); rc = 1; continue; }
    emit(io, (n.attr || "--------------e-------") + " " + ctx.resolve(f));
  }
  return rc;
};
CMD.netstat = (ctx, argv, io) => {
  emit(io, "Active Internet connections (w/o servers)");
  emit(io, "Proto Recv-Q Send-Q Local Address           Foreign Address         State");
  emit(io, "tcp        0      0 " + NET_ADDR + ":443       10.0.2.2:51234          ESTABLISHED");
  return 0;
};
CMD.lspci = (ctx, argv, io) => {
  emit(io, "00:00.0 Host bridge: Intel Corporation 440FX - 82441FX PMC [Natoma]");
  emit(io, "00:01.0 ISA bridge: Intel Corporation 82371SB PIIX3 ISA [Natoma/Triton II]");
  emit(io, "00:02.0 VGA compatible controller: Device 1234:1111");
  emit(io, "00:03.0 Ethernet controller: Intel Corporation 82540EM Gigabit Ethernet Controller");
  emit(io, "00:04.0 SCSI storage controller: Red Hat, Inc. Virtio block device");
  return 0;
};
CMD.lsusb = (ctx, argv, io) => { emit(io, "（QEMU 这台上没有 USB 控制器）"); return 0; };
CMD.hwclock = (ctx, argv, io) => { emit(io, new Date().toISOString().replace("T", " ").slice(0, 19) + "  0.000000 seconds"); return 0; };
CMD.sysctl = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-");
  const table = { "kernel.hostname": ctx.env.HOSTNAME, "kernel.ostype": "Linux", "kernel.osrelease": UTS,
                  "kernel.pid_max": "32768", "net.ipv4.ip_forward": "0", "vm.swappiness": "60" };
  if (!a.length) { for (const k of Object.keys(table)) emit(io, k + " = " + table[k]); return 0; }
  let rc = 0;
  for (const k of a) { if (table[k] !== undefined) emit(io, k + " = " + table[k]); else { io.err("sysctl: cannot stat /proc/sys/" + k.replace(/\./g, "/") + ": No such file or directory\n"); rc = 1; } }
  return rc;
};
CMD.swapon = (ctx, argv, io) => { io.err("swapon: 没有交换分区（启动时 free 里 Swap 是 0）\n"); return 1; };
CMD.swapoff = (ctx, argv, io) => { io.err("swapoff: 没有交换分区\n"); return 1; };
CMD.fstrim = (ctx, argv, io) => 0;
CMD.fsync = (ctx, argv, io) => 0;
CMD.modinfo = (ctx, argv, io) => { io.err("modinfo: module " + (argv[1] || "") + " not found\n"); return 1; };
CMD.last = (ctx, argv, io) => { emit(io, ctx.env.USER + "  pts/0        10.0.2.2         " + new Date().toISOString().slice(0, 16).replace("T", " ") + "   still logged in"); return 0; };
CMD.wall = (ctx, argv, io) => { emit(io, "Broadcast message from " + ctx.env.USER + "@" + ctx.env.HOSTNAME + " (pts/0):"); emit(io, argv.slice(1).join(" ")); return 0; };
CMD.write = CMD.wall;
CMD["start-stop-daemon"] = (ctx, argv, io) => { emit(io, "start-stop-daemon: 演示里没有守护进程可管"); return 0; };
CMD["[["] = (ctx, argv, io) => testImpl(ctx, argv.slice(0, -1).map((x) => x === "[[" ? "[" : x));
CMD.awk = (ctx, argv, io) => awkImpl(ctx, argv, io);
CMD.vi = CMD.nano;
CMD.ed = CMD.nano;
CMD.killall5 = (ctx, argv, io) => { emit(io, "killall5: 演示里不发全局信号"); return 0; };
CMD.sv = (ctx, argv, io) => { emit(io, "sv: 真机没启用 runit 服务目录"); return 0; };
CMD.svc = CMD.sv; CMD.svok = CMD.sv; CMD.runsv = CMD.sv; CMD.runsvdir = CMD.sv; CMD.svlogd = CMD.sv;
CMD.man = (ctx, argv, io) => { emit(io, "man: 真机没装 man 页；命令用法见 help / busybox --help"); return 0; };
CMD.httpd = (ctx, argv, io) => { io.err("httpd: 浏览器里开不了监听套接字（真机 /sbin/busybox httpd 可以）\n"); return 1; };
CMD.ftpd = CMD.httpd; CMD.telnetd = CMD.httpd; CMD.inetd = CMD.httpd; CMD.ntpd = CMD.httpd;
CMD.telnet = (ctx, argv, io) => { io.err("telnet: 浏览器里开不了裸套接字\n"); return 1; };
CMD.nc = CMD.telnet; CMD.tftp = CMD.telnet; CMD.ftpget = CMD.telnet; CMD.ftpput = CMD.telnet;
CMD.nslookup = (ctx, argv, io) => {
  const host = argv.slice(1).filter((x) => x[0] !== "-")[0];
  if (!host) { io.err("nslookup: 需要域名\n"); return 1; }
  emit(io, "Server:    " + NET_DNS);
  emit(io, "Address 1: " + NET_DNS);
  emit(io, "Name:     " + host);
  emit(io, "Address 1: " + (host === "www.parlz.com" ? "203.0.113.10" : "127.0.0.1"));
  return 0;
};
CMD.arp = (ctx, argv, io) => { emit(io, "? (" + NET_GW + ") at 52:55:0a:00:02:02 [ether]  on eth0"); return 0; };
CMD.arping = CMD.ping;
CMD.ipcalc = (ctx, argv, io) => {
  const a = argv.slice(1).filter((x) => x[0] !== "-")[0] || "";
  const m = /^(\d+)\.(\d+)\.(\d+)\.(\d+)\/(\d+)$/.exec(a);
  if (!m) { io.err("ipcalc: 用法: ipcalc 10.0.2.15/24\n"); return 1; }
  const ip = m.slice(1, 5).map(Number).join("."), bits = Number(m[5]);
  const mask = bits === 0 ? 0 : (0xffffffff << (32 - bits)) >>> 0;
  const mstr = [24, 16, 8, 0].map((s) => (mask >>> s) & 255).join(".");
  const ipNum = m.slice(1, 5).reduce((acc, o) => (acc << 8) + Number(o), 0) >>> 0;
  const net = (ipNum & mask) >>> 0;
  const nstr = [24, 16, 8, 0].map((s) => (net >>> s) & 255).join(".");
  emit(io, "Address:   " + ip + "         " + ip.padStart(15));
  emit(io, "Netmask:   " + mstr + " = " + bits + "           " + mstr.padStart(15));
  emit(io, "Network:   " + nstr + "/" + bits);
  return 0;
};
CMD.brctl = (ctx, argv, io) => { io.err("brctl: 内核没开网桥（真机也没用到）\n"); return 1; };
CMD.vconfig = CMD.brctl; CMD.ifenslave = CMD.brctl; CMD.dhcprelay = CMD.brctl; CMD.dnsd = CMD.brctl;

/* awk 子集：-F、BEGIN/END、/模式/、{print $1,$2}、变量、NR、-v */
function awkImpl(ctx, argv, io) {
  const a = argv.slice(1);
  let fs = /\s+/, prog = null, file = null, assigns = [];
  for (let i = 0; i < a.length; i++) {
    if (a[i] === "-F") fs = new RegExp(a[++i]);
    else if (a[i].startsWith("-F")) fs = new RegExp(a[i].slice(2));
    else if (a[i] === "-v") assigns.push(a[++i]);
    else if (a[i][0] === "-") continue;
    else if (prog === null) prog = a[i];
    else file = a[i];
  }
  if (prog === null) { io.err("awk: 用法: awk [-F 分隔] '程序' [文件]\n"); return 1; }
  let text;
  if (file) { text = fileText(ctx, io, file); if (text == null) return 1; } else text = readAll(ctx, io);
  const vars = {};
  for (const kv of assigns) { const eq = kv.indexOf("="); if (eq > 0) vars[kv.slice(0, eq)] = kv.slice(eq + 1); }
  const parts = [];
  const re = /(BEGIN|END)\s*\{([^}]*)\}|\/([^/]*)\/\s*\{([^}]*)\}|\{([^}]*)\}/g;
  let m;
  while ((m = re.exec(prog))) parts.push({ when: m[1] || (m[3] ? "match" : "each"), pat: m[3], code: m[2] || m[4] || m[5] });
  if (!parts.length) parts.push({ when: "each", code: prog });
  const runCode = (code, fields, nr, nf) => {
    for (const stmt of code.split(";")) {
      const s = stmt.trim();
      const p = /^print\s*(.*)$/.exec(s);
      if (p) {
        const out = p[1].trim() === "" ? "$0" : p[1];
        const vals = [];
        for (const tok of out.split(",").map((x) => x.trim())) {
          if (/^\$0$/.test(tok)) vals.push(fields.join(fs === /\s+/ ? " " : String(fs).slice(1, -1)));
          else if (/^\$\d+$/.test(tok)) vals.push(fields[Number(tok.slice(1)) - 1] || "");
          else if (/^"([^"]*)"$/.test(tok)) vals.push(tok.slice(1, -1));
          else if (tok === "NR") vals.push(String(nr));
          else if (tok === "NF") vals.push(String(nf));
          else vals.push(vars[tok] !== undefined ? vars[tok] : tok);
        }
        emit(io, vals.join(" "));
        continue;
      }
      const asg = /^([A-Za-z_]\w*)\s*=\s*(.*)$/.exec(s);
      if (asg) { vars[asg[1]] = asg[2].replace(/^"|"$/g, "").replace(/\$(\d+)/g, (mm, d) => fields[Number(d) - 1] || ""); continue; }
      if (s === "next") continue;
    }
  };
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  for (const p of parts.filter((x) => x.when === "BEGIN")) runCode(p.code, [], 0, 0);
  let nr = 0;
  for (const ln of lines) {
    nr++;
    const fields = fs === /\s+/ ? ln.trim().split(/\s+/) : ln.split(fs);
    for (const p of parts) {
      if (p.when === "BEGIN" || p.when === "END") continue;
      if (p.pat && !new RegExp(p.pat).test(ln)) continue;
      runCode(p.code, fields, nr, fields.length);
    }
  }
  for (const p of parts.filter((x) => x.when === "END")) runCode(p.code, [], nr, 0);
  return 0;
}

CMD.bash = async (ctx, argv, io) => {
  const a = argv.slice(1);
  const b = new Bash(ctx.vfs, ctx.env, ctx.sys, ctx.shell);
  const ci = a.indexOf("-c");
  if (ci >= 0) {
    b.args = ["bash"].concat(a.slice(ci + 2));
    const rc = await b.runCode(a[ci + 1] || "", io);
    return rc;
  }
  const script = a.find((x) => x[0] !== "-");
  if (script) {
    const rc = await b.runFile(script, a.slice(a.indexOf(script) + 1), io);
    return rc;
  }
  // 无参交互：切进 bash 模式（提示符变 root@parlz:~#），exit 回来
  if (ctx.sys.mode && ctx.sys.mode.kind !== "bash") {
    ctx.sys.enterBash();
    emit(io, "Parlz bash 5.3 —— 试试 echo $((2+3)) / ls | wc -l / for i in a b; do echo $i; done / exit");
    return 0;
  }
  return 0;
};

// sh = 真机的判别器 shx.c：无参 + tty → parlz-sh；带参或非 tty → bash
CMD.sh = async (ctx, argv, io) => {
  const a = argv.slice(1);
  if (!a.length && ctx.isTty) return CMD["parlz-sh"](ctx, argv, io);
  return CMD.bash(ctx, ["bash"].concat(a), io);
};
CMD.ash = CMD.bash;

/* ================= bash：真能跑的 bash 子集 =================
   认这些：&& || | ; & 重定向(> >> < 2> &>) 引号与 \ 转义 # 注释
           $VAR ${VAR} ${VAR:-x} $? $# $@ $1.. $((算术)) $(命令替换) `命令替换`
           if/then/elif/else/fi  for/in/do/done  while/until  case/esac  函数  { } ( )
   不认（会像 bash 一样报语法/未实现）：数组、here-doc、[[ =~ ]]、进程替换、作业控制 */

const BASH_BREAK = {}, BASH_CONT = {}, BASH_RETURN = {};

function bashLex(src) {
  const toks = [];
  let i = 0;
  const push = (t, v) => toks.push({ t, v });
  while (i < src.length) {
    const c = src[i];
    if (c === " " || c === "\t") { i++; continue; }
    if (c === "\\" && src[i + 1] === "\n") { i += 2; continue; }
    if (c === "\n") { push("nl", "\n"); i++; continue; }
    if (c === "#" && (toks.length === 0 || toks[toks.length - 1].t === "nl")) {
      while (i < src.length && src[i] !== "\n") i++;
      continue;
    }
    if (c === "'") {                                    // 单引号：字面
      let j = i + 1, s = "";
      while (j < src.length && src[j] !== "'") s += src[j++];
      push("word", [{ q: 1, s }]);
      i = j + 1;
      continue;
    }
    if (c === '"') {                                    // 双引号：可展开
      let j = i + 1, s = "";
      while (j < src.length && src[j] !== '"') {
        if (src[j] === "\\" && j + 1 < src.length && "$`\"\\\n".includes(src[j + 1])) { s += src[j + 1]; j += 2; continue; }
        s += src[j++];
      }
      push("word", [{ q: 2, s }]);
      i = j + 1;
      continue;
    }
    const three = src.substr(i, 3), two = src.substr(i, 2);
    if (three === "2>>" || two === "2>" || two === "&>" || two === ">>" || two === "&&" || two === "||") {
      push("op", three === "2>>" ? "2>>" : two); i += (three === "2>>" ? 3 : 2); continue;
    }
    if ("|;&(){}<>!".includes(c)) { push("op", c); i++; continue; }
    // 普通词（可能混引号）
    const parts = [];
    let cur = "";
    while (i < src.length) {
      const ch = src[i];
      if (ch === " " || ch === "\t" || ch === "\n") break;
      if ("|;&(){}<>".includes(ch)) break;
      if (ch === "$" && src[i + 1] === "(") {          // $() / $(( ))
        let depth = 0, j = i + 1, inner = "";
        for (; j < src.length; j++) {
          if (src[j] === "(") depth++;
          else if (src[j] === ")") { depth--; if (!depth) break; }
          if (depth >= 1) inner += src[j];
        }
        cur += src.substr(i, j - i + 1);
        i = j + 1;
        continue;
      }
      if (ch === "$" && src[i + 1] === "{") {          // ${VAR} 整体算词的一部分
        let j = i + 2;
        while (j < src.length && src[j] !== "}") j++;
        cur += src.substr(i, j - i + 1);
        i = j + 1;
        continue;
      }
      if (ch === "\\") { cur += src[i + 1] || ""; i += 2; continue; }
      if (ch === "'" || ch === '"') {
        const q = ch === "'" ? 1 : 2;
        if (cur) { parts.push({ q: 0, s: cur }); cur = ""; }
        let j = i + 1, s = "";
        while (j < src.length && src[j] !== ch) {
          if (ch === '"' && src[j] === "\\" && "$`\"\\".includes(src[j + 1] || "")) { s += src[j + 1]; j += 2; continue; }
          s += src[j++];
        }
        parts.push({ q, s });
        i = j + 1;
        continue;
      }
      cur += ch; i++;
    }
    if (cur) parts.push({ q: 0, s: cur });
    if (parts.length) push("word", parts);
  }
  push("nl", "\n");
  return toks;
}

class Bash {
  constructor(vfs, env, sys, sh) {
    this.vfs = vfs; this.env = env; this.sys = sys; this.sh = sh;
    this.vars = new Map(); this.funcs = new Map();
    this.lastRc = 0; this.args = ["bash"]; this.exitReq = false;
    this.toks = []; this.p = 0; this.io = null;
  }

  /* ---- 入口 ---- */
  async runLine(line, io) { return this.runScript(line, io); }
  async runScript(src, io) {
    this.toks = bashLex(src);
    this.p = 0;
    let rc = this.lastRc;
    try {
      const list = this.parseList();
      rc = await this.execList(list, io);
    } catch (e) {
      if (e === BASH_RETURN || e === BASH_BREAK || e === BASH_CONT) rc = this.retCode !== undefined ? this.retCode : this.lastRc;
      else if (e && e.syntax) { io.err("bash: syntax error near unexpected token `" + e.syntax + "'\n"); rc = 2; }
      else throw e;
    }
    this.retCode = undefined;
    if (this.exitReq) { this.exitReq = false; this.exited = true; rc = this.exitCode; this.lastRc = rc; return rc; }
    this.lastRc = rc;
    return rc;
  }
  async runFile(path, argv, io) {
    const n = this.vfs.stat(this.sh.resolve(path));
    if (!n || !n.data) { io.err("bash: " + path + ": No such file or directory\n"); return 127; }
    this.args = [path].concat(argv);
    return this.runScript(dec(n.data), io);
  }
  async runCode(code, io) { return this.runScript(code, io); }

  /* ---- 词法辅助 ---- */
  peek() { return this.toks[this.p]; }
  next() { return this.toks[this.p++]; }
  isOp(v) { const t = this.peek(); return t && t.t === "op" && t.v === v; }
  isWord(v) { const t = this.peek(); return t && t.t === "word" && this.wordText(t) === v; }
  wordText(t) { return t.v.map((p) => p.s).join(""); }
  skipNewlines() { while (this.peek() && this.peek().t === "nl") this.p++; }
  expectOp(v) { if (!this.isOp(v)) throw { syntax: v }; this.p++; }

  /* ---- 语法 ---- */
  parseList(stops) {
    const items = [];
    for (;;) {
      this.skipNewlines();
      const t = this.peek();
      if (!t) break;
      if (stops && t.t === "word" && stops.includes(this.wordText(t))) break;
      if (stops && t.t === "op" && stops.includes(t.v)) break;
      if (t.t === "op" && t.v !== ";") break;                 // `)` `}` 之类的收尾留给上层
      const before = this.p;
      const node = this.parsePipeline();
      let sep = ";";
      const nx = this.peek();
      if (nx && nx.t === "op" && (nx.v === "&&" || nx.v === "||" || nx.v === "&")) { sep = nx.v; this.p++; }
      else if (nx && nx.t === "op" && nx.v === ";") {
        const nx2 = this.toks[this.p + 1];
        this.p++;
        if (nx2 && nx2.t === "op" && nx2.v === ";") {          // `;;` = case 分支结束
          this.p++;
          items.push({ sep: ";", node });
          break;
        }
      }
      items.push({ sep, node });
      if (this.p === before) break;                            // 保险：不许原地打转
    }
    return { k: "list", items };
  }

  parsePipeline() {
    let neg = false;
    if (this.isOp("!")) { neg = true; this.p++; }
    const cmds = [this.parseCommand()];
    while (this.isOp("|")) { this.p++; this.skipNewlines(); cmds.push(this.parseCommand()); }
    return { k: "pipe", cmds, neg };
  }

  parseCommand() {
    this.skipNewlines();
    const t = this.peek();
    if (!t) return { k: "simple", words: [], redirs: [] };
    if (t.t === "word") {
      const w = this.wordText(t);
      if (w === "if") return this.parseIf();
      if (w === "for") return this.parseFor();
      if (w === "while" || w === "until") return this.parseWhile(w === "until");
      if (w === "case") return this.parseCase();
      if (w === "function") { this.p++; const name = this.wordText(this.next()); return this.parseFunc(name); }
      if (w === "{" ) { this.p++; const body = this.parseList(["}", "nl"]); this.expectOp("}"); return { k: "group", body }; }
    }
    if (t.t === "op" && t.v === "{") { this.p++; const body = this.parseList(["}", "nl"]); this.expectOp("}"); return { k: "group", body }; }
    if (t.t === "op" && t.v === "(") { this.p++; const body = this.parseList([")", "nl"]); this.expectOp(")"); return { k: "group", body, sub: true }; }
    // 函数定义 name() { ... }
    if (t.t === "word" && this.toks[this.p + 1] && this.toks[this.p + 1].t === "op" && this.toks[this.p + 1].v === "(") {
      const name = this.wordText(t); this.p += 2;
      if (!this.isOp(")")) { /* 退回当普通命令 */ this.p -= 2; return this.parseSimple(); }
      this.p++;
      return this.parseFunc(name);
    }
    return this.parseSimple();
  }

  parseFunc(name) {
    this.skipNewlines();
    let body;
    if (this.isOp("{")) { this.p++; body = this.parseList(["}", "nl"]); this.expectOp("}"); }
    else body = this.parseCommand();
    this.funcs.set(name, body);
    return { k: "noop" };
  }

  parseIf() {
    this.p++;                                     // 吃掉 `if`
    const branches = [];
    let cond = this.parseList(["then"]);
    this.p++;                                     // then
    let body = this.parseList(["elif", "else", "fi"]);
    branches.push({ cond, body });
    let elseBody = null;
    for (;;) {
      const t = this.peek();
      const w = t && t.t === "word" ? this.wordText(t) : null;
      if (w === "elif") {
        this.p++;
        const c2 = this.parseList(["then"]); this.p++;
        const b2 = this.parseList(["elif", "else", "fi"]);
        branches.push({ cond: c2, body: b2 });
        continue;
      }
      if (w === "else") { this.p++; elseBody = this.parseList(["fi"]); }
      break;
    }
    const f = this.peek();
    if (f && f.t === "word" && this.wordText(f) === "fi") this.p++;
    else throw { syntax: "fi" };
    return { k: "if", branches, else: elseBody };
  }

  parseFor() {
    this.p++;                                     // for
    const name = this.wordText(this.next());
    this.skipNewlines();
    let words = null;
    const t = this.peek();
    if (t && t.t === "word" && this.wordText(t) === "in") { this.p++; words = this.parseWordsUntil(["do"]); }
    this.skipNewlines();
    if (this.isWord("do")) this.p++;
    else { this.skipNewlines(); if (!this.isWord("do")) throw { syntax: "do" }; this.p++; }
    const body = this.parseList(["done"]);
    if (this.isWord("done")) this.p++;
    return { k: "for", name, words, body };
  }

  parseWhile(until) {
    this.p++;
    const cond = this.parseList(["do"]);
    if (this.isWord("do")) this.p++;
    const body = this.parseList(["done"]);
    if (this.isWord("done")) this.p++;
    return { k: "while", cond, body, until };
  }

  parseCase() {
    this.p++;
    const word = this.next();
    this.skipNewlines();
    if (!this.isWord("in")) throw { syntax: "in" };
    this.p++;
    const items = [];
    for (;;) {
      this.skipNewlines();
      if (this.isWord("esac") || (this.peek() && this.peek().t === "word" && this.wordText(this.peek()) === "esac")) {
        if (this.isWord("esac")) this.p++;
        break;
      }
      if (!this.peek()) break;
      const pats = [];
      for (;;) {
        const pt = this.peek();
        if (!pt) break;
        if (pt.t === "op" && pt.v === ")") { this.p++; break; }
        if (pt.t === "op" && pt.v === "|") { this.p++; continue; }
        pats.push(pt.t === "word" ? this.wordText(pt) : pt.v);
        this.p++;
      }
      const body = this.parseList(["esac"]);
      // 吃掉 ;; / ;& 
      if (this.peek() && this.peek().t === "op" && this.peek().v === ";") { this.p++; if (this.peek() && this.peek().t === "op" && this.peek().v === ";") this.p++; }
      items.push({ pats, body });
    }
    return { k: "case", word, items };
  }

  parseWordsUntil(stops) {
    const out = [];
    for (;;) {
      const t = this.peek();
      if (!t) break;
      if (t.t === "nl") { this.p++; continue; }
      if (t.t === "op") { if (t.v === ";") { this.p++; continue; } break; }   // `for i in a b; do` 的 `;`
      if (stops.includes(this.wordText(t))) break;
      out.push(this.next());
    }
    return out;
  }

  parseSimple() {
    const words = [], redirs = [];
    for (;;) {
      const t = this.peek();
      if (!t || t.t === "nl") break;
      if (t.t === "op") {
        if (t.v === ">" || t.v === ">>" || t.v === "<" || t.v === "2>" || t.v === "2>>" || t.v === "&>") {
          this.p++;
          const target = this.next();
          if (!target || target.t !== "word") throw { syntax: t.v };
          redirs.push({ kind: t.v, word: target });
          continue;
        }
        break;
      }
      words.push(this.next());
    }
    return { k: "simple", words, redirs };
  }

  /* ---- 展开 ---- */
  async expandWord(tok, io) {
    let out = "";
    for (const part of tok.v) {
      if (part.q === 1) { out += part.s; continue; }
      out += await this.expandPart(part.s, io);
    }
    return out;
  }
  async expandArgs(words, io) {
    const out = [];
    for (const w of words) {
      const s = await this.expandWord(w, io);
      if (s === "" && w.v.every((p) => p.q === 0)) continue;     // 无引号的空展开丢弃
      out.push(s);
    }
    return out;
  }
  async expandPart(s, io) {
    let res = "", i = 0;
    while (i < s.length) {
      const c = s[i];
      if (c === "\\") {                                  // 只转义 $ ` " \ ；`\n` 这种原样留给收命令处理
        const nx = s[i + 1] || "";
        if ("$`\"\\".includes(nx)) { res += nx; i += 2; } else { res += "\\"; i++; }
        continue;
      }
      if (c === "`") {
        let j = i + 1, cmd = "";
        while (j < s.length && s[j] !== "`") cmd += s[j++];
        res += (await this.capture(cmd, io)).replace(/\n$/, "");
        i = j + 1;
        continue;
      }
      if (c !== "$") { res += c; i++; continue; }
      if (s[i + 1] === "(") {
        const arithMode = s[i + 2] === "(";            // $(( )) 是算术，$( ) 是命令替换
        let depth = 0, j = i + 1, inner = "";
        for (; j < s.length; j++) {
          const ch = s[j];
          if (ch === "(") depth++;
          else if (ch === ")") { depth--; if (depth === 0) break; }
          if (j > i + 1) inner += ch;                  // 跳过紧跟在 $ 后的那个 (
        }
        if (arithMode) res += String(this.arith(inner.slice(0, -1)));
        else res += (await this.capture(inner, io)).replace(/\n$/, "");
        i = j + 1;
        continue;
      }
      if (s[i + 1] === "{") {
        let j = i + 2, name = "";
        while (j < s.length && /[A-Za-z0-9_]/.test(s[j])) name += s[j++];
        let op = null, arg = "";
        if (s[j] === ":" && "-=+?".includes(s[j + 1] || "")) { op = s[j + 1]; j += 2; while (j < s.length && s[j] !== "}") arg += s[j++]; }
        if (s[j] === "}") j++;
        res += this.refVar(name, op, arg);
        i = j;
        continue;
      }
      if (s[i + 1] === "?") { res += String(this.lastRc); i += 2; continue; }
      if (s[i + 1] === "#") { res += String(this.args.length - 1); i += 2; continue; }
      if (s[i + 1] === "$") { res += "1"; i += 2; continue; }          // $$ → 假 pid
      if (s[i + 1] === "@" || s[i + 1] === "*") { res += this.args.slice(1).join(" "); i += 2; continue; }
      if (/[0-9]/.test(s[i + 1] || "")) { res += this.args[Number(s[i + 1])] || ""; i += 2; continue; }
      let j = i + 1, name = "";
      while (j < s.length && /[A-Za-z0-9_]/.test(s[j])) name += s[j++];
      if (!name) { res += "$"; i++; continue; }
      res += this.getVar(name);
      i = j;
    }
    return res;
  }
  getVar(name) {
    if (this.vars.has(name)) return this.vars.get(name);
    return this.env[name] !== undefined ? this.env[name] : "";
  }
  refVar(name, op, arg) {
    const cur = this.getVar(name);
    if (!op) return cur;
    if (op === "-") return cur !== "" ? cur : arg;
    if (op === "=") { if (cur === "") { this.vars.set(name, arg); return arg; } return cur; }
    if (op === "+") return cur !== "" ? arg : "";
    if (op === "?") return cur !== "" ? cur : (arg || "parameter null or not set");
    return cur;
  }
  async capture(src, io) {
    const buf = [];
    const sink = { out: (s) => buf.push(s), err: (s) => io.err(s), progress: () => {}, input: null };
    await this.runScript(src, sink);
    return buf.join("");
  }

  /* ---- 算术 ---- */
  arith(expr) {
    const src = expr.replace(/\$?([A-Za-z_][A-Za-z0-9_]*)/g, (m, n) => {
      const v = this.getVar(n);
      return v === "" ? "0" : v;
    }).replace(/\$(\d+)/g, (m, d) => this.args[Number(d)] || "0");
    let i = 0;
    const peekc = () => src[i];
    const skip = () => { while (" \t".includes(src[i])) i++; };
    const primary = () => {
      skip();
      if (peekc() === "(") { i++; const v = expr0(); skip(); if (peekc() === ")") i++; return v; }
      const m = /^-?\d+|^0[xX][0-9a-fA-F]+/.exec(src.slice(i));
      if (m) { i += m[0].length; return Number(m[0]); }
      const nm = /^[A-Za-z_][A-Za-z0-9_]*/.exec(src.slice(i));
      if (nm) { i += nm[0].length; const v = this.getVar(nm[0]); return v === "" ? 0 : Number(v) || 0; }
      i++;
      return 0;
    };
    const unary = () => { skip(); if (peekc() === "-") { i++; return -unary(); } if (peekc() === "+") { i++; return unary(); } if (peekc() === "!") { i++; return unary() ? 0 : 1; } return primary(); };
    const mul = () => { let v = unary(); for (;;) { skip(); const c = peekc(); if (c === "*") { i++; v *= unary(); } else if (c === "/") { i++; const d = unary(); v = d ? Math.trunc(v / d) : 0; } else if (c === "%") { i++; const d = unary(); v = d ? v % d : 0; } else return v; } };
    const add = () => { let v = mul(); for (;;) { skip(); const c = peekc(); if (c === "+") { i++; v += mul(); } else if (c === "-") { i++; v -= mul(); } else return v; } };
    const cmp = () => {
      let v = add(); skip();
      for (;;) {
        skip();
        if (src[i] === "<" && src[i + 1] === "=") { i += 2; v = v <= add() ? 1 : 0; }
        else if (src[i] === ">" && src[i + 1] === "=") { i += 2; v = v >= add() ? 1 : 0; }
        else if (src[i] === "=" && src[i + 1] === "=") { i += 2; v = v === add() ? 1 : 0; }
        else if (src[i] === "!") { i += 2; v = v !== add() ? 1 : 0; }
        else if (src[i] === "<") { i++; v = v < add() ? 1 : 0; }
        else if (src[i] === ">") { i++; v = v > add() ? 1 : 0; }
        else return v;
      }
    };
    const land = () => { let v = cmp(); while (/^\s*&&/.test(src.slice(i))) { i += src.slice(i).match(/^\s*&&/)[0].length; const r = cmp(); v = (v && r) ? 1 : 0; } return v; };
    const lor = () => { let v = land(); while (/^\s*\|\|/.test(src.slice(i))) { i += src.slice(i).match(/^\s*\|\|/)[0].length; const r = land(); v = (v || r) ? 1 : 0; } return v; };
    const expr0 = lor;
    try { return expr0(); } catch (e) { return 0; }
  }

  /* ---- 执行 ---- */
  async execList(list, io) {
    let rc = 0;
    const items = list.items;
    for (let i = 0; i < items.length; i++) {
      // 分隔符挂在**前一项**上：`a && b || c` 里跳过与否看上一项怎么结束
      if (i > 0) {
        const sep = items[i - 1].sep;
        if (sep === "&&" && rc !== 0) continue;
        if (sep === "||" && rc === 0) continue;
      }
      rc = await this.execNode(items[i].node, io);
      this.lastRc = rc;
      if (this.exitReq) throw BASH_RETURN;
    }
    return rc;
  }

  async execNode(node, io) {
    switch (node.k) {
      case "noop": return 0;
      case "list": return this.execList(node, io);
      case "group": return this.execList(node.body, io);
      case "pipe": return this.execPipe(node, io);
      case "if": {
        for (const b of node.branches) {
          if (await this.execList(b.cond, io) === 0) return this.execList(b.body, io);
        }
        return node.else ? this.execList(node.else, io) : 0;
      }
      case "for": {
        const items = node.words ? await this.expandArgs(node.words, io)
                                 : (io.input != null ? io.input.split(/\s+/).filter(Boolean) : this.args.slice(1));
        let rc = 0;
        for (const v of items) {
          this.vars.set(node.name, v);
          try { rc = await this.execList(node.body, io); }
          catch (e) { if (e === BASH_BREAK) break; if (e === BASH_CONT) continue; throw e; }
        }
        return rc;
      }
      case "while": {
        let rc = 0, guard = 0;
        for (;;) {
          const c = await this.execList(node.cond, io);
          if ((c === 0) === !!node.until) break;
          if (++guard > 100000) { io.err("bash: while 循环次数过多（演示上限 100000）\n"); break; }
          try { rc = await this.execList(node.body, io); }
          catch (e) { if (e === BASH_BREAK) break; if (e === BASH_CONT) continue; throw e; }
        }
        return rc;
      }
      case "case": {
        const w = await this.expandWord(node.word, io);
        for (const it of node.items) {
          for (const pat of it.pats) {
            const re = new RegExp("^" + pat.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".") + "$");
            if (re.test(w)) return this.execList(it.body, io);
          }
        }
        return 0;
      }
      case "simple": return this.execSimple(node, io);
      default: return 0;
    }
  }

  async execPipe(node, io) {
    let input = null, rc = 0;
    for (let i = 0; i < node.cmds.length; i++) {
      const last = i === node.cmds.length - 1;
      const buf = [];
      const sink = {
        out: last ? (s) => io.out(s) : (s) => buf.push(s),
        err: (s) => io.err(s),
        progress: last ? (s) => io.progress && io.progress(s) : () => {},
        input,
      };
      rc = await this.execNode(node.cmds[i], sink);
      input = buf.join("");
      this.lastRc = rc;
    }
    if (node.neg) rc = rc === 0 ? 1 : 0;
    return rc;
  }

  async execSimple(node, io) {
    if (!node.words.length) {                     // 只有重定向：建/清空目标文件（bash 的行为）
      for (const r of node.redirs) {
        const target = await this.expandWord(r.word, io);
        if (r.kind === ">" || r.kind === "&>" || r.kind === "2>") this.writeRedir({ target, append: false }, "", io);
        else if (r.kind === ">>" || r.kind === "2>>") this.writeRedir({ target, append: true }, "", io);
      }
      return 0;
    }
    let argv = await this.expandArgs(node.words, io);
    if (!argv.length) return 0;
    // 行首赋值（bash 里只作用于这条命令；这里按"本行可见"处理，够用）
    while (argv.length && /^[A-Za-z_][A-Za-z0-9_]*=/.test(argv[0])) {
      const eq = argv[0].indexOf("=");
      this.vars.set(argv[0].slice(0, eq), await this.expandPart(argv[0].slice(eq + 1), io));
      argv = argv.slice(1);
    }
    if (!argv.length) return 0;
    const name = argv[0];

    // 重定向：> >> < 2> 2>> &>
    let input = io.input, outFile = null, errFile = null;
    for (const r of node.redirs) {
      const target = await this.expandWord(r.word, io);
      if (r.kind === "<") {
        const n = this.vfs.stat(this.sh.resolve(target));
        if (!n) { io.err("bash: " + target + ": No such file or directory\n"); return 1; }
        input = n.data ? dec(n.data) : "";
      } else if (r.kind === ">" || r.kind === ">>") outFile = { target, append: r.kind === ">>" };
      else if (r.kind === "2>" || r.kind === "2>>") errFile = { target, append: r.kind === "2>>" };
      else if (r.kind === "&>") outFile = errFile = { target, append: false };
    }

    const cap = [];
    const errCap = [];
    const disc = (t) => t === "/dev/null";
    const sink = {
      out: (s) => { (outFile && disc(outFile.target)) ? null : cap.push(s); },
      err: (s) => { (errFile && disc(errFile.target)) ? null : errCap.push(s); },
      progress: (s) => io.progress && io.progress(s),
      input,
    };
    let rc = await this.runBuiltinOrCommand(argv, sink, io);
    if (outFile && !disc(outFile.target)) this.writeRedir(outFile, cap.join(""), io);
    else if (!outFile && cap.length) io.out(cap.join(""));
    if (errFile && !disc(errFile.target)) this.writeRedir(errFile, errCap.join(""), io);
    else if (!errFile && errCap.length) io.err(errCap.join(""));
    return rc;
  }
  applyRedirs(node, io, rc, text) { if (text) io.out(text); return rc; }
  writeRedir(r, text, io) {
    const p = this.sh.resolve(r.target);
    const old = r.append ? this.vfs.stat(p) : null;
    const prev = old && old.data ? dec(old.data) : "";
    try { this.vfs.writeFile(p, prev + text, 0o644); }
    catch (e) { io.err("bash: " + r.target + ": " + e.message + "\n"); }
  }

  async runBuiltinOrCommand(argv, sink, io) {
    const name = argv[0];
    const B = this.builtins[name];
    if (B) return B.call(this, argv, sink, io);
    if (this.funcs.has(name)) {
      const saved = this.args;
      this.args = [name].concat(argv.slice(1));
      try { return await this.execNode(this.funcs.get(name), sink); }
      finally { this.args = saved; }
    }
    // 其余交给和 parlz-sh 同一套分发（ls/echo/mount… 的内建与外部命令都在这条路上）
    return await this.sh.execOne(argv, sink);
  }
}

Bash.prototype.builtins = {
  ":": () => 0,
  true: () => 0,
  false: () => 1,
  pwd: function (argv, io) { io.out(this.sh.cwd + "\n"); return 0; },
  echo: function (argv, io) {
    let nl = true, esc = false, i = 1;
    while (argv[i] && argv[i][0] === "-" && /^-n?e?$/.test(argv[i])) {
      if (argv[i].includes("n")) nl = false;
      if (argv[i].includes("e")) esc = true;
      i++;
    }
    let s = argv.slice(i).join(" ");
    if (esc) s = s.replace(/\\n/g, "\n").replace(/\\t/g, "\t").replace(/\\033/g, "\x1b").replace(/\\\\/g, "\\");
    io.out(s + (nl ? "\n" : ""));
    return 0;
  },
  printf: function (argv, io) {
    const fmt = (argv[1] || "").replace(/\\n/g, "\n").replace(/\\t/g, "\t");
    const args = argv.slice(2);
    if (!args.length) { io.out(fmt.replace(/%[sd]/g, "")); return 0; }
    let i = 0;
    while (i < args.length) {                           // 参数多于占位符时 bash 会重复用格式
      io.out(fmt.replace(/%[sd]/g, () => (args[i] !== undefined ? args[i++] : "")));
    }
    return 0;
  },
  cd: function (argv, io) {
    const target = argv[1] || this.env.HOME || "/root";
    const p = this.sh.resolve(target);
    const n = this.vfs.stat(p);
    if (!n || n.t !== "d") { io.err("bash: cd: " + target + ": No such file or directory\n"); return 1; }
    this.sh.cwd = p;
    return 0;
  },
  export: function (argv, io) {
    for (const a of argv.slice(1)) {
      const eq = a.indexOf("=");
      if (eq > 0) { const k = a.slice(0, eq), v = a.slice(eq + 1); this.vars.set(k, v); this.env[k] = v; }
      else if (this.vars.has(a)) this.env[a] = this.vars.get(a);
    }
    return 0;
  },
  unset: function (argv) { for (const a of argv.slice(1)) { this.vars.delete(a); delete this.env[a]; } return 0; },
  local: function (argv) { for (const a of argv.slice(1)) { const eq = a.indexOf("="); this.vars.set(eq > 0 ? a.slice(0, eq) : a, eq > 0 ? a.slice(eq + 1) : ""); } return 0; },
  exit: function (argv, io) { this.exitReq = true; this.exitCode = argv[1] !== undefined ? (parseInt(argv[1], 10) & 0xff) : this.lastRc; io.out("exit\n"); return this.exitCode; },
  return: function (argv) { this.retCode = argv[1] !== undefined ? parseInt(argv[1], 10) : this.lastRc; throw BASH_RETURN; },
  break: function () { throw BASH_BREAK; },
  continue: function () { throw BASH_CONT; },
  shift: function (argv) { const n = argv[1] ? parseInt(argv[1], 10) : 1; this.args = this.args.slice(0, 1).concat(this.args.slice(1 + n)); return 0; },
  set: function (argv, io) { if (!argv[1]) { for (const k of [...this.vars.keys()].sort()) io.out(k + "=" + this.vars.get(k) + "\n"); } return 0; },
  eval: async function (argv, io) { return this.runScript(argv.slice(1).join(" "), io); },
  source: async function (argv, io) { const f = argv[1]; const n = this.vfs.stat(this.sh.resolve(f)); if (!n || !n.data) { io.err("bash: " + f + ": No such file or directory\n"); return 1; } return this.runScript(dec(n.data), io); },
  ".": function (argv, io) { return this.builtins.source.call(this, argv, io); },
  test: function (argv) { return testImpl(this.sh.ctx(), argv); },
  "[": function (argv) { return testImpl(this.sh.ctx(), argv.slice(0, -1)); },
  "[[": function (argv, io) { return this.runTest2(argv.slice(1, -1), io); },
  read: function (argv, io) {
    const name = argv.filter((a) => a[0] !== "-")[1] || "REPLY";
    const line = (io.input || "").split("\n")[0];
    this.vars.set(name, line);
    return io.input != null ? 0 : 1;
  },
  type: function (argv, io) {
    for (const n of argv.slice(1)) {
      if (this.builtins[n]) io.out(n + " is a shell builtin\n");
      else if (this.funcs.has(n)) io.out(n + " is a function\n");
      else { const p = this.sh.findExec(n); if (p) io.out(n + " is " + p + "\n"); else { io.err("bash: type: " + n + ": not found\n"); return 1; } }
    }
    return 0;
  },
  command: async function (argv, io) { const rc = await this.sh.execOne(argv.slice(1), io); return rc; },
  alias: function (argv, io) { for (const a of argv.slice(1)) { const eq = a.indexOf("="); io.out("alias " + a + "\n"); } return 0; },
  trap: () => 0,
  wait: () => 0,
  umask: (argv, io) => { if (argv[1]) return 0; io.out("0022\n"); return 0; },
  times: () => 0,
  history: () => 0,
  jobs: () => 0,
  let: function (argv) { let v = 0; for (const a of argv.slice(1)) v = this.arith(a); return 0; },
  expr: function (argv, io) { return CMD.expr(this.sh.ctx(), argv, io); },
};

Bash.prototype.runTest2 = function (args, io) {
  // [[ a == b ]] / [[ -z x ]] / [[ a && b ]] / [[ ! x ]]
  const s = args.map((a) => a).join(" ");
  const tokens = s.match(/!=|==|=|-z|-n|\|\||&&|!|\(|\)|[^\s()]+/g) || [];
  let i = 0;
  const atom = () => {
    const t = tokens[i++];
    if (t === "!") return atom() ? 0 : 1;
    if (t === "(") { const v = expr(); if (tokens[i] === ")") i++; return v; }
    if (t === "-z") return (tokens[i++] || "") === "" ? 1 : 0;
    if (t === "-n") return (tokens[i++] || "") !== "" ? 1 : 0;
    if (t === "-f") { const n = this.vfs.stat(this.sh.resolve(tokens[i++] || "")); return n && n.t === "f" ? 1 : 0; }
    if (t === "-d") { const n = this.vfs.stat(this.sh.resolve(tokens[i++] || "")); return n && n.t === "d" ? 1 : 0; }
    if (t === "-e") { return this.vfs.stat(this.sh.resolve(tokens[i++] || "")) ? 1 : 0; }
    const op = tokens[i];
    if (op === "==" || op === "=") { i++; return t === tokens[i++] ? 1 : 0; }
    if (op === "!=") { i++; return t !== tokens[i++] ? 1 : 0; }
    return t && t.length ? 1 : 0;
  };
  const expr = () => {
    let v = atom();
    for (;;) {
      if (tokens[i] === "&&") { i++; const r = atom(); v = v && r ? 1 : 0; continue; }
      if (tokens[i] === "||") { i++; const r = atom(); v = v || r ? 1 : 0; continue; }
      return v;
    }
  };
  const v = expr();
  return v ? 0 : 1;
};

/* ================= 系统封装：种子 rootfs + pm ================= */

const PACKAGES = {
  // 四行全部对齐 output/feed/Packages 的真值(2026-10-01 重打包):
  // gcc/clang 的版本列以前是 "13"/写死值, 真机 feed 修好 VERSIONS 之后
  // 索引里就是 13.3.0 / 18.1.3 —— 演示站不许比真机更"含糊"。
  core:  { version: "0.1.0-rc", file: "core.pm",  size: 43698176  },
  pm:    { version: "1.1-RC+1", file: "pm.pm",    size: 7578112   },
  gcc:   { version: "13.3.0",   file: "gcc.pm",   size: 591771136 },
  clang: { version: "18.1.3",   file: "clang.pm", size: 1074919936 },
};

function seed(vfs) {
  const dirs = ["bin","boot","dev","etc","etc/apt","etc/pm","etc/ssl","etc/ssl/certs",
                "etc/yum.repos.d","mnt","proc",
                "root","sbin","sys","tmp","usr","usr/bin","usr/sbin","usr/local","usr/local/bin",
                "usr/share","usr/share/licenses",
                "usr/share/terminfo","var","var/cache/apt/archives","var/cache/yum",
                "var/lib","var/lib/apt/lists","var/lib/dpkg/info","var/lib/rpm/installed","parlz"];
  for (const d of dirs) vfs.mkdirp("/" + d);
  // /usr/share/licenses/<组件>/<文件> —— 目录与条目都从 LICENSES 现推,
  // 不再写死第二份名单(写死过一版: 真机加了 10 个组件, 演示还停在只有 bash)
  vfs.mkdirp("/usr/share/licenses");

  // 真文件（非软链）挂 src：需要字节时由 sys.ensureData 去 web/rootfs/<src> 取真的 ELF
  const withSrc = (node, src) => Object.assign(node, { src });
  vfs.add("/sbin/busybox", withSrc(nFile(null, 0o755, BUSYBOX_SIZE), "/sbin/busybox"));
  vfs.symlink("busybox", "/sbin/init");
  for (const [name, t, size, link] of REAL_BIN)
    if (t === "l") vfs.symlink(link, "/bin/" + name);
    else vfs.add("/bin/" + name, withSrc(nFile(null, 0o755, size), "/bin/" + name));
  for (const [name, , , link] of REAL_USRBIN) vfs.symlink(link, "/usr/bin/" + name);

  // /usr/share/licenses/<组件>/<文件> —— 目录与条目都从 LICENSES 现推, 不再写死
  // 第二份名单(写死过一版: 真机加了 10 个组件, 演示还停在只有 bash)。
  // 这些是纯文本, web/rootfs 里有实体字节(sync 脚本会带上), 所以挂 src 让
  // `cat /usr/share/licenses/linux-kernel/COPYING` 在演示里也能真读出 GPLv2 全文。
  vfs.mkdirp("/usr/share/licenses");
  vfs.add("/usr/share/licenses/README",
          withSrc(nFile(null, 0o644, LICENSES_README_SIZE), "/usr/share/licenses/README"));
  for (const [comp, file, size] of LICENSES) {
    const rel = "/usr/share/licenses/" + comp + "/" + file;
    vfs.mkdirp("/usr/share/licenses/" + comp);
    vfs.add(rel, withSrc(nFile(null, 0o644, size), rel));
  }

  vfs.writeFile("/etc/parlz-release", PARLZ_RELEASE, 0o644);
  // 介质上的授权说明(真机: 引导分区 LICENSE.TXT / ISO 根 / 已安装根 /LICENSE.TXT)
  vfs.writeFile("/LICENSE.TXT", MEDIA_LICENSE, 0o644);
  vfs.writeFile("/etc/resolv.conf", RESOLV, 0o644);
  vfs.writeFile("/etc/passwd", PASSWD, 0o644);
  vfs.writeFile("/etc/group", GROUP, 0o644);
  vfs.writeFile("/etc/inittab", INITTAB, 0o755);
  vfs.writeFile("/etc/pm/feeds.conf", FEEDS_DEFAULT, 0o644);
  vfs.writeFile("/etc/ssl/cert.pem", "# CA bundle（演示环境未随附实体）\n", 0o644);
  vfs.symlink("/usr/share/terminfo", "/etc/terminfo");

  vfs.writeFile("/usr/local/bin/parlz-boot.sh", "#!/bin/sh\n# 挂 devtmpfs → 重指 /dev/console → 配网 → login\n", 0o755);
  vfs.writeFile("/usr/local/bin/parlz-boot-body.sh", "#!/bin/sh\n# （真机上是那份带 install 看门狗的 body）\n", 0o755);
  vfs.writeFile("/parlz/banner", PARLZ_BANNER, 0o644);
  vfs.writeFile("/nettest.sh", "#!/bin/sh\necho \"=== TEST: bash 特性 ===\"\nbash -c 'arr=(a \"b c\" d); echo ARR=${arr[1]}'\n", 0o755);
  vfs.add("/init", withSrc(nFile(null, 0o755, 839608), "/init"));
  vfs.add("/install.d", withSrc(nFile(null, 0o755, 130), "/install.d"));

  vfs.writeFile("/proc/version", PROC_VERSION + "\n", 0o444);
  vfs.writeFile("/proc/cmdline", "console=ttyS0,115200 console=tty0 root=/dev/vda2 rw\n", 0o444);
  vfs.writeFile("/proc/cpuinfo", "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: QEMU Virtual CPU version 2.5+\ncpu MHz\t\t: 2599.998\n", 0o444);
  vfs.writeFile("/proc/uptime", "3.40 0.00\n", 0o444);
  vfs.writeFile("/proc/mounts", "", 0o444);
  for (const k of ["null","zero","console","tty","vda","vda1","vda2","vdb"])
    vfs.add("/dev/" + k, nFile(null, 0o666, 0));
}

function create(opts) {
  opts = opts || {};
  const vfs = new Vfs();
  seed(vfs);
  vfs.mounts = [["devtmpfs","/dev","devtmpfs",""], ["proc","/proc","proc",""],
                ["sysfs","/sys","sysfs",""], ["tmpfs","/tmp","tmpfs",""], ["/dev/vda2","/","ext4",""]];
  const sys = {
    vfs,
    feedBase: opts.feedBase || "feed",          // 本站就是镜像站：相对 feed/ 目录
    installed: {},
    tty: opts.tty || null,                      // 全屏程序（nano/less/top/watch）用的屏幕
    fetchImpl: opts.fetchImpl || ((u, i) => fetch(u, i)),
    resolveUrl(u) {
      if (/^[a-z]+:/i.test(u)) return u;        // http(s)/data/… 原样
      return u;                                 // 相对路径原样（浏览器自己拼当前页）
    },
  };
  // 与真机一致：init 里 setenv(PM_FEED, 官网)；pm 取源规则 = $PM_FEED > /etc/pm/feeds.conf
  const env = { USER: "guest", HOSTNAME: "parlz", HOME: "/root",
                PATH: "/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin",
                PM_FEED: FEEDS_DEFAULT,
                PWD: "/root", SHELL: "/bin/parlz-sh", TERM: "linux", LANG: "C" };
  const shell = new Shell(vfs, env, sys);

  const refreshProc = () => {
    vfs.writeFile("/proc/mounts",
      vfs.mounts.map(([s, d, t]) => s + " " + d + " " + t + " rw,relatime 0 0").join("\n") + "\n", 0o444);
  };
  refreshProc();
  sys.refreshProc = refreshProc;

  /* ---- 取源规则与真机 pm.c 一致：$PM_FEED > /etc/pm/feeds.conf，默认 www.parlz.com/feed ----
     演示里浏览器可能被 CORS 拦（官网没发 Access-Control-Allow-Origin），那就退回
     **本站同源镜像 feed/** —— 本站就是镜像站本体，两份内容是同一份；退回时会明说。 */
  function configuredFeed() {
    if (env.PM_FEED) return env.PM_FEED;
    const conf = vfs.stat("/etc/pm/feeds.conf");
    if (conf && conf.data) {
      for (const ln of dec(conf.data).split("\n")) {
        const s = ln.trim();
        if (s && s[0] !== "#") return s;
      }
    }
    return FEEDS_DEFAULT;
  }
  function feedCandidates() {
    const conf = configuredFeed(), out = [];
    const push = (u) => { if (u && !out.includes(u)) out.push(u); };
    let httpsPage = false;
    try { httpsPage = location.protocol === "https:"; } catch (e) { /* node/测试环境 */ }
    if (httpsPage) { push(conf.replace(/^http:/i, "https:")); push(conf); }
    else { push(conf); push(conf.replace(/^http:/i, "https:")); }
    push("feed");                                       // 同源镜像兜底（= 站点自己的 feed/）
    return out;
  }
  const isOfficial = (base) => /parlz\.com/i.test(base);

  async function fetchFirst(paths) {                    // 依次试候选源，返回 {res, base}
    const tried = [];
    for (const base of feedCandidates()) {
      const url = base.replace(/\/$/, "") + "/" + paths;
      try {
        const res = await sys.fetchImpl(url);
        if (res.ok) return { res, base, tried };
        tried.push(url + " → HTTP " + res.status);
      } catch (e) { tried.push(url + " → " + (e && e.message ? e.message : e)); }
    }
    return { res: null, base: null, tried };
  }

  const LIMIT = 64 * 1024 * 1024;              // 超过这个大小不整包落盘：优先用 .list 清单
  const AUTO_SCAN = 512 * 1024 * 1024;         // 清单缺失时自动流式扫描的上限（再大要 --scan）

  /* 流式扫真包：只解析 cpio 成员头，数据段直接丢弃（不落内存、不装字节）。
     真机 pm 是整包下载再解包；演示里这样既能拿到"真清单"，又不用吃掉 455 MB 内存。 */
  async function scanPackage(url, label, io) {
    const res = await sys.fetchImpl(url);
    if (!res.ok) throw new Error("HTTP " + res.status);
    const total = Number(res.headers.get("content-length")) || 0;
    if (!res.body || typeof res.body.getReader !== "function") {
      const buf = new Uint8Array(await res.arrayBuffer());     // 没流（测试桩/老浏览器）
      return parseCpio(buf);
    }
    const reader = res.body.getReader();
    let queue = [], qlen = 0, consumed = 0;
    const hex8 = (a, i) => parseInt(String.fromCharCode(a[i],a[i+1],a[i+2],a[i+3],a[i+4],a[i+5],a[i+6],a[i+7]), 16) || 0;
    async function fill(n) {
      while (qlen < n) {
        const { done, value } = await reader.read();
        if (done) return false;
        if (value && value.length) { queue.push(value); qlen += value.length; }
      }
      return true;
    }
    function take(n) {
      const out = new Uint8Array(n);
      let at = 0;
      while (at < n) {
        const c = queue[0], need = n - at;
        if (c.length <= need) { out.set(c, at); at += c.length; queue.shift(); qlen -= c.length; }
        else { out.set(c.subarray(0, need), at); queue[0] = c.subarray(need); qlen -= need; at += need; }
      }
      consumed += n;
      return out;
    }
    async function skip(n) {
      let left = n;
      while (left > 0) {
        if (!queue.length && !(await fill(1))) return false;
        const c = queue[0];
        if (c.length <= left) { left -= c.length; consumed += c.length; queue.shift(); qlen -= c.length; }
        else { queue[0] = c.subarray(left); qlen -= left; consumed += left; left = 0; }
      }
      return true;
    }
    const recs = [];
    let lastPct = -1;
    for (;;) {
      if (!(await fill(110))) break;
      const hdr = take(110);
      if (String.fromCharCode(hdr[0],hdr[1],hdr[2],hdr[3],hdr[4],hdr[5]) !== "070701") break;
      const mode = hex8(hdr, 14), size = hex8(hdr, 54), ns = hex8(hdr, 94);
      if (!ns || ns > 512) break;
      if (!(await fill(ns))) break;
      let name = dec(take(ns));
      const z = name.indexOf("\0");
      if (z >= 0) name = name.slice(0, z);
      const namePad = (4 - ((110 + ns) % 4)) % 4;
      if (namePad && !(await skip(namePad))) break;
      if (name.startsWith("TRAILER")) break;
      const kind = mode & 0o170000;
      let target;
      if (kind === 0o120000) {                        // 软链：数据就是目标路径
        if (!(await fill(size))) break;
        target = dec(take(size));
      } else if (kind !== 0o40000) {
        if (!(await skip(size))) break;               // 普通文件：字节直接丢
      }
      const dataPad = (4 - (size % 4)) % 4;
      if (dataPad && !(await skip(dataPad))) break;
      recs.push({ name, mode, size, kind, target, data: null });
      if (total && io.progress) {
        const pct = Math.floor((consumed / total) * 100);
        if (pct !== lastPct) { io.progress("流式扫描 " + label + " … " + pct + "%  (" + (consumed / 1048576).toFixed(1) + "/" + (total / 1048576).toFixed(1) + " MiB)"); lastPct = pct; }
      }
    }
    return recs;
  }

  async function readIndex() {
    const { res, base, tried } = await fetchFirst("Packages");
    if (res) {
      const rows = [];
      for (const ln of (await res.text()).split("\n")) {
        const s = ln.trim();
        if (!s || s[0] === "#") continue;
        const c = s.split(/\s+/);
        if (c.length >= 3) rows.push({ name: c[0], version: c[1], file: c[2], size: Number(c[3]) || 0 });
      }
      if (rows.length) return { rows, base, official: isOfficial(base), tried };
    }
    return { rows: Object.keys(PACKAGES).map((n) => Object.assign({ name: n }, PACKAGES[n])),
             base: configuredFeed(), official: false, offline: true, tried };
  }

  function installMembers(recs, pkg) {
    const files = [];
    let n = 0;                                  // 真机 pm 报的"共 N 个成员"含目录
    for (const m of recs) {
      const name = String(m.name).replace(/^\.\//, "").replace(/^\//, "");
      if (!name || name === ".") continue;
      const path = "/" + name;
      const kind = m.kind;
      n++;
      if (kind === 0o40000) { vfs.mkdirp(path, m.mode & 0o7777); continue; }
      if (kind === 0o120000) { vfs.add(path, nLink(m.target)); files.push(path); continue; }
      vfs.add(path, nFile(m.data ? m.data.slice() : null, m.mode & 0o7777, m.size));
      files.push(path);
    }
    vfs.mkdirp("/tmp/pm");
    vfs.writeFile("/tmp/pm/" + pkg + ".files", files.join("\n") + "\n", 0o644);   // 目录不进清单（真机如此）
    return n;
  }

  sys.cmds = {
    pm: async (ctx, argv, io) => {
      const sub = argv[1] || "help";
      if (sub === "version" || sub === "--version" || sub === "-V") { emit(io, PMVER); return 0; }
      if (sub === "help") { emit(io, "用法: pm install <包> | pm remove <包> | pm list | pm available | pm version"); return 0; }
      if (sub === "available") {
        const { rows, base, official, offline } = await readIndex();
        for (const r of rows) emit(io, "  [" + base + "] " + r.name + " " + r.version + " " + r.file + " " + r.size);
        if (!official) emit(io, offline
          ? "pm: 官网 " + configuredFeed() + " 与本站镜像都读不到，用的是内嵌索引"
          : "pm: 官网读不到（浏览器 CORS/离线），当前用本站同源镜像 feed/ —— 与官网同一份内容");
        return rows.length ? 0 : 1;
      }
      if (sub === "list") {
        const d = vfs.dir("/tmp/pm");
        const names = d ? [...d.kids.keys()].filter((x) => x.endsWith(".files")) : [];
        if (!names.length) { emit(io, "pm: (无已装包)"); return 0; }
        for (const x of names) emit(io, "pm: " + x.replace(/\.files$/, ""));
        return 0;
      }
      if (sub === "remove") {
        const pkg = argv[2];
        if (!pkg) { io.err("pm: 用法: pm remove <包>\n"); return 1; }
        const mf = vfs.stat("/tmp/pm/" + pkg + ".files");
        if (!mf || !mf.data) { emit(io, "pm: " + pkg + " 未安装(无 /tmp/pm/" + pkg + ".files)"); return 1; }
        const list = dec(mf.data).split("\n").filter(Boolean);
        for (const p of list.slice().reverse()) vfs.rmrf(p);
        vfs.rmrf("/tmp/pm/" + pkg + ".files");
        delete sys.installed[pkg];
        emit(io, "pm: 移除 " + pkg + " (" + list.length + " 个条目)");
        return 0;
      }
      if (sub !== "install") { io.err("pm: 未知子命令 " + sub + "\n"); return 1; }
      const spec = argv[2];
      if (!spec) { io.err("pm: 用法: pm install <包>\n"); return 1; }

      const local = vfs.stat(ctx.resolve(spec));
      if (local && local.t === "f" && spec.endsWith(".pm")) {
        emit(io, "pm: 安装本地包 " + ctx.resolve(spec) + " -> /");
        const recs = parseCpio(local.data || new Uint8Array(0));
        emit(io, "pm: 安装完成, 共 " + installMembers(recs, vfs.baseOf(spec).replace(/\.pm$/, "")) + " 个成员");
        return 0;
      }

      const { rows, base, official } = await readIndex();
      const pkg = rows.find((r) => r.name === spec);
      if (!pkg) {
        emit(io, "pm: 包 " + spec + " 在所有源均未找到(" + configuredFeed() + " + 本站镜像)");
        return 1;
      }
      emit(io, "pm: 从 " + base + " 下载 " + pkg.file);
      if (!official) emit(io, "pm: （官网 " + configuredFeed() + " 被浏览器 CORS 拦下或不可达，走本站同源镜像 feed/）");

      if (pkg.size > LIMIT) {
        const sizeMiB = (pkg.size / 1048576).toFixed(1);
        // 快路径：站点上有构建产物 <包>.list（真包导出的成员清单），直接登记，不用下 455 MB
        const list = await fetchFirst(spec + ".list");
        if (list.res) {
          const recs = [];
          for (const ln of (await list.res.text()).split("\n")) {
            if (!ln || ln[0] === "#") continue;
            const [name, modeS, sizeS, link] = ln.split("\t");
            const mode = parseInt(modeS, 8);
            recs.push({ name, mode, size: Number(sizeS) || 0, kind: mode & 0o170000, target: link || undefined, data: null });
          }
          const n = installMembers(recs, spec);
          emit(io, "pm: " + pkg.file + " 共 " + sizeMiB + " MiB —— 按站点上的 " + spec +
                  ".list 真清单登记 " + n + " 个成员（真机是整包解包落盘）");
          emit(io, "pm: " + spec + " 安装完成");
          sys.installed[spec] = true;
          return 0;
        }
        // 站点上没清单：像真机那样去读真包，只是流式扫成员头、数据不落盘
        const wantScan = argv.includes("--scan");
        if (!wantScan && pkg.size > AUTO_SCAN) {
          emit(io, "pm: " + pkg.file + " 共 " + sizeMiB + " MiB —— 太大，演示不自动下载实体");
          emit(io, "pm: 加 --scan 可流式扫描真包（要下满 " + sizeMiB + " MiB，随时刷新页面中止）；");
          emit(io, "pm: 或者把构建产物 feed/" + spec + ".list 随站点一起部署（scripts/web-pkg-manifest.py 生成）");
          return 1;
        }
        emit(io, "pm: 站点上没有 " + spec + ".list —— 像真机一样读真包（流式扫成员头，数据不落盘）");
        let recs2;
        try {
          recs2 = await scanPackage(base.replace(/\/$/, "") + "/" + pkg.file, pkg.file, io);
        } catch (e) {
          io.err("pm: " + pkg.file + " 读取失败：" + e.message + "\n");
          return 1;
        }
        if (!recs2.length) { io.err("pm: " + pkg.file + " 不是有效的 newc 归档\n"); return 1; }
        const n2 = installMembers(recs2, spec);
        emit(io, "pm: " + pkg.file + " 扫描完成，按真成员登记 " + n2 + " 个");
        emit(io, "pm: " + spec + " 安装完成");
        sys.installed[spec] = true;
        return 0;
      }

      let buf;
      try {
        const got = await fetchFirst(pkg.file);
        if (!got.res) throw new Error(got.tried.join("; "));
        const res = got.res;
        const total = Number(res.headers.get("content-length")) || pkg.size;
        if (res.body && typeof res.body.getReader === "function" && io.progress) {
          const reader = res.body.getReader();
          const chunks = [];
          let got = 0, last = -1;
          for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            chunks.push(value); got += value.length;
            const pct = Math.floor((got / total) * 100);
            if (pct !== last) { io.progress("下载 " + pkg.file + " … " + pct + "%  (" + (got / 1048576).toFixed(1) + "/" + (total / 1048576).toFixed(1) + " MiB)"); last = pct; }
          }
          buf = new Uint8Array(got);
          let at = 0;
          for (const c of chunks) { buf.set(c, at); at += c.length; }
        } else {
          buf = new Uint8Array(await res.arrayBuffer());
        }
      } catch (e) {
        io.err("pm: " + pkg.file + " 下载失败：" + e.message + "\n");
        io.err("pm: file:// 下浏览器可能拦 fetch —— 用本地 http 服务打开本站就能真下载真解包\n");
        return 1;
      }
      emit(io, "pm: 包下载完成 " + buf.length + " 字节, 开始安装");
      const recs = parseCpio(buf);
      if (!recs.length) { io.err("pm: " + pkg.file + " 不是有效的 newc 归档\n"); return 1; }
      emit(io, "pm: 安装完成, 共 " + installMembers(recs, spec) + " 个成员");
      emit(io, "pm: " + spec + " 安装完成");
      sys.installed[spec] = true;
      return 0;
    },
  };

  /* ---- 按需取真文件字节（web/rootfs/<path>，只读一次）---- */
  sys.ensureData = async (path) => {
    const n = vfs.stat(path);                 // 跟随软链：/bin/busybox → /sbin/busybox
    if (!n || n.t !== "f" || n.data || !n.src) return n;
    try {
      const res = await sys.fetchImpl("rootfs" + n.src);
      if (!res.ok) return n;
      const buf = new Uint8Array(await res.arrayBuffer());
      n.data = buf;
      n.size = buf.length;
    } catch (e) { /* 拿不到就保持"没有实体字节" */ }
    return n;
  };

  /* ---- 交互模式：parlz-sh（默认）/ bash（`bash` 进去，`exit` 回来）---- */
  const mode = { kind: "parlz-sh", bash: null };
  sys.mode = mode;
  sys.enterBash = () => {
    mode.kind = "bash";
    mode.bash = new Bash(vfs, env, sys, shell);
  };
  sys.exitBash = () => { mode.kind = "parlz-sh"; mode.bash = null; };

  /* 跑一条命令并把输出抓成字符串（watch 用）；期间把 tty 摘掉，免得嵌套全屏程序打架 */
  sys.runCapture = async (line) => {
    const buf = [];
    const savedTty = sys.tty;
    sys.tty = null;
    try {
      const io = { out: (s) => buf.push(s), err: (s) => buf.push(s), progress: () => {}, input: null };
      await shell.run(line, io);
    } finally { sys.tty = savedTty; }
    return buf.join("");
  };

  return {
    vfs, env, shell, sys, mode,
    runCapture: (line) => sys.runCapture(line),      // 免得到处写 sys.sys.runCapture
    async run(line, io) {
      if (mode.kind === "bash" && mode.bash) {
        const rc = await mode.bash.runLine(line, io);
        if (mode.bash.exited) { mode.bash.exited = false; sys.exitBash(); }
        return rc;
      }
      return shell.run(line, io);
    },
    prompt() {
      if (mode.kind === "bash")
        return "root@parlz:" + (shell.cwd === (env.HOME || "/root") ? "~" : shell.cwd) + "# ";
      return shell.prompt();
    },
  };
}

function hasImpl(name, sys) { return !!(CMD[name] || (sys && sys.cmds && sys.cmds[name])); }
root.ParlzSystem = { create, parseCpio, sha256, md5, PACKAGES, hasImpl };
})(typeof window !== "undefined" ? window : globalThis);
