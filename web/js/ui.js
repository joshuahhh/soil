export { logScale, percent, bindRange, bindCheck, gate };

// --- panel controls ---------------------------------------------------------
// the hud's sliders and switches are each bound to one value: dragging the
// control writes the value and calls the panel's apply, and the panel's
// refresh puts every value back into its control and readout (after a
// restore, a __demo call, the solver moving the lens...). a control's
// binding is one line, so adding one is the markup plus that line

// a slider that is log across min..max, its track 0..steps: the things on
// these sliders are felt in ratios. off: the bottom notch means 0 (off), and
// the scale starts one notch up. dp rounds what the slider sets, so that
// what is saved and shown is a round number
const logScale = (min, max, { steps = 1, off = false, dp = null } = {}) => {
  const k = Math.log(max / min);
  const round = dp === null ? (x) => x : (x) => Math.round(x * 10 ** dp) / 10 ** dp;
  return {
    from: (v) => off && v <= 0 ? 0 : round(min * Math.exp(k * v / steps)),
    to: (x) => off && x <= 0 ? 0 : steps * Math.log(Math.max(min, Math.min(max, x)) / min) / k,
  };
};
// a fraction on a 0..100 track
const percent = { from: (v) => v / 100, to: (x) => Math.round(x * 100) };
const linear = { from: (v) => v, to: (x) => x };

// a range input bound to obj[key] (or to opts.get/opts.set, for a value that
// lives somewhere else). fmt writes the readout, #<id>amount unless amount
// names another. on is the event that commits ('input' while dragging, by
// default). returns the refresh
const bindRange = (id, obj, key, { scale = linear, fmt, amount = id + 'amount', on = 'input',
  get = () => obj[key], set = (v) => { obj[key] = v; }, apply }) => {
  const el = document.getElementById(id), out = fmt && document.getElementById(amount);
  el.addEventListener(on, () => { set(scale.from(+el.value)); apply(); });
  return () => {
    el.value = scale.to(get());
    if (out) out.textContent = fmt(get());
  };
};
const bindCheck = (id, obj, key, apply) => {
  const el = document.getElementById(id);
  el.addEventListener('change', () => { obj[key] = el.checked; apply(); });
  return () => { el.checked = obj[key]; };
};

// what a switch (or another panel's) has turned off is dimmed and locked,
// its value kept: .off on each element, and every input in or among them
// disabled
const gate = (on, ...els) => {
  for (const el of els) {
    el.classList.toggle('off', !on);
    for (const input of el.tagName === 'INPUT' ? [el] : el.querySelectorAll('input'))
      input.disabled = !on;
  }
};
