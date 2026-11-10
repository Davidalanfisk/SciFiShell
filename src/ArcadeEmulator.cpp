#include "ArcadeEmulator.h"
#include "ArcadeInput.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <thread>
#include <chrono>

namespace sf {
bool readArcadeRom(const std::filesystem::path& path,std::string& bytes,std::string& error) {
    bytes.clear();
    std::error_code ec;
    const auto status=std::filesystem::symlink_status(path,ec);
    if(ec || !std::filesystem::is_regular_file(status)) { error="SELECT A REGULAR LOCAL .NES FILE"; return false; }
    if(path.extension()!=".nes" && path.extension()!=".NES") { error="ONLY .NES FILES ARE SUPPORTED"; return false; }
    const auto size=std::filesystem::file_size(path,ec);
    if(ec || size<16 || size>1024*1024) { error="NES IMPORT LIMIT: 16 BYTES TO 1 MIB"; return false; }
    std::ifstream in(path,std::ios::binary);
    bytes.resize(static_cast<size_t>(size)); in.read(bytes.data(),static_cast<std::streamsize>(size));
    if(!in || in.peek()!=std::char_traits<char>::eof()) { error="CANNOT READ STABLE ROM FILE"; bytes.clear(); return false; }
    auto b=[&](int i){return static_cast<unsigned char>(bytes[i]);};
    const bool nes2=(b(7)&12)==8;
    if(bytes.compare(0,4,"NES\x1a",4) || ((b(7)&12)!=0 && !nes2) || (b(6)&0xfe) ||
       (b(7)&0xf3) || (b(4)!=1 && b(4)!=2) || b(5)>1) {
        error="REQUIRES STANDARD NES MAPPER 0, NO TRAINER/BATTERY, 16/32K PRG"; bytes.clear(); return false;
    }
    const auto expected=16u+size_t(b(4))*16384+size_t(b(5))*8192;
    if(bytes.size()!=expected) { error="TRUNCATED OR TRAILING NES ROM DATA"; bytes.clear(); return false; }
    if(nes2) {
        if(b(8)!=0 || b(9)!=0 || b(10)!=0 || b(11)!=(b(5)==0?7:0) ||
           (b(12)!=0 && b(12)!=2) || b(13)!=0 || b(14)!=0 || b(15)>1) {
            error="NES 2.0 REQUIRES NTSC/MULTIREGION NROM AND STANDARD CONTROLLERS"; bytes.clear(); return false;
        }
    } else for(int i=8;i<16;++i)if(b(i)!=0) {
        error="UNSUPPORTED iNES HEADER EXTENSION"; bytes.clear(); return false;
    }
    return true;
}
}

#ifdef SCIFI_HAS_CEF
#include "GameplayDigest.h"
#include "WebBrowser.h"
#include "include/cef_client.h"
#include "include/cef_app.h"
#include <cstdio>

namespace sf {
namespace {
std::string base64(const std::string& s) {
    const char* chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve((s.size()+2)/3*4);
    for(size_t i=0;i<s.size();i+=3) {
        uint32_t n=uint32_t(uint8_t(s[i]))<<16;
        if(i+1<s.size())n|=uint32_t(uint8_t(s[i+1]))<<8;
        if(i+2<s.size())n|=uint8_t(s[i+2]);
        out+=chars[n>>18];out+=chars[(n>>12)&63];
        out+=i+1<s.size()?chars[(n>>6)&63]:'=';
        out+=i+2<s.size()?chars[n&63]:'=';
    }
    return out;
}
struct EmulatorState {
    CefRefPtr<CefBrowser> browser;
    std::vector<uint8_t> pixels=std::vector<uint8_t>(320*240*4,0);
    std::string error;
    bool ready=false;
    uint64_t frames=0;
};
std::weak_ptr<EmulatorState> closingSurface;
class EmulatorClient final:public CefClient,public CefRenderHandler,public CefLifeSpanHandler,
    public CefDisplayHandler,public CefRequestHandler,public CefResourceRequestHandler,public CefLoadHandler {
public:
    std::shared_ptr<EmulatorState> state;
    std::string script;
    explicit EmulatorClient(std::shared_ptr<EmulatorState> s):state(std::move(s)){}
    CefRefPtr<CefRenderHandler> GetRenderHandler() override {return this;}
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {return this;}
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {return this;}
    CefRefPtr<CefRequestHandler> GetRequestHandler() override {return this;}
    CefRefPtr<CefLoadHandler> GetLoadHandler() override {return this;}
    void OnLoadEnd(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int) override {
        if(frame->IsMain() && !script.empty()) {
            frame->ExecuteJavaScript(script,"about:blank",1); script.clear();
        }
    }
    void GetViewRect(CefRefPtr<CefBrowser>,CefRect& rect) override {rect={0,0,320,240};}
    void OnPaint(CefRefPtr<CefBrowser>,PaintElementType type,const RectList&,const void* buffer,int w,int h) override {
        if(type!=PET_VIEW || w!=320 || h!=240)return;
        if(state->ready)++state->frames;
        const auto* src=static_cast<const uint8_t*>(buffer);
        for(size_t i=0;i<state->pixels.size();i+=4) {
            state->pixels[i]=src[i+2];state->pixels[i+1]=src[i+1];state->pixels[i+2]=src[i];state->pixels[i+3]=255;
        }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {state->browser=nullptr;state->ready=false;}
    bool OnBeforePopup(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,int,const CefString&,const CefString&,
        cef_window_open_disposition_t,bool,const CefPopupFeatures&,CefWindowInfo&,CefRefPtr<CefClient>&,
        CefBrowserSettings&,CefRefPtr<CefDictionaryValue>&,bool*) override {return true;}
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,CefRefPtr<CefRequest> request,bool,bool) override {
        return request->GetURL().ToString()!="about:blank";
    }
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,
        CefRefPtr<CefRequest>,bool,bool,const CefString&,bool& disableDefault) override {
        disableDefault=true; return this;
    }
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,CefRefPtr<CefRequest>,
        CefRefPtr<CefCallback>) override {return RV_CANCEL;}
    bool OnConsoleMessage(CefRefPtr<CefBrowser>,cef_log_severity_t,const CefString& message,const CefString&,int) override {
        const auto text=message.ToString();
        if(text=="ARCADE_READY")state->ready=true;
        if(text.rfind("ARCADE_ERROR:",0)==0) {
            state->error=text.substr(0,150); std::fprintf(stderr,"%s\n",state->error.c_str());
        }
        return true;
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>,TerminationStatus,int,const CefString&) override {
        state->error="NES RENDERER STOPPED";state->ready=false;
    }
private:
    IMPLEMENT_REFCOUNTING(EmulatorClient);
};
}
struct ArcadeEmulator::Impl {
    std::shared_ptr<EmulatorState> state=std::make_shared<EmulatorState>();
    double accumulator=0,timeout=0;
    ArcadeButtons buttons;
    float volume=-1;
};
ArcadeEmulator::ArcadeEmulator():impl_(std::make_unique<Impl>()){}
ArcadeEmulator::~ArcadeEmulator() {
    auto s=impl_->state;
    if(!s->browser)return;
    closingSurface=s;
    s->ready=false;
    auto browser=s->browser;
    browser->GetHost()->SetAudioMuted(true);
    browser->GetHost()->WasHidden(true);
    browser->GetHost()->CloseBrowser(true);
    // Keep callbacks alive until OnBeforeClose; never unload an active page into the next cabinet.
    for(int i=0;s->browser && i<100;++i) {
        WebBrowser::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
bool ArcadeEmulator::start(const std::filesystem::path& engine,const std::filesystem::path& rom,std::string& error) {
    if(auto closing=closingSurface.lock();closing && closing->browser) {
        error="PREVIOUS NES SURFACE IS STILL CLOSING - RETRY";return false;
    }
    std::string bytes;
    if(!readArcadeRom(rom,bytes,error))return false;
    // Validated NES 2.0 NROM needs no extended hardware; adapt only the in-memory header for JSNES.
    if((static_cast<unsigned char>(bytes[7])&12)==8) {
        bytes[7]=0;std::fill(bytes.begin()+8,bytes.begin()+16,0);
    }
    std::error_code ec;
    const auto size=std::filesystem::file_size(engine,ec);
    if(ec || size>1024*1024) {error="BUNDLED NES ENGINE MISSING OR OVERSIZED";return false;}
    std::ifstream file(engine,std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(file)),{});
    GameplayDigest digest;
    if(!file || !gameplayDigestBytes(source,digest,error)) {error="CANNOT READ NES ENGINE";return false;}
    constexpr unsigned char expected[]={0x84,0x94,0x6f,0x0d,0xae,0x4b,0xcf,0x3b,0x2d,0xfe,0xbe,0x84,0xad,0x3b,0xfe,0xb1,
        0xdb,0xcb,0x41,0x3b,0xd9,0xb1,0xa6,0x9e,0x1f,0x45,0x91,0x0a,0x95,0xfd,0x7b,0x41};
    if(!std::equal(digest.sha256.begin(),digest.sha256.end(),expected)) {error="NES ENGINE INTEGRITY CHECK FAILED";return false;}
    if(!WebBrowser::ensureCef()) {error="CEF INITIALIZATION FAILED";return false;}
    CefWindowInfo window; window.SetAsWindowless(nullptr);
    CefBrowserSettings settings; settings.windowless_frame_rate=60;
    CefRequestContextSettings context;
    auto requestContext=CefRequestContext::CreateContext(context,nullptr);
    auto client=CefRefPtr<EmulatorClient>(new EmulatorClient(impl_->state));
    auto browser=CefBrowserHost::CreateBrowserSync(window,client,"about:blank",settings,nullptr,requestContext);
    if(!browser) {error="CANNOT CREATE ISOLATED NES SURFACE";return false;}
    impl_->state->browser=browser;
    source+="\n;(()=>{try{";
    source+=R"JS(
document.head.innerHTML='<meta http-equiv="Content-Security-Policy" content="default-src &#39;none&#39;; script-src &#39;unsafe-inline&#39;; style-src &#39;unsafe-inline&#39;; connect-src &#39;none&#39;; media-src &#39;none&#39;; frame-src &#39;none&#39;">';
document.body.style='margin:0;background:#080c19;overflow:hidden';
const canvas=document.createElement('canvas');canvas.width=256;canvas.height=240;
canvas.style='width:320px;height:240px;image-rendering:pixelated';document.body.appendChild(canvas);
const ctx=canvas.getContext('2d'),image=ctx.createImageData(256,240);
const audio=new AudioContext({sampleRate:48000}),queue=new Float32Array(32768);
let read=0,write=0;
const node=audio.createScriptProcessor(1024,0,2);
node.onaudioprocess=e=>{const l=e.outputBuffer.getChannelData(0),r=e.outputBuffer.getChannelData(1);
for(let i=0;i<l.length;i++){if(read!==write){l[i]=queue[read];r[i]=queue[(read+1)%queue.length];read=(read+2)%queue.length;}else{l[i]=r[i]=0;}}};
const gain=audio.createGain();gain.gain.value=0;node.connect(gain);gain.connect(audio.destination);audio.resume();
const nes=new jsnes.NES({onFrame:buffer=>{for(let i=0;i<buffer.length;i++){
const c=buffer[i],j=i*4;image.data[j]=c&255;image.data[j+1]=(c>>8)&255;image.data[j+2]=(c>>16)&255;image.data[j+3]=255;}
ctx.putImageData(image,0,0);},
onAudioSample:(l,r)=>{const next=(write+2)%queue.length;if(next!==read){queue[write]=l;queue[write+1]=r;write=next;}},
sampleRate:48000,emulateSound:true});
window.arcade={volume:v=>{gain.gain.value=v;},step:n=>{try{for(let i=0;i<n;i++)nes.frame();}catch(e){console.error('ARCADE_ERROR:'+String(e).slice(0,120));}},
input:mask=>{for(let i=0;i<8;i++)if(mask&(1<<i))nes.buttonDown(1,i);else nes.buttonUp(1,i);}};
)JS";
    source+="nes.loadROM(atob('"+base64(bytes)+"'));console.log('ARCADE_READY');";
    source+="}catch(e){console.error('ARCADE_ERROR:'+String(e).slice(0,120));}})();";
    client->script=std::move(source);
    return true;
}
void ArcadeEmulator::input(float x,float y,bool a,bool b,bool start,bool select) {
    impl_->buttons.set(uint8_t(int(a)|(int(b)<<1)|(int(select)<<2)|(int(start)<<3)|
        (int(y<-.4f)<<4)|(int(y>.4f)<<5)|(int(x<-.4f)<<6)|(int(x>.4f)<<7)));
}
void ArcadeEmulator::update(double dt) {
    auto s=impl_->state;
    if(!s->ready) {
        impl_->timeout+=dt;
        if(impl_->timeout>8 && s->error.empty())s->error="NES STARTUP TIMED OUT";
        return;
    }
    impl_->accumulator+=std::clamp(dt,0.0,.05);
    const int frames=int(impl_->accumulator*60);
    if(frames>0 && s->browser) {
        impl_->accumulator-=frames/60.0;
        const auto mask=impl_->buttons.sample();
        s->browser->GetMainFrame()->ExecuteJavaScript("window.arcade.input("+std::to_string(mask)+
            ");window.arcade.step("+std::to_string(frames)+");","about:blank",1);
    }
}
void ArcadeEmulator::pixels(std::vector<uint8_t>& rgba) const { if(impl_->state->ready)rgba=impl_->state->pixels; }
const std::string& ArcadeEmulator::error() const { return impl_->state->error; }
uint64_t ArcadeEmulator::frames() const { return impl_->state->frames; }
void ArcadeEmulator::volume(float value) {
    const auto s=impl_->state;
    value=std::clamp(value,0.0f,1.0f);
    if(!s->ready || !s->browser || value==impl_->volume)return;
    impl_->volume=value;
    s->browser->GetMainFrame()->ExecuteJavaScript("window.arcade.volume("+std::to_string(value)+");","about:blank",1);
}
}
#else
namespace sf {
struct ArcadeEmulator::Impl { std::string error="NES REQUIRES A CEF-ENABLED BUILD"; };
ArcadeEmulator::ArcadeEmulator():impl_(std::make_unique<Impl>()){}
ArcadeEmulator::~ArcadeEmulator()=default;
bool ArcadeEmulator::start(const std::filesystem::path&,const std::filesystem::path&,std::string& error) {error=impl_->error;return false;}
void ArcadeEmulator::input(float,float,bool,bool,bool,bool){}
void ArcadeEmulator::update(double){}
void ArcadeEmulator::pixels(std::vector<uint8_t>&) const{}
const std::string& ArcadeEmulator::error() const{return impl_->error;}
uint64_t ArcadeEmulator::frames() const{return 0;}
void ArcadeEmulator::volume(float){}
}
#endif
