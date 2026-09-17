# Logging reference

What Laptop Monitor records, what each number means, and what a bad value points at. Written for whoever picks up
optimization work next: read this and the logs instead of re-instrumenting the pipeline.

The instrumentation is **reporting only**. Nothing described here changes what the pipeline does, and nothing here
should be turned into a control loop without saying so explicitly.

## Where the logs are

`%TEMP%\LaptopMonitor\` — the Details page's *Open log folder* goes straight there. Both channels are written only
while Settings → *diagnostics log* is on (the default), and neither ever contains a pairing code or the host
credential. Uninstall removes the folder.

| File | Size | Content |
| --- | --- | --- |
| `host.log` | 2 MB, rotates to `host.1.log` | Readable lines: lifecycle, state changes, problems, periodic digests |
| `perf.jsonl` | 8 MB, rotates to `perf.1.jsonl` | One JSON object per second while the streaming engine runs |
| `setup.log` | 2 MB | The elevated `--setup` / `--uninstall` runs only |

`perf.jsonl` is written from the streaming engine's once-per-second block, so it exists only while **streaming is
started**. It does *not* require a connected receiver — a stream waiting for one still writes a record every
second, which is exactly the sample you want for idle cost. If the file is empty, the engine never ran.

Each record is prefixed with an `at` field (local time, `YYYY-MM-DD HH:MM:SS.mmm`) and is otherwise the same
object the benchmark passes to `--json` and flattens into `--csv`. A field measured in `benchmarks/` means the
same thing here.

### Getting a session to look at

```powershell
# Reproduce with the real app: start it, start the monitor, pair a receiver, use it, then read the file.
Get-Content "$env:TEMP\LaptopMonitor\perf.jsonl" -Tail 60

# Or without the GUI, on a chosen display (run from PowerShell; bash mangles \\.\DISPLAY2):
.\build\host\laptop-monitor-bench.exe --laptopmon --capture wgc --mode stream --seconds 120 --csv out.csv
```

The bench writes console, CSV and its JSON sink; it does **not** write `perf.jsonl`. Use the app when you want the
file.

Useful passes over a session (`jq` reads JSON Lines directly):

```bash
jq -r '[.at, .capture_fps, .encode_fps, .encode_ms_mean, .cpu_percent, .loop_busy_percent] | @tsv' perf.jsonl
jq -r 'select(.receiver_qp != null and .receiver_qp > 36) | [.at, .receiver_qp, .bitrate] | @tsv' perf.jsonl
jq -s 'map(.cpu_percent | numbers) | add / length' perf.jsonl      # mean process CPU over the session
jq -r 'select(.woke_idle > 40) | [.at, .woke_idle, .wakeups_per_s] | @tsv' perf.jsonl
```

## How to read the numbers

Three different accumulations are mixed in one record. Getting this wrong is the easiest way to draw a false
conclusion, so check the **Window** column before comparing two records.

| Window | Meaning |
| --- | --- |
| **cumulative** | Since this pipeline *session* began. A session ends on stop, on an encoder rebuild, or on any pipeline error, and the counter restarts from zero. A step down in `captured` means a new session, not lost frames. |
| **interval** | Just the last second. |
| **rolling** | A ring of the last N samples, *not* the last second. `Samples<>` defaults to N=4096 — at 60 fps that is roughly the last 68 seconds. Percentiles and most means are rolling. A rolling mean lags a change by a minute; do not read it as "right now". |
| **session max** | Highest value since the session began, never reset. |

Ring sizes: 4096 for the frame-path samples, 256 for keyframe/delta sizes and topology checks, 64 for encoder
rebuild durations.

## Reference machine

Every "healthy" figure below is from the development machine: Intel i5-1145G7, Iris Xe, Quick Sync H.264 MFT
(driver 32.0.101.7077), 8 logical processors, 16 GB RAM, one 1920×1080@60 virtual display, WGC capture. A
different GPU will move the encode numbers; the shape of a problem is what transfers, not the absolute value.

Fixed properties worth knowing before reading anything: three frames may be inside the encoder at once over a
four-surface NV12 ring, the periodic keyframe interval is 10 s, and the quality presets are Efficient
5→10 Mbps, Balanced 8→16 Mbps, Quality 12→20 Mbps (initial→maximum, minimum 1.5–2 Mbps).

### Measured baseline: streaming, no receiver connected

43 consecutive records, WGC, pipeline built and waiting for a receiver. Capture is not polled in this state
(`active` is false), so `captured`, `encoded` and `dropped` all stay at 0 — that is correct, not a fault. This is
what the app costs doing nothing useful, and it is the floor any idle-cost work has to beat:

| | |
| --- | --- |
| `cpu_percent` | 0.09% mean, 2.12% peak (of the whole 8-processor machine) |
| `wakeups_per_s` | 32, of which **32 were `woke_for_timeout` and 32 were `woke_idle`** |
| `loop_busy_percent` | 0.12%, longest iteration 0.4 ms |
| `working_set_mb` / `private_mb` | 105 / 142 |
| `gpu_local_used_mb` | 8 (budget 7395); shared segment reported 0/0 |
| `handles` / `gdi_objects` | 512 / 28 |
| `topology_check_ms` | 2.1 mean, 6.0 session max |
| `build_ms` | 708 |
| `io_write_kb` | 2 per second — most of which is this log |

Read that wake-up row carefully: **every single wake-up in this state was a timeout that did no work.** The engine
wakes ~32 times a second to discover there is nothing to do, because the poll timeout is 20 ms while
disconnected. It is cheap (0.09% CPU) but it is not free, and it is the clearest quantified target in the file.

---

## `perf.jsonl` field reference

### Identity and configuration

| Field | Window | Meaning |
| --- | --- | --- |
| `at` | — | Local wall-clock time the line was written |
| `type` | — | Always `host-stats` |
| `seconds` | cumulative | Seconds since the engine started (survives pipeline rebuilds) |
| `capture_backend` | — | `wgc` or `dxgi`. Auto prefers WGC |
| `encoder`, `gpu` | — | MFT name and adapter description |
| `video_path`, `hardware_encoder`, `flush_gpu` | — | Always `GPU`/`true`/`false` in normal operation |
| `fps_setting` | — | The configured rate (60 or 30), not the achieved one |
| `viewer_connected` | interval | Whether a receiver was connected at the moment of the sample |

### Frame flow

| Field | Window | Meaning | Healthy | What a bad value means |
| --- | --- | --- | --- | --- |
| `capture_fps` | interval | Frames taken from the backend | ≈60 with a moving desktop; near 0 on a still one | See `no_change` before assuming a fault |
| `encode_fps` | interval | Frames out of the encoder | tracks `capture_fps` | Below it → look at the `dropped_*` split |
| `captured`, `encoded` | cumulative | Totals | — | A step down = new session |
| `no_change` | cumulative | Polls where the backend had no new frame | Large and growing on a still desktop | This is normal, not a loss |
| `paced` | cumulative | Frames deliberately thinned to honour a 30 fps setting | 0 at 60 fps | Non-zero at 60 fps is a bug |
| `dropped` | cumulative | Sum of the four causes below | — | Always read the split, never this alone |
| `dropped_coalesced` | cumulative | Superseded inside the capture backend before the loop saw them | small | The loop is not draining capture fast enough |
| `dropped_superseded` | cumulative | A newer frame arrived while this one waited for the encoder | small | The encoder is the bottleneck, not capture |
| `dropped_ring_busy` | cumulative | Every NV12 surface was still held by the encoder | ~0 | The encoder is more than a ring behind; raising the ring only adds latency |
| `dropped_submit_failed` | cumulative | The encoder refused the surface | 0 | An encoder fault, not back-pressure |

### Latency

All in milliseconds. Percentiles are rolling.

| Field | Window | Meaning | Healthy |
| --- | --- | --- | --- |
| `acquire_delay_ms_mean` / `_p95` | rolling | Compositor's frame stamp → the loop taking the frame | **Can be negative.** WGC's `SystemRelativeTime` is the frame's *vsync target*, not its capture instant, so an event-driven loop often has the frame ~11 ms before that stamp |
| `capture_ms_mean` / `_p95` / `_p99` | rolling | The same quantity in the historical column — but it adds a **0.0** whenever the backend gave no source stamp, so it is diluted. Prefer `acquire_delay_ms_*` | — |
| `convert_submit_ms_mean` | rolling | CPU time to issue the NV12 conversion (not GPU execution) | well under 1 ms |
| `gpu_command_span_ms_mean` | rolling | D3D11 timestamp span for the conversion. **Includes driver submission delay** — not a pure video-processor duration | — |
| `encode_ms_mean` / `_p95` / `_p99` | rolling | Submit → encoded output | ~5 ms for delta frames |
| `keyframe_encode_ms_mean` | rolling (256) | Same, keyframes only | 20–30 ms |
| `pipeline_ms_mean` / `_p95` | rolling | Frame's source stamp → encoded bitstream in hand | — |
| `source_to_encoded_ms_mean` / `_p95` | rolling | Compositor present → encoded bitstream. Host-side glass-to-bitstream; the honest end-to-end host figure | — |
| `submit_interval_ms_p95` / `_max` | rolling | Spacing between frames handed to the encoder. `_max` is the rolling window's max, **not** this second's | ~16.7 ms at 60 fps |
| `send_ms_mean` / `send_ms_max` | rolling / interval | Time inside `transport->send()`. `_max` resets each second | ≪1 ms |

### Bitstream

| Field | Window | Meaning | Healthy |
| --- | --- | --- | --- |
| `frame_bytes_mean` | rolling | Mean over **all** frames, keyframes included — a keyframe skews it for a minute | — |
| `keyframe_bytes_mean` | rolling (256) | Keyframes only | 240–300 KB at 8 Mbps CBR |
| `delta_bytes_mean` | rolling (256) | Delta frames only. **This is the one to watch for pixelation** | ~15 KB at 8 Mbps |
| `frame_bytes_max` | interval | Largest frame this second | ≈ keyframe size |
| `keyframes` | cumulative | Keyframes produced | +1 per 10 s, plus one per PLI/join/drop |
| `encoded_bits_per_second` | derived | `encode_fps × frame_bytes_mean × 8`. Approximate (the mean is rolling), but it is what the receiver actually gets | near `bitrate` |

### Bitrate and encoder churn

| Field | Window | Meaning |
| --- | --- | --- |
| `bitrate` | — | What the encoder is **actually** configured at |
| `target_bitrate` | — | What `BitrateController` wants |
| `dynamic_bitrate` | — | `false` once the encoder refused a live change. **On Intel this goes false almost immediately and stays false** |
| `encoder_rebuilds` | cumulative | Times the encoder was recreated. Each one is a visible hitch |
| `encoder_rebuild_ms_mean` | rolling (64) | How long the stream was dark per rebuild |
| `build_ms` | — | Cost of standing this pipeline up (device + capture + converter + encoder). ~710 ms here; `host.log` breaks it into the four stages, and the encoder stage usually dominates — which is why a rebuild is expensive rather than free |
| `first_encoded_ms`, `first_sent_ms` | — | From pipeline ready to the first frame out. `null` until it happens |

`BitrateController` (`host/include/core.hpp`) cuts to 75% when loss > 5%, smoothed loss > 2.5%, RTT > 250 ms or
jitter > 40 ms; it raises by 250 kbps after 8 consecutive good samples. Because `dynamic_bitrate` is false on this
encoder, the engine instead recreates it — but only after 5 s and only if the target is ≤ 75% or ≥ +2 Mbps of the
current rate. **That hysteresis is what produces rebuild oscillation; see "Known pathology" below.**

### Engine thread cost

This block is where idle CPU lives.

| Field | Window | Meaning | Healthy |
| --- | --- | --- | --- |
| `wakeups_per_s` | interval | Loop iterations per second | ~60–120 while streaming; ~20–50 while idle (the poll timeout is 50 ms connected, 20 ms not) |
| `loop_busy_percent` | interval | Share of wall time the thread was awake | low single digits |
| `woke_for_frame` | interval | Woken by a capture frame event | should dominate while streaming |
| `woke_for_encoder` | interval | Woken by the encoder (output ready, input wanted, surface freed) | — |
| `woke_for_transport` | interval | Woken by a signaling/telemetry/PLI message | a handful |
| `woke_for_timeout` | interval | Woke because the wait expired with nothing to do | should dominate only while idle |
| `woke_idle` | interval | Wake-ups that produced **neither** a frame nor an encoded frame | **The number to minimise.** High `woke_idle` with a still desktop is pure waste |
| `loop_max_ms` | interval | Longest single iteration this second | a few ms; a spike usually means the topology check or a keyframe |
| `queue_depth` | instant | Frames inside the encoder | 0–3 (bounded at 3) |
| `topology_check_ms_mean` / `_max` | rolling (256) / **session max** | The full DXGI re-enumeration that runs every 10 s on this thread | 2.1 ms mean, 6.0 ms worst here — a real stall on the frame thread, now quantified rather than assumed |

### Process resources

From `host/src/resources.cpp`. All of the app's threads, not just the engine.

| Field | Window | Meaning |
| --- | --- | --- |
| `cpu_percent` | interval | Percent of the **whole machine's** capacity (all 8 logical processors), not of one core |
| `cpu_kernel_percent` / `cpu_user_percent` | interval | The split. A high kernel share points at syscalls, driver work or I/O rather than computation |
| `working_set_mb`, `private_mb`, `peak_working_set_mb` | instant | Resident, committed, and high-water memory |
| `page_faults_delta` | interval | Steady growth means the working set is being trimmed under memory pressure |
| `io_read_kb`, `io_write_kb` | interval | File I/O. Logging itself is in here — ~1.5 KB/s of it is `perf.jsonl` |
| `handles`, `gdi_objects`, `user_objects` | instant | Leak detectors. Monotonic growth over hours is the signal; the absolute value is not |
| `gpu_local_used_mb` / `_budget_mb` | instant | The adapter's `DXGI_MEMORY_SEGMENT_GROUP_LOCAL` usage / budget |
| `gpu_shared_used_mb` / `_budget_mb` | instant | The `NON_LOCAL` segment. **Read both — which one an integrated GPU uses is a driver decision.** On this Iris Xe the driver reports everything in the *local* segment (8 MB used of a 7395 MB budget) and 0/0 shared, not the other way around |
| `on_battery`, `battery_saver`, `battery_percent` | instant | Explains a sudden throughput change that has nothing to do with the code. Check this *first* when numbers look inexplicably bad |

The CPU fields are deltas, so the meter is primed once before the loop starts and again in the window's
constructor; they are `null` only if `GetProcessTimes` itself fails. The GPU fields are absent (not null) when the
adapter does not expose `IDXGIAdapter3`.

### Transport (`webrtc` sub-object)

| Field | Window | Meaning |
| --- | --- | --- |
| `webrtc.sent_frames`, `.encoded_bytes_sent` | cumulative | What reached the wire |
| `webrtc.transport_dropped` | cumulative | Frames refused because the send buffer was over 128 KB or the peer was down |
| `webrtc.transport_buffer_bytes` | instant | Currently queued |
| `webrtc.transport_buffer_pressure` | cumulative | Frames handed to the transport while over 32 KB was still queued (counted before the 128 KB refusal, so it includes frames then dropped) — latency the receiver feels |
| `webrtc.keyframe_requests` / `.keyframes_sent` | cumulative | PLIs received / keyframes emitted. Each PLI costs a 240–300 KB, 20–30 ms frame |
| `webrtc.bitrate_cuts` / `.bitrate_raises` | cumulative | Controller decisions |
| `webrtc.smoothed_loss` / `.smoothed_rtt_ms` | rolling | The EWMA values the controller actually decided on — not the raw sample |
| `webrtc.receiver_estimate_bps` | instant | The browser's REMB estimate. **Diagnostics only; nothing acts on it** |
| `webrtc.route` | instant | Selected ICE pair, e.g. `host -> host over UDP`. A `relayed` or TCP pair explains latency that is not the encoder's fault |
| `webrtc.timings` | — | `answer_ms`, `connected_ms`, `first_sent_ms`, `first_keyframe_ms`, `keyframe_response_ms`, all from the start of this peer connection |

### Receiver telemetry

The browser samples every second and sends it over the data channel; the host accepts at most one per 500 ms.
Every value is an **interval delta computed in the browser**, or `null` where that browser does not publish the
counter. `null` means *unavailable*, never *zero* — do not treat a missing counter as a clean picture.

Hoisted to the top level for convenience: `receiver_qp`, `receiver_corrupted`, `receiver_freezes`, `receiver_pli`.
The full set is under `webrtc.receiver`.

| Field | Meaning | Healthy |
| --- | --- | --- |
| `qp` | Mean quantizer the decoder saw. **This is pixelation itself, measured** | 20–30; over ~36 is visibly blocky |
| `corrupted` | Frames that arrived but never decoded | 0 |
| `freezes` | Freeze events | 0 |
| `pli`, `nack` | Recovery requests the receiver sent | 0 |
| `keyFramesDecoded` | Keyframes decoded in the interval | matches the host's keyframe rate |
| `fps`, `bitrate`, `loss`, `rttMs`, `jitterMs` | The usual link numbers | — |
| `jitterBufferMs` | Time frames waited before rendering | near 0 — the playout-delay extension is set to 0/0 |
| `decodeMs`, `processingMs` | Receiver-side decode and packet-to-frame delay | — |
| `dropped`, `decoded` | Frame counts | — |

---

## `host.log` line catalogue

Grep-able landmarks, in roughly the order a session produces them.

**Startup**

```
Laptop Monitor 0.2.0 starting
Machine: 8 logical processors | 15 GB RAM (86% in use) | Windows 25H2 build 26200 | on battery
Settings: 60 fps | quality balanced | capture auto | scale 150% | ... | signaling <url>
Diagnostics: <path>\host.log and <path>\perf.jsonl (one performance sample per second)
Startup: settings and credential ready N ms after launch
Startup: window ready N ms after launch
```

The machine and settings lines are the baseline every later number is relative to. **Always read them before
comparing two logs** — a 20%-battery machine in battery saver is not the same machine.

**Bringing the display and pipeline up**

```
Virtual display device created (...) in 110 ms (scheduled task 0 ms, helper pipe 110 ms, device 0 ms)
Pipeline: wgc -> GPU NV12 -> Intel® Quick Sync ... | 1920x1080@60 | CPU readback: no | mode stream
Pipeline built in N ms (device N, capture N, NV12 converter N, encoder N) at N bps
Streaming engine thread scheduling: MMCSS Capture
```

If the scheduling line says `above-normal priority` instead of `MMCSS Capture`, MMCSS was unavailable and a busy
desktop can starve the capture thread for a scheduler quantum — that changes how to read every latency number
below it.

**Periodic digests** (every 30 s while streaming; the full sample is in `perf.jsonl`)

```
Stream: capture 60.0 fps, encode 60.0 fps | encode 5.1 ms (p95 7.0) | present to encoded 12.3 ms | queue 1 | ...
Engine thread: 118 wake-ups/s (60 frame, 55 encoder, 3 transport, 0 timeout; 4 did nothing), busy 6.1% ...
Frames not encoded: 12 dropped (8 coalesced, 4 superseded, 0 ring busy, 0 refused), 0 paced out, ...
Receiver: 60.0 fps | 14.0 ms rtt | 0.01% loss | ... | quantizer 26 | corrupted 0 | freezes 0 | ...
```

**Every 2 minutes, whether or not the window is visible**

```
App process: 0 window paints in 120 s (0 while hidden), mean 0.00 ms, longest 0.00 ms | cpu 0.4% ... | AC
```

This is the record of what the app costs sitting in the tray — most of its life, and the part nobody measures.

**Events worth grepping for**

| Pattern | Meaning |
| --- | --- |
| `Recreating hardware encoder` | A rebuild, with the receiver numbers that caused it |
| `Encoder rebuild interrupted the stream for` | How long the picture was actually gone |
| `Bitrate target cut` | Controller reduced the target (rate-limited to one line per 5 s) |
| `Receiver asked for N keyframes in` | PLI storm — each one is a full intra frame |
| `Send queue was still draining` | The link cannot carry the current bitrate |
| `Picture quality dropped` / `Picture quality recovered` | See below |
| `Pipeline session ended after` | One-line totals for the session that just ended |
| `Capture source timestamps are not comparable` | Source stamps were discarded; `acquire_delay_ms_*` is then empty |
| `Display topology changed` | The pipeline was torn down and rebuilt; counters restart |

## Picture quality episodes

`QualityWatch` in `host/src/pipeline.cpp` exists for one specific complaint: *"it goes extremely pixelated and
glitchy, then fixes itself."* Those are two different faults that look alike and are both over before anyone can
look at a dashboard.

- **Pixelation** — the encoder has too few bits for 1080p, raises the quantizer, and the picture turns to blocks.
  Seen as `delta_bytes_mean` and `encoded_bits_per_second` collapsing, and `receiver_qp` climbing.
- **Glitching** — packets were lost, so the decoder shows torn and smeared blocks until a keyframe repairs it.
  Seen as `receiver_corrupted`, `receiver_freezes` and `keyframe_requests`.

The watch opens an episode when the receiver's quantizer exceeds 36, or the encoded rate falls below 55% of the
healthy baseline (the mean of the last 64 healthy seconds, armed after 10 samples), or frames are corrupted or
frozen. It closes after 3 consecutive clean seconds and writes one attributed line:

```
[warn ] Picture quality dropped: encoded rate fell to 1.4 Mbps from a usual 7.1 Mbps | encoder 1500 kbps ...
[info ] Picture quality recovered after 14 s (started: ...) | worst quantizer 44, lowest encoded rate 1.31 Mbps |
        encoder was recreated 2 time(s) at a lower bitrate: too few bits for this resolution
```

It deliberately survives a pipeline rebuild, because a rebuild is usually *part* of the episode rather than the
end of it. The thresholds are heuristics chosen from one machine — treat a single episode as a pointer to the
`perf.jsonl` window around it, not as a verdict.

## Known pathology, not yet fixed

Visible in the logs **before** any of this instrumentation existed, and left alone deliberately:

> The Intel MFT reports `AVEncCommonMeanBitRate` as not modifiable while streaming, so `dynamic_bitrate` goes
> false and every adaptation becomes a full encoder rebuild. `BitrateController` then oscillates, and the 5-second
> hysteresis is short enough to let it: the log shows the encoder recreated every ~5 s, alternating roughly
> 1.5 ↔ 3.5 Mbps. 1080p at 1.5 Mbps is blocky, and each rebuild is a visible hitch — which matches the
> pixelation complaint exactly.

Confirm it from a fresh session before acting: correlate `encoder_rebuilds` stepping up with `receiver_qp` rising
and `delta_bytes_mean` collapsing in the same seconds. If that correlation holds, the fault is in the adaptation
policy, not in capture, conversion or the encoder's throughput.

## What is *not* measured

Do not infer these from anything above:

- **GPU utilisation.** Only GPU *memory* is sampled. There is no engine-busy percentage; `gpu_command_span_ms_mean`
  includes driver submission delay and is not a substitute.
- **Per-thread CPU.** `cpu_percent` covers the whole process. The engine thread's share can only be bounded by
  `loop_busy_percent`, which is wall-clock occupancy, not CPU time.
- **Anything in the elevated helper or the driver.** `LaptopMonitorDisplay.exe` reports over its pipe and is not
  sampled; the IddCx driver is not instrumented at all.
- **The receiver's machine.** Only what the browser's `getStats()` publishes crosses the data channel.
- **Network path.** RTT, loss and the ICE pair type are all that is known; there is no path MTU, pacing or
  congestion-window visibility.

## Where the code is

| Concern | File |
| --- | --- |
| Both log channels, rotation, the record writer | `host/include/logging.hpp`, `host/src/logging.cpp` |
| Log location (`%TEMP%\LaptopMonitor`) | `logDirectory()` in `host/src/settings.cpp` |
| Process/GPU/power sampling | `host/include/resources.hpp`, `host/src/resources.cpp` |
| Per-second record, loop accounting, `QualityWatch` | `host/src/pipeline.cpp` |
| Snapshot shared with the GUI | `host/include/metrics.hpp` |
| Transport counters, ICE route, pressure reporting | `host/src/transport.cpp` |
| Bitrate policy | `BitrateController` in `host/include/core.hpp` |
| Receiver-side quality counters | `viewer/src/telemetry.ts`, `Telemetry` in `shared/protocol.ts` |
| Window paint cost | `reportUiCost()` in `host/app/ui/main_window.cpp` |

Adding a field: put it in the `stats` object in `pipeline.cpp` (it reaches `perf.jsonl`, the bench JSON sink and
the receiver's dashboard at once), add it to `MetricsSnapshot` only if the GUI or *Copy diagnostics* needs it, and
add it to the 30-second digest only if someone reading `host.log` for a story would miss it. Keep the readable log
readable; the record file is where volume belongs.
