# Original source archives

`toolchain/xcc/` contains exactly one unmodified upstream source archive. Its
filename is arbitrary; extraction uses the archive's contents.

- Project: [XCC](https://github.com/tyfkda/xcc), MIT license (`vendor/XCC-LICENSE.txt`).
- Original URL: https://codeload.github.com/tyfkda/xcc/tar.gz/refs/heads/main
- Snapshot downloaded: 9 November 2026, 505,682 bytes, filename `source.tar.gz`.
- Patch: `build-support/patches/xcc-host-only.patch` removes the WASI library
  follow-up from the native `wcc` target. Apple Clang compiles our C sources;
  `wcc` is used only as the WebAssembly object linker.

The build never downloads anything. Replacing the archive requires a version
compatible with the patch and the WASM checks.
