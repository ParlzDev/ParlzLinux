// web/term.js —— 极简 ANSI 终端：给 sim.html 的整页模拟机用。
//
// 支持：光标定位/移动/保存恢复、清屏清行、SGR（颜色/粗体/反显）、滚动、
//       备用屏（全屏程序）、滚动回看、键盘 → 终端序列（方向键/Delete/PgUp…）。
// 不追求 VT100 全兼容：只做 nano / less / top 这类程序真正用到的那一子集。
(function (root) {
"use strict";

const C0 = { NUL: 0, BEL: 7, BS: 8, HT: 9, LF: 10, CR: 13, ESC: 27 };

function isDigit(c) { return c >= "0" && c <= "9"; }

class Terminal {
  constructor(el, opts) {
    opts = opts || {};
    this.el = el;
    this.rows = opts.rows || 24;
    this.cols = opts.cols || 80;
    this.maxScrollback = opts.scrollback == null ? 1000 : opts.scrollback;
    this.onBell = opts.onBell || null;
    this.onExitRequest = opts.onExitRequest || null;
    this.scrollback = [];        // 滚出屏幕的历史行（字符串）
    this.viewOffset = 0;         // 回看偏移（0 = 贴底）
    this.cur = { r: 0, c: 0, visible: true };
    this.saved = { r: 0, c: 0 };
    this.scrollTop = 0;
    this.scrollBot = this.rows - 1;
    this.alt = false;
    this.screen = this.blank();
    this.altScreen = null;
    this.attrs = { fg: null, bg: null, inv: false, bold: false, dim: false };
    this.pending = "";           // 未解析完的转义序列
    this.raf = 0;
    this.keyQueue = [];
    this.keyWaiters = [];
    this.cellW = 0;
    this.cellH = 0;
    this._bindKeys();
    this._bindResize();
  }

  blank() {
    const rows = [];
    for (let r = 0; r < this.rows; r++) rows.push(this.blankRow());
    return rows;
  }
  blankRow() { const row = []; for (let c = 0; c < this.cols; c++) row.push({ ch: " ", a: null }); return row; }

  /* ---------- 尺寸 ---------- */
  measure() {
    const probe = document.createElement("span");
    probe.textContent = "M".repeat(10);
    probe.style.cssText = "position:absolute;visibility:hidden;white-space:pre";
    this.el.append(probe);
    const box = probe.getBoundingClientRect();
    this.cellW = box.width / 10;
    this.cellH = box.height;
    probe.remove();
  }
  fit() {
    this.measure();
    if (!this.cellW || !this.cellH) return false;
    const box = this.el.getBoundingClientRect();
    const cols = Math.max(40, Math.floor(box.width / this.cellW));
    const rows = Math.max(8, Math.floor(box.height / this.cellH));
    if (cols === this.cols && rows === this.rows) return false;
    this.resize(rows, cols);
    return true;
  }
  resize(rows, cols) {
    const keep = this.screen.map((row) => row.map((cell) => cell));
    this.rows = rows; this.cols = cols;
    this.screen = this.blank();
    for (let r = 0; r < rows; r++)
      if (keep[r]) for (let c = 0; c < cols; c++) if (keep[r][c]) this.screen[r][c] = keep[r][c];
    this.cur.r = Math.min(this.cur.r, rows - 1);
    this.cur.c = Math.min(this.cur.c, cols - 1);
    this.scrollTop = 0; this.scrollBot = rows - 1;
    this.render(true);
  }
  size() { return { rows: this.rows, cols: this.cols }; }

  _bindResize() {
    let t = 0;
    this._onResize = () => { clearTimeout(t); t = setTimeout(() => this.fit(), 120); };
    addEventListener("resize", this._onResize);
    // 字体是**非阻塞**加载的（见各页 head 那条 media="print" 的 link）：fit() 先按系统
    // 等宽字体量好字符格，webfont 后到一 swap，行/列数就对不上了。所以字体到货就重量一次。
    const ff = document.fonts;
    if (!ff) return;
    this._onFonts = () => this.fit();
    if (ff.ready && typeof ff.ready.then === "function") ff.ready.then(this._onFonts).catch(() => {});
    if (typeof ff.addEventListener === "function") ff.addEventListener("loadingdone", this._onFonts);
  }

  /* ---------- 写入 ---------- */
  write(data) {
    const s = this.pending + String(data);
    this.pending = "";
    let i = 0;
    while (i < s.length) {
      const c = s[i];
      if (c === "\x1b") {
        const seq = s.slice(i);
        const m = /^\x1b\[([0-9;?]*)([A-Za-z@`~])/.exec(seq);
        if (m) { this.csi(m[1], m[2]); i += m[0].length; continue; }
        const m2 = /^\x1b([()][A-Za-z0-9])/.exec(seq);          // 字符集选择：忽略
        if (m2) { i += m2[0].length; continue; }
        const m3 = /^\x1b[=>78MDEHc]/.exec(seq);
        if (m3) {                             // =/> 键盘模式, 7/8 保存恢复, M 反向索引, D 索引, E 换行, H 制表, c 复位
          const t = m3[0][1];
          if (t === "7") this.saved = { r: this.cur.r, c: this.cur.c };
          else if (t === "8") { this.cur.r = this.saved.r; this.cur.c = this.saved.c; }
          else if (t === "M") this.reverseIndex();
          else if (t === "D") this.lineFeed();
          else if (t === "E") { this.cur.c = 0; this.lineFeed(); }
          i += m3[0].length;
          continue;
        }
        if (seq.length < 8) { this.pending = seq; return; }        // 可能是半截序列
        i += 1;                                                    // 不认识的：跳过 ESC
        continue;
      }
      if (c === "\n") { this.cur.c = 0; this.lineFeed(); i++; continue; }
      if (c === "\r") { this.cur.c = 0; i++; continue; }
      if (c === "\b") { if (this.cur.c > 0) this.cur.c--; i++; continue; }
      if (c === "\t") { this.cur.c = Math.min(this.cols - 1, (Math.floor(this.cur.c / 8) + 1) * 8); i++; continue; }
      if (c === "\x07") { if (this.onBell) this.onBell(); i++; continue; }
      if (c === "\x00") { i++; continue; }
      this.putChar(c);
      i++;
    }
    this.scheduleRender();
  }

  csi(params, final) {
    const p = params.split(";").map((x) => (x === "" ? null : parseInt(x, 10)));
    const n = (i, d) => (p[i] == null || isNaN(p[i]) ? d : p[i]);
    switch (final) {
      case "H": case "f": this.cur.r = Math.min(this.rows - 1, Math.max(0, n(0, 1) - 1));
                          this.cur.c = Math.min(this.cols - 1, Math.max(0, n(1, 1) - 1)); break;
      case "A": this.cur.r = Math.max(0, this.cur.r - n(0, 1)); break;
      case "B": this.cur.r = Math.min(this.rows - 1, this.cur.r + n(0, 1)); break;
      case "C": this.cur.c = Math.min(this.cols - 1, this.cur.c + n(0, 1)); break;
      case "D": this.cur.c = Math.max(0, this.cur.c - n(0, 1)); break;
      case "G": this.cur.c = Math.min(this.cols - 1, Math.max(0, n(0, 1) - 1)); break;
      case "d": this.cur.r = Math.min(this.rows - 1, Math.max(0, n(0, 1) - 1)); break;
      case "J": {
        const mode = n(0, 0);
        if (mode === 2 || mode === 3) this.screen = this.blank();
        else if (mode === 0) { for (let c = this.cur.c; c < this.cols; c++) this.screen[this.cur.r][c] = { ch: " ", a: null };
                               for (let r = this.cur.r + 1; r < this.rows; r++) this.screen[r] = this.blankRow(); }
        else if (mode === 1) { for (let r = 0; r < this.cur.r; r++) this.screen[r] = this.blankRow();
                               for (let c = 0; c <= this.cur.c; c++) this.screen[this.cur.r][c] = { ch: " ", a: null }; }
        break;
      }
      case "K": {
        const mode = n(0, 0);
        if (mode === 0) for (let c = this.cur.c; c < this.cols; c++) this.screen[this.cur.r][c] = { ch: " ", a: null };
        else if (mode === 1) for (let c = 0; c <= this.cur.c; c++) this.screen[this.cur.r][c] = { ch: " ", a: null };
        else if (mode === 2) this.screen[this.cur.r] = this.blankRow();
        break;
      }
      case "m": this.sgr(p); break;
      case "r": this.scrollTop = Math.max(0, n(0, 1) - 1); this.scrollBot = Math.min(this.rows - 1, n(1, this.rows) - 1); break;
      case "s": this.saved = { r: this.cur.r, c: this.cur.c }; break;
      case "u": this.cur.r = this.saved.r; this.cur.c = this.saved.c; break;
      case "h": if (params === "?1049") this.enterAlt(); else if (params === "?25") this.cur.visible = true; break;
      case "l": if (params === "?1049") this.exitAlt(); else if (params === "?25") this.cur.visible = false; break;
      default: break;
    }
    this.scheduleRender();
  }
  sgr(p) {
    if (!p.length || p[0] == null) { this.attrs = { fg: null, bg: null, inv: false, bold: false, dim: false }; return; }
    for (const v of p) {
      if (v === 0 || v == null) this.attrs = { fg: null, bg: null, inv: false, bold: false, dim: false };
      else if (v === 1) this.attrs.bold = true;
      else if (v === 2) this.attrs.dim = true;
      else if (v === 7) this.attrs.inv = true;
      else if (v === 22) { this.attrs.bold = false; this.attrs.dim = false; }
      else if (v === 27) this.attrs.inv = false;
      else if (v >= 30 && v <= 37) this.attrs.fg = v - 30;
      else if (v >= 90 && v <= 97) this.attrs.fg = v - 90 + 8;
      else if (v === 39) this.attrs.fg = null;
      else if (v >= 40 && v <= 47) this.attrs.bg = v - 40;
      else if (v >= 100 && v <= 107) this.attrs.bg = v - 100 + 8;
      else if (v === 49) this.attrs.bg = null;
    }
  }

  putChar(ch) {
    if (this.cur.c >= this.cols) { this.cur.c = 0; this.lineFeed(); }
    const a = (this.attrs.fg == null && this.attrs.bg == null && !this.attrs.inv && !this.attrs.bold && !this.attrs.dim)
      ? null : Object.assign({}, this.attrs);
    this.screen[this.cur.r][this.cur.c] = { ch, a };
    this.cur.c++;
  }
  lineFeed() {
    if (this.cur.r === this.scrollBot) this.scrollUp();
    else if (this.cur.r < this.rows - 1) this.cur.r++;
    this.scheduleRender();
  }
  reverseIndex() {
    if (this.cur.r === this.scrollTop) this.scrollDown();
    else if (this.cur.r > 0) this.cur.r--;
  }
  scrollUp() {                                   // 区域上滚一行
    const gone = this.screen.splice(this.scrollTop, 1)[0];
    this.screen.splice(this.scrollBot, 0, this.blankRow());
    if (this.scrollTop === 0 && !this.alt) {     // 只把整屏顶部滚出去的行记进回看
      this.scrollback.push(gone.map((x) => x.ch).join("").replace(/\s+$/, ""));
      if (this.scrollback.length > this.maxScrollback) this.scrollback.shift();
      if (this.viewOffset > 0) this.viewOffset++;
    }
  }
  scrollDown() {
    this.screen.splice(this.scrollBot, 1);
    this.screen.splice(this.scrollTop, 0, this.blankRow());
  }

  /* ---------- 备用屏（全屏程序）---------- */
  enterAlt() {
    if (this.alt) return;
    this.altScreen = this.screen;
    this.alt = true;
    this.screen = this.blank();
    this.cur = { r: 0, c: 0, visible: this.cur.visible };
    this.scrollTop = 0; this.scrollBot = this.rows - 1;
    this.scheduleRender();
  }
  exitAlt() {
    if (!this.alt) return;
    this.screen = this.altScreen || this.blank();
    this.alt = false;
    this.cur = { r: this.rows - 1, c: 0, visible: true };
    this.scrollTop = 0; this.scrollBot = this.rows - 1;
    this.scheduleRender();
  }

  /* ---------- 渲染 ---------- */
  scheduleRender() {
    if (this.raf) return;
    // 只用 setTimeout，不用 requestAnimationFrame：后台标签页、内嵌视图、
    // 无头环境里 rAF 可能永远不触发（document.hidden 也未必是 true），
    // 那样屏幕会一直空着 —— 终端不需要跟帧对齐，10ms 合并一次足够。
    this.raf = setTimeout(() => { this.raf = 0; this.render(); }, 10);
  }
  cls(a) {
    if (!a) return "";
    const out = [];
    if (a.inv) out.push("t-inv");
    if (a.bold) out.push("t-bold");
    if (a.dim) out.push("t-dim");
    if (a.fg != null) out.push("t-fg" + a.fg);
    if (a.bg != null) out.push("t-bg" + a.bg);
    return out.join(" ");
  }
  render(force) {
    const showScrollback = this.viewOffset > 0 && !this.alt;
    const lines = [];
    if (showScrollback) {                          // 回看模式：从 scrollback 里取几行铺满屏
      const top = Math.max(0, this.scrollback.length - this.rows - this.viewOffset);
      for (let i = 0; i < this.rows; i++) lines.push(this.escape(this.scrollback[top + i] || ""));
    } else {
      for (let r = 0; r < this.rows; r++) {
        const row = this.screen[r];
        let html = "", run = "", runCls = "";
        for (let c = 0; c < this.cols; c++) {
          const cell = row[c];
          const cls = this.cls(cell.a) + (this.cur.visible && r === this.cur.r && c === this.cur.c ? " t-cur" : "");
          if (cls !== runCls) {
            if (run) html += runCls ? '<span class="' + runCls + '">' + this.escape(run) + "</span>" : this.escape(run);
            run = ""; runCls = cls;
          }
          run += cell.ch;
        }
        if (run) html += runCls ? '<span class="' + runCls + '">' + this.escape(run) + "</span>" : this.escape(run);
        lines.push(html.replace(/ +$/, "") || "");
      }
    }
    this.el.innerHTML = lines.join("\n");
    // innerHTML 会把手机键盘那个隐藏输入框一起清掉（它是 el 的孩子）——
    // 不补回来的话 this.input 指向一个游离节点，focus() 什么都聚焦不到，手机上敲不了字
    if (this.input) this.el.append(this.input);
  }
  escape(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  /* ---------- 输入 ---------- */
  _bindKeys() {
    // 按键挂在 document 上（模拟机页面里终端就是全屏的；只挂在 #screen 上的话，
    // 页面加载后焦点在 body，用户敲键盘什么都不会发生）。导航/表单控件上放行。
    const onKey = (e) => {
      const t = e.target;
      const tag = t && t.tagName;
      // 自己的隐藏输入框要放行（手机上就靠它接字符）；导航/表单控件照旧跳过
      if (tag === "SELECT" || tag === "TEXTAREA" || tag === "BUTTON" || tag === "A") return;
      if (tag === "INPUT" && t !== this.input) return;
      const seq = Terminal.keyToSeq(e);
      if (seq == null) return;
      e.preventDefault();
      this.pushKey(seq);
    };
    this._onKey = onKey;
    document.addEventListener("keydown", onKey);
    this.el.addEventListener("mousedown", () => this.focus());
    this._bindMobileInput();
    this.el.addEventListener("paste", (e) => {
      e.preventDefault();
      const text = (e.clipboardData || window.clipboardData).getData("text");
      for (const ch of text.replace(/\r\n?/g, "\n")) this.pushKey(ch === "\n" ? "\r" : ch);
    });
    this.el.addEventListener("wheel", (e) => {
      if (this.alt) return;                        // 全屏程序里不滚回看
      if (!this.scrollback.length) return;
      this.viewOffset = Math.max(0, Math.min(this.scrollback.length, this.viewOffset + (e.deltaY > 0 ? -3 : 3)));
      this.render();
      e.preventDefault();
    }, { passive: false });
  }
  // 手机上的系统键盘不会给 document 发 keydown（只往聚焦的输入框里塞字符），
  // 所以挂一个隐藏输入框：点终端就聚焦它，输入事件里按增删转成终端按键。
  _bindMobileInput() {
    if (!this.el || this.el.querySelector(".term-in")) return;
    const inp = document.createElement("input");
    inp.className = "term-in";
    inp.type = "text";
    inp.setAttribute("autocapitalize", "off");
    inp.setAttribute("autocomplete", "off");
    inp.setAttribute("autocorrect", "off");
    inp.setAttribute("spellcheck", "false");
    inp.setAttribute("aria-hidden", "true");
    inp.tabIndex = -1;
    this.el.append(inp);
    this.input = inp;
    let prev = "";
    inp.addEventListener("input", () => {
      const v = inp.value;
      if (v.length >= prev.length) {
        const added = v.slice(prev.length);
        for (const ch of added) this.pushKey(ch === "\n" ? "\r" : ch);
      } else {
        for (let i = 0; i < prev.length - v.length; i++) this.pushKey("\x7f");   // 退格
      }
      prev = ""; inp.value = "";
    });
    inp.addEventListener("keydown", (e) => {
      if (e.key === "Enter") { this.pushKey("\r"); prev = ""; inp.value = ""; e.preventDefault(); }
    });
    // 点一下终端 = 聚焦隐藏输入框 → 手机键盘弹出
    this.el.addEventListener("touchstart", () => { try { inp.focus({ preventScroll: true }); } catch (e) { inp.focus(); } }, { passive: true });
    this.el.addEventListener("mousedown", () => { try { inp.focus({ preventScroll: true }); } catch (e) { inp.focus(); } });
  }

  pushKey(seq) {
    if (this.dead) return;
    this.viewOffset = 0;
    if (this.keyWaiters.length) this.keyWaiters.shift()(seq);
    else this.keyQueue.push(seq);
  }
  readKey(timeoutMs) {
    // 已经销毁（离开模拟机页）：让还在等的循环**停住**而不是空转。
    // nano/less 的主循环收到 null 是 `continue`，永久 null 就是死循环烧 CPU。
    if (this.dead) return new Promise(() => {});
    if (this.keyQueue.length) return Promise.resolve(this.keyQueue.shift());
    return new Promise((resolve) => {
      const timer = timeoutMs ? setTimeout(() => {
        const i = this.keyWaiters.indexOf(handler);
        if (i >= 0) this.keyWaiters.splice(i, 1);
        resolve(null);
      }, timeoutMs) : null;
      const handler = (seq) => { if (timer) clearTimeout(timer); resolve(seq); };
      this.keyWaiters.push(handler);
    });
  }
  focus() {
    // 优先聚焦隐藏输入框：手机要靠它弹系统键盘；桌面照样收得到按键（见 _bindKeys 的放行）
    // 节点没在文档里（刚被摘页 / 还没挂载）时 focus() 是空操作，得退回屏幕本身
    if (this.input && this.input.isConnected) {
      try { this.input.focus({ preventScroll: true }); return; } catch (e) { /* 退回元素焦点 */ }
    }
    if (this.el && this.el.isConnected) this.el.focus();
  }

  // 离开模拟机页（AJAX 翻页）时必须调它：按键是挂在 **document** 上的，
  // 不摘的话站在任何一页敲键盘都会被这个已经看不见的终端吃掉。
  destroy() {
    if (this.dead) return;
    this.dead = true;
    if (this.raf) { clearTimeout(this.raf); this.raf = 0; }
    document.removeEventListener("keydown", this._onKey);
    removeEventListener("resize", this._onResize);
    if (document.fonts && this._onFonts) {
      if (typeof document.fonts.removeEventListener === "function") document.fonts.removeEventListener("loadingdone", this._onFonts);
      this._onFonts = null;
    }
    this.keyQueue.length = 0;
    const waiters = this.keyWaiters.splice(0);      // 主循环收到 null 就 break
    waiters.forEach((f) => f(null));
    if (this.el) this.el.textContent = "";
  }

  static keyToSeq(e) {
    const k = e.key;
    if (k === "Enter") return "\r";
    if (k === "Backspace") return "\x7f";
    if (k === "Tab") return "\t";
    if (k === "Escape") return "\x1b";
    if (k === "ArrowUp") return "\x1b[A";
    if (k === "ArrowDown") return "\x1b[B";
    if (k === "ArrowRight") return "\x1b[C";
    if (k === "ArrowLeft") return "\x1b[D";
    if (k === "Home") return "\x1b[H";
    if (k === "End") return "\x1b[F";
    if (k === "PageUp") return "\x1b[5~";
    if (k === "PageDown") return "\x1b[6~";
    if (k === "Delete") return "\x1b[3~";
    if (k === "Insert") return "\x1b[2~";
    if (e.ctrlKey && k.length === 1) {
      const c = k.toLowerCase().charCodeAt(0);
      if (c >= 97 && c <= 122) return String.fromCharCode(c - 96);      // ^a..^z
      if (k === " ") return "\x00";
      if (k === "[") return "\x1b";
      if (k === "\\") return "\x1c";
      if (k === "]") return "\x1d";
    }
    if (k.length === 1 && !e.metaKey) return k;
    return null;
  }
}

root.ParlzTerm = { Terminal };
})(typeof window !== "undefined" ? window : globalThis);
