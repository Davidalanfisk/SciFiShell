#pragma once
// Page layer analysis for the 3D Web Depth Compositor.
//
// The plan called for a CefDOMVisitor. A visitor only runs inside the renderer *process*, which would need a
// second set of IPC plumbing. Instead the analyzer injects a read-only script through CefFrame::ExecuteJavaScript that
// walks the DOM, reads computed styles and element rectangles, and reports a compact text summary through the
// console (captured by CefDisplayHandler::OnConsoleMessage). The DOM is never modified.
#include <cstdint>
#include <string>
#include <vector>

#include "include/cef_frame.h"

namespace sf {

enum class WebElementKind { Card = 0, Image = 1, Video = 2 };

// Depth bands: 0 background, 1 content, 2 UI, 3 overlay.
constexpr int kWebBandCount = 4;

struct WebElement {
    WebElementKind kind = WebElementKind::Card;
    int band = 1;
    float x = 0, y = 0, w = 0, h = 0;  // viewport pixels (same space as the page texture)
    float radius = 0;                  // CSS border radius (px)
    int z = 0;                         // computed z-index (ordering hint)
};

struct WebLayout {
    std::vector<WebElement> elements;
    double analysisMs = 0;   // time the script took inside the page
    int scanned = 0;         // elements visited
    bool tooComplex = false; // hit the scan cap or the time budget -> compositor falls back to a single quad
    uint64_t version = 0;    // bumped whenever a new result arrives
};

class WebLayerAnalyzer {
public:
    // Budgets (also enforced inside the injected script).
    static constexpr int kMaxScan = 10000;
    static constexpr int kMaxCards = 20;
    static constexpr int kMaxMedia = 24;
    static constexpr double kMaxMs = 40.0;

    // Runs the analysis script in `frame`. Safe to call repeatedly; callers debounce.
    void request(CefRefPtr<CefFrame> frame, int viewW, int viewH);
    // Feeds a console line. Returns true when it was an analyzer report (so it can be hidden from the log).
    bool onConsole(const std::string& message);
    void clear();

    const WebLayout& layout() const { return layout_; }
    bool editableFocused() const { return editable_; }

private:
    WebLayout layout_;
    bool editable_ = false;
};

}  // namespace sf
