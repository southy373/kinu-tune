#!/usr/bin/env python3
"""Convert the official BigVGAN v2 checkpoint to the ONNX model the plugin loads.

Downloads bigvgan_generator.pt + config.json from the official Hugging Face
repository (nvidia/bigvgan_v2_44khz_128band_512x, pinned revision), loads the
generator with NVIDIA's BigVGAN code (pinned commit), removes weight norm and
exports with the Dynamo exporter to:

    Resources/models/bigvgan_v2_44khz_128band.onnx

and additionally derives a ConvTranspose-free variant:

    Resources/models/bigvgan_v2_44khz_128band_noct.onnx

The variant exists for the DirectML session ladder in BigVGANVocoder.
On the user's machine, session creation succeeded while every DirectML
inference failed at the first ConvTranspose node (E_INVALIDARG from
the provider's operator author) - for the opset 20 model and for an
earlier opset 17 re-stamp of it, at full and at basic graph
optimisation alike - so the plug-in fell back to CPU (verified
2026-11-10 from the field log). The re-stamp was a dead end and batch
4 replaces it: each of the graph's 111 ConvTranspose nodes is
rewritten as the mathematically equivalent pair

    ConvTranspose(stride s, kernel k, pads p)
      = Conv(stride 1, kernel = spatially flipped + channel
             transposed weights, pads = k - 1 - p, plus
             output_padding on the end)
        applied to the input with s - 1 zeros inserted between
        samples,

where the zero insertion uses operators already present in the graph
(Reshape -> Pad -> Reshape -> Slice, the Slice end computed from the
runtime input length via Shape/Gather/arithmetic), so the dynamic
frame count keeps working. The derivation stays fail-closed: any
ConvTranspose outside the inventoried envelope (not 1-D,
dilation != 1, auto_pad != NOTSET, weights or bias not initializers)
aborts the conversion, and the output is kept only after the full
ONNX checker passes, a zero-ConvTranspose inventory, and a numerical
cross-check against the main model at several mel lengths agrees
within tolerance (max abs error 1.29e-05 when this was adopted, the
same order as the export's own error against the PyTorch reference).
Numerical equivalence is all that is proven here - whether DirectML
accepts the rewritten graph can only be confirmed on the user's
machine, and the ladder keeps the main model and CPU behind it.

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
OUTPUT_PATH_NOCT = os.path.join(
    REPO_ROOT, "Resources", "models",
    "bigvgan_v2_44khz_128band_noct.onnx",
)

# The pinned checkpoint's contract (its config.json), used by the smoke
# tests and by the noct derivation when it runs standalone against an
# already-exported main model.
PINNED_NUM_MELS = 128
PINNED_HOP_SIZE = 512

# A derived model's output must match the main model's on the same
# input. Measured value for the noct rewrite when this was adopted:
# 1.29e-05. The tolerance leaves headroom for runtime variation while
# still catching any real divergence.
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


def _attr(node, name, default=None):
    import onnx

    for a in node.attribute:
        if a.name == name:
            if a.type == onnx.AttributeProto.INTS:
                return list(a.ints)
            if a.type == onnx.AttributeProto.INT:
                return a.i
            if a.type == onnx.AttributeProto.STRING:
                return a.s.decode("utf-8")
            return a
    return default


def _noct_transform_weight(W, group):
    """ConvTranspose weight [Cin, Cout/g, k] -> Conv weight
    [Cout, Cin/g, k]: per group, transpose the (in, out) channel axes
    and flip the spatial axis."""
    import numpy as np

    Cin, Cg_out, k = W.shape
    ci = Cin // group
    W4 = W.reshape(group, ci, Cg_out, k)
    W4 = W4.transpose(0, 2, 1, 3)[:, :, :, ::-1]
    return np.ascontiguousarray(W4.reshape(group * Cg_out, ci, k))


def _noct_replacement(prefix, x_name, W, bias, stride, pads, opad, group,
                      out_name):
    """Nodes + initializers computing the ConvTranspose equivalent.
    See the module docstring for the identity used."""
    import numpy as np
    from onnx import helper, numpy_helper

    s = stride
    pb, pe = pads
    k = W.shape[2]
    nodes, inits = [], []

    def init(name, arr):
        inits.append(numpy_helper.from_array(np.asarray(arr), name=name))
        return name

    idx2 = init(f"{prefix}_c_idx2", np.array([2], dtype=np.int64))
    one = init(f"{prefix}_c_one", np.array([1], dtype=np.int64))
    s_t = init(f"{prefix}_c_s", np.array([s], dtype=np.int64))
    r4 = init(f"{prefix}_c_r4", np.array([0, 0, 0, 1], dtype=np.int64))
    padvec = init(f"{prefix}_c_pads",
                  np.array([0, 0, 0, 0, 0, 0, 0, s - 1], dtype=np.int64))
    r3 = init(f"{prefix}_c_r3", np.array([0, 0, -1], dtype=np.int64))
    starts = init(f"{prefix}_c_starts", np.array([0], dtype=np.int64))
    axes = init(f"{prefix}_c_axes", np.array([2], dtype=np.int64))
    steps = init(f"{prefix}_c_steps", np.array([1], dtype=np.int64))
    wname = init(f"{prefix}_w_noct", _noct_transform_weight(W, group))

    nodes.append(helper.make_node("Shape", [x_name], [f"{prefix}_shape"],
                                  name=f"{prefix}_shape_n"))
    nodes.append(helper.make_node("Gather", [f"{prefix}_shape", idx2],
                                  [f"{prefix}_t"], name=f"{prefix}_t_n"))
    nodes.append(helper.make_node("Sub", [f"{prefix}_t", one],
                                  [f"{prefix}_tm1"], name=f"{prefix}_tm1_n"))
    nodes.append(helper.make_node("Mul", [f"{prefix}_tm1", s_t],
                                  [f"{prefix}_ts"], name=f"{prefix}_ts_n"))
    nodes.append(helper.make_node("Add", [f"{prefix}_ts", one],
                                  [f"{prefix}_len"], name=f"{prefix}_len_n"))
    nodes.append(helper.make_node("Reshape", [x_name, r4],
                                  [f"{prefix}_x4"], name=f"{prefix}_x4_n"))
    nodes.append(helper.make_node("Pad", [f"{prefix}_x4", padvec],
                                  [f"{prefix}_xp"], name=f"{prefix}_xp_n"))
    nodes.append(helper.make_node("Reshape", [f"{prefix}_xp", r3],
                                  [f"{prefix}_xs"], name=f"{prefix}_xs_n"))
    nodes.append(helper.make_node(
        "Slice", [f"{prefix}_xs", starts, f"{prefix}_len", axes, steps],
        [f"{prefix}_stuffed"], name=f"{prefix}_stuffed_n"))
    conv_inputs = [f"{prefix}_stuffed", wname]
    if bias is not None:
        bname = init(f"{prefix}_b_noct", bias)
        conv_inputs.append(bname)
    nodes.append(helper.make_node(
        "Conv", conv_inputs, [out_name], name=f"{prefix}_conv_n",
        strides=[1], pads=[k - 1 - pb, k - 1 - pe + opad], group=group))
    return nodes, inits


def _noct_rewrite(model):
    """Replace every ConvTranspose node in model.graph. Returns an
    error string, or None on success (model modified in place)."""
    from onnx import numpy_helper

    graph = model.graph
    inits = {i.name: i for i in graph.initializer}

    ct_nodes = [n for n in graph.node if n.op_type == "ConvTranspose"]
    print(f"ConvTranspose nodes to rewrite: {len(ct_nodes)}")
    if not ct_nodes:
        return "source graph has no ConvTranspose nodes; nothing to do"

    new_nodes = []
    new_inits = []
    for node in graph.node:
        if node.op_type != "ConvTranspose":
            new_nodes.append(node)
            continue

        prefix = node.name or f"ct_{len(new_nodes)}"
        if len(node.input) not in (2, 3) or len(node.output) != 1:
            return f"{prefix}: unexpected input/output count"
        strides = _attr(node, "strides", [1])
        pads = _attr(node, "pads", [0, 0])
        opad = _attr(node, "output_padding", [0])
        dilations = _attr(node, "dilations", [1])
        group = _attr(node, "group", 1)
        auto_pad = _attr(node, "auto_pad", "NOTSET")
        if len(strides) != 1 or len(pads) != 2 or len(opad) != 1:
            return (f"{prefix}: not a 1-D ConvTranspose "
                    f"(strides={strides}, pads={pads}, opad={opad})")
        if dilations != [1]:
            return f"{prefix}: dilations {dilations} outside the envelope"
        if auto_pad != "NOTSET":
            return f"{prefix}: auto_pad {auto_pad!r} outside the envelope"

        w_init = inits.get(node.input[1])
        if w_init is None:
            return f"{prefix}: weight {node.input[1]!r} is not an initializer"
        W = numpy_helper.to_array(w_init)
        if W.ndim != 3:
            return f"{prefix}: weight has {W.ndim} dims, expected 3 (1-D)"
        bias = None
        if len(node.input) == 3:
            b_init = inits.get(node.input[2])
            if b_init is None:
                return f"{prefix}: bias {node.input[2]!r} is not an initializer"
            bias = numpy_helper.to_array(b_init)

        nodes, add_inits = _noct_replacement(
            prefix, node.input[0], W, bias, strides[0], tuple(pads),
            opad[0], group, node.output[0])
        new_nodes.extend(nodes)
        new_inits.extend(add_inits)

    del graph.node[:]
    graph.node.extend(new_nodes)
    graph.initializer.extend(new_inits)

    # Drop initializers the rewritten graph no longer references.
    referenced = set()
    for n in graph.node:
        referenced.update(n.input)
    referenced.update(o.name for o in graph.output)
    kept = [i for i in graph.initializer if i.name in referenced]
    dropped = len(graph.initializer) - len(kept)
    del graph.initializer[:]
    graph.initializer.extend(kept)
    print(f"nodes: {len(graph.node)}, initializers kept: {len(kept)} "
          f"(dropped {dropped} unreferenced)")
    return None


def _noct_verify(source_path, dest_path, num_mels, hop_size):
    """Fail-closed verification of the rewritten model. Returns an
    error string, or None on success; prints the evidence."""
    from collections import Counter

    import numpy as np
    import onnx
    import onnxruntime as ort

    model = onnx.load(dest_path)
    try:
        onnx.checker.check_model(model, full_check=True)
    except Exception as e:  # checker raises onnx.checker.ValidationError
        return f"rewritten model failed the ONNX checker: {e}"
    print("ONNX checker (full_check): PASS")

    ops = Counter(n.op_type for n in model.graph.node)
    if ops.get("ConvTranspose", 0) != 0:
        return "rewritten model still contains ConvTranspose nodes"
    print("ConvTranspose count: 0")
    del model

    rng = np.random.default_rng(0)
    cases = []
    for frames in (17, 43, 86):
        cases.append((
            f"random normal frames={frames}",
            rng.normal(-4.0, 2.0, size=(1, num_mels, frames)
                       ).astype(np.float32),
        ))
    cases.append((
        "random uniform frames=130",
        (rng.random((1, num_mels, 130), dtype=np.float32) * 8.0 - 10.0),
    ))

    src = ort.InferenceSession(source_path, providers=["CPUExecutionProvider"])
    dst = ort.InferenceSession(dest_path, providers=["CPUExecutionProvider"])
    worst = 0.0
    for label, mel in cases:
        frames = mel.shape[2]
        out_src = src.run(None, {"mel": mel})[0]
        out_dst = dst.run(None, {"mel": mel})[0]
        expected = (1, 1, frames * hop_size)
        if out_dst.shape != expected or out_src.shape != expected:
            return (f"{label}: shape mismatch src={out_src.shape} "
                    f"dst={out_dst.shape}, expected {expected}")
        err = float(np.max(np.abs(out_dst - out_src)))
        worst = max(worst, err)
        print(f"cross-check {label}: shape {out_dst.shape} "
              f"max abs error {err:.3e}")
    if worst > CROSS_CHECK_TOLERANCE:
        return (f"rewritten model diverges from the source "
                f"(max abs error {worst:.3e} > {CROSS_CHECK_TOLERANCE:.1e})")
    print(f"cross-check worst max abs error: {worst:.3e} "
          f"(tolerance {CROSS_CHECK_TOLERANCE:.1e})")
    return None


def derive_noct_variant(source_path, dest_path, num_mels, hop_size):
    """Write the ConvTranspose-free variant of the model at
    source_path to dest_path.

    Rewrites every ConvTranspose node (see the module docstring for
    the identity and why the variant exists), then proves the result:
    the full ONNX checker, a zero-ConvTranspose inventory, and a
    numerical cross-check against the source model at several mel
    lengths. Returns True on success.
    """
    import onnx

    print(f"Deriving ConvTranspose-free variant from {source_path} ...")
    model = onnx.load(source_path)
    err = _noct_rewrite(model)
    if err is not None:
        print(f"error: refusing the rewrite: {err}", file=sys.stderr)
        return False
    onnx.save_model(model, dest_path)
    del model

    err = _noct_verify(source_path, dest_path, num_mels, hop_size)
    if err is not None:
        print(f"error: noct variant failed verification: {err}",
              file=sys.stderr)
        return False
    return True


def main():
    need_main = not os.path.exists(OUTPUT_PATH)
    need_variant = not os.path.exists(OUTPUT_PATH_NOCT)
    if not need_main and not need_variant:
        print(f"BigVGAN models already present, skipping:\n"
              f"  {OUTPUT_PATH}\n  {OUTPUT_PATH_NOCT}")
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
            variant_tmp = os.path.join(workdir, "bigvgan_noct.onnx")
            if not derive_noct_variant(
                OUTPUT_PATH, variant_tmp, num_mels, hop_size
            ):
                return 1
            shutil.move(variant_tmp, OUTPUT_PATH_NOCT)
            size_mb = os.path.getsize(OUTPUT_PATH_NOCT) / 1e6
            print(f"Wrote {OUTPUT_PATH_NOCT} ({size_mb:.1f} MB)")

        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
