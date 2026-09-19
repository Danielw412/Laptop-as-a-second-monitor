"""Summarize successful streams only; archive wins over duplicate rolling records.

Usage: python benchmarks/analyze-diagnostics.py [JSONL or bench log ...]
Without paths, reads the app archive and rolling diagnostics on this Windows host.
Means of rolling statistics are explicitly sample summaries, not pooled frame percentiles.
"""
import collections
import argparse
import json
import math
import os
from pathlib import Path
import statistics


def read_records(paths):
    seen = set()
    for path in paths:
        for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            if not line.startswith("{"):
                continue
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get("type") != "host-stats":
                continue
            identity = (row.get("run_id"), row.get("engine_session"), row.get("at"), row.get("seconds"))
            if identity in seen:
                continue
            seen.add(identity)
            yield row


def summarize(rows, fields):
    result = {}
    for field in fields:
        values = [r.get(field) for r in rows]
        values = sorted(v for v in values if isinstance(v, (int, float)) and math.isfinite(v))
        if values:
            result[field] = dict(n=len(values), mean=round(statistics.mean(values), 3),
                                 p95=round(values[math.ceil(.95 * len(values)) - 1], 3),
                                 max=round(max(values), 3))
            if field in {"dropped", "freezes", "freezeMs", "nack", "pli", "intervalPacketsReceived", "intervalPacketsLost"}:
                result[field]["sum"] = round(sum(values), 3)
    return result


HOST = "capture_fps encode_fps encode_ms_mean encode_ms_p95 encode_ms_p99 host_acquire_to_encode_submit_ms_mean host_acquire_to_encoded_ms_mean host_acquire_to_send_ms_mean host_acquire_to_send_ms_p95 host_acquire_to_send_ms_max encoder_output_pickup_ms_mean send_ms_mean queue_depth cpu_percent loop_busy_percent gpu_engine_video_decode_percent gpu_engine_video_processing_percent gpu_engine_3d_percent bitrate target_bitrate source_frame_gap_ms_max encoded_bits_per_second".split()
RECEIVER = "fps loss rttMs jitterMs jitterBufferMs decodeMs processingMs dropped freezes freezeMs nack pli qp intervalPacketsReceived intervalPacketsLost".split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--warmup", type=float, default=0, help="seconds after first connected record to omit")
    parser.add_argument("--duration", type=float, help="seconds to retain after warmup")
    args = parser.parse_args()
    starts = []
    if args.paths:
        paths = args.paths
    else:
        archive = Path(os.environ["LOCALAPPDATA"]) / "LaptopMonitor/logs/sessions"
        paths = sorted(archive.glob("*/perf*.jsonl"))
        paths += sorted((Path(os.environ["TEMP"]) / "LaptopMonitor").glob("perf*.jsonl"))
        for log in (Path(os.environ["TEMP"]) / "LaptopMonitor").glob("host*.log"):
            starts += [line[:23] for line in log.read_text(encoding="utf-8-sig", errors="replace").splitlines()
                       if "Laptop Monitor " in line and " starting" in line]
        starts.sort()
    groups = collections.defaultdict(list)
    for row in read_records(paths):
        # Old records use the preceding process-start line, never a calendar day as a run boundary.
        run = row.get("run_id")
        if not run:
            prior = [s for s in starts if s <= row.get("at", "")]
            if not prior:
                continue  # Cannot establish this record's run provenance.
            run = "legacy-" + prior[-1]
        groups[run].append(row)
    output = {}
    for run, all_rows in groups.items():
        rows = sorted((r for r in all_rows if r.get("viewer_connected") and
                       r.get("webrtc", {}).get("sent_frames", 0) > 0), key=lambda r: (r.get("engine_session", 0), r["seconds"]))
        if not rows:
            continue
        first_seconds = {}
        for r in rows:
            first_seconds.setdefault(r.get("engine_session", 0), r["seconds"])
        rows = [r for r in rows if r["seconds"] - first_seconds[r.get("engine_session", 0)] >= args.warmup
                and (args.duration is None or r["seconds"] - first_seconds[r.get("engine_session", 0)] < args.warmup + args.duration)]
        if not rows:
            continue
        fresh = []
        previous = None
        for r in rows:
            spacing = r["seconds"] - previous["seconds"] if previous and r.get("engine_session") == previous.get("engine_session") else 1
            age = r.get("webrtc", {}).get("receiver_age_ms")
            if age is not None and 0 <= age < min(1500, spacing * 1000):
                fresh.append(r["webrtc"]["receiver"])
            previous = r
        pipelines = {}
        inferred_pipeline = 0
        previous_captured = None
        for r in rows:
            if previous_captured is not None and r["captured"] < previous_captured:
                inferred_pipeline += 1
            previous_captured = r["captured"]
            key = str(r.get("pipeline_session", "legacy-inferred-" + str(inferred_pipeline)))
            pipelines[key] = {k: r.get(k) for k in ("pipeline_build_reason", "captured", "encoded", "dropped", "dropped_coalesced", "dropped_superseded", "dropped_ring_busy", "dropped_submit_failed", "encoder_rebuilds", "encoder_rebuild_ms_mean")}
        output[run] = dict(connected_records=len(rows), fresh_receiver_records=len(fresh),
                           first=rows[0].get("at"), last=rows[-1].get("at"),
                           host=summarize(rows, HOST), receiver=summarize(fresh, RECEIVER),
                           transport=summarize([r["webrtc"] for r in rows], ["transport_buffer_bytes", "transport_buffer_pressure", "transport_dropped", "bitrate_raises", "bitrate_cuts"]),
                           active_host=summarize([r for r in rows if r["capture_fps"] >= 45], HOST),
                           pipelines=pipelines)
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
