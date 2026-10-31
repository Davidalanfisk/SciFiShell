#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#ifndef _WINDEF_
struct HWND__;
typedef HWND__* HWND;
#endif

namespace sf {

struct WinCaptureImpl;

// Captures one top-level window through Windows.Graphics.Capture, which sees what the compositor draws: GPU-rendered
// content (Chromium / Electron, UWP, DirectX) that PrintWindow returns black or stale. The newest frame is kept, so a
// window that has not repainted still has an image to show.
class WinCapture {
public:
    WinCapture();
    ~WinCapture();
    // False when the OS or the window does not support capture; the caller falls back to PrintWindow.
    bool start(HWND h);
    void stop();
    bool active() const;
    // Pulls the newest frame (if any). Returns true once at least one frame has ever arrived.
    // The pixels are BGRA with the window's visible bounds, `w` x `h`.
    bool latest(std::vector<uint32_t>& bgra, int& w, int& h);

private:
    std::unique_ptr<WinCaptureImpl> d_;
};

}  // namespace sf
