#!/usr/bin/env python3
"""Convert the official BigVGAN v2 checkpoint to the ONNX model the plugin loads.

Downloads bigvgan_generator.pt + config.json from the official Hugging Face
repository (nvidia/bigvgan_v2_44khz_128band_512x, pinned revision), loads the
generator with NVIDIA's BigVGAN code (pinned commit), removes weight norm and
exports with the Dynamo exporter to:

    Resources/models/bigvgan_v2_44khz_128band.onnx

The output is a single ~490 MB file, so it is never committed (see
.gitignore); CI runs this script before configuring, and local development
can run it once by hand. If the output file already exists the script does
nothing.

Model and code are MIT licensed (NVIDIA/BigVGAN, checkpoint metadata on
Hugging Face), which is why this vocoder replaces PC-NSF-HiFiGAN
(CC BY-NC-SA 4.0) in the speech build.

Requirements: torch (CPU), onnx, onnxruntime, huggingface_hub, git.
Verified with torch 2.15.0+cpu / onnxruntime 1.19.2 (the ONNX Runtime
version the plugin ships); the exported graph is opset 20 / IR 10 and the
script smoke-tests it with ONNX Runtime before finishing, including a
dynamic-length check.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HF_REPO = "nvidia/bigvgan_v2_44khz_128band_512x"
HF_REVISION = "95a9d1dcb12906c03edd938d77b9333d6ded7dfb"
GENERATOR_SHA256 = (
    "d9fe7ec6bd0b44ed9d66973d5012d8181c1570b01e5c72df51973e241dccd357"
)
BIGVGAN_REPO_URL = "https://github.com/NVIDIA/BigVGAN"
BIGVGAN_COMMIT = "7d2b454564a6c7d014227f635b7423881f14bdac"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT_PATH = os.path.join(
    REPO_ROOT, "Resources", "models", "bigvgan_v2_44khz_128band.onnx"
)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def clone_bigvgan_code(dest):
    subprocess.run(
        ["git", "clone", "--quiet", BIGVGAN_REPO_URL, dest], check=True
    )
    subprocess.run(
        ["git", "-C", dest, "checkout", "--quiet", BIGVGAN_COMMIT], check=True
    )


def main():
    if os.path.exists(OUTPUT_PATH):
        print(f"BigVGAN model already present, skipping: {OUTPUT_PATH}")
        return 0

    # Plain HTTPS downloads only: the Xet backend has been seen to stall
    # indefinitely on some networks, and 490 MB over HTTPS is fast enough.
    os.environ.setdefault("HF_HUB_DISABLE_XET", "1")

    from huggingface_hub import hf_hub_download

    # Work next to the output, not in the system temp dir: the downloads
    # and intermediate files are ~1.5 GB in total, which does not fit in
    # the small tmpfs some systems mount as /tmp. Everything under
    # Resources/models is gitignored, including this scratch directory.
    models_dir = os.path.dirname(OUTPUT_PATH)
    os.makedirs(models_dir, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix=".convert-", dir=models_dir)
    try:
        print(f"Downloading checkpoint from {HF_REPO}@{HF_REVISION} ...")
        generator_pt = hf_hub_download(
            HF_REPO, "bigvgan_generator.pt", revision=HF_REVISION,
            local_dir=workdir,
        )
        config_json = hf_hub_download(
            HF_REPO, "config.json", revision=HF_REVISION, local_dir=workdir
        )

        actual = sha256_of(generator_pt)
        if actual != GENERATOR_SHA256:
            print(
                f"error: generator checkpoint sha256 mismatch\n"
                f"  expected {GENERATOR_SHA256}\n  actual   {actual}",
                file=sys.stderr,
            )
            return 1

        code_dir = os.path.join(workdir, "BigVGAN")
        print(f"Cloning NVIDIA/BigVGAN@{BIGVGAN_COMMIT} ...")
        clone_bigvgan_code(code_dir)
        sys.path.insert(0, code_dir)

        import torch
        from torch.export import Dim

        from bigvgan import BigVGAN
        from env import AttrDict

        with open(config_json) as f:
            config = AttrDict(json.load(f))

        model = BigVGAN(config, use_cuda_kernel=False)
        checkpoint = torch.load(
            generator_pt, map_location="cpu", weights_only=False
        )
        model.load_state_dict(checkpoint["generator"])
        model.eval()
        model.remove_weight_norm()

        # Any in-range dummy mel works: only the time axis is dynamic.
        torch.manual_seed(0)
        dummy_mel = torch.rand(1, config.num_mels, 86) * 8.0 - 10.0

        export_dir = os.path.join(workdir, "export")
        os.makedirs(export_dir)
        exported = os.path.join(export_dir, "bigvgan.onnx")
        print("Exporting ONNX (Dynamo exporter) ...")
        with torch.no_grad():
            torch.onnx.export(
                model,
                (dummy_mel,),
                exported,
                input_names=["mel"],
                output_names=["waveform"],
                # Keyed by BigVGAN.forward's parameter name; the tuple form
                # makes the exporter pass a spurious extra argument.
                dynamic_shapes={"x": {0: Dim("batch"), 2: Dim("frames")}},
                dynamo=True,
            )

        # The Dynamo exporter spills the weights to a sidecar .data file;
        # the plugin loads a single file, so inline them (the model is
        # ~490 MB, well under the protobuf size limit).
        import onnx

        tmp_output = os.path.join(workdir, "bigvgan_single.onnx")
        print("Inlining weights into a single file ...")
        onnx.save_model(onnx.load(exported), tmp_output)
        shutil.rmtree(export_dir)

        # Smoke-test with ONNX Runtime: shape contract in, and the dynamic
        # time axis must scale the output by exactly the hop size.
        import numpy as np
        import onnxruntime as ort

        session = ort.InferenceSession(
            tmp_output, providers=["CPUExecutionProvider"]
        )
        mel_in = np.random.default_rng(0).normal(
            -4.0, 2.0, size=(1, config.num_mels, 43)
        ).astype(np.float32)
        out = session.run(None, {"mel": mel_in})[0]
        expected = 43 * config.hop_size
        if out.shape != (1, 1, expected):
            print(
                f"error: unexpected output shape {out.shape}, "
                f"expected (1, 1, {expected})",
                file=sys.stderr,
            )
            return 1

        os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
        shutil.move(tmp_output, OUTPUT_PATH)
        size_mb = os.path.getsize(OUTPUT_PATH) / 1e6
        print(f"Wrote {OUTPUT_PATH} ({size_mb:.1f} MB)")
        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
