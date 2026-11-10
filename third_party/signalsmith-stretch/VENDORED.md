# Vendored: Signalsmith Stretch 1.4.0 + Signalsmith Linear 0.6.4

- Upstream: Signalsmith Audio Ltd (Geraint Luff),
  https://signalsmith-audio.co.uk/code/stretch/
- Source archive: https://github.com/Signalsmith-Audio/signalsmith-stretch/archive/refs/tags/1.4.0.zip
- SHA-256 of the archive: `758960fbafaa0550d17626b1eb9497d0fd10f80dad00e96590099ef6c20390e6`
  (downloaded twice on 2026-11-10, identical hashes; also recorded in
  `cmake/DependencyHashes.cmake`)
- Licence: MIT — see `LICENSE-signalsmith-stretch.txt` in this directory
  and `licenses/Signalsmith-Stretch-LICENSE.txt`.
- Contents: the single header `signalsmith-stretch.h`, the upstream
  `README.md`, and its dependency below. The CLI, web demo and tests are
  not vendored.

## Dependency: Signalsmith Linear 0.6.4

The stretch header includes `signalsmith-linear/stft.h`. Upstream's own
CMake pins Signalsmith Linear at tag 0.6.4 (its FetchContent GIT_TAG).

- Source archive: https://github.com/Signalsmith-Audio/linear/archive/refs/tags/0.6.4.zip
  (the repository is named "adaptive"; it redirects to "linear")
- SHA-256 of the archive: `eff8c9f8f2573d1f8ef52a93de32c68a70547e5c07cc1ccf62fb41795d651056`
  (downloaded twice on 2026-11-10, identical hashes; also recorded in
  `cmake/DependencyHashes.cmake`)
- Licence: MIT — see `signalsmith-linear/LICENSE.txt` and
  `licenses/Signalsmith-Linear-LICENSE.txt`.
- Contents: the headers `stft.h`, `fft.h`, `linear.h`, `approx.h` and
  the `platform/` headers they can include, under `signalsmith-linear/`
  so the `#include "signalsmith-linear/..."` paths resolve with this
  directory on the include path. No `SIGNALSMITH_USE_*` acceleration
  flags are defined, so the library's own portable FFT is used.
