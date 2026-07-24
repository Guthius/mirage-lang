document.addEventListener("DOMContentLoaded", function () {
  var themeToggle = document.querySelector(".theme-toggle");
  if (themeToggle) {
    themeToggle.addEventListener("click", function () {
      var current = document.documentElement.getAttribute("data-theme");
      var next = current === "light" ? "dark" : "light";
      document.documentElement.setAttribute("data-theme", next);
      localStorage.setItem("mirage-theme", next);
    });
  }

  var navToggle = document.querySelector(".nav-toggle");
  var sidebar = document.querySelector(".sidebar");
  var backdrop = document.querySelector(".sidebar-backdrop");

  function closeNav() {
    if (sidebar) sidebar.classList.remove("open");
    if (backdrop) backdrop.classList.remove("open");
  }

  if (navToggle && sidebar) {
    navToggle.addEventListener("click", function () {
      sidebar.classList.toggle("open");
      if (backdrop) backdrop.classList.toggle("open");
    });
  }

  if (backdrop) {
    backdrop.addEventListener("click", closeNav);
  }

  sidebar && sidebar.querySelectorAll("a").forEach(function (link) {
    link.addEventListener("click", closeNav);
  });
});
