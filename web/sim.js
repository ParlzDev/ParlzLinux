// web/sim.js —— 整页模拟机：终端就是这台机器的屏幕与控制台。
//
// 原来这段是 sim.html 里的内联脚本。翻页改成 AJAX（router.js）之后必须挪成文件：
// 插进 DOM 的 `<script>` 内联体是不执行的 —— 留着的话模拟机页永远黑屏。
// 导出 mount 形态：每次进这一页重新开机一次；离开时 router 调收尾函数走 term.destroy()，
// 把挂在 **document** 上的按键钩子摘干净，不然站在别的页也打不出字。
(function (root) {
"use strict";

async function boot(term) {
  const say = (s) => term.write(String(s).replace(/\n/g, "\r\n"));
  let sys = null, prog = false;

  // 输出：progress 占一行时，后续输出先换行（不然会叠在进度行上）
  const io = {
    out: (s) => { if (prog) { term.write("\r\n"); prog = false; } term.write(s); },
    err: (s) => { if (prog) { term.write("\r\n"); prog = false; } term.write(s); },
    progress: (s) => { term.write("\r\x1b[K" + s); prog = true; },
  };

  try {
    term.fit();
    if (root.ParlzI18n) root.ParlzI18n.apply(document.documentElement.dataset.lang);

    sys = root.ParlzSystem.create({ feedBase: "feed", tty: term });
    root.__sim = sys;                                   // 调试用

    // 开机：版本串从 /etc 读（默认系统里就有；dmesg/nano 那些是被裁的命令，
    // 装在 core.pm 里，装完才出现）
    term.write("\x1b[2J\x1b[H");
    const cat = (p) => sys.runCapture("cat " + p).then((s) => String(s).replace(/\r?\n$/, ""));
    say("[    0.000000] Parlz 0.1.0 on x86_64 (Linux 7.2.5 base)");
    say("[    0.120000] console [tty0] enabled · console [ttyS0] enabled");
    say("[    2.310000] virtio_blk virtio1: [vda] 1048576 512-byte logical blocks (537 MB/512 MiB)");
    say("[    2.840000]  vda: vda1 vda2");
    say("[    3.020000] mounted /dev/vda2 (ext4) on /mnt · pivot_root OK");
    say("[    3.400000] Parlz boot ready");
    say("");
    say("ParlzOS " + await cat("/etc/parlz-release").then((s) => (s.match(/^version: (.*)$/m) || [])[1] || ""));
    say("输入 help 看命令；nano / less / top / watch 是全屏程序。");
    say("");
  } catch (err) {
    // 启动失败也要看得见原因，而不是留一块黑屏
    say("启动失败: " + (err && err.message ? err.message : err));
    say(String(err && err.stack || "").split("\n").slice(1, 4).join("\n"));
    return;
  }

  let line = "", hist = [], hpos = 0;
  const promptStr = () => sys.prompt();
  const redraw = () => term.write("\r\x1b[K" + promptStr() + line);
  const newPrompt = () => term.write("\r\n" + promptStr());

  term.focus();                       // 进来就能直接敲（不用先点一下）
  newPrompt();
  for (;;) {
    const k = await term.readKey();
    if (term.dead) return;            // 翻页走了：按键流已断，别再空转
    if (k == null) continue;
    if (k === "\r") {
      term.write("\r\n");
      const cmd = line;
      line = "";
      if (cmd.trim()) { if (hist[hist.length - 1] !== cmd) hist.push(cmd); hpos = hist.length; }
      try { await sys.run(cmd, io); } catch (e) { term.write(String(e && e.message || e) + "\r\n"); }
      if (prog) { term.write("\r\n"); prog = false; }
      newPrompt();
      continue;
    }
    if (k === "\x7f" || k === "\b") { if (line.length) { line = line.slice(0, -1); term.write("\b \b"); } continue; }
    if (k === "\x03") { term.write("^C"); line = ""; newPrompt(); continue; }
    if (k === "\x0c") { term.write("\x1b[2J\x1b[H"); newPrompt(); term.write(line); continue; }
    if (k === "\x1b[A") { if (hpos > 0) { hpos--; line = hist[hpos] || ""; redraw(); } continue; }
    if (k === "\x1b[B") { if (hpos < hist.length - 1) { hpos++; line = hist[hpos]; } else { hpos = hist.length; line = ""; } redraw(); continue; }
    if (k === "\x1b[H") { line = ""; redraw(); continue; }
    if (k.length === 1 && k >= " ") { line += k; term.write(k); continue; }
    if (k === "\t") { line += "\t"; term.write("\t"); continue; }
  }
}

function mount(container) {
  const el = container.querySelector("#screen");
  if (!el || !root.ParlzTerm || !root.ParlzSystem) return null;

  // 终端在这里建、不在 boot 里建：boot 是 async，若在它跑到一半时翻页离开，
  // mount 拿不到 term 就没法 destroy —— 那个挂在 document 上的按键钩子会一直吃键盘。
  const term = new root.ParlzTerm.Terminal(el, {});
  boot(term);

  // 点过导航/下拉之后也把键盘交回终端
  const onClick = (e) => {
    const tag = e.target && e.target.tagName;
    if (tag !== "INPUT" && tag !== "SELECT" && tag !== "BUTTON" && tag !== "A") term.focus();
  };
  document.addEventListener("click", onClick);

  return () => {
    document.removeEventListener("click", onClick);
    term.destroy();
  };
}

root.ParlzSim = { mount };
})(typeof window !== "undefined" ? window : globalThis);
