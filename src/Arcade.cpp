#include "Arcade.h"
#include "ArcadeEmulator.h"
#include "PluginManager.h"
#include "TextRender.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cmath>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sf {
namespace {
const char* titles[]={"PULSE RALLY","ORBIT BREAKER","SKY RESCUE","RIVER RUNNER","NES - LOCAL ROM"};
constexpr uint64_t audioOwner=0xffffffffffffffe0ull;
std::filesystem::path arcadeDirectory() {
    auto path=PluginManager::defaultStatePath();
    return path.empty()?std::filesystem::path{}:path.parent_path()/"arcade";
}
}
Arcade::Arcade(Renderer& renderer,Sfx& sound):renderer_(renderer),sound_(sound),
    storage_(arcadeDirectory(), "builtin.arcade"),dataDir_(arcadeDirectory()) {}
Arcade::~Arcade() { stop(); }
bool Arcade::prepare(const std::filesystem::path& path,std::string& error) {
    stop();
    if(!storage_.load(error) || !module_.load(path,"org.scifishell.arcade",error) || !module_.start(error)) { stop(); return false; }
    api_=static_cast<const SfArcadeV1*>(module_.getInterface(SF_ARCADE_INTERFACE));
    if(!api_ || api_->size!=sizeof(SfArcadeV1) || api_->abi!=SF_ARCADE_ABI ||
       !api_->begin || !api_->end || !api_->state || !api_->step || !api_->paint) {
        error="Arcade module interface is incompatible"; stop(); return false;
    }
    for(int i=0;i<SF_ARCADE_CABINETS;++i) {
        Config c{i%4,1,0};
        const GameplayStorage::Value* value=nullptr;
        if(!storage_.get("cabinet."+std::to_string(i),value,error)) { stop(); return false; }
        if(value) {
            const auto* text=std::get_if<std::string>(value);
            std::istringstream input(text?*text:"");
            if(!(input>>c.game>>c.difficulty>>c.invert) || (input>>std::ws && !input.eof()) ||
               c.game<0||c.game>4||c.difficulty<1||c.difficulty>3||c.invert<0||c.invert>1) {
                error="Invalid arcade cabinet preferences; preserved without overwriting"; stop(); return false;
            }
        }
        configs_[i]=c;
        screens_[i]=renderer_.createDynamicLabel(SF_ARCADE_WIDTH,SF_ARCADE_HEIGHT);
        if(!screens_[i]) { error="Cannot allocate arcade screen"; stop(); return false; }
    }
    for(int i=0;i<4;++i) {
        const GameplayStorage::Value* value=nullptr;
        if(!storage_.get("high."+std::to_string(i),value,error)) { stop(); return false; }
        highs_[i]=0;
        if(value) {
            auto* number=std::get_if<double>(value);
            if(!number || !std::isfinite(*number) || *number<0 || *number>999999 || std::floor(*number)!=*number) {
                error="Invalid arcade score data; preserved without overwriting"; stop(); return false;
            }
            highs_[i]=int(*number);
        }
    }
    notice_.clear(); previewAt_=0; update(0,0); return true;
}
void Arcade::stop() {
    leave();
    api_=nullptr;
    module_.unload();
    for(auto& screen:screens_)screen.reset();
}
SfArcadeState Arcade::state() const {
    auto s=api_?api_->state(api_->context):SfArcadeState{-1,0,0,0,0,0};
    if(emulator_)s.ticks=emulator_->frames();
    return s;
}
void Arcade::saveScore(SfArcadeState s) {
    if(s.game>=0 && s.game<4 && s.score>highs_[s.game]) {
        std::string error;
        if(storage_.set("high."+std::to_string(s.game),double(s.score),error))highs_[s.game]=s.score;
        else { notice_="SCORE NOT SAVED: "+error; std::fprintf(stderr,"%s\n",notice_.c_str()); }
    }
}
void Arcade::finishGame() {
    sound_.stopOwner(audioOwner);
    emulator_.reset();
    if(api_ && playing_) {
        saveScore(state());
        api_->end(api_->context);
    }
    playing_=false;
}
void Arcade::leave() { finishGame(); selected_=-1; row_=0; browseRequested_=false; previewAt_=0; }
void Arcade::select(int cabinet) {
    if(!api_ || cabinet<0 || cabinet>=SF_ARCADE_CABINETS)return;
    finishGame(); selected_=cabinet; row_=0; notice_.clear(); previewAt_=0;
}
bool Arcade::saveConfig(int cabinet,Config config) {
    std::string error;
    if(!storage_.set("cabinet."+std::to_string(cabinet),std::to_string(config.game)+" "+
                    std::to_string(config.difficulty)+" "+std::to_string(config.invert),error)) {
        notice_="NOT SAVED: "+error; std::fprintf(stderr,"%s\n",notice_.c_str()); return false;
    }
    configs_[cabinet]=config; notice_="CABINET SAVED"; return true;
}
void Arcade::play() {
    if(!api_ || selected_<0)return;
    finishGame();
    const auto c=configs_[selected_];
    if(c.game==4) {
        emulator_=std::make_unique<ArcadeEmulator>();
        std::string error;
        char* base=SDL_GetBasePath();
        const auto engine=std::filesystem::u8path(base?base:"")/"assets"/"arcade"/"jsnes.js";
        SDL_free(base);
        if(!emulator_->start(engine,dataDir_/("cabinet-"+std::to_string(selected_)+".nes"),error)) {
            notice_=error; emulator_.reset(); std::fprintf(stderr,"Arcade NES: %s\n",error.c_str()); return;
        }
    }
    if(!api_->begin(api_->context,selected_,c.game,c.difficulty)) {
        notice_="CANNOT START CABINET"; emulator_.reset(); return;
    }
    playing_=true; notice_.clear();
}
void Arcade::controls(double dt,float x,float y,bool fire,bool alternate,int vertical,int horizontal,bool confirm,bool back,
                      bool start,bool select) {
    if(selected_<0)return;
    if(back) {
        if(playing_) { finishGame(); row_=0; } else leave();
        previewAt_=0; return;
    }
    if(playing_) {
        if(configs_[selected_].invert)y=-y;
        if(emulator_) {
            emulator_->input(x,y,fire,alternate,start,select);
            if(!emulator_->error().empty()) { notice_=emulator_->error(); finishGame(); }
        } else {
            const auto before=state();
            api_->step(api_->context,dt,{x,y,int(fire),int(alternate)});
            const auto after=state();
            if(after.over && !before.over)saveScore(after);
            if(after.score>before.score)sound_.playOwned(audioOwner,SfxId::Pickup,.25f);
            if(after.lives<before.lives)sound_.playOwned(audioOwner,SfxId::DuckPop,.25f);
        }
    } else {
        row_=(row_+vertical+5)%5;
        if(horizontal) {
            auto c=configs_[selected_];
            if(row_==1)c.game=(c.game+horizontal+5)%5;
            if(row_==2)c.difficulty=(c.difficulty-1+horizontal+3)%3+1;
            if(row_==3)c.invert=1-c.invert;
            if(row_>=1 && row_<=3)saveConfig(selected_,c);
        }
        if(confirm) {
            if(row_==0)play();
            else if(row_==4)browseRequested_=true;
            else {
                auto c=configs_[selected_];
                if(row_==1)c.game=(c.game+1)%5;
                if(row_==2)c.difficulty=c.difficulty%3+1;
                if(row_==3)c.invert=1-c.invert;
                saveConfig(selected_,c);
            }
        }
    }
}
void Arcade::browseResult(const std::filesystem::path& path,const std::string& error) {
    if(!error.empty()) {notice_=error;std::fprintf(stderr,"Arcade browse: %s\n",error.c_str());}
    else if(!path.empty())importRom(path);
    previewAt_=0;
}
bool Arcade::importRom(const std::filesystem::path& source) {
    if(selected_<0 || playing_) { notice_="SELECT AN IDLE CABINET BEFORE IMPORT"; return false; }
    std::string bytes,error;
    if(!readArcadeRom(source,bytes,error)) { notice_=error; std::fprintf(stderr,"Arcade import: %s\n",error.c_str()); return false; }
    if(dataDir_.empty()) { notice_="ARCADE PROFILE DIRECTORY UNAVAILABLE"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(dataDir_,ec);
    if(ec) { notice_="CANNOT CREATE ARCADE DIRECTORY: "+ec.message(); return false; }
    const auto target=dataDir_/("cabinet-"+std::to_string(selected_)+".nes");
    auto temp=target;
    temp+=".tmp";
    {
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);
        out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); out.close();
        if(!out) { notice_="CANNOT WRITE IMPORTED ROM"; return false; }
    }
#ifdef _WIN32
    if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        notice_="CANNOT COMMIT IMPORTED ROM: "+std::to_string(GetLastError()); return false;
    }
#else
    std::filesystem::rename(temp,target,ec);
    if(ec) { notice_="CANNOT COMMIT IMPORTED ROM: "+ec.message(); return false; }
#endif
    auto c=configs_[selected_]; c.game=4;
    if(!saveConfig(selected_,c))return false;
    notice_="ROM IMPORTED. SELECT PLAY TO START"; previewAt_=0; return true;
}
void Arcade::paint(int cabinet,double time) {
    const auto c=configs_[cabinet]; Bitmap bm; bm.w=320; bm.h=240; bm.rgba.resize(320*240*4);
    const bool live=playing_ && selected_==cabinet;
    api_->paint(api_->context,c.game,live?-1:time+cabinet,bm.rgba.data());
    if(live && emulator_)emulator_->pixels(bm.rgba);
    auto text=[&](int y,const std::string& s,Color8 color=Color8{125,245,222,255}) {
        drawText(bm,8,float(y),s,12,FontFace::Mono,color);
    };
    if(selected_==cabinet && !playing_) {
        for(int y=30;y<218;++y)for(int x=0;x<320;++x) {
            auto* p=&bm.rgba[(y*320+x)*4]; p[0]/=4; p[1]/=4; p[2]/=4;
        }
        const std::string rows[]={"PLAY / RESTART",std::string("GAME: ")+titles[c.game],
            "DIFFICULTY: "+std::to_string(c.difficulty),"INVERT Y: "+std::string(c.invert?"ON":"OFF"),"BROWSE FOR NES ROM"};
        for(int i=0;i<5;++i)text(52+i*22,(row_==i?"> ":"  ")+rows[i],row_==i?Color8{255,203,123,255}:Color8{145,174,197,255});
        text(183,"BROWSE OR DROP A LAWFUL .NES FILE");
        text(200,"THEN SELECT PLAY. ESC/B: BACK");
    } else if(live) {
        const auto s=state();
        text(17,std::string(titles[c.game])+"  "+std::to_string(s.score));
        if(c.game==4 && s.ticks==0)text(115,"LOADING NES - PLEASE WAIT");
        if(c.game<4)text(231,"LIVES "+std::to_string(s.lives)+"   BEST "+std::to_string(highs_[c.game]));
        if(s.over) { text(106,"GAME OVER"); text(124,"FIRE TO RESTART"); }
        if(c.game==0)text(214,"W/S OR STICK: PADDLE");
        if(c.game==1)text(214,"TURN / THRUST / FIRE");
        if(c.game==2)text(214,"MOVE / FIRE / ALT: RESCUE AT GROUND");
        if(c.game==3)text(214,"TAP DIRECTION: CROSS TRAFFIC + RIVER");
    } else {
        text(18,titles[c.game]); text(231,"SELECT TO PLAY / CONFIGURE");
    }
    if(selected_==cabinet && !notice_.empty()) {
        for(int y=174;y<240;++y)for(int x=0;x<320;++x) {
            auto* p=&bm.rgba[(y*320+x)*4];p[0]=8;p[1]=12;p[2]=25;
        }
        auto lines=wrapText(notice_,11,FontFace::Mono,304,3);
        int y=189; for(const auto& line:lines){text(y,line,{255,174,109,255});y+=13;}
        text(234,"ENTER/A: OK  ESC/B: BACK");
    }
    renderer_.updateLabel(*screens_[cabinet],bm.rgba.data(),320,240);
}
void Arcade::update(double dt,double time,float volume) {
    if(!api_)return;
    if(emulator_) {emulator_->volume(volume);emulator_->update(dt);}
    previewAt_-=dt;
    if(previewAt_<=0) { for(int i=0;i<SF_ARCADE_CABINETS;++i)paint(i,time); previewAt_=.125; }
    else if(selected_>=0)paint(selected_,time);
}
}
