/* WebAssembly interface of the IR Converter engine. Every function marked EXPORT
 * is exported by the build (see build.sh). Pointers are linear-memory offsets. */
#include "../../core/irconv.c"

#define EXPORT

EXPORT uint32_t abi_version(void) { return 2; }
EXPORT uint32_t mem_alloc(uint32_t size) { return (uint32_t)(uintptr_t)xalloc(size); }
EXPORT void mem_release(uint32_t ptr) { xfree((void *)(uintptr_t)ptr); }

/* Takes ownership of the buffer on success; frees it on failure. */
EXPORT int image_open(uint32_t ptr, uint32_t len) {
    int r = engine_open((uint8_t *)(uintptr_t)ptr, len);
    if (r) xfree((void *)(uintptr_t)ptr);
    return r;
}
EXPORT int image_error_detail(void) { return S.error_detail; }
EXPORT void image_close(void) { session_reset(); }
EXPORT int image_kind(void) { return S.raw.kind; }
EXPORT uint32_t image_width(void) { return S.raw.data ? oriented_w() : 0; }
EXPORT uint32_t image_height(void) { return S.raw.data ? oriented_h() : 0; }
EXPORT uint32_t image_camera(void) { return (uint32_t)(uintptr_t)S.camera; }
EXPORT int image_warnings(void) { return S.raw.warnings; }
EXPORT int image_name_set(uint32_t ptr, uint32_t len) {
    uint32_t n = MIN(len, (uint32_t)sizeof S.name - 1);
    xmemcpy(S.name, (const void *)(uintptr_t)ptr, n); S.name[n] = 0;
    return 0;
}
EXPORT int image_swap_set(int on) { return engine_set_swap(on); }
EXPORT int image_swap(void) { return S.swap; }
EXPORT int image_temperature_set(double t) { return engine_set_temperature(t); }
EXPORT double image_temperature(void) { return S.temp; }
EXPORT double image_as_shot_temperature(void) { return S.t0; }
EXPORT int image_white_reset(void) { return engine_reset_white(); }

EXPORT int view_render(uint32_t maxw, uint32_t maxh) { return engine_render_view(maxw, maxh, 0); }
/* Display P3, extended range: RGBA half floats (8 bytes per pixel). */
EXPORT int view_render_hdr(uint32_t maxw, uint32_t maxh) { return engine_render_view(maxw, maxh, 1); }
EXPORT uint32_t view_address(void) { return (uint32_t)(uintptr_t)S.view; }
EXPORT uint32_t view_width(void) { return S.view_w; }
EXPORT uint32_t view_height(void) { return S.view_h; }

EXPORT int export_dng(void) { return engine_export_dng(); }
EXPORT int export_jpeg(void) { return engine_export_jpeg(); }
EXPORT uint32_t export_address(void) { return (uint32_t)(uintptr_t)S.out.data; }
EXPORT uint32_t export_size(void) { return S.out.len; }
EXPORT void export_release(void) { buf_free(&S.out); }
