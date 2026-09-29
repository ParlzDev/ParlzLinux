// 首页的代码展示：Hello World 的四种写法（Python / C / Go / Rust），
// 一个字母一个字母敲出来 → 停 0.5 秒 → 再一个字母一个字母删掉 → 换下一种，循环。
// 只有这块代码用 zeoseven 的英文字体（@import 在 index.html 的 head 里，
// 由 router.js 在翻页时补进 head）。
//
// 翻页是 AJAX 的，所以这里导出 `ParlzCodeShow.mount(root)`：
// 进首页由 router 调一次，离开时调它返回的收尾函数 —— 不收尾的话，
// 那条 setTimeout 链会一直在后台跑，往已经摘掉的节点里写字。
(function (root) {
"use strict";

const SNIPPETS = [
  {
    file: "hello.py", lang: "Python", tag: "python3 hello.py",
    code: [
      "#!/usr/bin/env python3",
      "# hello.py - the first program on ParlzOS",
      "",
      "def main():",
      '    print("Hello, World from ParlzOS!")',
      "",
      'if __name__ == "__main__":',
      "    main()",
    ],
  },
  {
    file: "hello.c", lang: "C", tag: "gcc hello.c -o hello",
    code: [
      "/* hello.c - the first program on ParlzOS */",
      "#include <stdio.h>",
      "",
      "int main(void) {",
      '    printf("Hello, World from ParlzOS!\\n");',
      "    return 0;",
      "}",
    ],
  },
  {
    file: "hello.go", lang: "Go", tag: "go run hello.go",
    code: [
      "// hello.go - the first program on ParlzOS",
      "package main",
      "",
      'import "fmt"',
      "",
      "func main() {",
      '    fmt.Println("Hello, World from ParlzOS!")',
      "}",
    ],
  },
  {
    file: "hello.rs", lang: "Rust", tag: "rustc hello.rs",
    code: [
      "// hello.rs - the first program on ParlzOS",
      "fn main() {",
      '    println!("Hello, World from ParlzOS!");',
      "}",
    ],
  },
];

const TYPE_MS = 26;      // 敲字速度（每字符毫秒）
const TYPE_NL = 90;      // 换行时多停一下
const HOLD_MS = 500;     // 敲完停 0.5 秒
const ERASE_MS = 12;     // 删除速度（比敲得快，像按住退格）
const GAP_MS = 200;      // 删干净到下一次开敲之间

const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
// 极简高亮：注释 / 字符串 / 关键字
const TOKENS = /(#[^\n]*|\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\])*")|\b(def|return|if|elif|else|for|while|import|from|as|package|func|fn|let|mut|use|struct|impl|int|void|printf|include|stdio|fmt|Println|println|main)\b/g;

function highlight(text) {
  let out = "", last = 0, m;
  TOKENS.lastIndex = 0;
  while ((m = TOKENS.exec(text))) {
    out += esc(text.slice(last, m.index));
    const cls = m[1] ? "c" : m[2] ? "s" : "k";
    out += '<span class="' + cls + '">' + esc(m[0]) + "</span>";
    last = m.index + m[0].length;
  }
  return out + esc(text.slice(last));
}

function mount(container) {
  const box = container.querySelector("#code");
  const head = container.querySelector("#codehead");
  if (!box) return null;
  // 注意：**不要**看 prefers-reduced-motion —— 系统关了"动态效果"时那是 reduce，
  // 之前一走降级分支就变成静态显示，看着像"没有动画"。这块就是要一直敲。
  let idx = 0, alive = true;

  const wrap = box.parentElement;                    // .codebox（特效挂在它上面）
  const phase = (name) => {
    if (!wrap || !wrap.classList) return;
    wrap.classList.toggle("typing", name === "typing");
    wrap.classList.toggle("erasing", name === "erasing");
  };
  // 光标一直在；hot=true 时把最后那个字单独包一层，让它闪一下（CSS 的 .cb-hot）
  const draw = (text, hot) => {
    const body = (hot && text)
      ? highlight(text.slice(0, -1)) + '<span class="cb-hot">' + esc(text.slice(-1)) + "</span>"
      : highlight(text);
    box.innerHTML = body + '<span class="cb-cur" aria-hidden="true"></span>';
    box.scrollTop = box.scrollHeight;
  };
  const later = (fn, ms) => { setTimeout(() => { if (alive) fn(); }, ms); };

  function play() {
    const snip = SNIPPETS[idx];
    if (head) head.textContent = snip.file + "  ·  " + snip.lang + "  ·  " + snip.tag;
    const text = snip.code.join("\n") + "\n";
    const next = () => { idx = (idx + 1) % SNIPPETS.length; later(play, GAP_MS); };

    // 一、一个字母一个字母敲出来（.typing 让边框发亮、标题那个点跟着脉动）
    let i = 0;
    phase("typing");
    (function type() {
      i += 1;
      draw(text.slice(0, i), true);
      if (i >= text.length) { later(erase, HOLD_MS); return; }   // 敲完停 0.5 秒
      later(type, text[i - 1] === "\n" ? TYPE_NL : TYPE_MS);
    })();

    // 二、再一个字母一个字母删掉
    function erase() {
      let n = text.length;
      phase("erasing");
      (function back() {
        n -= 1;
        draw(text.slice(0, n), true);
        if (n <= 0) { next(); return; }
        later(back, ERASE_MS);
      })();
    }
  }
  play();

  return () => {
    alive = false;                     // 链条上每一个 setTimeout 回来都先问一句
    phase(null);
  };
}

root.ParlzCodeShow = { mount, snippets: SNIPPETS };
})(typeof window !== "undefined" ? window : globalThis);
