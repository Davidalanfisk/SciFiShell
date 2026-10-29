#pragma once
// Audio/video file playback. Windows: Media Foundation source reader on a worker thread, audio through an SDL
// audio queue (which is also the master clock). Other platforms: open() fails and the viewer shows the poster only.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sf {

class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    // Blocks briefly while the worker opens the file. Playback starts paused; call play().
    bool open(const std::string& utf8Path);
    void close();

    bool isOpen() const;
    bool hasVideo() const;
    bool hasAudio() const;
    int videoWidth() const;
    int videoHeight() const;
    double duration() const;  // seconds, 0 when unknown

    void play();
    void pause();
    bool playing() const;
    void seek(double seconds);
    double position() const;
    void setVolume(float v);  // 0..1
    float volume() const;
    bool ended() const;       // reached the end of the stream (playback is paused there)

    // Main thread, once per frame: when a newer video frame is due, copies it (RGBA, width*height*4) and returns true.
    bool takeFrame(std::vector<uint8_t>& rgba);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sf
