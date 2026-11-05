#pragma once
// H.A.R.V.I - Holographic Autonomous Response & Virtual Intelligence: the shell's built-in assistant.
// A blue angular holographic fish (articulated jaw / lips, eyes with lids, gills, pectoral, dorsal and tail fins) that floats
// beside a dialogue panel. Everything he does is scriptable through say() / emote() / gesture() / look(), and what he says
// comes from a brain (scripted JSON dialogue trees today; the interface also fits a local or remote LLM).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MiniJson.h"
#include "PanelApp.h"
#include "Renderer.h"
#include "Utils.h"

namespace sf {

struct HarviButton {
    std::string label, action, go;  // action: dismiss | tip | emote:NAME | tool:NAME | page:NAME | key:NAME ; go: next node id
};
struct HarviReply {
    std::string text, emote = "neutral";
    std::vector<HarviButton> buttons;
};

// Where HARVI's words come from. Brains are asked in order; the first one that answers wins.
class HarviBrain {
public:
    virtual ~HarviBrain() = default;
    virtual const char* name() const = 0;
    virtual bool reply(const std::string& topic, HarviReply& out) = 0;  // topic: a node id, or free text for language models
    virtual std::string tip() { return {}; }
    virtual bool hasNode(const std::string&) const { return false; }
};

// Scripted dialogue trees (assets/harvi/dialogue.json): nodes with text, emotion and buttons, plus a pool of tips.
class ScriptedBrain : public HarviBrain {
public:
    explicit ScriptedBrain(const std::string& path);
    const char* name() const override { return "SCRIPTED"; }
    bool reply(const std::string& topic, HarviReply& out) override;
    std::string tip() override;
    bool hasNode(const std::string& id) const override { return nodes_.count(id) != 0; }
    bool loaded() const { return !nodes_.empty(); }

private:
    std::map<std::string, HarviReply> nodes_;
    std::vector<std::string> tips_;
    size_t tipAt_ = 0;
};

// Placeholder for a local or remote language model. It declines every request until an endpoint is configured, so the scripted
// brain below it keeps answering; wire a real client into reply() to give HARVI free-form conversation.
class LlmBrain : public HarviBrain {
public:
    const char* name() const override { return "LLM (NOT CONFIGURED)"; }
    bool reply(const std::string&, HarviReply&) override { return false; }
};

// The floating dialogue window next to the avatar: typed-out text and a grid of buttons.
class HarviPanel : public PanelApp {
public:
    explicit HarviPanel(Renderer& renderer);
    const char* name() const override { return "HARVI"; }
    void setContent(const std::string& text, size_t shown, const std::vector<std::string>& labels, const std::string& subtitle);
    int takeChoice() {
        const int c = choice_;
        choice_ = -1;
        return c;
    }

protected:
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;

private:
    std::string text_, sub_;
    size_t shown_ = 0;
    std::vector<std::string> labels_;
    std::vector<ui::Rect> rects_;
    int hover_ = -1, choice_ = -1;
};

class Harvi {
public:
    explicit Harvi(Renderer& renderer);

    // Summon / dismiss with a holographic dissolve.
    void toggle(Vec3 eye, Vec3 forward, bool vr);
    void hold(bool h) { hold_ = h; }  // keep still while the pointer is on the panel
    void summon(Vec3 eye, Vec3 forward, bool vr);
    void dismiss();
    bool active() const { return state_ != State::Off; }
    bool panelOpen() const { return panel_ && panel_->isOpen(); }
    HarviPanel& panel() { return *panel_; }

    // Scripting API.
    void say(const std::string& text, const std::string& emote = "neutral");  // speaks with lip-sync; also used to narrate subjects
    void showNode(const std::string& id);
    void emote(const std::string& name, float seconds = 3.0f);  // neutral happy confused thinking surprised sad
    void gesture(const std::string& name);                      // nod shake spin hop wave
    void look(float x, float y, float seconds = 2.0f);          // eyes: -1..1
    void setContext(const std::string& ctx);                    // the active window: settings media browser files car ...
    std::string takeAction() {                                  // tool:/page:/key: actions for the shell to carry out
        std::string a = std::move(action_);
        action_.clear();
        return a;
    }

    void update(double dt, Vec3 eye, Vec3 forward, bool vr);
    void render(const GpuLabel& any);

private:
    enum class State { Off, Appearing, On, Leaving };
    enum class Emo { Neutral, Happy, Confused, Thinking, Surprised, Sad };
    void choose(int i);
    void present(const HarviReply& r);
    void refreshPanel();
    void emit(const Vec3& at, float dt);
    static Emo parseEmo(const std::string& n);

    Renderer& renderer_;
    std::unique_ptr<HarviPanel> panel_;
    std::vector<std::unique_ptr<HarviBrain>> brains_;
    State state_ = State::Off;
    float emerge_ = 0, time_ = 0;
    // Dialogue
    HarviReply cur_;
    float revealed_ = 0;
    bool talking_ = false;
    std::string ctx_, node_, action_, shownCtx_;
    std::map<std::string, bool> ctxSeen_;
    size_t lastShown_ = size_t(-1);
    // Placement
    Vec3 pos_{}, eyeNow_{}, fwdNow_{0, 0, -1};
    bool vr_ = false, placed_ = false, hold_ = false, following_ = false;
    float yaw_ = 0;
    // Pose (smoothed) and scripted overrides.
    struct Pose {
        float jaw = 0, droop = 0.2f, gill = 0.1f, lookX = 0, lookY = 0, lid = 0, roll = 0, turn = 0, tilt = 0;
        float finAmp = 0.3f, finFreq = 3.0f, tailAmp = 0.25f, tailFreq = 3.0f, bobAmp = 0.02f, dots = 0, hold = 0;
    } p_;
    Emo emo_ = Emo::Neutral;
    float emoT_ = 0;
    float lookOvT_ = 0, lookOvX_ = 0, lookOvY_ = 0;
    float blinkT_ = 2.0f, blinkPhase_ = -1.0f;
    float spinT_ = -1, hopT_ = -1, nodT_ = -1, shakeT_ = -1, waveT_ = -1;
    float wander_ = 0;
    struct Particle {
        Vec3 p, v;
        float life = 0;
    };
    std::vector<Particle> parts_;
    float emitAcc_ = 0;
    Vec3 tailWorld_{};
};

}  // namespace sf
