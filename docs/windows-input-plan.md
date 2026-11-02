# Windows app input: investigation plan (pre-1.0)

**Symptom:** hosted Windows apps (e.g. File Explorer) render in their panel via Windows Graphics Capture, but mouse and keyboard input does not reach them.

**Status:** plan only. The deep dive happens in the final bug-fix stage before 1.0.

## Current design (starting facts)
- `src/WinCapture.cpp` captures the target HWND with Windows Graphics Capture (cursor capture disabled).
- `src/AppWindow.cpp` (~line 966) forwards input by briefly moving the real cursor (`SetCursorPos`), doing foreground tricks and sending a genuine click.

## Hypotheses to test (most likely first)
1. **Foreground/focus rules** - `SetForegroundWindow` is refused because the shell is not the foreground-lock owner, so synthetic input lands on the wrong window. Test `AttachThreadInput`, the ALT-key trick, and `AllowSetForegroundWindow`.
2. **UIPI / elevation** - input to an elevated target from a non-elevated shell is dropped. Check integrity levels of both processes; consider a `uiAccess` manifest (needs signing + a trusted install path) or documenting "run as admin".
3. **Window is not where we click** - the target is hidden, minimised, off-screen or on another monitor, so screen coordinates are wrong. Check DPI awareness (per-monitor v2 mismatch) and coordinate scaling.
4. **SendInput vs PostMessage** - compare `SendInput` (real, needs focus) with `PostMessage` of `WM_MOUSEMOVE/LBUTTONDOWN/KEYDOWN` to the correct child HWND (`ChildWindowFromPoint`), which works without focus for classic Win32 but not for all controls.
5. **UWP / WinUI / XAML Islands hosting** - Explorer's newer UI and UWP apps live in `ApplicationFrameWindow`/`Windows.UI.Core.CoreWindow`, which ignore posted messages and need real input to the correct HWND.
6. **Reparenting / occlusion** - moving the window off-screen or reparenting changes input routing; compare against leaving it in place and covering it.

## Experiments
1. Log `GetForegroundWindow`, `GetWindowThreadProcessId`, integrity level and DPI before and after each click.
2. Run the same click against a throw-away test window of our own, to separate shell bugs from target-app behaviour.
3. Try each strategy (SendInput with focus, PostMessage, UI Automation `InvokePattern`) behind an env switch and record the outcome.
4. Add a script command that dumps the HWND tree at the click point.

## Test matrix
| Target | Type | Elevated | Expected |
|---|---|---|---|
| Notepad | Win32 | no | click + typing |
| Explorer | Win32/XAML hybrid | no | click + typing |
| Calculator | UWP | no | click + typing |
| Task Manager | Win32 | yes | document limitation or uiAccess |
| Browser (Chromium) | Win32 + GPU | no | click + typing |

Also repeat at 100% and 150% DPI and on a secondary monitor.

## Exit criteria
Mouse (move, click, wheel) and keyboard work in the matrix rows above, or each failure has a documented reason and fallback.
