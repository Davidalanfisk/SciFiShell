#pragma once
// 3D file browser room: files and folders are cards on a ring around the player.
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LCARSUI.h"
#include "SceneGraph.h"
#include "Thumbnails.h"
#include "Viewer.h"

namespace sf {

class CardNode : public Node {
public:
    CardNode(Renderer& renderer, const DirEntry& entry, const std::string& label);
    void render(Renderer& renderer) override;
    bool isCard() const override { return true; }
    DirEntry entry;
    bool hover = false;
    bool isParentLink = false;
    bool tall = false;  // a room door: portrait label with the title and an info block
    int roomIndex = -1;  // >= 0: a hallway door to that room; -2: the door back to the hallway
    void setThumbnail(std::shared_ptr<GpuLabel> thumb) { thumb_ = std::move(thumb); }
    const std::shared_ptr<GpuLabel>& thumbnail() const { return thumb_; }
    void setInfo(std::vector<std::string> lines) { info_ = std::move(lines); label_.reset(); doorDirty_ = true; }
    bool isDoor() const { return tall; }  // lobby doors use the preview-and-details face
    void setDoorArt(Bitmap art) { art_ = std::move(art); hasArt_ = true; doorDirty_ = true; }
    void setDoorStats(std::string big, std::string label) { big_ = std::move(big); bigLabel_ = std::move(label); doorDirty_ = true; }
    Color8 accent{80, 220, 255, 255};
    bool hasPreviewRequested = false;
    void update(double dt, double time) override;

private:
    void ensureLabel();
    void bakeDoor();
    Bitmap art_, hoverBase_;
    bool hasArt_ = false, doorDirty_ = true, shownHover_ = false;
    std::string big_, bigLabel_;
    float hoverT_ = 0.0f, maxScroll_ = 0.0f, scroll_ = 0.0f;
    std::shared_ptr<GpuLabel> face_;
    Renderer* r_;
    std::string text_;
public:
    bool leavesBuilding = false;
private:
    std::vector<std::string> info_;
    std::shared_ptr<GpuLabel> label_;
    std::shared_ptr<GpuLabel> thumb_;
};

class FileRoom {
public:
    explicit FileRoom(Renderer& renderer);
    void load(std::string path);
    void loadEmpty();  // grid floor only, no cards
    bool loaded() const { return loaded_; }
    int cardCount() const { return static_cast<int>(cards_.size()); }
    float extent() const { return extent_; }  // walkable radius, grows with the number of files
    void setCardsVisible(bool v) {
        cardsVisible_ = v;
        for (auto* c : cards_) c->visible = v;
        if (header_) header_->visible = v;
    }
    // Building interior (any folder path): a corridor with one room per file. Collision keeps the player inside.
    bool building() const { return building_; }
    float floorHeight(const Vec3& p) const;
    float maxEye() const;  // highest the eye may fly
    bool inRoom() const { return inRoom_; }
    void debugEnter(int idx) { enterRoom(idx); }
    void refresh();  // reloads the current folder or room in place
    // Position/yaw the camera should jump to after entering or leaving a room (consumed once).
    bool takeSpawn(Vec3& pos, float& yaw);
    void setEye(Vec3 e) { eye_ = e; }
    void resolve(Vec3& p) const;
    float speedBoost() const { return std::min(8.0f, std::max(1.0f, rows_ / 12.0f)); }
    Node& root() { return *root_; }
    const std::string& currentPath() const { return path_; }
    // Uploads finished thumbnails (a few per frame). Call once per frame from the main thread.
    void pump();

    void setHover(Node* node);
    // Activates a card: folders are opened. Returns a short status message.
    std::string activate(Node* node);
    bool opensRoom(Node* node) const;  // activating this card enters a room / folder (can take a moment to build)
    // Goes to the parent folder (or the drives room from a drive root). Returns a status message.
    std::string goUp();
    // Opens the first text (textOnly) or image/video/audio card in the room; returns a status message.
    std::string openFirst(bool textOnly);
    // Opens the previous (-1) / next (+1) viewable file after `path` in this folder, wrapping around.
    std::string stepFrom(const std::string& path, int dir);
    void setAccent(Vec3 c);  // recolours the path header to the theme accent
    // Called when a media file (image/video/audio) card is activated.
    std::function<void(const OpenRequest&)> onOpenMedia;
    // Called when a non-viewable file card is activated: opens that file's own room (info + actions).
    std::function<void(const DirEntry&)> onOpenFile;
    std::function<void()> onExitBuilding;  // leaving the hallway returns to the city

private:
    void addSky();
    struct RoomInfo {
        std::string name, path;
        bool loose = false, ready = false, capped = false, hasPreview = false, previewIsImage = false;
        int files = 0, dirs = 0, imgs = 0, vids = 0, auds = 0, txts = 0, others = 0;
        DirEntry preview;
        std::vector<std::string> sample;
    };
    void resetScene();
    CardNode* addCard(std::unique_ptr<CardNode> card);
    void buildCorridor();
    bool soloRoom_ = false;  // a building with a single room: entered directly, and its door leaves the building
    void enterRoom(int idx, bool setSpawn = true);
    void exitRoom();
    void fillRoomInfo(int r);
    void addRoomExtras(int r);
    float roomDoorZ(int r) const;
    float roomDoorX(int r) const;
    void resolveLobby(Vec3& p) const;
    Renderer& renderer_;
    std::unique_ptr<Node> rootHolder_;
    Node* root_;
    std::string path_;
    PanelNode* header_ = nullptr;
    CardNode* hovered_ = nullptr;
    std::vector<CardNode*> cards_;
    ThumbnailLoader loader_;
    int generation_ = 0;
    bool loaded_ = false;
    float extent_ = 12.0f;
    bool building_ = false, cardsVisible_ = true;
    int rows_ = 0, roomIdx_ = -1;
    bool inRoom_ = false, spawnPending_ = false;
    float spawnYaw_ = 0.0f;
    Vec3 spawnPos_{0, 1.6f, 0};
    DirEntry parentEntry_;
    std::vector<RoomInfo> rooms_;
    std::vector<DirEntry> looseItems_;
    std::vector<char> requested_;
    Vec3 eye_{0, 1.6f, 0};
    struct Walk { float x0, x1, z0, z1; };
    std::vector<Walk> walk_;
    struct LobbySlot { float x, z, yaw; int level; };
    std::vector<LobbySlot> slots_;
    float hallHW_ = 14.0f, hallD_ = 24.0f;
    int levels_ = 1;
    std::vector<CardNode*> doorCards_;
    Node* liftCar_ = nullptr;
    std::vector<std::pair<CardNode*, float>> liftBtns_;  // in-car buttons and their height above the car floor
    int liftTarget_ = 0;
    float liftY_ = 0.0f;
    mutable Vec3 prevPos_{0, 1.6f, 2.0f};
};

}  // namespace sf
