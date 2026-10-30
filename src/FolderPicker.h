#pragma once
// A native "choose folders" dialog that runs on its own thread so the 3D world keeps rendering while it is open.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sf {

class FolderPicker {
public:
    ~FolderPicker();
    // Opens the dialog (multi-select). Ignored while one is already showing. owner is a native window handle or null.
    void start(void* owner);
    bool busy() const { return busy_.load(); }
    // True once per finished dialog that returned at least one folder.
    bool poll(std::vector<std::string>& out);

private:
    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::mutex mutex_;
    std::vector<std::string> result_;
    bool ready_ = false;
};

}  // namespace sf
