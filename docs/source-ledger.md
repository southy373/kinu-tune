# Source ledger

Licence and provenance record for third-party code and models taken into
this repository, as required by dev-rules section 14. The user-facing
summary lives in `THIRD_PARTY_NOTICES.txt`; this file is the intake
record (where each item came from, how it is pinned, and the intake
judgement).

## WORLD (speech analysis/synthesis system)

- What: C++ DSP library - Harvest (F0 estimation), CheapTrick (spectral
  envelope), D4C + CodeAperiodicity (aperiodicity). Used by the SiFi-GAN
  front-end (`Source/Audio/SiFiGANFeatures`) to compute the same
  features the SiFi-GAN model was trained with.
- Source: https://github.com/mmorise/World (Masanori Morise)
- Pinned: commit `c794856b537758a3d300757db2e5e31685a822c2`, vendored
  unmodified under `third_party/world/` (`src/*.cpp` from upstream
  `src/`, headers from upstream `src/world/` placed under
  `include/world/`). Examples, tools, tests and build files of the
  upstream repository were not vendored.
- Licence: modified BSD (3-clause, no endorsement). Full text:
  `licenses/WORLD-LICENSE.txt` and `third_party/world/LICENSE.txt`.
- Intake check (2026-11-14, dev-rules section 14): the vendored files
  were compared byte for byte against the upstream tarball of the
  pinned commit - all 25 files identical. Manual scan in place of
  `scan_external_code.sh` (the script and the intake skill files were
  not present in the working environment): the drop contains only
  `.cpp` / `.h` sources plus licence and notes - no build scripts, no
  shell or batch files, no prebuilt binaries, so nothing in it runs
  automatically. The sources contain no process execution, no network
  access and no file I/O; they are pure in-memory DSP.
- Judgement (3-stage): **safe** (stage 1 of 3). No caution items.

## SiFi-GAN model (sifigan_115k_gather.onnx + sifigan_kokoro_stats.json)

- What: neural vocoder generator used by the Plan B AI resynthesis
  engine (`Source/Audio/SiFiGANVocoder`). Placed in
  `Resources/models/` like the FCPE model; the installer copies it to
  the models folder with the other models.
- Provenance: trained by this project itself (LibriTTS-R, CC BY 4.0,
  then the Kokoro Speech Dataset, public domain) using the SiFi-GAN
  architecture (https://github.com/chomeyama/SiFi-GAN, MIT License,
  Copyright 2022 Reo Yoneyama). The weights are the project's own
  training output; the ONNX conversion is the project's own work. No
  third-party weights are included.
- Pinning: SHA-256 of the shipped files -
  `sifigan_115k_gather.onnx`:
  `62021642ef5f25bcc70526260fb35b8d5e8f95eb0a32e3374baf3591afcecd51`
  `sifigan_kokoro_stats.json`:
  `4ac9d1353c97cdebab795bbbffe3796a1cfd1537b12e31fdeb6a69d92200a516`
- Judgement (3-stage): **safe** (stage 1 of 3) - the project's own
  artifact, produced from the training pipeline recorded in the
  project's notes.
