# AGENTS.md

Orientation for anyone (human or agent) working in this repository. It says what each part is for, how the parts
talk to each other, how to build and test them, and which rules are not obvious from the code. Read this before
the code; read `README.md` for the user's view and `docs/logging-reference.md` before touching performance.

## What this is

Laptop Monitor turns a laptop with a browser into a second monitor for a Windows 11 PC. On the PC a virtual
1920x1080@60 display is added by an IddCx driver, captured on the GPU, encoded with the GPU's H.264 encoder and
streamed over a direct WebRTC connection to a web page on the other laptop. A Cloudflare Worker only introduces
the two machines. Video never touches a server, and the video path never touches the CPU.

Four deliverables, four toolchains:

| Part | Language / toolchain | Output | Runs where |
| --- | --- | --- | --- |
| Desktop app, elevated helper, bench tool | C++20, CMake + Ninja, MSVC, libdatachannel + Mbed TLS | `build/host/LaptopMonitor.exe`, `LaptopMonitorDisplay.exe`, `laptop-monitor-bench.exe` | Windows PC (host) |
| Virtual display driver | C++, MSBuild, WDK from NuGet | `driver/x64/Release/LaptopMonitorIdd/` | Windows PC, installed once |
| Signaling worker | TypeScript, Wrangler, Durable Objects | Cloudflare Worker `browser-monitor-signaling` | Cloudflare |
| Receiver page ("viewer") | TypeScript, Vite, vanilla DOM and CSS | `viewer/dist/`, published to GitHub Pages | The other laptop's browser |

The product used to be called Browser Monitor. Some names (the worker, the default signaling URL, install paths
that setup cleans up) still say so on purpose; do not rename them without reading the "Coming from the old Browser
Monitor build" section of `README.md`.

## Map

```
CMakeLists.txt            Host build: lm-logic (portable), lm-pipeline (Windows), app, helper, bench, tests
scripts/                  bootstrap.ps1 (winget deps), build-host.ps1 (deps + build + ctest), build-driver.ps1,
                          dev-build.ps1 (incremental build in the VS dev shell), install-driver.ps1 (elevated),
                          make-icons.py
host/include, host/src    The engine and the logic it is built from (see "Host" below)
host/app                  The Win32 desktop application (see "App" below)
host/helper               LaptopMonitorDisplay.exe: the only elevated process
host/bench                laptop-monitor-bench.exe: same pipeline, command line, CSV/JSON output
host/tests                core_tests.cpp (adaptation, stream shape, keyframe policy, RTP sample times, H.264 and
                          RTCP parsing, probe comparison, samples, latency/source-activity tracking, H.264 framing),
                          logic_tests.cpp (pairing, display identity, reducer lifecycle, settings, SHA-256, logging,
                          the per-run diagnostics archive)
driver/                   IddCx driver derived from Microsoft's sample; see driver/README.md
shared/protocol.ts        Signaling message schema and validators shared by worker and viewer
shared/ice.json           STUN servers and the WebRTC connection timeout, shared by host (via CMake) and viewer
signaling/src/index.ts    The worker: Code and Room Durable Objects, /pair, /room/<id> WebSocket, /health
viewer/                   The receiver page: index.html, src/{main,stage,session,dashboard,telemetry,preferences}.ts
tests/                    vitest unit tests (protocol, telemetry, preferences, probe) and signaling.integration.mjs
benchmarks/               run.ps1 runs the bench matrix; results/ is git-ignored measurement output
scripts/analyze-recording.py  decodes a bench recording or a mark dump with ffmpeg and compares it with the source
docs/logging-reference.md Every logged field, its window and what a bad value points at
.agents/skills/           Design skills used for frontend work (design-taste-frontend, redesign-existing-projects)
.github/workflows/        ci.yml (web + Windows build), pages.yml (viewer deploy), signaling.yml (worker deploy)
```

## How the pieces talk

**Display.** The app never touches the driver directly. `host/app/display_control.cpp` asks the Task Scheduler to
run `LaptopMonitorDisplay.exe --serve <pid>:<nonce>` (task "Laptop Monitor Display", registered by setup with
highest privileges), then talks to it over the named pipe `\\.\pipe\LaptopMonitor.Display.<nonce>` with two
commands, `ping` and `stop`. The helper creates the software device that loads the driver and exits when told,
when the pipe closes, or when the app process dies. The app finds the resulting monitor by EDID identity
(`LMV` / `0001` / "LaptopMon", `host/src/display_identity.cpp`), never by `\\.\DISPLAYn`.

**Lifecycle.** `host/src/app_state.cpp` is a pure reducer: `reduce(model, event) -> effects`. It has no platform
calls, which is why every stop/restart/exit chain is unit tested. `host/app/controller.cpp` executes the effects
(start display, start engine, kick viewer, quit) on the UI thread and feeds results back as events from any
thread through `post()`. Keep it that way: new lifecycle behaviour goes into the reducer and a test, not into the
window.

**Streaming.** `host/src/pipeline.cpp` (`StreamingEngine`) runs the capture -> D3D11 video processor (NV12) ->
hardware encoder MFT -> `transport.cpp` (libdatachannel WebRTC) loop on its own thread, in the MMCSS "Capture"
class, sleeping on three events (capture frame, encoder, transport). It publishes a `MetricsSnapshot`
(`host/include/metrics.hpp`) about once a second; the window copies it on a timer. Nothing in the GUI may run on
the frame path.

**Pairing.** The transport owns `PairingCodes` (`host/src/pairing.cpp`): a six-character code from the alphabet
without I, O, 0, 1; rotates every 120 s; the previous code stays valid 15 s. Only SHA-256 hashes of codes are
published to the worker. The host's identity is a random 256-bit credential (DPAPI-protected in
`%LOCALAPPDATA%\LaptopMonitor\host.credential`); its room is the first 32 hex characters of SHA-256 over it, which
the worker verifies statelessly. The viewer POSTs the code to `/pair`, gets a room and a single-use ticket,
authenticates its WebSocket with the ticket, receives a 12 h resume token, and keeps it in `sessionStorage` so a
reload reconnects without a code. One viewer per host; the host can kick (revokes the token, issues a new code).
Protocol version is 2 and lives in `shared/protocol.ts`; the worker rejects other versions. Additions since are
announced as `features` in "authenticated" and used only when listed, so new clients still work against an older
deployed worker: `heartbeat` (the text frame "ping" is answered "pong" by the runtime) and `resume` (a peer whose
WebSocket reconnects while its media connection is up says so with `live` on auth and `state` messages; when both
peers still hold the current generation the worker answers `ready` with `resume: true` and nobody renegotiates).
The signaling socket is only needed to (re)negotiate: neither side tears down a connected peer connection because a
WebSocket closed.

**Receiver flow.** `viewer/src/main.ts` owns the two views: the pairing page and the stage (`stage.ts`). A
`Session` (`session.ts`) reports `phase` as `pairing` -> `busy` -> `connected`; the page switches to the stage on
the first `busy`, which is inside the gesture that submitted the form, so the fullscreen request is allowed.
Typing the sixth character submits. The stage hides the cursor and toolbar after 2.5 s of stillness, holds a screen
wake lock, and returns to the pairing page only when the session ends (disconnect, kick, rejected credentials).
`dashboard.ts` renders receiver, connection and host metrics; `telemetry.ts` turns `getStats()` counters into
interval values and is what the host receives over the "telemetry" data channel, together with the session's event
ring (connection states, WebSocket close codes) that the host writes to its log. Over the same channel the host asks
for quality probes (`probe.ts` reads the decoded frame from the track and returns its luma grid) and the M key on
the stage sends a mark that makes the host save the last seconds of its stream.

## Build, run, test

Web (any OS):

```
npm ci
npm run typecheck            # viewer + worker
npm test                     # vitest: tests/*.test.ts
npm run build                # viewer -> viewer/dist
npm run dev:viewer           # http://127.0.0.1:5173
npm run dev:signaling        # wrangler dev on 127.0.0.1:8787 (use --port if 8787 is taken on your machine)
npm run test:integration     # tests/signaling.integration.mjs against SIGNALING_URL (default ws://127.0.0.1:8787)
```

Host (Windows, Visual Studio Build Tools with the C++ workload, CMake, Ninja, Python):

```
./scripts/build-host.ps1                                   # clones pinned deps, builds everything, runs ctest
./scripts/dev-build.ps1 -Targets LaptopMonitor,logic-tests # incremental; -Configure the first time
ctest --test-dir build/host --output-on-failure
cmake -S . -B build/logic -DLM_BUILD_HOST=OFF && cmake --build build/logic && ctest --test-dir build/logic
                                                           # portable logic tests only, no Windows SDK needed
```

Driver: `./scripts/build-driver.ps1` (no admin), install with `./scripts/install-driver.ps1` (elevated). Details
and troubleshooting in `driver/README.md`.

CI (`.github/workflows/ci.yml`) runs exactly the web commands above plus a `wrangler deploy --dry-run` and the
integration test against a local worker, then builds the host on `windows-2025`. Anything that must keep working
should be covered by one of those steps.

Local end-to-end without the driver or the app: run the worker on a free port, `npm run dev:viewer`, open the
page, set Advanced -> Signaling server to the worker, then either open the page in a second tab and use Advanced
-> "Sender test" -> "Stream test pattern" as the host, or run the bench as the host:
`laptop-monitor-bench --display \\.\DISPLAY2 --capture wgc --mode stream --signaling http://127.0.0.1:8788 --pattern`
(run from PowerShell; bash mangles `\\.\DISPLAY2`). The receiver resumes across host restarts, so pair once.

## Rules that are not visible in the code

Security and privacy:

- Pairing codes, the host credential, tickets and tokens are never logged, never put in a URL, never shown except
  the current code in the app. The receiver link the app can copy carries the code in the URL fragment only.
- The viewer refuses non-HTTPS signaling except on localhost (`preferences.ts`, `session.ts`); the host does the
  same in `settings.cpp::sanitized`. Keep both in step.
- Everything the worker relays is validated by `parseClient` in `shared/protocol.ts`; unknown fields are dropped,
  not forwarded. Add fields there and in the tests, never by loosening the validator.
- No TURN relay is configured and `DIRECT_FAILURE` says so; do not add a relay quietly.
- The elevated helper takes exactly two commands over its pipe. Anything new that needs elevation goes through
  setup (`host/app/setup.cpp`), which runs once with UAC, not through the helper.

Driver:

- It is Microsoft's IddCx sample (commit `97429c5`) with the minimal diff listed in `driver/README.md`. Keep diffs
  against the sample minimal; do not rewrite it.
- Never change Secure Boot, test signing, BCD, driver signature enforcement or BitLocker on a machine as part of a
  fix. Installation works with Secure Boot on; if it does not, the fix is in the certificate steps.

Host:

- The engine thread is event driven; do not add polling timers or sleeps to the loop. Read the "How the frame loop
  is paced" section of `README.md` before touching `pipeline.cpp`.
- The Intel encoder ignores live bitrate changes; bitrate changes recreate the encoder (`encoder.cpp`), which is a
  visible 250-400 ms hitch, so adaptation (`NetworkAdaptation` in `core.hpp`) moves in few, large steps. Its inputs
  are loss and queueing delay from RTCP only: jitter and REMB swing with keyframe bursts on a clean LAN, and acting
  on them starved a loss-free stream to 1.9 Mbps, where Quick Sync runs at QP 50 and leaves stale, blocky regions
  (`docs/logging-reference.md`, "Diagnosed 2026-09-22"). Judge picture quality by `qp_mean` and the quality probes,
  not by the receiver's `corrupted` counter.
- Frames handed to the network must form an unbroken H.264 reference chain: after a frame that never reached the
  packetizer the engine withholds delta frames until the next IDR, and sample times stay at least one RTP tick apart.
  Keep both when changing the send path.
- Settings are few on purpose (`host/include/settings.hpp`). Internal tuning stays in code.
- Current logs go to `%TEMP%\LaptopMonitor\` (`host.log`, `perf.jsonl`, `setup.log`); each run with diagnostics on
  is also archived in `%LOCALAPPDATA%\LaptopMonitor\logs\sessions\<run_id>\` (same lines plus `session.json`),
  which only uninstall or a person deletes. Settings and the credential live in `%LOCALAPPDATA%\LaptopMonitor\`.
  Every field written to `perf.jsonl` must be described in `docs/logging-reference.md`, and instrumentation stays
  reporting only: nothing may act on a diagnostic field without saying so.
- The window is custom drawn (Direct2D); controls are re-laid out every paint in `drawOverview/Details/Settings`
  and hit-tested from that layout. It is 440x664 DIP and does not scroll, so new controls need a place, not just
  an ID. The palette (`renderer.hpp::Theme`) is the product palette: #FFDBBB, #CCBEB1, #997E67, #664930 over
  off-white paper.
- Source files under `host/` use CRLF line endings; keep them.

Viewer:

- Vanilla TypeScript and CSS, no framework, no runtime dependencies. Fonts are self-hosted from `@fontsource-variable`
  packages (Instrument Sans, Geist Mono) and imported in `main.ts`; never link a font CDN.
- `style.css` defines the tokens: the same four product colours plus derived tints, with a `prefers-color-scheme`
  dark set. Containers use `--radius-container`, controls `--radius-control`; nothing else is rounded.
- Element IDs are the contract between `index.html`, `main.ts` and `stage.ts` (`code`, `server`, `join`,
  `connect`, `cancel`, `message`, `auto-fullscreen`, `stage`, `video`, `play`, `toolbar`, `stats`, `fullscreen`,
  `disconnect`, `dashboard`, `dashboard-grid`, `test-*`). Rename in all three or not at all.
- Copy rules from `.agents/skills/design-taste-frontend/SKILL.md` apply to every visible string: plain sentences,
  no em-dashes or en-dashes, no marketing filler, unavailable values read `n/a`. Run the skill's pre-flight
  check before shipping a visual change, and look at the page in both colour schemes and at phone width.
- Fullscreen can only be requested inside a user gesture. The flow that keeps it working is described under
  "Receiver flow" above; if you move where the stage appears, test it in Chrome, Edge and Safari.

## Where to change what

| Want to | Look in |
| --- | --- |
| Change how the app reacts to a state change | `host/src/app_state.cpp` + a case in `host/tests/logic_tests.cpp` |
| Add a control or a metric to the window | `host/app/ui/main_window.cpp` (`drawOverview`, `drawDetails`, `drawSettings`, `activate`) |
| Add a setting | `host/include/settings.hpp`, `settings.cpp` (JSON + `sanitized`), `main_window.cpp` (Settings page), `controller.cpp::updateSettings` |
| Change capture, conversion or encoding | `host/src/{capture,wgc,converter,encoder}.cpp`; measure with the bench before and after |
| Change pairing or the signaling protocol | `shared/protocol.ts`, `signaling/src/index.ts`, `host/src/transport.cpp`, `viewer/src/session.ts`, `tests/` (unit + integration), bump `PROTOCOL_VERSION` if incompatible |
| Change what the receiver shows | `viewer/index.html`, `viewer/src/style.css`, `viewer/src/main.ts` (views), `stage.ts` (stage behaviour), `dashboard.ts` (metrics) |
| Change what the host logs | `host/src/logging.cpp`, `pipeline.cpp` (per-second record), `session_archive.cpp` (run archive), `host/app/diagnostics.cpp` (when logs open and close), then `docs/logging-reference.md` |
| Change setup, uninstall or elevation | `host/app/setup.cpp`, `scripts/install-driver.ps1`; both must also clean up the old Browser Monitor names |

## Gotchas

- `npm ci` must be run from the repo root: `viewer` and `signaling` are npm workspaces.
- `wrangler dev` needs a free port; on some development machines 8787 is taken, use `--port 8788` and point both
  the viewer (Advanced) and the app (Settings -> Signaling URL) at it.
- The Pages build passes `VITE_SIGNALING_URL` from a repository variable that may be an empty string; `main.ts`
  therefore falls back on any falsy value, not only `undefined`.
- Windows reassigns `\\.\DISPLAYn`; anything that identifies the virtual display must go through
  `display_identity.cpp` / `display_query.cpp`.
- A receiver tab that is in the background counts frames as dropped; judge smoothness with the tab in front.
- WGC frame timestamps are the compositor's vsync target, not the capture instant, so "frame wait" can read
  slightly negative. That is expected.
- The viewer's `sessionStorage` resume means a reload reconnects silently. When testing a fresh pairing, use a new
  tab or "Disconnect" first.
