(function (root) {
  function digits(id) {
    return String(id || "").replace(/\D/g, "");
  }

  function formatId(id) {
    return digits(id).replace(/(\d{3})(?=\d)/g, "$1 ").trim();
  }

  function isMaskedSecret(v) {
    const s = String(v || "");
    return !s || /^[•·.\-\s]+$/.test(s);
  }

  function bindPeek(btn, reveal, hide, opts) {
    if (!btn) return;
    const hold = !!(opts && opts.hold);
    if (hold) {
      let held = false;
      const down = (ev) => {
        ev.preventDefault();
        held = true;
        if (btn.setPointerCapture && ev.pointerId != null) {
          try { btn.setPointerCapture(ev.pointerId); } catch (e) { /* ignore */ }
        }
        reveal();
      };
      const up = () => {
        if (!held) return;
        held = false;
        hide();
      };
      btn.addEventListener("pointerdown", down);
      btn.addEventListener("pointerup", up);
      btn.addEventListener("pointercancel", up);
      btn.addEventListener("keydown", (ev) => {
        if (ev.key === " " || ev.key === "Enter") down(ev);
      });
      btn.addEventListener("keyup", up);
      window.addEventListener("blur", up);
      btn.addEventListener("dragstart", (ev) => ev.preventDefault());
      btn.setAttribute("aria-label", "按住查看密码");
      btn.title = "按住查看";
      return;
    }
    let open = false;
    const label = () => {
      const text = (btn.textContent || "").trim();
      if (text === "查看" || text === "隐藏") btn.textContent = open ? "隐藏" : "查看";
    };
    const setOpen = (next) => {
      open = !!next;
      if (open) reveal();
      else hide();
      btn.setAttribute("aria-pressed", open ? "true" : "false");
      label();
    };
    btn.addEventListener("click", (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      setOpen(!open);
    });
    btn.setAttribute("aria-label", "查看密码");
    btn.title = "点击查看或隐藏";
    btn.setAttribute("aria-pressed", "false");
  }

  function bindSecretText(el, btn) {
    if (!el) return el;
    el._open = false;
    el._secret = "";
    const paint = () => {
      const v = el._secret || "";
      el.textContent = el._open ? (v || "------") : (v ? "••••••••" : "------");
      el.classList.toggle("is-open", !!el._open);
    };
    el._setSecret = (v) => {
      if (isMaskedSecret(v)) return;
      el._secret = String(v);
      paint();
    };
    el._getSecret = () => el._secret || "";
    el._showSecret = (show) => {
      el._open = !!show;
      paint();
    };
    paint();
    if (btn) bindPeek(btn, () => el._showSecret(true), () => el._showSecret(false));
    return el;
  }

  function bindSecretInput(input, btn) {
    if (!input) return;
    input.type = "password";
    input.setAttribute("autocomplete", "new-password");
    input.setAttribute("spellcheck", "false");
    bindPeek(btn, () => { input.type = "text"; }, () => { input.type = "password"; }, { hold: true });
  }

  function bindIdCombo(wrap, opts) {
    if (!wrap) return;
    const input = wrap.querySelector("input");
    const list = wrap.querySelector(".combo-list");
    const toggle = wrap.querySelector(".combo-toggle");
    if (!input || !list) return;
    let items = [];
    let open = false;

    const render = () => {
      const q = digits(input.value);
      const shown = items.filter((it) => !q || String(it.id || "").includes(q));
      if (!shown.length) {
        list.innerHTML = `<div class="combo-empty">${items.length ? "没有匹配的记录" : "还没有历史连接"}</div>`;
        return;
      }
      list.innerHTML = shown.map((it) => {
        const hint = it.hint || it.name || "历史连接";
        return `<button type="button" class="combo-item" data-id="${it.id}">
          <strong>${formatId(it.id)}</strong>
          <span>${hint}</span>
        </button>`;
      }).join("");
    };

    const refresh = async () => {
      try { items = (await opts.loadItems()) || []; } catch { items = []; }
      render();
    };

    const show = () => {
      open = true;
      list.hidden = false;
      wrap.classList.add("is-open");
      refresh();
    };
    const hide = () => {
      open = false;
      list.hidden = true;
      wrap.classList.remove("is-open");
    };

    toggle?.addEventListener("click", (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      if (open) hide();
      else show();
    });
    input.addEventListener("focus", show);
    input.addEventListener("input", () => {
      input.value = formatId(input.value);
      if (open) render();
      else show();
    });
    list.addEventListener("mousedown", (ev) => {
      const btn = ev.target.closest("[data-id]");
      if (!btn) return;
      ev.preventDefault();
      const it = items.find((x) => x.id === btn.getAttribute("data-id"));
      if (it && opts.onPick) opts.onPick(it);
      hide();
    });
    document.addEventListener("mousedown", (ev) => {
      if (!wrap.contains(ev.target)) hide();
    });
  }

  root.dustxUi = { digits, formatId, isMaskedSecret, bindPeek, bindSecretText, bindSecretInput, bindIdCombo };
})(window);
