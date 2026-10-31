#pragma once
// The media window: one interface for browsing and playing. Music is shown as albums (cover art from the tags or the
// folder), video as rows of posters per folder, plus a list of internet radio stations. A now-playing bar at the bottom
// holds the transport, seek bar, volume and a live spectrum.
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "MediaLibrary.h"
#include "MediaMeta.h"
#include "PanelApp.h"

namespace sf {

struct NowInfo {
    bool active = false, playing = false, video = false, live = false, loading = false;
    std::string title, path;
    double pos = 0, dur = 0;
    float vol = 1.0f;
    bool shuffle = false;
    int repeat = 0;
};

struct MediaHost {
    std::function<std::vector<MediaTrack>()> tracks;
    std::function<size_t()> count;
    std::function<bool()> scanning;
    std::function<int()> scanned;
    std::function<void()> addDefaultFolders;  // Music + Videos, then a rescan
    std::function<void()> openFolderSettings;
    std::function<void()> pickFolders;  // native multi-select folder dialog
    std::function<bool()> pickBusy;
    std::function<std::vector<std::string>()> folders;  // watched folders
    std::function<void(const std::string&)> removeFolder;
    std::function<void(const std::vector<std::string>&, int)> play;
    std::function<void(const std::string&, const std::string&)> playStream;  // name, url
    std::function<NowInfo()> now;
    std::function<void()> toggle, next, prev, toggleShuffle, cycleRepeat;
    std::function<void(double)> seek;
    std::function<void(float)> setVolume;
    std::function<bool(float*, int)> spectrum;
    std::function<bool(std::vector<uint8_t>&, int&, int&)> videoFrame;  // newest RGBA frame, its width and height
    std::function<void(int, int)> videoTarget;                           // largest picture worth decoding (pixels)
};

class MediaApp : public PanelApp {
public:
    MediaApp(Renderer& renderer, MediaHost host);
    const char* name() const override { return "MEDIA"; }
    bool wantsText() const override { return searchFocus_; }
    bool back() override;
public:
    void scriptClick(int id, int arg) { if (id < 0) theatre_ = true; else click(id, arg, 0, 0); invalidate(); }
    void showPlaying(bool video) { theatre_ = video; manage_ = false; invalidate(); }
    void enterCinema() { theatre_ = true; click(63, 0, 0, 0); }
private:
    bool chromeless() const override { return cinema_ && theatre_ && fullscreen(); }
    void setTab(int t);
    void setSearch(const std::string& s) { q_ = s; layoutDirty_ = true; invalidate(); }

protected:
    void onOpen() override;
    void onResize(int w, int h) override;
    void tick(double dt) override;
    void paint(Bitmap& bm) override;
    bool fsResizable() const override { return true; }
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onWheel(float x, float y, float notches) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    double repaintInterval() const override { return animating_ ? 0.04 : 0.0; }
    bool repaintPartial(Bitmap& bm, int& x, int& y, int& w, int& h) override;
    const Bitmap* videoLayer(int& x, int& y, int& w, int& h, bool& fresh) override;

private:
    struct Item {
        MediaTrack t;
        TrackMeta m;
        std::string title;
    };
    struct Album {
        std::string key, title, artist;
        int year = 0;
        std::vector<int> items;  // into items_, in track order
        bool artAsked = false;
    };
    struct Vid {
        MediaTrack t;
        std::string dir;
        bool asked = false;
    };
    struct Station {
        std::string name, url, genre;
    };
    struct Hit {
        ui::Rect r;
        int id, arg;
    };

    void reload();
    void rebuildAlbums();
    void loadStations();
    void pump();
    bool albumMatches(const Album& a) const;
    const Bitmap* artFor(const Album& a);
    const Bitmap* thumbFor(Vid& v);
    void playAlbum(int album, int from, bool shuffled);
    void click(int id, int arg, float x, float y);
    int hitAt(float x, float y) const;

    MediaHost host_;
    MediaMeta meta_;
    int tab_ = 0;  // 0 music, 1 video, 2 radio
    int albumOpen_ = -1;
    bool manage_ = false;  // folder list view
    std::string q_;
    bool searchFocus_ = false;

    std::vector<Item> items_;
    std::vector<Album> albums_;
    std::vector<Vid> vids_;
    std::vector<Station> stations_;
    std::map<std::string, TrackMeta> tagCache_;
    std::set<std::string> tagAsked_;
    size_t tagsOutstanding_ = 0;
    std::map<std::string, Bitmap> artCache_, thumbCache_;
    std::set<std::string> artMissing_, thumbMissing_, artAsked_, thumbAsked_;
    size_t libCount_ = ~size_t(0);
    bool libScanning_ = false, albumsDirty_ = false, layoutDirty_ = true;
    double reloadT_ = 99, albumT_ = 0;

    std::vector<Hit> hits_;
    int hover_ = -1;
    float scroll_ = 0, scrollTarget_ = 0, scrollMax_ = 0;
    float bands_[32] = {}, peaks_[32] = {};
    bool animating_ = false, bandsMoving_ = false;
    double specT_ = 0;
    int vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;
    bool vidFresh_ = false;
    bool specOnly_ = false, spValid_ = false;
    ui::Rect spRect_{};
    float spK_ = 1;
    Color8 spA_{}, spB_{}, spT_{};
    struct PxRect { int x = 0, y = 0, w = 0, h = 0; } spPx_;
    int spW_ = 0, spH_ = 0;
    std::vector<uint8_t> backdrop_;
    void snapshotBackdrop(const Bitmap& bm);
    void drawBars(Bitmap& bm, const ui::Rect& sp);
    // The clock and seek bar tick once a second; they repaint alone so the rest of the window (and the video) never stalls.
    bool seekDirty_ = false, skValid_ = false, skSeekable_ = false;
    PxRect skPx_;
    int skW_ = 0, skH_ = 0;
    std::vector<uint8_t> skBack_;
    float skK_ = 1, skX0_ = 0, skX1_ = 0, skY_ = 0;
    Color8 skAccent_{}, skText_{}, skDim_{};
    void drawSeek(Bitmap& bm);
    NowInfo now_;
    std::string nowPathSeen_;
    int nowAlbum_ = -1;
    float hoverFade_ = 0;
    double clockT_ = 0;
    bool theatre_ = false, videoSeen_ = false;  // a video is shown inside the window
    bool cinema_ = false, cinemaFs_ = false;  // true full screen: only the picture, controls fade away
    double idleT_ = 0;
    float uiFade_ = 1, lastPx_ = -1, lastPy_ = -1;
    void exitCinema();
    void paintCinema(Bitmap& bm);
    Bitmap frame_;
    Bitmap layer_;  // scrolling content is drawn here, then clipped onto the window
};

}  // namespace sf
