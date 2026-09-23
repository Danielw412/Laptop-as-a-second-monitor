"""Compare a recorded encoded stream with the source frames the encoder read.

A bench recording (laptop-monitor-bench --record out.h264 --record-source-every N) or a receiver-triggered dump
holds the exact access units that went to the network (out.h264, one index line per unit in out.jsonl) and,
optionally, sampled NV12 source surfaces (out.nv12, indexed by out.source.jsonl). This script decodes the stream with
ffmpeg, pairs every sampled source frame with the decoded frame of the same source sequence number, and reports luma
PSNR per horizontal band. That separates the stages of the pipeline:

  * a band far below the rest in the *decoded* stream is damage the encoder produced (quantization when QP is high,
    or a bad input surface when QP is normal), because the recording is what a lossless receiver decodes;
  * a clean decoded stream while the receiver showed damage points at transmission or the receiver's decoder.

Requires ffmpeg on PATH. Pure Python otherwise (no numpy), so it is slow on long recordings; --max-frames limits it.

  python scripts/analyze-recording.py out.h264 [--png-dir frames] [--bands 9]
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile


def load_jsonl(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def band_psnr(a, b, width, height, bands):
    """Luma PSNR per horizontal band and over the whole frame. a, b: NV12 frames (bytes)."""
    rows = height // bands
    out = []
    total = 0
    for band in range(bands):
        y0 = band * rows
        y1 = height if band == bands - 1 else y0 + rows
        start, end = y0 * width, y1 * width
        sse = sum((x - y) * (x - y) for x, y in zip(a[start:end], b[start:end]))
        total += sse
        mse = sse / max(1, end - start)
        out.append(99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse))
    mse = total / (width * height)
    return out, 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stream", help="recording .h264 (its .jsonl, .nv12 and .source.jsonl are found next to it)")
    ap.add_argument("--bands", type=int, default=9, help="horizontal bands per frame (default 9: 120 rows at 1080p)")
    ap.add_argument("--png-dir", help="write decoded and source PNGs of the worst frame here")
    ap.add_argument("--max-frames", type=int, default=40, help="compare at most this many sampled frames")
    ap.add_argument("--bad-db", type=float, default=28.0, help="a band below this PSNR (dB) is reported as damaged")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is not on PATH")
    base, _ = os.path.splitext(args.stream)
    units = load_jsonl(base + ".jsonl")
    sources = load_jsonl(base + ".source.jsonl")
    if not units:
        sys.exit(f"No index next to {args.stream}")

    qps = [u["qp"] for u in units if u.get("qp") is not None]
    keys = sum(1 for u in units if u.get("idr"))
    sizes = [u["bytes"] for u in units]
    print(f"{len(units)} access units, {keys} IDR, {sum(sizes) / 1e6:.2f} MB")
    if qps:
        s = sorted(qps)
        print(f"slice QP: mean {sum(qps) / len(qps):.1f}, median {s[len(s) // 2]:.1f}, "
              f"p95 {s[int(0.95 * (len(s) - 1))]:.1f}, max {s[-1]:.1f}; "
              f"{sum(1 for q in qps if q >= 40)} frames at QP >= 40, {sum(1 for q in qps if q >= 46)} at >= 46")
    gaps = 0
    prev = None
    for u in units:
        fn = u.get("frame_num")
        if u.get("idr"):
            prev = fn
            continue
        if fn is not None and prev is not None and fn not in (prev, prev + 1) and fn != 0:
            gaps += 1
        prev = fn if fn is not None else prev
    print(f"frame_num discontinuities (outside IDR): {gaps}")

    if not sources:
        print("No source snapshots in this recording; only the stream statistics above.")
        return
    width, height = sources[0]["width"], sources[0]["height"]
    pitches = {(s["row_pitch"], s["depth_pitch"]) for s in sources}
    print(f"source snapshots: {len(sources)} at {width}x{height}, staging row/depth pitch {sorted(pitches)} "
          f"(tightly packed NV12 would be {width}/{width * height * 3 // 2})")
    unit_by_seq = {u["seq"]: i for i, u in enumerate(units) if u.get("seq")}
    frame_bytes = width * height * 3 // 2

    with tempfile.TemporaryDirectory() as tmp:
        decoded = os.path.join(tmp, "decoded.nv12")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", args.stream, "-fps_mode", "passthrough", "-f", "rawvideo",
                        "-pix_fmt", "nv12", decoded], check=True)
        count = os.path.getsize(decoded) // frame_bytes
        print(f"ffmpeg decoded {count} frames from {len(units)} access units")
        worst = None
        results = []
        with open(base + ".nv12", "rb") as src, open(decoded, "rb") as dec:
            for s in sources[: args.max_frames]:
                unit = unit_by_seq.get(s["seq"])
                if unit is None or unit >= count:
                    print(f"  source seq {s['seq']}: not in the decoded stream (dropped before encoding?)")
                    continue
                src.seek(s["index"] * frame_bytes)
                a = src.read(frame_bytes)
                dec.seek(unit * frame_bytes)
                b = dec.read(frame_bytes)
                bands, whole = band_psnr(a, b, width, height, args.bands)
                u = units[unit]
                bad = [i for i, v in enumerate(bands) if v < args.bad_db]
                results.append((whole, s["seq"], unit, u.get("qp"), u["bytes"], bands, bad))
                if worst is None or whole < worst[0]:
                    worst = (whole, s, unit, a, b)
        for whole, seq, unit, qp, size, bands, bad in results:
            qp_text = f"{qp:.1f}" if isinstance(qp, (int, float)) else "n/a"
            print(f"  seq {seq:6} unit {unit:5} QP {qp_text:>5} {size:7} B  PSNR {whole:5.1f} dB  bands "
                  + " ".join(f"{v:4.1f}" for v in bands) + (f"  damaged bands {bad}" if bad else ""))
        if results:
            print(f"mean PSNR over sampled frames: {sum(r[0] for r in results) / len(results):.2f} dB, "
                  f"worst {min(r[0] for r in results):.2f} dB")
        if args.png_dir and worst:
            os.makedirs(args.png_dir, exist_ok=True)
            for name, data in (("source", worst[3]), ("decoded", worst[4])):
                raw = os.path.join(tmp, name + ".nv12")
                with open(raw, "wb") as f:
                    f.write(data)
                png = os.path.join(args.png_dir, f"{name}-seq{worst[1]['seq']}.png")
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "nv12", "-s",
                                f"{width}x{height}", "-i", raw, png], check=True)
                print("wrote", png)


if __name__ == "__main__":
    main()
