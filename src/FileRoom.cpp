#include "FileRoom.h"

#include <chrono>

#include <algorithm>

#include "TextRender.h"

namespace sf {

namespace {
constexpr float kRingRadius = 7.0f;
constexpr int kCardsPerTier = 14;
constexpr size_t kMaxEntries = 1000;
constexpr size_t kMaxRooms = 400;
constexpr size_t kInfoScan = 300;  // files inspected per room for the hallway preview
constexpr int kMaxTiers = 16;
constexpr float kCardPitch = 1.7f;
constexpr float kRoomSlot = 12.0f;  // hallway length per pair of opposite doors

// Cube node used as a flat label-less block (floor).
class BlockNode : public Node {
public:
    BlockNode(Renderer& r, Vec3 color, bool grid, bool sky = false) : color_(color), grid_(grid), sky_(sky) {
        label_ = r.createLabel(rasterizeLines({""}, 1, 4, 4));
    }
    void render(Renderer& r) override {
        if (r.mirror()) return;  // floor and sky are not reflected
        if (sky_) r.drawSky(*label_, worldMatrix());
        else r.drawCube(*label_, worldMatrix(), color_, grid_);
    }

private:
    std::shared_ptr<GpuLabel> label_;
    Vec3 color_;
    bool grid_, sky_;
};

struct Box {
    Vec3 c, s, color;
    float glow;  // -1 plain, 0 edge-lit
    bool always;
};

// All the walls, doors and lights of a building interior in one node, culled by distance to the eye.
class ShellNode : public Node {
public:
    ShellNode(Renderer& r, const Vec3* eye) : eye_(eye) { label_ = r.createLabel(rasterizeLines({""}, 1, 4, 4)); }
    void render(Renderer& r) override {
        const float range = r.mirror() ? 30.0f : 220.0f;
        for (const Box& b : boxes) {
            if (b.glow < -1.5f) continue;
            if (!b.always && std::fabs(b.c.z - eye_->z) - b.s.z * 0.5f > range) continue;
            r.drawCube(*label_, Mat4::translate({b.c.x, b.c.y + yOffset, b.c.z}) * Mat4::scale(b.s), b.color, false, false, b.glow);
        }
        // Transparent glass panes (glow -2) go last, as two back-to-back quads so they read from both sides.
        for (const Box& b : boxes) {
            if (b.glow >= -1.5f) continue;
            const bool alongX = b.s.x > b.s.z;
            const float len = alongX ? b.s.x : b.s.z, yaw = alongX ? 0.0f : kPi * 0.5f;
            for (int side = 0; side < 2; ++side)
                r.drawGlass(Mat4::translate({b.c.x, b.c.y + yOffset, b.c.z}) * Mat4::rotateY(yaw + side * kPi) * Mat4::scale({len, b.s.y, 1.0f}), len / b.s.y, 0.01f, 0.28f);
        }
    }
    std::vector<Box> boxes;
    float yOffset = 0.0f;  // lets a whole shell (the lift car) ride up and down

private:
    std::shared_ptr<GpuLabel> label_;
    const Vec3* eye_;
};

// A small glass plaque of text lines hung on a wall. The bitmap is built the first time it is drawn.
class InfoNode : public Node {
public:
    InfoNode(Renderer& r, std::vector<std::string> lines, Vec3 tint) : r_(r), lines_(std::move(lines)), tint_(tint) {
        scale = {3.2f, 1.9f, 0.06f};
    }
    void render(Renderer& r) override {
        if (!label_) build();
        r.drawCube(*label_, worldMatrix(), tint_, false, false, 0.0f);
    }

private:
    void build() {
        Bitmap bm;
        bm.w = 640;
        bm.h = 380;
        bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
        float y = 24.0f;
        for (size_t i = 0; i < lines_.size(); ++i) {
            const float size = i == 0 ? 52.0f : 38.0f;
            y += textLineHeight(size, FontFace::UI) * (i == 0 ? 1.0f : 1.05f);
            drawText(bm, 32.0f, y - 10.0f, lines_[i], size, FontFace::UI, Color8{});
        }
        label_ = r_.createLabel(bm);
    }
    Renderer& r_;
    std::vector<std::string> lines_;
    Vec3 tint_;
    std::shared_ptr<GpuLabel> label_;
};

// A slowly turning hologram of a glTF model, used as lobby decor.
class ModelNode : public Node {
public:
    ModelNode(Renderer& r, std::string file, float height, Vec3 neon) : file_(std::move(file)), height_(height), neon_(neon) {
        label_ = r.createLabel(rasterizeLines({""}, 1, 4, 4));
    }
    void render(Renderer& r) override {
        if (r.mirror()) return;
        if (id_ == -2) id_ = r.model(file_);
        if (id_ < 0) return;
        const Vec3 sz = r.modelSize(id_);
        const float fit = std::min(1.0f, 2.4f / std::max(0.01f, std::max(sz.x, sz.z) * height_));
        const float hh = height_ * fit;
        r.drawModel(id_, *label_, Mat4::translate(position) * Mat4::rotateY(spin_) * Mat4::scale({hh, hh, hh}), neon_, spin_, true);
    }
    void update(double dt, double) override { spin_ += float(dt) * 0.6f; }

private:
    std::string file_;
    float height_, spin_ = 0.0f;
    Vec3 neon_;
    int id_ = -2;
    std::shared_ptr<GpuLabel> label_;
};

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "."; }
const Vec3 kPalette[5] = {{0.1f, 0.35f, 0.7f}, {0.7f, 0.2f, 0.55f}, {0.75f, 0.45f, 0.1f}, {0.15f, 0.65f, 0.4f}, {0.45f, 0.6f, 0.25f}};
}  // namespace

CardNode::CardNode(Renderer& renderer, const DirEntry& e, const std::string& text) : entry(e), r_(&renderer), text_(text) {
    name = text;
    scale = {1.4f, 0.9f, 0.06f};
    pickable = true;
}

// Text labels are made on first draw, so cards that are never in view cost no GPU memory.
void CardNode::ensureLabel() {
    if (label_) return;
    if (tall) {  // a room door: the title on top, a gap for the preview, then the room's details
        Bitmap bm;
        bm.w = 520;
        bm.h = 650;
        bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
        if (!isParentLink) drawDoorOutline(bm);  // the exit is framed by its own neon posts
        const std::vector<std::string> title = wrapText(text_, 50.0f, FontFace::UI, 400.0f, 2);
        float y = 70.0f;
        for (const std::string& l : title) {
            drawText(bm, (bm.w - measureText(l, 50.0f, FontFace::UI)) * 0.5f, y + textAscent(50.0f, FontFace::UI), l, 50.0f, FontFace::UI, Color8{});
            y += textLineHeight(50.0f, FontFace::UI);
        }
        float iy = 400.0f;
        for (const std::string& l : info_) {
            drawText(bm, 58.0f, iy + textAscent(32.0f, FontFace::UI), l, 32.0f, FontFace::UI, Color8{});
            iy += textLineHeight(32.0f, FontFace::UI) * 1.08f;
        }
        label_ = r_->createLabel(bm);
        return;
    }
    std::vector<std::string> lines = wrapText(text_, 56.0f, FontFace::UI, 460.0f, 3);
    Bitmap bm;
    bm.w = 520;
    bm.h = 332;
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    float lineH = textLineHeight(56.0f, FontFace::UI);
    float top = (bm.h - lineH * lines.size()) * 0.5f;
    for (size_t i = 0; i < lines.size(); ++i) {
        float w = measureText(lines[i], 56.0f, FontFace::UI);
        drawText(bm, (bm.w - w) * 0.5f, top + lineH * i + textAscent(56.0f, FontFace::UI), lines[i], 56.0f,
                 FontFace::UI, Color8{});
    }
    label_ = r_->createLabel(bm);
}

void CardNode::render(Renderer& renderer) {
    Mat4 w = worldMatrix();
    if (hover) w = w * Mat4::scale({1.08f, 1.08f, 1.0f});
    if (thumb_) {
        renderer.drawCube(*thumb_, w, hover ? Vec3{1.25f, 1.25f, 1.25f} : Vec3{1.0f, 1.0f, 1.0f}, false, true,
                          hover ? 1.0f : 0.0f);
        return;
    }
    ensureLabel();
    Vec3 base = tall ? Vec3{0.02f, 0.035f, 0.06f} : isParentLink ? Vec3{0.28f, 0.24f, 0.5f} : (entry.isDir ? Vec3{0.1f, 0.28f, 0.52f} : Vec3{0.14f, 0.17f, 0.23f});
    if (tall && isParentLink) base = {0.55f, 0.95f, 1.0f};  // the exit glows
    Vec3 color = hover ? base * 1.4f : base;
    renderer.drawCube(*label_, w, color, false, false, tall ? -1.0f : (hover ? 1.0f : 0.0f));  // doors draw their own outline
}

FileRoom::FileRoom(Renderer& renderer) : renderer_(renderer) {
    rootHolder_ = std::make_unique<Node>();
    root_ = rootHolder_.get();
}

void FileRoom::setHover(Node* node) {
    if (hovered_ == node) return;
    if (hovered_) hovered_->hover = false;
    hovered_ = (node && node->isCard()) ? static_cast<CardNode*>(node) : nullptr;
    if (hovered_) hovered_->hover = true;
}

void FileRoom::addSky() {
    auto sky = std::make_unique<BlockNode>(renderer_, Vec3{0, 0, 0}, false, true);
    sky->position = {0, 0, 0};
    sky->scale = {2800, 2800, 2800};
    root_->add(std::move(sky));
}

void FileRoom::resetScene() {
    hovered_ = nullptr;
    doorCards_.clear();
    liftCar_ = nullptr;
    liftBtns_.clear();
    cards_.clear();
    requested_.clear();
    header_ = nullptr;
    generation_ = loader_.reset();
    root_->clearChildren();
    addSky();
    auto floor = std::make_unique<BlockNode>(renderer_, Vec3{0.05f, 0.07f, 0.12f}, true);
    floor->position = {0, -0.05f, 0};
    floor->scale = {6000, 0.1f, 6000};
    root_->add(std::move(floor));
}

CardNode* FileRoom::addCard(std::unique_ptr<CardNode> card) {
    CardNode* raw = card.get();
    cards_.push_back(raw);
    requested_.push_back(0);
    root_->add(std::move(card));
    return raw;
}

void FileRoom::loadEmpty() {
    building_ = false;
    inRoom_ = false;
    path_.clear();
    loaded_ = false;
    resetScene();
}

void FileRoom::load(std::string path) {
    building_ = false;
    inRoom_ = false;
    loaded_ = true;
    path_ = path;
    resetScene();

    auto header = std::make_unique<PanelNode>(renderer_, "File room  " + (path.empty() ? std::string("This PC") : clip(path, 22)), Vec3{0.45f, 0.6f, 1.0f}, 5.0f, 0.5f, 0);
    header->position = {0, 2.9f, -4.0f};
    header_ = static_cast<PanelNode*>(root_->add(std::move(header)));

    std::vector<DirEntry> entries;
    DirEntry up;
    up.name = "..";
    up.path = parentOrDrives(path);
    up.isDir = true;
    entries.push_back(up);
    for (auto& e : (path.empty() ? listDrives() : listDirectory(path, kMaxEntries))) entries.push_back(std::move(e));

    if (!path.empty()) {
        parentEntry_ = up;
        rooms_.clear();
        looseItems_.clear();
        std::vector<DirEntry> dirs;
        for (size_t i = 1; i < entries.size(); ++i) (entries[i].isDir ? dirs : looseItems_).push_back(entries[i]);
        if (!looseItems_.empty() || dirs.empty()) {
            RoomInfo loose;
            loose.name = "(FILES)";
            loose.path = path;
            loose.loose = true;
            rooms_.push_back(std::move(loose));
        }
        for (size_t i = 0; i < dirs.size() && rooms_.size() < kMaxRooms; ++i) {
            RoomInfo r;
            r.name = dirs[i].name;
            r.path = dirs[i].path;
            rooms_.push_back(std::move(r));
        }
        buildCorridor();
        return;
    }

    // Regular grid wrapped around a cylinder: C columns around, up to kMaxTiers rows, no row stagger. The radius grows
    // with the column count so the card pitch stays constant however many files there are.
    const int total = static_cast<int>(entries.size());
    const int columns = std::max(kCardsPerTier, (total + kMaxTiers - 1) / kMaxTiers);
    const float radius = std::max(kRingRadius, columns * kCardPitch / (2.0f * kPi));
    extent_ = radius + 30.0f;
    for (size_t i = 0; i < entries.size(); ++i) {
        int tier = static_cast<int>(i) / columns;
        int slot = static_cast<int>(i) % columns;
        float angle = (static_cast<float>(slot) / columns) * 2.0f * kPi;
        auto card = std::make_unique<CardNode>(renderer_, entries[i], entries[i].name);
        card->isParentLink = (i == 0);
        card->position = {radius * std::sin(angle), 0.8f + tier * 1.05f, radius * std::cos(angle)};
        card->yaw = angle + kPi;  // +Z face points at the room centre
        addCard(std::move(card));
        if (i > 0) {
            FileKind kind = classifyFile(entries[i].path, entries[i].isDir);
            if (kind != FileKind::Folder && kind != FileKind::Other) {
                requested_[i] = 1;
                loader_.request(generation_, static_cast<int>(i), entries[i].path, entries[i].name, kind);
            }
        }
    }
}

void FileRoom::refresh() {
    if (!loaded_) return;
    if (inRoom_) enterRoom(roomIdx_, false);
    else load(path_);
}

// The lobby of a building: an atrium whose walls are lined with doors (subfolders, plus "(FILES)" for loose files),
// at most kMaxPerFloor to a floor. When there are more rooms, more floors are stacked, each with a glass-railed walkway
// and a glass lift on the far wall. The open middle holds a fountain. No file objects live here; each door shows its
// room's details and a preview thumbnail.
namespace {
constexpr float kPitch = 4.6f, kFrontZ = 5.5f, kStrip = 3.0f, kLevelH = 5.4f;
constexpr float kShaftHalf = 2.6f, kShaftDepth = 2.3f, kFountainHalf = 3.3f;
constexpr int kMaxPerFloor = 40;
float fountainZ(float d) { return (kFrontZ - d) * 0.5f; }
}  // namespace

void FileRoom::buildCorridor() {
    building_ = true;
    inRoom_ = false;
    prevPos_ = {0, 1.6f, 2.0f};
    walk_.clear();
    cards_.clear();
    doorCards_.clear();
    requested_.clear();
    generation_ = loader_.reset();

    const int R = static_cast<int>(rooms_.size());
    levels_ = std::max(1, (R + kMaxPerFloor - 1) / kMaxPerFloor);
    const int L = levels_, per = std::max(1, (R + L - 1) / L);
    const bool multi = L > 1;
    auto floorSlots = [&](float hw, float d) {
        std::vector<LobbySlot> v;
        for (float x = -hw + kStrip + kPitch * 0.5f; x <= hw - kStrip - kPitch * 0.5f + 0.01f; x += kPitch) {
            if (std::fabs(x) < 4.0f) continue;  // the grand exit stands here
            v.push_back({x, kFrontZ, kPi, 0});
        }
        for (float z = -d + kStrip + kPitch * 0.5f; z <= kFrontZ - kStrip - kPitch * 0.5f + 0.01f; z += kPitch) {
            v.push_back({-hw, z, kPi * 0.5f, 0});
            v.push_back({hw, z, -kPi * 0.5f, 0});
        }
        for (float x = -hw + kStrip + kPitch * 0.5f; x <= hw - kStrip - kPitch * 0.5f + 0.01f; x += kPitch) {  // the back wall fills last
            if (multi && std::fabs(x) < kShaftHalf + 1.4f) continue;  // the lift shaft stands against the far wall
            v.push_back({x, -d, 0.0f, 0});
        }
        return v;
    };
    float hw = 10.0f, d = 16.0f;
    while (static_cast<int>(floorSlots(hw, d).size()) < per && hw < 120.0f) { hw += 2.0f; d += 3.4f; }
    const std::vector<LobbySlot> tmpl = floorSlots(hw, d);
    slots_.clear();
    for (int r = 0; r < R; ++r) {  // floors fill one after another, so neighbouring rooms stay together
        LobbySlot s = tmpl[std::min<size_t>(r % per, tmpl.size() - 1)];
        s.level = r / per;
        slots_.push_back(s);
    }
    hallHW_ = hw;
    hallD_ = d;

    auto shell = std::make_unique<ShellNode>(renderer_, &eye_);
    auto& B = shell->boxes;
    // Each building gets one of twelve looks, picked from its path.
    struct Look { Vec3 wall, carpet, rim; };
    static const Look looks[12] = {
        {{0.014f, 0.022f, 0.04f}, {0.036f, 0.038f, 0.043f}, {0.3f, 0.8f, 1.2f}},
        {{0.03f, 0.014f, 0.034f}, {0.045f, 0.03f, 0.05f}, {1.1f, 0.3f, 0.95f}},
        {{0.03f, 0.02f, 0.012f}, {0.05f, 0.04f, 0.03f}, {1.2f, 0.75f, 0.2f}},
        {{0.012f, 0.03f, 0.02f}, {0.03f, 0.05f, 0.04f}, {0.25f, 1.1f, 0.6f}},
        {{0.03f, 0.012f, 0.012f}, {0.05f, 0.03f, 0.03f}, {1.2f, 0.3f, 0.25f}},
        {{0.02f, 0.02f, 0.04f}, {0.03f, 0.03f, 0.06f}, {0.5f, 0.5f, 1.3f}},
        {{0.012f, 0.026f, 0.03f}, {0.02f, 0.045f, 0.05f}, {0.2f, 1.1f, 1.1f}},
        {{0.026f, 0.026f, 0.03f}, {0.05f, 0.05f, 0.055f}, {1.0f, 1.0f, 1.1f}},
        {{0.03f, 0.024f, 0.01f}, {0.045f, 0.045f, 0.02f}, {1.0f, 1.0f, 0.3f}},
        {{0.024f, 0.012f, 0.03f}, {0.04f, 0.025f, 0.05f}, {0.7f, 0.35f, 1.3f}},
        {{0.01f, 0.026f, 0.04f}, {0.02f, 0.04f, 0.06f}, {0.2f, 0.65f, 1.4f}},
        {{0.03f, 0.016f, 0.02f}, {0.05f, 0.03f, 0.035f}, {1.2f, 0.4f, 0.6f}}};
    const size_t pathHash = std::hash<std::string>{}(path_);
    const int lookIdx = static_cast<int>(pathHash % 12);
    const Look& lk = looks[lookIdx];
    const Vec3 wall = lk.wall, light{1.6f, 1.8f, 2.0f}, carpet = lk.carpet, rim = lk.rim;
    const Vec3 stone{0.1f, 0.11f, 0.13f}, water{0.08f, 0.42f, 0.7f}, glass{0.5f, 0.8f, 1.0f};
    auto add = [&](Vec3 c, Vec3 s, Vec3 col, float glow = 0.0f, bool always = false) { B.push_back({c, s, col, glow, always}); };
    const float zm = (kFrontZ - d) * 0.5f, zl = d + kFrontZ, H = multi ? L * kLevelH + 1.2f : 7.5f, fz = fountainZ(d);

    auto exitCard = std::make_unique<CardNode>(renderer_, parentEntry_, "EXIT");
    exitCard->isParentLink = true;
    exitCard->tall = true;  // the portrait door body fills the whole doorway
    exitCard->position = {0, 2.55f, kFrontZ - 0.24f};
    exitCard->yaw = kPi;
    exitCard->scale = {3.9f, 5.0f, 0.06f};
    addCard(std::move(exitCard));
    {
        const Vec3 neon{0.85f, 0.92f, 1.0f};
        for (float sgn : {-1.0f, 1.0f}) add({sgn * 2.3f, 2.6f, kFrontZ - 0.12f}, {0.3f, 5.2f, 0.2f}, neon * 2.0f, -1.0f, true);
        add({0, 5.2f, kFrontZ - 0.12f}, {4.9f, 0.3f, 0.2f}, neon * 2.0f, -1.0f, true);
        add({0, 0.05f, kFrontZ - 1.4f}, {4.4f, 0.02f, 2.6f}, neon * 0.2f, -1.0f, true);
    }

    // Shell: walls, ceiling, carpet and a rug under the fountain.
    add({-hw - 0.2f, H * 0.5f, zm}, {0.4f, H, zl}, wall, 0.0f, true);
    add({hw + 0.2f, H * 0.5f, zm}, {0.4f, H, zl}, wall, 0.0f, true);
    add({0, H + 0.1f, zm}, {hw * 2 + 0.8f, 0.2f, zl}, wall * 0.7f, -1.0f, true);
    add({0, H * 0.5f, kFrontZ + 0.2f}, {hw * 2 + 0.8f, H, 0.4f}, wall, 0.0f, true);
    if (multi) {  // the far wall opens onto the recessed lift shaft
        const float wl = hw + 0.4f - 1.6f;
        for (float sgn : {-1.0f, 1.0f}) add({sgn * (1.6f + wl * 0.5f), H * 0.5f, -d - 0.2f}, {wl, H, 0.4f}, wall, 0.0f, true);
    } else {
        add({0, H * 0.5f, -d - 0.2f}, {hw * 2 + 0.8f, H, 0.4f}, wall, 0.0f, true);
    }
    add({0, 0.0f, zm}, {hw * 2, 0.04f, zl}, carpet, -1.0f, true);
    const float rug = std::min(16.0f, hw * 2 - 8.0f);
    add({0, 0.035f, fz}, {rug, 0.02f, rug}, {0.075f, 0.07f, 0.085f}, -1.0f, true);
    add({0, 0.04f, fz}, {rug - 1.2f, 0.02f, rug - 1.2f}, carpet * 1.15f, -1.0f, true);
    add({0, 0.035f, (fz + kFrontZ) * 0.5f + 0.5f}, {2.4f, 0.02f, kFrontZ - fz}, {0.075f, 0.07f, 0.085f}, -1.0f, true);

    if (multi) {
        const float ix = hw - kStrip, z0 = -d + kStrip, z1 = kFrontZ - kStrip;
        for (int l = 1; l < L; ++l) {
            // A walkway ringing the hall (broken by the lift shaft) with glass railings that butt together at the corners.
            const float yt = l * kLevelH, ys = yt - 0.15f;
            add({-hw + kStrip * 0.5f, ys, zm}, {kStrip, 0.3f, zl}, carpet * 0.8f, -1.0f, true);
            add({hw - kStrip * 0.5f, ys, zm}, {kStrip, 0.3f, zl}, carpet * 0.8f, -1.0f, true);
            add({0, ys, kFrontZ - kStrip * 0.5f}, {ix * 2, 0.3f, kStrip}, carpet * 0.8f, -1.0f, true);
            add({0, ys, -d + kStrip * 0.5f}, {ix * 2, 0.3f, kStrip}, carpet * 0.8f, -1.0f, true);
            auto rail = [&](float cx, float cz, float sx, float sz) {
                add({cx, yt + 0.58f, cz}, {sx, 1.1f, sz}, glass, -2.0f, true);
                add({cx, yt + 1.15f, cz}, {std::max(sx, 0.06f), 0.06f, std::max(sz, 0.06f)}, rim, -1.0f, true);
            };
            rail(-ix, (z0 + z1) * 0.5f, 0.0f, z1 - z0);
            rail(ix, (z0 + z1) * 0.5f, 0.0f, z1 - z0);
            rail(0, z1, ix * 2, 0.0f);
            rail(0, z0, ix * 2, 0.0f);
            for (float cx : {-ix, ix})
                for (float cz : {z0, z1}) add({cx, yt + 0.6f, cz}, {0.14f, 1.2f, 0.14f}, rim, -1.0f, true);
        }

        // Lift shaft recessed behind the far wall so its glass doors sit flush with it and the walkways run straight past.
        const float oh = 1.6f, sb = -d - kShaftDepth, sc = -d - kShaftDepth * 0.5f;
        for (float sgn : {-1.0f, 1.0f}) add({sgn * (oh + 0.1f), H * 0.5f, sc}, {0.2f, H, kShaftDepth}, wall * 2.2f, 0.0f, true);
        add({0, H * 0.5f, sb - 0.1f}, {oh * 2 + 0.4f, H, 0.2f}, wall * 2.2f, 0.0f, true);
        add({0, H + 0.1f, sc}, {oh * 2 + 0.4f, 0.2f, kShaftDepth + 0.2f}, wall * 0.7f, -1.0f, true);
        add({0, 0.0f, sc}, {oh * 2, 0.04f, kShaftDepth}, carpet, -1.0f, true);
        add({0, L * kLevelH * 0.5f, -d + 0.03f}, {oh * 2, L * kLevelH, 0.0f}, glass, -2.0f, true);
        for (float sgn : {-1.0f, 1.0f}) add({sgn * oh, H * 0.5f, -d + 0.05f}, {0.06f, H, 0.06f}, rim * 0.9f, -1.0f, true);
        for (int l = 0; l < L; ++l) {
            add({0, l * kLevelH + 3.4f, -d + 0.05f}, {oh * 2, 0.06f, 0.06f}, rim * 1.5f, -1.0f, true);
            add({0, l * kLevelH + 0.03f, -d + 0.6f}, {oh * 2, 0.03f, 1.2f}, rim * 0.18f, -1.0f, true);
        }

        // The car: a glass cabin on a ShellNode of its own so it can ride the shaft.
        auto car = std::make_unique<ShellNode>(renderer_, &eye_);
        auto cadd = [&](Vec3 c, Vec3 s, Vec3 col, float glow) { car->boxes.push_back({c, s, col, glow, true}); };
        const float cd = 1.9f, cz = -d - 0.15f - cd * 0.5f, cf = -d - 0.15f;
        cadd({0, 0.08f, cz}, {2.9f, 0.14f, cd}, wall * 3.0f, 0.0f);
        cadd({0, 3.12f, cz}, {2.9f, 0.14f, cd}, wall * 3.0f, 0.0f);
        cadd({0, 3.04f, cz}, {2.4f, 0.04f, 1.4f}, light, -1.0f);
        cadd({0, 1.55f, cf}, {2.9f, 3.0f, 0.0f}, glass, -2.0f);
        cadd({0, 1.55f, cz - cd * 0.5f}, {2.9f, 3.0f, 0.06f}, wall * 2.5f, 0.0f);
        for (float sgn : {-1.0f, 1.0f}) cadd({sgn * 1.45f, 1.55f, cz}, {0.0f, 3.0f, cd}, glass, -2.0f);
        cadd({0, 1.1f, cz - cd * 0.5f + 0.05f}, {2.6f, 0.05f, 0.05f}, rim, -1.0f);
        cadd({0, 0.16f, cf}, {2.9f, 0.04f, 0.06f}, rim, -1.0f);
        cadd({0, 3.0f, cf}, {2.9f, 0.04f, 0.06f}, rim, -1.0f);
        liftCar_ = root_->add(std::move(car));
        liftTarget_ = 0;
        liftY_ = 0.0f;
    }

    if (L >= 3) {
        // Fountain: stone basin, glowing water, a pedestal and a bowl with a jet.
        const float fo = kFountainHalf * 2;
        add({0, 0.35f, fz - kFountainHalf + 0.25f}, {fo, 0.7f, 0.5f}, stone, 0.0f, true);
        add({0, 0.35f, fz + kFountainHalf - 0.25f}, {fo, 0.7f, 0.5f}, stone, 0.0f, true);
        add({-kFountainHalf + 0.25f, 0.35f, fz}, {0.5f, 0.7f, fo - 1.0f}, stone, 0.0f, true);
        add({kFountainHalf - 0.25f, 0.35f, fz}, {0.5f, 0.7f, fo - 1.0f}, stone, 0.0f, true);
        add({0, 0.52f, fz}, {fo - 1.0f, 0.04f, fo - 1.0f}, water, -1.0f, true);
        add({0, 0.9f, fz}, {1.2f, 1.8f, 1.2f}, stone, 0.0f, true);
        add({0, 1.8f, fz}, {2.8f, 0.25f, 2.8f}, stone, 0.0f, true);
        add({0, 1.95f, fz}, {2.4f, 0.04f, 2.4f}, water, -1.0f, true);
        add({0, 2.7f, fz}, {0.25f, 1.5f, 0.25f}, rim, -1.0f, true);
        add({0, 3.55f, fz}, {0.5f, 0.3f, 0.5f}, rim * 0.8f, -1.0f, true);
    }

    // Decor: ceiling ribs, wall light bands and floor lanes, arranged differently per look.
    {
        const int pat = lookIdx % 4;
        const float ribStep = pat == 0 ? 6.0f : pat == 1 ? 4.0f : pat == 2 ? 8.0f : 5.0f;
        for (float z = kFrontZ - 3.0f; z > -d + 1.0f; z -= ribStep) {
            add({0, H - 0.18f, z}, {hw * 2, 0.16f, 0.5f}, wall * 2.5f, 0.0f, true);
            add({0, H - 0.28f, z}, {hw * 2 - 1.0f, 0.04f, 0.12f}, rim * 0.9f, -1.0f, true);
        }
        const float by = pat == 3 ? 1.1f : 3.4f;
        for (float sgn : {-1.0f, 1.0f}) {
            add({sgn * (hw - 0.04f), by, zm}, {0.06f, 0.1f, zl - 1.0f}, rim, -1.0f, true);
            if (pat >= 2) add({sgn * (hw - 0.04f), by + 0.7f, zm}, {0.06f, 0.05f, zl - 1.0f}, rim * 0.5f, -1.0f, true);
        }
        add({0, 0.05f, zm}, {0.08f, 0.01f, zl - 1.0f}, rim * 0.8f, -1.0f, true);
        if (pat == 1 || pat == 2)
            for (float sgn : {-1.0f, 1.0f}) add({sgn * (hw - kStrip), 0.05f, zm}, {0.08f, 0.01f, zl - 1.0f}, rim * 0.5f, -1.0f, true);
        if (pat == 0 || pat == 3)
            for (float z = kFrontZ - 4.0f; z > -d + 2.0f; z -= 4.0f) add({0, 0.05f, z}, {hw * 1.2f, 0.01f, 0.06f}, rim * 0.35f, -1.0f, true);
    }

    // The exit: a layered frame, chevrons leading to it, a header bar and glowing steps.
    {
        const Vec3 neon{0.85f, 0.92f, 1.0f};
        add({0, 5.6f, kFrontZ - 0.12f}, {5.6f, 0.12f, 0.3f}, rim * 1.6f, -1.0f, true);
        for (float sgn : {-1.0f, 1.0f}) {
            add({sgn * 2.8f, 2.8f, kFrontZ - 0.12f}, {0.12f, 5.6f, 0.3f}, rim * 1.6f, -1.0f, true);
            add({sgn * 3.4f, 2.0f, kFrontZ - 0.3f}, {0.5f, 4.0f, 0.5f}, wall * 3.0f, 0.0f, true);
            add({sgn * 3.4f, 4.1f, kFrontZ - 0.3f}, {0.3f, 0.12f, 0.3f}, neon * 2.0f, -1.0f, true);
        }
        for (int i = 0; i < 4; ++i) {
            const float cz = kFrontZ - 1.6f - i * 1.1f, w = 3.0f - i * 0.3f;
            add({0, 0.06f, cz}, {w, 0.02f, 0.14f}, neon * (1.6f - i * 0.3f), -1.0f, true);
        }
        add({0, 0.03f, kFrontZ - 0.5f}, {4.6f, 0.02f, 0.3f}, rim * 1.4f, -1.0f, true);
    }

    // Layout variants: some looks add colonnades or a central plinth so lobbies differ in shape, not just colour.
    {
        const int lay = (lookIdx / 4) % 3;
        if (lay == 1 && L < 3) {
            for (float z = kFrontZ - 8.0f; z > -d + 4.0f; z -= 7.0f)
                for (float sgn : {-1.0f, 1.0f}) {
                    const float cx = sgn * (hw * 0.5f);
                    add({cx, H * 0.5f, z}, {0.7f, H, 0.7f}, wall * 3.0f, 0.0f, true);
                    add({cx, 1.0f, z}, {0.8f, 0.08f, 0.8f}, rim * 1.5f, -1.0f, true);
                    add({cx, H - 1.0f, z}, {0.8f, 0.08f, 0.8f}, rim * 1.5f, -1.0f, true);
                }
        } else if (lay == 2 && L < 3) {
            add({0, 0.2f, fz}, {6.0f, 0.4f, 6.0f}, wall * 3.0f, 0.0f, true);
            add({0, 0.42f, fz}, {5.6f, 0.04f, 5.6f}, rim, -1.0f, true);
            add({0, H * 0.5f, fz}, {0.12f, H, 0.12f}, rim * 1.3f, -1.0f, true);
        }
    }
    // A skylight over the fountain.
    add({0, H - 0.04f, fz}, {9.0f, 0.06f, 9.0f}, light * 0.9f, -1.0f, true);

    // Doors, one per room, on every floor.
    for (int r = 0; r < R; ++r) {
        const LobbySlot& sl = slots_[r];
        const Vec3 kc{0.85f, 0.92f, 1.0f};
        const float y0 = sl.level * kLevelH, nx = std::sin(sl.yaw), nz = std::cos(sl.yaw), tx = std::cos(sl.yaw), tz = -std::sin(sl.yaw);
        auto at = [&](float n, float tt, float y) { return Vec3{sl.x + nx * n + tx * tt, y0 + y, sl.z + nz * n + tz * tt}; };
        auto sz = [&](float along, float h, float thick) { return std::fabs(nx) > 0.5f ? Vec3{thick, h, along} : Vec3{along, h, thick}; };
        add(at(1.3f, 0, 0.05f), sz(2.8f, 0.03f, 2.0f), kc * 0.25f, -1.0f);

        auto door = std::make_unique<CardNode>(renderer_, DirEntry{rooms_[r].name, rooms_[r].path, true}, clip(rooms_[r].name, 40));
        door->roomIndex = r;
        door->tall = true;
        door->position = at(0.04f, 0, 1.55f);
        door->yaw = sl.yaw;
        door->scale = {2.4f, 3.0f, 0.06f};
        doorCards_.push_back(addCard(std::move(door)));
        if (rooms_[r].ready) addRoomExtras(r);
    }

    if (multi) {  // a single call button on the wall beside the shaft, one per floor
        for (int l = 0; l < L; ++l) {
            auto btn = std::make_unique<CardNode>(renderer_, DirEntry{}, "CALL");
            btn->roomIndex = -100 - l;
            btn->position = {2.2f, l * kLevelH + 1.4f, -d + 0.05f};
            btn->scale = {0.5f, 0.28f, 0.05f};
            addCard(std::move(btn));
        }
    }
    liftBtns_.clear();
    if (multi) {
        const int rowsN = (L + 1) / 2;
        const float sp = std::min(0.42f, 1.9f / rowsN);
        const float cz = -d - 0.15f - 0.95f;
        for (int j = 0; j < L; ++j) {  // in-car panel on the right wall; it rides with the car
            auto btn = std::make_unique<CardNode>(renderer_, DirEntry{}, j == 0 ? "G" : std::to_string(j));
            btn->roomIndex = -10 - j;
            const float off = 2.4f - (j / 2) * sp;
            btn->position = {1.4f, off, cz + 0.3f - (j % 2) * 0.4f};
            btn->yaw = -kPi * 0.5f;
            btn->scale = {0.3f, sp * 0.66f, 0.05f};
            liftBtns_.push_back({addCard(std::move(btn)), off});
        }
    }

    rows_ = static_cast<int>(d / 6.0f);
    extent_ = std::max(hw * 2.0f, d) + 20.0f;
    root_->add(std::move(shell));

    if (!header_) {
        auto header = std::make_unique<PanelNode>(renderer_, "File room  " + clip(path_, 22), Vec3{0.45f, 0.6f, 1.0f}, 5.0f, 0.5f, 0);
        header_ = static_cast<PanelNode*>(root_->add(std::move(header)));
    }
    header_->position = {0, 4.3f, kFrontZ - 0.25f};
    header_->yaw = kPi;
}

float FileRoom::roomDoorZ(int r) const { return slots_[r].z; }
float FileRoom::roomDoorX(int r) const { return slots_[r].x; }

// Scans one room's folder (capped) for the hallway plaque: counts by type and a few sample names.
void FileRoom::fillRoomInfo(int r) {
    RoomInfo& ri = rooms_[r];
    ri.ready = true;
    const std::vector<DirEntry> scanned = ri.loose ? std::vector<DirEntry>() : listDirectory(ri.path, kInfoScan);
    const std::vector<DirEntry>& items = ri.loose ? looseItems_ : scanned;
    ri.capped = !ri.loose && items.size() >= kInfoScan;
    bool havePreview = false;
    for (const DirEntry& e : items) {
        if (e.isDir) { ++ri.dirs; continue; }
        ++ri.files;
        switch (classifyFile(e.path, false)) {
            case FileKind::Image: ++ri.imgs; if (!havePreview || !ri.previewIsImage) { ri.preview = e; havePreview = true; ri.previewIsImage = true; } break;
            case FileKind::Video: ++ri.vids; if (!havePreview) { ri.preview = e; havePreview = true; } break;
            case FileKind::Audio: ++ri.auds; break;
            case FileKind::Text: ++ri.txts; break;
            default: ++ri.others; break;
        }
        if (ri.sample.size() < 3) ri.sample.push_back(e.name);
    }
    ri.hasPreview = havePreview;
    addRoomExtras(r);
}

// Puts a room's details on its door (under the title) and hangs the preview thumbnail on the door face.
void FileRoom::addRoomExtras(int r) {
    const RoomInfo& ri = rooms_[r];
    const LobbySlot& sl = slots_[r];
    const float y0 = sl.level * kLevelH, nx = std::sin(sl.yaw), nz = std::cos(sl.yaw), tx = std::cos(sl.yaw), tz = -std::sin(sl.yaw);
    auto at = [&](float n, float tt, float y) { return Vec3{sl.x + nx * n + tx * tt, y0 + y, sl.z + nz * n + tz * tt}; };
    const std::string plus = ri.capped ? "+" : "";
    std::vector<std::string> lines;
    lines.push_back(std::to_string(ri.files) + plus + " files" + (ri.dirs ? "  " + std::to_string(ri.dirs) + plus + " folders" : ""));
    std::string kinds;
    auto kind = [&](int n, const char* label) { if (n) kinds += (kinds.empty() ? "" : "  ") + std::to_string(n) + " " + label; };
    kind(ri.imgs, "images");
    kind(ri.vids, "videos");
    kind(ri.auds, "audio");
    kind(ri.txts, "text");
    if (!kinds.empty()) lines.push_back(kinds);
    for (const std::string& n : ri.sample) lines.push_back(clip(n, 26));
    if (r < static_cast<int>(doorCards_.size())) doorCards_[r]->setInfo(lines);

    if (ri.hasPreview) {
        auto prev = std::make_unique<CardNode>(renderer_, ri.preview, ri.preview.name);
        prev->roomIndex = r;
        prev->position = at(0.11f, 0, 1.85f);
        prev->yaw = sl.yaw;
        prev->scale = {1.6f, 1.0f, 0.05f};
        CardNode* c = addCard(std::move(prev));
        requested_.back() = 1;
        loader_.request(generation_, static_cast<int>(cards_.size()) - 1, c->entry.path, c->entry.name,
                        classifyFile(c->entry.path, false));
    }
}

// A room is a square gallery sized by its file count, with the files hung as frames on all four walls, tier on tier.
// It is built only when entered, so the hallway never holds file objects.
void FileRoom::enterRoom(int idx, bool setSpawn) {
    if (idx < 0 || idx >= static_cast<int>(rooms_.size())) return;
    const RoomInfo ri = rooms_[idx];
    std::vector<DirEntry> items = ri.loose ? looseItems_ : listDirectory(ri.path, kMaxEntries);
    if (setSpawn && items.size() == 1 && items[0].isDir) {  // a room holding just one folder: go straight through
        const std::string target = items[0].path;
        load(target);
        return;
    }
    roomIdx_ = idx;
    inRoom_ = true;
    building_ = true;
    walk_.clear();
    resetScene();

    const int n = static_cast<int>(items.size());
    const int tiers = std::clamp((n + 24) / 25, 2, 5);
    const float pitch = 2.4f, tierH = 1.5f;
    struct Slot { Vec3 p; float yaw; };
    auto makeSlots = [&](float S) {
        std::vector<Slot> out;
        const int along = std::max(0, static_cast<int>((S - 1.0f) / pitch));
        std::vector<float> ts;
        for (int j = 0; j < along; ++j) ts.push_back((j - (along - 1) * 0.5f) * pitch);
        const float h = S * 0.5f;
        for (float t : ts) out.push_back({{t, 0, -h + 0.03f}, 0.0f});          // far wall, left to right
        for (float t : ts) out.push_back({{h - 0.03f, 0, t}, -kPi * 0.5f});    // right wall, far to near
        for (auto it = ts.rbegin(); it != ts.rend(); ++it)
            if (std::fabs(*it) >= 3.0f) out.push_back({{*it, 0, h - 0.03f}, kPi});  // near wall, right to left, past the door
        for (auto it = ts.rbegin(); it != ts.rend(); ++it) out.push_back({{-h + 0.03f, 0, *it}, kPi * 0.5f});  // left wall
        return out;
    };
    float S = 10.0f;
    while (S < 120.0f && static_cast<int>(makeSlots(S).size()) * tiers < n) S += 1.0f;
    const std::vector<Slot> slots = makeSlots(S);
    const float H = 1.7f + tiers * tierH, h = S * 0.5f;
    const Vec3 kc = kPalette[idx % 5];
    const Vec3 wall{0.014f, 0.022f, 0.04f}, light{1.6f, 1.8f, 2.0f};

    auto shell = std::make_unique<ShellNode>(renderer_, &eye_);
    auto& B = shell->boxes;
    auto add = [&](Vec3 c, Vec3 s, Vec3 col, float glow = 0.0f) { B.push_back({c, s, col, glow, true}); };
    add({0, H * 0.5f, -h - 0.2f}, {S + 0.8f, H, 0.4f}, wall);
    add({0, H * 0.5f, h + 0.2f}, {S + 0.8f, H, 0.4f}, wall);
    add({-h - 0.2f, H * 0.5f, 0}, {0.4f, H, S + 0.8f}, wall);
    add({h + 0.2f, H * 0.5f, 0}, {0.4f, H, S + 0.8f}, wall);
    add({0, H + 0.1f, 0}, {S + 0.8f, 0.2f, S + 0.8f}, wall * 0.7f, -1.0f);
    add({0, 0.03f, 0}, {S - 0.2f, 0.04f, S - 0.2f}, Vec3{0.036f, 0.038f, 0.043f} + kc * 0.004f, -1.0f);
    for (int e : {-1, 1}) add({e * 1.5f, 1.55f, h - 0.1f}, {0.1f, 3.2f, 0.1f}, kc * 2.0f, -1.0f);
    add({0, 3.2f, h - 0.1f}, {3.1f, 0.1f, 0.1f}, kc * 2.0f, -1.0f);
    root_->add(std::move(shell));

    DirEntry back;
    back.name = "Lobby";
    back.path = ri.path;
    back.isDir = true;
    auto door = std::make_unique<CardNode>(renderer_, back, "Back to lobby");
    door->isParentLink = true;
    door->roomIndex = -2;
    door->position = {0, 1.55f, h - 0.04f};
    door->yaw = kPi;
    door->scale = {2.8f, 3.0f, 0.06f};
    addCard(std::move(door));

    for (int k = 0; k < n && !slots.empty(); ++k) {
        const int tier = k / static_cast<int>(slots.size());
        if (tier >= tiers) break;
        const Slot& sl = slots[k % slots.size()];
        auto card = std::make_unique<CardNode>(renderer_, items[k], items[k].name);
        card->position = {sl.p.x, 1.35f + tier * tierH, sl.p.z};
        card->yaw = sl.yaw;
        card->scale = {2.0f, 1.28f, 0.06f};
        addCard(std::move(card));
    }

    auto header = std::make_unique<PanelNode>(renderer_, clip(ri.name, 22) + "  " + std::to_string(n), kc * 1.3f, 5.0f, 0.5f, 0);
    header->position = {0, H - 0.6f, -h + 0.25f};
    header_ = static_cast<PanelNode*>(root_->add(std::move(header)));

    walk_.push_back({-h + 0.5f, h - 0.5f, -h + 0.5f, h - 0.5f});
    rows_ = static_cast<int>(S * 0.5f);
    extent_ = S + 20.0f;
    if (setSpawn) {
        spawnPos_ = {0, 1.6f, h - 2.0f};
        spawnYaw_ = 0.0f;
        spawnPending_ = true;
    }
}

void FileRoom::exitRoom() {
    if (!inRoom_) return;
    const int idx = roomIdx_;
    inRoom_ = false;
    resetScene();
    buildCorridor();
    const LobbySlot& sl = slots_[idx];
    const float inward = sl.level ? 1.7f : 3.2f;
    spawnPos_ = {sl.x + std::sin(sl.yaw) * inward, sl.level * kLevelH + 1.6f, sl.z + std::cos(sl.yaw) * inward};
    spawnYaw_ = sl.yaw + kPi;  // facing out over the atrium, away from the door just left
    prevPos_ = spawnPos_;
    spawnPending_ = true;
}

bool FileRoom::takeSpawn(Vec3& pos, float& yaw) {
    if (!spawnPending_) return false;
    spawnPending_ = false;
    pos = spawnPos_;
    yaw = spawnYaw_;
    return true;
}

void FileRoom::resolve(Vec3& p) const {
    if (!building_) return;
    if (!inRoom_ && !slots_.empty()) { resolveLobby(p); return; }
    float bd = 1e30f;
    Vec3 best = p;
    for (const Walk& r : walk_) {
        const float qx = std::clamp(p.x, r.x0, r.x1), qz = std::clamp(p.z, r.z0, r.z1);
        const float d = (qx - p.x) * (qx - p.x) + (qz - p.z) * (qz - p.z);
        if (d < bd) { bd = d; best.x = qx; best.z = qz; }
        if (d == 0.0f) return;
    }
    p.x = best.x;
    p.z = best.z;
}

// Which walkway level a player at this eye height is standing on or beneath.
static int levelAtEye(float eyeY, int L) {
    return std::clamp(static_cast<int>(std::floor((eyeY - 1.1f) / kLevelH)), 0, L - 1);
}

// Lobby movement. The carpet, the walkway slabs, the glass rails, the lift and the fountain are solid.
void FileRoom::resolveLobby(Vec3& p) const {
    const float hw = hallHW_, d = hallD_, eyeH = 0.35f;
    const int L = levels_;
    const Vec3 prev = prevPos_;
    auto isStrip = [&](float x, float z) { return !(x > -hw + kStrip && x < hw - kStrip && z > -d + kStrip && z < kFrontZ - kStrip); };
    bool inCar = false;
    p.x = std::clamp(p.x, -hw + 0.5f, hw - 0.5f);
    if (L > 1 && std::fabs(p.x) < 1.35f) {
        // Walk into the car when it is level with you; once inside you ride it.
        const bool wasIn = prev.z < -d - 0.05f;
        const bool aligned = prev.y > liftY_ + 0.2f && prev.y < liftY_ + 3.0f;
        inCar = p.z < -d + 0.55f && (wasIn || aligned);
    }
    if (inCar) {
        p.x = std::clamp(p.x, -1.3f, 1.3f);
        p.z = std::max(p.z, -d - 1.8f);
        p.y = std::clamp(p.y, liftY_ + eyeH, liftY_ + 2.9f);
    } else {
        p.z = std::clamp(p.z, -d + 0.5f, kFrontZ - 0.5f);
    }
    if (L > 1 && !inCar) {
        const int k = levelAtEye(prev.y, L);
        if (isStrip(p.x, p.z)) {  // stand on the walkway slab you are above, or stay beneath the one overhead
            const float lo = k > 0 ? k * kLevelH + eyeH : eyeH;
            const float hi = k < L - 1 ? (k + 1) * kLevelH - 0.5f : maxEye();
            p.y = std::clamp(p.y, lo, hi);
        } else if (k > 0 && isStrip(prev.x, prev.z) && prev.y < k * kLevelH + 2.85f) {
            p.x = prev.x;  // the glass rail stops anyone on foot; fly above it to leave the walkway
            p.z = prev.z;
        }
    }
    if (L >= 3 && p.y < 4.2f) {  // fountain basin, pedestal and bowl
        const float fz = fountainZ(d), pad = kFountainHalf + 0.4f;
        const float dx = p.x, dz = p.z - fz;
        if (std::fabs(dx) < pad && std::fabs(dz) < pad) {
            if (pad - std::fabs(dx) < pad - std::fabs(dz)) p.x = dx < 0 ? -pad : pad;
            else p.z = fz + (dz < 0 ? -pad : pad);
        }
    }
    prevPos_ = p;
}

// Height of the surface under a player: the lift car, a walkway slab, or the ground.
float FileRoom::floorHeight(const Vec3& p) const {
    if (!building_ || inRoom_ || slots_.empty() || levels_ <= 1) return 0.0f;
    if (std::fabs(p.x) < 1.35f && p.z < -hallD_ + 0.4f && p.y > liftY_ + 0.2f && p.y < liftY_ + 3.0f) return liftY_;
    const bool strip = !(p.x > -hallHW_ + kStrip && p.x < hallHW_ - kStrip && p.z > -hallD_ + kStrip && p.z < kFrontZ - kStrip);
    return strip ? levelAtEye(p.y, levels_) * kLevelH : 0.0f;
}

float FileRoom::maxEye() const {
    if (inRoom_) return 3.5f;
    return levels_ > 1 ? (levels_ - 1) * kLevelH + 3.5f : 6.0f;
}

void FileRoom::pump() {
    if (building_ && cardsVisible_) {
        int budget = 40;
        for (size_t i = 0; i < cards_.size(); ++i) {
            CardNode* cd = cards_[i];
            const Vec3 d = cd->position - eye_;
            const float d2 = d.x * d.x + d.z * d.z;
            cd->visible = d2 < 120.0f * 120.0f;
            // Thumbnails are only requested for frames the player is near.
            if (inRoom_ && budget > 0 && !requested_[i] && d2 < 30.0f * 30.0f && cd->roomIndex == -1 && !cd->isParentLink) {
                requested_[i] = 1;
                FileKind kind = classifyFile(cd->entry.path, cd->entry.isDir);
                if (kind != FileKind::Folder && kind != FileKind::Other) {
                    loader_.request(generation_, static_cast<int>(i), cd->entry.path, cd->entry.name, kind);
                    --budget;
                }
            }
        }
        if (!inRoom_) {
            for (int r = 0; r < static_cast<int>(rooms_.size()); ++r) {
                const float dx = roomDoorX(r) - eye_.x, dz = roomDoorZ(r) - eye_.z;
                if (rooms_[r].ready || dx * dx + dz * dz > 110.0f * 110.0f) continue;
                fillRoomInfo(r);  // one room per frame keeps the hallway smooth
                break;
            }
        }
    }
    if (liftCar_ && building_ && !inRoom_) {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        const bool nearShaft = std::fabs(eye_.x) < 2.2f && eye_.z < -hallD_ + 3.5f && eye_.z > -hallD_ + 0.3f;
        if (nearShaft) liftTarget_ = levelAtEye(eye_.y, levels_);  // the car comes to whoever walks up to it
        const float goal = liftTarget_ * kLevelH, step = 4.0f * dt;
        liftY_ += std::clamp(goal - liftY_, -step, step);
        static_cast<ShellNode*>(liftCar_)->yOffset = liftY_;
        for (auto& b : liftBtns_) b.first->position.y = liftY_ + b.second;
    }
    ThumbResult r;
    for (int n = 0; n < 3 && loader_.poll(r); ++n) {
        if (r.generation != generation_ || r.index < 0 || r.index >= static_cast<int>(cards_.size())) continue;
        cards_[r.index]->setThumbnail(renderer_.createLabel(r.bitmap, true));
    }
}

std::string FileRoom::goUp() {
    if (!loaded_) return "";
    if (inRoom_) {
        exitRoom();
        return "Back in the lobby";
    }
    if (building_ && onExitBuilding) {
        onExitBuilding();
        return "Back to the city";
    }
    if (path_.empty()) return "This PC";
    load(parentOrDrives(path_));
    return path_.empty() ? "This PC" : "Opened " + clip(path_, 24);
}

bool FileRoom::opensRoom(Node* node) const {
    auto* card = (node && node->isCard()) ? static_cast<CardNode*>(node) : nullptr;
    if (!card) return false;
    if (card->roomIndex >= 0) return true;
    return card->roomIndex > -2 && !card->isParentLink && card->entry.isDir;
}

std::string FileRoom::activate(Node* node) {
    auto* card = (node && node->isCard()) ? static_cast<CardNode*>(node) : nullptr;
    if (!card) return "";
    if (card->roomIndex >= 0) {
        const int idx = card->roomIndex;
        const std::string name = rooms_[idx].name;
        enterRoom(idx);  // destroys the card
        return "Entered " + clip(name, 24);
    }
    if (card->roomIndex <= -100) {
        liftTarget_ = std::clamp(-100 - card->roomIndex, 0, std::max(0, levels_ - 1));
        return "Lift called";
    }
    if (card->roomIndex <= -10) {
        liftTarget_ = std::clamp(-10 - card->roomIndex, 0, std::max(0, levels_ - 1));
        return liftTarget_ == 0 ? "Lift: ground floor" : "Lift: level " + std::to_string(liftTarget_);
    }
    if (card->roomIndex == -2) {
        exitRoom();
        return "Back in the lobby";
    }
    if (card->isParentLink && building_ && !inRoom_ && onExitBuilding) {
        onExitBuilding();
        return "Back to the city";
    }
    if (card->entry.isDir) {
        const std::string name = card->entry.name, target = card->entry.path;
        load(target);  // destroys the card, so use the copies afterwards
        return "Opened " + clip(name, 24);
    }
    FileKind kind = classifyFile(card->entry.path, false);
    if (onOpenMedia && (kind == FileKind::Image || kind == FileKind::Video || kind == FileKind::Audio || kind == FileKind::Text)) {
        OpenRequest req;
        req.path = card->entry.path;
        req.name = card->entry.name;
        req.kind = kind;
        req.cardPos = card->worldMatrix().transformPoint({0, 0, 0});
        req.cardYaw = card->yaw;
        req.cardScale = card->scale;
        req.poster = card->thumbnail();
        onOpenMedia(req);
        return (kind == FileKind::Text ? "Opened " : "Playing ") + clip(card->entry.name, 24);
    }
    if (onOpenFile) { DirEntry e = card->entry; onOpenFile(e); }
    return "File: " + clip(card->entry.name, 24);
}

std::string FileRoom::stepFrom(const std::string& path, int dir) {
    const int n = static_cast<int>(cards_.size());
    int cur = -1;
    for (int i = 0; i < n; ++i)
        if (cards_[i]->entry.path == path && cards_[i]->roomIndex == -1) cur = i;
    if (cur < 0 || n < 2) return "";
    for (int k = 1; k < n; ++k) {
        int i = ((cur + dir * k) % n + n) % n;
        CardNode* c = cards_[i];
        if (c->entry.isDir || c->roomIndex != -1) continue;
        FileKind kind = classifyFile(c->entry.path, false);
        if (kind == FileKind::Image || kind == FileKind::Video || kind == FileKind::Audio || kind == FileKind::Text)
            return activate(c);
    }
    return "";
}

std::string FileRoom::openFirst(bool textOnly) {
    for (CardNode* c : cards_) {
        if (c->entry.isDir || c->isParentLink || c->roomIndex != -1) continue;
        FileKind k = classifyFile(c->entry.path, false);
        const bool isText = k == FileKind::Text;
        const bool isMedia = k == FileKind::Image || k == FileKind::Video || k == FileKind::Audio;
        if (textOnly ? isText : isMedia) return activate(c);
    }
    return textOnly ? "No text file in this room" : "No media in this room";
}

void FileRoom::setAccent(Vec3 c) {
    if (header_) header_->setColor(c);
}

}  // namespace sf
