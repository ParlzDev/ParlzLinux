#!/usr/bin/env node
// web/system.js（官网演示终端背后的模拟真机）的语义与真装包测试。
// 宿主侧秒级跑完，不起浏览器：node scripts/web-demo-test.js
//
// 覆盖：真 VFS 种子（与真机 rootfs 对齐）、被裁命令的"先没有后能用"、
//       pm 真解包 web/feed/core.pm（sha256 与真包逐字节对齐）、parlz-sh 词法/重定向/
//       管道/退出码（照 userland/sh.c 复刻）、gcc 子集 → 真 ELF → 内置解释器执行。
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const WEB = path.join(__dirname, "..", "web");
eval(fs.readFileSync(path.join(WEB, "system.js"), "utf8"));   // 定义 globalThis.ParlzSystem
const { create, parseCpio, hasImpl } = globalThis.ParlzSystem;

let pass = 0, fail = 0;
const failures = [];
function check(name, cond, extra) {
  if (cond) { pass++; console.log("PASS  " + name); }
  else { fail++; failures.push(name); console.log("FAIL  " + name + (extra !== undefined ? "   << " + extra : "")); }
}

function makeSys(opts) {
  opts = opts || {};
  return create({
    fetchImpl: async (url) => {
      let u = String(url);
      // 官网与本站是同一份内容（本站就是镜像站）：www.parlz.com/feed/x → web/feed/x
      if (/^https?:\/\/www\.parlz\.com\//i.test(u)) {
        if (opts.blockOfficial) { throw new TypeError("Failed to fetch（模拟 CORS 被拦）"); }
        u = u.replace(/^https?:\/\/www\.parlz\.com\//i, "");
      }
      // 站点相对路径：feed/... 与 rootfs/... 都直接映射到 web/ 下
      const file = path.join(WEB, u.replace(/^\.?\//, ""));
      const miss = { ok: false, status: 404, statusText: "Not Found",
                     headers: { get: () => null, forEach: () => {} },
                     text: async () => "", arrayBuffer: async () => new ArrayBuffer(0) };
      if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) return miss;
      if (opts.hideLists && /\.list$/.test(file)) return miss;          // 模拟"站点上没部署清单"
      const stat = fs.statSync(file);
      const buf = opts.stream ? null : fs.readFileSync(file);
      const body = opts.stream
        ? require("stream").Readable.toWeb(fs.createReadStream(file))
        : null;
      return {
        ok: true, status: 200, statusText: "OK",
        body,
        headers: { get: (k) => (String(k).toLowerCase() === "content-length" ? String(stat.size) : null),
                   forEach: () => {} },
        text: async () => (buf || fs.readFileSync(file)).toString("utf8"),
        arrayBuffer: async () => {
          const b = buf || fs.readFileSync(file);
          return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
        },
      };
    },
  });
}

const sys = makeSys();

async function run(line) {
  const out = [];
  const io = { out: (s) => out.push(s), err: (s) => out.push(s), progress: () => {}, input: null };
  const rc = await sys.run(line, io);
  return { text: out.join(""), rc };
}

(async () => {
  /* ---------- 1. 真机种子 ---------- */
  check("VFS: /bin 有 77 项（与真机 ls -l 一致）", sys.vfs.list("/bin").length === 77, sys.vfs.list("/bin").length);
  check("VFS: /usr/bin 有 14 项（全是 busybox 软链）", sys.vfs.list("/usr/bin").length === 14, sys.vfs.list("/usr/bin").length);
  check("VFS: /bin/ls → ../sbin/busybox", sys.vfs.lstat("/bin/ls").link === "../sbin/busybox");
  check("VFS: /sbin/busybox 大小 = 真机 2501752", sys.vfs.stat("/sbin/busybox").size === 2501752);
  check("VFS: /etc/parlz-release 与真机同内容",
        (await run("cat /etc/parlz-release")).text.includes("build-tz: CST+0800"));
  check("VFS: uname -r 与发布号同源",
        (await run("uname -r")).text.trim().startsWith("7.2.5-CST+0800+"));
  check("VFS: /etc/pm/feeds.conf = 真机值",
        (await run("cat /etc/pm/feeds.conf")).text.trim() === "http://www.parlz.com/feed");
  check("VFS: /proc/version 与真机 banner 同源",
        (await run("cat /proc/version")).text.startsWith("Linux version 7.2.5-CST+0800+"));

  /* ---------- 2. 被裁掉的命令：装 core 之前**没有**（真机就是这样）---------- */
  let r = await run("which tr");
  check("trimmed: 装 core 前没有 tr", r.rc === 1 && r.text === "", JSON.stringify(r.text));
  r = await run("nano --version");
  check("trimmed: 装 core 前没有 nano", r.text.includes("sh: nano: not found"), JSON.stringify(r.text));
  r = await run("wc -l /etc/passwd");
  check("trimmed: 装 core 前没有 wc", r.text.includes("sh: wc: not found"), JSON.stringify(r.text));

  /* ---------- 3. pm：真下载真解包 core.pm ---------- */
  r = await run("pm available");
  check("pm: available 的源是官网 www.parlz.com/feed（真机默认镜像）",
        r.text.includes("[http://www.parlz.com/feed]") && r.text.includes("core") && r.text.includes("gcc"),
        JSON.stringify(r.text.slice(0, 90)));
  r = await run("pm install core");
  check("pm: 从官网 www.parlz.com/feed 下载（不是本地相对路径）",
        r.text.includes("pm: 从 http://www.parlz.com/feed 下载 core.pm"), JSON.stringify(r.text.slice(0, 90)));
  check("pm: install core 完成", r.text.includes("pm: core 安装完成"), JSON.stringify(r.text.slice(-120)));
  check("pm: 与真机同样报 348 个成员", r.text.includes("共 348 个成员"),
        JSON.stringify((r.text.match(/共 \d+ 个成员/g) || [])[0]));

  const nano = sys.vfs.stat("/bin/nano");
  check("pm: /bin/nano 落地且大小 = 真包内 1583288", nano && nano.size === 1583288, nano && nano.size);
  check("pm: /bin/nano 是真 ELF（真字节）", nano && nano.data && nano.data[0] === 0x7f && nano.data[1] === 0x45);
  check("pm: /bin/bc 是软链 ../sbin/busybox",
        sys.vfs.lstat("/bin/bc").t === "l" && sys.vfs.lstat("/bin/bc").link === "../sbin/busybox");
  check("pm: /usr/bin 补到 284 项（322 条软链分落 bin/usr-bin）",
        sys.vfs.list("/usr/bin").length === 284, sys.vfs.list("/usr/bin").length);

  const real = parseCpio(fs.readFileSync(path.join(WEB, "feed", "core.pm")));
  const nanoMem = real.find((m) => m.name === "bin/nano");
  const digest = (u8) => crypto.createHash("sha256").update(Buffer.from(u8)).digest("hex");
  check("pm: /bin/nano 与真包成员 sha256 逐字节一致", digest(nano.data) === digest(nanoMem.data),
        digest(nano.data).slice(0, 16) + " vs " + digest(nanoMem.data).slice(0, 16));
  r = await run("file /bin/nano");
  check("pm: file 认出真 ELF", r.text.includes("ELF 64-bit LSB executable, x86-64"), JSON.stringify(r.text));
  r = await run("sha256sum /bin/nano");
  check("pm: sha256sum 与宿主 crypto 一致", r.text.startsWith(digest(nanoMem.data)), r.text.slice(0, 20));
  r = await run("which nano");
  check("pm: which nano → /bin/nano", r.text.trim() === "/bin/nano", JSON.stringify(r.text));
  r = await run("pm list");
  check("pm: list 里有 core", r.text.includes("core"), JSON.stringify(r.text));

  /* ---------- 4. 装完之后被裁命令真的能用 ---------- */
  r = await run("wc -l /etc/passwd");
  check("core: 装完 wc 可用", r.text.trim() === "1 /etc/passwd", JSON.stringify(r.text));
  r = await run("awk -F: '{print $1}' /etc/passwd");
  check("core: 装完 awk 真能用（-F 取第 1 列）", r.text.trim() === "root", JSON.stringify(r.text));
  r = await run("awk '{print NR, $1}' /etc/group");
  check("core: awk 的 NR 也认", r.text.trim().startsWith("1 root"), JSON.stringify(r.text));
  r = await run("tr a-z A-Z < /etc/group");
  check("core: 装完 tr 可用（含输入重定向）", r.text === "ROOT:X:0:\n", JSON.stringify(r.text));
  r = await run("nano /tmp/x");
  check("core: 装完 nano 可用（给出真机信息）", r.text.includes("GNU nano 8.7.1"), JSON.stringify(r.text.slice(0, 60)));

  /* ---------- 5. parlz-sh 语义（照 sh.c 复刻）---------- */
  const cases = [
    ['echo "a  b"', "a  b\n", "引号内的空格不折叠"],
    ['echo a; echo b', "a\nb\n", "引号外的 ; 分段"],
    ['echo "a;b"', "a;b\n", "引号内的 ; 是普通字符"],
    ["echo 'a'$HOME", "a$HOME\n", "词里有单引号 → 整词不展开"],
    ['echo "" a', "a\n", "空引号词整体丢弃"],
    ["echo hi | tr a-z A-Z", "HI\n", "管道"],
    ["printf 'b\\na\\n' | sort", "a\nb\n", "管道 + sort"],
    ["echo hi | tr a-z A-Z | tr A-Z a-z", "hi\n", "三段管道"],
    ["echo a>f", "a>f\n", "`a>f` **不是**重定向（与真机一致）"],
    ["echo x # y", "x # y\n", "行中的 # 是普通字符"],
    ["echo $NOPE", "\n", "未定义变量 → 空串"],
    ["echo $USER@$HOSTNAME", "guest@parlz\n", "变量展开"],
    ["echo ${USER}!", "guest!\n", "${} 展开"],
    ["echo $", "$\n", "裸 $ 原样输出"],
    ["echo $$", "$$\n", "$$ 不是特殊变量（真机也原样）"],
  ];
  for (const [line, want, why] of cases) {
    const rr = await run(line);
    check("sh: " + why + "  [" + line + "]", rr.text === want, JSON.stringify(rr.text));
  }

  r = await run("echo hello > /tmp/f1");
  r = await run("cat /tmp/f1");
  check("sh: > 重定向写文件", r.text === "hello\n", JSON.stringify(r.text));
  r = await run("echo hello >/tmp/f2");
  r = await run("cat /tmp/f2");
  check("sh: >f 粘连写法同样有效", r.text === "hello\n", JSON.stringify(r.text));
  r = await run("echo world >> /tmp/f1");
  r = await run("cat < /tmp/f1");
  check("sh: >> 追加 + < 读入", r.text === "hello\nworld\n", JSON.stringify(r.text));
  r = await run("X=hello");
  r = await run("echo $X");
  check("sh: 行首赋值活过本行（真机是 putenv）", r.text === "hello\n", JSON.stringify(r.text));
  r = await run("export GREET=hi; echo $GREET");
  check("sh: export 赋值可见", r.text === "hi\n", JSON.stringify(r.text));
  r = await run("grep zzz /etc/passwd");
  const rcGrep = r.rc;
  r = await run("echo $?");
  check("sh: $? 反映上一条命令（grep 无匹配 → 1）", rcGrep === 1 && r.text === "1\n", rcGrep + " / " + JSON.stringify(r.text));
  r = await run("cd /nope");
  check("sh: cd 失败也是 0（真机 quirk），并打 perror",
        r.rc === 0 && r.text.includes("cd: No such file"), r.rc + " / " + JSON.stringify(r.text));
  r = await run("cd /tmp");
  r = await run("pwd");
  check("sh: cd/pwd 跟 cwd 走", r.text === "/tmp\n", JSON.stringify(r.text));
  r = await run("cd");
  r = await run("pwd");
  check("sh: 裸 cd 回 $HOME", r.text === "/root\n", JSON.stringify(r.text));
  r = await run("ls && echo hi");
  check("sh: && 不是运算符（真机把它当 ls 的参数）",
        r.text.includes("hi: No such file or directory"), JSON.stringify(r.text));
  r = await run("nosuchcmd");
  check("sh: 未知命令 → 127 + not found", r.rc === 127 && r.text.includes("sh: nosuchcmd: not found"), r.rc);
  r = await run("# just a comment");
  check("sh: 段首 # 是注释（无输出）", r.text === "", JSON.stringify(r.text));
  r = await run("echo 1 | echo 2 | echo 3 | echo 4 | echo 5 | echo 6 | echo 7");
  check("sh: 超过 5 个管道报 too many pipes", r.text.includes("too many pipes"), JSON.stringify(r.text));
  r = await run("echo $USER@$HOSTNAME:$(pwd)");
  check("sh: $() 不执行（真机也原样输出）", r.text.includes("$("), JSON.stringify(r.text));
  r = await run("ls /root /etc | head -3");
  check("sh: 管道喂给 head", r.text.split("\n").filter(Boolean).length === 3, JSON.stringify(r.text));

  /* ---------- 6. pm remove / 重装 ---------- */
  r = await run("pm remove core");
  check("pm: remove 按 /tmp/pm/core.files 倒序删", r.text.includes("pm: 移除 core ("), JSON.stringify(r.text.slice(0, 60)));
  check("pm: remove 之后 /bin/nano 没了", !sys.vfs.stat("/bin/nano"));
  r = await run("pm install core");
  check("pm: 重装 core 成功（可重复）", r.text.includes("pm: core 安装完成"));

  /* ---------- 7. gcc：大包按真清单登记 ---------- */
  r = await run("pm install gcc");
  check("pm: gcc 走真清单（11773 个成员）", r.text.includes("11773"), JSON.stringify((r.text.match(/登记 \d+ 个成员/) || [])[0]));
  check("pm: gcc 后 /bin/gcc → /opt/toolchain/gcc-13/bin/gcc",
        sys.vfs.lstat("/bin/gcc").t === "l" && sys.vfs.lstat("/bin/gcc").link === "/opt/toolchain/gcc-13/bin/gcc");
  let incFiles = 0, incDirs = 0;
  sys.vfs.walkAll("/usr/include", (p, n) => { if (n.t === "f") incFiles++; else if (n.t === "d") incDirs++; });
  check("pm: gcc 后 /usr/include 子树 = 真清单的 5503 文件 + 271 子目录",
        incFiles === 5503 && incDirs - 1 === 271, incFiles + " files / " + (incDirs - 1) + " dirs");

  // 拿真清单逐条核 VFS：抽样的 24 条 + 全部 165 条软链，路径/类型/大小都要对上
  const man = fs.readFileSync(path.join(WEB, "feed", "gcc.list"), "utf8").split("\n")
    .filter((l) => l && l[0] !== "#").map((l) => l.split("\t"));
  const sample = man.filter((_, i) => i % 500 === 0 || (parseInt(man[i][1], 8) & 0o170000) === 0o120000);
  let mismatch = null, checked = 0;
  for (const [name, modeS, sizeS, link] of sample) {
    const mode = parseInt(modeS, 8), kind = mode & 0o170000;
    const node = sys.vfs.lstat("/" + name);
    checked++;
    if (!node) { mismatch = name + " 缺失"; break; }
    if (kind === 0o120000 && (node.t !== "l" || node.link !== link)) { mismatch = name + " 软链目标不符"; break; }
    if (kind === 0o40000 && node.t !== "d") { mismatch = name + " 不是目录"; break; }
    if (kind !== 0o120000 && kind !== 0o40000 && node.size !== Number(sizeS)) { mismatch = name + " 大小 " + node.size + " ≠ " + sizeS; break; }
  }
  check("pm: gcc 清单抽样 " + checked + " 条（含全部软链）与 VFS 逐条对齐", mismatch === null, mismatch);
  r = await run("gcc --version");
  check("gcc: --version 可用", r.text.includes("13.3.0"), JSON.stringify(r.text.split("\n")[0]));

  /* ---------- 8. gcc 编译 → 真 ELF → 执行 ---------- */
  sys.vfs.writeFile("/tmp/hello.c", '#include <stdio.h>\nint main(void) { printf("hello from ELF\\n"); return 7; }\n', 0o644);
  r = await run("gcc /tmp/hello.c -o /tmp/hello");
  check("gcc: 编译产出", r.text.includes("真 ELF64"), JSON.stringify(r.text));
  const hello = sys.vfs.stat("/tmp/hello");
  check("gcc: 产物是合法 ELF64（magic + e_machine=x86-64）",
        hello.data[0] === 0x7f && hello.data[1] === 0x45 && hello.data[18] === 0x3e);
  r = await run("file /tmp/hello");
  check("gcc: file 认出 ELF 可执行", r.text.includes("ELF 64-bit LSB executable, x86-64"), JSON.stringify(r.text));
  r = await run("/tmp/hello");
  check("gcc: ./hello 真的跑起来了（内置 x86-64 解释器）", r.text === "hello from ELF\n", JSON.stringify(r.text));
  check("gcc: 退出码 = return 7", r.rc === 7, r.rc);
  r = await run("chmod 0644 /tmp/hello");
  r = await run("/tmp/hello");
  check("sh: 去掉执行位后 ./hello 被拒（not found or not executable）",
        r.rc === 127 && r.text.includes("not found or not executable"), r.rc + " " + JSON.stringify(r.text));

  /* ---------- 9. 其它命令 ---------- */
  r = await run("grep -c ^root /etc/passwd");
  check("grep: -c 计数", r.text.trim() === "1", JSON.stringify(r.text));
  r = await run("grep -n guest /etc/passwd");
  check("grep: 无匹配且 -n → rc 1 无输出", r.rc === 1 && r.text === "", r.rc + " " + JSON.stringify(r.text));
  r = await run("sed 's/root/admin/' /etc/passwd");
  check("sed: s/// 生效", r.text.startsWith("admin:"), JSON.stringify(r.text));
  r = await run("df");
  check("df: 至少 5 行", r.text.split("\n").length >= 5, JSON.stringify(r.text.slice(0, 40)));
  r = await run("ls -l /bin/nano");
  check("ls: -l 格式（类型 + 大小 + 名字）",
        /^- 1583288 {2}\/bin\/nano/.test(r.text.replace(/\x1b\[\d+m/g, "")), JSON.stringify(r.text));
  r = await run("ls /bin/ls");
  check("ls: 管道/终端下颜色码只包名字（这里是 tty，带 ANSI）",
        r.text.replace(/\x1b\[\d+m/g, "").trim() === "/bin/ls" && r.text.includes("\x1b["), JSON.stringify(r.text));
  r = await run("mount");
  check("mount: 无参打印 /proc/mounts（真机每行后多一个空行）",
        r.text.includes("/dev/vda2 / ext4") && r.text.includes("\n\n"), JSON.stringify(r.text.slice(0, 60)));
  r = await run("umount /dev/vda2");
  check("umount: 挂载表里有就能卸", r.rc === 0, r.rc);
  r = await run("cp /etc/passwd /tmp/p2; cat /tmp/p2");
  check("cp: 真拷贝", r.text.includes("root:x:0:0"), JSON.stringify(r.text));
  r = await run("ln -s /etc/passwd /tmp/p3; readlink /tmp/p3");
  check("ln -s + readlink", r.text.trim() === "/etc/passwd", JSON.stringify(r.text));
  r = await run("mkdir -p /tmp/a/b/c; ls -l /tmp/a");
  check("mkdir -p 建整条路径", r.text.includes("/tmp/a/b") === false && r.text.includes("b"), JSON.stringify(r.text));

  /* ---------- 10. /bin 与 /usr/bin 的**真 ELF** 全都有实现 ---------- */
  const elfNames = [];
  for (const dir of ["/bin", "/usr/bin", "/sbin"]) {
    for (const n of sys.vfs.list(dir) || []) {
      const node = sys.vfs.lstat(dir + "/" + n);
      if (node && node.t === "f") elfNames.push(n);          // 真文件 = 真 ELF（含 core.pm 装回来的 20 个）
    }
  }
  const elfMissing = elfNames.filter((n) => !hasImpl(n, sys.sys));
  check("/bin+/usr/bin+/sbin 的 " + elfNames.length + " 个真 ELF 全都有实现",
        elfMissing.length === 0, elfMissing.join(","));

  // busybox 的 applet 软链：常用的一批也必须有实现（长尾才允许"如实说明"）
  const commonApplets = ["xargs", "diff", "cmp", "comm", "fold", "expand", "unexpand", "rev", "tac", "shuf",
    "split", "truncate", "paste", "xxd", "cksum", "crc32", "hostid", "groups", "tty", "nologin", "usleep",
    "dos2unix", "unix2dos", "nohup", "nice", "timeout", "flock", "pidof", "pgrep", "pkill", "pstree",
    "lsof", "lsblk", "mountpoint", "netstat", "lspci", "lsusb", "hwclock", "sysctl", "swapon", "fstrim",
    "chattr", "lsattr", "blkid", "fdisk", "awk", "false", "unlink", "true", "killall5", "ipcalc",
    "run-parts", "add-shell", "remove-shell", "start-stop-daemon", "last", "wall", "arp", "nslookup"];
  const appletMissing = commonApplets.filter((n) => !hasImpl(n, sys.sys));
  check("常用 applet（" + commonApplets.length + " 个）都有实现", appletMissing.length === 0, appletMissing.join(","));
  r = await run("/bin/acpid");
  check("长尾 applet：给出如实的未实现说明（不假装成功）", r.text.includes("演示里没有对应实现") || r.text.includes("not found"), JSON.stringify(r.text.slice(0, 70)));

  /* ---------- 11. 真字节：web/rootfs 的真文件和 VFS 对上 ---------- */
  const hostHash = (p) => crypto.createHash("sha256").update(fs.readFileSync(path.join(WEB, "rootfs", p))).digest("hex");
  r = await run("sha256sum /bin/cat");
  check("真字节: /bin/cat 的 sha256 与 web/rootfs/bin/cat 一致", r.text.startsWith(hostHash("bin/cat")), r.text.slice(0, 24));
  r = await run("file /bin/cat");
  check("真字节: file /bin/cat → ELF x86-64", r.text.includes("ELF 64-bit LSB executable, x86-64"), JSON.stringify(r.text));
  r = await run("cp /bin/busybox /tmp/bb; sha256sum /tmp/bb");
  check("真字节: cp 复制后摘要不变（2.5 MB 真字节）", r.text.startsWith(hostHash("sbin/busybox")), r.text.slice(0, 24));
  r = await run("cat /bin/cat");
  check("真字节: cat 二进制不刷屏（给出提示）", r.text.includes("二进制文件"), JSON.stringify(r.text.slice(0, 60)));
  r = await run("strings /bin/bash | head -2");
  check("真字节: strings /bin/bash 能读出真内容", r.text.trim().split("\n").length >= 2, JSON.stringify(r.text.slice(0, 60)));

  /* ---------- 12. bash ---------- */
  const bashCases = [
    ["bash -c 'echo hi && echo there'", "hi\nthere\n", "&&"],
    ["bash -c 'false || echo fallback'", "fallback\n", "||"],
    ["bash -c 'echo $((2+3*4))'", "14\n", "$(( ))"],
    ["bash -c 'for i in a b c; do echo $i; done'", "a\nb\nc\n", "for"],
    ["bash -c 'if [ -f /etc/passwd ]; then echo yes; else echo no; fi'", "yes\n", "if"],
    ["bash -c 'x=5; if [ $x -gt 3 ]; then echo big; fi'", "big\n", "变量 + 比较"],
    ["bash -c 'echo a | tr a-z A-Z'", "A\n", "管道"],
    ["bash -c 'printf \"%s\\n\" one two | wc -l'", "2\n", "printf + 管道"],
    ["bash -c 'echo $(echo nested)'", "nested\n", "$( )"],
    ["bash -c 'f() { echo func $1; }; f arg'", "func arg\n", "函数"],
    ["bash -c 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'", "0\n1\n2\n", "while"],
    ["bash -c 'case abc in a*) echo match;; *) echo no;; esac'", "match\n", "case"],
    ["bash -c 'echo hi > /tmp/b1; cat /tmp/b1'", "hi\n", "重定向"],
    ["bash -c 'echo $USER ${HOME}'", "guest /root\n", "变量展开"],
    ["bash -c 'echo one; echo two'", "one\ntwo\n", "; 分段"],
    ["bash -c 'x=1; echo ${x:-def} ${y:-def}'", "1 def\n", "${x:-默认}"],
    ["bash -c 'echo a && echo b || echo c'", "a\nb\n", "&& || 优先级"],
  ];
  for (const [line, want, why] of bashCases) {
    const rr = await run(line);
    check("bash: " + why + "  [" + line.slice(0, 46) + "]", rr.text === want, JSON.stringify(rr.text));
  }
  r = await run("bash -c 'nosuchcmd 2>/dev/null; echo rc=$?'");
  check("bash: 2>/dev/null 把 stderr 丢掉", r.text === "rc=127\n", JSON.stringify(r.text));
  r = await run("bash -c 'echo err >&2'; ");
  check("bash: 未重定向的 stderr 照常出来", r.text.includes("err"), JSON.stringify(r.text));
  r = await run("sh -c 'echo from-sh && echo ok'");
  check("sh: 带参走 bash（真机 shx.c 的判别）", r.text === "from-sh\nok\n", JSON.stringify(r.text));
  sys.vfs.writeFile("/tmp/bs.sh", "#!/bin/bash\necho script $1 $2\nfor f in /etc/passwd /etc/group; do echo -n \"$f \"; test -f $f && echo ok; done\n", 0o755);
  r = await run("bash /tmp/bs.sh A B");
  check("bash: 跑脚本 + 位置参数", r.text.startsWith("script A B\n") && r.text.includes("/etc/passwd ok"), JSON.stringify(r.text));

  // 交互式 bash：提示符变 root@parlz:~#，exit 回 parlz-sh
  r = await run("bash");
  check("bash: 无参进入 bash 模式", sys.mode.kind === "bash", sys.mode.kind);
  check("bash: 模式里的提示符 = root@parlz:~#", sys.prompt() === "root@parlz:~# ", JSON.stringify(sys.prompt()));
  r = await run("echo $((6*7))");
  check("bash: 模式里能算 6*7", r.text === "42\n", JSON.stringify(r.text));
  r = await run("exit");
  check("bash: exit 退回 parlz-sh", sys.mode.kind === "parlz-sh" && sys.prompt().startsWith("guest@parlz"), sys.mode.kind);

  /* ---------- 13. ./xxx 执行 ELF（按名字派发到实现）---------- */
  await run("cd /bin");
  r = await run("./cat /etc/group");
  check("./cat 能跑（ELF 按名字派发）", r.text === "root:x:0:\n", JSON.stringify(r.text));
  r = await run("./bash -c 'echo via-dot-slash'");
  check("./bash -c 能跑", r.text === "via-dot-slash\n", JSON.stringify(r.text));
  r = await run("./uname -r");
  check("./uname -r 能跑", r.text.trim().startsWith("7.2.5-"), JSON.stringify(r.text));
  r = await run("/usr/bin/pwd");
  check("绝对路径调用也能跑", r.text.trim().startsWith("/"), JSON.stringify(r.text));
  r = await run("./nosuchbin");
  check("./ 不存在的 ELF：报 not found or not executable", r.text.includes("not found or not executable"), JSON.stringify(r.text));
  await run("cd /root");

  /* ---------- 14. 补齐的命令 ---------- */
  r = await run("adduser -D bob");
  r = await run("grep -c ^bob /etc/passwd");
  check("adduser: 真写 /etc/passwd", r.text.trim() === "1", JSON.stringify(r.text));
  r = await run("grep bob /etc/group");
  check("adduser: 真写 /etc/group", r.text.startsWith("bob:"), JSON.stringify(r.text));
  check("adduser: 建了家目录", !!sys.vfs.dir("/home/bob"));
  await run("touch /tmp/chownme");
  r = await run("chown bob /tmp/chownme");
  check("chown: 认 /etc/passwd 里的用户", r.rc === 0 && sys.vfs.stat("/tmp/chownme").uid === 1000,
        r.rc + " uid=" + (sys.vfs.stat("/tmp/chownme") || {}).uid);
  r = await run("chown nosuchuser /tmp/chownme");
  check("chown: 未知用户报 invalid user", r.text.includes("invalid user"), JSON.stringify(r.text));
  r = await run("deluser bob");
  check("deluser: 真删（passwd 里没了）", sys.vfs.list("/etc").length > 0 && !Buffer.from(sys.vfs.stat("/etc/passwd").data).toString("utf8").includes("bob"), r.rc);
  r = await run("mktemp");
  check("mktemp: 建出真文件并打印路径", r.text.trim().startsWith("/tmp/tmp.") && !!sys.vfs.stat(r.text.trim()), JSON.stringify(r.text));
  r = await run("dd if=/etc/passwd of=/tmp/pw bs=8 count=2");
  check("dd: 真拷贝 + 真统计", r.text.includes("2+0 records in") && r.text.includes("16 bytes"), JSON.stringify(r.text.split("\n").slice(1).join(" | ")));
  check("dd: 目标文件 16 字节", sys.vfs.stat("/tmp/pw").size === 16, sys.vfs.stat("/tmp/pw").size);
  r = await run("kill -l");
  check("kill -l: 列出信号", r.text.includes("HUP") && r.text.includes("KILL"), JSON.stringify(r.text.slice(0, 40)));
  r = await run("kill 99999");
  check("kill: 不存在的 pid 报 No such process", r.text.includes("No such process"), JSON.stringify(r.text));
  r = await run("ifdown eth0");
  r = await run("ifconfig");
  check("ifdown/ifconfig: 接口真的 down 了", !r.text.includes("inet addr:10.0.2.15"), JSON.stringify(r.text.slice(0, 90)));
  r = await run("ifup eth0");
  r = await run("ifconfig");
  check("ifup/ifconfig: 又起来了", r.text.includes("inet addr:10.0.2.15"), JSON.stringify(r.text.slice(0, 90)));
  r = await run("route -n");
  check("route -n: 路由表", r.text.includes("10.0.2.0") && r.text.includes("UG"), JSON.stringify(r.text.slice(0, 80)));
  r = await run("busybox ls /etc | head -2");
  check("busybox: 多路复用 applet", r.text.trim().split("\n").length === 2, JSON.stringify(r.text));
  r = await run("opkg --version");
  check("opkg: 版本（委托 pm 的同一套 feed）", r.text.includes("0.8.0"), JSON.stringify(r.text));
  r = await run("time echo hi");
  check("time: 打 real/user/sys 且真计时", r.text.includes("hi") && r.text.includes("real") && r.text.includes("user"), JSON.stringify(r.text.slice(0, 60)));
  r = await run("lsmod");
  check("lsmod: 表头（全内建内核没有模块）", r.text.includes("Module"), JSON.stringify(r.text));
  r = await run("chroot /tmp cat /etc/group");
  check("chroot: 换到空根后连 cat 都找不到（真的被限住了）", r.text.includes("cat: not found"), JSON.stringify(r.text));
  await run("mkdir -p /newroot/bin /newroot/etc");
  await run("cp /bin/cat /newroot/bin/cat");
  await run("echo hello-chroot > /newroot/etc/data");
  r = await run("chroot /newroot cat /etc/data");
  check("chroot: 换到自带 /bin 的根后命令真能跑", r.text === "hello-chroot\n", JSON.stringify(r.text));
  r = await run("cat /newroot/etc/data");
  check("chroot: 出来后原根的文件还在原地", r.text === "hello-chroot\n", JSON.stringify(r.text));

  /* ---------- 15. 官网被 CORS 拦时的退回路径（同一个 pm 逻辑）---------- */
  const sys2 = makeSys({ blockOfficial: true });
  const run2 = async (line) => {
    const out = [];
    const io = { out: (s) => out.push(s), err: (s) => out.push(s), progress: () => {}, input: null };
    const rc = await sys2.run(line, io);
    return { text: out.join(""), rc };
  };
  await run2("mkdir -p /tmp/x");
  r = await run2("pm available");
  check("CORS 场景: 索引退回本站同源镜像并说明原因",
        r.text.includes("官网读不到（浏览器 CORS/离线）") && r.text.includes("[feed]"), JSON.stringify(r.text.slice(0, 110)));
  r = await run2("pm install pm");
  check("CORS 场景: 装小包仍成功（走镜像）", r.text.includes("pm: pm 安装完成"), JSON.stringify(r.text.slice(-90)));
  r = await run2("cat /etc/pm/feeds.conf");
  check("CORS 场景: 盘上 feeds.conf 仍是官网地址", r.text.trim() === "http://www.parlz.com/feed", JSON.stringify(r.text));
  r = await run2("echo $PM_FEED");
  check("CORS 场景: PM_FEED 环境变量 = 官网（真机 init 同款）", r.text.trim() === "http://www.parlz.com/feed", JSON.stringify(r.text));

  /* ---------- 16. 产物级：feed 里的 pm.pm 必须带官网源 ---------- */
  const pmPkg = parseCpio(fs.readFileSync(path.join(WEB, "feed", "pm.pm")));
  const conf = pmPkg.find((m) => m.name === "etc/pm/feeds.conf");
  check("产物: feed/pm.pm 里的 /etc/pm/feeds.conf = 官网地址（装 pm 不会把源改回开发机）",
        conf && Buffer.from(conf.data).toString("utf8").trim() === "http://www.parlz.com/feed",
        conf ? JSON.stringify(Buffer.from(conf.data).toString("utf8")) : "包里没有 feeds.conf");
  const idx = fs.readFileSync(path.join(WEB, "feed", "Packages"), "utf8").trim().split("\n");
  const coreLine = idx.find((l) => l.startsWith("core "));
  const coreSize = fs.statSync(path.join(WEB, "feed", "core.pm")).size;
  check("产物: Packages 里 core 的尺寸 = 实际文件尺寸（索引与包必须对得上）",
        coreLine && Number(coreLine.split(/\s+/)[3]) === coreSize, coreLine + " vs " + coreSize);
  const pmLine = idx.find((l) => l.startsWith("pm "));
  const pmSize = fs.statSync(path.join(WEB, "feed", "pm.pm")).size;
  check("产物: Packages 里 pm 的尺寸 = 实际文件尺寸",
        pmLine && Number(pmLine.split(/\s+/)[3]) === pmSize, pmLine + " vs " + pmSize);

  /* ---------- 17. 站点上没部署 .list 时：像真机一样读真包（流式扫成员头）---------- */
  const sys3 = makeSys({ hideLists: true, stream: true });
  const run3 = async (line) => {
    const out = [];
    const io = { out: (s) => out.push(s), err: (s) => out.push(s), progress: () => {}, input: null };
    const rc = await sys3.run(line, io);
    return { text: out.join(""), rc };
  };
  r = await run3("pm install gcc");
  check("缺清单: 自动流式扫真包（真的读了 454.7 MiB 的头，数据不落盘）",
        r.text.includes("站点上没有 gcc.list") && r.text.includes("扫描完成"), JSON.stringify(r.text.slice(0, 120)));
  check("缺清单: 登记出的成员数与真清单一致（11773）", r.text.includes("11773") || r.text.includes("登记 11773 个"),
        JSON.stringify((r.text.match(/登记 \d+ 个/) || [])[0]));
  let inc3 = 0;
  sys3.vfs.walkAll("/usr/include", (p, n) => { if (n.t === "f") inc3++; });
  check("缺清单: /usr/include 子树仍是真清单的 5503 个文件", inc3 === 5503, inc3);
  check("缺清单: /bin/gcc 软链目标正确",
        sys3.vfs.lstat("/bin/gcc") && sys3.vfs.lstat("/bin/gcc").link === "/opt/toolchain/gcc-13/bin/gcc");
  r = await run3("pm install clang");
  check("缺清单且 >512 MiB: 不自动下载，提示 --scan 或部署 .list",
        r.text.includes("太大，演示不自动下载实体") && r.text.includes("--scan"), JSON.stringify(r.text.slice(0, 140)));

  /* ---------- 18. 整页模拟机（sim.html）的全屏程序：nano / less / top ---------- */
  function fakeTty(keys, rows, cols) {
    return {
      rows: rows || 24, cols: cols || 80, buf: "", script: keys.slice(), reads: 0,
      size() { return { rows: this.rows, cols: this.cols }; },
      write(s) { this.buf += String(s); },
      async readKey() {
        if (++this.reads > 800) throw new Error("readKey 调用次数异常（全屏程序没退出？）");
        return this.script.length ? this.script.shift() : "\x18";     // 兜底：^X
      },
      // 粗略把 ANSI 序列去掉，只留可见文本，便于断言
      text() {
        return this.buf.replace(/\x1b\[[0-9;?]*[A-Za-z@`~]/g, "").replace(/\x1b[=>78MDEHc]/g, "");
      },
    };
  }
  const tty18 = fakeTty(["h", "i", "\r", "t", "h", "e", "r", "e",
                         "\x17", "t", "h", "e", "r", "e", "\r",   // ^W 搜索
                         "\x0f", "\r",                             // ^O 保存 → 回车
                         "\x18"]);                                 // ^X 退出
  sys.sys.tty = tty18;                       // 用已装好 core 的那台（nano/less/top 都在 core.pm 里）
  const sys18 = sys;
  const run18 = async (line) => {
    const out = [];
    const io = { out: (s) => out.push(s), err: (s) => out.push(s), progress: () => {}, input: null };
    const rc = await sys18.run(line, io);
    return { text: out.join(""), rc };
  };
  r = await run18("nano /tmp/note.txt");
  check("nano: 全屏编辑后正常退出", r.rc === 0 && r.text === "", r.rc + " " + JSON.stringify(r.text));
  check("nano: 画了标题栏与快捷键栏", tty18.buf.includes("GNU nano 8.7.1") && tty18.buf.includes("^O"), "");
  check("nano: 用了备用屏（?1049h/l）", tty18.buf.includes("?1049h") && tty18.buf.includes("?1049l"));
  check("nano: ^W 搜索有反馈", tty18.buf.includes("找到: there"), JSON.stringify(tty18.buf.slice(-60)));
  r = await run18("cat /tmp/note.txt");
  check("nano: 存的文件真的是敲进去的内容", r.text === "hi\nthere\n", JSON.stringify(r.text));
  r = await run18("ls -l /tmp/note.txt");
  check("nano: 文件落进 VFS（0644）", r.text.includes("note.txt"), JSON.stringify(r.text));

  // nano 改已有文件 + 退出确认
  const tty18b = fakeTty(["x", "\x18", "n"]);                    // 改一下 → ^X → 选不保存
  sys18.sys.tty = tty18b;
  r = await run18("nano /tmp/note.txt");
  r = await run18("cat /tmp/note.txt");
  check("nano: 退出时选 No → 改动被丢弃", r.text === "hi\nthere\n", JSON.stringify(r.text));
  check("nano: 提示过 'Save modified buffer?'", tty18b.buf.includes("Save modified buffer"), JSON.stringify(tty18b.buf.slice(-80)));

  // less 分页器
  const tty18c = fakeTty([" ", "b", "q"]);
  sys18.sys.tty = tty18c;
  r = await run18("less /etc/passwd");
  check("less: 全屏分页并正常退出", r.rc === 0 && tty18c.buf.includes("root:x:0"), JSON.stringify(r.text));
  check("less: 底部状态栏显示文件名/进度", tty18c.buf.includes("(END)") || tty18c.buf.includes("%"), "");

  // top 实时视图
  const tty18d = fakeTty(["q"]);
  sys18.sys.tty = tty18d;
  r = await run18("top");
  check("top: 实时刷新并 q 退出", r.rc === 0 && tty18d.buf.includes("PID"), JSON.stringify(tty18d.text().slice(0, 60)));
  sys18.sys.tty = tty18;

  // 没有 tty 的老终端（首页那个）仍退回文字说明
  sys.sys.tty = null;
  r = await run("nano /tmp/x.txt");
  check("首页终端（无 tty）里 nano 给的是友好说明", r.text.includes("sim.html"), JSON.stringify(r.text.slice(0, 80)));

  /* ---------- 19. 首页那块是"代码展示"，不是模拟机了 ---------- */
  const idxHtml = fs.readFileSync(path.join(WEB, "index.html"), "utf8");
  check("首页: 没有可输入的终端了（#tin 已移除）", !/id="tin"/.test(idxHtml));
  check("首页: 有代码展示容器与文件名标题", /id="code"/.test(idxHtml) && /id="codehead"/.test(idxHtml));
  check("首页: 不再加载 system.js（模拟机只在 sim.html）",
        !/src="system\.js"/.test(idxHtml) && /src="codeshow\.js"/.test(idxHtml));
  check("首页: 代码框仍单独用 zeoseven 的英文字体（非阻塞 link + 只作用 .codebox .code）",
        /<link rel="stylesheet" href="https:\/\/fontsapi\.zeoseven\.com\/184\/main\/result\.css" media="print" onload="this\.media='all'">/.test(idxHtml) &&
        /\.codebox \.code \{[^}]*LXGW 975 Yuan SC 400W/.test(idxHtml) &&
        !/^[ \t]*@import/m.test(idxHtml));
  const show = fs.readFileSync(path.join(WEB, "codeshow.js"), "utf8");
  const langs = ["hello.py", "hello.c", "hello.go", "hello.rs"];
  check("代码展示: 四个片段都在（Python / C / Go / Rust）",
        langs.every((f) => show.includes(f)), langs.filter((f) => !show.includes(f)).join(","));
  check("代码展示: 逐字敲 → 停 0.5s → 逐字删（TYPE/HOLD/ERASE 三段都在）",
        /TYPE_MS\s*=/.test(show) && /HOLD_MS\s*=\s*500/.test(show) &&
        /ERASE_MS\s*=/.test(show) && /function erase\(/.test(show) &&
        /later\(type/.test(show) && /later\(back/.test(show) && /later\(erase/.test(show) &&
        show.includes("reduced"));
  check("代码展示: 导出 mount + 收尾（AJAX 翻页时那条 setTimeout 链必须能停）",
        /ParlzCodeShow = \{ mount/.test(show) && /let idx = 0, alive = true/.test(show) &&
        /const later = \(fn, ms\) => \{ setTimeout\(\(\) => \{ if \(alive\) fn\(\); \}, ms\); \}/.test(show) &&
        /return \(\) => \{\s*\n\s*alive = false;/.test(show), "");
  const simHtml = fs.readFileSync(path.join(WEB, "sim.html"), "utf8");
  check("模拟机: 仍在 sim.html（终端 + 全屏程序入口）",
        /id="screen"/.test(simHtml) && /term\.js/.test(simHtml) && /system\.js/.test(simHtml));

  /* ---------- 20. 首页代码动画：假 DOM + 立即执行的定时器，把状态机走一遍 ---------- */
  {
    const cls = { set: new Set(), add(c) { this.set.add(c); }, remove(c) { this.set.delete(c); },
                  toggle(c, on) { if (on) this.set.add(c); else this.set.delete(c); },
                  contains(c) { return this.set.has(c); } };
    const box = { innerHTML: "", scrollTop: 0, scrollHeight: 0,
                  parentElement: { classList: cls, scrollTop: 0, scrollHeight: 0 } };
    const head = { textContent: "" };
    const savedDoc = global.document, savedTimeout = global.setTimeout;
    const pending = [];
    global.document = { readyState: "complete", addEventListener: () => {} };
    global.setTimeout = (fn) => { pending.push(fn); return pending.length; };
    try {
      eval(fs.readFileSync(path.join(WEB, "codeshow.js"), "utf8"));
      // 翻页改成 AJAX 后 codeshow 不再自己跑，必须按 router 的方式装配一次
      const root = { querySelector: (sel) => (sel === "#code" ? box : sel === "#codehead" ? head : null) };
      const stop = globalThis.ParlzCodeShow.mount(root);
      check("代码动画: mount 返回收尾函数", typeof stop === "function", typeof stop);
      const visible = () => box.innerHTML.replace(/<[^>]*>/g, "").replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">").length;
      const trace = [];
      const heads = [head.textContent];
      let steps = 0, sawTypingCls = false, sawErasingCls = false, sawHot = false;
      while (pending.length && steps < 4000 && heads.length < 3) {
        pending.shift()();
        steps += 1;
        trace.push(visible());
        if (cls.contains("typing")) sawTypingCls = true;
        if (cls.contains("erasing")) sawErasingCls = true;
        if (box.innerHTML.indexOf('class="cb-hot"') >= 0) sawHot = true;
        if (head.textContent !== heads[heads.length - 1]) heads.push(head.textContent);
      }
      const grow = trace.some((v, i) => i && v > trace[i - 1]);
      const shrink = trace.some((v, i) => i && v < trace[i - 1]);
      const maxLen = Math.max.apply(null, trace);
      check("代码动画: 有逐字变长（敲）", grow);
      check("代码动画: 有逐字变短（删）", shrink);
      check("代码动画: 刚落下的那个字单独包了 .cb-hot（闪一下）", sawHot);
      check("代码动画: 整段敲满过（最长 " + maxLen + " 字符）", maxLen > 80, maxLen);
      check("代码动画: 敲→停→删→换下一个文件（" + heads.map((h) => h.split(" ")[0]).join(" → ") + "）",
            heads.length === 3 && heads[1].startsWith("hello.c") && heads[2].startsWith("hello.go"),
            JSON.stringify(heads.map((h) => h.slice(0, 12))));
      check("代码动画: 一轮步数有限且合理（" + steps + " 步）", steps > 200 && steps < 2000, steps);
      check("代码动画: .typing / .erasing 两个态真的切过（边框发光与那个点的脉动挂在它们上面）",
            sawTypingCls && sawErasingCls, sawTypingCls + "/" + sawErasingCls);

      // 收尾之后：队列里剩下的定时器全都不能再动这块 DOM
      const snapshot = box.innerHTML;
      stop();
      const rest = pending.length;
      for (let i = 0; i < rest; i++) pending.shift()();
      check("代码动画: 离开首页后链条停住（不再往摘掉的节点里写）",
            box.innerHTML === snapshot && pending.length === 0,
            "剩 " + pending.length + " 个定时器，画面" + (box.innerHTML === snapshot ? "没变" : "被改了"));
      check("代码动画: 收尾把 typing/erasing 摘干净（离开首页不留发光态）",
            !cls.contains("typing") && !cls.contains("erasing"),
            [...cls.set].join(","));
    } finally {
      global.document = savedDoc;
      global.setTimeout = savedTimeout;
    }
  }

  /* ---------- 21. 全站导航栏一致 + 每页都加载设置模块 ---------- */
  const allPages = ["index.html", "sim.html", "packages.html", "download.html", "license.html", "git.html"];
  const navOf = (f) => (fs.readFileSync(path.join(WEB, f), "utf8")
    .match(/<nav class="nav">[\s\S]*?<\/nav>/) || [""])[0]
    .replace(/\s*aria-current="page"/g, "")
    .replace(/#[^"]*/g, "");
  const navs = allPages.map((f) => navOf(f));
  check("导航: 六个页面的导航栏逐字一致（仓库排在四个页面链接之后、模拟机之前）",
        navs.every((n) => n === navs[0]) &&
        /nav\.licenses"[\s\S]*nav\.git"[\s\S]*nav\.sim"/.test(navs[0]),
        navs.filter((n) => n !== navs[0]).length + " 页不一致");
  const noSettings = allPages.filter((f) => !/src="settings\.js"/.test(fs.readFileSync(path.join(WEB, f), "utf8")));
  check("设置模块: 六个页面都加载 settings.js（否则那一页主题/语言是死的）",
        noSettings.length === 0, noSettings.join(","));
  const appSrc = fs.readFileSync(path.join(WEB, "app.js"), "utf8");
  check("设置模块: app.js 里不再重复定义（避免顶层 const 撞名）",
        !/const LANGS =/.test(appSrc) && !/const LANG_HOOKS =/.test(appSrc) && !/function syncLinks/.test(appSrc));
  const bootSrc = fs.readFileSync(path.join(WEB, "boot.js"), "utf8");
  check("设置模块: boot.js 仍负责首屏定 lang/theme（先于 CSS 生效）",
        /dataset\.theme = theme/.test(bootSrc) && /dataset\.lang = lang/.test(bootSrc));

  /* ---------- 22. settings.js（六页共用的主题/语言模块）：假 DOM 跑一遍 ---------- */
  {
    const store = {};
    const links = [
      { href: "index.html", getAttribute() { return this.href; }, setAttribute(k, v) { this.href = v; } },
      { href: "sim.html", getAttribute() { return this.href; }, setAttribute(k, v) { this.href = v; } },
    ];
    const sel = { value: "zh-CN", handlers: {}, addEventListener(ev, fn) { this.handlers[ev] = fn; }, dispatch(ev) { this.handlers[ev](); } };
    const btn = { textContent: "", title: "", addEventListener() {} };
    const root = { dataset: { lang: "zh-CN", theme: "dark" }, classList: { remove() {} } };
    const saved = { document: global.document, localStorage: global.localStorage, location: global.location,
                    history: global.history, setInterval: global.setInterval, addEventListener: global.addEventListener };
    global.setInterval = () => 0;        // 真身有个 1s 轮询，测试里别让它挂着不退出
    global.document = {
      documentElement: root,
      getElementById: (id) => (id === "theme" ? btn : id === "lang" ? sel : null),
      querySelectorAll: () => links,
    };
    global.localStorage = { getItem: (k) => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = v; } };
    global.location = { hash: "", pathname: "/sim.html", search: "" };
    global.history = { replaceState() {} };
    global.addEventListener = () => {};
    try {
      eval(fs.readFileSync(path.join(WEB, "settings.js"), "utf8"));
      check("settings: 初始化后下拉框跟着当前语言走", sel.value === "zh-CN", sel.value);
      check("settings: 主题按钮的文案/提示已填", btn.textContent.length > 0 && btn.title.length > 0, btn.textContent);
      check("settings: 初始就把站内链接带上状态 hash",
            links[0].href === "index.html#lang=zh-CN&theme=dark", links[0].href);
      sel.value = "pt";
      sel.dispatch("change");
      check("settings: 在页面里切语言 → html.lang / localStorage / 链接一起变",
            root.dataset.lang === "pt" && store["parlz-lang"] === "pt" &&
            links[0].href === "index.html#lang=pt&theme=dark", root.dataset.lang + " " + store["parlz-lang"] + " " + links[0].href);
    } finally {
      global.document = saved.document; global.localStorage = saved.localStorage;
      global.location = saved.location; global.history = saved.history;
      global.setInterval = saved.setInterval; global.addEventListener = saved.addEventListener;
    }
  }

  /* ---------- 23. 全站字体 + 手机版 ---------- */
  const css = fs.readFileSync(path.join(WEB, "style.css"), "utf8");
  const termSrc = fs.readFileSync(path.join(WEB, "term.js"), "utf8");
  const pagesSrc = allPages.map((f) => fs.readFileSync(path.join(WEB, f), "utf8"));
  const noFontLink = allPages.filter((f, i) =>
    !/<link rel="stylesheet" href="https:\/\/fontsapi\.zeoseven\.com\/198\/main\/result\.css" media="print" onload="this\.media='all'">/.test(pagesSrc[i]));
  check("字体: 全站正文仍是 zeoseven 198，但六页都用**非阻塞 link** 加载（media=print + onload 切回 all）",
        noFontLink.length === 0, noFontLink.join(","));
  check("字体: style.css 里不许再有 @import 规则（导入表会挡首次绘制，那 47 KB 就是手机端白屏几秒的元凶）",
        !/^[ \t]*@import/m.test(css));
  check("字体: 六页都 preconnect 了字体域名（省一次 DNS+TLS 往返）",
        allPages.filter((f, i) => !/<link rel="preconnect" href="https:\/\/fontsapi\.zeoseven\.com" crossorigin>/.test(pagesSrc[i])).length === 0);
  check("字体: 网页字体之前先有系统字体兜底（非阻塞加载时首屏就靠它们，不能是空的）",
        /--ui:\s*"JiangChengYueHuTi 400W", -apple-system/.test(css) && /--mono:[^;]*ui-monospace/.test(css) &&
        /font:\s*14px\/1\.75 var\(--ui\)/.test(css));
  check("字体: 等宽栈里有中日韩/复杂文字兜底（不然中文会显示不出来）",
        /Noto Sans CJK SC/.test(css) && /Noto Sans Arabic/.test(css) && /JiangChengYueHuTi 400W/.test(css));
  check("字体: 代码/终端仍走等宽栈（pre / code / .screen）",
        /pre, code, \.screen \{ font-family: var\(--mono\)/.test(css));
  check("慢网络: 翻页与取索引的缓存策略按域名分（线上用浏览器缓存，本地预览才每次回源）",
        /const CACHE_POLICY = /.test(appSrc) && /\? "no-cache" : "default"/.test(appSrc) &&
        /fetch\(FEED_REL \+ "\/Packages", \{ cache: CACHE_POLICY \}\)/.test(appSrc) &&
        /await fetch\(file, \{ cache: CACHE_POLICY \}\)/.test(fs.readFileSync(path.join(WEB, "router.js"), "utf8")));
  check("手机: 模拟机整屏按 dvh（100vh 是被地址栏挡住的大视口，提示符会落在看不见的地方）",
        /\.simpage \{ padding: 0; height: 100vh; height: 100dvh;/.test(css));
  check("字体: 非阻塞加载后终端会等字体到货重新量字符格（不然 swap 完行列就对不上）",
        /document\.fonts/.test(termSrc) && /ff\.addEventListener\("loadingdone", this\._onFonts\)/.test(termSrc) &&
        /removeEventListener\("loadingdone", this\._onFonts\)/.test(termSrc));
  check("慢网络: 模拟机屏幕上烘了一行 loading（终端起来前不是纯黑框）",
        /data-i18n-attr="aria-label:sim\.title">loading…/.test(simHtml));
  check("慢网络: 换语言前的隐藏时限压到 0.6s（读不到内容比闪一下中文严重）",
        /html\.i18n-wait main \{ visibility: hidden; animation: i18n-show 0s linear \.6s forwards; \}/.test(css) &&
        /i18n-show 0s linear \.6s forwards !important/.test(css));
  check("手机: 终端有隐藏输入框桥（系统键盘只往输入框发字符）",
        /_bindMobileInput/.test(termSrc) && /class = "term-in"|className = "term-in"/.test(termSrc) &&
        /addEventListener\("input"/.test(termSrc) && /touchstart/.test(termSrc));
  check("手机: 终端里的输入框不会抢走桌面按键（放行自己的那个）",
        /tag === "INPUT" && t !== this\.input/.test(termSrc));
  check("手机: 每次重画都把隐藏输入框留在屏幕里（render 的 innerHTML 会把它清掉，必须补回）",
        /if \(this\.input\) this\.el\.append\(this\.input\);/.test(termSrc) &&
        /this\.input && this\.input\.isConnected/.test(termSrc));
  check("导航: 高度被钉住（flex: 0 0 auto + min-height），模拟机页不会被压缩",
        /\.top \{[\s\S]*?flex: 0 0 auto; min-height: 2\.35rem;/.test(css));
  check("导航: 宽屏下不许换行（换行是各页高度不一致的根因）",
        /@media \(min-width: 641px\) \{\s*\.nav \{ flex-wrap: nowrap; overflow-x: auto; scrollbar-width: none; \}/.test(css) &&
        /\.nav > \* \{ flex: 0 0 auto; \}/.test(css));
  check("导航: 六页可用宽度一致（滚动条占位滚定，模拟机不占）",
        /scrollbar-gutter: stable/.test(css));
  check("导航: 模拟机页头部的上边距与左右留白和其它页一致（桌面 56/20、手机 26/16）",
        /\.simpage main \{[^}]*padding: 56px 20px 12px/.test(css) &&
        /\.simpage \.top \{ margin-top: 0; \}/.test(css) &&
        /body \{[\s\S]*?padding: 56px 20px 96px/.test(css) &&
        /\.simpage main \{ padding: 26px 16px 8px; \}/.test(css) &&
        /body \{ padding: 26px 16px 56px/.test(css));
  check("手机: 小屏媒体查询把字号/留白/终端都调过",
        /@media \(max-width: 640px\)/.test(css) && /\.screen \{ font-size: 11\.5px/.test(css) &&
        /webkit-text-size-adjust: 100%/.test(css));

  /* ---------- 24. AJAX 翻页（router.js）：结构 + 时长对齐 + 收尾 ---------- */
  {
    const rSrc = fs.readFileSync(path.join(WEB, "router.js"), "utf8");
    const simJs = fs.readFileSync(path.join(WEB, "sim.js"), "utf8");
    const setSrc = fs.readFileSync(path.join(WEB, "settings.js"), "utf8");
    const noRouter = allPages.filter((f) => !/src="router\.js"/.test(fs.readFileSync(path.join(WEB, f), "utf8")));
    check("翻页: 六个页面都加载 router.js", noRouter.length === 0, noRouter.join(","));
    check("翻页: 没有任何页面还留内联脚本（插进 DOM 的 <script> 内联体不执行，模拟机就是黑屏）",
          allPages.every((f) => !/<script>\s*\n/.test(fs.readFileSync(path.join(WEB, f), "utf8"))),
          allPages.filter((f) => /<script>\s*\n/.test(fs.readFileSync(path.join(WEB, f), "utf8"))).join(","));
    check("翻页: 走 fetch + DOMParser，拿不到就退回硬跳转（file:// 双击必须还能用）",
          /await fetch\(file, \{ cache: CACHE_POLICY \}\)/.test(rSrc) &&
          /new DOMParser\(\)\.parseFromString\(html, "text\/html"\)/.test(rSrc) &&
          /canRoute = false;/.test(rSrc) && /location\.href = a\.href;/.test(rSrc));
    check("翻页: 只换 main 里除 .top 之外的孩子（导航栏常驻，主题/语言监听器不重来）",
          /if \(!n\.classList\.contains\("top"\)\) n\.remove\(\);/.test(rSrc) &&
          /if \(!n\.classList\.contains\("top"\)\) live\.append\(n\);/.test(rSrc));
    check("翻页: 首页 head 里那块代码框字体会跟着补进 head",
          /function ensureHeadAssets\(doc\)/.test(rSrc) && /ensureHeadAssets\(doc\);/.test(rSrc));
    check("翻页: head 附属按 href 去重（onload 会把 media 从 print 改成 all，按 outerHTML 比会越翻越多）",
          /x\.tagName === "LINK" && x\.href === n\.href/.test(rSrc));
    check("翻页: 每次换页都重新上词条 + 补链接 hash（否则新内容是源语言、链接丢状态）",
          /s\.applyLang\(document\.documentElement\.dataset\.lang\); s\.syncLinks\(\);/.test(rSrc) &&
          /ParlzSettings = \{ applyLang, syncLinks, hash \}/.test(setSrc));
    check("翻页: aria-current 跟着新页走（.top 是常驻的，不自己更新就永远亮在旧页）",
          /function markCurrent\(file\)/.test(rSrc) && /markCurrent\(file\);/.test(rSrc));
    check("翻页: URL 用 pushState 更新、back 用 popstate 复原（scrollRestoration 手动）",
          /history\.pushState\(\{ page: file, scroll: 0 \}/.test(rSrc) &&
          /addEventListener\("popstate"/.test(rSrc) &&
          /history\.scrollRestoration = "manual"/.test(rSrc) &&
          /history\.replaceState\(\{ page: pageFile\(\), scroll: Math\.round\(window\.scrollY\) \}/.test(rSrc));
    check("翻页: 缓存的是 HTML 文本，不是解析好的 Document（搬进真文档会把缓存抽干）",
          /const htmlCache = new Map\(\)/.test(rSrc) && /htmlCache\.set\(file, html\)/.test(rSrc) &&
          !/Cache\.set\(file, doc\)/.test(rSrc));
    check("翻页: 修饰键 / download / 新标签 / 外链一律不拦",
          /e\.metaKey \|\| e\.ctrlKey \|\| e\.shiftKey \|\| e\.altKey/.test(rSrc) &&
          /a\.hasAttribute\("download"\) \|\| a\.target/.test(rSrc) &&
          /url\.origin !== location\.origin/.test(rSrc));
    check("翻页: 每页独有的脚本按页补加载（模拟机要 system+term+sim）",
          /"sim\.html": \["system\.js", "term\.js", "sim\.js"\]/.test(rSrc) &&
          /"index\.html": \["codeshow\.js"\]/.test(rSrc) &&
          /async function ensureDeps\(file\)/.test(rSrc));
    const leave = Number((rSrc.match(/const LEAVE_MS = (\d+)/) || [])[1]);
    const enter = Number((rSrc.match(/const ENTER_MS = (\d+)/) || [])[1]);
    const out1 = css.match(/main\.pg-out \{ animation: pgOut ([\d.]+)s/);
    const in1 = css.match(/main\.pg-in \{ animation: pgIn ([\d.]+)s/);
    check("翻页: 过渡是真的（淡出 + 淡入两段 keyframes 都在），且时长与 router 里等的毫秒数一致",
          !!out1 && !!in1 && Math.round(parseFloat(out1[1]) * 1000) === leave &&
          Math.round(parseFloat(in1[1]) * 1000) === enter,
          "router " + leave + "/" + enter + "，CSS " + (out1 && out1[1]) + "/" + (in1 && in1[1]));
    check("翻页: 关了系统动效也只去掉位移/模糊，淡入淡出必须留着（否则又变生硬一跳）",
          /@media \(prefers-reduced-motion: reduce\) \{[\s\S]*?main\.pg-out \{ animation: pgFadeOut[\s\S]*?main\.pg-in \{ animation: pgFadeIn/.test(css));
    check("翻页: 部件表不在加载时自己跑（每个都是 (root) => 收尾）",
          /window\.ParlzWidgets = \[/.test(appSrc) && /pkgTable, gitClone\]/.test(appSrc) &&
          !/^\(\(\) => \{/m.test(appSrc) && /function pkgTable\(root\)/.test(appSrc));
    check("翻页: 包表换了页就不再往摘掉的节点上画（gone 闸 + 注销语言钩子）",
          /let gone = false;/.test(appSrc) && /const offHook = addLangHook\(paintNote\)/.test(appSrc) &&
          /return \(\) => \{ gone = true; offHook\(\); \};/.test(appSrc) &&
          /const addLangHook = \(fn\) => \{/.test(setSrc));
    check("翻页: 模拟机挪成 sim.js 并导出 mount（内联体翻进来不执行）",
          /root\.ParlzSim = \{ mount \}/.test(simJs) && /container\.querySelector\("#screen"\)/.test(simJs) &&
          /<script src="sim\.js"><\/script>/.test(simHtml));
    check("翻页: 离开模拟机必须 destroy 终端（document 上的按键钩子不摘就在别的页吃键盘）",
          /term\.destroy\(\);/.test(simJs) && /destroy\(\) \{/.test(termSrc) &&
          /document\.removeEventListener\("keydown", this\._onKey\)/.test(termSrc) &&
          /removeEventListener\("resize", this\._onResize\)/.test(termSrc));
    check("翻页: 终端销毁后 readKey 停住而不是空转（nano/less 收到 null 是 continue，会烧 CPU）",
          /if \(this\.dead\) return new Promise\(\(\) => \{\}\);/.test(termSrc) &&
          /if \(term\.dead\) return;/.test(simJs));
    check("翻页: 首页代码框的 .cb-hot 真的会被画上（原签名把 hot 传成了 cursor，闪光一直是死的）",
          /const draw = \(text, hot\) =>/.test(fs.readFileSync(path.join(WEB, "codeshow.js"), "utf8")));
  }

  /* ---------- 25. 自托管 Git 仓库那一页（git.html + git/ 目录） ---------- */
  {
    const gitHtml = fs.readFileSync(path.join(WEB, "git.html"), "utf8");
    const rSrc2 = fs.readFileSync(path.join(WEB, "router.js"), "utf8");
    const i18nSrc = fs.readFileSync(path.join(WEB, "i18n.js"), "utf8");
    const locales = [...i18nSrc.matchAll(/^  "([A-Za-z-]+)": \{$/gm)].map((m) => m[1]);
    check("git 页: 与其它页**同一套 CSS 和部件**（hero + 逐字标 + 浮现 + 共用表/块样式），没有专属皮肤",
          /class="hero"/.test(gitHtml) && /<h1 id="word">git<span class="cursor"/.test(gitHtml) &&
          /data-reveal/.test(gitHtml) && /class="lede"/.test(gitHtml) &&
          /<section data-reveal/.test(gitHtml) && /class="hint"/.test(gitHtml) &&
          /class="feedline"/.test(gitHtml) && !/gitpage/.test(gitHtml),
          "gitpage 残留: " + /gitpage/.test(gitHtml));
    check("样式: style.css 里不许有 .gitpage（用户明确要和其它界面一样，别再分一套皮肤）",
          !/\.gitpage/.test(css));
    check("样式: 逐行浮现只挂在 JS 填的 #pkgs 表上 —— 烘在 HTML 里的静态表（许可证/仓库）不能没人给 .on",
          /html\.js #pkgs tbody tr \{ opacity: 0/.test(css) && /#pkgs tbody tr\.on \{ opacity: 1/.test(css) &&
          !/html\.js tbody tr \{ opacity: 0/.test(css));
    check("git 页: 只加载共用的四个脚本（这页没有动画部件，不带上 system.js/term.js/codeshow.js）",
          /src="i18n\.js"/.test(gitHtml) && /src="settings\.js"/.test(gitHtml) &&
          /src="app\.js"/.test(gitHtml) && /src="router\.js"/.test(gitHtml) &&
          !/src="system\.js"/.test(gitHtml) && !/src="codeshow\.js"/.test(gitHtml));
    check("git 页: 两个克隆入口都列出来，且说明是同一份裸库（子域与 /git/ 同路径）",
          /id="clone-main">https:\/\/www\.parlz\.com\/git\/parlz\.git</.test(gitHtml) &&
          /id="clone-sub">https:\/\/git\.os\.parlz\.com\/parlz\.git</.test(gitHtml));
    check("git 页: 诚实交代「服务端没上线之前 clone 连不通」，不许把这行删成光鲜的地址列表",
          /真正上线之前，clone 是连不上的/.test(gitHtml) &&
          /<b>只读镜像<\/b>/.test(gitHtml));
    check("git 页: 发布方式是只读镜像 + 本机/SSH 推送，产物不进仓库",
          /git push --mirror ssh:\/\/jgzyes@git\.os\.parlz\.com\/srv\/git\/parlz\.git/.test(gitHtml) &&
          /产物（ISO \/ IMG \/ \.pm）不进仓库/.test(gitHtml));
    check("git 页: 复制按钮走部件（AJAX 翻进来也挂得上），复用 pkg.copy 不另开词条",
          /function gitClone\(root\)/.test(appSrc) &&
          /window\.ParlzWidgets = \[spot, reveal, word, ticker, feedLabels, pkgTable, gitClone\]/.test(appSrc) &&
          (gitHtml.match(/data-i18n="pkg\.copy"/g) || []).length === 2);
    check("翻页: router 认得第六页 git.html（页面类仍只管 simpage，逐个 toggle 不整串赋值）",
          /"git\.html"/.test(rSrc2) &&
          /classList\.toggle\("simpage"/.test(rSrc2) &&
          /body\.live/.test(rSrc2));
    const gitKeys = ["nav.git", "git.title", "git.desc", "git.lede", "git.repo.h2", "git.th.repo",
                     "git.th.what", "git.th.branch", "git.th.rel", "git.what", "git.clone.h2",
                     "git.status", "git.push.h2", "git.push.note", "git.up.h2", "git.up.note"];
    const short = gitKeys.filter((k) =>
      (i18nSrc.match(new RegExp('"' + k.replace(/\./g, "\\.") + '":', "g")) || []).length !== locales.length);
    check("语言: git 页那 16 条词条在全部 " + locales.length + " 张语言表里都齐（缺一门就是那门语言翻到这页变回中文）",
          locales.length === 11 && short.length === 0, "缺: " + short.join(","));
    const gitReadme = fs.readFileSync(path.join(__dirname, "..", "git", "README.md"), "utf8");
    check("仓库目录: git/README.md 写清了放什么（裸库）、两个地址、只读发布与产物不进仓库",
          /parlz\.git/.test(gitReadme) && /git\.os\.parlz\.com/.test(gitReadme) &&
          /只读/.test(gitReadme) && /不放产物|不放产物/.test(gitReadme));
  }

  console.log("\n" + (fail ? "FAILED" : "ALL PASS") + ": " + pass + " passed, " + fail + " failed");
  if (fail) { console.log("失败项：\n  " + failures.join("\n  ")); process.exit(1); }
})();
