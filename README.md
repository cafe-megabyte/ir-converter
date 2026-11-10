# IR Converter

A raw converter for **infrared photography**, running entirely in the browser.
It reads **NEF** (Nikon) and **DNG** (Bayer and linear), swaps red and blue either
after development (like a channel mixer) or directly in the raw data, offers a white
balance **without any temperature limit** with a live preview, and saves **lossless DNG**
and **JPEG**. Everything is computed locally in C/WebAssembly; there is
no server interaction. After the first visit the app works offline and can be installed
as an app; installed copies update themselves.

<p>
<img src="docs/as-shot.jpg" width="49%" alt="Infrared photo of two palms by the sea, as shot: brownish sky, white palms">
<img src="docs/swap-6500k.jpg" width="49%" alt="The same photo with red and blue swapped at 6500 K: dark blue sky and sea, white palms">
</p>

*Nikon D90, rendered by IR Converter. Left: as shot (1767 K). Right: red/blue
swapped in the raw data, 6500 K.*

## Why

Infrared photos need two things that common raw converters handle poorly:

- **Red/blue channel swap** for the classic look (blue sky, white foliage).
- **Extreme white balance.** Adobe Camera Raw stops at 2000 K, so the usual workaround
  is a custom `.dcp` camera profile that shifts the temperature scale.

IR Converter does both and writes the result into a standard DNG that Lightroom and Camera Raw
open as shot. The workaround profile is created automatically.

## Use

- **Open** a file, or drop it onto the window.
- **Red/blue swap:** *No swap*, *Swap in raw* (the default; before white balance and
  colour matrix) or *Swap after development*, which exchanges red and blue of the developed
  image in **sRGB**, **Display P3** or **Adobe RGB**. That space starts as the camera's
  colour space setting (Adobe RGB if the camera was set to it, otherwise sRGB), and the
  JPG is saved in it. The **i** button next to it explains the chosen mode. Both swaps
  use the same temperature scale, so switching between them keeps the white balance.
  Switching never moves or resizes the image, so modes can be compared directly.
  DNGs written by IR Converter already contain their swap and open with *No swap*.
- **Zoom:** pinch with two fingers or on the trackpad, or use the mouse wheel; drag to
  pan. A double click or double tap switches between the whole image and the largest
  zoom (one image pixel per CSS pixel, at least two screen pixels). Zooming only affects
  the preview; DNG and JPG always contain the whole image.
- **Temperature:** the slider covers ×¼ to ×4 of the current value and re-centres
  whenever you release it, so it has no end stop. The field next to it accepts any value
  above 0 K, for example `350`, `0.5`, `120k`. **As shot** restores the value from the file.
- **Save DNG / Save JPG** writes `<name>-IR.dng` or `.jpg`. On touch devices the share
  sheet opens instead (for example “Save to Files”).

The interface is in German or English, following the browser language.

## Build

Requires macOS with Apple's Command Line Tools (`xcode-select --install`), nothing else.
Builds run offline in a sandbox that blocks the network and only allows writes below `build/`.

```sh
make                    # builds the WASM linker, engine.wasm and the website in build/web/
make serve              # http://localhost:8080/ (Ctrl-C to stop)
make check              # native checks, WASM cross-check, comparison with Apple's raw engine
make clean              # removes build/
make rebuild-toolchain  # rebuilds the linker
```

Apple Clang compiles C to `wasm32`. The only other tool is the small WebAssembly linker
`wcc` from [XCC](https://github.com/tyfkda/xcc) (MIT). It is bundled as an unmodified
source archive and built at the start of the first `make`; see
[vendor/SOURCE_ARCHIVES.md](vendor/SOURCE_ARCHIVES.md). No Node, Emscripten, CMake or Rust.

`make check` uses one sample photo, `samples/DSC_0080.NEF` (Nikon D90), which is not
part of the repository. Without it, only the checks that need no sample file run. The
DNG checks use the NEF's own DNG export as their source.

## Deploy

Copy all of `build/web/` to a static HTTPS host, including `releases/`. Subdirectories
work. Serve `.wasm` as `application/wasm`. Revalidate `index.html`, `service-worker.js`,
`site.webmanifest`, `favicon.ico` and `apple-touch-icon.png`; everything under
`releases/` can be cached forever.

The service worker downloads a new release completely before activating it. Without an
open image the app switches to the new release at once; with an image open, it switches
on the next start. Older releases are then deleted. The release id is shown on the start
screen.

## How it works

### Red/blue swap in the raw data
On a Bayer sensor the CFA pattern is rewritten (e.g. GBRG → GRBG), so red photosites
become blue and vice versa. Sensor data stays bit-exact, every raw converter honours it,
and the swap precedes white balance and colour matrices. Hues therefore differ slightly
from a channel swap after development. A linear DNG has no CFA pattern: its samples stay
as they are, and the exchange goes into the profile (see below).

### Red/blue swap after development
The image is developed normally and red and blue of the result are exchanged in the
chosen output space, like Photoshop's channel mixer in a document of that space. The
result depends on the space, because “red” and “blue” are that space's primaries; neutral
tones are the same in all of them. sRGB stays close to the raw swap; wider spaces shift
blues towards cyan. The primaries come from the embedded macOS ICC profiles.

Internally both swaps hold the photosites exchanged and share the temperature scale.
After development, the colour matrix is the one of normal development for the physical
white balance, followed by the exchange, which is a linear map on XYZ.

In the DNG the raw data stays unswapped and bit-exact. The exchange is folded into the
embedded profile, in both `ForwardMatrix` (which Adobe renders with) and `ColorMatrix`
(which Apple's raw engine renders with), so both show the swapped colours. Camera Raw and
Lightroom render with their own tone handling, so colours there are close but not
identical, and their temperature slider then acts on unswapped data, i.e. in the
opposite visual direction.

### Unbounded temperature
Between 1000 K and 15000 K the scale is physical: the Planckian locus after Krystek (1985)
and camera matrices per the DNG specification. A real Planckian locus ends at both sides
(T → 0 and T → ∞ give finite white points), which is not enough for infrared. With the
swap, the D90's as-shot white lies beyond the blue end. Outside that range the camera
neutral therefore continues log-linearly in log(T). Every value above 0 K is distinct and
monotonic. The start value reproduces the file's white balance exactly; the tint is kept.

### Lossless DNG export
- **Raw data stays bit-exact.** DNG tiles (Bayer or linear) are copied unchanged. NEF
  data is decompressed and stored as lossless JPEG, with the NEF curve as
  `LinearizationTable`.
- `AsShotNeutral` holds the chosen white balance.
- **Embedded profile “IR Converter”** (the `.dcp` trick, automated):
  - `ColorMatrix` is scaled so this white point reads about 5000 K in Camera Raw and
    Lightroom, so their slider starts mid-range.
  - `ForwardMatrix` reproduces the app's colours exactly according to the DNG
    specification; reopened exports render the same in every swap mode.
  - The XMP sets `crd:CameraProfile="IR Converter"` and drops stored WB/profile settings.
- **Previews:** every embedded preview is re-rendered at its original size. None is
  added if there was none. NEF imports keep the NEF's preview sizes.
- **Fast-load proxies** (DNG 1.7, JPEG XL) hold camera RGB and follow the new white
  balance and profile on their own. They are kept, except with the swap in the raw data
  of a Bayer image, where they are removed.
- The Nikon MakerNote is kept as `DNGPrivateData`; EXIF and GPS are preserved.

### JPEG export
Full resolution (Malvar-He-Cutler demosaicing; linear DNGs are already demosaiced),
quality 75, 4:2:0, EXIF preserved, orientation as EXIF tag. Colour space: sRGB, or with
the swap after development the chosen space (sRGB, Display P3 or Adobe RGB), always with
the unmodified macOS ICC profile embedded. The tone curve is the same in every space.

The JPEG is also an **HDR photo with a gain map**. The main image is the ordinary SDR
JPEG, so every viewer shows it as before. Behind it, in an MPF container, follows a
greyscale gain map at half resolution that brightens highlights exactly like the HDR
preview (up to 3×). It is described twice:
- **ISO 21496-1**: Apple Photos/Preview (macOS 15, iOS 18 and later), Chrome.
- **Ultra HDR XMP** (Adobe gain map): Android, Chrome.

Viewers use as much of the gain as the display's headroom allows.

### Preview and zoom
The whole image is previewed from 2×2 superpixels (at most half resolution). A zoomed
view renders only the visible rectangle: down to half size again from superpixels,
larger with the JPEG's demosaicing, so at 1:1 it equals the JPEG pixel for pixel. That
demosaicing works on white-balanced data and is repeated for every change.

### HDR preview
On HDR displays the preview uses an extended-range WebGPU canvas
(`rgba16float`, Display P3, `toneMapping: extended`). Midtones match the SDR rendering;
highlights brighten smoothly up to 3× SDR white, and the display clips beyond its own
headroom. Browsers or displays without this support show the SDR preview. The header
shows “HDR” when it is active.

### Supported files
- **NEF:** lossy and lossless compressed, 12/14 bit, uncompressed.
  - Colour matrices and crop come from `core/cameras.c`; currently only the **Nikon D90**.
  - To add a model, convert one NEF with Adobe DNG Converter and copy `ColorMatrix1/2`,
    `ForwardMatrix1/2`, `DefaultCrop*`, `ActiveArea`, `WhiteLevel` and
    `BaselineExposure`.
  - Unknown models open with a warning and approximate colours.
- **DNG:** Bayer CFA or linear (demosaiced, three samples of camera RGB), uncompressed
  or lossless JPEG, tiles or strips.
- **Not supported** (with a message): lossy or JPEG XL compressed raw data in DNG,
  linear DNGs with other than three samples, Nikon “High Efficiency” (TicoRAW),
  non-Bayer sensors.

NEF raw values are bit-identical to Adobe DNG Converter's (compared once for the
sample). The NEF curve is interpolated as in dcraw/LibRaw. Adobe interpolates slightly
differently, which shifts tonal values by up to about 1 %; the raw values themselves
are the same.

## Checks

- `tests/core-check.c` covers:
  - temperature round trips from 10 K to 10⁶ K, and monotonicity from 1 K to 10⁹ K;
  - DNG export from the NEF and from a DNG (the NEF's own export) in every swap mode:
    bit-exact raw data, NEF curve, CFA pattern, neutral, preview sizes, and that a
    reopened export renders the same;
  - the transfer curves of the embedded ICC profiles and the red/blue exchange per space;
  - JPEG export in every output space (embedded profile) and the HDR preview;
  - zoomed views: at 1:1 equal to the JPEG pipeline in all eight orientations.

  Not covered automatically: linear DNGs, fast-load proxies and DNGs from other
  converters. The NEF decoder was compared once with Adobe DNG Converter (see below).
- `tests/wasm-check.js` runs the same steps with `engine.wasm` in JavaScriptCore. DNG,
  JPEG and the previews (including zoomed ones) must be byte-identical to the native build.
- `raw-reference` opens the exported DNGs with Apple's raw engine (ImageIO) and compares
  their colours with IR Converter's, including both swaps. For the JPEGs it checks:
  - the expected ICC profile;
  - the MPF index and the Ultra HDR XMP;
  - that ImageIO finds the ISO gain map;
  - that Core Image's HDR decode equals the SDR image times a gain between 1 and 3.

## Layout

- `core/`: the engine in freestanding C11 (TIFF, NEF, DNG, lossless JPEG, colour,
  rendering, JPEG, export), built natively and as WebAssembly.
- `web/`: user interface, worker, service worker and the WASM adapter (`native/`).
- `build-support/`: build scripts, XCC patch and small Objective-C host tools.
- `assets/`: `icon.png` (transparent, so iOS can place it on a light or dark background)
  and `icon-maskable.png` (full bleed). `build-support/tools/icon-art.m` draws both.
- `docs/`: README pictures.
- `tests/`, `vendor/`.

Proudly generated with Claude
