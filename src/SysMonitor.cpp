#include "SysMonitor.h"

#include <windows.h>

#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>

#include "OsSettings.h"

namespace sf {

namespace {
constexpr size_t kHist = 120;
void push(std::deque<float>& d, float v) {
    d.push_back(std::clamp(v, 0.0f, 1.0f));
    while (d.size() > kHist) d.pop_front();
}
std::vector<float> vec(const std::deque<float>& d) { return {d.begin(), d.end()}; }
std::string pct(float v) {
    char b[16];
    std::snprintf(b, sizeof b, "%d%%", static_cast<int>(v * 100 + 0.5f));
    return b;
}
std::string rate(double bps) {
    char b[32];
    if (bps >= 1048576.0) std::snprintf(b, sizeof b, "%.1f MB/s", bps / 1048576.0);
    else std::snprintf(b, sizeof b, "%.0f KB/s", bps / 1024.0);
    return b;
}
unsigned long long ft(const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
}  // namespace

SysMonitor::SysMonitor(Renderer& renderer, std::function<SystemStats()> stats)
    : PanelApp(renderer, 1600, 900, 2.2f), stats_(std::move(stats)) {
    title_ = "SYSTEM MONITOR";
}

void SysMonitor::sampleProcesses(double wall) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    std::vector<Proc> out;
    std::map<unsigned long, unsigned long long> times;
    const int cores = std::max(1, cur_.cores);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            FILETIME c, e, k, u;
            Proc p;
            p.pid = pe.th32ProcessID;
            int n = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
            p.name.resize(n > 0 ? n - 1 : 0);
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, p.name.data(), n, nullptr, nullptr);
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                const unsigned long long t = ft(k) + ft(u);
                times[p.pid] = t;
                auto it = prevTimes_.find(p.pid);
                if (it != prevTimes_.end() && wall > 0 && t >= it->second)
                    p.cpu = static_cast<float>(static_cast<double>(t - it->second) / 1e7 / wall / cores);
            }
            PROCESS_MEMORY_COUNTERS pmc{};
            if (GetProcessMemoryInfo(h, &pmc, sizeof pmc)) p.mem = pmc.WorkingSetSize;
            CloseHandle(h);
            out.push_back(std::move(p));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    prevTimes_.swap(times);
    std::sort(out.begin(), out.end(), [&](const Proc& a, const Proc& b) {
        if (focus_ == kMemory) return a.mem > b.mem;
        return a.cpu != b.cpu ? a.cpu > b.cpu : a.mem > b.mem;
    });
    if (out.size() > 14) out.resize(14);
    procs_ = std::move(out);
}

void SysMonitor::sampleNetwork(double wall) {
    MIB_IFTABLE* t = nullptr;
    ULONG size = 0;
    GetIfTable(nullptr, &size, FALSE);
    std::vector<char> buf(size);
    t = reinterpret_cast<MIB_IFTABLE*>(buf.data());
    if (GetIfTable(t, &size, FALSE) != NO_ERROR) return;
    unsigned long long in = 0, out = 0;
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_IFROW& r = t->table[i];
        if (r.dwType == IF_TYPE_SOFTWARE_LOOPBACK || r.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
        in += r.dwInOctets;
        out += r.dwOutOctets;
    }
    if (netInit_ && wall > 0) {
        rxRate_ = static_cast<double>(in >= lastIn_ ? in - lastIn_ : 0) / wall;
        txRate_ = static_cast<double>(out >= lastOut_ ? out - lastOut_ : 0) / wall;
        push(rxH_, static_cast<float>(rxRate_ / 12.5e6));  // scaled to 100 Mbit/s
        push(txH_, static_cast<float>(txRate_ / 12.5e6));
    }
    lastIn_ = in;
    lastOut_ = out;
    netInit_ = true;
}

void SysMonitor::tick(double dt) {
    sampleT_ += dt;
    procT_ += dt;
    netT_ += dt;
    driveT_ += dt;
    if (sampleT_ >= 0.5) {
        sampleT_ = 0;
        cur_ = stats_();
        push(cpuH_, cur_.cpuLoad);
        push(ramH_, cur_.ramTotal ? static_cast<float>(cur_.ramUsed) / cur_.ramTotal : 0.0f);
        push(gpuH_, std::max(0.0f, cur_.gpuLoad));
        push(vramH_, std::max(0.0f, cur_.vramUsed));
    }
    if (procT_ >= 1.5) {
        sampleProcesses(procT_);
        procT_ = 0;
    }
    if (netT_ >= 1.0) {
        sampleNetwork(netT_);
        netT_ = 0;
    }
    if (driveT_ >= 15.0) {
        drives_ = os::drives();
        driveT_ = 0;
    }
}

bool SysMonitor::onHover(float x, float y) {
    int h = -1;
    for (int i = 0; i < 3; ++i)
        if (cardRect_[i].contains(x, y)) h = i;
    if (h == hoverCard_) return false;
    hoverCard_ = h;
    return true;
}

void SysMonitor::onClick(float x, float y, bool) {
    for (int i = 0; i < 3; ++i)
        if (cardRect_[i].contains(x, y)) {
            focus_ = i;
            procT_ = 99;
        }
}

void SysMonitor::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    Rect c = panel(bm, "SYSTEM MONITOR", cur_.cpuName.empty() ? "LIVE" : cur_.cpuName);
    const float pad = 24;
    const float leftW = 440;

    struct Card {
        const char* name;
        std::string value, sub;
        const std::deque<float>* hist;
        Color8 col;
    };
    const float ramF = cur_.ramTotal ? static_cast<float>(cur_.ramUsed) / cur_.ramTotal : 0.0f;
    Card cards[3] = {
        {"CPU", pct(cur_.cpuLoad), std::to_string(cur_.cores) + " threads", &cpuH_, t.accent},
        {"MEMORY", pct(ramF), bytesText(cur_.ramUsed) + " / " + bytesText(cur_.ramTotal), &ramH_, t.alt},
        {"GPU", cur_.gpuLoad >= 0 ? pct(cur_.gpuLoad) : "n/a", cur_.gpuName, &gpuH_, t.success},
    };
    const float ch = (c.h() - pad * 4) / 3;
    for (int i = 0; i < 3; ++i) {
        Rect r{c.x0 + pad, c.y0 + pad + i * (ch + pad), c.x0 + pad + leftW, c.y0 + pad + i * (ch + pad) + ch};
        cardRect_[i] = r;
        card(bm, r, i == focus_ ? State::Selected : (i == hoverCard_ ? State::Hover : State::Normal));
        label(bm, r.x0 + 18, r.y0 + 34, cards[i].name, 24, FontFace::UIBold, t.dim);
        labelRight(bm, r.x1 - 18, r.y0 + 40, cards[i].value, 40, FontFace::UIBold, cards[i].col);
        Rect g{r.x0 + 18, r.y0 + 56, r.x1 - 18, r.y1 - 34};
        graph(bm, g, vec(*cards[i].hist), cards[i].col);
        label(bm, r.x0 + 18, r.y1 - 10, ellipsize(cards[i].sub, 22, FontFace::UI, r.w() - 36), 22, FontFace::UI, t.muted);
    }

    const float rx = c.x0 + pad * 2 + leftW;
    const float rw = c.x1 - pad - rx;
    // Detail: per core bars (CPU), VRAM/RAM text otherwise.
    Rect d{rx, c.y0 + pad, rx + rw, c.y0 + pad + 250};
    card(bm, d, State::Normal);
    if (focus_ == kCpu) {
        label(bm, d.x0 + 18, d.y0 + 34, "PER THREAD LOAD", 24, FontFace::UIBold, t.dim);
        const int n = static_cast<int>(cur_.coreLoad.size());
        const int cols = std::max(1, std::min(n, 16));
        const int rows = n ? (n + cols - 1) / cols : 1;
        const float bw = (d.w() - 36 - (cols - 1) * 6) / cols, bh = (d.h() - 70) / rows;
        for (int i = 0; i < n; ++i) {
            const float x = d.x0 + 18 + (i % cols) * (bw + 6), y = d.y0 + 54 + (i / cols) * bh;
            fillRect(bm, x, y, x + bw, y + bh - 6, withAlpha(t.border, 90));
            const float v = std::clamp(cur_.coreLoad[i], 0.0f, 1.0f);
            fillRect(bm, x, y + (bh - 6) * (1 - v), x + bw, y + bh - 6, t.accent);
        }
    } else if (focus_ == kMemory) {
        label(bm, d.x0 + 18, d.y0 + 34, "MEMORY", 24, FontFace::UIBold, t.dim);
        label(bm, d.x0 + 18, d.y0 + 96, bytesText(cur_.ramUsed) + " in use", 44, FontFace::UIBold, t.text);
        label(bm, d.x0 + 18, d.y0 + 140, bytesText(cur_.ramTotal - std::min(cur_.ramTotal, cur_.ramUsed)) + " available of " + bytesText(cur_.ramTotal), 26, FontFace::UI, t.dim);
        bar(bm, {d.x0 + 18, d.y0 + 170, d.x1 - 18, d.y0 + 196}, ramF, t.alt);
    } else {
        label(bm, d.x0 + 18, d.y0 + 34, "GPU", 24, FontFace::UIBold, t.dim);
        label(bm, d.x0 + 18, d.y0 + 90, ellipsize(cur_.gpuName, 38, FontFace::UIBold, d.w() - 36), 38, FontFace::UIBold, t.text);
        const float v = std::max(0.0f, cur_.vramUsed);
        label(bm, d.x0 + 18, d.y0 + 140, cur_.vramUsed >= 0 ? "VRAM " + pct(v) + " of " + bytesText(cur_.vramTotal) : "VRAM n/a", 26, FontFace::UI, t.dim);
        bar(bm, {d.x0 + 18, d.y0 + 170, d.x1 - 18, d.y0 + 196}, v, t.success);
        graph(bm, {d.x0 + 18, d.y0 + 205, d.x1 - 18, d.y1 - 8}, vec(vramH_), t.success);
    }

    // Processes
    const float py0 = d.y1 + pad;
    Rect pr{rx, py0, rx + rw * 0.6f - pad / 2, c.y1 - pad};
    card(bm, pr, State::Normal);
    label(bm, pr.x0 + 18, pr.y0 + 34, focus_ == kMemory ? "TOP PROCESSES BY MEMORY" : "TOP PROCESSES", 24, FontFace::UIBold, t.dim);
    labelRight(bm, pr.x1 - 150, pr.y0 + 34, "CPU", 22, FontFace::UI, t.muted);
    labelRight(bm, pr.x1 - 18, pr.y0 + 34, "MEMORY", 22, FontFace::UI, t.muted);
    float y = pr.y0 + 66;
    const float rowH = std::min(34.0f, (pr.y1 - y - 6) / std::max<size_t>(1, procs_.size()));
    for (const Proc& p : procs_) {
        label(bm, pr.x0 + 18, y + rowH * 0.7f, ellipsize(p.name, 24, FontFace::UI, pr.w() - 260), 24, FontFace::UI, t.text);
        labelRight(bm, pr.x1 - 150, y + rowH * 0.7f, pct(std::min(1.0f, p.cpu)), 24, FontFace::Mono, p.cpu > 0.2f ? t.warn : t.dim);
        labelRight(bm, pr.x1 - 18, y + rowH * 0.7f, bytesText(p.mem), 24, FontFace::Mono, t.dim);
        y += rowH;
    }

    // Network + disks
    Rect nr{pr.x1 + pad, py0, rx + rw, py0 + (c.y1 - pad - py0) * 0.5f - pad / 2};
    card(bm, nr, State::Normal);
    label(bm, nr.x0 + 18, nr.y0 + 34, "NETWORK", 24, FontFace::UIBold, t.dim);
    label(bm, nr.x0 + 18, nr.y0 + 70, "DOWN " + rate(rxRate_), 24, FontFace::UI, t.accent);
    labelRight(bm, nr.x1 - 18, nr.y0 + 70, "UP " + rate(txRate_), 24, FontFace::UI, t.alt);
    graph(bm, {nr.x0 + 18, nr.y0 + 84, nr.x1 - 18, nr.y1 - 12}, vec(rxH_), t.accent);
    Rect dr{pr.x1 + pad, nr.y1 + pad, rx + rw, c.y1 - pad};
    card(bm, dr, State::Normal);
    label(bm, dr.x0 + 18, dr.y0 + 34, "DRIVES", 24, FontFace::UIBold, t.dim);
    y = dr.y0 + 68;
    for (const auto& [n, info] : drives_) {
        if (y > dr.y1 - 10) break;
        label(bm, dr.x0 + 18, y, n, 24, FontFace::UIBold, t.text);
        label(bm, dr.x0 + 80, y, ellipsize(info, 22, FontFace::UI, dr.w() - 100), 22, FontFace::UI, t.dim);
        y += 32;
    }
}

}  // namespace sf
