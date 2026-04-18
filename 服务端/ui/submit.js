(() => {
  const textarea = document.getElementById("submit-text");
  const counter = document.getElementById("char-counter");

  if (!textarea || !counter) {
    return;
  }

  function refreshCounter() {
    const max = textarea.maxLength > 0 ? textarea.maxLength : 0;
    counter.textContent = `${textarea.value.length} / ${max}`;
  }

  refreshCounter();
  textarea.addEventListener("input", refreshCounter);
})();
