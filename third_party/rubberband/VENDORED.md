# Vendored: Rubber Band Library 4.0.0

- Upstream: Breakfast Quay (Particular Programs Ltd), https://breakfastquay.com/rubberband/
- Source archive: https://github.com/breakfastquay/rubberband/archive/refs/tags/v4.0.0.tar.gz
  (the project's official GitHub mirror of the SourceHut repository)
- SHA-256 of the archive: `24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266`
  (downloaded twice on 2026-11-10, identical hashes; also recorded in
  `cmake/DependencyHashes.cmake`)
- Licence: GPL-2.0-or-later — see `COPYING` in this directory and
  `licenses/RubberBand-COPYING.txt`. No commercial licence is needed:
  the host is AGPL-3.0 and AGPLv3 section 13 permits the combination.
- Contents: the library sources listed by the upstream `meson.build`
  (`library_sources`), minus the C API (`src/rubberband-c.cpp`), the JNI
  bindings, the CLI, the Vamp/LADSPA wrappers and the tests, plus the
  public headers and the upstream `README.md`. `src/common/VectorOpsComplex.h`
  is kept (headers in `src/finer` and `src/common/FFT.cpp` include it);
  its `.cpp` is not part of the upstream library build and is not vendored.
- Build configuration (see the `rubberband` target in the top-level
  CMakeLists.txt): the built-in FFT (`USE_BUILTIN_FFT`) and the built-in
  BQ resampler (`USE_BQRESAMPLER`) on every platform. This keeps the
  FFTW3 GPL/commercial layer (and KissFFT/SLEEF/libsamplerate/Speex)
  out of the build entirely, matching the "auto" defaults of the
  upstream meson build, which also fall back to the built-ins.
