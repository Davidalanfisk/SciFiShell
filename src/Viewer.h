#pragma once
// Media viewer: a card flies out of the File Room, grows into a panel with the media surface, a details card and
// a control bar, and plays video/audio (MediaPlayer) or shows an image. Fullscreen fills the view.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CodeEditor.h"
#include "InputSystem.h"
#include "MediaPlayer.h"
#include "SceneGraph.h"
#include "Thumbnails.h"

namespace sf {

struct OpenRequest {
    std::string path, name;
    FileKind kind = FileKind::Other;
    Vec3 cardPos;
    float cardYaw = 0;
    Vec3 cardScale{1.4f, 0.9f, 0.06f};
    std::shared_ptr<GpuLabel> poster;  // the card's thumbnail (may be null)
};

// Where the user's head is (desktop camera or VR headset), used to place and follow the viewer.
struct HeadPose {
    Vec3 eye;
    Vec3 forward;
    bool vr = false;
    float aspect = 16.0f / 9.0f;
    float tanHalf = 0.684f;  // tan(fovY / 2), used to size panels against the screen
};

class Viewer {
public:
    explicit Viewer(Renderer& renderer);
    ~Viewer();

    bool open(const OpenRequest& req, const HeadPose& head);
    void close();                       // starts the shrink animation; active() turns false when finished
    bool active() const { return active_; }
    bool closing() const { return closing_; }
    Node& root() { return *root_; }

    void update(double dt, const HeadPose& head);

    void togglePause();
    void seekBy(double seconds);
    void changeVolume(float delta);
    void toggleFullscreen(const HeadPose& head);
    bool fullscreen() const { return fullscreen_; }
    // Desktop fullscreen: the viewer fills the screen in view space (the world is not drawn).
    bool overlay() const { return fullscreen_ && !lastHead_.vr && active_ && !closing_; }
    bool controlsShown() const { return barFade_ > 0.02f; }
    void wake() { idle_ = 0; }                 // any user input brings the fullscreen controls back
    void zoomBy(float factor);                 // fullscreen images only
    void panBy(float ndcX, float ndcY);        // drag, in fractions of the screen
    // Pointer press at a ray (mouse crosshair or VR controller). Returns true when the viewer used it.
    bool pointerPress(Vec3 origin, Vec3 dir);
    // Wheel zoom over an image shown in the card (not fullscreen). Returns true when the ray was over the picture.
    bool zoomAt(Vec3 origin, Vec3 dir, float factor);
    // Centre / facing / size of the whole card (title bar included) for re-centring the view on it.
    bool bounds(Vec3& centre, Vec3& normal, float& w, float& h) const;
    // Window behaviour (desktop): is the ray over the panel, and drag-by-details-card to reposition it.
    bool hits(Vec3 origin, Vec3 dir) const;
    bool dragStart(Vec3 origin, Vec3 dir);
    void dragTo(Vec3 origin, Vec3 dir);
    void dragEnd() { dragging_ = false; }
    bool dragging() const { return dragging_; }
    // Path of the file on show, and the callback that opens the previous (-1) / next (+1) file in the folder.
    const std::string& path() const { return req_.path; }
    std::function<void(int)> onStep;
    void step(int dir);
    bool wantsClose() const { return wantsClose_; }

    // Text mode (files opened with FileKind::Text).
    bool isText() const { return isText_; }
    bool isMedia() const { return isMedia_; }
    bool editing() const { return editing_; }
    void startEditing();
    void stopEditing();
    void editKey(EditKey key);
    void typeText(const std::string& utf8);
    void scrollText(float lines);
    bool saveText();

private:
    void redrawText(bool caretOn);
    bool prepareSwitch();
    void setStatus(const std::string& s, double seconds);
    void drawTextBar(Bitmap& bm);
    void drawTitle(int pxW);
    class LabelNode;
    void layout(const HeadPose& head);
    void drawBar();
    void buildDetails();
    void applyTransform(float e);

    Renderer& renderer_;
    std::unique_ptr<Node> rootHolder_;
    Node* root_;
    LabelNode* frame_ = nullptr;
    LabelNode* title_ = nullptr;
    std::shared_ptr<GpuLabel> titleLabel_;
    int titlePx_ = 256;
    static constexpr int kTitleBtn = 80;
    int titleButton(Vec3 origin, Vec3 dir) const;  // 1 fullscreen, 2 close, 0 none
    float frameW_ = 1, frameH_ = 1, uvOx_ = 0, uvOy_ = 0, uvS_ = 1;
    LabelNode* surface_ = nullptr;
    LabelNode* bar_ = nullptr;
    LabelNode* details_ = nullptr;

    MediaPlayer player_;
    OpenRequest req_;
    std::shared_ptr<GpuLabel> poster_, videoLabel_, imageLabel_, barLabel_, detailsLabel_;
    std::vector<uint8_t> frameBuf_;
    Bitmap barBitmap_;
    bool haveVideoFrame_ = false;

    bool active_ = false, closing_ = false, fullscreen_ = false, wantsClose_ = false;
    bool isMedia_ = false;     // video or audio (has a player)
    float t_ = 0;              // open animation 0..1
    float surfW_ = 2.8f, surfH_ = 1.6f, detW_ = 1.4f, barW_ = 2.8f, barH_ = 0.26f;
    Vec3 finalPos_, startPos_;
    float finalYaw_ = 0, startYaw_ = 0, startScale_ = 0.5f;
    float mediaAspect_ = 1.6f;
    double barTimer_ = 0, drawnPos_ = -1;
    bool drawnPlaying_ = false, drawnFs_ = false;
    float drawnVol_ = -1;
    HeadPose lastHead_;
    float zoom_ = 1.0f, panX_ = 0, panY_ = 0, idle_ = 0, barFade_ = 1.0f, boxW_ = 1, boxH_ = 1;
    Vec3 barBase_;
    bool switching_ = false;
    bool dragging_ = false;
    float grabDist_ = 2.0f;
    Vec3 grabOff_;

    CodeEditor editor_;
    bool isText_ = false, editing_ = false, discardArmed_ = false, drawnCaret_ = false;
    std::shared_ptr<GpuLabel> textLabel_;
    Bitmap textBitmap_;
    uint64_t drawnVersion_ = 0;
    bool drawnEditing_ = false;
    double caretTimer_ = 0, statusTimer_ = 0;
    std::string status_, drawnSig_;
};

}  // namespace sf
