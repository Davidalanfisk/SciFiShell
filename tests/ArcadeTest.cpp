#include "ArcadeApi.h"
#include "ArcadeRoom.h"
#include "ArcadeEmulator.h"
#include "ArcadeInput.h"
#include "SystemDriveRoot.h"
#include "NativePluginApi.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

extern "C" const SfNativePluginV1* sf_plugin_query_v1();
namespace {
void require(bool ok,const char* message) {
    if(!ok){std::fprintf(stderr,"ArcadeTest: %s\n",message);std::exit(1);}
}
void room() {
    using namespace sf::arcade;
    require(floor({0,1.7f,-16})==0,"ground under mezzanine");
    require(floor({0,6.5f,-16})==4.8f,"upper floor");
    for(float x=-10.5f;x<=10.5f;x+=.5f)
        require(floor({x,6.5f,-8.5f})==4.8f,"loft reaches both side walls without a floor gap");
    Point right=resolve({7,6.5f,-10},{10.4f,6.5f,-10});
    require(right.x>10 && floor(right)==4.8f,"can reach the right wall on the loft");
    Point rightRail=resolve({9,6.5f,-8.5f},{9,6.5f,-7.8f});
    require(rightRail.z<=-8.4f,"new right loft edge is protected");
    require(floor({0,6.5f,0})==0,"double height open area");
    Point p{9,1.7f,4.2f};
    for(int i=0;i<122;++i) {
        Point q=p; q.z-=.1f; p=resolve(p,q);
    }
    require(std::fabs(p.y-4.1f)<.01f,"walk up first flight and landing");
    for(int i=0;i<71;++i) {
        Point q=p; q.x-=.1f; p=resolve(p,q);
    }
    require(p.y>6.49f && p.x<2.4f,"walk up corner-turn second flight");
    for(int i=0;i<12;++i) {Point q=p;q.z-=.1f;p=resolve(p,q);}
    require(p.z<-8.4f && p.y>6.49f,"stair connects to mezzanine");
    for(float x=2.4f;x<=3.7f;x+=.2f) {
        Point q=resolve({x,6.5f,-8.5f},{x,6.5f,-7.9f});
        require(q.z>-8.1f && floor(q)>=4.79f,"continuous wide stair opening");
    }
    Point edge=resolve({0,6.5f,-8.5f},{0,6.5f,-8.0f});
    require(edge.z<=-8.4f,"upper railing is solid");
    auto collision=resolve({-6,1.7f,-1},{-6,1.7f,-2.5f});
    require(collision.z>-2.1f,"cabinet collision");
    auto ceiling=resolve({0,4.1f,-15},{0,6,-15});
    require(ceiling.y<=4.35f,"cannot jump through upper slab");
    auto wall=resolve({0,1.7f,0},{100,1.7f,100});
    require(wall.x<=10.5f && wall.z<=5.5f,"room wall bounds");
    // Reverse the same accessible path, including the turn.
    p={2.5f,6.5f,-8.5f};
    auto descend=[&](Point q){q.y=floor(q)+1.7f;p=resolve(p,q);};
    for(int i=0;i<15;++i){auto q=p;q.z+=.1f;descend(q);}
    for(int i=0;i<70;++i){auto q=p;q.x+=.1f;descend(q);}
    for(int i=0;i<116;++i){auto q=p;q.z+=.1f;descend(q);}
    require(p.y<1.91f && p.z>3.9f,"walk down stairs to ground");
    const auto separated=[](float ax,float az,float ahx,float ahz,float bx,float bz,float bhx,float bhz,float clearance) {
        return std::fabs(ax-bx)>=ahx+bhx+clearance || std::fabs(az-bz)>=ahz+bhz+clearance;
    };
    for(int i=0;i<12;++i) {
        auto c=cabinet(i);
        require(separated(c.x,c.z,.86f,.75f,9,-1,1,5,.35f),"cabinet and first flight clearance");
        require(separated(c.x,c.z,.86f,.75f,9,-7,1,1,.35f),"cabinet and corner landing clearance");
        require(separated(c.x,c.z,.86f,.75f,5,-7,3,1,.35f),"cabinet and upper flight clearance");
        Point approach=resolve({c.x,1.7f,c.z+2.0f},{c.x,1.7f,c.z+1.35f});
        require(std::fabs(approach.x-c.x)<.01f && std::fabs(approach.z-(c.z+1.35f))<.01f,
                "cabinet interaction position is unobstructed");
    }
}
void games() {
    const auto* plugin=sf_plugin_query_v1();
    require(plugin->abi==SF_NATIVE_PLUGIN_ABI,"native plugin ABI");
    auto* context=plugin->create();require(context && plugin->start(context),"native start");
    const auto* api=static_cast<const SfArcadeV1*>(plugin->get_interface(context,SF_ARCADE_INTERFACE));
    require(api && api->abi==SF_ARCADE_ABI && api->size==sizeof(*api),"arcade interface");
    require(api->state(context).cabinet==-1,"no running game at start");
    std::vector<uint8_t> a(320*240*4),b(a.size());
    for(int game=0;game<4;++game) {
        api->paint(context,game,0,a.data());api->paint(context,game,1,b.data());
        require(a!=b,"animated attract frame");
        require(api->state(context).cabinet==-1,"attract cannot start game");
        require(api->begin(context,game,game,2),"start original game");
        for(int i=0;i<360;++i)api->step(context,1.0/60,{.4f,-.5f,1,1});
        auto s=api->state(context);
        require(s.cabinet==game && s.game==game && s.ticks>0,"game simulation runs");
        require(s.score>=0 && s.score<=999999 && s.lives>=0 && s.lives<=3,"bounded score/lives");
        api->paint(context,game,-1,b.data());
        require(a!=b,"game produces playable frame");
        api->end(context);require(api->state(context).cabinet==-1,"end releases active owner");
    }
    require(api->begin(context,0,0,1),"start paddle scoring test");
    for(int frame=0;frame<1200;++frame) {
        api->paint(context,0,-1,a.data());
        int ballY=120;
        for(int y=0;y<240;++y)for(int x=0;x<320;++x) {
            const auto offset=(y*320+x)*4;
            if(a[offset]==255 && a[offset+1]==255 && a[offset+2]==255)ballY=y;
        }
        api->step(context,1.0/60,{0,std::clamp((ballY-120)/90.0f,-1.0f,1.0f),0,0});
    }
    require(api->state(context).score>=3 && api->state(context).lives==3,"paddle collisions award playable scores");
    api->end(context);
    require(api->begin(context,0,1,1),"start sub-frame input baseline");
    api->step(context,.018,{0,0,0,0});api->paint(context,1,-1,a.data());
    require(api->begin(context,0,1,1),"reset sub-frame input test");
    api->step(context,.004,{0,0,1,0});api->step(context,.014,{0,0,0,0});api->paint(context,1,-1,b.data());
    require(a!=b,"short VR/fire pulse survives a sub-frame update");
    require(api->begin(context,0,0,1) && api->begin(context,11,3,3),"replace active game");
    require(api->state(context).cabinet==11 && api->state(context).game==3,"exactly one active game");
    require(!api->begin(context,12,0,1) && !api->begin(context,0,5,1) && !api->begin(context,0,0,0),"reject invalid cabinet/config");
    auto ticks=api->state(context).ticks;
    api->step(context,std::numeric_limits<double>::quiet_NaN(),{0,0,0,0});
    api->step(context,-1,{0,0,0,0});
    api->step(context,1,{std::numeric_limits<float>::infinity(),0,0,0});
    require(api->state(context).ticks==ticks,"reject nonfinite inputs/delta");
    plugin->stop(context);require(api->state(context).cabinet==-1,"native stop frees game");
    plugin->destroy(context);
}
void imports() {
    const auto dir=std::filesystem::temp_directory_path()/("scifi-arcade-test-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    auto path=dir/"original.nes";
    std::string rom(16+16384+8192,'\0');rom.replace(0,4,"NES\x1a",4);rom[4]=1;rom[5]=1;
    auto write=[&]{std::ofstream f(path,std::ios::binary);f.write(rom.data(),rom.size());};
    write();std::string bytes,error;
    require(sf::readArcadeRom(path,bytes,error) && bytes==rom,"valid original mapper zero import");
    rom[7]=8;rom[12]=2;rom[15]=1;write();
    require(sf::readArcadeRom(path,bytes,error) && bytes==rom,"compatible multiregion NES 2.0 preserved unchanged");
    rom[12]=1;write();require(!sf::readArcadeRom(path,bytes,error),"PAL-only NES 2.0 rejected");
    rom[12]=2;rom[8]=16;write();require(!sf::readArcadeRom(path,bytes,error),"NES 2.0 submapper rejected");
    rom[8]=0;rom[10]=7;write();require(!sf::readArcadeRom(path,bytes,error),"NES 2.0 extra PRG RAM rejected");
    rom[10]=0;rom[15]=2;write();require(!sf::readArcadeRom(path,bytes,error),"NES 2.0 expansion device rejected");
    rom[7]=rom[12]=rom[15]=0;
    rom[6]=0x10;write();require(!sf::readArcadeRom(path,bytes,error),"unsupported mapper rejected");
    rom[6]=2;write();require(!sf::readArcadeRom(path,bytes,error),"battery ROM rejected explicitly");
    rom[6]=0;rom.pop_back();write();require(!sf::readArcadeRom(path,bytes,error),"truncated import rejected");
    rom.resize(1024*1024+1);write();require(!sf::readArcadeRom(path,bytes,error),"oversized import rejected");
    require(!sf::readArcadeRom(dir,bytes,error),"directory rejected");
    require(!sf::readArcadeRom(dir/"missing.nes",bytes,error),"missing import rejected");
    std::filesystem::remove(path);std::filesystem::remove(dir);
}
}
int main() {
#ifdef _WIN32
    std::wstring root;std::string rootError;
    require(sf::systemDriveRoot(root,rootError) && !root.empty(),"Windows system drive root is available");
    wchar_t windows[MAX_PATH+1]{},expected[MAX_PATH+1]{};
    require(GetWindowsDirectoryW(windows,MAX_PATH+1)>0 && GetVolumePathNameW(windows,expected,MAX_PATH+1),
            "Windows system volume query");
    require(root==expected && std::filesystem::path(root)==std::filesystem::path(root).root_path(),
            "NES picker starts at actual OS volume root");
#endif
    sf::ArcadeButtons buttons;
    buttons.set(8);buttons.set(0);
    require(buttons.sample()==8 && buttons.sample()==0,"NES Start pulse survives until emulated frame");
    buttons.set(1);
    require(buttons.sample()==1 && buttons.sample()==1,"NES held button persists across frames");
    buttons.set(0);require(buttons.sample()==0,"NES release clears input");
    sf::ArcadeMenuAxis axis;
    require(axis.step(1)==1 && axis.step(1)==0,"held gamepad/VR stick cannot repeat menu edits");
    require(axis.step(.59f)==0 && axis.step(.7f)==0,"menu threshold has hysteresis");
    require(axis.step(0)==0 && axis.step(-1)==-1 && axis.step(-1)==0,"neutral rearms stick direction");
    require(axis.step(std::numeric_limits<float>::quiet_NaN())==0 && axis.step(1)==1,"invalid/disconnected axis resets");
    room();games();imports();std::puts("Arcade input, room, games, single-owner lifecycle and imports passed");
}
