(() => {
  const msg = document.getElementById("message");
  const counter = document.getElementById("char-counter");
  const target = document.getElementById("target");
  const radios = Array.from(document.querySelectorAll("input[name='targetMode']"));

  if (!msg || !counter) return;

  function refreshCounter() {
    const max = msg.maxLength > 0 ? msg.maxLength : 0;
    counter.textContent = `${msg.value.length} / ${max}`;
  }

  function syncTargetMode() {
    const randomMode = radios.some((r) => r.checked && r.value === "random");
    if (target) target.disabled = randomMode;
  }

  refreshCounter();
  syncTargetMode();
  msg.addEventListener("input", refreshCounter);
  radios.forEach((r) => r.addEventListener("change", syncTargetMode));
})();
