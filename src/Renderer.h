#pragma once
// Diligent Engine wrapper: device/swapchain setup, two pipeline states (LCARS panel, lit 3D),
// primitive meshes and a single dynamic constant buffer that is refreshed per draw call.
#include <memory>
#include <string>

#include "DeviceContext.h"
#include "EngineFactory.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"
#include "Utils.h"

struct SDL_Window;

namespace sf {

// A small texture (text label) with one shader resource binding per pipeline.
struct GpuLabel {
    Diligent::RefCntAutoPtr<Diligent::ITexture> texture;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> panelSRB;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> solidSRB;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> webSRB;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> floorSRB;
    float aspect = 1.0f;
};

struct PanelParams {
    float time = 0, radius = 0.12f, aspect = 1, selected = 0, segments = 4;
};

class Renderer {
public:
    ~Renderer();
    bool init(SDL_Window* window, const std::string& shaderDir);
    void shutdown();
    void waitIdle();
    // Resolves the multisampled target into the image passed to beginTarget (called by present() for the desktop).
    void endTarget();
    void resize();
    // Full-screen finishing pass on the desktop view (bloom, light streaks, vignette, chromatic fringe, grain).
    void setPostFx(bool on) { postFx_ = on; }
    // Call once the 3D world is drawn and before any UI: applies the post effects to the world only, so UI stays crisp.
    void postWorld();
    void beginDesktopFrame();
    void present(bool vsync);

    // Render into an externally owned target (VR swapchain image).
    void beginTarget(Diligent::ITextureView* rtv, Diligent::ITextureView* dsv, uint32_t w, uint32_t h, bool desktop = false);
    // Clears depth of the current target so later geometry (the viewer panel) draws over the whole scene.
    void clearDepth();
    Diligent::RefCntAutoPtr<Diligent::ITexture> createDepthTexture(uint32_t w, uint32_t h);

    void setView(const Mat4& view, const Mat4& proj);
    float aspect() const;

    // imageMode: the texture is an opaque thumbnail; color.r is a brightness multiplier.
    std::shared_ptr<GpuLabel> createLabel(const Bitmap& bitmap, bool srgbImage = false);
    void drawPanel(const GpuLabel& label, const Mat4& world, Vec3 color, const PanelParams& params);
    // White text with a soft shadow and no panel behind it; the label fills the quad.
    void drawHudText(const GpuLabel& label, const Mat4& world);
    // glow: -1 = no edge light (plain block), 0 = resting edge light, 1 = hovered. grid = the glowing floor.
    void drawCube(const GpuLabel& label, const Mat4& world, Vec3 color, bool grid = false, bool imageMode = false,
                  float glow = -1.0f);
    // Procedural city block (axis-aligned box; world = translate * scale). kind: 3 tower, 4 fill bar, 5 road,
    // 6 platform, 7 beam. emerge 0..1 rise glow, fill 0..1 level, flash -1..1 change pulse.
    void drawBuilding(const GpuLabel& any, const Mat4& world, Vec3 neon, float seed, int kind, float emerge,
                      float fill, float flash);
    // GLB models (assets/models/<name>.glb), loaded once, normalised to height 1 and sitting on y = 0. -1 if unavailable.
    int model(const std::string& name);
    Vec3 modelSize(int id) const;  // x/z extents relative to the unit height
    void drawModel(int id, const GpuLabel& any, const Mat4& world, Vec3 neon, float seed, bool textured = false);
    // Theme colours used by the grid floor, card edge lights and the tool ring.
    void setTheme(Vec3 accent, Vec3 alt);
    void setTime(float t);
    // Procedural radial tool ring on a unit quad (any label supplies the required SRB).
    void drawRing(const GpuLabel& label, const Mat4& world, int segments, float hover, float rotation, float open,
                  float pulse);
    // Unlit opaque image (colour.r-style brightness multiplier) and unlit text-over-colour block.
    void drawSky(const GpuLabel& label, const Mat4& world);  // animated cloud dome
    // Mirror mode draws everything flipped under the floor; the floor and sky skip it.
    // beginReflection/endReflection redirect drawing into a half-resolution reflection texture that the metallic
    // floor shader samples (with brushed-metal blur and Fresnel) instead of blending a mirrored copy into the frame.
    void beginReflection();
    void endReflection();
    void setMirror(bool on) { mirror_ = on; }
    bool mirror() const { return mirror_; }
    void drawImage(const GpuLabel& label, const Mat4& world, float brightness = 1.0f);
    void drawFlat(const GpuLabel& label, const Mat4& world, Vec3 color);
    // Pyramid whose apex sits at the world matrix origin and whose body extends along local -Z.
    void drawPointer(const GpuLabel& label, const Mat4& world, Vec3 color);
    // Fixed-size sRGB RGBA texture that can be refreshed every frame (video frames, UI bars).
    std::shared_ptr<GpuLabel> createDynamicLabel(int w, int h);
    void updateLabel(GpuLabel& label, const uint8_t* rgba, int w, int h);
    // Uploads a sub-rectangle (tightly packed RGBA rows of width rw) at (x, y).
    void updateLabelRect(GpuLabel& label, int x, int y, int rw, int rh, const uint8_t* rgba, size_t strideBytes);

    // Web depth compositor primitives (alpha blended, unlit). uvRect = {u0, v0, du, dv} of the label texture.
    void drawWebRegion(const GpuLabel& label, const Mat4& world, const float uvRect[4], float aspect, float radius,
                       float rim, float opacity, const float tint[3]);
    // Translucent glass card with a lit rim on a unit quad scaled to (w, h); radius is in units of the quad height.
    void drawGlass(const Mat4& world, float aspect, float radius, float opacity = 0.62f);
    // Soft shadow/glow card; inner = size of the shadowed rectangle as a fraction of the quad (0..1 per axis).
    void drawWebShadow(const GpuLabel& label, const Mat4& world, const float color[3], float intensity, float innerX,
                       float innerY);

    Diligent::IRenderDevice* device() const { return device_; }
    Diligent::IDeviceContext* context() const { return context_; }

private:
    struct Mesh {
        Diligent::RefCntAutoPtr<Diligent::IBuffer> vb, ib;
        uint32_t indexCount = 0;
    };
    struct Pipeline {
        Diligent::RefCntAutoPtr<Diligent::IPipelineState> pso;
    };

    bool createPipeline(Pipeline& out, const char* name, const char* vsFile, const char* psFile, bool blend, bool premul = false,
                        bool reflection = false, bool post = false, bool blit = false);
    void runPost();
    void ensureReflection(uint32_t w, uint32_t h);
    Mesh createMesh(const std::vector<float>& vertices, const std::vector<uint32_t>& indices);
    void buildMeshes();
    struct MsaaTargets {
        Diligent::RefCntAutoPtr<Diligent::ITexture> color, depth;
        uint32_t w = 0, h = 0;
    };
    void ensureMsaaTargets(MsaaTargets& m, uint32_t w, uint32_t h);
    void draw(Pipeline& pipeline, Diligent::IShaderResourceBinding* srb, const Mesh& mesh, const Mat4& world,
              const float color[4], const float params[4], const float light[4]);

    SDL_Window* window_ = nullptr;
    Diligent::IEngineFactory* factory_ = nullptr;
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context_;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapChain_;
    Diligent::RefCntAutoPtr<Diligent::IShaderSourceInputStreamFactory> shaderFactory_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> constants_;
    Pipeline panelPipeline_, solidPipeline_, webPipeline_, floorPipeline_, postPipeline_, blitPipeline_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> postSRB_;
    Diligent::RefCntAutoPtr<Diligent::ITexture> postTex_;
    uint32_t postW_ = 0, postH_ = 0;
    bool desktopActive_ = false;
    bool postFx_ = true;
    bool worldPosted_ = false;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> blitSRB_;
    bool mirror_ = false;
    void bindLabel(GpuLabel& label);
    uint32_t samples_ = 1;
    MsaaTargets msaa_[3];  // [0] desktop, [1] VR eye, [2] reflection
    Diligent::RefCntAutoPtr<Diligent::ITexture> reflTex_;  // resolved reflection image sampled by the floor
    uint32_t reflW_ = 0, reflH_ = 0, curW_ = 0, curH_ = 0;
    bool reflFresh_ = false;
    Diligent::ITextureView* mainRtv_ = nullptr;
    Diligent::ITextureView* mainDsv_ = nullptr;
    Diligent::ITexture* resolveDst_ = nullptr;
    Diligent::ITextureView* currentDsv_ = nullptr;
    Diligent::ITexture* resolveSrc_ = nullptr;
    Mesh quad_, cube_, pyramid_;
    std::vector<Mesh> models_;
    std::vector<std::shared_ptr<GpuLabel>> modelTex_;
    std::vector<Vec3> modelSizes_;
    std::vector<std::pair<std::string, int>> modelIds_;
    std::shared_ptr<GpuLabel> glassLabel_;
    Mat4 view_ = Mat4::identity(), proj_ = Mat4::identity();
    float lightDir_[4] = {-0.4f, -1.0f, -0.6f, 0.0f};
    float accent_[4] = {0.1f, 0.85f, 1.0f, 0.0f};
    float alt_[4] = {1.0f, 0.65f, 0.1f, 0.0f};
};

}  // namespace sf
