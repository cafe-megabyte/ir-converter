// Runs the same operations as tests/core-check.c with engine.wasm and
// requires byte-identical results (DNG, JPEG and the screen view).
(async () => {
  const {instance} = await WebAssembly.instantiate(moduleBytes, {});
  const e = instance.exports;
  const mem = () => new Uint8Array(e.memory.buffer);
  const dir = 'build/internal/tests/';
  function open(path, name) {
    const data = readBytes(path);
    if (!data) throw new Error('Missing ' + path);
    const p = e.mem_alloc(data.length);
    mem().set(data, p);
    const r = e.image_open(p, data.length);
    if (r) throw new Error(`open ${path}: ${r}/${e.image_error_detail()}`);
    if (name) {
      const n = Uint8Array.from(name, ch => ch.charCodeAt(0)), q = e.mem_alloc(n.length);
      mem().set(n, q); e.image_name_set(q, n.length); e.mem_release(q);
    }
  }
  function same(label, expectedPath, ptr, size) {
    const expected = readBytes(dir + expectedPath), actual = mem().subarray(ptr, ptr + size);
    if (!expected) throw new Error('Missing native reference ' + expectedPath);
    if (expected.length !== actual.length) throw new Error(`${label}: size ${actual.length} != ${expected.length}`);
    for (let i = 0; i < actual.length; i++)
      if (actual[i] !== expected[i]) throw new Error(`${label}: first difference at byte ${i}`);
    report(`${label}: identical to native (${size} bytes)`);
  }
  function exported(label, ref, r) {
    if (r) throw new Error(`${label}: error ${r}`);
    same(label, ref, e.export_address(), e.export_size());
    e.export_release();
  }

  // DNG source: the NEF's own plain export, written by core-check.
  open(dir + 'NEF-plain.dng');
  if (e.image_swap() !== 0) throw new Error('own export opens with a swap');
  if (e.image_swap_set(1) || e.image_temperature_set(100000)) throw new Error('settings');
  exported('DNG -> DNG (swap, 100000 K)', 'DNG-swap.dng', e.export_dng());
  exported('JPEG of the same state', 'DNG-swap.jpg', e.export_jpeg());

  open(dir + 'NEF-plain.dng');
  if (e.image_swap_set(2) || e.image_space_set(2) || e.image_temperature_set(60000)) throw new Error('settings');
  exported('DNG -> DNG (swap after development, Adobe RGB, 60000 K)', 'DNG-post.dng', e.export_dng());
  exported('JPEG of the same state (Adobe RGB)', 'DNG-post.jpg', e.export_jpeg());

  open('samples/DSC_0080.NEF', 'DSC_0080.NEF');
  if (e.image_swap() !== 1 || e.image_swap_set(0)) throw new Error('default swap mode');
  exported('NEF -> DNG', 'NEF-plain.dng', e.export_dng());

  open('samples/DSC_0080.NEF');
  e.image_swap_set(1); e.image_temperature_set(30000);
  exported('NEF -> JPEG (swap, 30000 K)', 'NEF-swap.jpg', e.export_jpeg());
  if (e.view_render(1600, 1200)) throw new Error('view');
  same('Screen view', 'view.rgba', e.view_address(), e.view_width() * e.view_height() * 4);
  if (e.view_render_hdr(1600, 1200)) throw new Error('HDR view');
  same('HDR screen view', 'view-hdr.bin', e.view_address(), e.view_width() * e.view_height() * 8);
  if (e.view_render_region(1001.5, 703.25, 1200, 800, 400, 266, 0)) throw new Error('zoomed view');
  same('Zoomed view (superpixels)', 'view-zoom-half.rgba', e.view_address(), e.view_width() * e.view_height() * 4);
  if (e.view_render_region(1001.5, 703.25, 600, 400, 450, 300, 0)) throw new Error('zoomed view');
  same('Zoomed view (demosaiced)', 'view-zoom.rgba', e.view_address(), e.view_width() * e.view_height() * 4);
  if (e.view_render_region(1001.5, 703.25, 600, 400, 450, 300, 1)) throw new Error('zoomed HDR view');
  same('Zoomed HDR view', 'view-zoom-hdr.bin', e.view_address(), e.view_width() * e.view_height() * 8);
  globalThis.testDone = true;
})().catch(error => { report('WASM check failed: ' + error); globalThis.testFailed = true; globalThis.testDone = true; });
