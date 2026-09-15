# Browser Monitor virtual display driver

A single **1920×1080 @ 60 Hz** virtual monitor for Windows 11, built on Microsoft's IddCx sample
[`microsoft/Windows-driver-samples/video/IndirectDisplay`](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay)
at commit `97429c5623590d52f001249460daf43e6749d777` (MS-PL, see `LICENSE`).

The host (`browser-monitor.exe`) needs no changes. The monitor appears as a normal `\\.\DISPLAYn` output, and the
existing DXGI duplication / WGC capture path can select it with `--display`.

## Changes from the sample

| Sample | Here |
| --- | --- |
| `IddSampleDriver/` | `BrowserMonitorIdd/` (renamed DLL, INF, catalog, hardware IDs `BrowserMonitorIdd` / `Root\BrowserMonitorIdd`, `DeviceGroupId`, WPP GUID) |
| 3 monitors, 2 borrowed Dell/Lenovo EDIDs | 1 monitor with its own EDID 1.4 (`BrowserMon`, one 1920×1080@60 CTA-861 timing, 527×296 mm) |
| 3 monitor modes and 10 target modes | only 1920×1080@60 in the monitor, default and target mode lists |
| `IddSampleApp/` | `BrowserMonitorIddApp/` (new IDs; also checks the creation `HRESULT` the sample ignored) |
| ARM64 + x64, installed WDK | x64 only, WDK/SDK from pinned NuGet packages (`packages.config`, `Directory.Build.props`) |

Frame handling is unchanged: `SwapChainProcessor::RunCore` acquires each frame and releases it right away.
Shared textures and forwarding frames to the host are not implemented yet.

## Build (no admin)

```powershell
./scripts/build-driver.ps1            # -Configuration Debug for a debug build
```

This needs Visual Studio Build Tools with the C++ workload and the MSVC Spectre-mitigated libraries. The script
downloads `nuget.exe` into `.deps/tools` and restores the WDK and SDK packages into `.deps/packages`. It builds
with `SignMode=Off`, because the WDK's auto-generated test certificate needs elevation. Output:

- `driver/x64/Release/BrowserMonitorIdd/`: driver package (`.inf`, `.dll`, unsigned `.cat`)
- `driver/x64/Release/BrowserMonitorIddApp.exe`: creates the monitor device

## Install and run (elevated PowerShell)

```powershell
./scripts/install-driver.ps1                         # sign catalog, trust cert, pnputil /add-driver
./driver/x64/Release/BrowserMonitorIddApp.exe        # monitor exists until you press x
```

`install-driver.ps1` creates a self-signed code-signing certificate (non-exportable key in `LocalMachine\My`).
It adds that certificate to `LocalMachine\Root` and `TrustedPublisher` on **this machine only**, signs the catalog
with it, and stages the package. `./scripts/install-driver.ps1 -Uninstall` removes the driver package and the
certificate.

## Verify

- **Settings → System → Display** shows a second display, **BrowserMon**, at 1920×1080. Its advanced display
  settings show a 60 Hz refresh rate.
- `build/host/browser-monitor.exe --list` lists the new `\\.\DISPLAYn`, which can be streamed with
  `--display \\.\DISPLAYn`.
- In Device Manager, **Display adapters → Browser Monitor Virtual Display** reports no problem code.

## Troubleshooting

- **Code 52 (signature)**: the certificate was not trusted. Rerun `install-driver.ps1`. If policy still blocks it,
  enable test signing: `bcdedit /set testsigning on` and reboot. Secure Boot must be off for this.
- **Code 10 / 31, or the monitor never appears**: check Event Viewer under **Applications and Services Logs →
  Microsoft → Windows → DriverFrameworks-UserMode**.
- **`SwDeviceCreate failed` / `Device creation failed`**: run the app elevated after `install-driver.ps1`.
