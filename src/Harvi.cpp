#include "Harvi.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sf {

namespace {
constexpr float kTypeRate = 38.0f;  // characters per second
const Vec3 kBlue{0.10f, 0.55f, 1.0f};
const Vec3 kCyan{0.15f, 1.0f, 1.0f};

float approach(float cur, float target, float dt, float rate) { return cur + (target - cur) * std::min(1.0f, dt * rate); }
float smooth01(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// Rotation about Z (the fish's nose axis), built from the rotations the matrix class has.
Mat4 rotZ(float a) { return Mat4::rotateX(kPi * 0.5f) * Mat4::rotateY(a) * Mat4::rotateX(-kPi * 0.5f); }
}  // namespace

// ---- Scripted brain ---------------------------------------------------------------------------------------------------
ScriptedBrain::ScriptedBrain(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const Json j = Json::parse(ss.str());
    const Json& nodes = j["nodes"];
    for (auto& kv : nodes.o) {
        HarviReply r;
        r.text = kv.second["say"].str();
        r.emote = kv.second["emote"].str("neutral");
        const Json& bs = kv.second["buttons"];
        for (size_t i = 0; i < bs.size(); ++i) {
            HarviButton b;
            b.label = bs.at(i)["label"].str();
            b.action = bs.at(i)["action"].str();
            b.go = bs.at(i)["goto"].str();
            r.buttons.push_back(b);
        }
        nodes_[kv.first] = r;
    }
    const Json& tips = j["tips"];
    for (size_t i = 0; i < tips.size(); ++i) tips_.push_back(tips.at(i).str());
    if (!tips_.empty()) tipAt_ = static_cast<size_t>(std::rand()) % tips_.size();
}

bool ScriptedBrain::reply(const std::string& topic, HarviReply& out) {
    auto it = nodes_.find(topic);
    if (it == nodes_.end()) return false;
    out = it->second;
    return true;
}

std::string ScriptedBrain::tip() {
    if (tips_.empty()) return {};
    tipAt_ = (tipAt_ + 1) % tips_.size();
    return "Did you know? " + tips_[tipAt_];
}

// ---- Dialogue panel ---------------------------------------------------------------------------------------------------
HarviPanel::HarviPanel(Renderer& renderer) : PanelApp(renderer, 1100, 560, 1.1f) { title_ = "H.A.R.V.I"; }

void HarviPanel::setContent(const std::string& text, size_t shown, const std::vector<std::string>& labels, const std::string& subtitle) {
    if (text == text_ && shown == shown_ && labels == labels_ && subtitle == sub_) return;
    text_ = text;
    shown_ = shown;
    labels_ = labels;
    sub_ = subtitle;
    invalidate();
}

void HarviPanel::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    const Rect c = panel(bm, "H.A.R.V.I", sub_);
    const float pad = 30.0f, px = 31.0f;
    const auto lines = wrapText(text_, px, FontFace::UI, c.w() - pad * 2, 6);
    float y = c.y0 + pad + textAscent(px, FontFace::UI);
    size_t left = shown_;
    for (const std::string& ln : lines) {
        if (left == 0) break;
        const std::string part = ln.size() <= left ? ln : ln.substr(0, left);
        drawText(bm, c.x0 + pad, y, part, px, FontFace::UI, t.text);
        left -= std::min(left, ln.size() + 1);
        y += textLineHeight(px, FontFace::UI) * 1.12f;
    }
    rects_.clear();
    const int cols = 3;
    const int rows = (static_cast<int>(labels_.size()) + cols - 1) / cols;
    const float bh = 58.0f, gap = 12.0f;
    const float bw = (c.w() - pad * 2 - gap * (cols - 1)) / cols;
    const float y0 = c.y1 - pad - rows * bh - std::max(0, rows - 1) * gap;
    for (size_t i = 0; i < labels_.size(); ++i) {
        const float x = c.x0 + pad + (i % cols) * (bw + gap), yy = y0 + (i / cols) * (bh + gap);
        const Rect r{x, yy, x + bw, yy + bh};
        rects_.push_back(r);
        const bool danger = labels_[i] == "DISMISS";
        button(bm, r, labels_[i], static_cast<int>(i) == hover_ ? State::Hover : State::Normal, !danger && i == 0, 24.0f);
    }
}

bool HarviPanel::onHover(float x, float y) {
    int h = -1;
    for (size_t i = 0; i < rects_.size(); ++i)
        if (rects_[i].contains(x, y)) h = static_cast<int>(i);
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void HarviPanel::onClick(float x, float y, bool) {
    for (size_t i = 0; i < rects_.size(); ++i)
        if (rects_[i].contains(x, y)) {
            choice_ = static_cast<int>(i);
            return;
        }
}

// ---- Avatar -----------------------------------------------------------------------------------------------------------
Harvi::Harvi(Renderer& renderer) : renderer_(renderer) {
    panel_ = std::make_unique<HarviPanel>(renderer);
    const char* base = SDL_GetBasePath();
    brains_.push_back(std::make_unique<LlmBrain>());
    brains_.push_back(std::make_unique<ScriptedBrain>(std::string(base ? base : "./") + "assets/harvi/dialogue.json"));
}

Harvi::Emo Harvi::parseEmo(const std::string& n) {
    if (n == "happy") return Emo::Happy;
    if (n == "confused") return Emo::Confused;
    if (n == "thinking") return Emo::Thinking;
    if (n == "surprised") return Emo::Surprised;
    if (n == "sad") return Emo::Sad;
    return Emo::Neutral;
}

void Harvi::emote(const std::string& name, float seconds) {
    emo_ = parseEmo(name);
    emoT_ = emo_ == Emo::Neutral ? 0.0f : seconds;
}

void Harvi::gesture(const std::string& name) {
    if (name == "spin") spinT_ = 0;
    else if (name == "hop") hopT_ = 0;
    else if (name == "nod") nodT_ = 0;
    else if (name == "shake") shakeT_ = 0;
    else if (name == "wave") waveT_ = 0;
}

void Harvi::look(float x, float y, float seconds) {
    lookOvX_ = clampf(x, -1, 1);
    lookOvY_ = clampf(y, -1, 1);
    lookOvT_ = seconds;
}

void Harvi::toggle(Vec3 eye, Vec3 forward, bool vr) {
    if (state_ == State::On || state_ == State::Appearing) dismiss();
    else summon(eye, forward, vr);
}

void Harvi::summon(Vec3 eye, Vec3 forward, bool vr) {
    if (state_ == State::On || state_ == State::Appearing) return;
    eyeNow_ = eye;
    fwdNow_ = forward;
    vr_ = vr;
    placed_ = false;
    state_ = State::Appearing;
    parts_.clear();
    spinT_ = 0;  // a little twirl as he materialises
    const std::string ctxNode = "ctx_" + ctx_;
    bool known = false;
    for (auto& b : brains_) known = known || (!ctx_.empty() && b->hasNode(ctxNode));
    if (known) {
        ctxSeen_[ctx_] = true;
        showNode(ctxNode);
    } else {
        showNode("root");
    }
}

void Harvi::dismiss() {
    if (state_ == State::Off || state_ == State::Leaving) return;
    state_ = State::Leaving;
    panel_->close();
    emote("happy", 1.0f);
    talking_ = false;
}

void Harvi::setContext(const std::string& ctx) {
    if (ctx == ctx_) return;
    ctx_ = ctx;
    if (state_ != State::On || ctx.empty() || ctxSeen_[ctx]) return;
    const std::string id = "ctx_" + ctx;
    for (auto& b : brains_)
        if (b->hasNode(id)) {
            ctxSeen_[ctx] = true;
            showNode(id);
            return;
        }
}

void Harvi::showNode(const std::string& id) {
    HarviReply r;
    for (auto& b : brains_)
        if (b->reply(id, r)) {
            node_ = id;
            present(r);
            return;
        }
    say("I do not have an answer for that yet.", "confused");
}

void Harvi::say(const std::string& text, const std::string& emo) {
    if (state_ == State::Off || state_ == State::Leaving) return;
    HarviReply r;
    r.text = text;
    r.emote = emo;
    present(r);
}

void Harvi::present(const HarviReply& r) {
    cur_ = r;
    bool menu = false, dis = false;
    for (const auto& b : cur_.buttons) {
        menu = menu || b.go == "root";
        dis = dis || b.action == "dismiss";
    }
    if (!menu && node_ != "root") cur_.buttons.push_back({"MENU", "", "root"});
    if (!dis) cur_.buttons.push_back({"DISMISS", "dismiss", ""});
    revealed_ = 0;
    talking_ = !cur_.text.empty();
    lastShown_ = size_t(-1);
    emote(r.emote, static_cast<float>(r.text.size()) / kTypeRate + 2.0f);
    if (r.emote == "happy" || r.emote == "surprised") hopT_ = 0;
    if (r.emote == "confused") shakeT_ = 0;
    refreshPanel();
}

void Harvi::refreshPanel() {
    std::vector<std::string> labels;
    for (const auto& b : cur_.buttons) labels.push_back(b.label);
    panel_->setContent(cur_.text, std::min(cur_.text.size(), static_cast<size_t>(revealed_)), labels,
                       ctx_.empty() ? std::string("SCRIPTED") : "SCRIPTED  |  " + ctx_);
}

void Harvi::choose(int i) {
    if (i < 0 || i >= static_cast<int>(cur_.buttons.size())) return;
    const HarviButton b = cur_.buttons[static_cast<size_t>(i)];
    if (b.action == "dismiss") {
        dismiss();
        return;
    }
    if (b.action == "tip") {
        for (auto& br : brains_) {
            const std::string t = br->tip();
            if (!t.empty()) {
                say(t, "happy");
                return;
            }
        }
        return;
    }
    if (b.action.rfind("emote:", 0) == 0) emote(b.action.substr(6));
    else if (!b.action.empty()) action_ = b.action;
    if (!b.go.empty()) showNode(b.go);
    else emote("happy", 1.5f);
}

void Harvi::emit(const Vec3& at, float dt) {
    emitAcc_ += dt;
    while (emitAcc_ > 0.045f) {
        emitAcc_ -= 0.045f;
        Particle p;
        const float r1 = std::rand() / float(RAND_MAX) - 0.5f, r2 = std::rand() / float(RAND_MAX) - 0.5f;
        p.p = at + Vec3{r1 * 0.04f, r2 * 0.04f, 0};
        p.v = {r1 * 0.05f, 0.04f + r2 * 0.03f, 0};
        p.life = 1.0f;
        parts_.push_back(p);
    }
}

void Harvi::update(double dtd, Vec3 eye, Vec3 forward, bool vr) {
    if (state_ == State::Off) return;
    const float dt = static_cast<float>(dtd);
    time_ += dt;
    eyeNow_ = eye;
    fwdNow_ = forward;
    vr_ = vr;

    if (state_ == State::Appearing) {
        emerge_ = std::min(1.0f, emerge_ + dt / 0.9f);
        if (emerge_ > 0.35f && !panel_->isOpen()) panel_->open(eye, forward);
        if (emerge_ >= 1.0f) state_ = State::On;
    } else if (state_ == State::Leaving) {
        emerge_ = std::max(0.0f, emerge_ - dt / 0.7f);
        if (emerge_ <= 0.0f) {
            state_ = State::Off;
            parts_.clear();
            return;
        }
    } else if (!panel_->isOpen()) {
        dismiss();  // the window's own close button
    }

    // Placement: lower right of the view, eased so he follows without snapping. The avatar sits left of the dialogue panel.
    {
        Vec3 f = {fwdNow_.x, 0, fwdNow_.z};
        f = normalize(length(f) < 0.2f ? Vec3{0, 0, -1} : f);
        const Vec3 right = normalize(cross(f, {0, 1, 0}));
        const float dist = vr_ ? 1.15f : 1.55f, side = vr_ ? 0.32f : 0.6f, down = vr_ ? 0.22f : 0.3f;
        const Vec3 target = eye + f * dist + right * side + Vec3{0, -down + (fwdNow_.y * dist * 0.5f), 0};
        if (!placed_) {
            pos_ = target;
            placed_ = true;
        } else if (!hold_) {
            // Stay put until the view has moved well away, then glide over and settle.
            const float away = length(target - pos_);
            if (away > (vr_ ? 0.9f : 1.6f)) following_ = true;
            if (following_) {
                pos_ = pos_ + (target - pos_) * std::min(1.0f, dt * 2.5f);
                if (away < 0.05f) following_ = false;
            }
        }
        yaw_ = std::atan2(eye.x - pos_.x, eye.z - pos_.z);
        if (panel_->isOpen()) panel_->setPlacement(Mat4::translate(pos_) * Mat4::rotateY(yaw_));
    }

    // Dialogue typing.
    if (talking_) {
        revealed_ += kTypeRate * dt;
        if (revealed_ >= static_cast<float>(cur_.text.size())) {
            revealed_ = static_cast<float>(cur_.text.size());
            talking_ = false;
        }
    }
    const size_t shown = static_cast<size_t>(revealed_);
    if (shown != lastShown_) {
        lastShown_ = shown;
        refreshPanel();
    }
    if (panel_->isOpen()) {
        const int c = panel_->takeChoice();
        if (c >= 0) choose(c);
    }

    if (emoT_ > 0 && !talking_) {
        emoT_ -= dt;
        if (emoT_ <= 0) emo_ = Emo::Neutral;
    }
    if (lookOvT_ > 0) lookOvT_ -= dt;
    for (float* g : {&spinT_, &hopT_, &nodT_, &shakeT_, &waveT_})
        if (*g >= 0) {
            *g += dt;
            const float len = (g == &spinT_) ? 1.1f : (g == &hopT_ ? 0.7f : (g == &waveT_ ? 1.8f : 0.9f));
            if (*g > len) *g = -1;
        }

    // Pose targets per emotion.
    float droop = 0.2f, gill = 0.1f, lid = 0, roll = 0, finAmp = 0.3f, finFreq = 3, tailAmp = 0.25f, tailFreq = 3, bob = 0.02f, dots = 0, tilt = 0;
    float lx = 0, ly = 0;
    switch (emo_) {
        case Emo::Happy: droop = -0.1f; gill = 0.2f; lid = 0.35f; finAmp = 0.6f; finFreq = 9; tailAmp = 0.5f; tailFreq = 8; bob = 0.05f; break;
        case Emo::Confused: droop = 0.5f; gill = 0.8f; roll = 0.35f; finAmp = 0.15f; finFreq = 2; tailAmp = 0.1f; lx = std::sin(time_ * 2.4f) * 0.8f; break;
        case Emo::Thinking: gill = 0.3f; roll = -0.15f; finAmp = 0.1f; finFreq = 2; tailAmp = 0.1f; dots = 1; lx = 0.5f; ly = 0.7f; break;
        case Emo::Surprised: droop = -0.5f; gill = 1.0f; finAmp = 0.8f; finFreq = 12; bob = 0.0f; ly = 0.2f; break;
        case Emo::Sad: droop = 0.8f; lid = 0.45f; finAmp = 0.05f; tailAmp = 0.08f; bob = 0.01f; tilt = 0.3f; ly = -0.6f; break;
        default: break;
    }
    if (talking_) {
        finAmp = std::max(finAmp, 0.4f);
        finFreq = std::max(finFreq, 5.0f);
        gill = std::max(gill, 0.35f);
    }
    if (waveT_ >= 0) {
        finAmp = 0.9f;
        finFreq = 10;
        droop = -0.2f;
    }
    if (lookOvT_ > 0) {
        lx = lookOvX_;
        ly = lookOvY_;
    }
    p_.droop = approach(p_.droop, droop, dt, 6);
    p_.gill = approach(p_.gill, gill, dt, 6);
    p_.lid = approach(p_.lid, lid, dt, 8);
    p_.roll = approach(p_.roll, roll, dt, 4);
    p_.tilt = approach(p_.tilt, tilt, dt, 4);
    p_.finAmp = approach(p_.finAmp, finAmp, dt, 5);
    p_.finFreq = approach(p_.finFreq, finFreq, dt, 5);
    p_.tailAmp = approach(p_.tailAmp, tailAmp, dt, 5);
    p_.tailFreq = approach(p_.tailFreq, tailFreq, dt, 5);
    p_.bobAmp = approach(p_.bobAmp, bob, dt, 5);
    p_.dots = approach(p_.dots, dots, dt, 6);
    p_.lookX = approach(p_.lookX, lx, dt, 9);
    p_.lookY = approach(p_.lookY, ly, dt, 9);

    // Lip-sync from the character being spoken: vowels open wide, closed-lip consonants shut, the rest half open.
    float jawT = emo_ == Emo::Surprised ? 0.55f : 0.0f;
    if (talking_ && shown < cur_.text.size()) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(cur_.text[shown])));
        if (std::string("aeiou").find(c) != std::string::npos) jawT = (c == 'a' || c == 'o') ? 1.0f : 0.75f;
        else if (std::string("mbp").find(c) != std::string::npos || c == ' ' || c == '.' || c == ',') jawT = 0.0f;
        else if (std::isalpha(static_cast<unsigned char>(c))) jawT = 0.4f;
    }
    p_.jaw = approach(p_.jaw, jawT, dt, 22);

    blinkT_ -= dt;
    if (blinkPhase_ >= 0) {
        blinkPhase_ += dt / 0.18f;
        if (blinkPhase_ > 1) blinkPhase_ = -1;
    } else if (blinkT_ <= 0) {
        blinkPhase_ = 0;
        blinkT_ = 1.5f + 3.0f * (std::rand() / float(RAND_MAX));
    }

    // Trail from the tail.
    emit(Mat4::translate(pos_).transformPoint(Mat4::rotateY(yaw_).transformDir({-1.45f, 0.02f, 0.18f})), dt * (0.6f + emerge_));
    for (auto& p : parts_) {
        p.p = p.p + p.v * dt;
        p.life -= dt * 0.9f;
    }
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const Particle& p) { return p.life <= 0; }), parts_.end());
    if (parts_.size() > 60) parts_.erase(parts_.begin(), parts_.begin() + (parts_.size() - 60));
    panel_->update(dtd);
}

void Harvi::render(const GpuLabel& any) {
    if (state_ == State::Off) return;
    if (panel_->isOpen()) panel_->render();
    renderer_.clearDepth();
    const float t = time_;
    const float e = emerge_;
    auto holo = [&](bool pyr, const Mat4& m, Vec3 col, float a, float glow = 0.0f) { renderer_.drawHolo(any, pyr, m, col, e, a, glow); };

    for (const auto& p : parts_) holo(false, Mat4::translate(p.p) * Mat4::scale({0.012f, 0.012f, 0.012f}), kCyan, p.life * 1.3f, 0.3f);

    float yaw = -kPi * 0.5f - 0.2f - (talking_ ? 0.25f : 0.0f);
    yaw += std::sin(t * 2.0f) * 0.06f;
    if (spinT_ >= 0) yaw += 2 * kPi * smooth01(spinT_ / 1.1f);
    if (shakeT_ >= 0) yaw += std::sin(shakeT_ * 22.0f) * 0.3f * (1.0f - shakeT_ / 0.9f);
    float pitch = -p_.tilt;
    if (nodT_ >= 0) pitch += std::sin(nodT_ * 14.0f) * 0.3f * (1.0f - nodT_ / 0.9f);
    const float hop = hopT_ >= 0 ? std::sin(kPi * hopT_ / 0.7f) * 0.22f : 0.0f;
    const float bobY = std::sin(t * 1.7f) * p_.bobAmp + hop;
    const Vec3 wander{std::sin(t * 0.6f) * 0.05f, std::sin(t * 0.9f) * 0.03f, std::sin(t * 0.45f) * 0.06f};
    const Mat4 base = Mat4::translate(pos_) * Mat4::rotateY(yaw_);
    const float S = 0.75f;
    const Vec3 anchor{-0.8f, 0.02f, 0.18f};
    const Mat4 root = base * Mat4::translate(anchor + wander + Vec3{0, bobY, 0}) * Mat4::rotateY(yaw) * rotZ(p_.roll) * Mat4::rotateX(pitch) *
                      Mat4::scale({S, S, S});

    // Body: two angular halves meeting at the widest point (pyramids point down -Z from their apex).
    holo(true, root * Mat4::translate({0, 0, -0.30f}) * Mat4::rotateY(kPi) *     Mat4::scale({0.28f, 0.44f, 0.30f}), kBlue, 1.0f);
        holo(true, root * Mat4::translate({0, 0, 0.55f}) * Mat4::scale({0.28f, 0.44f, 0.55f}), kBlue, 1.0f);
    holo(false, root * Mat4::translate({0, 0.17f, 0.12f}) * Mat4::scale({0.025f, 0.02f, 0.4f}), kCyan, 1.0f, 0.4f);

    // Jaw: upper and lower lip hinge at the front of the head and open with p_.jaw.
    holo(true, root * Mat4::translate({0, 0.02f, -0.30f}) * Mat4::rotateX(p_.jaw * 0.12f) * Mat4::translate({0, 0, -0.30f}) * Mat4::rotateY(kPi) *
                   Mat4::scale({0.20f, 0.05f, 0.30f}),
         kCyan, 0.9f, 0.1f);
    holo(true, root * Mat4::translate({0, -0.04f, -0.30f}) * Mat4::rotateX(-p_.jaw * 0.55f) * Mat4::translate({0, 0, -0.27f}) * Mat4::rotateY(kPi) *
                   Mat4::scale({0.17f, 0.04f, 0.27f}),
         kCyan, 0.9f, 0.1f + p_.jaw * 0.3f);

    // Tail: hinge at the rear, swings with a wiggle.
    const float tailSw = std::sin(t * p_.tailFreq) * p_.tailAmp + (talking_ ? std::sin(t * 9.0f) * 0.1f : 0.0f);
    holo(true, root * Mat4::translate({0, 0, 0.5f}) * Mat4::rotateY(tailSw) * Mat4::rotateY(kPi) * Mat4::scale({0.03f, 0.40f, 0.38f}), kCyan, 0.7f, 0.15f);
    holo(true, root * Mat4::translate({0, 0, 0.5f}) * Mat4::rotateY(tailSw * 1.3f) * Mat4::rotateY(kPi) * Mat4::scale({0.03f, 0.2f, 0.55f}), kBlue, 0.6f, 0.1f);

    // Dorsal fin: swept back, ripples.
    const float dors = std::sin(t * p_.finFreq * 0.8f) * p_.finAmp * 0.4f;
    holo(true, root * Mat4::translate({0, 0.20f, -0.10f}) * rotZ(dors) * Mat4::rotateX(kPi * 0.5f + 0.55f) * Mat4::scale({0.03f, 0.26f, 0.30f}), kCyan, 0.65f, 0.15f);

    // Pectoral fins: flap about their root; they droop or lift with the mood.
    const float flap = std::sin(t * p_.finFreq) * p_.finAmp * (waveT_ >= 0 ? 1.4f : 1.0f);
    for (int side = -1; side <= 1; side += 2) {
        const float sgn = static_cast<float>(side);
        const float ang = -(p_.droop + flap * 0.5f) * sgn;
        const float sweep = (kPi * 0.5f + 0.6f) * -sgn;
        holo(true, root * Mat4::translate({sgn * 0.17f, -0.10f, -0.03f}) * rotZ(ang) * Mat4::rotateY(sweep) * Mat4::scale({0.18f, 0.03f, 0.26f}), kCyan, 0.65f, 0.15f);
    }

    // Gills: slats on each side that flare open.
    for (int side = -1; side <= 1; side += 2) {
        const float sgn = static_cast<float>(side);
        for (int i = 0; i < 3; ++i) {
            const float flare = (p_.gill * 0.9f + 0.1f * std::sin(t * 2.5f + i)) * sgn;
            holo(false, root * Mat4::translate({sgn * 0.19f, 0.0f, -0.09f + i * 0.045f}) * Mat4::rotateY(flare) * Mat4::scale({0.012f, 0.20f, 0.025f}), kCyan, 1.0f,
                 0.3f + p_.gill * 0.5f);
        }
    }

    // Eyes: a ball, a pupil that looks around and an eyelid that blinks or squints.
    const float blink = blinkPhase_ >= 0 ? std::sin(kPi * blinkPhase_) : 0.0f;
    const float cover = std::max(blink, p_.lid);
    for (int side = -1; side <= 1; side += 2) {
        const float sgn = static_cast<float>(side);
        const Vec3 ec{sgn * 0.11f, 0.07f, -0.15f};
        holo(false, root * Mat4::translate(ec) * Mat4::scale({0.09f, 0.09f, 0.09f}), {0.6f, 0.9f, 1.0f}, 0.8f, 0.2f);
        holo(false, root * Mat4::translate({ec.x + sgn * 0.045f, ec.y + p_.lookY * 0.02f, ec.z - p_.lookX * 0.022f}) * Mat4::scale({0.035f, 0.04f, 0.04f}),
             {0.9f, 1.0f, 1.0f}, 1.0f, 1.2f);
        if (cover > 0.03f) {
            const float h = 0.10f * cover;
            holo(false, root * Mat4::translate({ec.x + sgn * 0.003f, ec.y + 0.05f - h * 0.5f, ec.z}) * Mat4::scale({0.098f, h, 0.098f}), kBlue, 1.0f, 0.2f);
        }
    }

    // Thinking dots above the head.
    if (p_.dots > 0.05f)
        for (int i = 0; i < 3; ++i) {
            const float k = 0.5f + 0.5f * std::sin(t * 5.0f - i * 1.1f);
            holo(false, root * Mat4::translate({(i - 1) * 0.16f, 0.52f + 0.04f * k, -0.2f}) * Mat4::scale(Vec3{0.05f, 0.05f, 0.05f} * (0.6f + 0.6f * k)), kCyan, p_.dots, 1.0f);
        }
}

}  // namespace sf
