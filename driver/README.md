# Laptop Monitor virtual display driver

A single **1920×1080 @ 60 Hz** virtual monitor for Windows 11, built on Microsoft's IddCx sample
[`microsoft/Windows-driver-samples/video/IndirectDisplay`](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay)
at commit `97429c5623590d52f001249460daf43e6749d777` (MS-PL, see `LICENSE`).

The host needs no changes. The monitor appears as a normal `\\.\DISPLAYn` output; the app finds it by its EDID
identity (`LMV0001` / "LaptopMon") and the bench tool selects it with `--laptopmon` or an explicit `--display`.

In daily use the device is created by `LaptopMonitorDisplay.exe` through the scheduled task that Laptop Monitor's
one-time setup registers (see the top-level README). `LaptopMonitorIddApp.exe` remains as the standalone
console version of the same thing for driver testing.

## Changes from the sample

| Sample | Here |
| --- | --- |
| `IddSampleDriver/` | `LaptopMonitorIdd/` (renamed DLL, INF, catalog, hardware IDs `LaptopMonitorIdd` / `Root\LaptopMonitorIdd`, `DeviceGroupId`, WPP GUID) |
| 3 monitors, 2 borrowed Dell/Lenovo EDIDs | 1 monitor with its own EDID 1.4 (`LaptopMon`, one 1920×1080@60 CTA-861 timing, 294×166 mm) |
| 3 monitor modes and 10 target modes | only 1920×1080@60 in the monitor, default and target mode lists |
| `IddSampleApp/` | `LaptopMonitorIddApp/` (new IDs; also checks the creation `HRESULT` the sample ignored) |
| ARM64 + x64, installed WDK | x64 only, WDK/SDK from pinned NuGet packages (`packages.config`, `Directory.Build.props`) |

Frame handling is unchanged: `SwapChainProcessor::RunCore` acquires each frame and releases it right away.
Shared textures and forwarding frames to the host are not implemented yet.

### Why 294 × 166 mm

The EDID's physical image size is the only thing that tells Windows how large to draw on this monitor, and it is
what the *recommended* per-display scaling is computed from. 294 × 166 mm is a 13.3" 16:9 panel, which puts
1920×1080 at ~166 DPI and makes Windows recommend 150% — a small laptop screen, which is exactly what the receiver
is. The sample's 527 × 296 mm (a 24" panel, ~93 DPI) recommended 100% and left text unreadably small on the
receiving laptop. The declared timing is untouched, so the stream is still a full 1920×1080. The declared size is
in both places Windows reads it: the basic display parameters (29 × 17 cm) and the detailed timing descriptor.

The host can pin an explicit percentage per display on top of that; see `host/src/display_scale.cpp`.

## Build (no admin)

```powershell
./scripts/build-driver.ps1            # -Configuration Debug for a debug build
```

This needs Visual Studio Build Tools with the C++ workload and the MSVC Spectre-mitigated libraries. The script
downloads `nuget.exe` into `.deps/tools` and restores the WDK and SDK packages into `.deps/packages`. It builds
with `SignMode=Off`, because the WDK's auto-generated test certificate needs elevation. Output:

- `driver/x64/Release/LaptopMonitorIdd/`: driver package (`.inf`, `.dll`, unsigned `.cat`)
- `driver/x64/Release/LaptopMonitorIddApp.exe`: creates the monitor device

## Install, start, stop, uninstall

Tested on Windows 11 Pro 10.0.26200 (Intel Iris Xe) with **Secure Boot on and test signing off**. No BCD,
Secure Boot, driver-signature-enforcement or BitLocker change is needed.

1. **Install** (elevated PowerShell, once):

   ```powershell
   ./scripts/install-driver.ps1
   ```

   It creates `CN=Laptop Monitor IDD Local Test Signing` in `LocalMachine\My`: a self-signed code-signing
   certificate (code-signing EKU only, `CA=false`, non-exportable key, valid 2 years). It adds the public certificate
   to `LocalMachine\Root` and `LocalMachine\TrustedPublisher` on this machine only, signs
   `laptopmonitoridd.cat`, verifies it and runs `pnputil /add-driver` (published as `oemNN.inf`).

2. **Start** (elevated; plain `SwDeviceCreate` fails with `0x80070005`):

   ```powershell
   ./driver/x64/Release/LaptopMonitorIddApp.exe     # prints "Device created"; the monitor exists while this runs
   ```

   On first start Windows may attach LaptopMon in **Duplicate** mode, sharing `\\.\DISPLAY1`. Choose
   **Settings → System → Display → LaptopMon → Extend desktop to this display** and **Keep changes** once. Windows
   remembers this for the monitor (it came back extended after stop/start and after uninstall/reinstall). The monitor
   is invisible: if a window lands on it, move it back with **Win+Shift+←/→**.

3. **Stop**: press **x** in the app's console. The app exits with code 0 and the software device is removed.

4. **Uninstall** (elevated):

   ```powershell
   ./scripts/install-driver.ps1 -Uninstall
   ```

   It stops `LaptopMonitorIddApp.exe` if it is running, then removes the `SWD\LaptopMonitorIdd` device node and
   the driver package (including `drivers\UMDF\LaptopMonitorIdd.dll`). It also removes the certificate from all three
   stores and deletes its private key. Windows keeps the non-present `DISPLAY\LMV0001` monitor node, like any unplugged
   monitor.

## Verify

- Device Manager / `Get-PnpDevice`: **Laptop Monitor Virtual Display** (`SWD\LAPTOPMONITORIDD\LAPTOPMONITORIDD`,
  `oemNN.inf`) and **Generic Monitor (LaptopMon)** (`DISPLAY\LMV0001\…`) are both `OK` with problem code 0.
- The monitor's registry EDID matches `s_SampleMonitors` byte for byte. `WmiMonitorID` reports manufacturer `LMV`,
  product `0001`, name `LaptopMon`.
- `QueryDisplayConfig` / `EnumDisplaySettings`: target `LaptopMon`, 1920×1080, 60 Hz, only mode `1920x1080@60`,
  non-primary, on adapter **Laptop Monitor Virtual Display**.
- Settings → System → Display → *LaptopMon* offers **150% (Recommended)** in the scale list, and
  `(Get-WmiObject WmiMonitorBasicDisplayParams -Namespace root\wmi).MaxHorizontalImageSize` reports `29` cm for it.
- `build/host/laptop-monitor-bench.exe --list` shows it as a secondary display on the render GPU, e.g.
  `\\.\DISPLAY5 | Intel(R) Iris(R) Xe Graphics | 1920x1080 | secondary`, followed by a `LaptopMon:` line with
  its device path.

The `\\.\DISPLAYn` number is not stable: it changed from `DISPLAY5` to `DISPLAY6` after an uninstall/reinstall. The
monitor device path (`\\?\DISPLAY#LMV0001#1&1eee597c&0&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}` on the test
machine) is what `--remember` stores as the display identity. It stays the same across stop/start, but a reinstall
creates a new instance (`…&1&UID256…`), so pair again with `--remember` after reinstalling.

## Baseline with the existing host

Capture, conversion and encode use the host unchanged. The frame stays on the GPU the whole way: DXGI duplication or
WGC texture → D3D11 video processor BGRA→NV12 → `MFCreateDXGISurfaceBuffer` into the hardware MFT. The host reports
`video_path=GPU` and `CPU readback: no`; only the compressed H.264 bitstream is read on the CPU.

```powershell
./build/host/laptop-monitor-bench.exe --laptopmon --capture dxgi --mode capture-encode --pattern --seconds 20 --csv results.csv
./build/host/laptop-monitor-bench.exe --laptopmon --capture wgc  --mode capture-encode --pattern --seconds 20
```

`--pattern` draws a moving bar on the selected display so there are frames to capture. Measured on 2026-09-15:
Intel Iris Xe, Intel® Quick Sync Video H.264 Encoder MFT, 1920×1080@60, 20 s per run. Figures are means, with
p95 after the slash.

| Run | Capture fps | Encode fps | Dropped | Capture ms | Convert submit ms | Encode ms | Capture→encoded ms | CPU % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| DXGI capture | 51.8 | – | 133 | 0.15 / 0.34 | – | – | – | 1.2 |
| DXGI convert | 43.4 | – | 240 | 0.16 / 0.31 | 0.94 | – | – | 1.9 |
| DXGI capture-encode | 54.8 | 54.8 | 89 | 0.16 / 0.28 | 0.83 | 5.16 / 6.48 | 7.44 / 8.97 | 4.0 |
| WGC capture | 56.8 | – | 0 | 0.014 / 0.026 | – | – | – | 1.2 |
| WGC convert | 58.6 | – | 0 | 0.015 / 0.031 | 0.81 | – | – | 1.9 |
| WGC capture-encode | 54.5 | 54.6 | 0 | 0.026 / 0.077 | 0.89 | 4.96 / 6.44 | 7.30 / 8.81 | 4.1 |
| Encode only (generated GPU source) | 59.8 | 59.8 | 0 | – | – | 4.27 / 4.88 | 5.13 / 5.92 | 1.2 |

For DXGI, `dropped` counts desktop updates that duplication merged between two polls (`AccumulatedFrames - 1`),
not encoder rejections. "Convert submit" is the CPU time spent submitting the video-processor work, which runs
asynchronously on the GPU. The pattern is drawn by the same loop that captures, so it limits these frame rates.

**WebRTC** (DXGI, local `npm run dev -w signaling` + viewer in Chrome, same machine, 77.6 s connected): the host
captured, encoded and sent about 50 fps with zero transport drops. It had to recreate the encoder once at 10 Mbps
because live bitrate changes are not supported, and sent 33 keyframes. The encoder took 4.86 / 6.28 ms, capture to
encoded output 6.44 / 7.91 ms, CPU 2.8%. The viewer reported direct WebRTC with no relay: 3883 frames decoded, 0
dropped, 51 fps, 0% loss, 1 ms RTT, 8.5 Mbps. The peer connected 128 ms after `ready`, and the first keyframe went
out at 157 ms. Chrome counts frames as dropped while the viewer tab is hidden, so keep it in the foreground when
judging smoothness.

## Troubleshooting

- **Monitor shows only as duplicate / `--list` shows no secondary display**: Windows attached LaptopMon in
  Duplicate mode; pick **Extend desktop to this display** (see Start).
- **Code 52 (signature)**: this did not happen with Secure Boot on. Check that the certificate is in `Root` and
  `TrustedPublisher` and rerun `install-driver.ps1`. Test signing requires turning Secure Boot off, which is a
  machine security decision; don't use it as a routine fix.
- **Code 10 / 31, or the monitor never appears**: check Event Viewer under **Applications and Services Logs →
  Microsoft → Windows → DriverFrameworks-UserMode**.
- **`SwDeviceCreate failed with 0x80070005`**: run `LaptopMonitorIddApp.exe` elevated.
- **`Device creation failed` / no driver**: run `install-driver.ps1` first.
