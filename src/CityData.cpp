#include "CityData.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <intrin.h>
#endif

namespace fs = std::filesystem;

namespace sf {

#ifdef _WIN32
struct CityData::PdhState {
    PDH_HQUERY cpuQ = nullptr;
    std::vector<PDH_HCOUNTER> cores;
    PDH_HQUERY gpuQ = nullptr;
    PDH_HCOUNTER gpuC = nullptr;
    PDH_HQUERY memQ = nullptr;
    PDH_HCOUNTER memC = nullptr;
    double gpuAge = 0;
    unsigned long long lastIdle = 0, lastKernel = 0, lastUser = 0;
};
#else
struct CityData::PdhState {};
#endif

CityData::CityData() = default;
CityData::~CityData() { stop(); }

void CityData::sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::string narrow(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

static std::atomic<bool> g_fromCache{false};
static std::atomic<long long> g_lastSave{0};

void CityData::start() {
    if (started_) return;
    started_ = true;
    std::vector<std::string> roots;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::string r = std::string(1, char('A' + i)) + ":\\";
        UINT t = GetDriveTypeA(r.c_str());
        if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE) roots.push_back(r);
    }
#else
    roots.push_back("/");
#endif
    {
        std::lock_guard<std::mutex> lk(mu_);
        drives_.resize(roots.size());
        folderIndex_.resize(roots.size());
        for (size_t i = 0; i < roots.size(); ++i) {
            drives_[i].root = roots[i];
            drives_[i].name = roots[i].substr(0, roots[i].size() > 1 && roots[i][1] == ':' ? 2 : 1);
        }
    }
    pending_ = (int)roots.size();
    if (loadCache(roots)) { pending_ = 0; g_fromCache = true; }  // the city can be laid out straight away; the scan then corrects it
    for (size_t i = 0; i < roots.size(); ++i) {
        threads_.emplace_back([this, i, r = roots[i]] { scanDrive((int)i, r); });
#ifdef _WIN32
        if (GetDriveTypeA(roots[i].c_str()) == DRIVE_FIXED)
            threads_.emplace_back([this, i, r = roots[i]] { watchDrive((int)i, r); });
#endif
    }
}

void CityData::stop() {
    if (!started_) return;
    stop_ = true;
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (void* h : watchHandles_) CancelIoEx((HANDLE)h, nullptr);
    }
#endif
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    started_ = false;
#ifdef _WIN32
    if (pdh_) {
        if (pdh_->cpuQ) PdhCloseQuery(pdh_->cpuQ);
        if (pdh_->gpuQ) PdhCloseQuery(pdh_->gpuQ);
        if (pdh_->memQ) PdhCloseQuery(pdh_->memQ);
        pdh_.reset();
    }
#endif
}

void CityData::publishFolder(int slot, size_t folder, uint64_t files, uint64_t bytes, bool done) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& d = drives_[slot];
    if (folder >= d.folders.size()) return;
    auto& f = d.folders[folder];
    d.files += files - f.files;
    d.bytes += bytes - f.bytes;
    f.files = files;
    f.bytes = bytes;
    f.done = done;
    ++version_;
}

void CityData::publishSub(int slot, size_t folder, size_t sub, uint64_t files, uint64_t bytes, bool done) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& d = drives_[slot];
    if (folder >= d.folders.size() || sub >= d.folders[folder].subs.size()) return;
    auto& f = d.folders[folder];
    auto& s = f.subs[sub];
    d.files += files - s.files;
    d.bytes += bytes - s.bytes;
    f.files += files - s.files;
    f.bytes += bytes - s.bytes;
    s.files = files;
    s.bytes = bytes;
    s.done = done;
    ++version_;
}

// Immediate children of a directory: subfolders (reparse points skipped) plus a count of the files directly inside.
static void listChildren(const fs::path& dir, std::vector<fs::path>& dirs, uint64_t& files, uint64_t& bytes) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
#ifdef _WIN32
        DWORD a = GetFileAttributesW(it->path().c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const bool isDir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
        if (it->is_symlink(e2)) continue;
        const bool isDir = it->is_directory(e2);
#endif
        if (isDir) {
            dirs.push_back(it->path());
        } else {
            ++files;
            auto s = it->file_size(e2);
            if (!e2) bytes += s;
        }
    }
}

void CityData::scanDrive(int slot, std::string root) {
    sleepMs(1500);  // let the shell start first
    std::error_code ec;
    auto sp = fs::space(fs::path(root), ec);
    std::vector<fs::path> top;
    bool cached = false;  // the layout came from the cache: reconcile with it instead of rebuilding
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& d = drives_[slot];
        cached = d.listed;
        if (!ec) {
            d.total = sp.capacity;
            d.freeBytes = sp.available;
        }
        if (!cached) {
            d.folders.push_back({"(ROOT FILES)", 0, 0, false});
            folderIndex_[slot]["(ROOT FILES)"] = 0;
        }
    }
    uint64_t rootFiles = 0, rootBytes = 0;
    fs::directory_iterator it(fs::path(root), fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && !stop_; it.increment(ec)) {
        std::error_code e2;
        bool isDir = it->is_directory(e2);
        if (isDir) {
            auto name = it->path().filename().u8string();
            std::string n(name.begin(), name.end());
            if (n == "System Volume Information" || n == "$RECYCLE.BIN") continue;
#ifdef _WIN32
            DWORD a = GetFileAttributesW(it->path().c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
#endif
            top.push_back(it->path());
        } else {
            ++rootFiles;
            auto s = it->file_size(e2);
            if (!e2) rootBytes += s;
        }
    }
    std::vector<std::vector<fs::path>> pendingSubs;
    std::vector<std::pair<uint64_t, uint64_t>> pendingDirect;
    constexpr size_t kNone = static_cast<size_t>(-1);
    std::vector<size_t> fidx(top.size(), 0), filesSlot(top.size(), kNone), moreSlot(top.size(), kNone);
    std::vector<std::vector<size_t>> subSlot(top.size());
    std::set<std::pair<size_t, size_t>> seen;  // (folder, sub) pairs the live scan has refreshed
    {
        // Phase 1b: list each block's subfolders so the city can be laid out before any counting.
        auto& subDirs = pendingSubs;
        auto& direct = pendingDirect;
        subDirs.resize(top.size());
        direct.resize(top.size());
        for (size_t i = 0; i < top.size() && !stop_; ++i) listChildren(top[i], subDirs[i], direct[i].first, direct[i].second);

        std::lock_guard<std::mutex> lk(mu_);
        auto& d = drives_[slot];
        if (!cached && rootFiles > 0) d.folders[0].subs.push_back({"(FILES)", rootFiles, rootBytes, true, {}});
        for (size_t i = 0; i < top.size(); ++i) {
            auto u = top[i].filename().u8string();
            std::string n(u.begin(), u.end());
            size_t fi;
            auto found = folderIndex_[slot].find(n);
            if (found != folderIndex_[slot].end()) {
                fi = found->second;
            } else {
                fi = d.folders.size();
                folderIndex_[slot][n] = fi;
                d.folders.push_back(CityFolder{n, 0, 0, false, {}});
            }
            fidx[i] = fi;
            CityFolder& f = d.folders[fi];
            auto& sidx = subIndex_[{slot, (int)fi}];
            auto ensure = [&](const std::string& name) {
                auto it2 = sidx.find(name);
                if (it2 != sidx.end()) return it2->second;
                sidx[name] = f.subs.size();
                f.subs.push_back({name, 0, 0, false, {}});
                return f.subs.size() - 1;
            };
            if (direct[i].first > 0) filesSlot[i] = ensure("(FILES)");
            for (size_t k = 0; k < subDirs[i].size() && k < kMaxSubs; ++k) {
                auto su = subDirs[i][k].filename().u8string();
                subSlot[i].push_back(ensure(std::string(su.begin(), su.end())));
            }
            if (subDirs[i].size() > kMaxSubs) moreSlot[i] = ensure("(MORE)");
        }
        d.listed = true;
        ++version_;
    }
    publishFolder(slot, 0, rootFiles, rootBytes, true);
    if (!cached) --pending_;

    for (size_t i = 0; i < top.size() && !stop_; ++i) {
        const size_t fi = fidx[i];
        if (filesSlot[i] != kNone) {
            publishSub(slot, fi, filesSlot[i], pendingDirect[i].first, pendingDirect[i].second, true);
            seen.insert({fi, filesSlot[i]});
        }
        uint64_t moreFiles = 0, moreBytes = 0;
        for (size_t k = 0; k < pendingSubs[i].size() && !stop_; ++k) {
            const bool more = k >= kMaxSubs;
            const size_t si = more ? moreSlot[i] : subSlot[i][k];
            seen.insert({fi, si});
            uint64_t files = more ? moreFiles : 0, bytes = more ? moreBytes : 0, n = 0;
            std::error_code e3;
            fs::recursive_directory_iterator rit(pendingSubs[i][k], fs::directory_options::skip_permission_denied, e3), rend;
            for (; !e3 && rit != rend && !stop_; rit.increment(e3)) {
                std::error_code e4;
#ifdef _WIN32
                DWORD a = GetFileAttributesW(rit->path().c_str());
                if (a == INVALID_FILE_ATTRIBUTES) continue;
                if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (a & FILE_ATTRIBUTE_DIRECTORY) rit.disable_recursion_pending();
                    continue;
                }
                if (a & FILE_ATTRIBUTE_DIRECTORY) continue;
#else
                if (rit->is_symlink(e4)) { rit.disable_recursion_pending(); continue; }
                if (rit->is_directory(e4)) continue;
#endif
                ++files;
                auto s = rit->file_size(e4);
                if (!e4) bytes += s;
                if (++n % 4096 == 0) publishSub(slot, fi, si, files, bytes, false);
                if (n % 2048 == 0) sleepMs(2);
            }
            if (more) { moreFiles = files; moreBytes = bytes; }
            publishSub(slot, fi, si, files, bytes, !more || k + 1 == pendingSubs[i].size());
        }
        bool listedAll;
        {
            std::lock_guard<std::mutex> lk(mu_);
            drives_[slot].folders[fi].done = true;
            ++version_;
            listedAll = std::all_of(drives_.begin(), drives_.end(), [](const CityDrive& x) { return x.listed; });
        }
        // First run: keep a growing snapshot on disk so an interrupted scan still gives the next launch a city.
        const long long nowS = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!g_fromCache && listedAll && nowS - g_lastSave > 15 && !stop_) { g_lastSave = nowS; saveCache(); }
    }

    if (cached && !stop_) {
        std::vector<std::tuple<size_t, size_t>> gone;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto& d = drives_[slot];
            for (size_t fi = 1; fi < d.folders.size(); ++fi)
                for (size_t si = 0; si < d.folders[fi].subs.size(); ++si)
                    if (!seen.count({fi, si}) && d.folders[fi].subs[si].files > 0) gone.emplace_back(fi, si);
        }
        for (auto& g : gone) publishSub(slot, std::get<0>(g), std::get<1>(g), 0, 0, true);
    }
    bool all = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        drives_[slot].done = true;
        all = std::all_of(drives_.begin(), drives_.end(), [](const CityDrive& x) { return x.done; });
        ++version_;
    }
    if (all && !stop_) saveCache();
}

static std::string cacheFile() {
    std::string dir;
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    dir = std::string(base ? base : ".") + "\\SciFiShell";
#else
    const char* base = std::getenv("HOME");
    dir = std::string(base ? base : ".") + "/.scifishell";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + "/city.cache";
}

// The last finished scan, so the next start can lay the city out at once.
void CityData::saveCache() {
    std::string out = "SCIFICITY1\n";
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& d : drives_) {
            out += "D\t" + d.root + "\t" + d.name + "\t" + std::to_string(d.total) + "\t" + std::to_string(d.freeBytes) + "\t" +
                   std::to_string(d.files) + "\t" + std::to_string(d.bytes) + "\n";
            for (const auto& f : d.folders) {
                out += "F\t" + f.name + "\t" + std::to_string(f.files) + "\t" + std::to_string(f.bytes) + "\n";
                for (const auto& sub : f.subs)
                    out += "S\t" + sub.name + "\t" + std::to_string(sub.files) + "\t" + std::to_string(sub.bytes) + "\n";
            }
        }
    }
    const std::string path = cacheFile(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << out;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

bool CityData::loadCache(const std::vector<std::string>& roots) {
    std::ifstream f(cacheFile(), std::ios::binary);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line) || line != "SCIFICITY1") return false;
    std::vector<CityDrive> drives;
    auto fields = [](const std::string& l) {
        std::vector<std::string> v;
        size_t a = 0;
        while (true) {
            size_t b = l.find('\t', a);
            v.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return v;
    };
    auto num = [](const std::string& x) { return static_cast<uint64_t>(std::strtoull(x.c_str(), nullptr, 10)); };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto v = fields(line);
        if (v[0] == "D" && v.size() >= 7) {
            CityDrive d;
            d.root = v[1];
            d.name = v[2];
            d.total = num(v[3]);
            d.freeBytes = num(v[4]);
            d.files = num(v[5]);
            d.bytes = num(v[6]);
            d.listed = true;
            drives.push_back(std::move(d));
        } else if (v[0] == "F" && v.size() >= 4 && !drives.empty()) {
            drives.back().folders.push_back(CityFolder{v[1], num(v[2]), num(v[3]), false, {}});
        } else if (v[0] == "S" && v.size() >= 4 && !drives.empty() && !drives.back().folders.empty()) {
            drives.back().folders.back().subs.push_back(CityFolder{v[1], num(v[2]), num(v[3]), false, {}});
        } else {
            return false;
        }
    }
    if (drives.size() != roots.size()) return false;
    for (size_t i = 0; i < roots.size(); ++i)
        if (drives[i].root != roots[i] || drives[i].folders.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    drives_ = std::move(drives);
    for (size_t i = 0; i < drives_.size(); ++i) {
        for (size_t fi = 0; fi < drives_[i].folders.size(); ++fi) {
            folderIndex_[i][drives_[i].folders[fi].name] = fi;
            auto& sidx = subIndex_[{(int)i, (int)fi}];
            for (size_t si = 0; si < drives_[i].folders[fi].subs.size(); ++si) sidx[drives_[i].folders[fi].subs[si].name] = si;
        }
    }
    ++version_;
    return true;
}

void CityData::onChange(int slot, const std::wstring& rel, unsigned long action) {
    size_t cut = rel.find_first_of(L"\\/");
    std::string first = cut == std::wstring::npos ? "(ROOT FILES)" : narrow(rel.substr(0, cut));
    std::wstring comp2;  // second path component, when there is one
    bool hasThird = false;
    if (cut != std::wstring::npos) {
        size_t cut2 = rel.find_first_of(L"\\/", cut + 1);
        comp2 = rel.substr(cut + 1, cut2 == std::wstring::npos ? std::wstring::npos : cut2 - cut - 1);
        hasThird = cut2 != std::wstring::npos;
    }
    std::wstring full;
    {
        std::lock_guard<std::mutex> lk(mu_);
        full.assign(drives_[slot].root.begin(), drives_[slot].root.end());
    }
    full += rel;
    uint64_t sz = 0;
    bool isFile = true;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info{};
    bool exists = GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &info) != 0;
    if (exists) {
        isFile = !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        sz = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }
#endif
    std::lock_guard<std::mutex> lk(mu_);
    auto& d = drives_[slot];
    auto& idx = folderIndex_[slot];
    auto fit = idx.find(first);
    if (fit == idx.end()) {
        if (cut != std::wstring::npos && action == 1 /*ADDED*/ && d.listed) {
            idx[first] = d.folders.size();
            d.folders.push_back({first, 0, 0, true, {}});
            fit = idx.find(first);
        } else {
            return;
        }
    }
    size_t fi = fit->second;
    auto& f = d.folders[fi];

    // The building this change belongs to: "(FILES)" for loose files, otherwise the second path component.
    std::string bname = "(FILES)";
    if (hasThird || (!comp2.empty() && !isFile)) bname = narrow(comp2);
    int si = -1;
    {
        auto& sidx = subIndex_[{slot, (int)fi}];
        auto sit = sidx.find(bname);
        if (sit != sidx.end()) {
            si = (int)sit->second;
        } else if (f.subs.size() < kMaxSubs + 2 && action != 2 && action != 4) {
            sidx[bname] = f.subs.size();
            si = (int)f.subs.size();
            f.subs.push_back({bname, 0, 0, true, {}});
        }
    }
    CityFolder* sub = si >= 0 ? &f.subs[(size_t)si] : nullptr;
    auto& ch = changes_[{slot, (int)fi, si}];
    ch.drive = slot;
    ch.folder = (int)fi;
    ch.sub = si;
    if (action == 1 /*ADDED*/ || action == 5 /*RENAMED_NEW*/) {
        if (isFile) {
            ++f.files; ++d.files; f.bytes += sz; d.bytes += sz;
            if (sub) { ++sub->files; sub->bytes += sz; }
        }
        ++ch.added;
    } else if (action == 2 /*REMOVED*/ || action == 4 /*RENAMED_OLD*/) {
        if (f.files > 0) {
            uint64_t avg = f.bytes / f.files;
            --f.files;
            f.bytes -= std::min(f.bytes, avg);
            if (d.files > 0) --d.files;
            d.bytes -= std::min(d.bytes, avg);
        }
        ++ch.removed;
    } else {
        ++ch.modified;
    }
    ++version_;
}

void CityData::watchDrive(int slot, std::string root) {
#ifdef _WIN32
    HANDLE h = CreateFileA(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        watchHandles_.push_back(h);
    }
    std::vector<char> buf(256 * 1024);
    while (!stop_) {
        DWORD got = 0;
        BOOL ok = ReadDirectoryChangesW(h, buf.data(), (DWORD)buf.size(), TRUE,
                                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                        &got, nullptr, nullptr);
        if (!ok) break;
        if (got == 0) continue;  // overflow
        char* p = buf.data();
        for (;;) {
            auto* fn = (FILE_NOTIFY_INFORMATION*)p;
            std::wstring rel(fn->FileName, fn->FileNameLength / sizeof(WCHAR));
            onChange(slot, rel, fn->Action);
            if (!fn->NextEntryOffset) break;
            p += fn->NextEntryOffset;
        }
    }
    CloseHandle(h);
#else
    (void)slot; (void)root;
#endif
}

bool CityData::snapshot(std::vector<CityDrive>& out, uint64_t& version, bool& ready) {
    ready = pending_ == 0 && started_;
    if (version_ == version) return false;
    std::lock_guard<std::mutex> lk(mu_);
    out = drives_;
    version = version_;
    return true;
}

void CityData::takeChanges(std::vector<CityChange>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : changes_) out.push_back(kv.second);
    changes_.clear();
}

void CityData::pollStats(double dt) {
#ifdef _WIN32
    statAccum_ += dt;
    if (statAccum_ < 0.5) return;
    double step = statAccum_;
    statAccum_ = 0;
    if (!pdh_) pdh_ = std::make_unique<PdhState>();
    auto& P = *pdh_;

    if (!statInit_) {
        statInit_ = true;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        stats_.cores = (int)std::clamp<DWORD>(si.dwNumberOfProcessors, 1, 64);
        stats_.coreLoad.assign(stats_.cores, 0.f);
        char brand[49] = {};
        int r[4];
        std::string name;
        for (unsigned i = 0x80000002; i <= 0x80000004; ++i) {
            __cpuid(r, (int)i);
            name.append((char*)r, 16);
        }
        name = name.c_str();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        (void)brand;
        stats_.cpuName = name.empty() ? "CPU" : name;

        // GPU via DXGI: pick the adapter with the most dedicated memory.
        IDXGIFactory1* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) {
            SIZE_T best = 0;
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d;
                a->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (d.DedicatedVideoMemory > best || stats_.gpuName.empty())) {
                    best = d.DedicatedVideoMemory;
                    stats_.vramTotal = d.DedicatedVideoMemory;
                    stats_.gpuName = narrow(d.Description);
                }
                a->Release();
            }
            fac->Release();
        }
        if (stats_.gpuName.empty()) stats_.gpuName = "GPU";

        // Per-core CPU load counters.
        if (PdhOpenQueryW(nullptr, 0, &P.cpuQ) == ERROR_SUCCESS) {
            for (int i = 0; i < stats_.cores; ++i) {
                std::wstring path = L"\\Processor(" + std::to_wstring(i) + L")\\% Processor Time";
                PDH_HCOUNTER c = nullptr;
                if (PdhAddEnglishCounterW(P.cpuQ, path.c_str(), 0, &c) != ERROR_SUCCESS) c = nullptr;
                P.cores.push_back(c);
            }
            PdhCollectQueryData(P.cpuQ);
        }
    }

    // Overall CPU from system times.
    {
        FILETIME idle, kern, user;
        if (GetSystemTimes(&idle, &kern, &user)) {
            auto u = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            unsigned long long i = u(idle), k = u(kern), us = u(user);
            if (P.lastKernel) {
                double tot = double(k - P.lastKernel) + double(us - P.lastUser);
                double busy = tot - double(i - P.lastIdle);
                if (tot > 0) stats_.cpuLoad = (float)std::clamp(busy / tot, 0.0, 1.0);
            }
            P.lastIdle = i; P.lastKernel = k; P.lastUser = us;
        }
    }
    if (P.cpuQ && PdhCollectQueryData(P.cpuQ) == ERROR_SUCCESS) {
        for (int i = 0; i < (int)P.cores.size(); ++i) {
            if (!P.cores[i]) continue;
            PDH_FMT_COUNTERVALUE v;
            if (PdhGetFormattedCounterValue(P.cores[i], PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
                stats_.coreLoad[i] = (float)std::clamp(v.doubleValue / 100.0, 0.0, 1.0);
        }
    }

    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        stats_.ramTotal = ms.ullTotalPhys;
        stats_.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
    }

    // GPU engine counters (wildcards expand when the query is created, so refresh it periodically).
    P.gpuAge += step;
    if (!P.gpuQ || P.gpuAge > 10.0) {
        if (P.gpuQ) { PdhCloseQuery(P.gpuQ); P.gpuQ = nullptr; P.gpuC = nullptr; }
        if (P.memQ) { PdhCloseQuery(P.memQ); P.memQ = nullptr; P.memC = nullptr; }
        P.gpuAge = 0;
        if (PdhOpenQueryW(nullptr, 0, &P.gpuQ) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(P.gpuQ, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &P.gpuC) != ERROR_SUCCESS)
                P.gpuC = nullptr;
            PdhCollectQueryData(P.gpuQ);
        }
        if (PdhOpenQueryW(nullptr, 0, &P.memQ) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(P.memQ, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &P.memC) != ERROR_SUCCESS)
                P.memC = nullptr;
            PdhCollectQueryData(P.memQ);
        }
    } else {
        auto sumArray = [](PDH_HCOUNTER c, double& total) {
            DWORD sz = 0, cnt = 0;
            PDH_STATUS s = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, nullptr);
            if (s != PDH_MORE_DATA || !sz) return false;
            std::vector<char> b(sz);
            auto* arr = (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data();
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &cnt, arr) != ERROR_SUCCESS) return false;
            total = 0;
            for (DWORD i = 0; i < cnt; ++i) total += arr[i].FmtValue.doubleValue;
            return true;
        };
        if (P.gpuC && PdhCollectQueryData(P.gpuQ) == ERROR_SUCCESS) {
            double t = 0;
            if (sumArray(P.gpuC, t)) stats_.gpuLoad = (float)std::clamp(t / 100.0, 0.0, 1.0);
        }
        if (P.memC && PdhCollectQueryData(P.memQ) == ERROR_SUCCESS && stats_.vramTotal) {
            double t = 0;
            if (sumArray(P.memC, t)) stats_.vramUsed = (float)std::clamp(t / double(stats_.vramTotal), 0.0, 1.0);
        }
    }
#else
    (void)dt;
#endif
}

}  // namespace sf
