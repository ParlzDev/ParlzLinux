// scripts/web-git-export.js —— 把 git/parlz.git 导成官网能静态浏览的三份数据。
//
// 产物（默认 web/git/，全站唯一需要"跟着包"的目录）。**数据都是 .js**（不是 .json）：
//   manifest.js     很小的入口：head / 分支 / 标签 / 提交数 / 文件数 / 分片表 / blobs.bin 的总长
//   t/<分片>.js     按**顶层条目**切的目录索引（dirs: { 路径: [ {n,t,m,s,b,o,l} ] }）
//   blobs.bin       所有文本文件按序拼起来的字节，条目里记 o=偏移、l=长度（这个只能 Range）
//   log.js          提交列表（h/a/e/d/s）
//   c/<短哈希>.js   单次提交的说明与改动清单（太大就截断并标注）
//
// 为什么要拼一个 blobs.bin：仓库有 9.6 万个文件，逐文件落进 web/ 再手工部署是不现实的；
// 合成一个文件后，**点开某个文件才用 Range 取它那一段**（几 KB），部署只多一个大文件与几个 JSON。
// file:// 双击时浏览器不能发 Range —— 那种情况下树/历史照看，正文预览给一句实话。
const { execFileSync, spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const GITD = process.env.GIT_DIR || path.join(ROOT, "git", "parlz.git");
const OUT = process.env.GIT_OUT || path.join(ROOT, "web", "git");
// 单文件超过这个字节数就不进正文库（**树里照样列**，只是不给逐行预览）。
// 256 KB 挡掉的是 drivers/gpu/drm/amd 那种几十 MB 的机器生成寄存器头（367 个）。
const MAX_TEXT = Number(process.env.GIT_MAX_TEXT || 256 * 1024);
// 想让正文库更小就按前缀排除（这些路径的文件仍在树里列出，只是没逐行预览），例如：
//   GIT_SKIP=linux-7.2.5/drivers/gpu,linux-7.2.5/Documentation sh scripts/web-git-export.sh
const SKIP = (process.env.GIT_SKIP || "").split(",").map((s) => s.trim()).filter(Boolean);
const MAX_DIFF_FILES = Number(process.env.GIT_MAX_DIFF_FILES || 2000);

const g = (args) => execFileSync("git", ["--git-dir=" + GITD, "-c", "core.quotepath=false", ...args],
  { maxBuffer: 1 << 28, encoding: "utf8" });

function die(msg) { console.error("web-git-export: " + msg); process.exit(1); }
if (!fs.existsSync(GITD)) die("没有仓库 " + GITD + "（先 sh scripts/git-init-repo.sh）");

// 数据一律出成 **JS**（`ParlzGitData["<相对路径>"] = {...}`），不是 .json：
// 浏览器在 file://（双击打开本站是明确要求）下会拦 `fetch`，但 `<script>` 注入照样能跑。
// 唯一的例外是逐文件正文 —— 那个必须靠 HTTP Range，见 blobs.bin 与 web/git.js 里的实话分支。
function dataFile(rel, obj) {
  const p = path.join(OUT, rel);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  fs.writeFileSync(p, "window.ParlzGitData=window.ParlzGitData||{};ParlzGitData[" +
    JSON.stringify(rel) + "]=" + JSON.stringify(obj) + ";\n");
}

const HEAD = g(["rev-parse", "HEAD"]).trim();
const BRANCH = g(["symbolic-ref", "--short", "HEAD"]).trim();
const SHORT = HEAD.slice(0, 12);
console.log("== 导出 git/parlz.git  HEAD " + SHORT + " (" + BRANCH + ")");

// ---------- 1. 整棵树（ls-tree -r -t --long：含目录条目，一次拿全） ----------
const rows = g(["ls-tree", "-r", "-t", "--long", "-z", HEAD]).split("\0").filter(Boolean).map((line) => {
  // "<mode> <type> <sha>\t<path>"（目录没有 size，git 给 "-"）
  const tab = line.indexOf("\t");
  const meta = line.slice(0, tab).split(" ");
  const p = line.slice(tab + 1);
  return { p, m: meta[0], t: meta[1], b: meta[2] === "-" ? "" : meta[2],
           s: meta[3] && meta[3] !== "-" ? Number(meta[3]) : 0 };
});
const files = rows.filter((r) => r.t === "blob");
const trees = rows.filter((r) => r.t === "tree");
console.log("   文件 " + files.length + " · 目录 " + trees.length);

// ---------- 2. 正文库：git cat-file --batch 一路读，二进制/超限跳过 ----------
const order = files.slice().sort((a, b) => (a.p < b.p ? -1 : 1));
const byPath = new Map();                 // path → { o, l }（进正文库的才有）
fs.mkdirSync(OUT, { recursive: true });
fs.mkdirSync(path.join(OUT, "t"), { recursive: true });
const binPath = path.join(OUT, "blobs.bin");
const w = fs.openSync(binPath, "w");
let off = 0, kept = 0, skippedBin = 0, skippedBig = 0, skippedSkip = 0;

const cat = spawn("git", ["--git-dir=" + GITD, "cat-file", "--batch"], { stdio: ["pipe", "pipe", "inherit"] });
const chunks = [];
let buf = Buffer.alloc(0), pending = [];   // 每个请求按顺序对应一个 blob

function looksText(b) {
  const n = Math.min(b.length, 8000);
  for (let i = 0; i < n; i++) if (b[i] === 0) return false;
  return true;
}
function consume() {
  // 头部形如 "<sha> blob <size>\n" 或 "miss <sha>\n"
  for (;;) {
    const nl = buf.indexOf(10);
    if (nl < 0) return;
    const head = buf.slice(0, nl).toString("utf8");
    const parts = head.split(" ");
    const size = Number(parts[parts.length - 1]);
    if (!Number.isFinite(size) || parts[0] === "miss") {
      if (parts[0] === "miss") { const req = pending.shift(); if (req) req.resolve(null); continue; }
      const need = nl + 1;
      if (buf.length < need + size + 1) return;
      const data = buf.subarray(need, need + size);
      buf = buf.subarray(need + size + 1);
      const req = pending.shift(); if (req) req.resolve(data);
      continue;
    }
    const need = nl + 1 + size + 1;
    if (buf.length < need) return;
    const data = buf.subarray(nl + 1, nl + 1 + size);
    buf = buf.subarray(need);
    const req = pending.shift(); if (req) req.resolve(data);
  }
}
cat.stdout.on("data", (d) => { buf = Buffer.concat([buf, d]); consume(); });
const reqs = [];
cat.stdout.on("end", () => reqs.forEach((r) => r()));
const closed = new Promise((res) => reqs.push(res));

function readBlob(sha) {
  return new Promise((resolve, reject) => {
    pending.push({ resolve, reject });
    cat.stdin.write(sha + "\n");
    setImmediate(consume);
  });
}

(async () => {
  for (const f of order) {
    if (SKIP.some((p) => f.p.startsWith(p))) { skippedSkip++; continue; }
    // 先看 ls-tree 给的大小：超限的直接跳过，**不要去读那 22 MB 的寄存器头**
    // （drivers/gpu/drm/amd 里那种 *_sh_mask.h 单个就几十 MB，读了既慢又一定用不上）
    if (f.s > MAX_TEXT) { skippedBig++; continue; }
    let data = null;
    try { data = await readBlob(f.b); } catch (e) { data = null; }
    if (!data) continue;
    if (!looksText(data)) { skippedBin++; continue; }
    if (data.length > MAX_TEXT) { skippedBig++; continue; }
    fs.writeSync(w, data);
    byPath.set(f.p, { o: off, l: data.length });
    off += data.length;
    kept++;
    if ((kept % 5000) === 0) console.log("   正文 " + kept + " 个 / " + (off / 1048576).toFixed(1) + " MiB");
  }
  fs.closeSync(w);
  cat.stdin.end();
  await closed;
  console.log("   进正文库 " + kept + " 个（跳过二进制 " + skippedBin + " · 超 " +
    Math.round(MAX_TEXT / 1024) + " KB 的 " + skippedBig + "）· blobs.bin " +
    (off / 1048576).toFixed(1) + " MiB");

  // ---------- 3. 按顶层条目切目录索引 ----------
  const entry = (r) => ({ n: r.p.split("/").pop(), t: r.t === "tree" ? "d" : r.m === "120000" ? "l" : "f",
                          m: r.m, s: r.s || undefined, b: r.b || undefined,
                          o: byPath.has(r.p) ? byPath.get(r.p).o : undefined,
                          l: byPath.has(r.p) ? byPath.get(r.p).l : undefined });
  const topOf = (p) => (p.indexOf("/") < 0 ? "" : p.slice(0, p.indexOf("/")));
  const tops = new Map();          // 顶层分片名 → { dirs: {路径: [条目]} }
  const put = (key, r) => {
    const dir = r.p.indexOf("/") < 0 ? "" : r.p.slice(0, r.p.lastIndexOf("/"));
    if (!tops.get(key).dirs[dir]) tops.get(key).dirs[dir] = [];
    tops.get(key).dirs[dir].push(entry(r));
  };
  const rootFiles = rows.filter((r) => topOf(r.p) === "" || (!r.p.includes("/") && r.t === "tree"));
  for (const r of rows) {
    const key = r.p.includes("/") ? r.p.slice(0, r.p.indexOf("/")) : "__root";
    if (!tops.has(key)) tops.set(key, { dirs: {} });
    put(key, r);
  }
  void rootFiles;
  const parts = [];
  for (const [key, val] of [...tops.entries()].sort((a, b) => a[0] < b[0] ? -1 : 1)) {
    for (const d of Object.keys(val.dirs)) val.dirs[d].sort((a, b) => (a.t === b.t ? a.n.localeCompare(b.n) : a.t === "d" ? -1 : 1));
    const file = key === "__root" ? "root.js" : encodeURIComponent(key) + ".js";
    dataFile("t/" + file, val.dirs);
    parts.push({ k: key, f: file, dirs: Object.keys(val.dirs).length });
  }
  console.log("   索引分片 " + parts.length + " 个");

  // ---------- 4. 提交历史 ----------
  const logRaw = g(["log", "--pretty=format:%H\x01%an\x01%ae\x01%aI\x01%s", "-z", HEAD]);
  const log = logRaw.split("\0").filter(Boolean).map((rec) => {
    const [h, a, e, d, s] = rec.split("\x01");
    return { h, a, e, d, s };
  });
  dataFile("log.js", log);
  console.log("   提交 " + log.length + " 条");

  // ---------- 5. 单次提交详情（改动清单 + 截断规则） ----------
  fs.mkdirSync(path.join(OUT, "c"), { recursive: true });
  const dirOf = (p) => (p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : "");
  const topFor = (p) => (p.includes("/") ? p.slice(0, p.indexOf("/")) : "__root");
  for (const c of log) {
    const sh = c.h.slice(0, 12);
    const names = (() => {
      try {
        return g(["diff-tree", "-r", "--no-commit-id", "--root", "-z", "--name-status", c.h])
          .split("\0").filter(Boolean);
      } catch (e) { return []; }
    })();
    const changes = [];
    // `-z` 是用 NUL 分隔**字段**的：`A\0path\0`、重命名是 `R100\0旧路径\0新路径\0`。
    // 不能按 "A\tpath" 一条记录切（第一版那么写，匹配不上就一直 i-- —— 直接死循环）。
    for (let i = 0; i < names.length; i++) {
      const kind = names[i][0];
      if (!/[RAMCDKTU]/.test(kind)) continue;
      if (kind === "R" || kind === "C") {
        const from = names[i + 1];
        const to = names[i + 2];
        if (to === undefined) break;
        changes.push({ p: to, k: kind, from });
        i += 2;
      } else {
        const p = names[i + 1];
        if (p === undefined) break;
        changes.push({ p, k: kind });
        i += 1;
      }
    }
    const truncated = changes.length > MAX_DIFF_FILES;
    const stat = { total: changes.length, added: 0, deleted: 0, modified: 0 };
    changes.forEach((x) => { if (x.k === "A") stat.added++; else if (x.k === "D") stat.deleted++; else stat.modified++; });
    const out = {
      h: c.h, s: sh, a: c.a, e: c.e, d: c.d,
      msg: (() => { try { return g(["log", "-1", "--pretty=%B", c.h]); } catch (e) { return c.s; } })().trim(),
      stat, truncated,
      // 首次提交就是整棵树入库：全量列出来既没意义也大得离谱，给按顶层目录的汇总
      files: truncated ? [] : changes.slice(0, MAX_DIFF_FILES),
      byTop: truncated ? (() => {
        const agg = new Map();
        changes.forEach((x) => { const k = topFor(x.p); agg.set(k, (agg.get(k) || 0) + 1); });
        return [...agg.entries()].sort((a, b) => b[1] - a[1]).map(([k, n]) => ({ k, n }));
      })() : undefined,
    };
    dataFile("c/" + sh + ".js", out);
  }
  console.log("   提交详情 " + log.length + " 份（> " + MAX_DIFF_FILES + " 个改动就只给汇总）");

  // ---------- 6. manifest ----------
  const tags = g(["for-each-ref", "--format=%(refname:lstrip=2)\x01%(objectname:short)\x01%(*authordate:iso)", "refs/tags"])
    .split("\n").filter(Boolean).map((l) => { const [n, h, d] = l.split("\x01"); return { n, h, d }; });
  dataFile("manifest.js", {
    repo: "parlz.git", head: HEAD, short: SHORT, branch: BRANCH,
    commits: log.length, files: files.length, dirs: trees.length,
    tags,
    blobsKept: kept, blobsBytes: off, blobFile: "blobs.bin",
    maxText: MAX_TEXT, skip: SKIP, skipped: { binary: skippedBin, big: skippedBig, byPath: skippedSkip },
    parts: parts.map((p) => ({ k: p.k, f: p.f, dirs: p.dirs })),
    about: (() => { try { return fs.readFileSync(path.join(GITD, "description"), "utf8").trim(); } catch (e) { return ""; } })(),
    generated: new Date().toISOString(),
  });
  console.log("== 写好了 " + OUT + "（manifest + " + parts.length + " 个索引分片 + blobs.bin " +
    (off / 1048576).toFixed(1) + " MiB）");
})().catch((e) => die(e && e.stack || e));
