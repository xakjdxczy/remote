// Dust particle background for 尘埃X. Lightweight canvas animation.
(function () {
  document.getElementById("year").textContent = new Date().getFullYear();

  const canvas = document.getElementById("dust");
  const ctx = canvas.getContext("2d");
  let w, h, particles, raf;
  const COLORS = ["#5b8cff", "#b56bff", "#22e6c8", "#8aa4ff"];

  function resize() {
    w = canvas.width = window.innerWidth * devicePixelRatio;
    h = canvas.height = window.innerHeight * devicePixelRatio;
    canvas.style.width = window.innerWidth + "px";
    canvas.style.height = window.innerHeight + "px";
    const count = Math.min(150, Math.floor((window.innerWidth * window.innerHeight) / 12000));
    particles = Array.from({ length: count }, spawn);
  }

  function spawn() {
    return {
      x: Math.random() * w,
      y: Math.random() * h,
      r: (Math.random() * 1.8 + 0.4) * devicePixelRatio,
      vx: (Math.random() - 0.5) * 0.25 * devicePixelRatio,
      vy: (Math.random() - 0.5) * 0.25 * devicePixelRatio,
      a: Math.random() * 0.5 + 0.15,
      c: COLORS[(Math.random() * COLORS.length) | 0],
    };
  }

  const mouse = { x: -9999, y: -9999 };
  window.addEventListener("mousemove", (e) => {
    mouse.x = e.clientX * devicePixelRatio;
    mouse.y = e.clientY * devicePixelRatio;
  });

  function tick() {
    ctx.clearRect(0, 0, w, h);
    for (const p of particles) {
      // gentle drift + subtle attraction toward the cursor
      const dx = mouse.x - p.x, dy = mouse.y - p.y;
      const d2 = dx * dx + dy * dy;
      if (d2 < (160 * devicePixelRatio) ** 2) {
        p.vx += (dx / Math.sqrt(d2 + 1)) * 0.02;
        p.vy += (dy / Math.sqrt(d2 + 1)) * 0.02;
      }
      p.x += p.vx;
      p.y += p.vy;
      p.vx *= 0.99;
      p.vy *= 0.99;
      if (p.x < 0) p.x = w; if (p.x > w) p.x = 0;
      if (p.y < 0) p.y = h; if (p.y > h) p.y = 0;
      ctx.globalAlpha = p.a;
      ctx.fillStyle = p.c;
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.r, 0, Math.PI * 2);
      ctx.fill();
    }
    // faint connecting lines for a constellation feel
    ctx.globalAlpha = 0.06;
    ctx.strokeStyle = "#8aa4ff";
    for (let i = 0; i < particles.length; i += 2) {
      const a = particles[i];
      for (let j = i + 1; j < i + 6 && j < particles.length; j++) {
        const b = particles[j];
        const dx = a.x - b.x, dy = a.y - b.y;
        if (dx * dx + dy * dy < (120 * devicePixelRatio) ** 2) {
          ctx.beginPath();
          ctx.moveTo(a.x, a.y);
          ctx.lineTo(b.x, b.y);
          ctx.stroke();
        }
      }
    }
    ctx.globalAlpha = 1;
    raf = requestAnimationFrame(tick);
  }

  window.addEventListener("resize", resize);
  resize();
  tick();
})();
