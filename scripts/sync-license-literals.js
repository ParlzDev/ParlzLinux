#!/usr/bin/env node
// sync-license-literals.js - 把仓库里的授权文本**写进** web/system.js 的两个字面量:
//   const LICENSES = [...]              // /usr/share/licenses 的条目(名字+字节数)
//   const MEDIA_LICENSE = [...]         // 介质副本说明的全文(盘上 /LICENSE.TXT)
//   const LICENSES_README_SIZE / MEDIA_LICENSE_SIZE
//
// 为什么要脚本写而不是手抄: 手抄一定漂。实测漂过两次 —— bash 的 GPLv3 字节数
// 差 2、改了介质说明后演示里还是旧文本, 判据一片红(红的是判据过期, 不是产品)。
// web-demo-test.js 里有逐条比对的判据兜底, 这个脚本是让它不需要人去修数字。
//
//   node scripts/sync-license-literals.js          # 需要改就改
//   node scripts/sync-license-literals.js --check  # 只检查是否一致(不一致退出码 1)
const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const SYS = path.join(ROOT, "web", "system.js");
const LIC = path.join(ROOT, "third_party", "licenses");
const CHECK = process.argv.includes("--check");

// 条目顺序 = 交付盘里 ls 看到的顺序(组件名字典序), parlz 自己那份放最后
const COMPS = ["linux-kernel", "busybox", "syslinux", "bash", "nano", "wget",
               "glibc", "openssl", "curl", "miniz"];
const rows = [];
for (const c of COMPS) {
  for (const f of fs.readdirSync(path.join(LIC, c)).sort()) {
    rows.push([c, f, fs.statSync(path.join(LIC, c, f)).size]);
  }
}
rows.push(["parlz", "LICENSE", fs.statSync(path.join(ROOT, "LICENSE")).size]);
rows.push(["parlz", "PARLZ.LICENSE", fs.statSync(path.join(ROOT, "PARLZ.LICENSE")).size]);
rows.push(["parlz", "LICENSES.md", fs.statSync(path.join(ROOT, "LICENSES.md")).size]);
rows.sort((a, b) => (a[0] === b[0] ? a[1].localeCompare(b[1]) : a[0].localeCompare(b[0])));

const media = fs.readFileSync(path.join(LIC, "PARLZ-MEDIA-LICENSE.txt"), "utf8").replace(/\n$/, "");
const readmeSize = fs.statSync(path.join(LIC, "README")).size;
const mediaSize = Buffer.byteLength(fs.readFileSync(path.join(LIC, "PARLZ-MEDIA-LICENSE.txt")), "utf8");

const wantLic = "const LICENSES = [\n" +
  rows.map((r) => `  ["${r[0]}", "${r[1]}", ${r[2]}],`).join("\n") + "\n];";
const wantMedia = "const MEDIA_LICENSE = [\n" +
  media.split("\n").map((l) => "  " + JSON.stringify(l) + ",").join("\n") +
  '\n  "",\n].join("\\n");';

let src = fs.readFileSync(SYS, "utf8");
const blocks = [
  [/const LICENSES = \[[\s\S]*?\];/, wantLic],
  [/const MEDIA_LICENSE = \[[\s\S]*?\]\.join\("\\n"\);/, wantMedia],
  [/const LICENSES_README_SIZE = \d+;/, `const LICENSES_README_SIZE = ${readmeSize};`],
  [/const MEDIA_LICENSE_SIZE = \d+;/, `const MEDIA_LICENSE_SIZE = ${mediaSize};`],
];
let changed = 0, missing = 0;
for (const [re, want] of blocks) {
  if (!re.test(src)) { console.log("!! 在 web/system.js 里找不到匹配: " + re.source.slice(0, 40)); missing++; continue; }
  const got = src.match(re)[0];
  if (got !== want) { changed++; if (!CHECK) src = src.replace(re, () => want); }
}
if (!CHECK) { if (changed) { fs.writeFileSync(SYS, src, "utf8"); } }
const bad = changed || missing;
console.log(bad === 0 ? `web/system.js 的授权字面量已与仓库真文件一致(${rows.length} 条 + 介质说明 ${mediaSize} 字节)`
                      : (CHECK ? `不一致: 需改 ${changed} 处, 缺失 ${missing} 处` : `已改写 ${changed} 处(条目 ${rows.length})`));
process.exit(bad ? 1 : 0);
