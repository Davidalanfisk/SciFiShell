#include "Renderer.h"
#include "ModelLoader.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <cstdio>
#include <cstring>

#include "MapHelper.hpp"

#if defined(_WIN32)
#    include "EngineFactoryD3D12.h"
#elif defined(__APPLE__)
#    include "EngineFactoryMtl.h"
#else
#    include "EngineFactoryVk.h"
#endif

using namespace Diligent;

namespace sf {

namespace {
// Must match the cbuffer layout in the shaders.
struct Constants {
    float mvp[16];
    float world[16];
    float color[4];
    float params[4];
    float light[4];
    float accent[4];  // theme accent rgb + time
    float alt[4];     // theme alert colour
};
constexpr TEXTURE_FORMAT kColorFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
constexpr TEXTURE_FORMAT kDepthFormat = TEX_FORMAT_D32_FLOAT;
}  // namespace

Renderer::~Renderer() { shutdown(); }

void Renderer::waitIdle() {
    if (context_) {
        context_->Flush();
        context_->WaitForIdle();
    }
}

void Renderer::shutdown() {
    waitIdle();
    msaa_[0] = {};
    msaa_[1] = {};
    msaa_[2] = {};
    reflTex_.Release();
    resolveDst_ = nullptr;
    resolveSrc_ = nullptr;
    quad_ = {};
    pyramid_ = {};
    cube_ = {};
    panelPipeline_ = {};
    solidPipeline_ = {};
    webPipeline_ = {};
    floorPipeline_ = {};
    constants_.Release();
    shaderFactory_.Release();
    swapChain_.Release();
    context_.Release();
    device_.Release();
}

bool Renderer::init(SDL_Window* window, const std::string& shaderDir) {
    window_ = window;
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window, &wm)) {
        std::fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return false;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);

    SwapChainDesc sc;
    sc.Width = static_cast<Uint32>(w);
    sc.Height = static_cast<Uint32>(h);
    sc.ColorBufferFormat = kColorFormat;
    sc.DepthBufferFormat = kDepthFormat;

#if defined(_WIN32)
    auto* factory = GetEngineFactoryD3D12();
    factory_ = factory;
    EngineD3D12CreateInfo ci;
    factory->CreateDeviceAndContextsD3D12(ci, &device_, &context_);
    if (!device_) return false;
    Win32NativeWindow native{wm.info.win.window};
    factory->CreateSwapChainD3D12(device_, context_, sc, FullScreenModeDesc{}, native, &swapChain_);
#elif defined(__APPLE__)
    auto* factory = GetEngineFactoryMtl();
    factory_ = factory;
    EngineMtlCreateInfo ci;
    factory->CreateDeviceAndContextsMtl(ci, &device_, &context_);
    if (!device_) return false;
    SDL_MetalView view = SDL_Metal_CreateView(window);
    MacOSNativeWindow native{view};
    factory->CreateSwapChainMtl(device_, context_, sc, native, &swapChain_);
#else
    auto* factory = GetEngineFactoryVk();
    factory_ = factory;
    EngineVkCreateInfo ci;
    factory->CreateDeviceAndContextsVk(ci, &device_, &context_);
    if (!device_) return false;
    LinuxNativeWindow native;
    native.WindowId = static_cast<Uint32>(wm.info.x11.window);
    native.pDisplay = wm.info.x11.display;
    factory->CreateSwapChainVk(device_, context_, sc, native, &swapChain_);
#endif
    if (!swapChain_) return false;

    {
        const Uint32 mask = static_cast<Uint32>(device_->GetTextureFormatInfoExt(kColorFormat).SampleCounts) &
                            static_cast<Uint32>(device_->GetTextureFormatInfoExt(kDepthFormat).SampleCounts);
        samples_ = (mask & SAMPLE_COUNT_4) ? 4 : 1;  // 4x MSAA when the GPU supports it
    }
    factory_->CreateDefaultShaderSourceStreamFactory(shaderDir.c_str(), &shaderFactory_);
    if (!shaderFactory_) return false;

    BufferDesc cb;
    cb.Name = "Constants";
    cb.Size = sizeof(Constants);
    cb.Usage = USAGE_DYNAMIC;
    cb.BindFlags = BIND_UNIFORM_BUFFER;
    cb.CPUAccessFlags = CPU_ACCESS_WRITE;
    device_->CreateBuffer(cb, nullptr, &constants_);

    if (!createPipeline(panelPipeline_, "LCARS panel", "lcars_panel.vs.hlsl", "lcars_panel.ps.hlsl", true)) return false;
    if (!createPipeline(solidPipeline_, "Basic 3D", "basic_3d.vs.hlsl", "basic_3d.ps.hlsl", false)) return false;
    if (!createPipeline(webPipeline_, "Web layer", "basic_3d.vs.hlsl", "web_layer.ps.hlsl", true)) return false;
    if (!createPipeline(floorPipeline_, "Floor", "basic_3d.vs.hlsl", "basic_3d.ps.hlsl", true, true, true)) return false;
    buildMeshes();
    if (std::getenv("SCIFI_NOPOST")) postFx_ = false;
    createPipeline(postPipeline_, "Post", "basic_3d.vs.hlsl", "post.ps.hlsl", false, false, false, true);
    createPipeline(blitPipeline_, "Post blit", "basic_3d.vs.hlsl", "post.ps.hlsl", false, false, false, false, true);
    return true;
}

void Renderer::ensureMsaaTargets(MsaaTargets& m, uint32_t w, uint32_t h) {
    if (m.color && m.depth && m.w == w && m.h == h) return;
    m.color.Release();
    m.depth.Release();
    TextureDesc td;
    td.Type = RESOURCE_DIM_TEX_2D;
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.SampleCount = samples_;
    td.Name = "MSAA color";
    td.Format = kColorFormat;
    td.BindFlags = BIND_RENDER_TARGET;
    td.ClearValue.Format = kColorFormat;
    td.ClearValue.Color[0] = 0.01f;
    td.ClearValue.Color[1] = 0.01f;
    td.ClearValue.Color[2] = 0.03f;
    td.ClearValue.Color[3] = 1.0f;
    device_->CreateTexture(td, nullptr, &m.color);
    td.Name = "MSAA depth";
    td.Format = kDepthFormat;
    td.BindFlags = BIND_DEPTH_STENCIL;
    td.ClearValue = {};
    td.ClearValue.Format = kDepthFormat;
    td.ClearValue.DepthStencil.Depth = 1.0f;
    device_->CreateTexture(td, nullptr, &m.depth);
    m.w = w;
    m.h = h;
}

void Renderer::runPost() {
    if (!postFx_ || !postPipeline_.pso) return;
    const auto& scd = swapChain_->GetDesc();
    if (!postTex_ || postW_ != scd.Width || postH_ != scd.Height) {
        postTex_.Release();
        postSRB_.Release();
        TextureDesc td;
        td.Name = "Post source";
        td.Type = RESOURCE_DIM_TEX_2D;
        td.Width = scd.Width;
        td.Height = scd.Height;
        td.MipLevels = 1;
        td.Format = scd.ColorBufferFormat;
        td.BindFlags = BIND_SHADER_RESOURCE;
        device_->CreateTexture(td, nullptr, &postTex_);
        if (!postTex_) return;
        blitSRB_.Release();
        postPipeline_.pso->CreateShaderResourceBinding(&postSRB_, true);
        if (blitPipeline_.pso) blitPipeline_.pso->CreateShaderResourceBinding(&blitSRB_, true);
        if (blitSRB_)
            if (auto* bv = blitSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Tex"))
                bv->Set(postTex_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        if (auto* var = postSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Tex"))
            var->Set(postTex_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        postW_ = scd.Width;
        postH_ = scd.Height;
    }
    ITextureView* rtv = swapChain_->GetCurrentBackBufferRTV();
    CopyTextureAttribs ca;
    ca.pSrcTexture = rtv->GetTexture();
    ca.pDstTexture = postTex_;
    ca.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    ca.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context_->CopyTexture(ca);
    context_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp{0, 0, static_cast<float>(postW_), static_cast<float>(postH_), 0.0f, 1.0f};
    context_->SetViewports(1, &vp, postW_, postH_);
    const Mat4 sv = view_, sp = proj_;
    view_ = Mat4::identity();
    proj_ = Mat4::identity();
    const float color[4] = {1.0f / static_cast<float>(postW_), 1.0f / static_cast<float>(postH_), accent_[3], 1.0f};
    const float zero[4] = {0, 0, 0, 0};
    draw(postPipeline_, postSRB_, quad_, Mat4::scale({2, 2, 1}), color, zero, zero);
    view_ = sv;
    proj_ = sp;
}

void Renderer::postWorld() {
    if (!desktopActive_ || !postFx_ || worldPosted_ || !postPipeline_.pso) return;
    if (samples_ <= 1) {
        runPost();
        context_->SetRenderTargets(1, &mainRtv_, mainDsv_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        worldPosted_ = true;
        return;
    }
    if (!blitPipeline_.pso || !resolveDst_) return;
    ResolveTextureSubresourceAttribs ra;
    ra.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    ra.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context_->ResolveTextureSubresource(resolveSrc_, resolveDst_, ra);
    runPost();
    if (!blitSRB_) return;
    // Copy the finished world back into the multisampled target so the UI can be drawn over it (and still be anti-aliased).
    CopyTextureAttribs ca;
    ca.pSrcTexture = resolveDst_;
    ca.pDstTexture = postTex_;
    ca.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    ca.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context_->CopyTexture(ca);
    context_->SetRenderTargets(1, &mainRtv_, mainDsv_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp{0, 0, static_cast<float>(curW_), static_cast<float>(curH_), 0.0f, 1.0f};
    context_->SetViewports(1, &vp, curW_, curH_);
    const Mat4 sv = view_, sp = proj_;
    view_ = Mat4::identity();
    proj_ = Mat4::identity();
    const float color[4] = {1.0f / static_cast<float>(postW_), 1.0f / static_cast<float>(postH_), 0.0f, 1.0f};
    const float blit[4] = {1, 0, 0, 0};
    const float zero[4] = {0, 0, 0, 0};
    draw(blitPipeline_, blitSRB_, quad_, Mat4::scale({2, 2, 1}), color, blit, zero);
    view_ = sv;
    proj_ = sp;
    clearDepth();
    worldPosted_ = true;
}

void Renderer::endTarget() {
    desktopActive_ = false;
    worldPosted_ = false;
    if (!resolveDst_) return;
    ResolveTextureSubresourceAttribs ra;
    ra.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    ra.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context_->ResolveTextureSubresource(resolveSrc_, resolveDst_, ra);
    // VR runtimes expect the image back in the render-target state when it is released.
    StateTransitionDesc tr{resolveDst_, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_RENDER_TARGET,
                           STATE_TRANSITION_FLAG_UPDATE_STATE};
    context_->TransitionResourceStates(1, &tr);
    resolveDst_ = nullptr;
}

bool Renderer::createPipeline(Pipeline& out, const char* name, const char* vsFile, const char* psFile, bool blend, bool premul,
                              bool reflection, bool post, bool blit) {
    GraphicsPipelineStateCreateInfo ci;
    ci.PSODesc.Name = name;
    ci.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;
    auto& g = ci.GraphicsPipeline;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = kColorFormat;
    g.DSVFormat = kDepthFormat;
    g.SmplDesc.Count = post ? 1 : samples_;
    if (post) g.DSVFormat = TEX_FORMAT_UNKNOWN;
    g.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    g.RasterizerDesc.CullMode = CULL_MODE_NONE;
    g.DepthStencilDesc.DepthEnable = (post || blit) ? False : True;
    g.DepthStencilDesc.DepthWriteEnable = blend ? False : True;
    if (blend) {
        auto& rt = g.BlendDesc.RenderTargets[0];
        rt.BlendEnable = True;
        rt.SrcBlend = premul ? BLEND_FACTOR_ONE : BLEND_FACTOR_SRC_ALPHA;
        rt.DestBlend = BLEND_FACTOR_INV_SRC_ALPHA;
        rt.SrcBlendAlpha = BLEND_FACTOR_ONE;
        rt.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
    }

    ShaderCreateInfo sci;
    sci.SourceLanguage = SHADER_SOURCE_LANGUAGE_HLSL;
    sci.pShaderSourceStreamFactory = shaderFactory_;
    sci.EntryPoint = "main";
    sci.Desc.UseCombinedTextureSamplers = true;

    RefCntAutoPtr<IShader> vs, ps;
    sci.Desc.ShaderType = SHADER_TYPE_VERTEX;
    sci.Desc.Name = vsFile;
    sci.FilePath = vsFile;
    device_->CreateShader(sci, &vs);
    sci.Desc.ShaderType = SHADER_TYPE_PIXEL;
    sci.Desc.Name = psFile;
    sci.FilePath = psFile;
    device_->CreateShader(sci, &ps);
    if (!vs || !ps) {
        std::fprintf(stderr, "Failed to compile shaders for pipeline '%s'\n", name);
        return false;
    }

    LayoutElement layout[] = {
        LayoutElement{0, 0, 3, VT_FLOAT32, False},  // position
        LayoutElement{1, 0, 3, VT_FLOAT32, False},  // normal
        LayoutElement{2, 0, 2, VT_FLOAT32, False},  // uv
    };
    g.InputLayout.LayoutElements = layout;
    g.InputLayout.NumElements = 3;
    ci.pVS = vs;
    ci.pPS = ps;

    ci.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    ShaderResourceVariableDesc vars[] = {{SHADER_TYPE_PIXEL, "g_Tex", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
                                         {SHADER_TYPE_PIXEL, "g_Refl", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ci.PSODesc.ResourceLayout.Variables = vars;
    ci.PSODesc.ResourceLayout.NumVariables = reflection ? 2 : 1;
    SamplerDesc sampler;
    sampler.MinFilter = sampler.MagFilter = sampler.MipFilter = FILTER_TYPE_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = TEXTURE_ADDRESS_CLAMP;
    ImmutableSamplerDesc samplers[] = {{SHADER_TYPE_PIXEL, "g_Tex", sampler}, {SHADER_TYPE_PIXEL, "g_Refl", sampler}};
    ci.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
    ci.PSODesc.ResourceLayout.NumImmutableSamplers = reflection ? 2 : 1;

    device_->CreateGraphicsPipelineState(ci, &out.pso);
    if (!out.pso) return false;
    for (SHADER_TYPE st : {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL})
        if (auto* v = out.pso->GetStaticVariableByName(st, "Constants")) v->Set(constants_);
    return true;
}

Renderer::Mesh Renderer::createMesh(const std::vector<float>& vertices, const std::vector<uint32_t>& indices) {
    Mesh m;
    BufferDesc vd;
    vd.Name = "Mesh VB";
    vd.Usage = USAGE_IMMUTABLE;
    vd.BindFlags = BIND_VERTEX_BUFFER;
    vd.Size = static_cast<Uint64>(vertices.size() * sizeof(float));
    BufferData vdata{vertices.data(), vd.Size};
    device_->CreateBuffer(vd, &vdata, &m.vb);
    BufferDesc id;
    id.Name = "Mesh IB";
    id.Usage = USAGE_IMMUTABLE;
    id.BindFlags = BIND_INDEX_BUFFER;
    id.Size = static_cast<Uint64>(indices.size() * sizeof(uint32_t));
    BufferData idata{indices.data(), id.Size};
    device_->CreateBuffer(id, &idata, &m.ib);
    m.indexCount = static_cast<uint32_t>(indices.size());
    return m;
}

void Renderer::buildMeshes() {
    // Unit quad in the XY plane facing +Z; uv (0,0) is the top-left corner.
    quad_ = createMesh({-0.5f, -0.5f, 0, 0, 0, 1, 0, 1,  //
                        0.5f, -0.5f, 0, 0, 0, 1, 1, 1,   //
                        0.5f, 0.5f, 0, 0, 0, 1, 1, 0,    //
                        -0.5f, 0.5f, 0, 0, 0, 1, 0, 0},
                       {0, 1, 2, 0, 2, 3});

    // Unit cube: each face is built from (normal, u, v) with u x v = normal.
    const Vec3 faces[6][3] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    };
    std::vector<float> v;
    std::vector<uint32_t> idx;
    for (int f = 0; f < 6; ++f) {
        const Vec3 n = faces[f][0], u = faces[f][1], t = faces[f][2];
        const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        uint32_t base = static_cast<uint32_t>(v.size() / 8);
        for (int i = 0; i < 4; ++i) {
            Vec3 p = n * 0.5f + u * (su[i] * 0.5f) + t * (sv[i] * 0.5f);
            v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, (su[i] + 1) * 0.5f, 1.0f - (sv[i] + 1) * 0.5f});
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    cube_ = createMesh(v, idx);

    // Pointer pyramid: apex at the origin, square base one unit behind it along -Z.
    const Vec3 apex{0, 0, 0};
    const Vec3 bc[4] = {{-0.5f, -0.5f, -1}, {0.5f, -0.5f, -1}, {0.5f, 0.5f, -1}, {-0.5f, 0.5f, -1}};
    std::vector<float> pv;
    std::vector<uint32_t> pi;
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 outward) {
        Vec3 n = normalize(cross(b - a, c - a));
        if (dot(n, outward) < 0) {
            std::swap(b, c);
            n = n * -1.0f;
        }
        uint32_t base = static_cast<uint32_t>(pv.size() / 8);
        for (const Vec3& p : {a, b, c}) pv.insert(pv.end(), {p.x, p.y, p.z, n.x, n.y, n.z, 0.5f, 0.5f});
        pi.insert(pi.end(), {base, base + 1, base + 2});
    };
    for (int i = 0; i < 4; ++i) {
        const Vec3 a = bc[i], b = bc[(i + 1) % 4];
        tri(apex, a, b, (a + b) * 0.5f);
    }
    tri(bc[0], bc[1], bc[2], {0, 0, -1});
    tri(bc[0], bc[2], bc[3], {0, 0, -1});
    pyramid_ = createMesh(pv, pi);
}

void Renderer::resize() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (swapChain_ && w > 0 && h > 0) swapChain_->Resize(static_cast<Uint32>(w), static_cast<Uint32>(h));
}

float Renderer::aspect() const {
    const auto& d = swapChain_->GetDesc();
    return d.Height ? static_cast<float>(d.Width) / static_cast<float>(d.Height) : 1.0f;
}

void Renderer::beginDesktopFrame() {
    const auto& scd = swapChain_->GetDesc();
    beginTarget(swapChain_->GetCurrentBackBufferRTV(), swapChain_->GetDepthBufferDSV(), scd.Width, scd.Height, true);
}

void Renderer::beginTarget(ITextureView* rtv, ITextureView* dsv, uint32_t w, uint32_t h, bool desktop) {
    desktopActive_ = desktop;
    if (samples_ > 1) {
        MsaaTargets& m = msaa_[desktop ? 0 : 1];
        ensureMsaaTargets(m, w, h);
        resolveDst_ = rtv->GetTexture();
        resolveSrc_ = m.color;
        rtv = m.color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        dsv = m.depth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
    }
    context_->SetRenderTargets(1, &rtv, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    context_->SetViewports(1, &vp, w, h);
    const float clear[4] = {0.002f, 0.005f, 0.014f, 1.0f};
    context_->ClearRenderTarget(rtv, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context_->ClearDepthStencil(dsv, CLEAR_DEPTH_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    currentDsv_ = dsv;
    mainRtv_ = rtv;
    mainDsv_ = dsv;
    curW_ = w;
    curH_ = h;
    reflFresh_ = false;
}

void Renderer::ensureReflection(uint32_t w, uint32_t h) {
    w = std::max(64u, w);
    h = std::max(64u, h);
    if (reflTex_ && reflW_ == w && reflH_ == h) return;
    reflTex_.Release();
    TextureDesc td;
    td.Name = "Floor reflection";
    td.Type = RESOURCE_DIM_TEX_2D;
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.Format = kColorFormat;
    td.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    device_->CreateTexture(td, nullptr, &reflTex_);
    reflW_ = w;
    reflH_ = h;
    reflFresh_ = false;
}

void Renderer::beginReflection() {
    if (!mainRtv_) return;
    ensureReflection(curW_ / 2, curH_ / 2);
    ITextureView *rtv, *dsv;
    ensureMsaaTargets(msaa_[2], reflW_, reflH_);
    dsv = msaa_[2].depth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
    rtv = samples_ > 1 ? msaa_[2].color->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET)
                       : reflTex_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    context_->SetRenderTargets(1, &rtv, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp{0, 0, static_cast<float>(reflW_), static_cast<float>(reflH_), 0.0f, 1.0f};
    context_->SetViewports(1, &vp, reflW_, reflH_);
    const float clear[4] = {0, 0, 0, 0};  // alpha 0 = nothing reflected here (the floor shows the sky instead)
    context_->ClearRenderTarget(rtv, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context_->ClearDepthStencil(dsv, CLEAR_DEPTH_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    currentDsv_ = dsv;
}

void Renderer::endReflection() {
    if (!mainRtv_ || !reflTex_) return;
    if (samples_ > 1) {
        ResolveTextureSubresourceAttribs ra;
        ra.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        ra.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        context_->ResolveTextureSubresource(msaa_[2].color, reflTex_, ra);
    }
    ITextureView* rtv = mainRtv_;
    context_->SetRenderTargets(1, &rtv, mainDsv_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp{0, 0, static_cast<float>(curW_), static_cast<float>(curH_), 0.0f, 1.0f};
    context_->SetViewports(1, &vp, curW_, curH_);
    currentDsv_ = mainDsv_;
    reflFresh_ = true;
}

void Renderer::clearDepth() {
    if (currentDsv_) context_->ClearDepthStencil(currentDsv_, CLEAR_DEPTH_FLAG, 1.0f, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

RefCntAutoPtr<ITexture> Renderer::createDepthTexture(uint32_t w, uint32_t h) {
    TextureDesc td;
    td.Name = "VR depth";
    td.Type = RESOURCE_DIM_TEX_2D;
    td.Width = w;
    td.Height = h;
    td.Format = kDepthFormat;
    td.BindFlags = BIND_DEPTH_STENCIL;
    td.ClearValue.Format = kDepthFormat;
    td.ClearValue.DepthStencil.Depth = 1.0f;
    RefCntAutoPtr<ITexture> tex;
    device_->CreateTexture(td, nullptr, &tex);
    return tex;
}

void Renderer::present(bool vsync) {
    endTarget();
    swapChain_->Present(vsync ? 1 : 0);
}

void Renderer::setView(const Mat4& view, const Mat4& proj) {
    view_ = view;
    proj_ = proj;
}

std::shared_ptr<GpuLabel> Renderer::createLabel(const Bitmap& bm, bool srgbImage) {
    auto label = std::make_shared<GpuLabel>();

    // CPU mip chain (alpha-weighted box filter) so thumbnails stay clean at a distance.
    std::vector<std::vector<uint8_t>> levels;
    std::vector<std::pair<int, int>> sizes;
    levels.push_back(bm.rgba);
    sizes.push_back({bm.w, bm.h});
    while (srgbImage && (sizes.back().first > 1 || sizes.back().second > 1)) {
        auto [pw, ph] = sizes.back();
        int nw = std::max(1, pw / 2), nh = std::max(1, ph / 2);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        const auto& prev = levels.back();
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(x * 2 + dx, pw - 1), sy = std::min(y * 2 + dy, ph - 1);
                        const uint8_t* p = &prev[(static_cast<size_t>(sy) * pw + sx) * 4];
                        float a = p[3] / 255.0f;
                        acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
                    }
                uint8_t* o = &next[(static_cast<size_t>(y) * nw + x) * 4];
                if (acc[3] > 0.0f) {
                    o[0] = static_cast<uint8_t>(acc[0] / acc[3] + 0.5f);
                    o[1] = static_cast<uint8_t>(acc[1] / acc[3] + 0.5f);
                    o[2] = static_cast<uint8_t>(acc[2] / acc[3] + 0.5f);
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
                o[3] = static_cast<uint8_t>(acc[3] * 0.25f * 255.0f + 0.5f);
            }
        levels.push_back(std::move(next));
        sizes.push_back({nw, nh});
    }

    TextureDesc td;
    td.Name = "Label";
    td.Type = RESOURCE_DIM_TEX_2D;
    td.Width = static_cast<Uint32>(bm.w);
    td.Height = static_cast<Uint32>(bm.h);
    td.MipLevels = static_cast<Uint32>(levels.size());
    td.Format = srgbImage ? TEX_FORMAT_RGBA8_UNORM_SRGB : TEX_FORMAT_RGBA8_UNORM;
    td.BindFlags = BIND_SHADER_RESOURCE;
    td.Usage = USAGE_IMMUTABLE;
    std::vector<TextureSubResData> subs;
    for (size_t i = 0; i < levels.size(); ++i)
        subs.push_back({levels[i].data(), static_cast<Uint64>(sizes[i].first) * 4});
    TextureData data{subs.data(), static_cast<Uint32>(subs.size())};
    device_->CreateTexture(td, &data, &label->texture);
    label->aspect = bm.h ? static_cast<float>(bm.w) / static_cast<float>(bm.h) : 1.0f;
    bindLabel(*label);
    return label;
}

void Renderer::bindLabel(GpuLabel& label) {
    auto bind = [&](Pipeline& p, Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding>& srb) {
        p.pso->CreateShaderResourceBinding(&srb, true);
        if (auto* var = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Tex"))
            var->Set(label.texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    };
    bind(panelPipeline_, label.panelSRB);
    bind(solidPipeline_, label.solidSRB);
    bind(webPipeline_, label.webSRB);
    bind(floorPipeline_, label.floorSRB);
}

void Renderer::draw(Pipeline& pipeline, IShaderResourceBinding* srb, const Mesh& mesh, const Mat4& world,
                    const float color[4], const float params[4], const float light[4]) {
    {
        MapHelper<Constants> cb(context_, constants_, MAP_WRITE, MAP_FLAG_DISCARD);
        Mat4 mw = mirror_ ? Mat4::scale({1, -1, 1}) * world : world;
        Mat4 mvp = proj_ * view_ * mw;
        std::memcpy(cb->mvp, mvp.m, sizeof(cb->mvp));
        std::memcpy(cb->world, mw.m, sizeof(cb->world));
        std::memcpy(cb->color, color, sizeof(cb->color));
        std::memcpy(cb->params, params, sizeof(cb->params));
        std::memcpy(cb->light, light, sizeof(cb->light));
        std::memcpy(cb->accent, accent_, sizeof(cb->accent));
        std::memcpy(cb->alt, alt_, sizeof(cb->alt));
    }
    context_->SetPipelineState(pipeline.pso);
    context_->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    IBuffer* vbs[] = {mesh.vb};
    context_->SetVertexBuffers(0, 1, vbs, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                               SET_VERTEX_BUFFERS_FLAG_RESET);
    context_->SetIndexBuffer(mesh.ib, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawIndexedAttribs da;
    da.IndexType = VT_UINT32;
    da.NumIndices = mesh.indexCount;
    da.Flags = DRAW_FLAG_VERIFY_ALL;
    context_->DrawIndexed(da);
}

void Renderer::drawPanel(const GpuLabel& label, const Mat4& world, Vec3 c, const PanelParams& p) {
    const float color[4] = {c.x, c.y, c.z, 1.0f};
    const float params[4] = {p.time, p.radius, p.aspect, p.selected};
    const float light[4] = {p.segments, label.aspect, 0, 0};
    draw(panelPipeline_, label.panelSRB, quad_, world, color, params, light);
}

void Renderer::drawHudText(const GpuLabel& label, const Mat4& world) {
    const float color[4] = {1, 1, 1, 1};
    const float params[4] = {0, 0, 1, -1.0f};
    const float light[4] = {0, label.aspect, 0, 0};
    draw(panelPipeline_, label.panelSRB, quad_, world, color, params, light);
}

void Renderer::setTheme(Vec3 accent, Vec3 alt) {
    accent_[0] = accent.x; accent_[1] = accent.y; accent_[2] = accent.z;
    alt_[0] = alt.x; alt_[1] = alt.y; alt_[2] = alt.z;
}

void Renderer::setTime(float t) { accent_[3] = t; }

void Renderer::drawRing(const GpuLabel& label, const Mat4& world, int segments, float hover, float rotation, float open,
                        float pulse) {
    const float color[4] = {accent_[0], accent_[1], accent_[2], open};
    const float params[4] = {2.0f, static_cast<float>(segments), hover, accent_[3]};
    const float extra[4] = {rotation, open, pulse, 0};
    draw(webPipeline_, label.webSRB, quad_, world, color, params, extra);
}

void Renderer::drawCube(const GpuLabel& label, const Mat4& world, Vec3 c, bool grid, bool imageMode, float glow, bool interior) {
    float color[4] = {c.x, c.y, c.z, 1.0f};
    const float params[4] = {glow, imageMode ? 1.0f : 0.0f, interior ? 2.0f : 1.0f, grid ? 1.0f : 0.0f};
    Vec3 eye = view_.rigidInverse().transformPoint({0, 0, 0});
    const float eyeL[4] = {eye.x, eye.y, eye.z, 0};  // the floor shader needs the eye for its reflection
    if (grid) {
        ensureReflection(curW_ / 2, curH_ / 2);
        if (auto* var = label.floorSRB->GetVariableByName(SHADER_TYPE_PIXEL, "g_Refl"))
            var->Set(reflTex_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        // Floor colour slot carries the reflection lookup: 1/width, 1/height of the main target and a validity flag.
        color[0] = 1.0f / static_cast<float>(std::max(1u, curW_));
        color[1] = 1.0f / static_cast<float>(std::max(1u, curH_));
        color[2] = reflFresh_ ? 1.0f : 0.0f;
    }
    draw(grid ? floorPipeline_ : solidPipeline_, grid ? label.floorSRB : label.solidSRB, cube_, world, color, params,
         grid ? eyeL : lightDir_);
}

void Renderer::drawBuilding(const GpuLabel& any, const Mat4& world, Vec3 neon, float seed, int kind, float emerge,
                            float fill, float flash) {
    const float color[4] = {neon.x, neon.y, neon.z, seed};
    const float params[4] = {emerge, fill, flash, static_cast<float>(kind)};
    Vec3 eye = view_.rigidInverse().transformPoint({0, 0, 0});
    const float eyeL[4] = {eye.x, eye.y, eye.z, 0};
    if (kind == 6) draw(floorPipeline_, any.floorSRB, cube_, world, color, params, eyeL);  // glass pane: blended, no depth write
    else draw(solidPipeline_, any.solidSRB, cube_, world, color, params, eyeL);
}

int Renderer::model(const std::string& name) {
    for (auto& e : modelIds_)
        if (e.first == name) return e.second;
    int id = -1;
    const char* base = SDL_GetBasePath();
    std::vector<float> v;
    std::vector<uint32_t> idx;
    Vec3 size;
    Bitmap atlas;
    if (loadGltfMesh(std::string(base ? base : "./") + "assets/models/" + name + ".glb", v, idx, size, &atlas)) {
        models_.push_back(createMesh(v, idx));
        modelTex_.push_back(createLabel(atlas, true));
        modelSizes_.push_back(size);
        id = static_cast<int>(models_.size()) - 1;
    }
    modelIds_.push_back({name, id});
    return id;
}
Vec3 Renderer::modelSize(int id) const { return id >= 0 && id < static_cast<int>(modelSizes_.size()) ? modelSizes_[id] : Vec3{1, 1, 1}; }
void Renderer::drawModel(int id, const GpuLabel& any, const Mat4& world, Vec3 neon, float seed, bool textured) {
    if (id < 0 || id >= static_cast<int>(models_.size())) return;
    const float color[4] = {neon.x, neon.y, neon.z, seed};
    const float params[4] = {0, 0, 0, textured ? 9.0f : 8.0f};
    Vec3 eye = view_.rigidInverse().transformPoint({0, 0, 0});
    const float eyeL[4] = {eye.x, eye.y, eye.z, 0};
    draw(solidPipeline_, textured ? modelTex_[id]->solidSRB : any.solidSRB, models_[id], world, color, params, eyeL);
}

void Renderer::drawPointer(const GpuLabel& label, const Mat4& world, Vec3 c) {
    const float color[4] = {c.x, c.y, c.z, 1.0f};
    const float params[4] = {0.6f, 0.0f, 1, 0};
    draw(solidPipeline_, label.solidSRB, pyramid_, world, color, params, lightDir_);
}

void Renderer::drawSky(const GpuLabel& label, const Mat4& world) {
    const float color[4] = {0, 0, 0, 1};
    const float params[4] = {-1, 0, 1, 2.0f};
    Vec3 eye = view_.rigidInverse().transformPoint({0, 0, 0});
    const float eyeL[4] = {eye.x, eye.y, eye.z, 0};
    draw(solidPipeline_, label.solidSRB, cube_, Mat4::translate(eye) * world, color, params, eyeL);
}

void Renderer::drawImage(const GpuLabel& label, const Mat4& world, float brightness) {
    const float color[4] = {brightness, brightness, brightness, 1.0f};
    const float params[4] = {0, 2.0f, 1, 0};
    draw(solidPipeline_, label.solidSRB, cube_, world, color, params, lightDir_);
}

void Renderer::drawFlat(const GpuLabel& label, const Mat4& world, Vec3 c) {
    const float color[4] = {c.x, c.y, c.z, 1.0f};
    const float params[4] = {0, 3.0f, 1, 0};
    draw(solidPipeline_, label.solidSRB, cube_, world, color, params, lightDir_);
}

std::shared_ptr<GpuLabel> Renderer::createDynamicLabel(int w, int h) {
    auto label = std::make_shared<GpuLabel>();
    TextureDesc td;
    td.Name = "DynamicLabel";
    td.Type = RESOURCE_DIM_TEX_2D;
    td.Width = static_cast<Uint32>(w);
    td.Height = static_cast<Uint32>(h);
    td.MipLevels = 1;
    td.Format = TEX_FORMAT_RGBA8_UNORM_SRGB;
    td.BindFlags = BIND_SHADER_RESOURCE;
    td.Usage = USAGE_DEFAULT;
    std::vector<uint8_t> black(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
    TextureSubResData sub{black.data(), static_cast<Uint64>(w) * 4};
    TextureData data{&sub, 1};
    device_->CreateTexture(td, &data, &label->texture);
    label->aspect = static_cast<float>(w) / static_cast<float>(h);
    bindLabel(*label);
    return label;
}

void Renderer::updateLabelRect(GpuLabel& label, int x, int y, int rw, int rh, const uint8_t* rgba, size_t strideBytes) {
    const TextureDesc& d = label.texture->GetDesc();
    if (x < 0 || y < 0 || rw <= 0 || rh <= 0 || static_cast<Uint32>(x + rw) > d.Width || static_cast<Uint32>(y + rh) > d.Height)
        return;
    Box box{static_cast<Uint32>(x), static_cast<Uint32>(x + rw), static_cast<Uint32>(y), static_cast<Uint32>(y + rh)};
    TextureSubResData sub{rgba, static_cast<Uint64>(strideBytes)};
    context_->UpdateTexture(label.texture, 0, 0, box, sub, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void Renderer::drawWebRegion(const GpuLabel& label, const Mat4& world, const float uvRect[4], float aspect, float radius,
                             float rim, float opacity, const float tint[3]) {
    const float color[4] = {tint[0], tint[1], tint[2], opacity};
    const float params[4] = {0.0f, radius, aspect, rim};
    draw(webPipeline_, label.webSRB, quad_, world, color, params, uvRect);
}

void Renderer::drawGlass(const Mat4& world, float aspect, float radius, float opacity) {
    if (!glassLabel_) {
        Bitmap bm;
        bm.w = bm.h = 2;
        bm.rgba.assign(16, 255);
        glassLabel_ = createLabel(bm, false);
    }
    const float color[4] = {0, 0, 0, opacity};
    const float params[4] = {3.0f, radius, aspect, 0.0f};
    const float light[4] = {0, 0, 1, 1};
    draw(webPipeline_, glassLabel_->webSRB, quad_, world, color, params, light);
}

void Renderer::drawWebShadow(const GpuLabel& label, const Mat4& world, const float c[3], float intensity, float innerX,
                             float innerY) {
    const float color[4] = {c[0], c[1], c[2], intensity};
    const float params[4] = {1.0f, 0, 1, 0};
    const float inner[4] = {innerX, innerY, 0, 0};
    draw(webPipeline_, label.webSRB, quad_, world, color, params, inner);
}

void Renderer::updateLabel(GpuLabel& label, const uint8_t* rgba, int w, int h) {
    const TextureDesc& d = label.texture->GetDesc();
    if (static_cast<Uint32>(w) != d.Width || static_cast<Uint32>(h) != d.Height) return;
    Box box{0, d.Width, 0, d.Height};
    TextureSubResData sub{rgba, static_cast<Uint64>(w) * 4};
    context_->UpdateTexture(label.texture, 0, 0, box, sub, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

}  // namespace sf
