(function () {
  var stored = localStorage.getItem("mirage-theme");
  var theme = stored || (window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark");
  document.documentElement.setAttribute("data-theme", theme);
})();
