// web/git.js —— 官网上的仓库浏览器：about / summary / refs / log / tree / commit / diff / stats
// 八个视图，照 git.kernel.org 那一排（那排词本身是 cgit 的固定术语，按用户要求原样保留英文）。
//
// 数据来源：scripts/web-git-export.sh 从 git/parlz.git 导出的静态文件，全在 web/git/ 下 ——
//   manifest.js · t/<顶层分片>.js（目录索引，按顶层条目切，进哪块才拉哪块）·
//   blobs.bin（所有文本按路径序拼成的单文件）· log.js · c/<短哈希>.js
//
// 三条要紧的规矩：
//  1) **正文按 Range 取**。blobs.bin 是一百多 MB 的一个文件，点开某个文件只取它那一段；
//     服务端要是没回 206 就**立刻 cancel 响应体**，不然点一下就把整包拖走了。
//     file:// 双击（浏览器拦 Range）时树与历史照看，正文给一句实话。
//  2) **文件内容只当文本插**：全程 textContent，绝不把仓库里的字节交给 innerHTML。
//  3) **路由放 query**（?r=tree:userland），不放 hash —— hash 那个位置是 settings.js 的
//     `#lang=xx&theme=yy`，抢同一个槽会互相盖掉。同页换 query 由这页自己接管，
//     router.js 见 pathname 没变就只发 parlz:route 事件、不重渲染。
(function (root) {
"use strict";

const DATA = "git";
const VIEWS = ["about", "summary", "refs", "log", "tree", "commit", "diff", "stats"];
const MAX_LINES = 3000;          // 一屏最多渲染这么多行，剩下的给"取全文"
const PAGE = 60;                 // log 一页多少条

const parts = new Map();         // 分片文件名 → Promise(对象)
const data = new Map();          // 相对路径 → Promise(对象)
let man = null, host = null, live = true, seq = 0;
let ctx = { path: "", sha: "" };

const tt = (k, v) => (typeof root.ParlzI18n !== "undefined" ? root.ParlzI18n.t(k, v) : k);
// 数据全部走 **`<script>` 注入**，不走 fetch：本站要求能双击 file:// 打开，
// 而 file:// 下浏览器一律拦 fetch（Firefox 直接抛，Chrome 也是），一拦这页就"读不到导出数据"。
// <script> 的相对路径在 file:// 下照样能加载；只有逐文件正文那次 Range 请求例外（那必须 http）。
function loadData(rel) {
  if (data.has(rel)) return data.get(rel);
  const p = new Promise((resolve, reject) => {
    const s = document.createElement("script");
    s.src = DATA + "/" + rel;
    s.async = true;
    s.onload = () => {
      const v = root.ParlzGitData && root.ParlzGitData[rel];
      if (v === undefined) reject(new Error(rel + " 加载了但没给出数据"));
      else resolve(v);
    };
    s.onerror = () => reject(new Error("读不到 " + DATA + "/" + rel +
      " —— 没部署，或还没跑 sh scripts/web-git-export.sh 生成"));
    document.head.append(s);
  });
  data.set(rel, p);
  p.catch(() => { data.delete(rel); });      // 失败的别缓存，下次还能试
  return p;
}
function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined && text !== null) n.textContent = String(text);
  return n;
}
const mib = (b) => (b == null || isNaN(b) ? "" :
  b < 1024 ? b + " B" : b < 1048576 ? (b / 1024).toFixed(1) + " KiB" : (b / 1048576).toFixed(1) + " MiB");

async function dirsOfTop(top) {
  const part = man.parts.find((x) => x.k === top);
  if (!part) return {};
  if (!parts.has(part.f)) parts.set(part.f, loadData("t/" + part.f));
  return parts.get(part.f);
}
// 一个目录的子项：[{n,t,m,s,b,o,l}]，t = d 目录 / l 软链 / f 文件
async function listing(path) {
  const top = path === "" ? "__root" : path.split("/")[0];
  const dirs = await dirsOfTop(top);
  return (dirs[path] || []).map((e) => ({ ...e, p: path === "" ? e.n : path + "/" + e.n }));
}
function entryOf(path) {
  const dir = path.indexOf("/") < 0 ? "" : path.slice(0, path.lastIndexOf("/"));
  const name = path.split("/").pop();
  return listing(dir).then((rows) => rows.find((r) => r.n === name) || null);
}

/* ---------- 路由 ---------- */
const fileOf = () => (location.pathname.split("/").pop() || "git.html");
function parseRoute() {
  const r = new URLSearchParams(location.search).get("r") || "summary";
  const i = r.indexOf(":");
  return i < 0 ? { v: r, a: "" } : { v: r.slice(0, i), a: r.slice(i + 1) };
}
// 路径里可能有空格、& 甚至 #，所以 query 参数一律 encodeURIComponent；
// 读回来时在 render() 里 decodeURIComponent（只有 tree/file 两条路需要）。
const hrefOf = (v, a) => fileOf() + "?r=" + (a ? encodeURIComponent(v + ":" + a) : encodeURIComponent(v));
function go(v, a) {
  try { history.pushState({ cg: v + ":" + (a || "") }, "", hrefOf(v, a)); }
  catch (e) { /* file:// 下 Chrome 抛 SecurityError：视图照样切，只是地址不动 */ }
  render();
}
function link(v, a, label, cls) {
  const x = el("a", cls || "u", label);
  x.href = hrefOf(v, a);
  x.dataset.cg = a ? v + ":" + a : v;
  return x;
}

/* ---------- 外壳：那一排视图词 ---------- */
function shell(cur) {
  const box = el("div", "cgit-bar");
  VIEWS.forEach((v, i) => {
    const target = { about: ["about", ""], summary: ["summary", ""], refs: ["refs", ""],
                     log: ["log", ""], tree: ["tree", ctx.path], commit: ["commit", ctx.sha],
                     diff: ["diff", ctx.sha], stats: ["stats", ""] }[v];
    const on = (v === "tree" && (cur === "tree" || cur === "file")) ||
               (v === "commit" && (cur === "commit" || cur === "diff")) || v === cur;
    const a = link(target[0], target[1], v, on ? "cg-on" : "");
    if (on) a.setAttribute("aria-current", "true");
    box.append(a);
    if (i < VIEWS.length - 1) box.append(document.createTextNode(" "));
  });
  return box;
}
function crumbs(path, tail) {
  const c = el("div", "cg-crumb");
  c.append(link("tree", "", man.repo));
  const seg = path ? path.split("/") : [];
  for (let i = 0; i < seg.length; i++) {
    c.append(document.createTextNode(" / "));
    c.append(link("tree", seg.slice(0, i + 1).join("/"), seg[i]));
  }
  if (tail) { c.append(document.createTextNode(" / ")); c.append(el("span", "", tail)); }
  return c;
}
function row(cells) {
  const tr = el("tr");
  cells.forEach(([cls, node]) => { const td = el("td", cls); td.append(node); tr.append(td); });
  return tr;
}

/* ---------- tree ---------- */
async function viewTree(path) {
  ctx.path = path;
  const box = shell("tree");
  box.append(crumbs(path));
  const rows = await listing(path);
  rows.sort((a, b) => (a.t === b.t ? a.n.localeCompare(b.n) : a.t === "d" ? -1 : 1));
  const table = el("table"), tb = el("tbody");
  if (path !== "") {
    const up = path.indexOf("/") < 0 ? "" : path.slice(0, path.lastIndexOf("/"));
    tb.append(row([["name", link("tree", up, "..")], ["size", el("span")], ["ver", el("span")], ["cmd", el("span")]]));
  }
  for (const e of rows) {
    const what = e.t === "d" ? "tree" : e.t === "l" ? "link" : (e.o == null ? "bin" : "text");
    tb.append(row([
      ["name", link(e.t === "d" ? "tree" : "file", e.p, e.n + (e.t === "l" ? " →" : ""))],
      ["size", el("span", "", e.t === "d" ? "" : mib(e.s))],
      ["ver", el("span", "", e.t === "d" ? "" : what + " " + e.m)],
      ["cmd", el("span", "", e.b ? e.b.slice(0, 10) : "")],
    ]));
  }
  table.append(tb);
  box.append(table);
  box.append(el("p", "hint", `${man.files} files · ${man.dirs} directories · ${man.commits} commits`));
  return box;
}

/* ---------- file ---------- */
async function viewFile(path) {
  ctx.path = path;
  const box = shell("file");
  box.append(crumbs(path.indexOf("/") < 0 ? "" : path.slice(0, path.lastIndexOf("/")), path.split("/").pop()));
  const e = await entryOf(path);
  box.append(el("h2", "", path.split("/").pop()));
  if (!e) { box.append(el("p", "state bad", "not in the index")); return box; }
  box.append(el("p", "hint", mib(e.s) + " · mode " + e.m + (e.b ? " · blob " + e.b.slice(0, 12) : "")));
  if (e.t === "l" || e.o == null) {
    box.append(el("p", "hint", tt("git.ui.noblob", { n: Math.round((man.maxText || 0) / 1024), size: mib(e.s) })));
    return box;
  }
  let text = null;
  try {
    const res = await fetch(DATA + "/" + man.blobFile, {
      cache: typeof CACHE_POLICY !== "undefined" ? CACHE_POLICY : "default",
      headers: { Range: "bytes=" + e.o + "-" + (e.o + e.l - 1) },
    });
    if (res.status === 206) text = await res.text();
    else { if (res.body && res.body.cancel) res.body.cancel(); }      // 没回 206 绝不把整包读进来
  } catch (err) { text = null; }
  if (text == null) {
    box.append(el("p", "state bad", tt("git.ui.norange")));
    const how = el("pre", "cg-msg");
    how.textContent = "cd web && python3 -m http.server 8000     # 然后开 http://127.0.0.1:8000/git.html";
    box.append(how);
    return box;
  }
  const lines = text.split("\n");
  if (lines.length && lines[lines.length - 1] === "") lines.pop();
  const shown = Math.min(lines.length, MAX_LINES);
  const wrap = el("div", "cg-blob");
  for (let i = 0; i < shown; i++) {
    const r = el("div", "cg-l");
    r.append(el("span", "cg-n", String(i + 1)));
    r.append(el("span", "cg-t", lines[i]));
    wrap.append(r);
  }
  box.append(wrap);
  box.append(el("p", "hint", tt("git.ui.lines", { n: lines.length, size: mib(e.l) }) +
    (lines.length > shown ? " · " + tt("git.ui.more", { n: shown }) : "")));
  return box;
}

/* ---------- log ---------- */
async function viewLog(off) {
  const log = await loadData("log.js");
  const start = Number(off) || 0;
  ctx.sha = (log[start] || log[0] || {}).h ? log[start].h.slice(0, 12) : ctx.sha;
  const box = shell("log");
  box.append(crumbs(""));
  const table = el("table"), tb = el("tbody");
  for (const c of log.slice(start, start + PAGE)) {
    tb.append(row([
      ["ver", el("span", "", (c.d || "").slice(0, 10))],
      ["name", row0(link("commit", c.h.slice(0, 12), c.s))],
      ["size", el("span", "", c.a)],
      ["cmd", el("span", "", c.h.slice(0, 8))],
    ]));
  }
  table.append(tb);
  box.append(table);
  const tail = el("p", "hint", `${log.length} commits`);
  if (log.length > start + PAGE) tail.append(document.createTextNode(" · "), link("log", String(start + PAGE), "next »"));
  box.append(tail);
  return box;
}
function row0(node) { const f = document.createDocumentFragment(); f.append(node); return f; }

/* ---------- commit / diff ---------- */
async function viewCommit(sha, wantDiff) {
  const c = await loadData("c/" + (sha || man.short) + ".js");
  ctx.sha = c.s;
  const box = shell(wantDiff ? "diff" : "commit");
  box.append(crumbs(""));
  box.append(el("h2", "", c.s));
  const meta = el("p", "hint");
  meta.append("author" + " " + c.a + " <" + c.e + "> · " + (c.d || "").slice(0, 19) + " · ");
  meta.append(link("commit", c.s, c.s));
  meta.append(document.createTextNode(" · "));
  meta.append(link(wantDiff ? "commit" : "diff", c.s, wantDiff ? "log view" : "diff view"));
  box.append(meta);
  const pre = el("pre", "cg-msg");
  pre.textContent = c.msg || c.s;
  box.append(pre);
  box.append(el("p", "hint", `${c.stat.added} added · ${c.stat.deleted} deleted · ${c.stat.modified} modified · ${c.stat.total} files changed`));
  if (c.truncated) {
    box.append(el("p", "state", tt("git.ui.initial", { n: c.stat.total })));
    const t2 = el("table"), b2 = el("tbody");
    for (const x of (c.byTop || [])) b2.append(row([["name", el("span", "", x.k)], ["size", el("span", "", x.n)]]));
    t2.append(b2);
    box.append(t2);
    return box;
  }
  const table = el("table"), tb = el("tbody");
  for (const f of (c.files || []).slice(0, 500)) {
    tb.append(row([["ver", el("span", "", f.k)],
                   ["name", row0(link(wantDiff ? "diff" : "file", f.p, f.p))]]));
  }
  table.append(tb);
  box.append(table);
  if ((c.files || []).length > 500) box.append(el("p", "hint", "… " + (c.files.length - 500)));
  return box;
}

/* ---------- about / refs / summary / stats ---------- */
async function viewAbout() {
  const box = shell("about");
  const head = el("h2", "", man.repo);
  box.append(head);
  const p = el("pre", "cg-msg");
  p.textContent = man.about || "(no repository description)";
  box.append(p);
  const dl = el("dl");
  const pair = (k, v) => { dl.append(el("dt", "", k)); dl.append(el("dd", "", String(v))); };
  pair("head", man.branch + " → " + man.short);
  pair("commits", man.commits);
  pair("files", man.files + " (" + man.dirs + " " + "dirs" + ")");
  pair("text", man.blobsKept + " · " + mib(man.blobsBytes));
  pair("last export", (man.generated || "").slice(0, 10));
  box.append(dl);
  box.append(el("p", "hint", "clone addresses are in the section below"));
  return box;
}
async function viewRefs() {
  const log = await loadData("log.js");
  const box = shell("refs");
  const tb = el("tbody");
  const tr = el("tr");
  tr.append(el("td", "name", "head"));
  const td = el("td");
  td.append(document.createTextNode(man.branch + " → "), link("tree", "@" + man.short, man.short));
  tr.append(td);
  tr.append(el("td", "ver", (log[0] && (log[0].d || "").slice(0, 10)) || ""));
  tb.append(tr);
  for (const tg of man.tags || []) {
    const t2 = el("tr");
    t2.append(el("td", "name", "tag"));
    const td2 = el("td");
    td2.append(document.createTextNode(tg.n + " → "), link("commit", tg.h.slice(0, 12), tg.h.slice(0, 8)));
    t2.append(td2);
    t2.append(el("td", "ver", tg.d || ""));
    tb.append(t2);
  }
  const table = el("table");
  table.append(tb);
  box.append(table);
  return box;
}
async function viewSummary() {
  const log = await loadData("log.js");
  const last = log[0] || {};
  ctx.sha = last.h ? last.h.slice(0, 12) : man.short;
  const box = shell("summary");
  box.append(crumbs(""));
  const dl = el("dl");
  const pair = (k, v) => { dl.append(el("dt", "", k)); const dd = el("dd"); dd.append(v); dl.append(dd); };
  pair("age", (last.d || "").slice(0, 19));
  pair("commit", link("commit", ctx.sha, man.short));
  pair("author", last.a + " <" + last.e + ">");
  pair("message", last.s || "");
  pair("refs", "1 head (" + man.branch + ") · " + ((man.tags || []).length) + " tags");
  pair("files", man.files);
  pair("text", man.blobsKept + " · " + mib(man.blobsBytes));
  box.append(dl);
  const tb = el("tbody");
  for (const c of log.slice(0, 15)) {
    const tr = el("tr");
    tr.append(el("td", "ver", (c.d || "").slice(0, 10)));
    const td = el("td", "name");
    td.append(link("commit", c.h.slice(0, 12), c.s));
    tr.append(td);
    tr.append(el("td", "size", c.a));
    tb.append(tr);
  }
  const t = el("table");
  t.append(tb);
  box.append(t);
  box.append(el("p", "hint", `${log.length} commits` + " · "), link("log", "", "all log"));
  return box;
}
async function viewStats() {
  const log = await loadData("log.js");
  const byAuthor = new Map(), byDay = new Map();
  for (const c of log) {
    byAuthor.set(c.a, (byAuthor.get(c.a) || 0) + 1);
    const d = (c.d || "").slice(0, 10);
    if (d) byDay.set(d, (byDay.get(d) || 0) + 1);
  }
  const box = shell("stats");
  const mk = (title, map, cap) => {
    box.append(el("h2", "", title));
    const tb = el("tbody");
    const list = [...map.entries()].sort((x, y) => y[1] - x[1]).slice(0, cap);
    const max = list.length ? list[0][1] : 1;
    for (const [k, n] of list) {
      const tr = el("tr");
      tr.append(el("td", "name", k));
      tr.append(el("td", "size", n));
      const bar = el("td", "ver");
      const i = el("span", "cg-bar");
      i.style.width = Math.max(4, Math.round((n / max) * 120)) + "px";
      bar.append(i);
      tr.append(bar);
      tb.append(tr);
    }
    const t = el("table");
    t.append(tb);
    box.append(t);
  };
  mk("authors", byAuthor, 20);
  mk("dates", byDay, 30);
  box.append(el("p", "hint", `${man.commits} commits in the log`));
  return box;
}

/* ---------- 渲染 ---------- */
async function render() {
  if (!host || !live) return;
  const { v, a } = parseRoute();
  const mine = ++seq;
  let node;
  try {
    if (!man) throw new Error(tt("git.ui.nodata"));
    // URLSearchParams.get() 已经解过一次百分号编码，这里**不能再 decode**
    // （路径里真有个 % 的话，再解一次会 URIError 抛出来）
    if (v === "tree") node = await viewTree(a);
    else if (v === "file") node = await viewFile(a);
    else if (v === "log") node = await viewLog(a);
    else if (v === "commit") node = await viewCommit(a || man.short, false);
    else if (v === "diff") node = await viewCommit(a || man.short, true);
    else if (v === "about") node = await viewAbout();
    else if (v === "refs") node = await viewRefs();
    else if (v === "stats") node = await viewStats();
    else node = await viewSummary();
  } catch (e) {
    node = shell(v);
    node.append(el("p", "state bad", tt("git.ui.nodata") + "（" + (e && e.message ? e.message : e) + "）"));
    node.append(el("p", "hint", tt("git.ui.howto")));
  }
  if (!live || mine !== seq || !host) return;
  host.textContent = "";
  host.append(node);
  host.append(el("p", "cg-made", "last export" + " " +
    (man && man.generated ? man.generated.slice(0, 10) : "—") +
    (man ? " · " + man.repo + " @ " + (man.short || "—") : "")));
  if (root.ParlzI18n) root.ParlzI18n.apply(document.documentElement.dataset.lang);
}

function mount(container) {
  const box = container.querySelector("#cgit");
  if (!box) return null;
  live = true;
  host = box;
  const onClick = (e) => {
    const a = e.target && e.target.closest ? e.target.closest("a[data-cg]") : null;
    if (!a) return;
    e.preventDefault();
    const val = a.dataset.cg;
    const i = val.indexOf(":");
    go(i < 0 ? val : val.slice(0, i), i < 0 ? "" : val.slice(i + 1));
  };
  box.addEventListener("click", onClick);
  const onRoute = () => { render(); };
  root.addEventListener("parlz:route", onRoute);
  (async () => {
    try { man = await loadData("manifest.js"); }
    catch (e) { man = null; }
    render();
  })();
  return () => {
    live = false;
    host = null;
    box.removeEventListener("click", onClick);
    root.removeEventListener("parlz:route", onRoute);
  };
}

root.ParlzGit = { mount, views: VIEWS };
})(typeof window !== "undefined" ? window : globalThis);
