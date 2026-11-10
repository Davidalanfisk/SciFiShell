#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <utility>

namespace sf::arcade {
struct Point { float x,y,z; };
struct Box { Point at,size,color; bool solid; };
inline constexpr float loftFront=-8.0f, stairWest=2.0f, stairEast=10.0f;
inline constexpr float stairOpeningLeft=2.0f, stairOpeningRight=4.15f;
inline Point cabinet(int i) {
    constexpr float rows[]={-3.0f,-10.0f,-15.0f};
    return {-6.0f+4*(i%4),0,rows[i/4]};
}
inline std::vector<Box> roomBoxes() {
    std::vector<Box> b;
    const Point dark{.035f,.043f,.07f}, wood{.18f,.095f,.055f}, cyan{.12f,.8f,.75f}, pink{.65f,.13f,.46f};
    auto add=[&](Point p,Point s,Point c,bool solid=false){b.push_back({p,s,c,solid});};
    add({0,-.12f,-8},{22,.24f,28},{.025f,.02f,.045f});
    add({0,4.5f,-22},{22,9,.25f},dark);
    add({-11,4.5f,-8},{.25f,9,28},dark);
    add({11,4.5f,-8},{.25f,9,28},dark);
    add({0,4.5f,6},{22,9,.25f},dark);
    add({0,9,-8},{22,.2f,28},dark);
    add({0,4.68f,(loftFront-22)/2},{22,.24f,loftFront+22},wood);
    for(float z:{-20.0f,-14.0f,-8.0f,-2.0f,4.0f}) {
        add({0,8.7f,z},{22,.22f,.3f},dark);
        add({0,8.55f,z},{19,.045f,.06f},z<-8?cyan:pink);
    }
    for(float x:{-10.7f,10.7f}) {
        add({x,3.8f,-8},{.06f,.06f,27},cyan);
        add({x,6.6f,-15},{.06f,.06f,13},pink);
    }
    // Mezzanine edge rail leaves a clear opening onto the upper stair.
    for (auto pair:{std::pair{(-11.0f+stairOpeningLeft)/2,stairOpeningLeft+11},
                    std::pair{(stairOpeningRight+11)/2,11-stairOpeningRight}}) {
        add({pair.first,5.85f,-8.1f},{pair.second,.09f,.12f},cyan);
        for(float x=pair.first-pair.second/2;x<=pair.first+pair.second/2;x+=1.2f)
            add({x,5.3f,-8.1f},{.06f,1.05f,.06f},dark);
    }
    for(int i=0;i<12;++i) {
        auto p=cabinet(i); const auto accent=i%2?pink:cyan;
        add({p.x,.58f,p.z},{1.6f,1.16f,1.25f},dark,true);
        add({p.x,1.65f,p.z-.35f},{1.6f,1.05f,.65f},dark,true);
        add({p.x,2.38f,p.z-.2f},{1.72f,.42f,1.0f},accent);
        add({p.x,1.14f,p.z+.22f},{1.72f,.16f,1.05f},wood);
        for(float dx:{-.78f,.78f})add({p.x+dx,1.7f,p.z+.01f},{.08f,1.2f,.15f},accent);
        add({p.x-.64f,1.31f,p.z+.43f},{.08f,.2f,.08f},cyan);
        for(float dx:{.18f,.38f,.58f})add({p.x+dx,1.25f,p.z+.48f},{.1f,.07f,.1f},pink);
        add({p.x,.45f,p.z+.64f},{.16f,.05f,.03f},{.8f,.65f,.3f});
    }
    // Two straight flights, turning west on the corner landing.
    for(int i=0;i<12;++i) {
        float h=(i+1)*.2f;
        add({9,h/2,4-(i+.5f)*10/12},{2,h,10.0f/12},wood);
        add({9,h+.015f,4-(i+1)*10.0f/12},{2,.03f,.05f},cyan);
        float h2=2.4f+(i+1)*.2f;
        add({8-(i+.5f)/3,h2/2,-7},{1.0f/3,h2,2},wood);
        add({8-(i+1)/3.0f,h2+.015f,-7},{.05f,.03f,2},cyan);
    }
    add({9,1.2f,-7},{2,2.4f,2},wood);
    add({3,2.4f,-7},{2,4.8f,2},wood);
    add({2,5.4f,-7},{.1f,1.2f,.1f},dark);
    add({2,6.0f,-7},{.1f,.08f,2},cyan);
    for(int i=0;i<=12;++i) {
        float h=i*.2f;
        add({10,h+.6f,4-i*10.0f/12},{.06f,1.2f,.06f},dark);
        add({10,h+1.2f,4-i*10.0f/12},{.1f,.08f,.85f},cyan);
        add({8-i*.5f,3.0f+h,-6},{.06f,1.2f,.06f},dark);
        add({8-i*.5f,3.6f+h,-6},{.52f,.08f,.1f},cyan);
    }
    // Upstairs apartment: office desk, chair, bookcase, bed, sofa and divider.
    add({-6,5.55f,-20},{5,1.5f,1.2f},wood,true);
    add({-6,6.7f,-20},{1.5f,.9f,.12f},cyan);
    add({-6,5.2f,-18.5f},{1, .8f,1},dark,true);
    add({-10.3f,6.1f,-18},{.7f,2.6f,4},wood,true);
    for(int i=0;i<8;++i)add({-9.85f,5.2f+(i%3)*.65f,-19.5f+(i/3)*.9f},{.18f,.48f,.25f},i%2?cyan:pink);
    add({3.7f,5.25f,-19},{3.6f,.9f,4.8f},wood,true);
    add({3.7f,5.76f,-18.7f},{3.45f,.18f,4.1f},{.13f,.16f,.26f});
    add({3.7f,5.95f,-20.3f},{2.5f,.25f,.7f},{.65f,.57f,.47f});
    add({-.5f,5.8f,-19},{.18f,2,5.7f},wood,true);
    add({-6.5f,5.25f,-11},{3,.9f,1.4f},{.19f,.11f,.24f},true);
    add({-6.5f,5.6f,-11.6f},{3,1.3f,.25f},{.19f,.11f,.24f});
    add({-6.5f,5.1f,-13.3f},{2,.6f,1.1f},wood,true);
    return b;
}
inline float stair(float x,float z) {
    if(x>=8 && x<=10 && z>=-6 && z<=4) return std::ceil((4-z)*1.2f)*.2f;
    if(x>=8 && x<=10 && z>=-8 && z<-6) return 2.4f;
    if(x>=2 && x<=4 && z>=-8 && z<=-6) return 4.8f;
    if(x>4 && x<8 && z>=-8 && z<=-6) return 2.4f+std::ceil((8-x)*3)*.2f;
    return -1;
}
inline float floor(Point p) {
    float h=stair(p.x,p.z);
    if(h>=0 && p.y>=h+1.3f)return h;
    if(p.z<=loftFront && p.y>=6.1f)return 4.8f;
    return 0;
}
inline Point resolve(Point prev,Point p) {
    p.x=std::clamp(p.x,-10.5f,10.5f); p.z=std::clamp(p.z,-21.5f,5.5f);
    const bool upstairs=prev.y>6.1f;
    if(upstairs && prev.z<=loftFront-.4f && p.z>loftFront-.4f &&
       !(p.x>stairOpeningLeft+.35f && p.x<stairOpeningRight-.35f))p.z=loftFront-.4f;
    const float h=stair(p.x,p.z);
    if(h>=0 && p.y<h+1.3f) { p.x=prev.x; p.z=prev.z; }
    // Rail on the inner edge of the first flight and south side of the second.
    if(prev.x>=8 && prev.x<=10 && prev.z>=-6 && prev.z<=4 && prev.y>2.2f && p.x<8.3f) p.x=8.3f;
    if(prev.z>=-8 && prev.z<=-6 && prev.x>=2 && prev.x<8 && p.z>-6.3f)p.z=-6.3f;
    if(prev.z>-8 && prev.z<=-6 && prev.x>=2 && prev.x<4 && p.x<2.35f)p.x=2.35f;
    if(!upstairs && p.z<loftFront)p.y=std::min(p.y,4.35f);
    static const auto boxes=roomBoxes();
    for(const auto& b:boxes) if(b.solid && p.y>b.at.y-b.size.y/2 && p.y-1.6f<b.at.y+b.size.y/2) {
        const float hx=b.size.x/2+.35f,hz=b.size.z/2+.35f,dx=p.x-b.at.x,dz=p.z-b.at.z;
        if(std::fabs(dx)<hx && std::fabs(dz)<hz) {
            if(hx-std::fabs(dx)<hz-std::fabs(dz))p.x=b.at.x+(dx<0?-hx:hx);
            else p.z=b.at.z+(dz<0?-hz:hz);
        }
    }
    p.y=std::max(p.y,floor(p)+1.7f);
    return p;
}
}
