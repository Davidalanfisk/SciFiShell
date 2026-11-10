#include "AppShell.h"
#include "ArcadeRoom.h"
#include "SystemDriveRoot.h"
#include <SDL.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#include <windows.h>
#include <commdlg.h>
#endif

namespace sf {
namespace {
void browseArcadeRom(SDL_Window* window,Arcade& arcade) {
#ifdef _WIN32
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if(!SDL_GetWindowWMInfo(window,&wm)) {
        arcade.browseResult({},std::string("CANNOT OPEN ROM PICKER: ")+SDL_GetError());
        return;
    }
    wchar_t path[32768]{};
    std::wstring root;std::string rootError;
    if(!systemDriveRoot(root,rootError)) {arcade.browseResult({},rootError);return;}
    // A bare directory in lpstrFile is rejected by the Windows common dialog.
    const std::wstring initialFile=root+L"*.nes";
    if(initialFile.size()>=std::size(path)) {arcade.browseResult({},"WINDOWS SYSTEM DRIVE PATH IS TOO LONG");return;}
    std::copy(initialFile.begin(),initialFile.end(),path);
    OPENFILENAMEW dialog{};
    dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=wm.info.win.window;
    dialog.lpstrFilter=L"NES ROM files (*.nes)\0*.nes\0\0";
    dialog.lpstrFile=path;
    dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.lpstrTitle=L"Select a lawful NES ROM";
    dialog.lpstrInitialDir=root.c_str();
    dialog.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    const bool relative=SDL_GetRelativeMouseMode()==SDL_TRUE;
    if(relative)SDL_SetRelativeMouseMode(SDL_FALSE);
    const bool selected=GetOpenFileNameW(&dialog)!=FALSE;
    const DWORD error=selected?0:CommDlgExtendedError();
    if(relative)SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_KEYDOWN);SDL_FlushEvent(SDL_KEYUP);
    SDL_FlushEvent(SDL_MOUSEBUTTONDOWN);SDL_FlushEvent(SDL_MOUSEBUTTONUP);
    SDL_FlushEvent(SDL_MOUSEMOTION);SDL_FlushEvent(SDL_MOUSEWHEEL);SDL_FlushEvent(SDL_TEXTINPUT);
    if(selected)arcade.browseResult(std::filesystem::path(path),{});
    else if(error)arcade.browseResult({},"ROM PICKER FAILED: "+std::to_string(error));
    else arcade.browseResult({},{});
#else
    (void)window;
    arcade.browseResult({},"ROM PICKER IS NOT AVAILABLE; DROP A .NES FILE");
#endif
}
}
Plugin AppShell::makeArcadePlugin() {
    Plugin plugin{kDowntownArcadeId,"NEON CIRCUIT ARCADE",
        "Original games, configurable cabinets and loft; trusted native code","1.0.0",
        []{},[this]{arcade_->stop();}};
    plugin.prepare=[this](std::string& error) {
        char* base=SDL_GetBasePath();
        if(!base) {error=std::string("Cannot locate arcade module: ")+SDL_GetError();return false;}
        const auto path=std::filesystem::u8path(base)/SCIFI_ARCADE_MODULE_FILENAME;
        SDL_free(base); return arcade_->prepare(path,error);
    };
    plugin.cancelPrepare=[this]{arcade_->stop();};
    return plugin;
}
void AppShell::selectArcade(int cabinet) {
    if(!arcade_ || !arcade_->running() || downtownInterior_!=kDowntownArcadeId)return;
    if(!arcade_->engaged())arcadeReturn_=camera_;
    releaseGameplayControl();
    arcade_->select(cabinet);
    const auto p=arcade::cabinet(cabinet);
    camera_.position={p.x,1.7f,p.z+1.35f};camera_.yaw=camera_.pitch=0;
    flying_=false;vy_=0;arcadeMouseY_=0;
    arcadeVrX_={};arcadeVrY_={};arcadePadX_={};arcadePadY_={};
    arcadePadBack_=arcadePadStart_=false;
    arcadePickerRelease_=false;
}
void AppShell::tickArcade(double dt,FrameInput& in,PadState& pad) {
    if(!arcade_ || !arcade_->engaged())return;
    const bool focused=(SDL_GetWindowFlags(window_)&SDL_WINDOW_INPUT_FOCUS)!=0 || arcadeScriptFocus_;
    const bool blocked=!focused || downtownInterior_!=kDowntownArcadeId || focus_.p || settingsPanel_->isOpen() ||
        start_->isOpen() || switcher_->isOpen() || viewer_->active() || appsPanel_->isOpen() || osk_->isOpen() || harvi_->panelOpen();
    if(blocked) {
        arcade_->leave(); camera_=arcadeReturn_; return;
    }
    if(!in.dropped.empty()) {
        if(in.dropped.size()==1)arcade_->importRom(std::filesystem::u8path(in.dropped.front()));
        else menu_->setStatus("ARCADE - drop exactly one lawful NES ROM");
        in.dropped.clear();
    }
    if(arcadePickerRelease_) {
        const bool held=pad.aHeld||pad.bHeld||pad.startHeld||pad.backHeld||pad.xHeld||pad.rt>.5f||
            vrInput_.selectHeld||vrInput_.kbdHeld||vrInput_.backPressed||vrInput_.menuPressed;
        if(!held)arcadePickerRelease_=false;
        const bool quit=in.quit,resized=in.resized,screenshot=in.screenshot;
        in=FrameInput{};in.quit=quit;in.resized=resized;in.screenshot=screenshot;
        pad=PadState{};
        vrInput_.moveX=vrInput_.moveY=vrInput_.lookX=vrInput_.lookY=0;
        vrInput_.selectPressed=vrInput_.backPressed=vrInput_.menuPressed=vrInput_.kbdPressed=vrInput_.switchPressed=false;
        vrInput_.selectHeld=vrInput_.kbdHeld=false;
        return;
    }
    const auto* keys=SDL_GetKeyboardState(nullptr);
    const float kx=float(keys[SDL_SCANCODE_D]||keys[SDL_SCANCODE_RIGHT])-float(keys[SDL_SCANCODE_A]||keys[SDL_SCANCODE_LEFT]);
    const float ky=float(keys[SDL_SCANCODE_S]||keys[SDL_SCANCODE_DOWN])-float(keys[SDL_SCANCODE_W]||keys[SDL_SCANCODE_UP]);
    arcadeMouseY_=clampf(arcadeMouseY_+in.lookY*.008f,-1,1);
    float x=clampf(kx+pad.lx+vrInput_.moveX+float(pad.rightHeld)-float(pad.leftHeld),-1,1);
    float y=clampf(ky-pad.ly-vrInput_.moveY+float(pad.downHeld)-float(pad.upHeld),-1,1);
    if(arcade_->state().game==0 && std::fabs(y)<.1f)y=arcadeMouseY_;
    const bool mouse=(SDL_GetMouseState(nullptr,nullptr)&SDL_BUTTON(SDL_BUTTON_LEFT))!=0;
    arcade_->controls(dt,x,y,keys[SDL_SCANCODE_SPACE]||mouse||pad.aHeld||pad.rt>.5f||vrInput_.selectHeld||vrInput_.selectPressed,
        keys[SDL_SCANCODE_LSHIFT]||pad.xHeld||vrInput_.kbdHeld||vrInput_.kbdPressed,
        int(in.down||pad.downPressed)-int(in.up||pad.upPressed)+int(in.wheel<0)-int(in.wheel>0)+
            -arcadeVrY_.step(vrInput_.moveY)-arcadePadY_.step(pad.ly),
        int(in.right||pad.rightPressed)-int(in.left||pad.leftPressed)+arcadeVrX_.step(vrInput_.moveX)+arcadePadX_.step(pad.lx),
        in.select||in.click||pad.aPressed||(pad.startHeld&&!arcadePadStart_)||vrInput_.selectPressed,
        in.escape||in.back||pad.bPressed||(pad.backHeld&&!arcadePadBack_)||vrInput_.backPressed,
        keys[SDL_SCANCODE_RETURN]||pad.startHeld||vrInput_.menuPressed,
        keys[SDL_SCANCODE_TAB]||pad.lbHeld||vrInput_.switchPressed);
    if(arcade_->takeBrowseRequest()) {
        browseArcadeRom(window_,*arcade_);
        arcadePickerRelease_=true;
    }
    arcadePadBack_=pad.backHeld;arcadePadStart_=pad.startHeld;
    if(!arcade_->engaged())camera_=arcadeReturn_;
    const bool quit=in.quit,resized=in.resized,screenshot=in.screenshot;
    in=FrameInput{};in.quit=quit;in.resized=resized;in.screenshot=screenshot;
    pad=PadState{};
    vrInput_.moveX=vrInput_.moveY=vrInput_.lookX=vrInput_.lookY=0;
    vrInput_.selectPressed=vrInput_.backPressed=vrInput_.menuPressed=vrInput_.kbdPressed=vrInput_.switchPressed=false;
    vrInput_.selectHeld=vrInput_.kbdHeld=false;
}
}
