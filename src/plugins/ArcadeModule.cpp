#include "ArcadeApi.h"
#include "NativePluginApi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>

namespace {
constexpr float pi = 3.14159265f;
struct Body { float x=0, y=0, vx=0, vy=0; bool live=false; };
struct Game {
    SfArcadeV1 api{};
    SfArcadeState state{-1,0,0,3,0,0};
    int difficulty=1;
    double accumulator=0, clock=0, shot=0;
    float x=160, y=200, vx=0, vy=0, angle=-pi/2, enemy=120;
    bool fire=false, hop=false;
    bool pulseFire=false,pulseAlternate=false;
    std::array<Body,32> bodies{};
    std::array<Body,12> bullets{};
    uint32_t random=0x51983;
    float rand(float n) { random=random*1664525u+1013904223u; return (random>>8)*(n/16777216.0f); }
    void reset() {
        accumulator=clock=shot=0; x=160; y=200; vx=0; vy=0; angle=-pi/2; enemy=120;
        fire=hop=pulseFire=pulseAlternate=false; bodies={}; bullets={}; random=0x51983;
        if (state.game==0) { x=160; y=120; vx=105; vy=65; }
        if (state.game==1) for (int i=0;i<8;++i) bodies[i]={rand(320),rand(110),rand(50)-25,rand(40)+10,true};
        if (state.game==2) {
            x=70; y=120;
            for (int i=0;i<12;++i) bodies[i]={rand(320),rand(150)+15,20+rand(25),0,true};
        }
        if(state.game==3)y=220;
    }
    void damage() {
        if (--state.lives<=0) state.over=1;
        x=state.game==2 ? 70.0f : 160.0f; y=state.game==1 ? 180.0f : 220.0f;
        vx=vy=0;
    }
    static float wrap(float x,float n) { return x-std::floor(x/n)*n; }
    void tick(SfArcadeInput in) {
        if (state.over) {
            if (in.fire && !fire) { state.score=0; state.lives=3; state.over=0; reset(); }
            fire=in.fire!=0; return;
        }
        clock+=1.0/60; ++state.ticks;
        const float dt=1.0f/60, speed=1+0.15f*(difficulty-1);
        if (state.game==0) {
            const float paddle=std::clamp(in.y*90+120,24.0f,216.0f);
            enemy+=std::clamp(y-enemy,-(50+15.0f*difficulty)*dt,(50+15.0f*difficulty)*dt);
            x+=vx*dt*speed; y+=vy*dt*speed;
            if (y<5 || y>235) { y=std::clamp(y,5.0f,235.0f); vy=-vy; }
            if (vx<0 && x<18 && x>8 && std::fabs(y-paddle)<27) {
                vx=std::min(230.0f,-vx+4); vy+=(y-paddle)*2; ++state.score;
            }
            if (vx>0 && x>302 && std::fabs(y-enemy)<27) vx=-vx;
            if (x<0 || x>320) {
                if (x<0 && --state.lives==0) state.over=1;
                if (x>320) state.score+=10;
                x=160; y=120; vx=-105; vy=65;
            }
            // Paddle position is stored separately from ball motion.
            angle=paddle;
        } else if (state.game==1) {
            angle+=in.x*3.5f*dt;
            if (in.y<-.15f) { vx+=std::cos(angle)*100*dt; vy+=std::sin(angle)*100*dt; }
            vx*=.995f; vy*=.995f;
            x=wrap(x+vx*dt,320); y=wrap(y+vy*dt,240);
            shot-=dt;
            if (in.fire && shot<=0) for (auto& b:bullets) if (!b.live) {
                b={x,y,std::cos(angle)*220,std::sin(angle)*220,true}; shot=.18; break;
            }
            int alive=0;
            for (auto& a:bodies) if (a.live) {
                ++alive; a.x=wrap(a.x+a.vx*dt*speed,320); a.y=wrap(a.y+a.vy*dt*speed,240);
                if (clock>2 && std::hypot(a.x-x,a.y-y)<14) { damage(); clock=0; }
            }
            if (!alive) for (int i=0;i<8;++i) bodies[i]={rand(320),0,rand(70)-35,rand(40)+15,true};
        } else if (state.game==2) {
            x=std::clamp(x+in.x*125*dt,8.0f,312.0f); y=std::clamp(y+in.y*125*dt,12.0f,210.0f);
            shot-=dt;
            if (in.fire && shot<=0) for (auto& b:bullets) if (!b.live) {
                b={x+10,y,240,0,true}; shot=.15; break;
            }
            for (auto& a:bodies) if (a.live) {
                a.x=wrap(a.x-a.vx*dt*speed,320);
                a.y=std::clamp(a.y+std::sin(float(clock)*2+a.vx)*20*dt,10.0f,200.0f);
                if (clock>1.5 && std::hypot(a.x-x,a.y-y)<13) { damage(); clock=0; }
            }
            if (y>193 && in.alternate) { state.score+=25; y=155; }
            if (std::none_of(bodies.begin(),bodies.end(),[](Body b){return b.live;}))
                for (int i=0;i<12;++i) bodies[i]={320+rand(60),rand(160)+10,20+rand(35),0,true};
        } else {
            const bool pressed=std::fabs(in.x)>.4f || std::fabs(in.y)>.4f;
            if (pressed && !hop) {
                if (std::fabs(in.y)>=std::fabs(in.x)) y+=in.y>0 ? 20 : -20;
                else x+=in.x>0 ? 20 : -20;
            }
            hop=pressed; x=std::clamp(x,10.0f,310.0f);
            const int lane=int(std::round((220-y)/20));
            if (lane>=1 && lane<=9 && lane!=5) {
                const float offset=float(clock)*(25+lane*4)*speed*(lane%2 ? 1 : -1);
                const float phase=wrap(x-offset,110);
                if (lane<5 ? phase<35 : phase>72) { damage(); }
                else if (lane>5) x=std::clamp(x+(lane%2?1:-1)*(25+lane*4)*dt*speed,10.0f,310.0f);
            }
            if (y<=20) { state.score+=100; x=160; y=220; }
            if (y>220) y=220;
        }
        if (state.game==1 || state.game==2) for (auto& b:bullets) if (b.live) {
            b.x+=b.vx*dt; b.y+=b.vy*dt;
            if (b.x<0||b.x>320||b.y<0||b.y>240) b.live=false;
            for (auto& a:bodies) if (b.live && a.live && std::hypot(a.x-b.x,a.y-b.y)<13) {
                a.live=b.live=false; state.score+=10;
            }
        }
        state.score=std::min(state.score,999999);
        fire=in.fire!=0;
    }
};
void* create() { return new(std::nothrow) Game; }
int start(void* p) { return p!=nullptr; }
void end(void* p) {
    auto& g=*static_cast<Game*>(p);g.state.cabinet=-1;g.bodies={};g.bullets={};g.accumulator=0;
    g.pulseFire=g.pulseAlternate=false;
}
void destroy(void* p) { delete static_cast<Game*>(p); }
int begin(void* p,int cabinet,int game,int difficulty) {
    if (!p || cabinet<0 || cabinet>=SF_ARCADE_CABINETS || game<0 || game>4 || difficulty<1 || difficulty>3) return 0;
    auto& g=*static_cast<Game*>(p);
    end(p); g.state={cabinet,game,0,3,0,0}; g.difficulty=difficulty; g.reset(); return 1;
}
SfArcadeState state(void* p) { return static_cast<Game*>(p)->state; }
void step(void* p,double dt,SfArcadeInput in) {
    auto& g=*static_cast<Game*>(p);
    if (g.state.cabinet<0 || g.state.game==4 || !std::isfinite(dt) || dt<=0 ||
        !std::isfinite(in.x) || !std::isfinite(in.y)) return;
    in.x=std::clamp(in.x,-1.0f,1.0f); in.y=std::clamp(in.y,-1.0f,1.0f);
    g.pulseFire=g.pulseFire||in.fire;g.pulseAlternate=g.pulseAlternate||in.alternate;
    g.accumulator+=std::min(dt,.1);
    while (g.accumulator>=1.0/60) {
        auto frame=in;frame.fire=frame.fire||g.pulseFire;frame.alternate=frame.alternate||g.pulseAlternate;
        g.tick(frame);g.pulseFire=g.pulseAlternate=false;g.accumulator-=1.0/60;
    }
}
struct Canvas {
    uint8_t* p;
    void rect(int x,int y,int w,int h,uint32_t col) {
        for(int yy=std::max(0,y);yy<std::min(240,y+h);++yy) for(int xx=std::max(0,x);xx<std::min(320,x+w);++xx) {
            auto* d=p+(yy*320+xx)*4;
            d[0]=uint8_t(col>>16); d[1]=uint8_t(col>>8); d[2]=uint8_t(col); d[3]=255;
        }
    }
    void line(float ax,float ay,float bx,float by,uint32_t c) {
        int n=int(std::max(std::fabs(bx-ax),std::fabs(by-ay)))+1;
        for(int i=0;i<=n;++i) rect(int(ax+(bx-ax)*i/n),int(ay+(by-ay)*i/n),2,2,c);
    }
};
void paint(void* p,int game,double t,uint8_t* rgba) {
    if (!rgba || !std::isfinite(t)) return;
    const auto& g=*static_cast<Game*>(p); Canvas c{rgba};
    c.rect(0,0,320,240,0x080c19);
    const bool live=t<0 && g.state.cabinet>=0 && g.state.game==game;
    const float clock=live?float(g.clock):float(std::fmod(t,1000));
    const float x=live?g.x:160+90*std::sin(clock), y=live?g.y:120+65*std::cos(clock*1.3f);
    if (game==0) {
        for(int yy=8;yy<240;yy+=14)c.rect(159,yy,2,7,0x30475a);
        c.rect(10,int(live?g.angle:120+65*std::cos(clock))-24,5,48,0x64ffda);
        c.rect(305,int(live?g.enemy:120+50*std::sin(clock))-24,5,48,0xffbb60);
        c.rect(int(x)-3,int(y)-3,6,6,0xffffff);
    } else if(game==1||game==2) {
        for(int i=0;i<45;++i) c.rect((i*71)%320,int(Game::wrap(i*37+clock*7,240)),1,1,0x42618c);
        if (game==2) { c.line(0,227,320,227,0x8c4de0); for(int i=0;i<7;++i)c.rect(i*47,218,3,7,0x64ffda); }
        auto rock=[&](float xx,float yy) {
            for(int j=0;j<7;++j) {
                const float a=j*2*pi/7,b=(j+1)*2*pi/7;
                c.line(xx+std::cos(a)*11,yy+std::sin(a)*11,xx+std::cos(b)*11,yy+std::sin(b)*11,0xffbb60);
            }
        };
        if(live) {
            for(auto a:g.bodies) if(a.live)rock(a.x,a.y);
            for(auto b:g.bullets) if(b.live)c.rect(int(b.x),int(b.y),3,3,0xffffff);
        } else for(int i=0;i<6;++i)rock(Game::wrap(i*59+clock*12,320),40+i*27.0f);
        const float angle=game==2?0:live?g.angle:clock;
        float sx=live?x:160, sy=live?y:120;
        c.line(sx+std::cos(angle)*11,sy+std::sin(angle)*11,sx+std::cos(angle+2.5f)*9,sy+std::sin(angle+2.5f)*9,0x64ffda);
        c.line(sx+std::cos(angle)*11,sy+std::sin(angle)*11,sx+std::cos(angle-2.5f)*9,sy+std::sin(angle-2.5f)*9,0x64ffda);
        c.line(sx+std::cos(angle+2.5f)*9,sy+std::sin(angle+2.5f)*9,sx+std::cos(angle-2.5f)*9,sy+std::sin(angle-2.5f)*9,0x64ffda);
    } else if(game==3) {
        c.rect(0,20,320,90,0x102e52); c.rect(0,111,320,18,0x26554a);
        c.rect(0,211,320,29,0x26554a);
        const float speed=live?1+.15f*(g.difficulty-1):1;
        for(int lane=1;lane<=9;++lane)if(lane!=5) {
            float offset=clock*(25+lane*4)*speed*(lane%2?1:-1);
            for(int i=-2;i<5;++i)c.rect(int(Game::wrap(offset,110)+i*110),220-lane*20-6,lane>5?72:35,12,lane>5?0x9f7148:0xff9a5c);
        }
        c.rect(int(live?x:160)-6,int(live?y:220)-6,12,12,0x64ffda);
    } else for(int i=0;i<6;++i)c.rect(40+i*42,80+int(std::sin(clock+i)*20),24,50,0x7454b5);
}
const void* getInterface(void* p,const char* id) {
    if (!p || !id || std::strcmp(id,SF_ARCADE_INTERFACE)) return nullptr;
    auto& api=static_cast<Game*>(p)->api;
    api={sizeof(api),SF_ARCADE_ABI,p,begin,end,step,state,paint}; return &api;
}
}
#ifdef _WIN32
#define SF_EXPORT __declspec(dllexport)
#else
#define SF_EXPORT __attribute__((visibility("default")))
#endif
extern "C" SF_EXPORT const SfNativePluginV1* sf_plugin_query_v1() {
    static const SfNativePluginV1 plugin{sizeof(plugin),SF_NATIVE_PLUGIN_ABI,
        "org.scifishell.arcade",create,start,end,getInterface,destroy};
    return &plugin;
}
