#pragma once
// Keyboard + mouse input via SDL2, collected into one per-frame snapshot.
#include <SDL.h>

#include <string>
#include <vector>

namespace sf {

// Text-editing keys. Unlike the edge-triggered flags below these also fire on key repeat.
enum class EditKey { Backspace, Delete, Enter, Tab, Left, Right, Up, Down, Home, End, PageUp, PageDown, DocStart, DocEnd, Save, Paste, Edit, Copy, Cut, SelectAll, Undo };

struct FrameInput {
    std::vector<EditKey> keys;      // this frame's editing keys, in order
    std::string text;               // typed text (UTF-8) from SDL text input
    float wheel = 0;                // mouse wheel notches (positive = up)
    float moveX = 0, moveY = 0;     // strafe / forward, -1..1
    float lookX = 0, lookY = 0;     // mouse delta in pixels (relative mode only)
    bool select = false, back = false, up = false, down = false;  // edge-triggered
    bool space = false;             // space pressed this frame (menu select; fly-up in the room)
    bool escape = false;            // Escape pressed this frame
    float moveZ = 0;                // fly: +1 space, -1 left ctrl
    bool left = false, right = false, fullscreen = false;  // edge-triggered (viewer shortcuts: seek, F)
    bool shift = false;             // left shift held (movement boost)
    bool click = false;             // left mouse button pressed this frame
    bool clickUp = false;           // left mouse button released this frame
    bool browser = false;           // B pressed (open the web browser)
    bool focusAddress = false;      // Ctrl+L
    bool depthCycle = false;        // F3: web depth effects Off/Subtle/Cinematic
    bool depthSite = false;         // F4: toggle depth effects for the current site
    bool reload = false;            // F5
    bool ring = false;              // middle click / F2: summon the tool ring
    bool pin = false;               // right click: pin the view to the window under the aim (or release it)
    bool themeCycle = false;        // F6: next colour theme
    bool navBack = false, navForward = false;  // Alt+Left / Alt+Right
    int tabStep = 0;                // Alt+Tab / Ctrl+Tab: +1 next task, -1 previous (with Shift)
    bool modReleased = false;       // Alt or Ctrl released (commits the task switcher)
    bool appFullscreen = false;     // F11: toggle the hosted program's fullscreen mode
    bool screenshot = false;        // F12: save a screenshot
    bool toggleVR = false;
    bool quit = false;
    std::vector<std::string> dropped;  // files / folders dropped on the window
    bool resized = false;
    bool mouseMoved = false;
    int mouseX = 0, mouseY = 0;
};

class InputSystem {
public:
    // Pumps SDL events and returns this frame's input.
    FrameInput poll();
    void setRelativeMouse(bool enabled);
    bool relativeMouse() const { return relative_; }
    void setTextInput(bool enabled);  // SDL text input (IME / typed characters) for the editor

private:
    bool relative_ = false;
};

}  // namespace sf
