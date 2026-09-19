# Diagnostics-led optimization, 2026-09-19

Implemented and measured a narrow bitrate-adaptation change: a clean but lightly used connection no longer
automatically earns a larger encoder budget. This removed an unnecessary 633 ms encoder rebuild in a connected
still-desktop benchmark. Sustained-motion streaming retained approximately 60 FPS and upward quality adaptation.
There is no demonstrated improvement to steady-state frame latency or measured pixel fidelity.

## Evidence and selection

Read the current `host.log`, `perf.jsonl`, `perf.1.jsonl`, and the archived `host.log`, `perf.jsonl`, and
`session.json` under `%LOCALAPPDATA%\LaptopMonitor\logs\sessions\20260918-192646-23324`.
The archive describes Windows 11 25H2/build 26200, Iris Xe driver 32.0.101.7077, AC power, WGC/Auto,
1920x1080 at 60 FPS, Balanced quality, and a graceful exit.

Only records with `viewer_connected == true` and positive `webrtc.sent_frames` enter the analysis.
Archive/rolling duplicates are removed. Failed connection attempts and disconnected portions, including the
archive's second engine session, are excluded. Historical runs without IDs are assigned using the preceding
process-start line in `host.log`, rather than pooling separate runs by date.

| Successful process run | Connected records available | Encoder rebuilds observed | Mean rebuild interruption |
| --- | ---: | ---: | ---: |
| 2026-09-18 00:14:09.529, rolling logs | 65 | 1 | 359 ms |
| 2026-09-18 15:13:04.243, rolling logs | 319 | 6 | 438 ms |
| 20260918-192646-23324, archive | 299 | 3 | 372 ms |

These are surviving connected samples, not the complete duration of every historical run. Older records lack
receiver sample age, so their receiver counters are not summed. The recent archive supplies 297 fresh receiver
samples (296 with interval loss/FPS data). Repeated samples are filtered using receiver age versus host spacing.

## Bottlenecks and non-bottlenecks

The recent archive's largest avoidable interruptions were the encoder rebuilds. At 19:28:42, 19:30:18 and
19:31:51, upward changes from 8 to 10 to 12 to 14 Mbps interrupted the stream for 286.4, 428.7 and 401.5 ms:
1.117 seconds total. The latter two pipeline sessions captured only about two new frames per second.
Low loss and RTT alone caused the controller to raise the budget despite little actual traffic.

Ordinary processing was substantially faster than these interruptions:

| Recent successful archive metric | Value |
| --- | ---: |
| Capture / encode / receiver FPS, sample means | 9.41 / 9.39 / 9.35 |
| Encode latency, mean of rolling means | 5.54 ms |
| Acquire to encode submission, mean of rolling means | 1.39 ms |
| Acquire to encoded output, mean of rolling means | 6.92 ms |
| Acquire to send completion, mean of rolling means | 8.25 ms |
| Worst acquire-to-send interval maximum | 70.05 ms |
| Encoder output pickup, mean of rolling means | 0.16 ms |
| Send-call time, mean of rolling means | 1.32 ms |
| Encoder queue depth, mean / maximum | 0.13 / 1 |
| Receiver jitter buffer / decode / packet-to-frame, sample means | 1.84 / 1.27 / 3.20 ms |
| Packet loss / receiver drops / transport drops / buffer-pressure count | 0 / 0 / 0 / 0 |
| Host process CPU, mean / peak, whole machine | 0.55% / 3.91% |
| Host GPU video-decode / video-processing engine utilization, means | 3.18% / 1.54% |

Intel reports encoder work in the GPU engine class named video decode. These utilization readings do not show
saturation. At the last connected sample of each pipeline, there were ten total capture/encoder drops:
seven coalesced and three superseded, with zero ring-busy or submit-refused drops. Terminal `host.log` summaries
agree with this drop total. No evidence supports enlarging queues or replacing capture/encoding architecture.

Most low FPS in this run tracks low source activity: capture and encode rates agree, and long source gaps
coincide with a mostly still desktop. This is not evidence of a 60 FPS throughput ceiling. Two additional
interruptions were explicitly logged as display-topology recovery, not bitrate changes.

The older 15:13 run has genuine control inputs demanding attention: RTT spikes of 277-336 ms and jitter of
41-44 ms triggered cuts even with reported zero loss. It reached a 1.5 Mbps target and a roughly 1.9 Mbps
configured encoder. The log alone cannot establish whether those spikes are congestion or timing artifacts.
The cut policy is therefore unchanged; loss/jitter-driven oscillation is not claimed fixed.

Do not treat the quality-warning prose as a diagnosis. The recent archive called upward rebuilds changes to a
lower bitrate, and called freezes packet loss despite zero packet loss/NACKs. Receiver quantizer was unavailable.
Ordinary source pauses can also trigger the encoded-rate-collapse warning. Conclusions here use underlying
counters and events, not those labels.

## Changes in this task

- `host/include/core.hpp`: eight consecutive healthy samples earn a 250 kbps increase only when successful
  encoded-send throughput is at least 75% of the current target. Idle/unknown/invalid demand resets growth.
  Loss, RTT and jitter cuts, minimum/maximum bitrate, and increase step remain unchanged.
- `host/src/transport.cpp`: derive demand from the existing successful-byte counter on the host steady clock
  between telemetry messages. Accept 0.5-3 second intervals; the first sample after peer reset is unknown.
  REMB and receiver throughput do not control the budget. No protocol or authentication changes.
- `host/tests/core_tests.cpp`: exercise idle suppression, sustained growth, idle interruptions, the utilization
  boundary, unavailable/invalid demand, congestion cuts with no demand, and maximum bitrate.
- `docs/logging-reference.md`: explicitly document the sender counters' new control role.
- `benchmarks/analyze-diagnostics.py`: reproducible filtering, deduplication and summaries, with optional
  warmup and matched-duration windows. Cumulative counters are not summed across records.

All pre-existing uncommitted work was preserved. Host source files retain CRLF. No changes were committed.

## Controlled before/after measurements

Built the original working tree first and copied its benchmark executable before editing. Both variants used
the same secondary 1920x1080 display, WGC, 60 FPS setting, Intel encoder, local signaling on port 8796, and the
same local receiver page. Only one host benchmark ran at a time. The moving source was the existing vsync-paced
`--pattern`. The still source was the same display without the pattern. These are local loopback measurements,
not a repeat of the user's remote-laptop network conditions.

Windows below omit the first ten connected seconds and retain the next 80 seconds, including rebuilds.
Host means are means of recorded rolling statistics, not pooled per-frame averages or percentiles. Per-second
record counts differ slightly because reports can take more than a second. Receiver interval counters use only
fresh telemetry. On a still source, host latency rings retain a few startup frames, so their apparent latency
differences are not meaningful evidence of faster frame handling.

| Still desktop, matched window | Before | After |
| --- | ---: | ---: |
| Connected host records | 77 | 78 |
| Encoder rebuilds | 1 | 0 |
| Total rebuild downtime | 632.9 ms | 0 ms |
| Maximum encoder budget | 10 Mbps | 8 Mbps |
| Receiver FPS (keep-alive repeats) | 0.974 | 0.974 |
| Receiver dropped frames / freezes | 0 / 0 | 0 / 0 |
| Host CPU, whole machine | 0.087% | 0.085% |

The unchanged still source used about 0.2-0.3 Mbps, far below its original 8 Mbps budget. Suppressing an increase
here removes encoder churn, rather than reducing the initial quality preset or imposing a lower bitrate floor.

| Moving pattern, matched window | Before | After |
| --- | ---: | ---: |
| Connected host records | 79 | 79 |
| Capture / encode FPS | 59.52 / 59.49 | 59.71 / 59.67 |
| Receiver FPS | 59.19 | 59.25 |
| Encode time, mean of rolling means | 5.01 ms | 4.98 ms |
| Acquire to send, mean of rolling means | 6.42 ms | 6.66 ms |
| Acquire to send, mean of rolling p95 values | 7.57 ms | 7.74 ms |
| Receiver jitter buffer / decode | 0.29 / 0.84 ms | 0.29 / 1.10 ms |
| Receiver packet-to-frame | 1.19 ms | 1.45 ms |
| Host CPU, whole machine | 2.44% | 2.47% |
| Receiver drops / freezes | 0 / 1 | 0 / 1 |
| Maximum encoder budget | 10 Mbps | 10 Mbps |
| Upward rebuilds / measured interruption | 1 / 386.1 ms | 1 / 553.3 ms |

Motion still earns a useful higher budget, including its existing Intel rebuild cost. No steady-state latency
or CPU improvement is claimed: some timings are slightly higher after, and rebuild construction time varied.
This change does not touch the encoder construction or frame-processing path. The clearly demonstrated gain
is avoiding an entire rebuild when the stream does not use its current budget.

One preliminary sandboxed `--pattern` run connected but produced no moving capture frames. It is retained as
`baseline-motion.log` for provenance but excluded from the motion comparison. The successful moving-pattern
comparisons ran on the normal interactive desktop. The still comparison used the same execution context for
both variants. No connection-failure-only run contributes to any baseline or comparison.

## Validation and reproduction

Native Release builds of LaptopMonitor, the benchmark and test targets passed. `ctest --test-dir build/host
--output-on-failure` passed both suites. Controller tests verify that congestion cuts still work when idle or
demand is unknown. The receiver successfully resumed across benchmark host restarts. `git diff --check` passed.

Raw local benchmark logs, the preserved baseline executable and generated JSON summaries are in
`benchmarks/results/latency-20260919/` (git-ignored). The final matched summary is `comparison.json`; historical
successful streams are summarized in `baseline-summary.json`.

```powershell
python benchmarks/analyze-diagnostics.py
python benchmarks/analyze-diagnostics.py --warmup 10 --duration 80 `
  benchmarks/results/latency-20260919/baseline-still.log `
  benchmarks/results/latency-20260919/after-still.log `
  benchmarks/results/latency-20260919/baseline-motion-desktop.log `
  benchmarks/results/latency-20260919/after-motion-desktop.log
```

The 75% demand threshold is a conservative heuristic, not a measured optimum. It can delay quality growth for
intermittent content that never uses that fraction of the budget. Initial presets and congestion cuts are
unchanged. Before further tuning, capture a matched scrolling/text workload on the real receiving laptop with
available quantizer or image-quality measurements. The existing logs do not measure glass-to-glass latency,
and unavailable quantizer data prevents a numerical before/after claim about pixel fidelity. Further changes
to loss response, GOP, queues, capture, or codec settings would need additional evidence.
