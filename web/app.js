// IR Converter user interface. All processing happens in worker.js / engine.wasm.
const TEXT = {
  de: {
    open: 'Öffnen', drop: 'NEF- oder DNG-Datei öffnen oder hier ablegen',
    hint: 'Alles wird lokal auf diesem Gerät berechnet.',
    swap: 'Rot/Blau tauschen', temp: 'Farbtemperatur', asShot: 'Wie Aufnahme',
    saveDng: 'DNG speichern', saveJpg: 'JPG speichern',
    opening: 'Öffne …', savingDng: 'Erzeuge DNG …', savingJpg: 'Erzeuge JPG …', saved: 'Gespeichert: ',
    asShotIs: 'Aufnahme: ', invalid: 'Bitte eine Temperatur größer als 0 eingeben.',
    warnCamera: 'Kamera nicht in der Tabelle – Farben nur angenähert.', warnWb: 'Kein Weißabgleich in der Datei – Startwert geschätzt.',
    errors: {1: 'Nicht genug Speicher für dieses Bild.', 2: 'Keine NEF- oder DNG-Datei.', 4: 'Die Datei ist beschädigt oder unvollständig.',
      5: 'Kein Bild geöffnet.', '-1': 'Interner Fehler: '},
    unsupported: {0: 'Dieses Dateiformat wird nicht unterstützt.',
      1: 'Diese Kompression wird nicht unterstützt (z. B. verlustbehaftetes oder JPEG-XL-DNG, Nikon „High Efficiency“).',
      2: 'Bereits entwickelte (lineare) DNGs werden nicht unterstützt.', 3: 'Nur Bayer-Sensoren (RGB 2×2) werden unterstützt.',
      4: 'Dieser Bildaufbau wird nicht unterstützt.', 5: 'Die Datei enthält kein RAW-Bild.'}
  },
  en: {
    open: 'Open', drop: 'Open or drop a NEF or DNG file',
    hint: 'Everything is computed locally on this device.',
    swap: 'Swap red/blue', temp: 'Temperature', asShot: 'As shot',
    saveDng: 'Save DNG', saveJpg: 'Save JPG',
    opening: 'Opening …', savingDng: 'Creating DNG …', savingJpg: 'Creating JPG …', saved: 'Saved: ',
    asShotIs: 'As shot: ', invalid: 'Please enter a temperature above 0.',
    warnCamera: 'Camera not in the table – colours are approximate.', warnWb: 'No white balance in the file – start value estimated.',
    errors: {1: 'Not enough memory for this image.', 2: 'Not a NEF or DNG file.', 4: 'The file is damaged or incomplete.',
      5: 'No image open.', '-1': 'Internal error: '},
    unsupported: {0: 'This file format is not supported.',
      1: 'This compression is not supported (e.g. lossy or JPEG XL DNG, Nikon “High Efficiency”).',
      2: 'Already developed (linear) DNGs are not supported.', 3: 'Only Bayer sensors (RGB 2×2) are supported.',
      4: 'This image layout is not supported.', 5: 'The file contains no raw image.'}
  }
};
const SPAN = 2;   // slider covers 2^-SPAN .. 2^SPAN around its centre

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
      <div class="busy" data-ref="busy" hidden><span class="spinner"></span><span data-ref="busyText"></span></div>
    </section>
    <footer class="panel">
      <div class="row">
        <label class="switch"><input data-ref="swap" type="checkbox" disabled><span class="track"><span class="knob"></span></span><span>${t.swap}</span></label>
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
    const off = !!text || !opened;
    for (const el of [$.swap, $.slider, $.kelvin, $.reset, $.dng, $.jpg]) el.disabled = off;
    $.file.disabled = !!text;
  }
  function errorText(data) {
    if (data.code === 3) return t.unsupported[data.detail] || t.unsupported[0];
    if (data.code === -1) return t.errors['-1'] + (data.message || '');
    return t.errors[data.code] || t.errors[2];
  }
  function applyInfo(info) {
    $.swap.checked = !!info.swap;
    center = current = info.temperature;
    $.slider.value = 0;
    $.kelvin.value = fmt(info.temperature);
    $.asshot.textContent = t.asShotIs + fmt(info.asShot) + ' K';
  }
  function requestView() {
    const r = $.stage.getBoundingClientRect(), dpr = Math.min(window.devicePixelRatio || 1, 3);
    worker.postMessage({type: 'view', width: Math.round(r.width * dpr), height: Math.round(r.height * dpr), hdr: !!hdr});
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
  $.swap.addEventListener('change', () => worker.postMessage({type: 'swap', on: $.swap.checked}));
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
  const stop = e => { e.preventDefault(); e.stopPropagation(); };
  ui.addEventListener('dragover', e => { stop(e); ui.classList.add('dragging'); });
  ui.addEventListener('dragleave', e => { if (e.target === ui || !ui.contains(e.relatedTarget)) ui.classList.remove('dragging'); });
  ui.addEventListener('drop', e => { stop(e); ui.classList.remove('dragging'); openFile(e.dataTransfer?.files?.[0]); });
  let resizeTimer;
  const observer = new ResizeObserver(() => { clearTimeout(resizeTimer); resizeTimer = setTimeout(() => opened && requestView(), 120); });
  observer.observe($.stage);
  setBusy(null);

  return {
    release,
    busy: () => opened || !!busyJob,
    dispose() { observer.disconnect(); worker.terminate(); hdr?.device.destroy(); ui.remove(); css.remove(); }
  };
}
