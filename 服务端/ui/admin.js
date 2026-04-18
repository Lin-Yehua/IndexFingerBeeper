(() => {
  const params = new URLSearchParams(window.location.search);
  const page = params.get("page") || "1";
  const storageKey = `admin-scroll-${page}`;

  function restorePosition() {
    if (window.location.hash) {
      const target = document.querySelector(window.location.hash);
      if (target) {
        target.scrollIntoView({ behavior: "auto", block: "center" });
        target.classList.add("flash");
        window.setTimeout(() => target.classList.remove("flash"), 1200);
        return;
      }
    }

    const savedY = window.sessionStorage.getItem(storageKey);
    if (savedY !== null) {
      window.scrollTo(0, Number(savedY));
    }
  }

  window.addEventListener("beforeunload", () => {
    window.sessionStorage.setItem(storageKey, String(window.scrollY));
  });

  window.addEventListener("load", restorePosition);
})();
