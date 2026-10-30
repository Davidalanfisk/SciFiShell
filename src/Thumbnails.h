#pragma once
// File classification and background thumbnail generation for the File Room.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <vector>

#include "TextRender.h"
#include "Utils.h"

namespace sf {

enum class FileKind { Folder, Image, Video, Audio, Text, Other };

// Extension based; unknown extensions are Other.
FileKind classifyFile(const std::string& path, bool isDir);
std::string fileExtensionUpper(const std::string& path);

// Decodes the frame at roughly 10% of a video (max 5s in). Windows (Media Foundation) only;
// returns false on other platforms or when the codec is unavailable. Output is opaque RGBA.
bool extractVideoFrame(const std::string& utf8Path, Bitmap& out);

constexpr int kThumbW = 384;
constexpr int kThumbH = 248;

// Lobby door faces: one opaque portrait bitmap (preview art on top, title or hover details below).
constexpr int kDoorW = 520;
constexpr int kDoorH = 650;
constexpr int kDoorArtW = 440;
constexpr int kDoorArtH = 275;

struct DoorFaceSpec {
    const Bitmap* art = nullptr;  // kDoorArtW x kDoorArtH, or null for a placeholder panel
    std::string bigNumber, bigLabel;  // placeholder panel content (e.g. "128" / "files")
    Color8 accent{80, 220, 255, 255};
    bool hover = false;
};
// Frame, preview art and divider; the text area is left empty.
Bitmap composeDoorBase(const DoorFaceSpec& spec);
// Draws the title (idle) or the full name and details (hover, scrolled by `scroll` px) into the text area of `face`.
// Returns how far the hover content can scroll.
float drawDoorText(Bitmap& face, const std::string& title, const std::vector<std::string>& info, bool hover, float scroll, Color8 accent);

struct ThumbResult {
    int generation = 0;
    int index = 0;
    Bitmap bitmap;
};

// Single worker thread. request() is cheap; results are polled from the main thread.
class ThumbnailLoader {
public:
    ThumbnailLoader();
    ~ThumbnailLoader();

    // Drops queued work and invalidates in-flight results; returns the new generation.
    int reset();
    void request(int generation, int index, const std::string& path, const std::string& name, FileKind kind, bool plain = false);
    bool poll(ThumbResult& out);

private:
    struct Job {
        int generation, index;
        std::string path, name;
        FileKind kind;
        bool plain;  // door art: no name strip or badge
    };
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<ThumbResult> results_;
    std::atomic<int> generation_{0};
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace sf
