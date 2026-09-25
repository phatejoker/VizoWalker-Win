# VizoWalker Native Windows v0.1.2

Proof-of-concept native replacement for the former WebView2-based Extended Canvas.

## Scope of this build

This version deliberately does **not** change:
- Surface resolution
- Surface brightness
- Windows registry
- drivers
- display drivers / virtual displays
- system services
- security policies

It only uses:
- Win32
- Windows Graphics Capture
- Direct3D 11 / DXGI
- the standard Windows HID stack

## Expected topology

1. Windows display mode: **Extend these displays**
2. Surface internal panel: **primary display**
3. VIZO Z1 Pro: **secondary display**
4. For this first test, manually set the Surface to **1920×1080 before launch**

At startup VizoWalker Native:
- captures the Windows primary monitor
- opens borderless fullscreen on the first non-primary monitor
- includes the real Windows cursor in the capture
- reads VIZO HID tracking (`VID_0483`, `PID_5743`, report 3)
- applies the same quaternion mapping and 42° / 98% world-locked plane geometry used by the working browser prototype

## Hotkeys

Global hotkeys:

- `Ctrl+Alt+Shift+C` — recenter
- `Ctrl+Alt+Shift+V` — toggle cursor confinement to the Surface primary display
- `Ctrl+Alt+Shift+Q` — emergency quit

`Esc` also closes the app when its window has focus.

Cursor confinement is enabled by default to reproduce the previous Extended Canvas workflow.
It is explicitly released on normal app shutdown and also by the top-level process guard.

## Important: resolution/brightness automation is NOT in v0.1

This first native build intentionally leaves display configuration untouched.

If the native architecture proves stable, the next version can add a transactional display-state layer:

1. read and persist current Surface resolution + brightness
2. switch primary panel to 1920×1080
3. set brightness to minimum
4. start capture
5. on every normal shutdown path restore the exact saved state
6. retain a recovery state file until restore succeeds, so a later launch can recover from an abnormal termination

That automation should only be added after this native capture/render/tracking path is validated.

## Build

GitHub Actions:
- Actions → **Build VizoWalker Native**
- Run workflow
- download `VizoWalker-Native-win-arm64`

No WebView2 Runtime is used by this project.

## v0.1.1

Compilation fix only:
- removed `Windows` namespace ambiguity between Win32 interop headers and C++/WinRT
- explicitly qualified Windows Graphics Capture / DirectX WinRT types
- corrected D3D11 COM device conversion to C++/WinRT
- corrected `winrt::hstring` to `std::wstring` error reporting conversion

No runtime behavior, permissions, display settings, registry, driver, or system-state logic was added.

## v0.1.2

Compilation fix only:
- fully qualifies the COM `IDirect3DDxgiInterfaceAccess` interface from
  `windows.graphics.directx.direct3d11.interop.h`

No runtime behavior or system interaction changed.
