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
a click or key press, so a reloaded page waits for one). The page holds a screen wake lock while the display is
connected, so the receiving laptop does not dim or sleep under you. A reload or a short signaling drop reconnects
by itself with the session token; *Disconnect* in the toolbar returns to the code page, and so does being
disconnected from the host.

LaptopMon is found by its EDID identity (manufacturer `LMV`, product `0001`, name `LaptopMon`), never by its
`\\.\DISPLAYn` number, which Windows reassigns. Laptop Monitor never falls back to a physical display: if
LaptopMon is missing it waits, and if it is set as the primary display it refuses and tells you.

### Scaling

The stream is always 1920×1080: nothing is downscaled and no browser zoom is involved. What changes is how large
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
  process RAM/GPU memory and the recent log on the clipboard; *Open log folder* opens `%TEMP%\LaptopMonitor`.
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

Signaling stays discovery only: video is direct WebRTC with STUN (no TURN). The default endpoint is
`https://browser-monitor-signaling.danielruoqiao.workers.dev`; change it in Settings or at build time with
`-DLM_DEFAULT_SIGNALING_URL=...`. The worker in `signaling/` speaks protocol version 2; deploy it with
`npm run deploy -w signaling` (or the *Deploy signaling* workflow) before using this version of the app against it.

## Complete uninstall / cleanup

Settings → **Uninstall…** (one UAC prompt), or run `LaptopMonitor.exe --uninstall` from an elevated prompt. It
removes, in this order:

1. the running helper and the scheduled task *Laptop Monitor Display*;
2. `%ProgramFiles%\Laptop Monitor\`;
3. the *start at sign-in* entry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\LaptopMonitor`);
4. `%LOCALAPPDATA%\LaptopMonitor\` (settings, host credential) and `%TEMP%\LaptopMonitor\` (logs);
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
                          app_state, settings, logging), display_query, pipeline (StreamingEngine)
host/app                  Win32 app: main, controller, display_control (helper client), setup, ui/ (Direct2D window,
                          renderer, tray)
host/helper               LaptopMonitorDisplay.exe
host/bench                laptop-monitor-bench.exe
host/tests                core-tests (bitrate, samples, H.264 framing), logic-tests (pairing, detection, lifecycle,
                          settings, SHA-256)
shared/protocol.ts        signaling message schema shared by worker and receiver
signaling/, viewer/       Cloudflare Worker and receiver page (viewer/src: main = views, stage = the display,
                          session = signaling + WebRTC, dashboard + telemetry = metrics, preferences = storage)
docs/                     logging-reference.md: what every logged field means, for performance work
tests/                    vitest (protocol, telemetry) and the signaling integration test
```

- C++: `./scripts/dev-build.ps1 -Configure` then `ctest --test-dir build/host`. Portable tests only:
  `cmake -S . -B build/logic -DLM_BUILD_HOST=OFF && cmake --build build/logic && ctest --test-dir build/logic`.
- Web: `npm ci`, `npm run typecheck`, `npm test`, then `npm run dev:signaling` in one terminal and
  `npm run test:integration` in another (exercises credential auth, code pairing, tickets, one-viewer limit,
  rotation overlap, resume, kick, host replacement, throttling against the local worker).
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

### Logging

Everything is written to `%TEMP%\LaptopMonitor\` (*Open log folder* in Details goes straight there), on two
channels, both governed by the *diagnostics log* setting and neither ever containing a pairing code or credential:

- `host.log` (2 MB, rotates to `host.1.log`) - readable lines: startup with the machine profile and the settings in
  force, lifecycle and state changes, how long the virtual display and the pipeline took to build and where that
  time went, encoder rebuilds with the receiver numbers that caused them, bitrate cuts, keyframe storms, a send
  queue that will not drain, a performance digest every 30 s, what the window itself costs every 2 min, and a
  one-line summary of each pipeline session as it ends. `setup.log` holds the elevated setup and uninstall runs.
- `perf.jsonl` (8 MB, rotates to `perf.1.jsonl`) - one JSON object per second while streaming, with the full
  sample: capture/encode rates and latencies with percentiles, the four drop causes counted apart (coalesced,
  superseded, ring busy, refused) plus paced frames, engine-thread wake-ups by reason and the share of wall time
  the thread was awake, keyframe versus delta frame size and encode cost, process CPU split into kernel and user
  time, working set, GPU memory in use, handles, battery and battery-saver state, encoder rebuild count and cost,
  the display re-enumeration stall, transport buffer pressure, the selected ICE route, and the receiver's own
  telemetry. This is the file to read when the question is "where is the CPU, GPU or memory going".

The per-second record is the same object the benchmark writes to its CSV and JSON sink, so a field measured in
`benchmarks/` means the same thing in a user's log. `perf.jsonl` is written while the streaming engine runs; it
does not need a connected receiver, so a stream waiting for one still records what idling costs.

**Every field, what a healthy value looks like and what a bad one points at: [`docs/logging-reference.md`](docs/logging-reference.md).**
Read that before instrumenting anything new.

#### Picture quality episodes

"It went extremely pixelated and glitchy, then fixed itself" is two different faults that look alike, and both are
over before anyone can look. The receiver reports the decoder's mean quantizer (pixelation itself: roughly 20-30
normal, over 36 visibly blocky), frames that arrived but never decoded, freezes, PLI and NACK; the host watches
those together with its own encoded bit rate and writes one line when quality drops and one when it recovers:

```
[warn ] Picture quality dropped: encoded rate fell to 1.4 Mbps from a usual 7.1 Mbps | encoder 1500 kbps …
[info ] Picture quality recovered after 14 s (started: …) | worst quantizer 44, lowest encoded rate 1.31 Mbps |
        encoder was recreated 2 time(s) at a lower bitrate: too few bits for this resolution
```

The closing line names the cause: an encoder recreated at a lower bitrate (too few bits for 1080p, so the
quantizer climbs and the picture turns to blocks), packet loss (torn and smeared blocks until a keyframe repairs
them), or a quantizer that rose on its own because the desktop content got harder to encode. Watching only; it
never changes what the pipeline does.

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
