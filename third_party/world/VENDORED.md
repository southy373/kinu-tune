# Vendored: WORLD 0.3.x (speech analysis/synthesis system)

- Upstream: Masanori Morise, https://github.com/mmorise/World
- Pinned commit: `c794856b537758a3d300757db2e5e31685a822c2`
  (fetched 2026-11-14; the tree in this directory is that commit's
  `src/*.cpp` and `src/world/*.h`, unmodified, placed here as
  `src/*.cpp` and `include/world/*.h`)
- Licence: modified BSD (3-clause, no endorsement) — see `LICENSE.txt`
  in this directory and `licenses/WORLD-LICENSE.txt`.
- Contents: the full library sources (`cheaptrick`, `codec`, `common`,
  `d4c`, `dio`, `fft`, `harvest`, `matlabfunctions`, `stonemask`,
  `synthesis`, `synthesisrealtime`) and the public headers under
  `include/world/`. Examples, tools and tests are not vendored.
- Why: the SiFi-GAN front-end (Source/Audio/SiFiGANFeatures) needs the
  exact analysis the model's training-time extraction used — Harvest
  (F0), CheapTrick (spectral envelope) and D4C + CodeAperiodicity
  (aperiodicity) — which pyworld wraps from this library. Using the
  original C++ sources keeps the plugin's numbers aligned with the
  training pipeline instead of reimplementing the algorithms.
- Build configuration (see the `world` target in the top-level
  CMakeLists.txt): a plain static library, no options; WORLD selects
  its FFT size from the sampling rate at run time.
- Intake note: dev-rules section 14's scan/record flow was completed
  on 2026-11-14 before this code was pushed. The intake skill files
  and `scan_external_code.sh` were not present in the working
  environment, so the scan was done manually following section 14's
  text: every vendored file was verified byte-identical against the
  upstream tarball of the pinned commit, and the drop was checked for
  automatically running mechanisms (none - sources only, no process,
  network or file I/O calls). Judgement (3-stage): safe. Records:
  `verify-map.md` (kept outside the repository) and
  `docs/source-ledger.md`.
