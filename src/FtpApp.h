#pragma once
// A very basic FTP client window: connect with host / user / password (anonymous when left empty), browse the remote
// folders, download the selected file to the Downloads folder and upload a local file picked with the file browser.
// Network calls run on a worker thread so the window never stalls.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "PanelApp.h"

namespace sf {

class FtpApp : public PanelApp {
public:
    explicit FtpApp(Renderer& renderer);
    ~FtpApp() override;
    const char* name() const override { return "FTP"; }
    bool wantsText() const override { return focus_ >= 0; }
    bool fsResizable() const override { return true; }
    void upload(const std::string& localPath);

    std::function<void()> requestUpload;  // the shell shows the file browser in pick mode

protected:
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onWheel(float x, float y, float notches) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    void tick(double dt) override;
    bool keepAlive() const override { return busy_; }
    double repaintInterval() const override { return 0.5; }

private:
    struct Entry {
        std::string name;
        bool dir = false;
        unsigned long long size = 0;
    };
    struct Hit {
        ui::Rect r;
        int id;
    };
    void act(int id);
    void connect();
    void disconnect();
    void listing(const std::string& cd);  // "" = refresh, ".." = up
    void download(const Entry& e);
    void run(std::function<void()> job);
    void setStatus(const std::string& s);
    std::string& field(int i) { return i == 0 ? host_ : i == 1 ? user_ : pass_; }

    std::string host_, user_, pass_;
    int focus_ = -1;
    std::vector<Entry> entries_;
    std::string cwd_ = "/", status_ = "Enter a server and press CONNECT. Leave user empty for anonymous.";
    bool connected_ = false;
    int sel_ = -1;
    float scroll_ = 0;
    std::vector<Hit> hits_;
    int hover_ = -1;
    ui::Rect list_;
    float rowH_ = 40;
    std::mutex mu_;
    std::atomic<bool> busy_{false};
    std::thread worker_;
    void* net_ = nullptr;
    void* ftp_ = nullptr;
    bool dirtyFromWorker_ = false;
};

}  // namespace sf
