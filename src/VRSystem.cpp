#include "VRSystem.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#    include <d3d12.h>
#    include <unknwn.h>
#    define XR_USE_PLATFORM_WIN32
#    define XR_USE_GRAPHICS_API_D3D12
#    include <openxr/openxr.h>
#    include <openxr/openxr_platform.h>

#    include "CommandQueueD3D12.h"
#    include "RenderDeviceD3D12.h"

using namespace Diligent;

namespace sf {

namespace {
constexpr int64_t kSwapchainFormat = 29;  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
constexpr float kNear = 0.05f, kFar = 2500.0f;

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    std::vector<RefCntAutoPtr<ITexture>> textures;
    std::vector<RefCntAutoPtr<ITextureView>> views;
    RefCntAutoPtr<ITexture> depth;
    RefCntAutoPtr<ITextureView> depthView;
};
}  // namespace

struct VRSystem::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace stageSpace = XR_NULL_HANDLE;
    XrSpace aimSpace = XR_NULL_HANDLE;
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction poseAction = XR_NULL_HANDLE, selectAction = XR_NULL_HANDLE, moveAction = XR_NULL_HANDLE,
             backAction = XR_NULL_HANDLE, menuAction = XR_NULL_HANDLE;
    XrPath leftHand = XR_NULL_PATH, rightHand = XR_NULL_PATH;
    Eye eyes[2];
    bool sessionRunning = false;
    bool prevSelect = false, prevBack = false, prevMenu = false;

    XrPath path(const char* s) const {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance, s, &p);
        return p;
    }
    XrAction makeAction(const char* name, const char* localized, XrActionType type, const XrPath* subs, uint32_t n) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = type;
        std::strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(ci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        ci.countSubactionPaths = n;
        ci.subactionPaths = subs;
        XrAction a = XR_NULL_HANDLE;
        xrCreateAction(actionSet, &ci, &a);
        return a;
    }
    void suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& b) {
        std::vector<XrActionSuggestedBinding> sb;
        for (auto& p : b) sb.push_back({p.first, path(p.second)});
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = path(profile);
        s.suggestedBindings = sb.data();
        s.countSuggestedBindings = static_cast<uint32_t>(sb.size());
        xrSuggestInteractionProfileBindings(instance, &s);
    }
    bool setupActions() {
        XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(asi.actionSetName, "shell");
        std::strcpy(asi.localizedActionSetName, "Shell");
        if (XR_FAILED(xrCreateActionSet(instance, &asi, &actionSet))) return false;
        leftHand = path("/user/hand/left");
        rightHand = path("/user/hand/right");
        XrPath both[2] = {leftHand, rightHand};
        poseAction = makeAction("pointer", "Pointer", XR_ACTION_TYPE_POSE_INPUT, both, 2);
        selectAction = makeAction("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT, both, 2);
        moveAction = makeAction("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, both, 2);
        backAction = makeAction("back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT, both, 2);
        menuAction = makeAction("tools", "Tool ring", XR_ACTION_TYPE_BOOLEAN_INPUT, both, 2);

        suggest("/interaction_profiles/oculus/touch_controller",
                {{poseAction, "/user/hand/right/input/aim/pose"},
                 {selectAction, "/user/hand/right/input/trigger/value"},
                 {moveAction, "/user/hand/left/input/thumbstick"},
                 {menuAction, "/user/hand/left/input/y/click"},
                 {backAction, "/user/hand/right/input/b/click"}});
        suggest("/interaction_profiles/valve/index_controller",
                {{poseAction, "/user/hand/right/input/aim/pose"},
                 {selectAction, "/user/hand/right/input/trigger/value"},
                 {moveAction, "/user/hand/left/input/thumbstick"},
                 {menuAction, "/user/hand/left/input/b/click"},
                 {backAction, "/user/hand/right/input/b/click"}});
        suggest("/interaction_profiles/htc/vive_controller",
                {{poseAction, "/user/hand/right/input/aim/pose"},
                 {selectAction, "/user/hand/right/input/trigger/value"},
                 {moveAction, "/user/hand/left/input/trackpad"},
                 {menuAction, "/user/hand/left/input/menu/click"},
                 {backAction, "/user/hand/right/input/menu/click"}});
        suggest("/interaction_profiles/khr/simple_controller",
                {{poseAction, "/user/hand/right/input/aim/pose"},
                 {selectAction, "/user/hand/right/input/select/click"},
                 {backAction, "/user/hand/right/input/menu/click"}});

        XrSessionActionSetsAttachInfo att{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        att.countActionSets = 1;
        att.actionSets = &actionSet;
        if (XR_FAILED(xrAttachSessionActionSets(session, &att))) return false;

        XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action = poseAction;
        si.subactionPath = rightHand;
        si.poseInActionSpace.orientation.w = 1.0f;
        return XR_SUCCEEDED(xrCreateActionSpace(session, &si, &aimSpace));
    }
};

VRSystem::VRSystem() : impl_(new Impl) {}
VRSystem::~VRSystem() { stop(); }

bool VRSystem::running() const { return impl_->session != XR_NULL_HANDLE; }

void VRSystem::stop() {
    Impl& d = *impl_;
    for (Eye& e : d.eyes) {
        if (e.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(e.swapchain);
        e = Eye{};
    }
    if (d.aimSpace) xrDestroySpace(d.aimSpace);
    if (d.stageSpace) xrDestroySpace(d.stageSpace);
    if (d.actionSet) xrDestroyActionSet(d.actionSet);
    if (d.session) {
        if (d.sessionRunning) xrEndSession(d.session);
        xrDestroySession(d.session);
    }
    if (d.instance) xrDestroyInstance(d.instance);
    d.aimSpace = d.stageSpace = XR_NULL_HANDLE;
    d.actionSet = XR_NULL_HANDLE;
    d.session = XR_NULL_HANDLE;
    d.instance = XR_NULL_HANDLE;
    d.sessionRunning = false;
}

bool VRSystem::start(Renderer& renderer, std::string& error) {
    Impl& d = *impl_;
    if (d.instance) return true;
    auto fail = [&](const char* msg) {
        error = msg;
        stop();
        return false;
    };

    const char* ext = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ici.applicationInfo.applicationName, "SciFiShell");
    std::strcpy(ici.applicationInfo.engineName, "SciFiShell");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = &ext;
    if (XR_FAILED(xrCreateInstance(&ici, &d.instance))) return fail("No OpenXR runtime");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(d.instance, &sgi, &d.systemId))) return fail("No headset found");

    PFN_xrGetD3D12GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(d.instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    if (!getReqs) return fail("D3D12 NOT SUPPORTED");
    XrGraphicsRequirementsD3D12KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (XR_FAILED(getReqs(d.instance, d.systemId, &reqs))) return fail("D3D12 REQUIREMENTS FAILED");

    RefCntAutoPtr<IRenderDeviceD3D12> device12(renderer.device(), IID_RenderDeviceD3D12);
    if (!device12) return fail("NOT A D3D12 DEVICE");
    ICommandQueue* cq = renderer.context()->LockCommandQueue();
    RefCntAutoPtr<ICommandQueueD3D12> queue12(cq, IID_CommandQueueD3D12);
    ID3D12CommandQueue* nativeQueue = queue12 ? queue12->GetD3D12CommandQueue() : nullptr;
    renderer.context()->UnlockCommandQueue();
    if (!nativeQueue) return fail("NO D3D12 QUEUE");

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device12->GetD3D12Device();
    binding.queue = nativeQueue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = d.systemId;
    if (XR_FAILED(xrCreateSession(d.instance, &sci, &d.session))) return fail("SESSION CREATE FAILED");

    XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(d.session, &rsi, &d.stageSpace))) {
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (XR_FAILED(xrCreateReferenceSpace(d.session, &rsi, &d.stageSpace))) return fail("NO REFERENCE SPACE");
    }
    if (!d.setupActions()) return fail("ACTION SETUP FAILED");

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(d.instance, d.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    if (viewCount != 2) return fail("STEREO NOT SUPPORTED");
    std::vector<XrViewConfigurationView> cfg(2, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(d.instance, d.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, cfg.data());

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(d.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(d.session, fmtCount, &fmtCount, formats.data());
    bool haveFormat = false;
    for (int64_t f : formats) haveFormat |= (f == kSwapchainFormat);
    if (!haveFormat) return fail("NO SRGB SWAPCHAIN FORMAT");

    for (int i = 0; i < 2; ++i) {
        Eye& e = d.eyes[i];
        e.width = cfg[i].recommendedImageRectWidth;
        e.height = cfg[i].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = kSwapchainFormat;
        sc.sampleCount = 1;
        sc.width = e.width;
        sc.height = e.height;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(d.session, &sc, &e.swapchain))) return fail("SWAPCHAIN CREATE FAILED");

        uint32_t n = 0;
        xrEnumerateSwapchainImages(e.swapchain, 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> imgs(n, XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(e.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (auto& img : imgs) {
            RefCntAutoPtr<ITexture> tex;
            device12->CreateTextureFromD3DResource(img.texture, RESOURCE_STATE_RENDER_TARGET, &tex);
            if (!tex) return fail("TEXTURE WRAP FAILED");
            TextureViewDesc vd;
            vd.ViewType = TEXTURE_VIEW_RENDER_TARGET;
            vd.Format = TEX_FORMAT_RGBA8_UNORM_SRGB;
            RefCntAutoPtr<ITextureView> view;
            tex->CreateView(vd, &view);
            if (!view) return fail("RTV CREATE FAILED");
            e.textures.push_back(tex);
            e.views.push_back(view);
        }
        e.depth = renderer.createDepthTexture(e.width, e.height);
        e.depthView = e.depth->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);
    }
    return true;
}

void VRSystem::frame(Renderer& renderer, const Mat4& rig, const DrawScene& drawScene, VRInput& input) {
    Impl& d = *impl_;
    input = VRInput{};
    if (!d.session) return;

    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(d.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                d.sessionRunning = XR_SUCCEEDED(xrBeginSession(d.session, &bi));
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(d.session);
                d.sessionRunning = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                d.sessionRunning = false;
            }
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!d.sessionRunning) return;

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(d.session, nullptr, &fs))) return;
    if (XR_FAILED(xrBeginFrame(d.session, nullptr))) return;

    // Input
    XrActiveActionSet active{d.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_SUCCEEDED(xrSyncActions(d.session, &sync))) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = d.selectAction;
        gi.subactionPath = d.rightHand;
        xrGetActionStateBoolean(d.session, &gi, &b);
        bool sel = b.isActive && b.currentState;
        input.selectPressed = sel && !d.prevSelect;
        d.prevSelect = sel;

        b = XrActionStateBoolean{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = d.backAction;
        xrGetActionStateBoolean(d.session, &gi, &b);
        bool back = b.isActive && b.currentState;
        input.backPressed = back && !d.prevBack;
        d.prevBack = back;

        b = XrActionStateBoolean{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = d.menuAction;
        gi.subactionPath = d.leftHand;
        xrGetActionStateBoolean(d.session, &gi, &b);
        bool menu = b.isActive && b.currentState;
        input.menuPressed = menu && !d.prevMenu;
        d.prevMenu = menu;
        gi.subactionPath = d.rightHand;

        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = d.moveAction;
        gi.subactionPath = d.leftHand;
        xrGetActionStateVector2f(d.session, &gi, &v);
        if (v.isActive) {
            input.moveX = v.currentState.x;
            input.moveY = v.currentState.y;
        }

        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(d.aimSpace, d.stageSpace, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            const auto& q = loc.pose.orientation;
            const auto& p = loc.pose.position;
            Mat4 world = rig * Mat4::fromPose(q.x, q.y, q.z, q.w, {p.x, p.y, p.z});
            input.valid = true;
            input.pointerOrigin = world.transformPoint({0, 0, 0});
            input.pointerDir = normalize(world.transformDir({0, 0, -1}));
        }
    }

    XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {nullptr};
    uint32_t layerCount = 0;

    bool rendered = false;
    if (fs.shouldRender) {
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = d.stageSpace;
        uint32_t count = 0;
        if (XR_SUCCEEDED(xrLocateViews(d.session, &li, &vs, 2, &count, views)) && count == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            rendered = true;
            for (int i = 0; i < 2; ++i) {
                Eye& e = d.eyes[i];
                uint32_t idx = 0;
                xrAcquireSwapchainImage(e.swapchain, nullptr, &idx);
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(e.swapchain, &wi);

                const auto& q = views[i].pose.orientation;
                const auto& p = views[i].pose.position;
                const auto& f = views[i].fov;
                EyeView ev2;
                ev2.view = (rig * Mat4::fromPose(q.x, q.y, q.z, q.w, {p.x, p.y, p.z})).rigidInverse();
                ev2.proj = Mat4::perspectiveTan(std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp),
                                                std::tan(f.angleDown), kNear, kFar);
                ev2.width = e.width;
                ev2.height = e.height;

                renderer.beginTarget(e.views[idx], e.depthView, e.width, e.height);
                drawScene(ev2);
                renderer.endTarget();
                renderer.context()->Flush();
                xrReleaseSwapchainImage(e.swapchain, nullptr);

                projViews[i].pose = views[i].pose;
                projViews[i].fov = views[i].fov;
                projViews[i].subImage.swapchain = e.swapchain;
                projViews[i].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(e.width), static_cast<int32_t>(e.height)}};
                projViews[i].subImage.imageArrayIndex = 0;
            }
            layer.space = d.stageSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
            layerCount = 1;
        }
    }
    (void)rendered;

    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = layerCount;
    fe.layers = layerCount ? layers : nullptr;
    xrEndFrame(d.session, &fe);
}

}  // namespace sf

#else  // ---- Non-Windows: OpenXR graphics binding not implemented for this backend ----

namespace sf {

struct VRSystem::Impl {};

VRSystem::VRSystem() : impl_(new Impl) {}
VRSystem::~VRSystem() = default;
bool VRSystem::running() const { return false; }
void VRSystem::stop() {}
bool VRSystem::start(Renderer&, std::string& error) {
    error = "VR NEEDS WINDOWS D3D12 BUILD";
    return false;
}
void VRSystem::frame(Renderer&, const Mat4&, const DrawScene&, VRInput& input) { input = VRInput{}; }

}  // namespace sf

#endif
