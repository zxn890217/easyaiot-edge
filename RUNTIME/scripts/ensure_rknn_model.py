#!/usr/bin/env python3
"""Convert YOLO .onnx (or .pt) → Rockchip NPU .rknn for RUNTIME (RK3588 / RK356x).

Runs on the x86_64 Linux control plane (VIDEO backend) because rknn-toolkit2 is
only published for x86_64 hosts; the produced .rknn is what the edge device loads.

Idempotent: skips when output exists and is newer than input (unless --force).

Usage:
  python3 ensure_rknn_model.py --input yolov8n.onnx --output /models/3/rknn/model.rknn
  python3 ensure_rknn_model.py --input model.onnx --quantized --dataset calib/dataset.txt
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

SUPPORTED_PLATFORMS = (
    "rk3588", "rk3588s", "rk3576", "rk3562", "rk3566", "rk3568", "rk3399", "rv1126", "rv1109",
)


def _write_names(target_path: Path, names) -> None:
    names_path = target_path.with_suffix(".names")
    lines: list[str] = []
    if isinstance(names, dict):
        for i in range(len(names)):
            lines.append(str(names.get(i, names.get(str(i), f"class_{i}"))))
    elif isinstance(names, (list, tuple)):
        lines = [str(x) for x in names]
    else:
        return
    names_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote_names={names_path} count={len(lines)}")


def _names_from_onnx(onnx_path: Path):
    """Read the `names` custom metadata field written by ultralytics onnx export."""
    try:
        import onnx
    except Exception as exc:  # pragma: no cover - onnx ships with rknn-toolkit2
        print(f"WARN: onnx package unavailable, skip names metadata: {exc}", file=sys.stderr)
        return None
    try:
        model = onnx.load(str(onnx_path), load_external_data=False)
        for prop in model.metadata_props:
            if prop.key == "names" and prop.value:
                raw = prop.value
                try:
                    return json.loads(raw.replace("'", '"'))
                except Exception:  # noqa: BLE001 - best effort
                    return None
    except Exception as exc:  # pragma: no cover
        print(f"WARN: failed reading names metadata: {exc}", file=sys.stderr)
    return None


def _ensure_onnx(input_path: Path, imgsz: int, force: bool) -> Path:
    """Return a loadable .onnx, exporting from .pt via ensure_onnx_model.py when needed."""
    if input_path.suffix.lower() == ".onnx":
        return input_path

    onnx_path = input_path.with_suffix(".onnx")
    script_dir = Path(__file__).resolve().parent
    ensure_onnx = script_dir / "ensure_onnx_model.py"
    if not ensure_onnx.is_file():
        raise FileNotFoundError(f"ensure_onnx_model.py not found beside {Path(__file__).name}")

    cmd = [sys.executable, str(ensure_onnx), "--input", str(input_path),
           "--output", str(onnx_path), "--imgsz", str(imgsz)]
    if force:
        cmd.append("--force")
    print(f"exporting_onnx={' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    sys.stdout.write(proc.stdout or "")
    sys.stderr.write(proc.stderr or "")
    if proc.returncode != 0 or not onnx_path.is_file():
        raise RuntimeError(f".pt → .onnx export failed rc={proc.returncode}")
    return onnx_path


def convert_onnx(
    onnx_path: Path,
    rknn_path: Path,
    *,
    target_platform: str = "rk3588",
    quantized: bool = True,
    dataset: str = "",
    mean_values=(0.0, 0.0, 0.0),
    std_values=(255.0, 255.0, 255.0),
    opt_level: int = 3,
    force: bool = False,
) -> int:
    if not force and rknn_path.is_file():
        if (not onnx_path.is_file()) or rknn_path.stat().st_mtime >= onnx_path.stat().st_mtime:
            print(f"skip_existing={rknn_path}")
            if not rknn_path.with_suffix(".names").is_file():
                names = _names_from_onnx(onnx_path)
                if names:
                    _write_names(rknn_path, names)
            if not rknn_path.with_suffix(".rknn.json").is_file():
                _write_sidecar(
                    rknn_path,
                    onnx_path,
                    target_platform=target_platform,
                    quantized=None,
                    dataset=dataset,
                    mean_values=mean_values,
                    std_values=std_values,
                )
            return 0

    try:
        from rknn.api import RKNN
    except Exception as exc:  # pragma: no cover - only present on x86_64 control plane
        print(
            "ERROR: rknn-toolkit2 is required to build .rknn. "
            f"Install it on the export host (python3.10 x86_64): {exc}",
            file=sys.stderr,
        )
        return 2

    if not onnx_path.is_file():
        print(f"ERROR: input onnx not found: {onnx_path}", file=sys.stderr)
        return 2

    rknn_path.parent.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=False)
    # mean/std bake the /255 normalization into the NPU graph, so RUNTIME feeds
    # raw uint8 NHWC RGB tensors (no float blobFromImage on the edge device).
    rknn.config(
        mean_values=[list(mean_values)],
        std_values=[list(std_values)],
        target_platform=target_platform,
        opt_level=opt_level,
    )

    print(f"loading_onnx={onnx_path}")
    if rknn.load_onnx(model=str(onnx_path)) != 0:
        print("ERROR: rknn.load_onnx failed", file=sys.stderr)
        rknn.release()
        return 3

    do_quant = bool(quantized)
    if do_quant and not dataset:
        print("WARN: no calibration dataset; falling back to non-quantized (fp16) build")
        do_quant = False

    print(f"building target={target_platform} quantized={do_quant} dataset={dataset or '-'}")
    if rknn.build(do_quantization=do_quant, dataset=dataset if do_quant else None) != 0:
        print("ERROR: rknn.build failed", file=sys.stderr)
        rknn.release()
        return 4

    if rknn.export_rknn(str(rknn_path)) != 0:
        print("ERROR: rknn.export_rknn failed", file=sys.stderr)
        rknn.release()
        return 5

    rknn.release()

    if not rknn_path.is_file():
        print(f"ERROR: conversion produced no file at {rknn_path}", file=sys.stderr)
        return 6

    names = _names_from_onnx(onnx_path)
    if names:
        _write_names(rknn_path, names)
    elif onnx_path.with_suffix(".names").is_file():
        rknn_path.with_suffix(".names").write_bytes(onnx_path.with_suffix(".names").read_bytes())
        print(f"copied_names={rknn_path.with_suffix('.names')}")

    _write_sidecar(
        rknn_path,
        onnx_path,
        target_platform=target_platform,
        quantized=do_quant,
        dataset=dataset,
        mean_values=mean_values,
        std_values=std_values,
    )
    print(f"ok={rknn_path} size={rknn_path.stat().st_size}")
    return 0


def _write_sidecar(
    rknn_path: Path,
    onnx_path: Path,
    *,
    target_platform: str,
    quantized,
    dataset: str,
    mean_values,
    std_values,
) -> None:
    """Write the <model>.rknn.json sidecar consumed by RknnEngine::LoadModel."""
    side, layout = _graph_shape(onnx_path)
    meta = {
        "source_onnx": str(onnx_path),
        "target_platform": target_platform,
        "quantized": quantized,
        "dataset": dataset or None,
        "mean_values": list(mean_values),
        "std_values": list(std_values),
        "imgsz": side,
        "model_layout": layout,
    }
    rknn_path.with_suffix(".rknn.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"wrote_sidecar={rknn_path.with_suffix('.rknn.json')} layout={layout} imgsz={side}")


def _graph_shape(onnx_path: Path) -> tuple[int, str]:
    """(input side length, output layout) from the source ONNX graph.

    The layout is recorded in the .rknn sidecar because the RKNN runtime reports
    tensor dims in its own axis order, which makes [1, N, 6] and [1, 6, N]
    indistinguishable from the C++ side alone.
    """
    try:
        import onnx

        model = onnx.load(str(onnx_path), load_external_data=False)
        graph = model.graph
        side = 0
        if graph.input:
            dims = [d.dim_value for d in graph.input[0].type.tensor_type.shape.dim]
            non_batch = [d for d in dims[1:] if d and d > 1]
            if non_batch:
                side = max(non_batch)
        layout = "detect"
        if graph.output:
            out_dims = [d.dim_value for d in graph.output[0].type.tensor_type.shape.dim]
            if out_dims and out_dims[-1] == 6:
                layout = "end2end"
        return side, layout
    except Exception as exc:  # noqa: BLE001 - metadata only
        print(f"WARN: failed reading graph shapes: {exc}", file=sys.stderr)
        return 0, "detect"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", "-i", required=True, help=".onnx (preferred) or .pt path")
    ap.add_argument("--output", "-o", default="", help="output .rknn path (default: sibling)")
    ap.add_argument("--target-platform", default=os.getenv("RKNN_TARGET_PLATFORM", "rk3588"),
                    choices=list(SUPPORTED_PLATFORMS))
    ap.add_argument("--imgsz", type=int, default=640, help="imgsz used when exporting .pt → .onnx")
    ap.add_argument("--quantized", action="store_true", help="INT8 quantization (needs --dataset)")
    ap.add_argument("--no-quant", dest="quantized", action="store_false",
                    help="force fp16 build (no calibration dataset needed)")
    ap.set_defaults(quantized=True)
    ap.add_argument("--dataset", default="", help="rknn-toolkit2 calibration dataset.txt (one image path per line)")
    ap.add_argument("--opt-level", type=int, default=3)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    input_path = Path(args.input)
    if args.output:
        rknn_path = Path(args.output)
    else:
        rknn_path = input_path.with_suffix(".rknn")

    try:
        onnx_path = _ensure_onnx(input_path, imgsz=args.imgsz, force=args.force)
    except Exception as exc:  # noqa: BLE001
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    return convert_onnx(
        onnx_path,
        rknn_path,
        target_platform=args.target_platform,
        quantized=args.quantized,
        dataset=args.dataset,
        opt_level=args.opt_level,
        force=args.force,
    )


if __name__ == "__main__":
    sys.exit(main())
