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
    MediaPlayer(MediaPlayer&&) noexcept;
    MediaPlayer& operator=(MediaPlayer&&) noexcept;

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

    // Spectrum of the audio being heard right now: n log spaced bands (about 50 Hz to 16 kHz), each 0..1.
    // Returns false when nothing audible is playing.
    bool spectrum(float* bands, int n);

    // Main thread, once per frame: when a newer video frame is due, copies it (RGBA, width*height*4) and returns true.
    bool takeFrame(std::vector<uint8_t>& rgba);
    // Same, and reports the size of the frame handed over (it can be smaller than videoWidth() x videoHeight(), see below).
    bool takeFrame(std::vector<uint8_t>& rgba, int& w, int& h);
    // Decoded frames are scaled down (never up) on the decoder thread to fit this box, so a 4K movie shown in a small window
    // does not drag 33 MB frames through the UI. 0 x 0 = native size.
    void setMaxVideoSize(int w, int h);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sf
