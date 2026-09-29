// parlz 官网首屏引导：在正文之前定好 主题 / 语言 / 书写方向。
//
// 放在 <head> 里**同步**执行（阻塞渲染），所以主题不会闪；
// 换语言前的正文由 i18n-wait 挡住（见 style.css），换上再放出来。
// 取值优先级：URL hash > localStorage > 浏览器语言 / 系统配色 > 默认。
(function () {
  var r = document.documentElement;
  r.className = "js";
  var LOCALES = ["en-US", "zh-CN", "zh-Hant", "pt", "fr", "es", "id", "hi", "bn", "ar", "ur"];
  var RTL = ["ar", "ur"];                      // 这两种从右往左排
  var q = new URLSearchParams(location.hash.replace(/^#/, ""));
  function pick(v, ok) { return ok.indexOf(v) >= 0 ? v : ""; }

  function fromBrowser() {
    var n = (navigator.language || "en").toLowerCase();
    // 中文要分简繁：zh-TW / zh-HK / zh-MO / zh-Hant* 走繁体，其余 zh* 走简体
    if (n.indexOf("zh") === 0) {
      return (n.indexOf("hant") >= 0 || n.indexOf("tw") >= 0 ||
              n.indexOf("hk") >= 0 || n.indexOf("mo") >= 0) ? "zh-Hant" : "zh-CN";
    }
    if (n.indexOf("pt") === 0) return "pt";
    if (n.indexOf("fr") === 0) return "fr";
    if (n.indexOf("es") === 0) return "es";
    if (n.indexOf("hi") === 0) return "hi";
    if (n.indexOf("bn") === 0) return "bn";
    if (n.indexOf("id") === 0 || n.indexOf("in") === 0) return "id";   // in = 印尼语旧代号
    if (n.indexOf("ur") === 0) return "ur";
    if (n.indexOf("ar") === 0) return "ar";
    return "en-US";
  }

  var theme = pick(q.get("theme"), ["light", "dark"]);
  if (!theme) { try { theme = pick(localStorage.getItem("parlz-theme"), ["light", "dark"]); } catch (e) { /* 忽略 */ } }
  if (!theme) theme = matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";

  var lang = pick(q.get("lang"), LOCALES);
  if (!lang) { try { lang = pick(localStorage.getItem("parlz-lang"), LOCALES); } catch (e) { /* 忽略 */ } }
  if (!lang) lang = fromBrowser();

  r.lang = lang;
  r.dataset.lang = lang;
  r.dataset.theme = theme;
  r.dir = RTL.indexOf(lang) >= 0 ? "rtl" : "ltr";

  // 正文烘的是简体中文：换语言前先别显示，免得闪一下错语言
  // （zh-Hant 的正文是繁体，同样要等 i18n.js 换上）
  if (lang !== "zh-CN") r.classList.add("i18n-wait");

  // 带 hash 打开的（站内链接会带）就把它固化成"当前设置"，免得各处各说各话
  if (q.get("theme") || q.get("lang")) {
    try {
      localStorage.setItem("parlz-theme", theme);
      localStorage.setItem("parlz-lang", lang);
    } catch (e) { /* 忽略 */ }
  }

  // app.js 若没能跑起来（被拦/加载失败），2.5s 后撤掉"等 JS"的状态：
  // 页面退回中文 + 无动效，但**内容完整可见**
  setTimeout(function () {
    if (!window.__parlzReady) r.classList.remove("js", "i18n-wait");
  }, 2500);
})();
