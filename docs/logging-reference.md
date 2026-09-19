# Logging reference

What Laptop Monitor records, what each number means, and what a bad value points at. Written for whoever picks up
optimization work next: read this and the logs instead of re-instrumenting the pipeline.

The instrumentation is **reporting only**. Nothing described here changes what the pipeline does, and nothing here
should be turned into a control loop without saying so explicitly.

## Where the logs are

Two places, both written only while Settings → *diagnostics log* is on (the default), and neither ever containing a
pairing code, the host credential, a ticket, a token or an SDP. Uninstall removes both.

| Location | Kept | Content |
| --- | --- | --- |
| `%TEMP%\LaptopMonitor\` (current logs) | Rolling: each file replaces one backup; Windows may also clean Temp | Every run appends to the same two files |
| `%LOCALAPPDATA%\LaptopMonitor\logs\sessions\<run_id>\` (archive) | Until uninstall or someone deletes a folder by hand. A later run never deletes an earlier one | One folder per run of the app with diagnostics on |

The Details page's *Open log folder* offers all three: **Current logs** (`%TEMP%\LaptopMonitor`), **This run's
archive**, and **All archived runs** (the `sessions` folder). *Copy diagnostics* includes the run id and this run's
archive path.

### Current logs

| File | Size | Content |
| --- | --- | --- |
| `host.log` | 2 MB, rotates to `host.1.log` | Readable lines: lifecycle, state changes, problems, periodic digests. Several runs back to back; each begins with `Laptop Monitor <version> starting` and `Run <run_id>` |
| `perf.jsonl` | 8 MB, rotates to `perf.1.jsonl` | One JSON object per second while the streaming engine runs |
| `setup.log` | 2 MB | The elevated `--setup` / `--uninstall` runs only (never archived) |

At 4-6 KB per record, `perf.jsonl` holds roughly 30 minutes of streaming before rotation throws the oldest half
away. That is what the archive is for.

### Session archive

```
%LOCALAPPDATA%\LaptopMonitor\logs\sessions\
  20260918-171419-17960\          <run_id>: local start time, then the process id
    session.json                  what the run was and how it ended (below)
    host.log                      the same lines as the rolling host.log, from when diagnostics were on
    perf.jsonl                    the same records as the rolling perf.jsonl
    host.001.log, perf.001.jsonl  full segments, oldest first, when a run outgrows one file
```

Every line and record is written to the rolling file and to the archive in the same call, stamp included, so a
line in one is byte-identical to the line in the other. Within one run the archive is bounded: `host.log` becomes
a numbered segment at 8 MB and `perf.jsonl` at 32 MB, and only the newest 3 (host) and 7 (perf) segments are kept
beside the live file - about 12 hours of continuous streaming. The oldest segment *of that run* is removed past
the cap; no run ever touches another run's folder. Read segments in number order, then the live file:
`perf.001.jsonl perf.002.jsonl ... perf.jsonl`.

The folder is created when diagnostics are on at startup, or the moment they are switched on mid-run. Switched
off and on again in the same run, it reopens the same folder and appends (`diagnostics_periods` lists each
stretch). The startup line `Session archive: ... | N runs kept in ..., M MB in total` says how large the archive
has grown; nothing prunes it automatically.

#### session.json

| Field | Meaning |
| --- | --- |
| `schema` | 1 |
| `run_id`, `pid`, `process_created` | The run's identity. `process_created` (Windows FILETIME) tells a reused pid apart |
| `app`, `version` | `Laptop Monitor`, the build version |
| `started_at`, `started_at_utc` | Process start (local, same format as `at`; and ISO UTC) |
| `diagnostic_tag` | The `--diagnostic-tag` of the run, or null |
| `launch` | `background` (started hidden, as at sign-in) and the tag argument as given |
| `machine` | Logical processors, RAM, memory load, Windows version and build, AC/battery, and every hardware GPU with vendor/device id, memory and **driver version** |
| `settings` | The settings when the archive opened. Pairing data never appears; a signaling URL loses any `user:password@` |
| `settings_changes` | `[{at, settings}]` for every change made during the run |
| `diagnostics_periods` | `[{on, off}]`: when this run was being recorded |
| `termination` | How the run (or its recording) ended, below |
| `ended_at`, `duration_s` | End time, and seconds from process start to it |
| `pipeline_sessions`, `engine_sessions`, `perf_records` | Totals for the run, written at close |
| `exit_code` / `fatal_error` / `windows_end_session` | Present with the matching termination |
| `ended_at_source`, `reconciled_at`, `reconciled_by` | Present on `abnormal` runs: the end is an estimate, made by a later run |

| `termination` | Meaning |
| --- | --- |
| `running` | The archive is open. Seen afterwards, the process died before the next launch looked |
| `graceful` | The application exited normally |
| `diagnostics_off` | Logging was switched off; the run itself went on |
| `windows_session_end` | Sign-out, shutdown or restart (`windows_end_session` says which); written before Windows ends the process |
| `uninstall` | Closed so uninstall could delete it (you will rarely see this: uninstall deletes the folder) |
| `fatal` | Startup failed; `fatal_error` says why |
| `abnormal` | Found still `running` by a later launch after its process (same pid **and** creation time) was gone: a crash, a forced kill or power loss. `ended_at` is the newest file write in its folder |

`abnormal` is decided only when the process is certainly gone; a process that exists but cannot be queried is
treated as alive. The later run logs `Earlier run <run_id> ended without closing its log`.

### Run, engine and pipeline identifiers

Every `perf.jsonl` record carries four identifiers, so a record stands on its own even when files from many runs
are concatenated:

| Field | Scope | Changes when |
| --- | --- | --- |
| `run_id` | Process | Never within a run. Equals the archive folder name. The bench has one too (its own process) |
| `engine_session` | Run | The streaming engine starts: Start streaming, a settings change that restarts the stream. `seconds` restarts with it |
| `pipeline_session` | Run (unique across engines) | The capture/converter/encoder pipeline is built. **Every cumulative counter restarts at exactly this boundary** |
| `pipeline_build_reason` | Pipeline session | Why it was built: `start` (first build of this engine), `encoder_rebuild` (bitrate change on an encoder that ignores live changes), `recovery` (after an error: topology change, stalled encoder, lost capture) or `display_returned` (the display vanished and came back) |

`pipeline_session_seconds` is the time since that pipeline became ready. A step down in `captured` together with a
new `pipeline_session` is a rebuild, not lost frames; a step down *without* one would be a bug.

The same numbers are in `host.log`: `Pipeline built in ... | pipeline session 3 (encoder_rebuild), engine session 1`
and `Pipeline session ended after 41 s (pipeline session 3): ...`.

### Labelling a controlled test

```powershell
# Exit Laptop Monitor from its tray icon first: a tag belongs to a whole run.
.\build\host\LaptopMonitor.exe --diagnostic-tag scrolling      # or with --background, wherever the app lives
.\build\host\laptop-monitor-bench.exe --laptopmon --mode capture-encode --seconds 60 --diagnostic-tag benchmark-1
```

A tag is 1-48 letters, digits, `.`, `_` or `-`. It goes into `session.json` and into every record as
`diagnostic_tag` (absent when untagged). The app refuses an invalid tag with a message and runs untagged; a second
launch with a tag while the app is already running says it cannot apply it. The bench puts the tag (and its
`run_id`) into the JSON lines it prints; its CSV columns are unchanged.

### Getting a session to look at

```powershell
# Reproduce with the real app: start it, start the monitor, pair a receiver, use it, then read the file.
Get-Content "$env:TEMP\LaptopMonitor\perf.jsonl" -Tail 60

# Or without the GUI, on a chosen display (run from PowerShell; bash mangles \\.\DISPLAY2):
.\build\host\laptop-monitor-bench.exe --laptopmon --capture wgc --mode stream --seconds 120 --csv out.csv
```

The bench writes console, CSV and its JSON sink; it does **not** write `perf.jsonl` or an archive. Use the app when
you want the files.

Useful passes over a session (`jq` reads JSON Lines directly):

```bash
jq -r '[.at, .capture_fps, .encode_fps, .encode_ms_mean, .cpu_percent, .loop_busy_percent] | @tsv' perf.jsonl
jq -r 'select(.receiver_qp != null and .receiver_qp > 36) | [.at, .receiver_qp, .bitrate] | @tsv' perf.jsonl
jq -s 'map(.cpu_percent | numbers) | add / length' perf.jsonl      # mean process CPU over the session
jq -r 'select(.woke_idle > 40) | [.at, .woke_idle, .wakeups_per_s] | @tsv' perf.jsonl
# Where one frame's time goes, on the host clock only:
jq -r '[.at, .host_acquire_to_convert_ms_mean, .host_acquire_to_encode_submit_ms_mean,
        .host_acquire_to_encoded_ms_mean, .host_acquire_to_send_ms_mean, .host_acquire_to_send_ms_max] | @tsv' perf.jsonl
# Still desktop or stalled capture:
jq -r '[.at, .capture_fps, .source_frame_gap_ms_max, .ms_since_last_source_frame, .user_input_idle_ms,
        .cursor_on_display] | @tsv' perf.jsonl
```

### Comparing runs

```powershell
$sessions = "$env:LOCALAPPDATA\LaptopMonitor\logs\sessions"
Get-ChildItem $sessions -Directory | ForEach-Object { Get-Content "$($_.FullName)\session.json" -Raw | ConvertFrom-Json } |
  Select-Object run_id, diagnostic_tag, started_at, duration_s, termination, perf_records, pipeline_sessions |
  Format-Table
```

```bash
cd "$LOCALAPPDATA/LaptopMonitor/logs/sessions"
jq -r '[.run_id, .diagnostic_tag, .termination, .duration_s, .machine.gpus[0].driver_version] | @tsv' */session.json
# One number per run, streaming seconds only (segments first, then the live file, so records stay in order):
for d in */; do cat "$d"perf.[0-9]*.jsonl "$d"perf.jsonl 2>/dev/null | jq -s --arg run "${d%/}" '
  map(select(.viewer_connected)) | {run: $run, tag: (.[0].diagnostic_tag // "-"), seconds: length,
   capture_fps: (map(.capture_fps) | add / length),
   send_p95_ms: (map(.host_acquire_to_send_ms_p95 | numbers) | add / length),
   gpu_video_decode: (map(.gpu_engine_video_decode_percent | numbers) | add / length)}'; done
# All records of one tag, across runs:
cat */perf*.jsonl | jq -c 'select(.diagnostic_tag == "scrolling")'
```

Compare like with like. Before comparing two runs, check `machine` (battery, driver version), `settings`, and that
both were streaming at the same `fps_setting`. Within a run, split by `pipeline_session` before averaging anything
cumulative, and use `webrtc.receiver_age_ms` to drop repeated receiver samples (see *Receiver telemetry*).

## How to read the numbers

Several accumulations are mixed in one record. Getting this wrong is the easiest way to draw a false conclusion,
so check the **Window** column before comparing two records.

| Window | Meaning |
| --- | --- |
| **cumulative** | Since this pipeline *session* began (`pipeline_session`). A session ends on stop, on an encoder rebuild, or on any pipeline error, and the counter restarts from zero. A step down in `captured` means a new session, not lost frames. |
| **interval** | Just the last second (the time between two records). |
| **interval max** | The largest single value measured in the last second; null when nothing was measured in it. |
| **rolling** | A ring of the last N samples, *not* the last second. `Samples<>` defaults to N=4096 — at 60 fps that is roughly the last 68 seconds. Percentiles and most means are rolling. A rolling mean lags a change by a minute; do not read it as "right now". |
| **instant** | The value at the moment the record was written. |
| **session max** | Highest value since the session began, never reset. |

Ring sizes: 4096 for the frame-path samples (the host-clock latencies included), 256 for keyframe/delta sizes and
topology checks, 64 for encoder rebuild durations. A new pipeline session starts every ring empty.

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

### Measured baseline: streaming a moving source

2026-09-18, bench `--mode stream --pattern` (a bar moving every refresh) on a secondary 1080p60 display, receiver in
Chrome on the same machine over localhost:

| | |
| --- | --- |
| `capture_fps` / `encode_fps` | 59.5 / 59.5 |
| `source_frame_gap_ms_p95` / `_max` | 17-24 / 18-34 |
| `host_acquire_to_encode_submit_ms_mean` | 0.9 |
| `host_acquire_to_encoded_ms_mean` / `_p95` | 5.7 / 6.3 |
| `host_acquire_to_send_ms_mean` / `_p95` / `_p99` / `_max` | 6.2 / 6.9 / 7.4 / 8.6 |
| `encoded_to_send_ms_mean` / `send_ms_mean` | 0.55 / 0.54 |
| `encoder_output_pickup_ms_mean` / `_p95` | 0.1 / 0.2 |
| `source_to_encoded_ms_mean` (compositor stamp, for contrast) | **−8.2** |
| `gpu_engine_video_decode_percent` / `_video_processing_` / `_3d_` | 18 / 9 / 2 (3D 6% for all processes) |
| `resource_sample_ms` | 0.2-0.4 |
| receiver `interFrameDelayMs` ± `interFrameDelayStdMs` | 16.8 ± 1.4 |

The same display left still (something on it redraws about twice a second): `capture_fps` 2-3,
`source_frame_gap_ms_max` ≈ 520, `ms_since_last_source_frame` 0-500, `host_acquire_to_encoded_ms_mean` 6.5-7
(the GPU clocks down between frames), and `acquire_delay_ms_mean` drifting from +57 to −2 ms over twelve seconds.

---

## `perf.jsonl` field reference

### Identity and configuration

| Field | Window | Meaning |
| --- | --- | --- |
| `at` | — | Local wall-clock time the line was written |
| `type` | — | Always `host-stats` |
| `run_id`, `engine_session`, `pipeline_session`, `pipeline_build_reason` | — | See *Run, engine and pipeline identifiers* above |
| `pipeline_session_seconds` | instant | Seconds since this pipeline became ready |
| `diagnostic_tag` | — | The run's `--diagnostic-tag`; absent when untagged |
| `seconds` | cumulative | Seconds since the engine started (survives pipeline rebuilds; restarts with `engine_session`) |
| `capture_backend` | — | `wgc` or `dxgi`. Auto prefers WGC |
| `encoder`, `gpu` | — | MFT name and adapter description |
| `video_path`, `hardware_encoder`, `flush_gpu` | — | Always `GPU`/`true`/`false` in normal operation |
| `fps_setting` | — | The configured rate (60 or 30), not the achieved one |
| `viewer_connected` | instant | Whether a receiver was connected at the moment of the sample |

### Frame flow

| Field | Window | Meaning | Healthy | What a bad value means |
| --- | --- | --- | --- | --- |
| `capture_fps` | interval | Frames taken from the backend | ≈60 with a moving desktop; near 0 on a still one | See *Source activity* before assuming a fault |
| `encode_fps` | interval | Frames out of the encoder | tracks `capture_fps` | Below it → look at the `dropped_*` split |
| `captured`, `encoded` | cumulative | Totals | — | A step down = new `pipeline_session` |
| `no_change` | cumulative | Polls where the backend had no new frame | Large and growing on a still desktop | This is normal, not a loss |
| `paced` | cumulative | Frames deliberately thinned to honour a 30 fps setting | 0 at 60 fps | Non-zero at 60 fps is a bug |
| `dropped` | cumulative | Sum of the four causes below | — | Always read the split, never this alone |
| `dropped_coalesced` | cumulative | Superseded inside the capture backend before the loop saw them | small | The loop is not draining capture fast enough |
| `dropped_superseded` | cumulative | A newer frame arrived while this one waited for the encoder | small | The encoder is the bottleneck, not capture |
| `dropped_ring_busy` | cumulative | Every NV12 surface was still held by the encoder | ~0 | The encoder is more than a ring behind; raising the ring only adds latency |
| `dropped_submit_failed` | cumulative | The encoder refused the surface | 0 | An encoder fault, not back-pressure |

### Source activity

`no_change` stays the headline signal for a still WGC desktop, but on its own it cannot tell "nothing on the screen
changed" from "capture stopped delivering frames", and it is inflated by every encoder wake-up (a busy 60 fps stream
shows *more* no-change polls, ~165/s, than a still desktop, ~20/s). These fields answer the question per interval,
on the host's steady clock. They are diagnostics only; nothing acts on them.

| Field | Window | Meaning |
| --- | --- | --- |
| `no_change_interval` | interval | `no_change` over the last second |
| `repeat_frames_interval` | interval | Re-encodes of the last surface: the once-a-second keep-alive of a still desktop, or an answer to a keyframe request without new content. Not source frames, and never traced |
| `source_frame_gap_ms_p95` / `_max` | interval (gaps that *ended* in the second) | Spacing between consecutive new source frames, taken when the backend handed each frame to the engine. Null when fewer than two frames bracket a gap in the second |
| `ms_since_last_source_frame` | instant | Age of the newest source frame at the record. Null before the first frame of a pipeline session, and while no receiver is connected (capture is not polled then, so the gap would measure that, not the source) |
| `user_input_idle_ms` | instant | Time since the last keyboard or mouse input anywhere in the Windows session (`GetLastInputInfo`) |
| `cursor_on_display` | instant | Whether the pointer is on the captured display. Capture includes the cursor, so a moving pointer there must produce frames. Null when Windows would not say (secure desktop) |

A stall still in progress shows in `ms_since_last_source_frame`, not in `source_frame_gap_ms_max` (no gap has ended
yet); the frame that ends it reports the whole stall as the gap. Gaps never span a pipeline rebuild or a period
without a receiver.

| Seen together | Reading |
| --- | --- |
| `capture_fps` ≈ 0, `ms_since_last_source_frame` growing, `user_input_idle_ms` just as large | Nobody is touching anything: a still desktop. Not a fault |
| `capture_fps` ≈ 0, `ms_since_last_source_frame` growing, `user_input_idle_ms` small **and** `cursor_on_display` true | The pointer is moving on the captured display yet no frames arrive: capture has stalled |
| `capture_fps` low and steady, `source_frame_gap_ms_max` ≈ constant (500 ms, 1000 ms) | Something on the display redraws on a timer (a caret, a clock); the desktop is otherwise still |
| `capture_fps` high, then one `source_frame_gap_ms_max` of seconds | A capture hiccup (or the compositor stopped presenting) that ended; check `host.log` around it |
| `user_input_idle_ms` small, `cursor_on_display` false | Input is going to another display; says nothing about this one |

Video playing on a still desktop produces frames without input, so large `user_input_idle_ms` with steady frames
is normal.

### Latency

All in milliseconds. Two clocks are involved, and they are never mixed in one number.

**Host-clock latency.** Every new source frame gets a sequence number and its host acquisition time
(`steady_clock`, taken when the capture backend hands the frame to the engine). A `FrameTrace` travels with the
frame into the encoder's own bookkeeping (keyed by the sample time it already uses for `encode_ms`) and comes back
on the encoded output, so each stage below is measured on *the same frames* on *one monotonic clock*. Samples are
taken when a traced frame leaves the encoder and when it is handed to the transport. Repeats of the last surface are
not traced. Every field has `_mean`, `_p95`, `_p99` over the rolling ring (null until the first sample) and `_max`
over the interval (null when no frame was measured in it).

| Field | From → to | What a bad value means |
| --- | --- | --- |
| `host_acquire_to_convert_ms_*` | acquired → NV12 conversion issued | The frame waited for the encoder (a carried frame) or for this thread. Healthy ≈ 0 |
| `host_acquire_to_encode_submit_ms_*` | acquired → encoder accepted the surface | Adds the conversion's CPU submit; the gap from the line above is `convert_submit_ms` |
| `host_acquire_to_encoded_ms_*` | acquired → encoded bitstream in hand on the engine thread | The host-side glass-to-bitstream on one clock. Minus the line above ≈ `encode_ms` |
| `host_acquire_to_send_ms_*` | acquired → `transport->send()` returned | Everything the host adds. Stream mode only |
| `encoded_to_send_ms_*` | bitstream in hand → `send()` returned | Output-side cost: send time plus other frames handled in the same encoder poll. Every sent frame, repeats included |
| `encoder_output_pickup_ms_*` | the MFT's HaveOutput event reached our callback → the engine thread had copied the bitstream out | How long a finished frame waited for this thread. `encode_ms` minus this is the encoder's own time. High pickup with normal encode = host scheduling, not the GPU |
| `untraced_outputs_interval` (interval count) | — | Encoder outputs whose sample time matched no submission. Their stages are not guessed; expected 0 |

Correlation limits, stated plainly: the encoder stage relies on the MFT returning the input's sample time on its
output (the existing `encode_ms` relies on the same). The pickup stage pairs HaveOutput events with outputs in
order, which the asynchronous MFT contract guarantees (one `ProcessOutput` per event); if the event ring ever
overflowed, those outputs get no pickup sample rather than a wrong one. "Sent" means handed to libdatachannel:
packetization and the socket write happen inside that call, but the time on the wire and in the network is not
measured here (see the receiver's `jitterMs`, `rttMs`). The time a frame spent in the WGC frame pool *before* the
engine took it cannot be measured on the host clock: the only stamp for that is the compositor's own.

**Compositor-stamp latency** (the historical fields, unchanged). These start from the frame's source stamp - on WGC
that is `SystemRelativeTime`, the frame's *vsync target*, not its capture instant - so they can be negative and
drift. Measured 2026-09-18: `source_to_encoded_ms_mean` −8.2 ms while `host_acquire_to_encoded_ms_mean` was 5.7 ms
for the same frames. Use them to compare with older logs; use the host-clock fields to attribute time.

| Field | Window | Meaning | Healthy |
| --- | --- | --- | --- |
| `acquire_delay_ms_mean` / `_p95` | rolling | Compositor's frame stamp → the loop taking the frame | **Can be negative.** An event-driven loop often has the frame ~11 ms before its vsync-target stamp |
| `capture_ms_mean` / `_p95` / `_p99` | rolling | The same quantity in the historical column — but it adds a **0.0** whenever the backend gave no source stamp, so it is diluted. Prefer `acquire_delay_ms_*` | — |
| `convert_submit_ms_mean` | rolling | CPU time to issue the NV12 conversion (not GPU execution) | well under 1 ms |
| `gpu_command_span_ms_mean` | rolling | D3D11 timestamp span for the conversion. **Includes driver submission delay** — not a pure video-processor duration | — |
| `encode_ms_mean` / `_p95` / `_p99` | rolling | Submit → encoded output (host clock, includes the pickup delay above) | ~5 ms for delta frames |
| `keyframe_encode_ms_mean` | rolling (256) | Same, keyframes only | 20–30 ms |
| `pipeline_ms_mean` / `_p95` | rolling | Frame's source stamp → encoded bitstream in hand | — |
| `source_to_encoded_ms_mean` / `_p95` | rolling | Compositor present → encoded bitstream | can be negative on WGC |
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
| `encoder_rebuilds` | cumulative (engine) | Times the encoder was recreated. Each one is a visible hitch, and each starts a new `pipeline_session` with reason `encoder_rebuild` |
| `encoder_rebuild_ms_mean` | rolling (64) | How long the stream was dark per rebuild |
| `build_ms` | — | Cost of standing this pipeline up (device + capture + converter + encoder). ~710 ms here; `host.log` breaks it into the four stages, and the encoder stage usually dominates — which is why a rebuild is expensive rather than free |
| `first_encoded_ms`, `first_sent_ms` | — | From pipeline ready to the first frame out. `null` until it happens |

`BitrateController` (`host/include/core.hpp`) cuts to 75% when loss > 5%, smoothed loss > 2.5%, RTT > 250 ms or
jitter > 40 ms; it raises by 250 kbps after 8 consecutive good samples **that also used at least 75% of the
current target bitrate**. Demand is successful encoded bytes sent divided by host elapsed time between admitted
telemetry messages (0.5-3 seconds). The first sample after peer reset, stale intervals and quiet intervals do not
earn growth and reset the consecutive-good count. These sender counters now explicitly control upward adaptation;
REMB, receiver throughput and the other diagnostic fields remain reporting only. Cuts do not require demand.
This avoids rebuilding a lightly used encoder just because an idle link has low loss and RTT.
Because `dynamic_bitrate` is false on this
encoder, the engine instead recreates it — but only after 5 s and only if the target is ≤ 75% or ≥ +2 Mbps of the
current rate. Demand gating reduces unnecessary upward rebuilds; it does not fix all loss-driven oscillation
described under "Known pathology" below.

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
| `io_read_kb`, `io_write_kb` | interval | File I/O. Logging itself is in here — ~1.5 KB/s of it is `perf.jsonl`, twice that with the archive mirroring it |
| `handles`, `gdi_objects`, `user_objects` | instant | Leak detectors. Monotonic growth over hours is the signal; the absolute value is not |
| `gpu_local_used_mb` / `_budget_mb` | instant | The adapter's `DXGI_MEMORY_SEGMENT_GROUP_LOCAL` usage / budget |
| `gpu_shared_used_mb` / `_budget_mb` | instant | The `NON_LOCAL` segment. **Read both — which one an integrated GPU uses is a driver decision.** On this Iris Xe the driver reports everything in the *local* segment (8 MB used of a 7395 MB budget) and 0/0 shared, not the other way around |
| `gpu_engine_<class>_percent` | interval | GPU engine time of **this process**, below |
| `gpu_engine_<class>_system_percent` | interval | GPU engine time of **every process** on the adapter, below |
| `resource_sample_ms` | instant | What taking this whole block cost the engine thread (0.2-0.4 ms a second here) |
| `on_battery`, `battery_saver`, `battery_percent` | instant | Explains a sudden throughput change that has nothing to do with the code. Check this *first* when numbers look inexplicably bad |

The CPU fields are deltas, so the meter is primed once before the loop starts and again in the window's
constructor; they are `null` only if `GetProcessTimes` itself fails. The GPU memory fields are absent (not null)
when the adapter does not expose `IDXGIAdapter3`.

#### GPU engine utilisation

Source: the kernel graphics statistics (`D3DKMTQueryStatistics`), the same per-engine running time System Informer
shows. When the engine first samples an adapter it asks once how many engines ("nodes") the adapter has and what
type each one declares, and logs the map (`GPU engines on <gpu>: 0 3d, 1 video_decode, 2 copy, 3 video_processing,
4 video_decode, 5 other, ...`). After that each sample is two small kernel queries per engine (this process, all
processes) - no performance-counter discovery, nothing on the frame path.

`<class>` is one of `3d`, `copy`, `video_decode`, `video_encode`, `video_processing`, `video_codec`, `other`
(compute, overlay, crypto and scene assembly fold into `other`). What the percentages mean:

- **Percent of wall time that engine was busy over the interval** (running time delta ÷ elapsed time).
- **Per class, the busiest engine of that class** - Task Manager's convention. This Iris Xe has two `video_decode`
  engines; the value is the busier one, not their sum.
- **Engines run in parallel**, so several classes can each approach 100% at once; the classes do not add up to a
  GPU total, and there is no single "GPU %" here on purpose.
- `_percent` is this process's work; `_system_percent` is every process's. The busiest engine for this process and
  the busiest one overall can differ, so the difference of the two is only approximately "everyone else". A class
  near 100% system-wide while this process uses little of it is contention from another application.
- The kernel books a DMA packet's time when it completes, so one interval can include a little of the previous
  one; values are capped at 100.
- **Which engine does what is vendor-specific.** On Intel Quick Sync the H.264 encode is booked to a
  **`video_decode`** engine (the VDBox) - there is no `video_encode` engine on this Iris Xe - and the NV12
  conversion to `video_processing`. Measured 1080p60: `video_decode` 17-19%, `video_processing` 8-9%, `3d` 2-4%.

Verified 2026-09-18 against Windows' own `GPU Engine(pid_*)\Utilization Percentage` counters sampled over the same
seconds: within 0.5 percentage points per engine (3D 4.0 vs 3.9-4.0, video decode 16.8 vs 16.5-17.4, video
processing 8.5 vs 7.9-8.6). The running time is in 100 ns units (the SDK header's comment says microseconds; a
controlled 3D load reads 54% one way and an impossible 545% the other).

Availability: a class is **absent** when the adapter has no engine of it, or when engine time is unavailable
altogether (then `host.log` says `GPU engine utilization unavailable on <gpu>: <why>` once). It is **null** on the
first sample after the pipeline starts (a delta needs two readings) and whenever any engine of that class failed to
answer - a maximum over only some of them could hide the busy one.

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
| `webrtc.receiver_age_ms` | instant | Time since the receiver's latest telemetry arrived. Null before the first |
| `webrtc.timings` | — | `answer_ms`, `connected_ms`, `first_sent_ms`, `first_keyframe_ms`, `keyframe_response_ms`, all from the start of this peer connection |

### Receiver telemetry

The browser samples `getStats()` about once a second and sends the result over the data channel; the host accepts
at most one per 500 ms and keeps the latest under `webrtc.receiver`. `null` means *unavailable*, never *zero* — do
not treat a missing counter as a clean picture. Every new field is optional, so an older receiver page simply leaves
it out.

**The receiver's interval is not the host's.** A browser can sample slower than asked (a background or throttled
tab; one local test sampled every 2 s), so the same message then appears in two consecutive records. Divide counts
by `intervalMs`, and before summing interval counts across records keep only records where
`webrtc.receiver_age_ms` is below the record spacing (~1000 ms): a larger age means that record repeats the
previous sample.

Hoisted to the top level for convenience: `receiver_qp`, `receiver_corrupted`, `receiver_freezes`, `receiver_pli`,
`receiver_frames_received`, `receiver_frames_decoded`, `receiver_freeze_ms`. The full set is under
`webrtc.receiver`.

| Field | Window | Meaning | Healthy |
| --- | --- | --- | --- |
| `qp` | interval | Mean quantizer the decoder saw. **This is pixelation itself, measured** | 20–30; over ~36 is visibly blocky. Chrome did not publish `qpSum` for this H.264 stream in the 2026-09-18 local test, so it read null |
| `corrupted` | interval | Frames that arrived but never decoded | 0 |
| `freezes` | interval | Freeze events (the browser's definition: a frame much later than the recent average) | 0 |
| `freezeMs` | interval | How long the picture was frozen in total | 0 |
| `pauses`, `pauseMs` | interval | No frame for 5 s or more, and how long. **A still desktop produces pauses**; read with the host's source activity | — |
| `pli`, `nack`, `fir` | interval | Recovery requests the receiver sent | 0 |
| `keyFramesDecoded` | interval | Keyframes decoded | matches the host's keyframe rate |
| `intervalMs` | — | The span the interval counts cover | ~1000 |
| `intervalFramesReceived`, `intervalFramesDecoded` | interval | Frame flow at the receiver | ≈ `fps × intervalMs / 1000` |
| `intervalFramesRendered` | interval | Frames shown. Chrome did not publish `framesRendered` in the local test: null | — |
| `intervalPacketsReceived`, `intervalPacketsLost` | interval | The counts behind `loss`, so one lost packet out of ten is not read like 100 out of 1000 | lost 0 |
| `interFrameDelayMs`, `interFrameDelayStdMs` | interval | Mean and standard deviation of the gap between shown frames: smoothness as the viewer saw it | 16.8 ± 1.4 at 60 fps |
| `jitterBufferTargetMs` | interval | The jitter buffer's target per frame. Measured 11 ms while `jitterBufferMs` was 0.35 | — |
| `availableIncomingBitrate` | instant | The selected candidate pair's incoming bandwidth estimate, only when the browser provides one (Chrome did: ~12 Mbps on localhost) | above `bitrate` |
| `decoder`, `powerEfficientDecoder` | instant | The decoder implementation and whether the browser calls it power efficient (hardware). The spec lets a browser expose them only when the page may see hardware details (fullscreen, or capturing camera or microphone); null otherwise, as in the local test, which was not fullscreen | — |
| `fps`, `bitrate`, `loss`, `rttMs`, `jitterMs` | interval / instant | The usual link numbers | — |
| `jitterBufferMs` | interval | Time frames waited before rendering | near 0 — the playout-delay extension is set to 0/0 |
| `decodeMs`, `processingMs` | interval | Receiver-side decode and packet-to-frame delay | — |
| `dropped` | interval | Frames dropped at the receiver | 0 |
| `decoded` | **cumulative** | `framesDecoded` since the stream started (the one cumulative field; the interval count is `intervalFramesDecoded`) | — |

A counter that goes *down* between two samples belongs to a new stream, so that interval is null rather than zero or
negative. The one exception is `packetsLost`, which legitimately falls when duplicates arrive; that interval counts 0.

The receiver's own view of the ICE route is not sent: the host's `webrtc.route` already names the selected pair and
its protocol.

---

## `host.log` line catalogue

Grep-able landmarks, in roughly the order a session produces them.

**Startup**

```
Laptop Monitor 0.2.0 starting
Run 20260918-171419-17960 (process 17960) | diagnostic tag scrolling
Machine: 8 logical processors | 15 GB RAM (86% in use) | Windows 25H2 build 26200 | on battery
Settings: 60 fps | quality balanced | capture auto | scale 150% | ... | signaling <url>
Diagnostics: <path>\host.log and <path>\perf.jsonl (one performance sample per second)
Earlier run 20260918-160102-1608 ended without closing its log (crash, forced exit or power loss); ...
Session archive: <folder> (run 20260918-171419-17960, tag scrolling) | 12 runs kept in <sessions>, 85 MB in total
Startup: settings and credential ready N ms after launch
Startup: window ready N ms after launch
```

The machine and settings lines are the baseline every later number is relative to. **Always read them before
comparing two logs** — a 20%-battery machine in battery saver is not the same machine. `Run` marks where a run
begins in the rolling `host.log`, which holds several.

**Bringing the display and pipeline up**

```
Virtual display device created (...) in 110 ms (scheduled task 0 ms, helper pipe 110 ms, device 0 ms)
Pipeline: wgc -> GPU NV12 -> Intel® Quick Sync ... | 1920x1080@60 | CPU readback: no | mode stream
Pipeline built in N ms (device N, capture N, NV12 converter N, encoder N) at N bps | pipeline session 3 (encoder_rebuild), engine session 1
Streaming engine thread scheduling: MMCSS Capture
GPU engines on Intel(R) Iris(R) Xe Graphics: 0 3d, 1 video_decode, 2 copy, 3 video_processing, 4 video_decode, 5 other, ...
```

If the scheduling line says `above-normal priority` instead of `MMCSS Capture`, MMCSS was unavailable and a busy
desktop can starve the capture thread for a scheduler quantum — that changes how to read every latency number
below it. The GPU engine line appears once per adapter and is the key to every `gpu_engine_*` field.

**Periodic digests** (every 30 s while streaming; the full sample is in `perf.jsonl`)

```
Stream: capture 60.0 fps, encode 60.0 fps | encode 5.1 ms (p95 7.0) | present to encoded 12.3 ms | queue 1 | ...
Source: 58 new frames in the last second, gap p95 24 ms, max 33 ms | 116 polls with no new frame | newest frame 0.0 s ago | 0 repeat encodes | user input 2.3 s ago, pointer on this display
Host clock: acquire to convert 0.0, to encoder 0.9, to encoded 5.7 (p95 6.3), to sent 6.2 (p95 7.0) ms | encoder output pickup 0.1 ms (p95 0.2)
GPU engines, this process / all processes: 3d 2/6% | copy 0/0% | video decode 18/18% | video processing 9/9% | other 0/0%
Engine thread: 118 wake-ups/s (60 frame, 55 encoder, 3 transport, 0 timeout; 4 did nothing), busy 6.1% ...
Frames not encoded: 12 dropped (8 coalesced, 4 superseded, 0 ring busy, 0 refused), 0 paced out, ...
Receiver: 60.0 fps | 14.0 ms rtt | 0.01% loss | ... | decode 0.7 ms | frozen 0 ms | keyframe requests 1 | ...
```

**Every 2 minutes, whether or not the window is visible**

```
App process: 0 window paints in 120 s (0 while hidden), mean 0.00 ms, longest 0.00 ms | cpu 0.4% ... | AC
```

This is the record of what the app costs sitting in the tray — most of its life, and the part nobody measures.

**Events worth grepping for**

| Pattern | Meaning |
| --- | --- |
| `Run 2` | The start of a run in the rolling log |
| `Earlier run` | A previous run died without closing its archive |
| `Session archive unavailable` | The archive could not be created; the rolling logs still work |
| `GPU engine utilization unavailable` | Why no `gpu_engine_*` fields appear |
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
frozen. It closes after 3 consecutive clean seconds and writes one attributed line. Both lines end with what the
**source** was doing, so an episode that is really a still desktop can be recognised:

```
[warn ] Picture quality dropped: encoded rate fell to 0.1 Mbps from a usual 0.4 Mbps | encoder 8000 kbps ...
        | host encode 2 fps, queue 0 | source: 2 new source frames in the last second, newest 0.1 s ago,
        0 repeat encodes, user input 2418.8 s ago
[info ] Picture quality recovered after 14 s (started: ...) | worst quantizer 44, lowest encoded rate 1.31 Mbps |
        encoder was recreated 2 time(s) at a lower bitrate: too few bits for this resolution |
        source: 2.4 new frames/s over the 12 degraded seconds, none at all in 3 of them
```

A still desktop trips the watch by design of its triggers: the encoded rate collapses to the keep-alive repeats,
and the receiver counts the long gaps between frames as freezes (and, past 5 s, pauses). Observed 2026-09-18 on a
nearly still display: repeated "encoded rate fell to 0.1 Mbps" episodes with the source at 2-3 new frames per second
and nobody touching the input. The triggers were deliberately left as they were; the source context is what tells
such an episode from a real one.

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

Confirm it from a fresh session before acting: correlate `pipeline_session` stepping up with
`pipeline_build_reason == "encoder_rebuild"`, `receiver_qp` rising and `delta_bytes_mean` collapsing in the same
seconds. If that correlation holds, the fault is in the adaptation policy, not in capture, conversion or the
encoder's throughput.

## Attributing a problem

A starting point for the question "where is it", with the fields that decide it:

| Suspect | Look at | It is this when |
| --- | --- | --- |
| Source / capture | `capture_fps`, `source_frame_gap_ms_*`, `ms_since_last_source_frame`, `user_input_idle_ms`, `cursor_on_display`, `dropped_coalesced` | Few or no frames while input says the screen should be changing; long gaps that end |
| Conversion | `convert_submit_ms_mean`, `gpu_command_span_ms_mean`, `gpu_engine_video_processing_*` | Submit is slow, or the video-processing engine is saturated |
| Encoder | `encode_ms_*` minus `encoder_output_pickup_ms_*`, `keyframe_encode_ms_mean`, `dropped_superseded`, `dropped_ring_busy`, `queue_depth`, the engine class carrying encode (`video_decode` on Intel) | The encoder's own time is high or its engine is saturated |
| Host scheduling / GPU contention | `encoder_output_pickup_ms_*`, `host_acquire_to_convert_ms_*`, `loop_max_ms`, `loop_busy_percent`, the scheduling line, `gpu_engine_*_system_percent` versus `_percent`, `cpu_percent` | Frames and outputs wait for this thread, or another process owns the engines |
| Transport / network | `encoded_to_send_ms_*`, `send_ms_*`, `transport_buffer_*`, `transport_dropped`, `route`, receiver `loss`, `rttMs`, `jitterMs`, `intervalPacketsLost`, `nack` | Send or buffering grows, or the receiver reports loss and jitter |
| Receiver decode / render | receiver `decodeMs`, `processingMs`, `jitterBufferMs`, `interFrameDelayStdMs`, `freezeMs`, `intervalFramesDecoded` versus `intervalFramesReceived`, `dropped`, `decoder` | Frames arrive on time but decode late, are dropped, or are shown unevenly |
| Bitrate controller / rebuilds | `pipeline_session`, `pipeline_build_reason`, `encoder_rebuilds`, `bitrate`, `target_bitrate`, `bitrate_cuts`, `encoder_rebuild_ms_mean` | Rebuilds line up with the complaint |

## What is *not* measured

Do not infer these from anything above:

- **A single GPU utilisation figure.** Engines are reported per class; they run in parallel and do not add up.
  `gpu_command_span_ms_mean` includes driver submission delay and is not an engine duration either.
- **Time in the WGC frame pool before the engine took a frame, on the host clock.** The only stamp for it is the
  compositor's vsync target (`acquire_delay_ms_*`), which is a different quantity.
- **Time on the wire.** "Sent" is `transport->send()` returning; RTT and jitter come from the receiver.
- **Per-frame receiver timing.** The receiver reports interval aggregates; there is no end-to-end glass-to-glass
  number, because the two machines share no clock.
- **Per-thread CPU.** `cpu_percent` covers the whole process. The engine thread's share can only be bounded by
  `loop_busy_percent`, which is wall-clock occupancy, not CPU time.
- **Anything in the elevated helper or the driver.** `LaptopMonitorDisplay.exe` reports over its pipe and is not
  sampled; the IddCx driver is not instrumented at all.
- **The receiver's machine.** Only what the browser's `getStats()` publishes crosses the data channel; the decoder's
  name is usually withheld by the browser.
- **Network path.** RTT, loss and the ICE pair type are all that is known; there is no path MTU, pacing or
  congestion-window visibility.

## Where the code is

| Concern | File |
| --- | --- |
| Both log channels, rotation, archive mirroring, the record writer | `host/include/logging.hpp`, `host/src/logging.cpp` |
| Run id, diagnostic tag, pipeline/engine session numbers, `session.json`, reconciliation | `host/include/session_archive.hpp`, `host/src/session_archive.cpp` |
| Opening and closing the logs and the archive in the app | `host/app/diagnostics.cpp` (from `main.cpp`, `controller.cpp`, `main_window.cpp`) |
| Log locations (`%TEMP%\LaptopMonitor`, `%LOCALAPPDATA%\LaptopMonitor\logs\sessions`) | `logDirectory()`, `sessionsDirectory()` in `host/src/settings.cpp` |
| Process/GPU memory/GPU engine/power sampling | `host/include/resources.hpp`, `host/src/resources.cpp` |
| Per-second record, loop accounting, host-clock tracing, source activity, `QualityWatch` | `host/src/pipeline.cpp` |
| `LatencyTrack`, `SourceActivity`, `busyPercent` (portable, unit tested) | `host/include/core.hpp` |
| `FrameTrace` and where the encoder carries it | `host/include/platform.hpp`, `host/src/encoder.cpp` |
| Snapshot shared with the GUI | `host/include/metrics.hpp` |
| Transport counters, ICE route, pressure reporting, receiver age | `host/src/transport.cpp` |
| Bitrate policy | `BitrateController` in `host/include/core.hpp` |
| Receiver-side counters | `viewer/src/telemetry.ts`, `Telemetry` in `shared/protocol.ts` |
| Window paint cost | `reportUiCost()` in `host/app/ui/main_window.cpp` |

Adding a field: put it in the `stats` object in `pipeline.cpp` (it reaches `perf.jsonl`, the archive, the bench JSON
sink and the receiver's dashboard at once), add it to `MetricsSnapshot` only if the GUI, *Copy diagnostics* or
`QualityWatch` needs it, and add it to the 30-second digest only if someone reading `host.log` for a story would
miss it. Keep the readable log readable; the record file is where volume belongs. A receiver field goes into
`telemetry.ts` as an optional property of `Telemetry`, null when the browser does not publish it, with a test.
