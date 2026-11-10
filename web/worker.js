// Runs the WASM engine off the main thread. Render requests are coalesced:
// while a frame is computed, only the newest slider position is kept.
'use strict';
let wasm, ready;
let viewW = 1024, viewH = 768, viewHdr = false, dirty = false, scheduled = false, open = false;

function bytes() { return new Uint8Array(wasm.memory.buffer); }
function cString(ptr) {
  const b = bytes(); let end = ptr;
  while (b[end]) end++;
  return new TextDecoder().decode(b.subarray(ptr, end));
}
function info() {
  return {
    width: wasm.image_width(), height: wasm.image_height(), kind: wasm.image_kind(),
    camera: cString(wasm.image_camera()), warnings: wasm.image_warnings(), swap: wasm.image_swap(),
    temperature: wasm.image_temperature(), asShot: wasm.image_as_shot_temperature()
  };
}
function schedule() {
  dirty = true;
  if (scheduled) return;
  scheduled = true;
  setTimeout(render, 0);
}
function render() {
  scheduled = false;
  if (!dirty || !open) return;
  dirty = false;
  const hdr = viewHdr;
  const r = hdr ? wasm.view_render_hdr(viewW, viewH) : wasm.view_render(viewW, viewH);
  if (r) { postMessage({type: 'error', code: r, detail: 0}); return; }
  const w = wasm.view_width(), h = wasm.view_height(), ptr = wasm.view_address();
  const pixels = bytes().slice(ptr, ptr + w * h * (hdr ? 8 : 4));
  postMessage({type: 'frame', width: w, height: h, hdr, temperature: wasm.image_temperature(), buffer: pixels.buffer}, [pixels.buffer]);
}

async function init(url) {
  const response = await fetch(url);
  const {instance} = await WebAssembly.instantiate(await response.arrayBuffer(), {});
  wasm = instance.exports;
  if (wasm.abi_version() !== 2) throw new Error('Engine version mismatch');
}

self.onmessage = async ({data}) => {
  try {
    if (data.type === 'init') { ready = init(data.wasm); await ready; postMessage({type: 'ready'}); return; }
    await ready;
    switch (data.type) {
    case 'open': {
      open = false;
      wasm.image_close();
      const src = new Uint8Array(data.buffer);
      const ptr = wasm.mem_alloc(src.length);
      if (!ptr) { postMessage({type: 'error', code: 1, detail: 0}); return; }
      bytes().set(src, ptr);
      const r = wasm.image_open(ptr, src.length);
      if (r) { postMessage({type: 'error', code: r, detail: wasm.image_error_detail()}); return; }
      const name = new TextEncoder().encode(data.name || '');
      const np = wasm.mem_alloc(name.length + 1);
      if (np) { bytes().set(name, np); wasm.image_name_set(np, name.length); wasm.mem_release(np); }
      open = true;
      postMessage({type: 'opened', info: info()});
      schedule();
      break;
    }
    case 'view':
      viewW = Math.max(16, Math.min(2048, data.width | 0)); viewH = Math.max(16, Math.min(2048, data.height | 0));
      viewHdr = !!data.hdr;
      schedule();
      break;
    case 'swap': {
      const r = wasm.image_swap_set(data.on ? 1 : 0);
      if (r) { postMessage({type: 'error', code: r, detail: 0}); return; }
      postMessage({type: 'state', info: info()});
      schedule();
      break;
    }
    case 'temperature':
      if (wasm.image_temperature_set(data.value) === 0) schedule();
      break;
    case 'reset':
      wasm.image_white_reset();
      postMessage({type: 'state', info: info()});
      schedule();
      break;
    case 'export': {
      const r = data.format === 'dng' ? wasm.export_dng() : wasm.export_jpeg();
      if (r) { postMessage({type: 'error', code: r, detail: 0, during: 'export'}); return; }
      const ptr = wasm.export_address(), size = wasm.export_size();
      const out = bytes().slice(ptr, ptr + size);
      wasm.export_release();
      postMessage({type: 'exported', format: data.format, buffer: out.buffer}, [out.buffer]);
      break;
    }
    }
  } catch (error) {
    postMessage({type: 'error', code: -1, detail: 0, message: String(error?.message || error)});
  }
};
