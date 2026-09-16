# Browser Monitor

Turn any laptop with a browser into a second monitor for a Windows 11 PC.

Browser Monitor adds a virtual 1920×1080@60 display ("BrowserMon") to Windows, captures it on the GPU, encodes it
with the GPU's H.264 encoder and streams it over a direct WebRTC connection to a web page on the other laptop.
Nothing is read back to the CPU on the video path: capture texture → D3D11 video processor (NV12) → hardware
encoder MFT → RTP. A Cloudflare Worker only introduces the two machines; video never touches a server.

Daily use is: launch **Browser Monitor**, read the six-character code it shows, type that code on the receiver.

## What is in the box

| Piece | Where | Purpose |
| --- | --- | --- |
| `BrowserMonitor.exe` | `build/host/` | The desktop app: tray icon, one compact window, all controls, settings and metrics. Runs unelevated. |
| `BrowserMonitorDisplay.exe` | `build/host/`, installed to `%ProgramFiles%\Browser Monitor\` | The only elevated piece. Holds the software device that loads the virtual display driver. Started on demand through a scheduled task, controlled over a named pipe, exits when the app does. |
| `BrowserMonitorIdd` | `driver/` | The IddCx virtual display driver (derived from Microsoft's sample). See `driver/README.md`. |
| Signaling worker | `signaling/` | Cloudflare Worker + Durable Objects that pair a host with one viewer and relay SDP/ICE. No media. |
| Receiver | `viewer/`, published to GitHub Pages | The web page the other laptop opens. Enter the code, get the display. |
| `browser-monitor-bench.exe` | `build/host/` | Command-line benchmark and diagnostics tool using the same pipeline. |

## One-time setup

You need Windows 11, a GPU with a D3D11-capable H.264 encoder (Intel Quick Sync, NVENC or AMD AMF), Visual Studio
Build Tools with the C++ workload, CMake, Node.js and Python (`scripts/bootstrap.ps1` installs the last three with
winget).

1. **Build the driver** (no admin): `./scripts/build-driver.ps1`. Details and requirements in `driver/README.md`.
2. **Build the host** (no admin): `./scripts/build-host.ps1`. This clones the pinned libdatachannel and Mbed TLS,
   builds them, builds the app, the helper and the bench tool into `build/host/`, and runs the C++ tests.
3. **Launch `build/host/BrowserMonitor.exe`** and click **Set up…**. Windows asks for administrator approval once.
   The elevated setup step:
   - stages the driver package if it is not installed yet (it runs `scripts/install-driver.ps1`, which creates a
     local code-signing certificate, trusts it on this machine only, signs the catalog and runs `pnputil`);
   - copies `BrowserMonitorDisplay.exe` to `%ProgramFiles%\Browser Monitor\` so only administrators can replace what
     gets elevated later;
   - registers the scheduled task **Browser Monitor Display**: run level *highest*, principal = your account,
     interactive logon, no triggers, on demand only, no time limit, hidden.

   After this, no launch ever shows a UAC prompt again. The app itself always runs unelevated; it asks the task
   scheduler to start the installed helper, which is what removes the need for an admin terminal.

   If you prefer to stage the driver yourself, run `./scripts/install-driver.ps1` in an elevated PowerShell before
   step 3; setup then only installs the helper and the task.

4. The first time BrowserMon appears, Windows may attach it in *Duplicate* mode. Browser Monitor notices this and
   switches the desktop to *Extend* automatically. If you ever need to do it by hand: Settings → System → Display →
   BrowserMon → *Extend desktop to this display*.

The receiver needs nothing installed. Open the published page (the app's pairing card links to it) in Chrome,
Edge or Safari on the other laptop.

## Daily use

1. Start **Browser Monitor** (or let it start at sign-in from Settings). It goes through *Starting virtual display →
   Finding BrowserMon → Starting encoder → Connecting signaling → Ready for receiver* in a few seconds.
2. Read the **pairing code** on the Overview page (or right-click the tray icon → *Copy pairing code*).
3. On the other laptop, open the receiver page and enter the code. The stream starts; the app shows **Connected**
   with live FPS, bitrate, RTT, dropped frames, resolution and encoder.

BrowserMon is found by its EDID identity (manufacturer `BMV`, product `0001`, name `BrowserMon`), never by its
`\\.\DISPLAYn` number, which Windows reassigns. Browser Monitor never falls back to a physical display: if
BrowserMon is missing it waits, and if it is set as the primary display it refuses and tells you.

### Controls

| Action | What it does |
| --- | --- |
| **Disconnect receiver** | Drops the current viewer (server-side too), issues a fresh code, keeps streaming ready. |
| **Stop streaming** / **Start streaming** | Stops the encoder and signaling; the virtual display stays as a plain desktop extension. |
| **Stop monitor** / **Start monitor** | Stops streaming and removes the virtual display. The app stays in the tray. |
| **Restart** | Full restart: display, encoder and signaling. |
| **Exit Browser Monitor** | Stops streaming, removes the virtual display, stops the helper, removes the tray icon and exits. |

Keyboard: Tab / Shift+Tab move focus, Enter or Space activates, ←/→ switch pages, Ctrl+C copies the code, Esc
hides the window to the tray.

### Tray

Closing the window hides Browser Monitor to the tray (Settings → *Keep running in the tray when the window
closes*; turn it off to make the close button exit). Left-click the tray icon to show or hide the window;
right-click for *Open*, *Copy pairing code*, *Disconnect receiver*, *Stop/Start streaming*, *Stop/Start monitor*,
*Restart* and *Exit Browser Monitor*. The icon's dot shows the state: green ready, blue connected, amber busy or
reconnecting, red error, grey stopped. A balloon appears when the receiver connects or leaves.

### Pages

- **Overview**: pairing code with countdown, four status rows (virtual display, streaming, receiver, signaling),
  six headline metrics, the controls above.
- **Details**: capture/encode/receiver FPS, encoder and receiver bitrate, RTT, jitter, packet loss, dropped frames,
  capture, encode and frame-to-encoded latency (measured from the compositor's own frame stamp), how long frames
  waited before capture, the receiver's jitter-buffer and decode delay, encoder queue and keyframes, connection
  duration, sent frames/bytes, signaling and WebRTC state, GPU, encoder, capture backend, video path (GPU, no CPU
  readback), host CPU, setup status. *Copy diagnostics* puts all of it plus the recent log on the clipboard; *Open log folder* opens
  `%LOCALAPPDATA%\BrowserMonitor\logs`.
- **Settings**: start at sign-in, start the virtual display automatically, keep running in the tray on close,
  diagnostics log, capture backend (Auto prefers Windows Graphics Capture and falls back to DXGI duplication),
  frame rate (60/30), quality preset (Efficient 5→10 Mbps, Balanced 8→16 Mbps, Quality 12→20 Mbps), signaling URL
  with reset, and the setup/uninstall buttons. Pipeline settings apply by restarting the stream only.

Metrics come from the streaming engine's once-per-second snapshot and the receiver's telemetry over the WebRTC
data channel; the window polls them on a timer and never touches the frame path.

## Pairing

The user-visible flow is one six-character code from the alphabet `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no I, O, 0
or 1). Underneath:

- Each installation has a random 256-bit **host credential**, created on first run and stored DPAPI-protected in
  `%LOCALAPPDATA%\BrowserMonitor\host.credential`. It is never shown, logged or typed. Its room on the signaling
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

Signaling stays discovery only: video is direct WebRTC with STUN (no TURN). The default endpoint is
`https://browser-monitor-signaling.danielruoqiao.workers.dev`; change it in Settings or at build time with
`-DBM_DEFAULT_SIGNALING_URL=...`. The worker in `signaling/` speaks protocol version 2; deploy it with
`npm run deploy -w signaling` (or the *Deploy signaling* workflow) before using this version of the app against it.

## Complete uninstall / cleanup

Settings → **Uninstall…** (one UAC prompt), or run `BrowserMonitor.exe --uninstall` from an elevated prompt. It
removes, in this order:

1. the running helper and the scheduled task *Browser Monitor Display*;
2. `%ProgramFiles%\Browser Monitor\`;
3. the *start at sign-in* entry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\BrowserMonitor`);
4. `%LOCALAPPDATA%\BrowserMonitor\` (settings, host credential, logs);
5. the driver package, the `SWD\BrowserMonitorIdd` device node and the local signing certificate, via
   `scripts/install-driver.ps1 -Uninstall` (when the scripts folder is found next to the build; otherwise the
   driver package is removed with `pnputil` and the certificate is left for `install-driver.ps1 -Uninstall`).

Windows keeps the non-present monitor node `DISPLAY\BMV0001`, like any unplugged monitor. Nothing else is written
outside the build tree.

## Development

```
host/include, host/src   pipeline (capture, converter, encoder, wgc, transport), logic (pairing, display_identity,
                          app_state, settings, logging), display_query, pipeline (StreamingEngine)
host/app                  Win32 app: main, controller, display_control (helper client), setup, ui/ (Direct2D window,
                          renderer, tray)
host/helper               BrowserMonitorDisplay.exe
host/bench                browser-monitor-bench.exe
host/tests                core-tests (bitrate, samples, H.264 framing), logic-tests (pairing, detection, lifecycle,
                          settings, SHA-256)
shared/protocol.ts        signaling message schema shared by worker and receiver
signaling/, viewer/       Cloudflare Worker and receiver page
tests/                    vitest (protocol, telemetry) and the signaling integration test
```

- C++: `./scripts/dev-build.ps1 -Configure` then `ctest --test-dir build/host`. Portable tests only:
  `cmake -S . -B build/logic -DBM_BUILD_HOST=OFF && cmake --build build/logic && ctest --test-dir build/logic`.
- Web: `npm ci`, `npm run typecheck`, `npm test`, then `npm run dev:signaling` in one terminal and
  `npm run test:integration` in another (exercises credential auth, code pairing, tickets, one-viewer limit,
  rotation overlap, resume, kick, host replacement, throttling against the local worker).
- Receiver against a local worker: `npm run dev:viewer`, open `http://127.0.0.1:5173`, expand *Advanced* and set the
  signaling URL to `http://127.0.0.1:8787`; point the app's Settings → Signaling URL at the same address.
- Bench: `browser-monitor-bench --list`, then e.g.
  `browser-monitor-bench --browsermon --capture wgc --mode capture-encode --pattern --seconds 20 --csv out.csv`.
  `--mode stream` pairs like the app (prints the code on the console). `benchmarks/run.ps1` runs the matrix.
  `--pattern` draws a vsync-paced moving bar on the selected display (exactly one new frame per refresh), so
  capture FPS can be compared with the display's refresh rate. `--test-bitrate-switch N` alternates the encoder
  bitrate between the preset's minimum and maximum every N seconds to check whether a live change takes effect
  (watch `frame_bytes_mean`). Per-second JSON/CSV columns include `acquire_delay_ms` (compositor stamp to
  capture), `source_to_encoded_ms`, `wakeups_per_s`, `loop_max_ms`, `submit_interval_ms`, `send_ms`,
  `frame_bytes_max`, `keyframes` and the receiver's `jitterBufferMs`, `decodeMs` and `processingMs`.

### How the frame loop is paced

The engine thread never polls for frames on a timer. Windows Graphics Capture signals an event the moment the
compositor hands over a frame, the hardware encoder signals one for every event it raises (input wanted, output
ready, input surface released), and the transport signals one for signaling or telemetry messages; the loop sleeps
on those three handles. Desktop duplication has no event, so that backend blocks inside `AcquireNextFrame`. Each
frame is converted into a four-surface NV12 ring and handed to the encoder with the compositor's timestamp, up to
three frames may be inside the encoder at once (a keyframe takes 20-30 ms on an integrated GPU and would otherwise
cost the next two frames), and the thread runs in the MMCSS "Capture" scheduling class. RTP packets carry the
playout-delay header extension set to zero so the browser renders each frame as soon as it is decoded. The Intel
encoder accepts but ignores live bitrate changes, so bitrate adaptation still recreates the encoder; the periodic
keyframe interval is 10 s (keyframes are otherwise produced on demand: viewer join, PLI, transport drop).

Logs go to `%LOCALAPPDATA%\BrowserMonitor\logs\host.log` (2 MB rotation) and `setup.log`; they never contain codes
or credentials.

## Troubleshooting

- **"Setup required" although the display exists**: an externally started `BrowserMonitorIddApp.exe` also works;
  the app streams it but cannot stop it (*Stop monitor* says so). Run *Set up…* once to let the app manage it.
- **"The display helper did not start"**: the scheduled task is missing or was blocked. Settings → *Repair setup…*.
  Group Policy that disables Task Scheduler for standard users prevents this design from working.
- **"BrowserMon is set as the primary display"**: make a physical display primary in Windows display settings.
  Browser Monitor will not stream a primary display.
- **Receiver says the code is not recognized**: codes expire after about two minutes; use the one currently shown.
  "Browser Monitor is not running on the host yet" means the worker knows the code but the host is offline.
- **Direct WebRTC connection failed**: the network blocks peer-to-peer traffic. No TURN relay is configured.
- **Signaling rejected this host**: the deployed worker speaks an older protocol; redeploy `signaling/`.
- Driver problems (code 52, code 10/31, `0x80070005`): see `driver/README.md`.

## Known limitations

- The window's controls are custom-drawn (Direct2D). Keyboard navigation and focus rings are implemented; UI
  Automation providers for screen readers are not.
- The scheduled task elevates whatever is at `%ProgramFiles%\Browser Monitor\BrowserMonitorDisplay.exe`; that
  folder is administrator-writable only, which is the point of copying the helper there.
- One 1920×1080@60 virtual monitor, one viewer, H.264 baseline over STUN only.
