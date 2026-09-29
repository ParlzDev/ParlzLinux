// parlz 官网脚本：页面部件（光斑 / 浮现 / 字标 / 跑马灯 / feed 文案 / 包表）。
//
// 站点既能用 http 服务打开，也能直接 file:// 双击打开：
//   · 取数/下载一律走**相对路径**（file:// 下 fetch 会被浏览器拦，下面有内嵌索引兜底）
//   · 主题与语言存 localStorage；同时写进 URL hash 并把**站内所有 .html 链接**都改成
//     带 hash 的 —— file:// 下有些浏览器禁 localStorage，hash 是跨页面同步的唯一通道
//   · 对外显示的 feed 地址固定成站点域名（guest 里 feeds.conf 抄的就是它）
//
// **翻页由 router.js 接管**（AJAX 取页 + 过渡动画），所以这里的部件一个都不许在脚本
// 加载时自己跑一遍：每个部件都是 `(root) => 收尾函数 | null`，进哪页由 router 拿那一页
// 的容器调一次，离开时调收尾函数。写回"加载即执行"的话，AJAX 翻进来的那一页就是死的。
const SITE = "https://www.parlz.com";                     // 站点正式地址
const FEED_REL = "feed";                                  // 相对，本地预览/双击都能用
const FEED_ABS = SITE + "/feed";                          // www.parlz.com/feed/*.pm
// 内嵌兜底索引（= 真 feed/Packages 的原样内容）：file:// 或服务没起时也能看到包
const FALLBACK = [
  { name: "core",  version: "0.1.0-rc", file: "core.pm",  size: 44484096 },
  { name: "pm",    version: "1.1-RC+1", file: "pm.pm",    size: 7578112 },
  { name: "gcc",   version: "13",       file: "gcc.pm",   size: 476792320 },
  { name: "clang", version: "18.1.3",   file: "clang.pm", size: 1013805056 },
];
const reduced = () => matchMedia("(prefers-reduced-motion: reduce)").matches;
// 取资源的缓存策略：本地预览要**每次回源**（改完文件看到旧内容会让人误判成 bug），
// 但线上用 no-cache 等于每翻一页都多一趟往返 —— 手机上正是"加载慢"的那一份。
// 所以只有 localhost / 127. 这一类本地地址才强制校验，其它走浏览器正常缓存。
const CACHE_POLICY = /^(localhost|127\.|\[::1\]|10\.0\.2\.)/.test(location.hostname) ? "no-cache" : "default";

/* ---------- 跟随指针的光斑（挂 window，整份文档只装一次） ---------- */
function spot() {
  if (reduced() || spot.on) return null;
  spot.on = true;
  let raf = 0, x = 50, y = 0;
  addEventListener("pointermove", (e) => {
    x = (e.clientX / innerWidth) * 100;
    y = (e.clientY / innerHeight) * 100;
    if (raf) return;
    raf = requestAnimationFrame(() => {
      document.body.style.setProperty("--mx", x + "%");
      document.body.style.setProperty("--my", y + "%");
      document.body.classList.add("live");
      raf = 0;
    });
  }, { passive: true });
  return null;
}

/* ---------- 进入视口浮现 ---------- */
function reveal(root) {
  const items = root.querySelectorAll("[data-reveal]");
  if (!items.length) return null;
  if (!("IntersectionObserver" in window)) { items.forEach((el) => el.classList.add("on")); return null; }
  const io = new IntersectionObserver((entries) => {
    for (const en of entries) {
      if (en.isIntersecting) { en.target.classList.add("on"); io.unobserve(en.target); }
    }
  }, { rootMargin: "-8% 0px -12% 0px" });
  items.forEach((el) => io.observe(el));
  return () => io.disconnect();
}

/* ---------- 字标逐字浮现 ---------- */
function word(root) {
  const h1 = root.querySelector("#word");
  if (!h1) return null;
  // 原文只在第一次取（拆成 span 之后 textContent 就只剩一个字母了）
  if (h1.__text === undefined) {
    let s = "";
    for (const n of [...h1.childNodes]) if (n.nodeType === 3) s += n.textContent;
    h1.__text = s;
  }
  if (!h1.__text) return null;
  h1.classList.remove("on");
  const cursor = h1.querySelector(".cursor");
  const frag = document.createDocumentFragment();
  [...h1.__text].forEach((ch, i) => {
    const s = document.createElement("span");
    s.className = "ch";
    s.textContent = ch;
    s.style.transitionDelay = (i * 70) + "ms";
    frag.append(s);
  });
  if (cursor) frag.append(cursor);
  h1.textContent = "";
  h1.append(frag);
  // 用 setTimeout 而不是 requestAnimationFrame 上"亮起来"的那个类：
  // 后台标签页里 rAF 可以永远不触发，那样标题就是透明的（终端那边踩过同一个坑）。
  setTimeout(() => h1.classList.add("on"), 50);
  return null;
}

/* ---------- 跑马灯 ---------- */
function ticker(root) {
  const box = root.querySelector("#tick");
  if (!box) return null;
  const items = ["LINUX 7.2.5", "PARLZ rc 0.1", "EL TORITO", "SYSLINUX",
                 "INITRAMFS", "PIVOT_ROOT", "PM+1.1-RC+1", "-F\\V",
                 "BUSYBOX", "STATIC LINKED"];
  const set = () => items.map((txt) => {
    const s = document.createElement("span");
    s.append(document.createTextNode("· "));
    const b = document.createElement("b");
    b.textContent = txt;
    s.append(b);
    return s;
  });
  box.textContent = "";
  box.append(...set(), ...set());   // 复制一份，slide -50% 时正好接上
  return null;
}

/* ---------- 复制按钮 ---------- */
function copyOnClick(btn, text, labelKey) {
  btn.addEventListener("click", async () => {
    const label = labelKey ? null : btn.textContent;
    try {
      await navigator.clipboard.writeText(text);
      btn.textContent = t("pkg.copied");
      btn.classList.add("copied");
    } catch { btn.textContent = t("pkg.manual"); }
    setTimeout(() => {
      btn.textContent = labelKey ? t(labelKey) : label;
      btn.classList.remove("copied");
    }, 1400);
  });
}
const FEED_SHOWN = FEED_ABS;   // 对外就这一个地址（本地预览取数仍走相对路径）

/* ---------- feed 地址文案 + 复制（三处 id 在同源页面上只会出现其一） ---------- */
function feedLabels(root) {
  for (const id of ["feed", "feedurl", "feedurl2"]) {
    const el = root.querySelector("#" + id);
    if (el) el.textContent = FEED_SHOWN;
  }
  const btn = root.querySelector("#copyfeed");
  if (btn) copyOnClick(btn, FEED_ABS, "pkg.copy");
  return null;
}

/* ---------- pm 索引（页面与终端共用一份缓存） ---------- */
const feedPkgs = fetch(FEED_REL + "/Packages", { cache: CACHE_POLICY })
  .then((res) => {
    if (!res.ok) throw new Error("HTTP " + res.status);
    return res.text();
  })
  .then((text) => {
    const rows = text.split("\n")
      .map((l) => l.trim())
      .filter((l) => l && !l.startsWith("#"))
      .map((l) => l.split(/\s+/))
      .map(([name, version, file, size]) =>
        ({ name, version, file, size: Number(size) }));
    if (!rows.length) throw new Error("索引为空");
    return rows;
  })
  .catch(() => {
    // file:// 双击打开（浏览器会拦 fetch）或服务没起：退回内嵌副本，包照样看得到
    return FALLBACK;
  });

const mib = (bytes) => {
  const n = Number(bytes);
  if (!Number.isFinite(n) || n <= 0) return "";
  return n < 1048576 ? (n / 1024).toFixed(1) + " KiB" : (n / 1048576).toFixed(1) + " MiB";
};

/* ---------- 包表行 ---------- */
function pkgRow(p, i) {
  const tr = document.createElement("tr");
  tr.style.transitionDelay = (i * 90) + "ms";

  const tdName = document.createElement("td");
  tdName.className = "name";
  const num = document.createElement("span");
  num.className = "num";
  num.textContent = String(i + 1).padStart(2, "0") + "  ";
  tdName.append(num, document.createTextNode(p.name));

  const tdVer = document.createElement("td");
  tdVer.className = "ver";
  tdVer.textContent = p.version || "—";

  const tdSize = document.createElement("td");
  tdSize.className = "size";
  tdSize.textContent = mib(p.size);

  const tdCmd = document.createElement("td");
  tdCmd.className = "cmd";
  const btn = document.createElement("button");
  btn.className = "copy";
  btn.type = "button";
  btn.textContent = "pm install " + p.name;
  copyOnClick(btn, "pm install " + p.name);
  const dl = document.createElement("a");
  dl.className = "u";
  dl.href = FEED_REL + "/" + p.file;
  dl.setAttribute("download", "");
  dl.textContent = "  ↓";
  dl.title = t("pkg.dltitle", { file: p.file });
  tdCmd.append(btn, dl);

  tr.append(tdName, tdVer, tdSize, tdCmd);
  return tr;
}

/* ---------- 包列表页 ---------- */
function pkgTable(root) {
  const stateEl = root.querySelector("#state"), tableEl = root.querySelector("#pkgs");
  if (!stateEl || !tableEl) return null;
  const tbody = tableEl.querySelector("tbody");
  const noteEl = root.querySelector("#note");
  let count = 0, bigNames = [];
  let gone = false;                       // 已经翻页走了：别再往摘下来的节点上画

  const paintNote = () => {
    if (gone || !noteEl || noteEl.hidden) return;
    noteEl.textContent = t("packages.note", { n: count }) + " " +
      (bigNames.length ? t("packages.note.big", { list: bigNames.join(" / ") }) : "");
  };
  const offHook = addLangHook(paintNote);

  feedPkgs.then((pkgs) => {
    if (gone) return;
    if (!pkgs.length) { stateEl.textContent = t("packages.state.empty"); return; }
    pkgs.forEach((p, i) => tbody.append(pkgRow(p, i)));
    tableEl.hidden = false;
    stateEl.remove();
    setTimeout(() => tbody.querySelectorAll("tr").forEach((tr) => tr.classList.add("on")), 50);

    count = pkgs.length;
    bigNames = pkgs.filter((p) => p.size > 50 * 1048576).map((p) => p.name);
    noteEl.hidden = false;
    paintNote();
  }).catch((err) => {
    if (gone) return;
    stateEl.className = "state bad";
    stateEl.textContent = t("packages.state.err", { url: FEED_REL + "/Packages", msg: err.message });
  });

  return () => { gone = true; offHook(); };
}

/* ---------- git 页：两个克隆地址一键复制 ----------
   文案复用 pkg.copy / pkg.copied / pkg.manual，不另开词条（地址本身是技术串，不进翻译） */
function gitClone(root) {
  for (const id of ["clone-main", "clone-sub"]) {
    const el = root.querySelector("#" + id);
    const btn = root.querySelector("#copy-" + id);
    if (el && btn) copyOnClick(btn, el.textContent.trim());
  }
  return null;
}

// 六个页面共用的部件表：进哪页都由 router 依次调用，容器里没有对应元素就自己跳过
window.ParlzWidgets = [spot, reveal, word, ticker, feedLabels, pkgTable, gitClone];

// <head> 的内联脚本用它判断 app.js 是否跑完：没跑完就把"等 JS"的状态撤掉，
// 页面至少是**完整可读**的，不会只剩半页
window.__parlzReady = true;
