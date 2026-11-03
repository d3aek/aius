(() => {
  "use strict";

  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ---------- scroll reveal ---------- */
  const revealables = document.querySelectorAll(
    ".section__head, .card, .compare, .mixer, .stage, .term, .spec-group, .part, .steps, .doc, .cta__inner"
  );
  if ("IntersectionObserver" in window && !reduceMotion) {
    revealables.forEach((el) => el.classList.add("reveal"));
    const io = new IntersectionObserver(
      (entries) => {
        entries.forEach((e) => {
          if (e.isIntersecting) {
            e.target.classList.add("in-view");
            io.unobserve(e.target);
          }
        });
      },
      { threshold: 0.15, rootMargin: "0px 0px -40px 0px" }
    );
    revealables.forEach((el) => io.observe(el));
  } else {
    revealables.forEach((el) => el.classList.add("in-view"));
  }

  /* ---------- downmix matrix toy ----------
     Illustrative only: models dialogue level as the centre gain (plus a
     share of the presence boost) against the RMS of everything else. */
  const mixer = document.querySelector(".mixer");
  if (!mixer) return;

  const PRESETS = {
    itu:   { c: 71,  f: 100, s: 71,  lfe: 0,  eq: 0 },
    tv:    { c: 71,  f: 100, s: 100, lfe: 50, eq: 0 },
    aius:  { c: 100, f: 80,  s: 45,  lfe: 30, eq: 60 },
    night: { c: 120, f: 60,  s: 30,  lfe: 10, eq: 40 },
  };

  const inputs = {};
  mixer.querySelectorAll("input[data-k]").forEach((el) => (inputs[el.dataset.k] = el));
  const chips = mixer.querySelectorAll("[data-preset]");
  const ratioEl = mixer.querySelector("[data-ratio]");
  const fillEl = mixer.querySelector("[data-fill]");
  const verdictEl = mixer.querySelector("[data-verdict]");
  const meterL = mixer.querySelector('[data-meter="l"]');
  const meterR = mixer.querySelector('[data-meter="r"]');

  const state = () => {
    const v = {};
    for (const k in inputs) v[k] = Number(inputs[k].value);
    return {
      c: v.c / 100, f: v.f / 100, s: v.s / 100, lfe: v.lfe / 100,
      eq: v.eq / 10, // dB
    };
  };

  const fmt = (n) => n.toFixed(2);
  let outGain = 1;

  function render() {
    const st = state();

    mixer.querySelectorAll("[data-out]").forEach((el) => {
      const k = el.dataset.out;
      el.value = el.textContent = k === "eq" ? `+${st.eq.toFixed(1)}` : fmt(st[k]);
    });
    mixer.querySelectorAll("[data-eq]").forEach((el) => {
      el.textContent = fmt(st[el.dataset.eq]);
    });

    const voice = st.c * Math.pow(10, (st.eq * 0.5) / 20);
    const fx = Math.sqrt(st.f ** 2 + st.s ** 2 + (st.lfe * 0.8) ** 2);
    const db = voice <= 0 ? -24 : fx <= 0 ? 24 : 20 * Math.log10(voice / fx);
    const clamped = Math.max(-12, Math.min(12, db));

    ratioEl.textContent = `${db >= 0 ? "+" : ""}${Math.max(-24, Math.min(24, db)).toFixed(1)} dB`;
    fillEl.style.width = `${((clamped + 12) / 24) * 100}%`;

    let msg, bad = false;
    if (db < -3)      { msg = "Dialogue buried. Reaching for the remote."; bad = true; }
    else if (db < 1)  { msg = "Roughly level. Quiet lines will still slip under."; }
    else if (db < 9)  { msg = "Words sit clearly on top of the mix."; }
    else              { msg = "Dialogue way forward. Effects losing their punch."; }
    verdictEl.textContent = msg;
    verdictEl.classList.toggle("is-bad", bad);

    outGain = st.c + st.f + st.s + st.lfe; // drives the fake meters

    // keep preset chip in sync
    const cur = { c: +inputs.c.value, f: +inputs.f.value, s: +inputs.s.value, lfe: +inputs.lfe.value, eq: +inputs.eq.value };
    chips.forEach((ch) => {
      const p = PRESETS[ch.dataset.preset];
      const match = Object.keys(p).every((k) => p[k] === cur[k]);
      ch.classList.toggle("is-active", match);
    });
  }

  function applyPreset(name) {
    const p = PRESETS[name];
    if (!p) return;
    for (const k in p) inputs[k].value = p[k];
    render();
  }

  for (const k in inputs) inputs[k].addEventListener("input", render);
  chips.forEach((ch) => ch.addEventListener("click", () => applyPreset(ch.dataset.preset)));

  /* fake programme-level meters */
  let t = 0;
  function tick() {
    t += 0.06;
    const base = Math.min(1.15, outGain / 2.6);
    const l = base * (0.62 + 0.25 * Math.sin(t * 2.1) + 0.13 * Math.sin(t * 5.3 + 1));
    const r = base * (0.62 + 0.25 * Math.sin(t * 1.8 + 0.7) + 0.13 * Math.sin(t * 4.7));
    meterL.style.width = `${Math.max(4, Math.min(100, l * 100))}%`;
    meterR.style.width = `${Math.max(4, Math.min(100, r * 100))}%`;
    meterL.classList.toggle("is-hot", l > 0.9);
    meterR.classList.toggle("is-hot", r > 0.9);
    requestAnimationFrame(tick);
  }

  render();
  if (!reduceMotion) requestAnimationFrame(tick);
  else { meterL.style.width = "60%"; meterR.style.width = "60%"; }
})();

