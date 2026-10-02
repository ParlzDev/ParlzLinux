// web/router.js —— 站内翻页改成 AJAX：取新页 → 淡出 → 换内容 → 淡入，不再整页硬跳。
//
// 为什么能这么切：五个页面的 `<div class="top">`（品牌 + 导航 + 语言 + 主题）是**逐字
// 一致**的，所以导航栏是常驻的 —— 换掉的只有 `<main>` 里除 `.top` 之外的孩子。
// 于是主题按钮/语言下拉上挂着的监听器一直有效，settings.js 不用重来，
// 翻页后语言/主题自然不会"各页不一样"（那正是以前每次硬跳都重新初始化一遍的老毛病）。
//
// 三条纪律：
//  1. **只搬 main 的孩子**，head 里那一页特有的东西（首页代码框的 @import <style>）单独补，
//     否则翻回首页时那块字体退回等宽，看着像"没加载全"。
//  2. **离开一页必须调它的收尾函数**：首页的打字动画是一条 setTimeout 链、模拟机的按键
//     钩子挂在 document 上 —— 不收尾就是"在别的页也听得到这边敲键盘"。
//  3. fetch 拿不到就退回**硬跳转**（file:// 双击打开时浏览器拦 fetch，Firefox 尤其严）。
//     这条兜底比动画重要：宁可像以前那样跳，也不能点了没反应。
(function (root) {
"use strict";

const FILES = ["index.html", "packages.html", "download.html", "license.html", "sim.html", "git.html"];
// 每一页额外要的脚本（各页 HTML 里已经带了一份，这里管的是 AJAX 翻进来的那次）
const DEPS = {
  "index.html": ["codeshow.js"],
  "sim.html": ["system.js", "term.js", "sim.js"],
  "git.html": ["git.js"],
};
const LEAVE_MS = 220;      // 淡出时长，和 style.css 里 pgOut 对齐（改一边要改另一边）
const ENTER_MS = 360;      // 淡入时长，同上（pgIn）

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const mainEl = () => document.querySelector("main");
const pageFile = () => {
  const f = (location.pathname.split("/").pop() || "").replace(/\.html?$/, "");
  return FILES.indexOf(f + ".html") >= 0 ? f + ".html" : "index.html";
};

// 缓存的是**原始 HTML 文本**，不是解析好的 Document：节点一旦搬进真文档，
// 缓存里那份就被抽干了，第二次翻到同一页就是空白。
const htmlCache = new Map();
let canRoute = typeof fetch === "function" && typeof DOMParser === "function";
let busy = false;
let teardown = null;
let currentFile = pageFile();     // 真文档里现在是哪一页（同页只换 query 时靠它让路）

/* ---------- 顶部的细进度条：取页/加载脚本期间给一点"正在动"的反馈 ---------- */
function bar(state) {
  let el = document.getElementById("pgbar");
  if (!el) {
    el = root.document.createElement("div");
    el.id = "pgbar";
    el.setAttribute("aria-hidden", "true");
    document.body.append(el);
  }
  if (state === "start") { el.classList.add("on"); el.classList.remove("done"); return; }
  if (state === "done") {
    el.classList.add("on", "done");
    setTimeout(() => el.classList.remove("on"), 200);        // 冲到 100% 再整体淡出
    setTimeout(() => el.classList.remove("done"), 520);
    return;
  }
  el.classList.remove("on");
  setTimeout(() => el.classList.remove("done"), 320);
}

/* ---------- 脚本 / head 附属 ---------- */
function hasScript(src) {
  const abs = new URL(src, document.baseURI).href;
  return [...document.querySelectorAll("script[src]")].some((s) => s.src === abs);
}
function loadScript(src) {
  return new Promise((resolve) => {
    const el = document.createElement("script");
    el.src = src;
    el.onload = () => resolve(true);
    el.onerror = () => resolve(false);
    document.head.append(el);
  });
}
async function ensureDeps(file) {
  const list = DEPS[file] || [];
  for (const src of list) if (!hasScript(src) && !(await loadScript(src))) return false;
  return true;
}
// 首页 head 里那块只给代码框用的 @import <style>：翻页时得跟着补进来。
// **去重按 href 比，不能按 outerHTML** —— 那条 onload 会把 media 从 print 改成 all，
// 一比不相等就会每翻一次页多塞一份 47 KB 的字体表（实测翻两趟就攒了三份）。
function ensureHeadAssets(doc) {
  const have = () => [...document.head.querySelectorAll("style, link[rel=stylesheet]")];
  for (const n of doc.head.querySelectorAll("style, link[rel=stylesheet]")) {
    const dup = n.tagName === "LINK"
      ? have().some((x) => x.tagName === "LINK" && x.href === n.href)
      : have().some((x) => x.tagName === "STYLE" && x.outerHTML === n.outerHTML);
    if (!dup) document.head.insertAdjacentHTML("beforeend", n.outerHTML);
  }
}

/* ---------- 部件装配：进一页就把这页的元素挂起来，返回"离开时的收尾" ---------- */
const PAGE_MODULE = { "sim.html": "ParlzSim", "index.html": "ParlzCodeShow", "git.html": "ParlzGit" };
function mountAll(main, file) {
  const stops = [];
  const run = (fn, arg) => {
    try { const stop = fn(arg); if (typeof stop === "function") stops.push(stop); }
    catch (e) { console.warn("parlz router: 部件装配失败", e); }
  };
  (root.ParlzWidgets || []).forEach((w) => run(w, main));
  const name = PAGE_MODULE[file];
  const mod = name ? root[name] : null;
  if (mod && typeof mod.mount === "function") run(mod.mount, main);
  return () => { stops.forEach((s) => { try { s(); } catch (e) { /* 收尾失败不拦翻页 */ } }); };
}

/* ---------- 换内容 ---------- */
function swap(doc, file) {
  const live = mainEl(), src = doc.querySelector("main");
  if (!live || !src) return false;
  ensureHeadAssets(doc);          // 首页代码框那块 @import 字体在 head 里，不补就退回等宽

  if (teardown) { teardown(); teardown = null; }
  [...live.children].forEach((n) => { if (!n.classList.contains("top")) n.remove(); });
  [...src.children].forEach((n) => {
    if (!n.classList.contains("top")) live.append(n);      // 跨文档 append 会自动 adopt
  });

  const body = doc.body;
  // 只跟着源页开/关 .simpage（模拟机那一页的整屏布局），别整串赋值 ——
  // body.live 是指针光斑加的，抹掉它光斑就没了
  document.body.classList.toggle("simpage", !!(body && body.classList.contains("simpage")));
  document.body.dataset.page = (body && body.dataset.page) || file.replace(/\.html?$/, "");
  const title = doc.querySelector("title");
  if (title) document.title = title.textContent;
  const md = doc.querySelector('meta[name="description"]');
  const liveMd = document.querySelector('meta[name="description"]');
  if (md && liveMd) liveMd.setAttribute("content", md.getAttribute("content"));

  // 新内容先上词条、再补状态 hash，最后才装配部件 —— 顺序反了会先闪一下源语言，
  // 而部件读到的文案也还是旧的
  const s = root.ParlzSettings;
  if (s) { s.applyLang(document.documentElement.dataset.lang); s.syncLinks(); }
  markCurrent(file);
  currentFile = file;
  teardown = mountAll(live, file);
  return true;
}

function markCurrent(file) {
  document.querySelectorAll(".top .nav a[href]").forEach((a) => {
    const base = (a.getAttribute("href") || "").split("#")[0].split("/").pop();
    if (base === file) a.setAttribute("aria-current", "page");
    else a.removeAttribute("aria-current");
  });
}

/* ---------- 取一页（失败就说不清可路由，交给调用方硬跳） ---------- */
async function getDoc(file) {
  try {
    let html = htmlCache.get(file);
    if (html == null) {
      const res = await fetch(file, { cache: CACHE_POLICY });
      if (!res.ok) throw new Error("HTTP " + res.status);
      html = await res.text();
      htmlCache.set(file, html);
    }
    const doc = new DOMParser().parseFromString(html, "text/html");
    if (!doc.querySelector("main")) throw new Error(file + " 没有 <main>");
    if (!(await ensureDeps(file))) throw new Error(file + " 的脚本没加载上");
    return doc;
  } catch (e) {
    canRoute = false;          // file:// 拦 fetch / 没起服务：这一站就退回普通跳转
    console.warn("parlz router: 退回整页跳转 ——", e.message);
    return null;
  }
}

/* ---------- 一次翻页：淡出 → 换 → 淡入 ---------- */
async function render(file, doc, scroll) {
  const live = mainEl();
  live.classList.remove("pg-in");
  live.classList.add("pg-out");
  await wait(LEAVE_MS);
  if (!swap(doc, file)) { live.classList.remove("pg-out"); bar("off"); return false; }
  if (scroll) window.scrollTo(0, scroll);
  else { try { window.scrollTo({ top: 0, behavior: "instant" }); } catch (e) { window.scrollTo(0, 0); } }
  void live.offsetWidth;                 // 强制一次重排：同一个 main 连续两次加同名 class 才不会不重放
  live.classList.remove("pg-out");
  live.classList.add("pg-in");
  bar("done");
  setTimeout(() => live.classList.remove("pg-in"), ENTER_MS);
  return true;
}

async function goTo(file, opts) {
  opts = opts || {};
  if (busy || !canRoute) return false;
  busy = true;
  bar("start");
  try {
    const doc = await getDoc(file);
    if (!doc) { bar("off"); return false; }
    if (opts.push !== false) {
      // 先把"这一页滚到哪儿了"记进当前历史条目：scrollRestoration 设成了 manual，
      // 浏览器不管这事儿，back 回来不自己记就永远落在页顶
      try { history.replaceState({ page: pageFile(), scroll: Math.round(window.scrollY) }, ""); } catch (e) { /* 忽略 */ }
    }
    if (!(await render(file, doc, opts.scroll))) return false;
    if (opts.push !== false) {
      const hash = root.ParlzSettings ? root.ParlzSettings.hash() : location.hash;
      // Chrome 在 file:// 下 pushState 会抛 SecurityError：URL 不动，翻页照样能用
      try { history.pushState({ page: file, scroll: 0 }, "", file + hash); } catch (e) { /* 忽略 */ }
    }
    return true;
  } finally {
    busy = false;
  }
}

/* ---------- 链接拦截 ---------- */
function routable(a) {
  if (!canRoute || !a || !a.getAttribute) return "";
  const href = a.getAttribute("href");
  if (!href || a.hasAttribute("download") || a.target) return "";
  let url;
  try { url = new URL(a.href, document.baseURI); } catch (e) { return ""; }
  if (url.origin !== location.origin) return "";
  const file = (url.pathname.split("/").pop() || "");
  if (FILES.indexOf(file) < 0) return "";
  if (url.pathname === location.pathname) return "";      // 同一页：让浏览器只管 hash
  return file;
}

document.addEventListener("click", (e) => {
  if (e.defaultPrevented || e.button !== 0) return;
  if (e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;   // 新标签/后台标签照旧走浏览器
  const a = e.target && e.target.closest ? e.target.closest("a[href]") : null;
  const file = routable(a);
  if (!file) return;
  e.preventDefault();
  goTo(file, { push: true }).then((ok) => { if (!ok) location.href = a.href; });
}, false);

addEventListener("popstate", () => {
  const file = pageFile();
  if (!canRoute) return;
  // 同一页只是 query 变了（仓库浏览器自己的 ?r=tree:userland 这类视图切换）：
  // 不该重渲染整页，发个事件让那一页自己回去读路由
  if (file === currentFile) { dispatchEvent(new CustomEvent("parlz:route")); return; }
  goTo(file, { push: false, scroll: (history.state && history.state.scroll) || 0 })
    .then((ok) => { if (!ok) location.reload(); });
});

/* ---------- 起手：把当前这一页的部件装配起来（和 AJAX 翻页同一条路） ---------- */
try { history.scrollRestoration = "manual"; } catch (e) { /* 忽略 */ }
markCurrent(pageFile());
if (mainEl()) teardown = mountAll(mainEl(), pageFile());

root.ParlzRouter = { goTo, get canRoute() { return canRoute; }, files: FILES };
})(typeof window !== "undefined" ? window : globalThis);
