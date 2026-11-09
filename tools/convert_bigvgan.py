#!/usr/bin/env python3
"""Convert the official BigVGAN v2 checkpoint to the ONNX model the plugin loads.

Downloads bigvgan_generator.pt + config.json from the official Hugging Face
repository (nvidia/bigvgan_v2_44khz_128band_512x, pinned revision), loads the
generator with NVIDIA's BigVGAN code (pinned commit), removes weight norm and
exports with the Dynamo exporter to:

    Resources/models/bigvgan_v2_44khz_128band.onnx

and additionally derives a lower-opset variant:

    Resources/models/bigvgan_v2_44khz_128band_opset17.onnx

The variant exists for the DirectML session ladder in BigVGANVocoder:
session creation succeeded on the user's machine while every inference
failed on a ConvTranspose node, and the DirectML execution provider's
opset support has historically lagged the newest exporter defaults
(the main export is opset 20). Neither the legacy TorchScript exporter
nor the Dynamo exporter can target opset 17 for this graph (the legacy
exporter fails on the alias-free activation's Pad -> Conv, and the
Dynamo exporter's automatic downgrade has no version adapter for Pad),
so the variant is produced by re-stamping the exported graph's opset
declaration from 20 to 17. That is only sound because the graph uses no
post-17 operator features - verified before this approach was adopted:
the full ONNX checker passes under the opset 17 schema, and the
re-stamped model's CPU output is bit-identical to the opset 20 model's
(max abs error 0.0; both sit 1.23e-05 from the PyTorch reference). The
derivation below stays fail-closed: it refuses any node outside the
inventoried operator set, any Pad carrying the opset 18 "axes" input,
and any output that drifts from the main model beyond a small
tolerance, so a future checkpoint whose graph changes shape fails the
build loudly instead of shipping a mislabelled model. Only the opset
declaration changes; the IR version stays 10, which the plugin's ONNX
Runtime 1.19.2 reads.

Each output is a single ~490 MB file, so neither is ever committed (see
.gitignore); CI runs this script before configuring, and local
development can run it once by hand. Each output is generated only when
missing - the variant can be derived from an existing main model
without downloading anything.

Model and code are MIT licensed (NVIDIA/BigVGAN, checkpoint metadata on
Hugging Face), which is why this vocoder replaces PC-NSF-HiFiGAN
(CC BY-NC-SA 4.0) in the speech build.

Requirements: torch (CPU), onnx, onnxruntime, huggingface_hub, git.
Verified with torch 2.15.0+cpu / onnxruntime 1.19.2 (the ONNX Runtime
version the plugin ships); the exported graph is opset 20 / IR 10 and
the script smoke-tests both files with ONNX Runtime before finishing,
including a dynamic-length check.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

# Windows CI consoles use a legacy code page (cp1252) that cannot encode
# some characters printed by torch/onnx export logging (e.g. check marks);
# make this process's output encoding-proof before doing anything else.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

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
OUTPUT_PATH_OPSET17 = os.path.join(
    REPO_ROOT, "Resources", "models",
    "bigvgan_v2_44khz_128band_opset17.onnx",
)

# The pinned checkpoint's contract (its config.json), used by the smoke
# tests and by the opset 17 derivation when it runs standalone against
# an already-exported main model.
PINNED_NUM_MELS = 128
PINNED_HOP_SIZE = 512

TARGET_OPSET17 = 17

# Every operator in the exported graph, inventoried from the opset 20
# model. The re-stamp to 17 is sound only while the graph stays inside
# this set (all of these predate opset 17 in the form this graph uses
# them); derive_opset17_variant() refuses anything else.
OPSET17_ALLOWED_OPS = {
    "Add", "Clip", "Conv", "ConvTranspose", "Div", "Mul", "Pad", "Pow",
    "Sin", "Slice",
}

# The re-stamped model's output must match the main model's on the same
# input. Measured value when this was adopted: exactly 0.0 (only the
# opset declaration differs). The tolerance leaves headroom for runtime
# variation while still catching any real divergence.
CROSS_CHECK_TOLERANCE = 1e-4


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


def _graph_violation(graph):
    """First reason this graph cannot be re-stamped to opset 17, or None."""
    for node in graph.node:
        if node.op_type not in OPSET17_ALLOWED_OPS:
            return f"operator {node.op_type!r} is outside the inventoried set"
        # Pad gained an optional "axes" input in opset 18; the form this
        # graph uses (data, pads[, constant_value]) is the pre-18 one.
        if node.op_type == "Pad" and len(node.input) > 2:
            return "a Pad node carries the opset 18 'axes' input"
        for attr in node.attribute:
            if attr.HasField("g"):  # subgraph attribute
                violation = _graph_violation(attr.g)
                if violation is not None:
                    return violation
    return None


def derive_opset17_variant(source_path, dest_path, num_mels, hop_size):
    """Write the opset 17 variant of the model at source_path to dest_path.

    Re-stamps the ai.onnx opset declaration only (see the module
    docstring for why that is sound here), then proves the result:
    fail-closed graph inventory, the full ONNX checker under the opset
    17 schema, an ONNX Runtime smoke test, and a numerical cross-check
    against the source model. Returns True on success.
    """
    import numpy as np
    import onnx
    import onnxruntime as ort

    print(f"Deriving opset {TARGET_OPSET17} variant from {source_path} ...")
    model = onnx.load(source_path)

    current = None
    for op in model.opset_import:
        if (op.domain or "ai.onnx") == "ai.onnx":
            current = op.version
    if current is None or current <= TARGET_OPSET17:
        print(
            f"error: source model opset is {current}, nothing to derive",
            file=sys.stderr,
        )
        return False

    violation = _graph_violation(model.graph)
    if violation is not None:
        print(
            f"error: refusing to re-stamp the graph to opset "
            f"{TARGET_OPSET17}: {violation}",
            file=sys.stderr,
        )
        return False

    for op in model.opset_import:
        if (op.domain or "ai.onnx") == "ai.onnx":
            op.version = TARGET_OPSET17
    onnx.save_model(model, dest_path)
    del model

    try:
        onnx.checker.check_model(onnx.load(dest_path), full_check=True)
    except Exception as e:  # checker raises onnx.checker.ValidationError
        print(f"error: opset 17 model failed the ONNX checker: {e}",
              file=sys.stderr)
        return False

    mel_in = np.random.default_rng(0).normal(
        -4.0, 2.0, size=(1, num_mels, 43)
    ).astype(np.float32)
    variant_session = ort.InferenceSession(
        dest_path, providers=["CPUExecutionProvider"]
    )
    out_variant = variant_session.run(None, {"mel": mel_in})[0]
    expected = 43 * hop_size
    if out_variant.shape != (1, 1, expected):
        print(
            f"error: opset 17 variant produced shape {out_variant.shape}, "
            f"expected (1, 1, {expected})",
            file=sys.stderr,
        )
        return False

    source_session = ort.InferenceSession(
        source_path, providers=["CPUExecutionProvider"]
    )
    out_source = source_session.run(None, {"mel": mel_in})[0]
    max_err = float(np.max(np.abs(out_variant - out_source)))
    print(f"Opset 17 variant vs main model: max abs error {max_err:.3e}")
    if max_err > CROSS_CHECK_TOLERANCE:
        print(
            f"error: opset 17 variant diverges from the main model "
            f"(max abs error {max_err:.3e} > {CROSS_CHECK_TOLERANCE:.1e})",
            file=sys.stderr,
        )
        return False
    return True


def main():
    need_main = not os.path.exists(OUTPUT_PATH)
    need_variant = not os.path.exists(OUTPUT_PATH_OPSET17)
    if not need_main and not need_variant:
        print(f"BigVGAN models already present, skipping:\n"
              f"  {OUTPUT_PATH}\n  {OUTPUT_PATH_OPSET17}")
        return 0

    # Plain HTTPS downloads only: the Xet backend has been seen to stall
    # indefinitely on some networks, and 490 MB over HTTPS is fast enough.
    os.environ.setdefault("HF_HUB_DISABLE_XET", "1")

    # Work next to the output, not in the system temp dir: the downloads
    # and intermediate files are ~1.5 GB in total, which does not fit in
    # the small tmpfs some systems mount as /tmp. Everything under
    # Resources/models is gitignored, including this scratch directory.
    models_dir = os.path.dirname(OUTPUT_PATH)
    os.makedirs(models_dir, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix=".convert-", dir=models_dir)
    try:
        # The standalone derivation path (main model already present)
        # relies on the pinned checkpoint's contract.
        num_mels = PINNED_NUM_MELS
        hop_size = PINNED_HOP_SIZE

        if need_main:
            from huggingface_hub import hf_hub_download

            print(f"Downloading checkpoint from {HF_REPO}@{HF_REVISION} ...")
            generator_pt = hf_hub_download(
                HF_REPO, "bigvgan_generator.pt", revision=HF_REVISION,
                local_dir=workdir,
            )
            config_json = hf_hub_download(
                HF_REPO, "config.json", revision=HF_REVISION,
                local_dir=workdir,
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
            num_mels = config.num_mels
            hop_size = config.hop_size

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
                    # Keyed by BigVGAN.forward's parameter name; the tuple
                    # form makes the exporter pass a spurious extra
                    # argument.
                    dynamic_shapes={"x": {0: Dim("batch"), 2: Dim("frames")}},
                    dynamo=True,
                )

            # The Dynamo exporter spills the weights to a sidecar .data
            # file; the plugin loads a single file, so inline them (the
            # model is ~490 MB, well under the protobuf size limit).
            import onnx

            tmp_output = os.path.join(workdir, "bigvgan_single.onnx")
            print("Inlining weights into a single file ...")
            onnx.save_model(onnx.load(exported), tmp_output)
            shutil.rmtree(export_dir)

            # Smoke-test with ONNX Runtime: shape contract in, and the
            # dynamic time axis must scale the output by exactly the hop
            # size.
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

            shutil.move(tmp_output, OUTPUT_PATH)
            size_mb = os.path.getsize(OUTPUT_PATH) / 1e6
            print(f"Wrote {OUTPUT_PATH} ({size_mb:.1f} MB)")

        if need_variant:
            variant_tmp = os.path.join(workdir, "bigvgan_opset17.onnx")
            if not derive_opset17_variant(
                OUTPUT_PATH, variant_tmp, num_mels, hop_size
            ):
                return 1
            shutil.move(variant_tmp, OUTPUT_PATH_OPSET17)
            size_mb = os.path.getsize(OUTPUT_PATH_OPSET17) / 1e6
            print(f"Wrote {OUTPUT_PATH_OPSET17} ({size_mb:.1f} MB)")

        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
