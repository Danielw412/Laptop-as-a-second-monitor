# Laptop Monitor

Turn any laptop with a browser into a second monitor for a Windows 11 PC.

Laptop Monitor adds a virtual 1920×1080@60 display ("LaptopMon") to Windows, captures it on the GPU, encodes it
with the GPU's H.264 encoder and streams it over a direct WebRTC connection to a web page on the other laptop.
Nothing is read back to the CPU on the video path: capture texture → D3D11 video processor (NV12) → hardware
encoder MFT → RTP. A Cloudflare Worker only introduces the two machines; video never touches a server.

Daily use is: launch **Laptop Monitor**, read the six-character code it shows, type that code on the receiver.

## What is in the box

| Piece | Where | Purpose |
| --- | --- | --- |
| `LaptopMonitor.exe` | `build/host/` | The desktop app: tray icon, one compact window, all controls, settings and metrics. Runs unelevated. |
| `LaptopMonitorDisplay.exe` | `build/host/`, installed to `%ProgramFiles%\Laptop Monitor\` | The only elevated piece. Holds the software device that loads the virtual display driver. Started on demand through a scheduled task, controlled over a named pipe, exits when the app does. |
| `LaptopMonitorIdd` | `driver/` | The IddCx virtual display driver (derived from Microsoft's sample). See `driver/README.md`. |
| Signaling worker | `signaling/` | Cloudflare Worker + Durable Objects that pair a host with one viewer and relay SDP/ICE. No media. |
| Receiver | `viewer/`, published to GitHub Pages | The web page the other laptop opens. Enter the code, get the display. |
| `laptop-monitor-bench.exe` | `build/host/` | Command-line benchmark and diagnostics tool using the same pipeline. |

## One-time setup

You need Windows 11, a GPU with a D3D11-capable H.264 encoder (Intel Quick Sync, NVENC or AMD AMF), Visual Studio
Build Tools with the C++ workload, CMake, Node.js and Python (`scripts/bootstrap.ps1` installs the last three with
winget).

1. **Build the driver** (no admin): `./scripts/build-driver.ps1`. Details and requirements in `driver/README.md`.
2. **Build the host** (no admin): `./scripts/build-host.ps1`. This clones the pinned libdatachannel and Mbed TLS,
   builds them, builds the app, the helper and the bench tool into `build/host/`, and runs the C++ tests.
3. **Launch `build/host/LaptopMonitor.exe`** and click **Set up…**. Windows asks for administrator approval once.
   The elevated setup step:
   - stages the driver package if it is not installed yet (it runs `scripts/install-driver.ps1`, which creates a
     local code-signing certificate, trusts it on this machine only, signs the catalog and runs `pnputil`);
   - copies `LaptopMonitorDisplay.exe` to `%ProgramFiles%\Laptop Monitor\` so only administrators can replace what
     gets elevated later;
   - registers the scheduled task **Laptop Monitor Display**: run level *highest*, principal = your account,
     interactive logon, no triggers, on demand only, no time limit, hidden.

   After this, no launch ever shows a UAC prompt again. The app itself always runs unelevated; it asks the task
   scheduler to start the installed helper, which is what removes the need for an admin terminal.

   If you prefer to stage the driver yourself, run `./scripts/install-driver.ps1` in an elevated PowerShell before
   step 3; setup then only installs the helper and the task.

4. The first time LaptopMon appears, Windows may attach it in *Duplicate* mode. Laptop Monitor notices this and
   switches the desktop to *Extend* automatically. If you ever need to do it by hand: Settings → System → Display →
   LaptopMon → *Extend desktop to this display*.

The receiver needs nothing installed. Open the published page (the app's pairing card links to it) in Chrome,
Edge or Safari on the other laptop.

## Daily use

1. Start **Laptop Monitor** (or let it start at sign-in from Settings). It goes through *Starting virtual display →
   Finding LaptopMon → Starting encoder → Connecting signaling → Ready for receiver* in a few seconds.
2. Read the **pairing code** on the Overview page (or right-click the tray icon → *Copy pairing code*).
3. On the other laptop, open the receiver page and type the code. The sixth character connects; the page moves
   to the display and fills the screen. The app shows **Connected** with live FPS, bitrate, RTT, dropped frames,
   resolution and encoder.

Only the code has to be typed. If the receiver runs against its own signaling worker, set it once under *Advanced*:
the address is kept in a cookie (with localStorage as a fallback) scoped to that page, so it comes back on every
later visit. A `#server=` link overrides it and replaces what was stored. **Copy link** in the app puts a receiver
link with the current code on the clipboard (`…/#code=K7M4Q2`); opened on the other laptop, it connects on its
own. The code only ever travels in the URL fragment, which browsers never send to a server.

### Receiver

The page has two states: the code page and the display. Once the code is accepted it switches to the display and
asks the browser for fullscreen (turn that off under *Advanced* if you prefer a window). While a picture is
showing, the cursor and the small toolbar disappear after 2.5 seconds without mouse movement and come back on any
movement. **F** toggles fullscreen, **S** opens the connection and performance panel, **Esc** leaves fullscreen,
and a click on the picture fills the screen when it is not already full (the browser only allows fullscreen from
a click or key press, so a reloaded page waits for one). **M** marks a damaged picture: the host saves the last
seconds of its stream and the next source frame next to its logs, and the page saves the frame it decoded as a PNG
(see *Logging*). The page holds a screen wake lock while the display is connected, so the receiving laptop does not
dim or sleep under you. A dropped signaling connection does not interrupt the picture (see *Connection*); a reload
reconnects by itself with the session token; *Disconnect* in the toolbar returns to the code page, and so does being
disconnected from the host.

LaptopMon is found by its EDID identity (manufacturer `LMV`, product `0001`, name `LaptopMon`), never by its
`\\.\DISPLAYn` number, which Windows reassigns. Laptop Monitor never falls back to a physical display: if
LaptopMon is missing it waits, and if it is set as the primary display it refuses and tells you.

### Scaling

The stream is 1920×1080 and no browser zoom is involved. (Only when the network cannot carry it does adaptation
first halve the frame rate and then, below 2.5 Mbps, send 1280×720; see *Connection*.) What changes is how large
Windows *draws* on that display, which is what makes it readable on a 13-inch receiver.

- The driver's EDID declares LaptopMon as a **294 × 166 mm (13.3") panel**, so 1920×1080 works out to ~166 DPI and
  Windows' own *recommended* scaling for it lands at 150% — the same as a 13-inch laptop screen.
- Settings → *Windows scaling (this display only)* then pins it: **Recommended**, 100%, 125%, **150%** (the
  default) or 175%. The app applies it to LaptopMon's display path alone, through the same per-monitor mechanism
  Settings → System → Display uses. Your real monitors and the system-wide scaling are never touched.
- It is applied once per display session, so if you change it yourself in Windows that choice stays until the next
  start. If Windows refuses (it will not scale a display that is cloned or primary), the app says so and names the
  exact Settings path to use instead.

125% is the sharper choice if 150% feels too large; both keep the full 1080p picture.

### Controls

| Action | What it does |
| --- | --- |
| **Copy code** / **Copy link** | Puts the pairing code, or the receiver link that carries it and connects on open, on the clipboard. |
| **Disconnect receiver** | Drops the current viewer (server-side too), issues a fresh code, keeps streaming ready. |
| **Stop streaming** / **Start streaming** | Stops the encoder and signaling; the virtual display stays as a plain desktop extension. |
| **Stop monitor** / **Start monitor** | Stops streaming and removes the virtual display. The app stays in the tray. |
| **Restart** | Full restart: display, encoder and signaling. |
| **Exit Laptop Monitor** | Stops streaming, removes the virtual display, stops the helper, removes the tray icon and exits. |

Keyboard: Tab / Shift+Tab move focus, Enter or Space activates, ←/→ switch pages, Ctrl+C copies the code,
Ctrl+Shift+C copies the receiver link, Esc hides the window to the tray.

### Tray

Closing the window hides Laptop Monitor to the tray (Settings → *Keep running in the tray when the window
closes*; turn it off to make the close button exit). Left-click the tray icon to show or hide the window;
right-click for *Open*, *Copy pairing code*, *Copy receiver link*, *Disconnect receiver*, *Stop/Start streaming*,
*Stop/Start monitor*, *Restart* and *Exit Laptop Monitor*. The icon's dot shows the state: green ready, blue
connected, amber busy or reconnecting, red error, grey stopped. A balloon appears when the receiver connects or
leaves.

### Pages

- **Overview**: pairing code with countdown and the two copy buttons, a link to the receiver page (opened from
  here it carries the current code), four status rows (virtual display, streaming, receiver, signaling), six
  headline metrics, the controls above.
- **Details**: capture/encode/receiver FPS, encoder and receiver bitrate, RTT, jitter, packet loss, dropped frames,
  capture, encode and frame-to-encoded latency (measured from the compositor's own frame stamp), how long frames
  waited before capture, the receiver's jitter-buffer and decode delay, encoder queue and keyframes, connection
  duration, sent frames/bytes, signaling and WebRTC state, GPU, encoder, capture backend, video path (GPU, no CPU
  readback), host CPU, setup status. *Copy diagnostics* puts all of it plus the drop breakdown, engine-thread cost,
  process RAM/GPU memory, the run id and the recent log on the clipboard; *Open log folder* offers the current logs
  (`%TEMP%\LaptopMonitor`), this run's archive, or all archived runs.
- **Settings**: start at sign-in, start the virtual display automatically, keep running in the tray on close,
  diagnostics log, Windows scaling for LaptopMon only (see *Scaling*), capture backend (Auto prefers Windows Graphics Capture and falls back to DXGI duplication),
  frame rate (60/30), quality preset (Efficient 5→10 Mbps, Balanced 8→16 Mbps, Quality 12→20 Mbps), signaling URL
  with reset, and the setup/uninstall buttons. Pipeline settings apply by restarting the stream only.

Metrics come from the streaming engine's once-per-second snapshot and the receiver's telemetry over the WebRTC
data channel; the window polls them on a timer and never touches the frame path.

## Pairing

The user-visible flow is one six-character code from the alphabet `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no I, O, 0
or 1). Underneath:

- Each installation has a random 256-bit **host credential**, created on first run and stored DPAPI-protected in
  `%LOCALAPPDATA%\LaptopMonitor\host.credential`. It is never shown, logged or typed. Its room on the signaling
  server is the first 32 hex characters of SHA-256 over the credential, which the worker verifies statelessly.
- The app generates codes with the system CSPRNG. A code is shown for **2 minutes**; the previous one stays valid
  for **15 seconds** after rotation so a code read just before the change still works. Rotation never touches an
  established connection: codes are only checked when a new viewer joins.
- The host publishes **SHA-256 hashes** of its valid codes (with remaining lifetimes) to its room; the worker keeps a
  per-code directory entry (a Durable Object named by the hash) that expires with the code.
- The receiver posts the code in a request body (never a URL) to `/pair`, receives the room and a **single-use
  ticket**, and authenticates its WebSocket with the ticket. It then gets a **resume token** (12 h) so a reload or a
  signaling blip reconnects without a code. *Disconnect receiver* revokes the token.
- **One viewer maximum**: a second valid code presentation is refused with `viewer-occupied`.
- Brute force: 12 pairing attempts per minute per client IP at the worker (Cloudflare rate-limit binding) plus a
  per-room pause after 10 bad tickets or tokens in a minute. Wrong codes never reach a room.
- A restarted host replaces its stale socket instead of being locked out; the credential proves it is the same
  installation.

### Connection

The signaling WebSocket only introduces the two machines. Once the direct WebRTC connection is up, neither side
tears it down because a WebSocket closed: the host and the page reconnect their sockets in the background, and a
worker that supports it (`features: ["resume", "heartbeat"]` in its "authenticated" message) confirms that both
still hold the same session and lets them keep it — no renegotiation, no gap in the picture. An older worker still
works; there a WebSocket drop costs a renegotiation (a few seconds) as before. The page sends a heartbeat every
20 s so an idle socket is not timed out on the way, and the host detects a dead socket after three unanswered pings
(about 40 s). If the direct connection itself drops, the host waits up to 20 s for ICE to recover (and sends a
keyframe when it does) before the page negotiates a new one. Both ends log the exact reason for every close: the
host in `host.log`, the page in events it sends to the host's log.

The encoder's bitrate follows the network, not a fixed number: it is cut only on real congestion (packet loss or a
round-trip time well above the link's own baseline, both from RTCP), recovers after 10 clean seconds, and climbs
above the preset's starting bitrate towards its maximum only while the desktop actually needs the bits. When the
target cannot carry 1080p60 well, the stream drops to 30 fps first (half the frames, twice the bits per frame) and to
720p only below 2.5 Mbps. On Intel's encoder every change rebuilds it (a 250-400 ms pause), so changes are few.

Signaling stays discovery only: video is direct WebRTC with STUN (no TURN). The default endpoint is
`https://browser-monitor-signaling.danielruoqiao.workers.dev`; change it in Settings or at build time with
`-DLM_DEFAULT_SIGNALING_URL=...`. The worker in `signaling/` speaks protocol version 2; deploy it with
`npm run deploy -w signaling` (or the *Deploy signaling* workflow) before using this version of the app against it;
redeploy it to get session resume and heartbeats.

## Complete uninstall / cleanup

Settings → **Uninstall…** (one UAC prompt), or run `LaptopMonitor.exe --uninstall` from an elevated prompt. It
removes, in this order:

1. the running helper and the scheduled task *Laptop Monitor Display*;
2. `%ProgramFiles%\Laptop Monitor\`;
3. the *start at sign-in* entry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LaptopMonitor`);
4. `%LOCALAPPDATA%\LaptopMonitor\` (settings, host credential, archived diagnostics runs) and
   `%TEMP%\LaptopMonitor\` (current logs);
5. the driver package, the `SWD\LaptopMonitorIdd` device node and the local signing certificate, via
   `scripts/install-driver.ps1 -Uninstall` (when the scripts folder is found next to the build; otherwise the
   driver package is removed with `pnputil` and the certificate is left for `install-driver.ps1 -Uninstall`).

Windows keeps the non-present monitor node `DISPLAY\LMV0001`, like any unplugged monitor. Nothing else is written
outside the build tree.

### Coming from the old "Browser Monitor" build

This product used to be called Browser Monitor, and the rename changed every installed name. Setup and uninstall
both clear what the old build left behind first: the *Browser Monitor Display* scheduled task,
`%ProgramFiles%\Browser Monitor\` and the old `Run` entry — an elevated task pointing at an executable nothing
drives any more is not something to leave installed. Uninstall additionally removes `%LOCALAPPDATA%\BrowserMonitor\`.

Two things are deliberately not carried over:

- **Settings and the host credential** now live under `%LOCALAPPDATA%\LaptopMonitor\`, so the app starts on
  defaults and generates a fresh credential. Receivers pair once more with a new code.
- **The old driver package** has different hardware IDs (`BrowserMonitorIdd` / `BMV0001`), so the new one installs
  beside it rather than replacing it. Run the old build's *Uninstall...* before switching, or remove it afterwards
  with `pnputil /remove-device` on the `SWD\BROWSERMONITORIDD` node and `pnputil /delete-driver <oemNN.inf>
  /uninstall` for `browsermonitoridd.inf`.

## Development

`AGENTS.md` is the map: what each directory is for, how the parts talk to each other, the rules that are not
visible in the code, and where to change what.

```
host/include, host/src   pipeline (capture, converter, encoder, wgc, transport), logic (pairing, display_identity,
                          app_state, settings, logging, session_archive), display_query, resources, pipeline
                          (StreamingEngine)
host/app                  Win32 app: main, controller, diagnostics (log and archive lifecycle), display_control
                          (helper client), setup, ui/ (Direct2D window, renderer, tray)
host/helper               LaptopMonitorDisplay.exe
host/bench                laptop-monitor-bench.exe
host/tests                core-tests (adaptation, stream shape, keyframe policy, RTP sample times, H.264 and RTCP
                          parsing, probe comparison, samples, latency and source-activity tracking, GPU busy share,
                          H.264 framing), logic-tests (pairing, detection, lifecycle, settings, SHA-256, logging, the
                          per-run archive)
shared/protocol.ts        signaling message schema shared by worker and receiver
signaling/, viewer/       Cloudflare Worker and receiver page (viewer/src: main = views, stage = the display,
                          session = signaling + WebRTC, dashboard + telemetry = metrics, preferences = storage)
docs/                     logging-reference.md: what every logged field means, for performance work
tests/                    vitest (protocol, telemetry, preferences, probe) and the signaling integration test
scripts/analyze-recording.py  decodes a recorded stream with ffmpeg and compares it with the source frames
```

- C++: `./scripts/dev-build.ps1 -Configure` then `ctest --test-dir build/host`. Portable tests only:
  `cmake -S . -B build/logic -DLM_BUILD_HOST=OFF && cmake --build build/logic && ctest --test-dir build/logic`.
- Web: `npm ci`, `npm run typecheck`, `npm test`, then `npm run dev:signaling` in one terminal and
  `npm run test:integration` in another (exercises credential auth, code pairing, tickets, one-viewer limit,
  rotation overlap, heartbeat, session resume, resume token, kick, host replacement, throttling against the local
  worker).
- Receiver against a local worker: `npm run dev:viewer`, open `http://127.0.0.1:5173`, expand *Advanced* and set the
  signaling URL to `http://127.0.0.1:8787` (it is remembered from then on); point the app's Settings → Signaling URL
  at the same address.
- Bench: `laptop-monitor-bench --list`, then e.g.
  `laptop-monitor-bench --laptopmon --capture wgc --mode capture-encode --pattern --seconds 20 --csv out.csv`.
  `--mode stream` pairs like the app (prints the code on the console). `benchmarks/run.ps1` runs the matrix.
  `--pattern` draws a vsync-paced moving bar on the selected display (exactly one new frame per refresh), so
  capture FPS can be compared with the display's refresh rate. `--test-bitrate-switch N` alternates the encoder
  bitrate between the preset's minimum and maximum every N seconds to check whether a live change takes effect
  (watch `frame_bytes_mean`). Per-second JSON/CSV columns include `acquire_delay_ms` (compositor stamp to
  capture), `source_to_encoded_ms`, `wakeups_per_s`, `loop_max_ms`, `submit_interval_ms`, `send_ms`,
  `frame_bytes_max`, `keyframes` and the receiver's `jitterBufferMs`, `decodeMs` and `processingMs`; the JSON
  lines also carry the host-clock latencies, source activity and GPU engine time described in the logging
  reference. `--diagnostic-tag scrolling` labels every JSON line of a controlled run.
- Controlled quality tests without touching a real display: `--synthetic --content desktop` generates a busy
  desktop on the GPU (scrolling terminal text, a window switch every 3 s, a 30 fps video-like region; `scroll` and
  `bar` are the gentler ones, and `--pattern --content desktop` draws the same on a chosen display for capture
  tests). `--bitrate N` fixes the bitrate; `--rate-control`, `--buffer-ms`, `--max-qp`, `--gop` try encoder
  settings (the log's `Encoder configuration:` line says which ones the encoder actually took). `--record out.h264
  --record-source-every 30` writes the exact stream plus sampled NV12 source frames, and
  `python scripts/analyze-recording.py out.h264 --png-dir frames` reports per-frame QP and PSNR per horizontal band
  between what was encoded and what a receiver decodes. `--log-dir dir` writes `host.log` and `perf.jsonl` there,
  which also turns on quality probes and receiver marks in `--mode stream`.

### How the frame loop is paced

The engine thread never polls for frames on a timer. Windows Graphics Capture signals an event the moment the
compositor hands over a frame, the hardware encoder signals one for every event it raises (input wanted, output
ready, input surface released), and the transport signals one for signaling or telemetry messages; the loop sleeps
on those three handles. Desktop duplication has no event, so that backend blocks inside `AcquireNextFrame`. Each
frame is converted into a four-surface NV12 ring and handed to the encoder with the compositor's timestamp, up to
three frames may be inside the encoder at once (a keyframe takes 20-30 ms on an integrated GPU and would otherwise
cost the next two frames), and the thread runs in the MMCSS "Capture" scheduling class. RTP packets carry the
playout-delay header extension set to zero so the browser renders each frame as soon as it is decoded. The Intel
encoder accepts but ignores live bitrate changes, so bitrate adaptation recreates the encoder. Keyframes are forced
on demand (viewer join, PLI or FIR, ICE recovery, a frame that never reached the network) with requests folded
together and at most one per 300 ms, plus a refresh every 20 s while streaming. After a frame that never reached the
network, delta frames are held until the next keyframe, because the receiver would otherwise decode them against a
picture it never got; RTP timestamps of consecutive frames are always at least one tick apart.

### Logging

The current logs are in `%TEMP%\LaptopMonitor\` (*Open log folder* in Details), on two channels, both governed by
the *diagnostics log* setting and neither ever containing a pairing code or credential:

- `host.log` (2 MB, rotates to `host.1.log`) - readable lines: startup with the machine profile and the settings in
  force, lifecycle and state changes, how long the virtual display and the pipeline took to build and where that
  time went, encoder rebuilds with the receiver numbers that caused them, bitrate cuts, keyframe storms, a send
  queue that will not drain, a performance digest every 30 s, what the window itself costs every 2 min, and a
  one-line summary of each pipeline session as it ends. `setup.log` holds the elevated setup and uninstall runs.
- `perf.jsonl` (8 MB, rotates to `perf.1.jsonl`) - one JSON object per second while streaming, with the full
  sample: capture/encode rates and latencies with percentiles, the four drop causes counted apart (coalesced,
  superseded, ring busy, refused) plus paced frames, engine-thread wake-ups by reason and the share of wall time
  the thread was awake, keyframe versus delta frame size and encode cost, process CPU split into kernel and user
  time, working set, GPU memory in use, GPU engine busy time per engine class (this process and all processes),
  handles, battery and battery-saver state, encoder rebuild count and cost, the display re-enumeration stall,
  each frame's QP as the encoder reported it, the network as RTCP describes it (loss, RTT against the link's
  baseline, NACK, PLI, FIR) and what adaptation made of it, keyframe requests by reason, the selected ICE route,
  where each frame's time goes on the host's own clock (capture to conversion, encoder, encoded output and send),
  whether the source was still or stalled, quality probe results, and the receiver's own telemetry. This is the file
  to read when the question is "where is the CPU, GPU or memory going".

Those rolling files are small and Temp gets cleaned, so every run with diagnostics on is also archived for good in
`%LOCALAPPDATA%\LaptopMonitor\logs\sessions\<run_id>\`: the same `host.log` and `perf.jsonl` lines plus a
`session.json` with the version, machine, GPU driver, settings, optional diagnostic tag and how the run ended
(`graceful`, `abnormal` for a crash or forced exit, and so on). Nothing prunes the archive; uninstall removes it.
Start the app with `--diagnostic-tag scrolling` (or any short label) to mark a controlled test in both.

The per-second record is the same object the benchmark writes to its CSV and JSON sink, so a field measured in
`benchmarks/` means the same thing in a user's log. `perf.jsonl` is written while the streaming engine runs; it
does not need a connected receiver, so a stream waiting for one still records what idling costs.

**Every field, what a healthy value looks like and what a bad one points at: [`docs/logging-reference.md`](docs/logging-reference.md).**
Read that before instrumenting anything new.

#### Picture quality episodes

"It went extremely pixelated and glitchy, then fixed itself" has three common causes that look alike: the encoder
running out of bits (QP 44 and above: flat blocks and stale rectangles wherever the picture changes, on any network),
packet loss (torn and smeared blocks until a keyframe repairs them), and a receiver that falls behind. The host
watches its own per-frame QP, RTCP loss and keyframe requests, the receiver's freezes and the quality probes, and
writes one line when quality drops and one when it recovers, naming the cause:

```
[warn ] Picture quality dropped: encoder quantizer 50 at 1898 kbps (flat blocks where the picture changes) | ...
[info ] Picture quality recovered after 14 s (started: ...) | too few bits for what changed on screen (encoder QP
        up to 50) | worst QP 50, lowest encoded rate 1.31 Mbps, 0 encoder rebuilds, 12 degraded seconds
```

**Quality probes** check the picture end to end: every 10 s the host reads back the exact surface it encoded and asks
the receiver for the same frame after decoding (by RTP timestamp); a coarse luma grid of both is compared, and the
result says `match`, `quantized` (detail lost: too few bits), `corrupted` (a region shows other content) or
`unrelated`. The captured frame is compared with the encoder's input the same way, so a fault in capture or
conversion shows too. Watching only; nothing here changes what the pipeline does. The measurements behind all this,
and how the 2026-09-22 pixelation and disconnects were traced, are in the logging reference.

## Troubleshooting

- **"Setup required" although the display exists**: an externally started `LaptopMonitorIddApp.exe` also works;
  the app streams it but cannot stop it (*Stop monitor* says so). Run *Set up…* once to let the app manage it.
- **"The display helper did not start"**: the scheduled task is missing or was blocked. Settings → *Repair setup…*.
  Group Policy that disables Task Scheduler for standard users prevents this design from working.
- **Everything is still too small (or too large)**: Settings → *Windows scaling (this display only)*. If the app
  says Windows would not apply it, the display is cloned or primary — fix that first, or set it by hand under
  Settings → System → Display → *LaptopMon* → *Scale*. Changing it there is fine; the app only re-applies its own
  choice the next time the display starts.
- **"LaptopMon is set as the primary display"**: make a physical display primary in Windows display settings.
  Laptop Monitor will not stream a primary display.
- **Receiver says the code is not recognized**: codes expire after about two minutes; use the one currently shown.
  "Laptop Monitor is not running on the host yet" means the worker knows the code but the host is offline.
- **Direct WebRTC connection failed**: the network blocks peer-to-peer traffic. No TURN relay is configured.
- **Signaling rejected this host**: the deployed worker speaks an older protocol; redeploy `signaling/`.
- Driver problems (code 52, code 10/31, `0x80070005`): see `driver/README.md`.

## Known limitations

- The window's controls are custom-drawn (Direct2D). Keyboard navigation and focus rings are implemented; UI
  Automation providers for screen readers are not.
- The scheduled task elevates whatever is at `%ProgramFiles%\Laptop Monitor\LaptopMonitorDisplay.exe`; that
  folder is administrator-writable only, which is the point of copying the helper there.
- One 1920×1080@60 virtual monitor, one viewer, H.264 baseline over STUN only.
