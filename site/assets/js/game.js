/* 字母陨石 · 26键大冒险 — 尘埃X
 * Canvas meteor game with WebAudio synth sounds.
 * Mechanics: type the falling letter to destroy it. Faster presses => higher
 * attack power; consecutive hits build combo multiplier. Missed meteors / wrong
 * keys reset the combo and (for misses) damage the base.
 */
(function () {
  "use strict";

  // ------------------------------------------------------------------ layout
  const ROWS = ["QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"];
  const FINGER = {
    Q: "f-pinky", A: "f-pinky", Z: "f-pinky",
    W: "f-ring", S: "f-ring", X: "f-ring",
    E: "f-mid", D: "f-mid", C: "f-mid",
    R: "f-index", F: "f-index", V: "f-index", T: "f-index", G: "f-index", B: "f-index",
    Y: "rf-index", H: "rf-index", N: "rf-index", U: "rf-index", J: "rf-index", M: "rf-index",
    I: "rf-mid", K: "rf-mid",
    O: "rf-ring", L: "rf-ring",
    P: "rf-pinky",
  };
  const FINGER_COLOR = {
    "f-pinky": "#ff6b8b", "f-ring": "#ffb35c", "f-mid": "#ffe45c", "f-index": "#5cff9d",
    "rf-index": "#5cd8ff", "rf-mid": "#5c8bff", "rf-ring": "#b56bff", "rf-pinky": "#ff6bf0",
  };

  // ------------------------------------------------------------------ audio
  const Audio = (function () {
    let ctx = null, master = null, enabled = true;
    function ensure() {
      if (!ctx) {
        ctx = new (window.AudioContext || window.webkitAudioContext)();
        master = ctx.createGain();
        master.gain.value = 0.5;
        master.connect(ctx.destination);
      }
      if (ctx.state === "suspended") ctx.resume();
      return ctx;
    }
    function tone(freq, dur, type, vol, freqEnd) {
      if (!enabled) return;
      ensure();
      const t = ctx.currentTime;
      const o = ctx.createOscillator();
      const g = ctx.createGain();
      o.type = type || "square";
      o.frequency.setValueAtTime(freq, t);
      if (freqEnd) o.frequency.exponentialRampToValueAtTime(Math.max(30, freqEnd), t + dur);
      g.gain.setValueAtTime(0.0001, t);
      g.gain.exponentialRampToValueAtTime(vol || 0.3, t + 0.008);
      g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
      o.connect(g); g.connect(master);
      o.start(t); o.stop(t + dur + 0.02);
    }
    function noise(dur, vol, cutoff) {
      if (!enabled) return;
      ensure();
      const t = ctx.currentTime;
      const n = Math.floor(ctx.sampleRate * dur);
      const buf = ctx.createBuffer(1, n, ctx.sampleRate);
      const d = buf.getChannelData(0);
      for (let i = 0; i < n; i++) d[i] = (Math.random() * 2 - 1) * (1 - i / n);
      const src = ctx.createBufferSource();
      src.buffer = buf;
      const f = ctx.createBiquadFilter();
      f.type = "lowpass"; f.frequency.value = cutoff || 1800;
      const g = ctx.createGain(); g.gain.value = vol || 0.4;
      src.connect(f); f.connect(g); g.connect(master);
      src.start(t);
    }
    // background pulse
    let bgTimer = null;
    function startBg() {
      if (!enabled || bgTimer) return;
      let step = 0;
      const notes = [55, 55, 82.4, 55, 65.4, 55, 82.4, 98];
      bgTimer = setInterval(() => {
        tone(notes[step % notes.length], 0.18, "triangle", 0.12);
        step++;
      }, 340);
    }
    function stopBg() { if (bgTimer) { clearInterval(bgTimer); bgTimer = null; } }
    return {
      shoot() { tone(880, 0.09, "square", 0.22, 180); },
      explode() { noise(0.22, 0.5, 1400); tone(220, 0.18, "sawtooth", 0.18, 60); },
      wrong() { tone(150, 0.16, "square", 0.28, 90); },
      combo(level) { const b = 520 + level * 40; tone(b, 0.08, "square", 0.25); setTimeout(() => tone(b * 1.5, 0.1, "square", 0.25), 70); },
      damage() { noise(0.4, 0.6, 800); tone(70, 0.4, "sawtooth", 0.3, 40); },
      levelUp() { [523, 659, 784, 1046].forEach((f, i) => setTimeout(() => tone(f, 0.12, "square", 0.25), i * 70)); },
      startBg, stopBg, ensure,
      toggle() { enabled = !enabled; if (!enabled) stopBg(); return enabled; },
      get enabled() { return enabled; },
    };
  })();

  // ------------------------------------------------------------------ DOM
  const $ = (id) => document.getElementById(id);
  const canvas = $("stage");
  const ctx = canvas.getContext("2d");
  const kbEl = $("keyboard");
  const els = {
    score: $("hud-score"), combo: $("hud-combo"), attack: $("hud-attack"),
    attackFill: $("attack-fill"), level: $("hud-level"), hpFill: $("hp-fill"),
    stageWrap: document.querySelector(".stage-wrap"),
    overStart: $("overlay-start"), overOver: $("overlay-over"),
    finalScore: $("final-score"), finalCombo: $("final-combo"), finalBest: $("final-best"),
    finalRank: $("final-rank"),
  };
  const keyEls = {}; // letter -> element

  (function buildKeyboard() {
    ROWS.forEach((row) => {
      const rowEl = document.createElement("div");
      rowEl.className = "kb-row";
      for (const ch of row) {
        const k = document.createElement("div");
        const fc = FINGER[ch] || "f-index";
        k.className = "key " + fc;
        k.textContent = ch;
        const dot = document.createElement("span");
        dot.className = "finger-dot";
        k.appendChild(dot);
        rowEl.appendChild(k);
        keyEls[ch] = k;
      }
      kbEl.appendChild(rowEl);
    });
  })();

  // ------------------------------------------------------------------ state
  let W = 0, H = 0, dpr = 1;
  const state = {
    running: false,
    meteors: [], particles: [], beams: [], stars: [],
    score: 0, combo: 0, bestCombo: 0, level: 1,
    hp: 100, maxHp: 100,
    spawnTimer: 0, spawnInterval: 1400,
    recentHits: [], // timestamps for attack power (press speed)
    attackMult: 1,
    last: 0,
    best: Number(localStorage.getItem("dustx_best") || 0),
  };

  function resize() {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    const r = els.stageWrap.getBoundingClientRect();
    W = r.width; H = r.height;
    canvas.width = W * dpr; canvas.height = H * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    if (!state.stars.length || state.stars._w !== W) makeStars();
  }
  function makeStars() {
    state.stars = Array.from({ length: 90 }, () => ({
      x: Math.random() * W, y: Math.random() * H,
      z: Math.random() * 0.8 + 0.2, r: Math.random() * 1.4 + 0.3,
    }));
    state.stars._w = W;
  }
  window.addEventListener("resize", resize);

  // ------------------------------------------------------------------ helpers
  const rand = (a, b) => a + Math.random() * (b - a);
  function computeAttack() {
    const now = performance.now();
    state.recentHits = state.recentHits.filter((t) => now - t < 1500);
    const aps = state.recentHits.length / 1.5; // hits per second
    state.attackMult = Math.min(4, 1 + aps * 0.32);
    return state.attackMult;
  }

  function spawnMeteor() {
    const letter = String.fromCharCode(65 + (Math.random() * 26 | 0));
    const fc = FINGER[letter] || "f-index";
    const size = rand(26, 34);
    state.meteors.push({
      letter, x: rand(size + 10, W - size - 10), y: -size,
      vy: (46 + state.level * 7) * rand(0.85, 1.2) / 60, // px per frame (~60fps)
      size, color: FINGER_COLOR[fc], spin: rand(-0.05, 0.05), rot: 0,
      born: performance.now(),
    });
    refreshTargets();
  }

  function refreshTargets() {
    const present = new Set(state.meteors.map((m) => m.letter));
    for (const ch in keyEls) keyEls[ch].classList.toggle("target", present.has(ch));
  }

  function burst(x, y, color, n) {
    for (let i = 0; i < n; i++) {
      const a = Math.random() * Math.PI * 2;
      const sp = rand(1, 6);
      state.particles.push({ x, y, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp, life: 1, color, r: rand(1.5, 4) });
    }
  }

  function flashKey(letter, cls) {
    const k = keyEls[letter];
    if (!k) return;
    k.classList.add(cls);
    setTimeout(() => k.classList.remove(cls), 130);
  }

  // ------------------------------------------------------------------ input
  function onKey(letter) {
    if (!state.running) { start(); return; }
    // pick the most urgent (lowest on screen) meteor with this letter
    let target = null;
    for (const m of state.meteors) {
      if (m.letter === letter && (!target || m.y > target.y)) target = m;
    }
    if (!target) { // wrong key
      state.combo = 0;
      flashKey(letter, "wrong");
      Audio.wrong();
      updateHud();
      return;
    }
    // hit!
    const now = performance.now();
    state.recentHits.push(now);
    const atk = computeAttack();
    state.combo += 1;
    state.bestCombo = Math.max(state.bestCombo, state.combo);
    if (state.combo % 5 === 0) Audio.combo(state.combo / 5);

    // reaction bonus: destroying higher on screen (reacted fast) => more points
    const reaction = 1 + (1 - Math.min(1, target.y / H)) * 0.8;
    const gain = Math.round(10 * (1 + state.combo * 0.1) * atk * reaction);
    state.score += gain;

    state.beams.push({ x: target.x, y: target.y, life: 1, color: target.color });
    burst(target.x, target.y, target.color, 18);
    Audio.shoot(); Audio.explode();
    flashKey(letter, "hit");
    state.meteors.splice(state.meteors.indexOf(target), 1);
    refreshTargets();

    // level up by score
    const newLevel = 1 + Math.floor(state.score / 400);
    if (newLevel > state.level) {
      state.level = newLevel;
      state.spawnInterval = Math.max(430, 1400 - state.level * 85);
      Audio.levelUp();
    }
    updateHud();
  }

  window.addEventListener("keydown", (e) => {
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    const k = e.key.length === 1 ? e.key.toUpperCase() : "";
    if (k >= "A" && k <= "Z") { e.preventDefault(); onKey(k); }
  });

  // ------------------------------------------------------------------ loop
  function update(dt) {
    // spawn
    state.spawnTimer += dt;
    if (state.spawnTimer >= state.spawnInterval) {
      state.spawnTimer = 0;
      spawnMeteor();
    }
    const frame = dt / (1000 / 60);
    // meteors
    for (let i = state.meteors.length - 1; i >= 0; i--) {
      const m = state.meteors[i];
      m.y += m.vy * frame; m.rot += m.spin * frame;
      if (m.y >= H - 26) { // hit base
        state.meteors.splice(i, 1);
        refreshTargets();
        state.combo = 0;
        state.hp = Math.max(0, state.hp - 12);
        Audio.damage();
        els.stageWrap.classList.add("shake");
        setTimeout(() => els.stageWrap.classList.remove("shake"), 300);
        burst(m.x, H - 24, "#ff4d6d", 24);
        updateHud();
        if (state.hp <= 0) return gameOver();
      }
    }
    // particles
    for (let i = state.particles.length - 1; i >= 0; i--) {
      const p = state.particles[i];
      p.x += p.vx * frame; p.y += p.vy * frame; p.vy += 0.08 * frame;
      p.life -= 0.02 * frame;
      if (p.life <= 0) state.particles.splice(i, 1);
    }
    // beams
    for (let i = state.beams.length - 1; i >= 0; i--) {
      state.beams[i].life -= 0.08 * frame;
      if (state.beams[i].life <= 0) state.beams.splice(i, 1);
    }
    computeAttack();
    updateAttackBar();
  }

  function draw() {
    ctx.clearRect(0, 0, W, H);
    // stars
    for (const s of state.stars) {
      s.y += s.z * 0.3;
      if (s.y > H) s.y = 0;
      ctx.globalAlpha = s.z;
      ctx.fillStyle = "#9fb3ff";
      ctx.fillRect(s.x, s.y, s.r, s.r);
    }
    ctx.globalAlpha = 1;

    // base line + planet glow
    const baseY = H - 12;
    const g = ctx.createLinearGradient(0, baseY - 40, 0, H);
    g.addColorStop(0, "rgba(91,140,255,0)");
    g.addColorStop(1, "rgba(91,140,255,0.35)");
    ctx.fillStyle = g;
    ctx.fillRect(0, baseY - 40, W, 52);
    ctx.strokeStyle = "rgba(120,160,255,0.6)";
    ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(0, baseY); ctx.lineTo(W, baseY); ctx.stroke();

    // beams (from base to hit point)
    for (const b of state.beams) {
      ctx.globalAlpha = b.life;
      ctx.strokeStyle = b.color; ctx.lineWidth = 3;
      ctx.beginPath(); ctx.moveTo(W / 2, baseY); ctx.lineTo(b.x, b.y); ctx.stroke();
    }
    ctx.globalAlpha = 1;

    // meteors
    for (const m of state.meteors) {
      ctx.save();
      ctx.translate(m.x, m.y);
      // trail
      ctx.globalAlpha = 0.25;
      ctx.fillStyle = m.color;
      ctx.beginPath(); ctx.ellipse(0, -m.size, m.size * 0.55, m.size * 1.4, 0, 0, Math.PI * 2); ctx.fill();
      ctx.globalAlpha = 1;
      // body
      ctx.shadowColor = m.color; ctx.shadowBlur = 22;
      ctx.fillStyle = m.color;
      ctx.beginPath(); ctx.arc(0, 0, m.size, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
      ctx.fillStyle = "rgba(10,14,30,0.85)";
      ctx.beginPath(); ctx.arc(0, 0, m.size - 4, 0, Math.PI * 2); ctx.fill();
      // letter
      ctx.fillStyle = "#fff";
      ctx.font = `800 ${m.size}px system-ui, sans-serif`;
      ctx.textAlign = "center"; ctx.textBaseline = "middle";
      ctx.fillText(m.letter, 0, 2);
      ctx.restore();
    }

    // particles
    for (const p of state.particles) {
      ctx.globalAlpha = Math.max(0, p.life);
      ctx.fillStyle = p.color;
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2); ctx.fill();
    }
    ctx.globalAlpha = 1;
  }

  function frame(ts) {
    if (!state.running) return;
    const dt = Math.min(50, ts - state.last || 16);
    state.last = ts;
    update(dt);
    draw();
    requestAnimationFrame(frame);
  }

  // ------------------------------------------------------------------ hud
  function updateHud() {
    els.score.textContent = state.score;
    els.combo.textContent = "x" + state.combo;
    els.combo.style.transform = "scale(" + (1 + Math.min(0.6, state.combo * 0.03)) + ")";
    els.level.textContent = state.level;
    els.hpFill.style.width = (state.hp / state.maxHp * 100) + "%";
  }
  function updateAttackBar() {
    els.attack.textContent = "x" + state.attackMult.toFixed(1);
    els.attackFill.style.width = ((state.attackMult - 1) / 3 * 100) + "%";
  }

  // ------------------------------------------------------------------ flow
  function reset() {
    state.meteors = []; state.particles = []; state.beams = [];
    state.score = 0; state.combo = 0; state.bestCombo = 0; state.level = 1;
    state.hp = state.maxHp; state.spawnTimer = 0; state.spawnInterval = 1400;
    state.recentHits = []; state.attackMult = 1;
    refreshTargets(); updateHud(); updateAttackBar();
  }

  function start() {
    Audio.ensure();
    reset();
    els.overStart.classList.add("hidden");
    els.overOver.classList.add("hidden");
    state.running = true;
    state.last = performance.now();
    Audio.startBg();
    requestAnimationFrame(frame);
  }

  function gameOver() {
    state.running = false;
    Audio.stopBg();
    Audio.damage();
    state.best = Math.max(state.best, state.score);
    localStorage.setItem("dustx_best", state.best);
    els.finalScore.textContent = state.score;
    els.finalCombo.textContent = state.bestCombo;
    els.finalBest.textContent = state.best;
    els.finalRank.textContent = rankText(state.score);
    els.overOver.classList.remove("hidden");
  }

  function rankText(s) {
    if (s >= 4000) return "★★★ 键盘宗师 · 尘埃已成星海";
    if (s >= 2000) return "★★ 疾风指法 · 势不可挡";
    if (s >= 800) return "★ 渐入佳境 · 继续加速";
    return "新手上路 · 多练几局盲打！";
  }

  // buttons
  $("btn-start").addEventListener("click", start);
  $("btn-restart").addEventListener("click", start);
  $("sound-toggle").addEventListener("click", (e) => {
    const on = Audio.toggle();
    e.currentTarget.textContent = on ? "🔊" : "🔇";
    if (on && state.running) Audio.startBg();
  });

  // init
  resize();
  reset();
})();
