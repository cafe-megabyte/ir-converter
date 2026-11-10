// IR Converter user interface. All processing happens in worker.js / engine.wasm.
const TEXT = {
  de: {
    open: 'Öffnen', drop: 'NEF- oder DNG-Datei öffnen oder hier ablegen',
    hint: 'Alles wird lokal auf diesem Gerät berechnet.',
    swap: 'Rot/Blau-Tausch', swapModes: ['Kein Tausch', 'Tausch im RAW', 'Tausch nach Entwicklung'],
    space: 'Farbraum', info: 'Erklärung', explain: ['Rot und Blau bleiben, wie die Kamera sie aufgenommen hat.',
      'Rot und Blau werden in den Rohdaten getauscht, vor Weißabgleich und Farbmatrix. Das DNG enthält die Rohdaten verlustfrei.',
      'Getauscht wird nach der Entwicklung im gewählten Farbraum, wie mit einem Kanalmixer; das JPG wird in diesem Farbraum gespeichert. ' +
      'Das DNG enthält weiterhin die unveränderten, ungetauschten Rohdaten; der Tausch steckt im eingebetteten Farbprofil. Lightroom und Camera Raw ' +
      'zeigen die Farben daher nur angenähert, und ihr Temperaturregler wirkt in umgekehrter Richtung.'],
    temp: 'Farbtemperatur', asShot: 'Wie Aufnahme',
    saveDng: 'DNG speichern', saveJpg: 'JPG speichern',
    opening: 'Öffne …', savingDng: 'Erzeuge DNG …', savingJpg: 'Erzeuge JPG …', saved: 'Gespeichert: ',
    asShotIs: 'Aufnahme: ', invalid: 'Bitte eine Temperatur größer als 0 eingeben.',
    warnCamera: 'Kamera nicht in der Tabelle – Farben nur angenähert.', warnWb: 'Kein Weißabgleich in der Datei – Startwert geschätzt.',
    errors: {1: 'Nicht genug Speicher für dieses Bild.', 2: 'Keine NEF- oder DNG-Datei.', 4: 'Die Datei ist beschädigt oder unvollständig.',
      5: 'Kein Bild geöffnet.', '-1': 'Interner Fehler: '},
    unsupported: {0: 'Dieses Dateiformat wird nicht unterstützt.',
      1: 'Diese Kompression wird nicht unterstützt (z. B. verlustbehaftetes oder JPEG-XL-DNG, Nikon „High Efficiency“).',
      2: 'Dieses lineare DNG wird nicht unterstützt (nur drei Farbkanäle).', 3: 'Nur Bayer-Sensoren (RGB 2×2) und lineare DNGs werden unterstützt.',
      4: 'Dieser Bildaufbau wird nicht unterstützt.', 5: 'Die Datei enthält kein RAW-Bild.'}
  },
  en: {
    open: 'Open', drop: 'Open or drop a NEF or DNG file',
    hint: 'Everything is computed locally on this device.',
    swap: 'Red/blue swap', swapModes: ['No swap', 'Swap in raw', 'Swap after development'],
    space: 'Colour space', info: 'Explanation', explain: ['Red and blue stay as the camera recorded them.',
      'Red and blue are swapped in the raw data, before white balance and colour matrix. The DNG keeps the raw data losslessly.',
      'Red and blue are swapped after development in the chosen colour space, like a channel mixer; the JPG is saved in this colour space. ' +
      'The DNG still holds the unchanged, unswapped raw data; the swap is part of its embedded colour profile. Lightroom and Camera Raw ' +
      'therefore show the colours only approximately, and their temperature slider works in the opposite direction.'],
    temp: 'Temperature', asShot: 'As shot',
    saveDng: 'Save DNG', saveJpg: 'Save JPG',
    opening: 'Opening …', savingDng: 'Creating DNG …', savingJpg: 'Creating JPG …', saved: 'Saved: ',
    asShotIs: 'As shot: ', invalid: 'Please enter a temperature above 0.',
    warnCamera: 'Camera not in the table – colours are approximate.', warnWb: 'No white balance in the file – start value estimated.',
    errors: {1: 'Not enough memory for this image.', 2: 'Not a NEF or DNG file.', 4: 'The file is damaged or incomplete.',
      5: 'No image open.', '-1': 'Internal error: '},
    unsupported: {0: 'This file format is not supported.',
      1: 'This compression is not supported (e.g. lossy or JPEG XL DNG, Nikon “High Efficiency”).',
      2: 'This linear DNG is not supported (three colour channels only).', 3: 'Only Bayer sensors (RGB 2×2) and linear DNGs are supported.',
      4: 'This image layout is not supported.', 5: 'The file contains no raw image.'}
  }
};
const SPAN = 2;   // slider covers 2^-SPAN .. 2^SPAN around its centre
const SPACES = ['sRGB', 'Display P3', 'Adobe RGB'];
const SWAP_POST = 2;
const MAX_VIEW = 2048;   // largest view the worker renders, per side
// Segmented control: one button per option, behaving as a radio group.
const segment = (ref, label, names, attr) =>
  `<div class="seg" data-ref="${ref}" role="radiogroup" aria-label="${label}">` +
  names.map((n, i) => `<button type="button" role="radio" aria-checked="false" data-${attr}="${i}" disabled>${n}</button>`).join('') + '</div>';

export async function start(root, {release} = {}) {
  const lang = (navigator.languages?.[0] || navigator.language || 'en').toLowerCase().startsWith('de') ? 'de' : 'en';
  const t = TEXT[lang];
  document.documentElement.lang = lang;
  const css = document.createElement('link');
  css.rel = 'stylesheet'; css.href = new URL('app.css', import.meta.url).href;
  await new Promise(resolve => { css.onload = css.onerror = resolve; document.head.append(css); });

  const ui = document.createElement('div');
  ui.className = 'ir';
  ui.dataset.release = release || '';
  ui.innerHTML = `
    <header class="bar">
      <span class="brand">IR Converter</span>
      <span class="meta" data-ref="meta"></span>
      <label class="btn primary"><span>${t.open}</span><input data-ref="file" type="file" accept=".nef,.NEF,.dng,.DNG,image/x-adobe-dng,image/x-nikon-nef"></label>
    </header>
    <section class="stage" data-ref="stage">
      <canvas data-ref="canvas" hidden></canvas>
      <canvas data-ref="gpu" hidden></canvas>
      <div class="empty" data-ref="empty"><p class="drop">${t.drop}</p><p class="hint">${t.hint}</p></div>
      <p class="tip" id="ir-tip" data-ref="tip" role="note" hidden></p>
      <div class="busy" data-ref="busy" hidden><span class="spinner"></span><span data-ref="busyText"></span></div>
    </section>
    <footer class="panel">
      <div class="row">
        ${segment('swap', t.swap, t.swapModes, 'mode')}
        ${segment('space', t.space, SPACES, 'space')}
        <button type="button" class="info" data-ref="info" aria-label="${t.info}" aria-expanded="false" aria-controls="ir-tip">i</button>
        <span class="asshot" data-ref="asshot"></span>
      </div>
      <div class="row temp">
        <span class="label">${t.temp}</span>
        <input data-ref="slider" class="slider" type="range" min="-1000" max="1000" step="1" value="0" disabled aria-label="${t.temp}">
        <span class="kelvin"><input data-ref="kelvin" type="text" inputmode="decimal" autocomplete="off" disabled aria-label="${t.temp} (K)"><span>K</span></span>
        <button data-ref="reset" class="btn" disabled>${t.asShot}</button>
      </div>
      <div class="row actions">
        <button data-ref="dng" class="btn" disabled>${t.saveDng}</button>
        <button data-ref="jpg" class="btn" disabled>${t.saveJpg}</button>
        <span class="msg" data-ref="msg" role="status"></span>
      </div>
    </footer>`;
  const $ = {};
  ui.querySelectorAll('[data-ref]').forEach(el => { $[el.dataset.ref] = el; });
  root.querySelector('#boot')?.remove();
  root.append(ui);
  const ctx = $.canvas.getContext('2d');

  // HDR: an extended-range WebGPU canvas (Display P3, half floats). Where
  // the browser or display cannot do that, the 2D canvas shows SDR.
  const hdrQuery = matchMedia('(dynamic-range: high)');
  let hdr = null;
  async function initHdr() {
    if (!navigator.gpu || !hdrQuery.matches) return null;
    try {
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) return null;
      const device = await adapter.requestDevice();
      const context = $.gpu.getContext('webgpu');
      if (!context) return null;
      context.configure({device, format: 'rgba16float', colorSpace: 'display-p3', alphaMode: 'opaque',
        toneMapping: {mode: 'extended'}, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_DST});
      const config = context.getConfiguration?.();
      if (config && config.toneMapping?.mode !== 'extended') { context.unconfigure(); device.destroy(); return null; }
      const state = {device, context};
      device.lost.then(() => { if (hdr === state) { hdr = null; updateMeta(); requestView(); } });
      return state;
    } catch (error) { console.debug('IR Converter HDR unavailable:', error); return null; }
  }
  function refreshHdr() {
    initHdr().then(state => {
      if (!state && hdr) { hdr.context.unconfigure(); hdr.device.destroy(); }
      if (state && hdr) { state.context.unconfigure(); state.device.destroy(); state = hdr; }
      hdr = state; updateMeta(); if (opened) requestView();
    });
  }
  hdrQuery.addEventListener?.('change', () => { if (!hdrQuery.matches && hdr) { hdr.context.unconfigure(); hdr.device.destroy(); hdr = null; updateMeta(); if (opened) requestView(); } else refreshHdr(); });
  let metaText = '';
  function updateMeta() { $.meta.textContent = metaText + (hdr && metaText ? ' · HDR' : ''); }

  const worker = new Worker(new URL('worker.js', import.meta.url));
  let opened = false, center = 5000, current = 5000, name = 'image', busyJob = null, pendingFile = null;
  let swapMode = 1, space = 0;   // until a file opens: swap in the raw data
  const segButtons = el => [...el.querySelectorAll('button')];
  function showSwap() {
    segButtons($.swap).forEach(b => { const on = +b.dataset.mode === swapMode; b.setAttribute('aria-checked', on); b.classList.toggle('on', on); });
    segButtons($.space).forEach(b => { const on = +b.dataset.space === space; b.setAttribute('aria-checked', on); b.classList.toggle('on', on); });
    $.tip.textContent = t.explain[swapMode];
    enable();
  }
  // The colour space applies only to the swap after development; it stays
  // in place (disabled) otherwise, so the image keeps its size and position.
  function enable() {
    const off = !!busyJob || !opened;
    for (const el of [$.slider, $.kelvin, $.reset, $.dng, $.jpg, ...segButtons($.swap)]) el.disabled = off;
    for (const el of segButtons($.space)) el.disabled = off || swapMode !== SWAP_POST;
    $.file.disabled = !!busyJob;
  }

  const fmt = v => {
    if (!(v > 0)) return '';
    if (v >= 100) return Math.round(v).toLocaleString(lang);
    if (v >= 1) return v.toLocaleString(lang, {maximumFractionDigits: 1});
    return v.toLocaleString(lang, {maximumSignificantDigits: 3});
  };
  const parse = text => {
    let s = String(text).trim().toLowerCase().replace(/[\s'’  ]/g, '');
    let mult = 1;
    if (s.endsWith('k')) { mult = 1000; s = s.slice(0, -1); }
    if (/^\d{1,3}([.,]\d{3})+$/.test(s) && mult === 1) s = s.replace(/[.,]/g, '');   // thousands separators
    else if (s.includes(',') && s.includes('.')) s = s.lastIndexOf(',') > s.lastIndexOf('.') ? s.replace(/\./g, '').replace(',', '.') : s.replace(/,/g, '');
    else s = s.replace(',', '.');
    const v = Number(s) * mult;
    return Number.isFinite(v) && v > 0 ? v : NaN;
  };
  const sliderValue = () => center * 2 ** (SPAN * $.slider.value / 1000);
  function setTemperature(v, recenter) {
    current = v;
    if (recenter) { center = v; $.slider.value = 0; }
    if (document.activeElement !== $.kelvin) $.kelvin.value = fmt(v);
    worker.postMessage({type: 'temperature', value: v});
  }
  function message(text, kind = '') { $.msg.textContent = text; $.msg.dataset.kind = kind; }
  function setBusy(text) {
    busyJob = text;
    $.busy.hidden = !text; $.busyText.textContent = text || '';
    enable();
  }
  function errorText(data) {
    if (data.code === 3) return t.unsupported[data.detail] || t.unsupported[0];
    if (data.code === -1) return t.errors['-1'] + (data.message || '');
    return t.errors[data.code] || t.errors[2];
  }
  function applyInfo(info) {
    swapMode = info.swap; space = info.space;
    showSwap();
    center = current = info.temperature;
    $.slider.value = 0;
    $.kelvin.value = fmt(info.temperature);
    $.asshot.textContent = t.asShotIs + fmt(info.asShot) + ' K';
  }
  // Zoom and pan. `zoom` is relative to the fitted image (1 = whole image);
  // (cx, cy) is the image point at the centre of the stage, in pixels of the
  // oriented image. The newest frame keeps showing, moved and scaled, until
  // the engine has rendered the new view (sharp up to single pixels).
  let imgW = 0, imgH = 0, zoom = 1, cx = 0, cy = 0, frame = null;
  const dprNow = () => Math.min(window.devicePixelRatio || 1, 3);
  const stageSize = () => ({w: $.stage.clientWidth, h: $.stage.clientHeight});
  const fitScale = () => { const {w, h} = stageSize(); return imgW ? Math.min(w / imgW, h / imgH) : 1; };
  // Largest zoom: one image pixel per CSS pixel, and at least two screen pixels.
  const maxZoom = () => Math.max(1, Math.max(1, 2 / dprNow()) / fitScale());
  function clampView() {
    zoom = Math.min(Math.max(zoom, 1), maxZoom());
    const s = fitScale() * zoom, {w, h} = stageSize();
    const clamp = (c, size, view) => size * s <= view ? size / 2 : Math.min(Math.max(c, view / (2 * s)), size - view / (2 * s));
    cx = clamp(cx, imgW, w); cy = clamp(cy, imgH, h);
  }
  function resetZoom() { zoom = 1; cx = imgW / 2; cy = imgH / 2; }
  // Positions the canvases so the frame's image rectangle lands where the
  // current zoom and pan put it.
  function place() {
    if (!frame) return;
    const s = fitScale() * zoom, {w, h} = stageSize(), [rx, ry, rw, rh] = frame.region;
    const box = {left: (rx - cx) * s + w / 2, top: (ry - cy) * s + h / 2, width: rw * s, height: rh * s};
    const sharp = box.width * dprNow() > frame.width * 1.5;   // magnified beyond the frame: show pixels
    for (const c of [$.canvas, $.gpu]) {
      for (const k in box) c.style[k] = box[k] + 'px';
      c.classList.toggle('pixels', sharp);
    }
    $.stage.classList.toggle('zoomed', zoom > 1.001);
  }
  function requestView() {
    const {w, h} = stageSize(), dpr = dprNow();
    if (!imgW || zoom <= 1.001) {
      worker.postMessage({type: 'view', width: Math.round(w * dpr), height: Math.round(h * dpr), hdr: !!hdr});
      return;
    }
    const s = fitScale() * zoom;
    // Whole image pixels, so the frame can be placed exactly.
    const x0 = Math.max(0, Math.floor(cx - w / (2 * s))), x1 = Math.min(imgW, Math.ceil(cx + w / (2 * s)));
    const y0 = Math.max(0, Math.floor(cy - h / (2 * s))), y1 = Math.min(imgH, Math.ceil(cy + h / (2 * s)));
    const k = Math.min(1, MAX_VIEW / ((x1 - x0) * s * dpr), MAX_VIEW / ((y1 - y0) * s * dpr));   // keep the aspect ratio
    worker.postMessage({type: 'view', width: Math.ceil((x1 - x0) * s * dpr * k), height: Math.ceil((y1 - y0) * s * dpr * k),
      hdr: !!hdr, region: [x0, y0, x1 - x0, y1 - y0]});
  }
  // Zooms by `factor` keeping the image point under (px, py) (stage pixels) in place.
  function zoomAt(factor, px, py) {
    if (!opened || !imgW) return;
    const {w, h} = stageSize(), s = fitScale() * zoom;
    const ix = cx + (px - w / 2) / s, iy = cy + (py - h / 2) / s;
    zoom *= factor; clampView();
    const s2 = fitScale() * zoom;
    cx = ix - (px - w / 2) / s2; cy = iy - (py - h / 2) / s2;
    clampView(); place(); requestView();
  }
  function panBy(dx, dy) {
    if (!opened || zoom <= 1.001) return;
    const s = fitScale() * zoom;
    cx -= dx / s; cy -= dy / s;
    clampView(); place(); requestView();
  }
  async function save(buffer, ext, mime) {
    const file = new File([buffer], `${name}-IR.${ext}`, {type: mime});
    const touch = matchMedia('(pointer: coarse)').matches;
    if (touch && navigator.canShare?.({files: [file]})) {
      try { await navigator.share({files: [file]}); message(t.saved + file.name, 'ok'); return; }
      catch (error) { if (error.name === 'AbortError') return; }
    }
    const url = URL.createObjectURL(file);
    const a = document.createElement('a');
    a.href = url; a.download = file.name; a.rel = 'noopener';
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    message(t.saved + file.name, 'ok');
  }
  async function openFile(file) {
    if (!file) return;
    if (busyJob) { pendingFile = file; return; }
    setBusy(t.opening); message('');
    name = file.name.replace(/\.[^.]+$/, '') || 'image';
    try {
      const buffer = await file.arrayBuffer();
      worker.postMessage({type: 'open', buffer, name: file.name}, [buffer]);
    } catch (error) { setBusy(null); message(String(error.message || error), 'error'); }
  }

  worker.onmessage = ({data}) => {
    switch (data.type) {
    case 'opened': {
      opened = true;
      imgW = data.info.width; imgH = data.info.height; frame = null; resetZoom();
      applyInfo(data.info);
      metaText = `${data.info.camera} · ${data.info.width} × ${data.info.height}`;
      updateMeta();
      const warn = [];
      if (data.info.warnings & 1) warn.push(t.warnCamera);
      if (data.info.warnings & 2) warn.push(t.warnWb);
      message(warn.join(' '), warn.length ? 'warn' : '');
      $.empty.hidden = true;
      setBusy(null);
      requestView();
      break;
    }
    case 'state': applyInfo(data.info); break;
    case 'frame': {
      if (data.hdr) {
        if (!hdr) { requestView(); break; }   // HDR went away meanwhile
        if ($.gpu.width !== data.width || $.gpu.height !== data.height) { $.gpu.width = data.width; $.gpu.height = data.height; }
        hdr.device.queue.writeTexture({texture: hdr.context.getCurrentTexture()}, data.buffer,
          {bytesPerRow: data.width * 8}, {width: data.width, height: data.height});
      } else {
        if ($.canvas.width !== data.width || $.canvas.height !== data.height) { $.canvas.width = data.width; $.canvas.height = data.height; }
        ctx.putImageData(new ImageData(new Uint8ClampedArray(data.buffer), data.width, data.height), 0, 0);
      }
      $.gpu.hidden = !data.hdr; $.canvas.hidden = !!data.hdr;
      frame = {region: data.region, width: data.width};
      place();
      break;
    }
    case 'exported': {
      setBusy(null);
      save(data.buffer, data.format, data.format === 'dng' ? 'image/x-adobe-dng' : 'image/jpeg');
      break;
    }
    case 'error':
      if (data.during !== 'export' && busyJob === t.opening) { opened = false; $.canvas.hidden = $.gpu.hidden = true; $.empty.hidden = false; metaText = ''; updateMeta(); }
      setBusy(null);
      message(errorText(data), 'error');
      break;
    }
    if (!busyJob && pendingFile) { const f = pendingFile; pendingFile = null; openFile(f); }
  };
  worker.postMessage({type: 'init', wasm: new URL('engine.wasm', import.meta.url).href});
  refreshHdr();

  $.file.addEventListener('change', () => { openFile($.file.files[0]); $.file.value = ''; });
  $.swap.addEventListener('click', e => {
    const mode = +e.target.closest('button')?.dataset.mode;
    if (Number.isInteger(mode) && mode !== swapMode) { swapMode = mode; showSwap(); worker.postMessage({type: 'swap', mode}); }
  });
  $.space.addEventListener('click', e => {
    const value = +e.target.closest('button')?.dataset.space;
    if (Number.isInteger(value) && value !== space) { space = value; showSwap(); worker.postMessage({type: 'space', space: value}); }
  });
  // Arrow keys move within a segmented control, as in a radio group.
  for (const group of [$.swap, $.space]) group.addEventListener('keydown', e => {
    const step = {ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1}[e.key];
    if (!step) return;
    const buttons = segButtons(group), i = buttons.indexOf(document.activeElement);
    if (i < 0) return;
    e.preventDefault();
    const next = buttons[(i + step + buttons.length) % buttons.length];
    next.focus(); next.click();
  });
  $.slider.addEventListener('input', () => setTemperature(sliderValue(), false));
  $.slider.addEventListener('change', () => setTemperature(sliderValue(), true));
  $.kelvin.addEventListener('change', () => {
    const v = parse($.kelvin.value);
    if (Number.isNaN(v)) { message(t.invalid, 'warn'); $.kelvin.value = fmt(current); return; }
    message('');
    setTemperature(v, true);
    $.kelvin.value = fmt(v);
  });
  $.kelvin.addEventListener('keydown', e => { if (e.key === 'Enter') $.kelvin.blur(); });
  $.reset.addEventListener('click', () => worker.postMessage({type: 'reset'}));
  $.dng.addEventListener('click', () => { setBusy(t.savingDng); worker.postMessage({type: 'export', format: 'dng'}); });
  $.jpg.addEventListener('click', () => { setBusy(t.savingJpg); worker.postMessage({type: 'export', format: 'jpg'}); });
  // Explanation of the swap mode, on demand.
  function showTip(show) { $.tip.hidden = !show; $.info.setAttribute('aria-expanded', show); $.info.classList.toggle('on', show); }
  $.info.addEventListener('click', () => showTip($.tip.hidden));
  document.addEventListener('pointerdown', e => { if (!$.tip.hidden && e.target !== $.info && !$.tip.contains(e.target)) showTip(false); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$.tip.hidden) showTip(false); });

  // Zoom: two fingers, trackpad pinch, mouse wheel; drag pans; double click or
  // double tap toggles between the whole image and the largest zoom.
  const at = e => { const r = $.stage.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  $.stage.addEventListener('wheel', e => {
    if (!opened) return;
    e.preventDefault();
    const d = e.deltaMode === 1 ? e.deltaY * 33 : e.deltaY;
    // A mouse wheel zooms; a trackpad pans with two fingers and zooms by pinching (ctrlKey).
    const wheel = e.deltaMode === 1 || (e.deltaX === 0 && Number.isInteger(e.deltaY) && Math.abs(e.deltaY) >= 50);
    if (e.ctrlKey || e.metaKey || wheel) zoomAt(Math.exp(-d * (e.ctrlKey ? 0.01 : 0.002)), ...at(e));
    else panBy(-e.deltaX, -e.deltaY);
  }, {passive: false});
  const pointers = new Map();
  let pinch = null, lastTap = 0;
  const pinchState = () => {
    const [a, b] = [...pointers.values()];
    return {d: Math.hypot(a[0] - b[0], a[1] - b[1]) || 1, m: [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]};
  };
  $.stage.addEventListener('pointerdown', e => {
    if (!opened || e.button > 0 || $.tip.contains(e.target)) return;
    try { $.stage.setPointerCapture(e.pointerId); } catch { /* pointer already gone */ }
    pointers.set(e.pointerId, at(e));
    if (pointers.size === 2) pinch = pinchState();
    if (pointers.size === 1) {
      const now = performance.now();
      if (now - lastTap < 300) { lastTap = 0; zoomAt(zoom > 1.001 ? 1 / zoom : maxZoom() / zoom, ...at(e)); }
      else lastTap = now;
    }
  });
  $.stage.addEventListener('pointermove', e => {
    const prev = pointers.get(e.pointerId);
    if (!prev) return;
    const p = at(e);
    if (pointers.size === 1) { pointers.set(e.pointerId, p); panBy(p[0] - prev[0], p[1] - prev[1]); if (Math.hypot(p[0] - prev[0], p[1] - prev[1]) > 2) lastTap = 0; return; }
    pointers.set(e.pointerId, p);
    if (pointers.size !== 2 || !pinch) return;
    const next = pinchState();
    panBy(next.m[0] - pinch.m[0], next.m[1] - pinch.m[1]);
    zoomAt(next.d / pinch.d, ...next.m);
    pinch = next; lastTap = 0;
  });
  const lift = e => { pointers.delete(e.pointerId); pinch = pointers.size === 2 ? pinchState() : null; };
  $.stage.addEventListener('pointerup', lift);
  $.stage.addEventListener('pointercancel', lift);
  // Safari on macOS reports trackpad pinches as gesture events.
  let gestureZoom = 1;
  $.stage.addEventListener('gesturestart', e => { e.preventDefault(); gestureZoom = 1; });
  $.stage.addEventListener('gesturechange', e => {
    e.preventDefault();
    if (pointers.size >= 2) return;   // touch pinch, handled above
    zoomAt(e.scale / gestureZoom, ...at(e)); gestureZoom = e.scale;
  });
  $.stage.addEventListener('gestureend', e => e.preventDefault());

  const stop = e => { e.preventDefault(); e.stopPropagation(); };
  ui.addEventListener('dragover', e => { stop(e); ui.classList.add('dragging'); });
  ui.addEventListener('dragleave', e => { if (e.target === ui || !ui.contains(e.relatedTarget)) ui.classList.remove('dragging'); });
  ui.addEventListener('drop', e => { stop(e); ui.classList.remove('dragging'); openFile(e.dataTransfer?.files?.[0]); });
  let resizeTimer;
  const observer = new ResizeObserver(() => {
    if (opened) { clampView(); place(); }
    clearTimeout(resizeTimer); resizeTimer = setTimeout(() => opened && requestView(), 120);
  });
  observer.observe($.stage);
  setBusy(null);
  showSwap();

  return {
    release,
    busy: () => opened || !!busyJob,
    dispose() { observer.disconnect(); worker.terminate(); hdr?.device.destroy(); ui.remove(); css.remove(); }
  };
}
