// web/settings.js —— 全站设置：明暗主题 + 语言，跨页面 / 跨标签同步。
//
// **每个页面都要加载它**（index / sim / packages / download / license）：
// 主题按钮和语言下拉的处理器就在这里。踩过的坑：sim.html 当初只加载了 system.js + term.js，
// 于是那一页的主题按钮和语言下拉是死的 —— 在 sim 上切语言没反应，翻回别的页面
// 看到的是没切过去的语言，看着就像"切换页面后语言都不一样"。
const LANGS = typeof PARLZ_LANGS === "undefined"
  ? ["en-US", "zh-CN", "zh-Hant", "pt", "fr", "es", "id", "hi", "bn", "ar", "ur"]
  : PARLZ_LANGS;
const i18n = typeof ParlzI18n === "undefined" ? null : ParlzI18n;
const t = (key, vars) => (i18n ? i18n.t(key, vars) : key);
const i18nApply = (lang) => {
  if (i18n) i18n.apply(lang);
  else document.documentElement.dataset.lang = lang;
};
const LANG_HOOKS = [];    // 换语言时要跟着重画的东西（各页面的模块自己注册）
// 注册/注销换语言回调。**返回注销函数**是因为翻页：AJAX 换页后旧内容已经摘掉，
// 钩子还留在数组里的话，下一次换语言就会往一堆游离节点上写字（永远删不掉的泄漏）。
const addLangHook = (fn) => {
  LANG_HOOKS.push(fn);
  return () => { const i = LANG_HOOKS.indexOf(fn); if (i >= 0) LANG_HOOKS.splice(i, 1); };
};

(() => {
  const r = document.documentElement;
  const themeBtn = document.getElementById("theme");
  const langSel = document.getElementById("lang");

  const hash = () => "#lang=" + r.dataset.lang + "&theme=" + r.dataset.theme;

  // 站内每个 .html 链接都带上当前状态。**不能**只改一次就完事：
  // 改过一次的链接会带 hash，再用 [href$=".html"] 就选不中了（旧代码就栽在这，
  // 第二次切换后链接留着上一个 hash，翻页把旧主题带过去 —— 看着就是"各页不同步"）
  function syncLinks() {
    const h = hash();
    document.querySelectorAll("a[href]").forEach((a) => {
      const raw = a.getAttribute("href");
      if (!raw || /^[a-z][a-z0-9+.-]*:/i.test(raw) || raw.charAt(0) === "#") return;
      const base = raw.split("#")[0];
      if (!/\.html?$/.test(base)) return;
      a.setAttribute("href", base + h);
    });
  }

  function persist() {
    try {
      localStorage.setItem("parlz-theme", r.dataset.theme);
      localStorage.setItem("parlz-lang", r.dataset.lang);
    } catch (e) { /* file:// 下有些浏览器禁 localStorage：链接 hash 已经兜住 */ }
    // Chrome 在 file:// 下 replaceState 会抛 SecurityError，忽略即可
    try { history.replaceState(null, "", location.pathname + location.search + hash()); } catch (e) { /* 忽略 */ }
  }

  function paintTheme() {
    const light = r.dataset.theme === "light";
    if (themeBtn) {
      themeBtn.textContent = t("nav.theme");
      themeBtn.title = t(light ? "theme.toDark" : "theme.toLight");
    }
  }

  function applyLang(lang) {
    i18nApply(lang);
    if (langSel) langSel.value = lang;
    paintTheme();
    LANG_HOOKS.forEach((f) => f());
  }

  // 首屏：<head> 的 boot.js 已定好 lang/theme，这里把它们铺进 DOM
  applyLang(r.dataset.lang);
  syncLinks();

  if (themeBtn) themeBtn.addEventListener("click", () => {
    r.dataset.theme = r.dataset.theme === "light" ? "dark" : "light";
    paintTheme();
    persist();
    syncLinks();
  });

  if (langSel) langSel.addEventListener("change", () => {
    applyLang(langSel.value);
    persist();
    syncLinks();
  });

  // 另一个标签页/另一个页面改了 → 这里立刻跟着变（localStorage 是站点级共享的）
  addEventListener("storage", (e) => {
    if (e.key === "parlz-theme" && (e.newValue === "light" || e.newValue === "dark")) {
      r.dataset.theme = e.newValue;
      paintTheme();
    } else if (e.key === "parlz-lang" && LANGS.indexOf(e.newValue) >= 0) {
      applyLang(e.newValue);
    }
  });
  // 兜底：有的环境（某些 file://、内嵌视图）根本不发 storage 事件 —— 定时对一眼
  setInterval(() => {
    let th = null, lg = null;
    try { th = localStorage.getItem("parlz-theme"); lg = localStorage.getItem("parlz-lang"); } catch (e) { return; }
    if ((th === "light" || th === "dark") && th !== r.dataset.theme) {
      r.dataset.theme = th;
      paintTheme();
      syncLinks();
    }
    if (LANGS.indexOf(lg) >= 0 && lg !== r.dataset.lang) { applyLang(lg); syncLinks(); }
  }, 1000);

  // 翻页（router.js）之后要用同一套逻辑给新内容铺语言、给新链接补状态 hash，
  // 别在 router 里另写一份 —— 那正是"各页语言不一样"这类问题的来源
  (typeof window !== "undefined" ? window : globalThis).ParlzSettings = { applyLang, syncLinks, hash };
})();
