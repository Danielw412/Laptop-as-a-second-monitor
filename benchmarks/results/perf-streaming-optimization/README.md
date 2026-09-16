# Streaming pipeline optimization: before / after

Measured 2026-09-15 on an Intel i5-1145G7 with Iris Xe graphics (driver 32.0.101.7077) and the Intel Quick Sync
H.264 encoder MFT, Windows 11 26200. Source: `laptop-monitor-bench --pattern`, a vsync-paced moving bar (exactly
one new frame per refresh of the captured 60 Hz display). "Before" is the tick-driven loop instrumented with the
same counters; "after" is the event-driven loop on this branch. Every row is the mean of the per-second values
after the first five seconds of a 25-30 s run, unless stated otherwise.

## Capture + encode, physical secondary display, WGC

| Metric | Before | After |
| --- | ---: | ---: |
| Capture / encode fps (source 59.3) | 57.7 | 59.3 |
| Frame wait, compositor stamp to capture, mean / p95 (ms) | -2.7 / +6.1 | -11.8 / -6.5 |
| Compositor stamp to encoded output, mean / p95 (ms) | +4.0 / +12.8 | -5.6 / -0.6 |
| Encode latency mean / p95 / p99 (ms) | 5.42 / 6.53 / 6.79 | 5.09 / 5.67 / 7.10 |
| Engine thread wake-ups per second | 587 | 218 |
| Process CPU (% of 8 logical CPUs) | 2.51 | 2.27 |
| Frames lost to keyframe encodes (30 s) | 0 (2 s GOP: 2-frame gaps every keyframe) | 1 |
| Submit interval max (ms) | 34.4 | 40.3 |

WGC stamps each frame with its vsync target time, so the "frame wait" is negative once the loop takes the frame the
instant the compositor delivers it; the difference between the columns (about 9 ms mean, 13 ms at p95) is the
latency the timer tick used to add.

## End-to-end loopback stream (host and receiver on the same machine, 105 s at 60 fps)

| Metric | Before | After |
| --- | ---: | ---: |
| Receiver fps | 58.0 | 58.8 |
| Receiver jitter buffer delay per frame (ms) | 8.55 | 0.34 |
| Receiver packet-to-decoded-frame (ms) | 9.34 | 1.16 |
| Receiver decode (ms) | 0.73 | 0.77 |
| Receiver freezes / dropped frames | 0 / 0 | 0 / 0 |
| Host frame wait mean (ms) | -5.2 | -12.4 |
| Host encode mean / p99 (ms) | 5.38 / 7.64 | 4.80 / 6.30 |
| Host wake-ups per second | 572 | 178 |
| Host loop longest iteration mean / max (ms) | 2.9 / 10.2 | 2.0 / 4.0 |
| Host send call mean / max (ms) | 0.67 / 8.42 | 0.54 / 2.42 |
| Host CPU (% of 8 logical CPUs) | 3.01 | 2.18 |
| Keyframes sent | 54 | 12 |
| Transport drops | 0 | 0 |

The receiver-side gain comes from the playout-delay RTP header extension (min = max = 0) and from RTP timestamps
that now carry the compositor's evenly spaced stamps instead of the host loop's scheduling jitter. On a real network
Chrome's adaptive jitter buffer would otherwise grow with jitter; the extension caps it at zero.

## LaptopMon virtual display (after only)

WGC while the desktop app was also capturing and encoding the same display: 54 fps captured of 58.5 presented,
frame wait -11.8 ms, 1 dropped frame in 20 s, encode 6.7 ms mean with two encoders sharing the GPU.

Desktop duplication (DXGI) is pathological on this GPU/driver with a flip-model fullscreen source, on both the
physical display and the virtual one (13-20 fps, 800-1100 merged frames, 40-100 ms stalls inside the acquire /
convert / release sequence), before and after; the blocking `AcquireNextFrame` did not change that. It remains the
fallback only.

## Live bitrate change experiment (`--test-bitrate-switch 6`)

`IsModifiable(AVEncCommonMeanBitRate)` is false on the Intel MFT. Forcing `SetValue` is "accepted" but frame sizes
stay at the original CBR budget (16.7 KB/frame at 8 Mbps, unchanged at 1.5 or 16 Mbps); renegotiating the output
type with a new `MF_MT_AVG_BITRATE` is also accepted, also ignored, and stalled encoding to 12-45 fps. The engine
therefore keeps recreating the encoder when the target moves by 25 % or 2 Mbps.

## Files

`before-*` / `after-*` CSVs are the per-second bench output; the `*-stream-loopback.csv` files add the receiver's
telemetry columns (`receiver_*`).
