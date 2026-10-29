#include "WebLayerAnalyzer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sf {

namespace {
constexpr const char* kTag = "__SCIFI_LAYERS__";

// Read-only DOM walk. Reports "tag|ms|scanned|complex|k,band,x,y,w,h,radius,z;..." through console.log.
// Bands: 0 background (huge in-flow blocks), 1 content, 2 UI (fixed/sticky bars, nav, controls), 3 overlay (modals).
const char* kScript = R"JS((function(VW,VH,MAXS,MAXC,MAXM,MAXMS){
try{
var t0=performance.now(),complex=false;
var all=document.body?document.body.getElementsByTagName('*'):[];
var total=all.length,n=Math.min(total,MAXS);
var cards=[],media=[],vp=VW*VH,scanned=0;
for(var i=0;i<n;i++){
 if((i&63)===0&&performance.now()-t0>MAXMS)break;
 var el=all[i],tag=el.tagName;scanned++;
 if(tag==='SCRIPT'||tag==='STYLE'||tag==='LINK'||tag==='META'||tag==='NOSCRIPT'||tag==='BR'||tag==='path')continue;
 var r=el.getBoundingClientRect();
 if(r.width<24||r.height<24||r.right<=0||r.bottom<=0||r.left>=VW||r.top>=VH)continue;
 var cs=getComputedStyle(el);
 if(cs.display==='none'||cs.visibility==='hidden')continue;
 var op=parseFloat(cs.opacity);if(op<0.05)continue;
 var pos=cs.position,zi=parseInt(cs.zIndex,10),hasZ=!isNaN(zi);
 var x=Math.max(r.left,0),y=Math.max(r.top,0),w=Math.min(r.right,VW)-x,h=Math.min(r.bottom,VH)-y;
 var area=w*h,fixedLike=(pos==='fixed'||pos==='sticky'),role=el.getAttribute('role');
 var band=1;
 if(area>0.6*vp&&!fixedLike)band=0;
 else if(fixedLike&&((hasZ&&zi>=1000)||role==='dialog'||el.getAttribute('aria-modal')==='true'))band=3;
 else if(fixedLike||tag==='NAV'||tag==='HEADER'||tag==='BUTTON'||tag==='INPUT'||tag==='SELECT'||role==='navigation'||role==='button')band=2;
 else if(hasZ&&zi>=1000&&pos!=='static')band=3;
 var rad=parseFloat(cs.borderTopLeftRadius)||0;
 var rec=[0,band,Math.round(x),Math.round(y),Math.round(w),Math.round(h),Math.round(rad),hasZ?Math.max(-999,Math.min(999,zi)):0,area];
 var bg=cs.backgroundImage;
 if(tag==='VIDEO'||(tag==='IFRAME'&&w>=300&&h>=170)){rec[0]=2;media.push(rec);}
 else if(tag==='IMG'&&w>=48&&h>=48){rec[0]=1;media.push(rec);}
 else if(bg&&bg.indexOf('url(')>=0&&w>=150&&h>=100&&area<0.8*vp){rec[0]=1;media.push(rec);}
 else if(band!==0&&area>6000&&area<0.3*vp&&w>=80&&h>=60&&!(y<8&&band===2)&&((pos!=='static'&&hasZ)||op<1||cs.transform!=='none'||cs.filter!=='none'||cs.boxShadow!=='none'||tag==='TABLE'||tag==='ASIDE'||tag==='FIGURE'||tag==='FORM')){cards.push(rec);}
}
var byArea=function(a,b){return b[8]-a[8];};
cards.sort(byArea);media.sort(byArea);
var out=cards.slice(0,MAXC).concat(media.slice(0,MAXM)),s=[];
for(var j=0;j<out.length;j++){var o=out[j];s.push(o[0]+','+o[1]+','+o[2]+','+o[3]+','+o[4]+','+o[5]+','+o[6]+','+o[7]);}
var ms=performance.now()-t0;
console.log('__SCIFI_LAYERS__|'+ms.toFixed(1)+'|'+scanned+'|'+(complex?1:0)+'|'+s.join(';'));
}catch(e){console.log('__SCIFI_LAYERS__|0|0|1|');}
}))JS";
// Installs (once per page) listeners that report whether an editable control has focus, so the shell knows when the
// keyboard belongs to the page and when WASD may move the camera.
const char* kFocusScript = R"JS((function(){
if(window.__sfFocus)return;window.__sfFocus=1;
var rep=function(){var a=document.activeElement,t=a&&a.tagName;
var ed=a&&(t==='INPUT'||t==='TEXTAREA'||t==='SELECT'||a.isContentEditable);
console.log('__SCIFI_FOCUS__|'+(ed?1:0));};
document.addEventListener('focusin',rep,true);
document.addEventListener('focusout',function(){setTimeout(rep,0);},true);
rep();
})();)JS";
}  // namespace

void WebLayerAnalyzer::request(CefRefPtr<CefFrame> frame, int viewW, int viewH) {
    if (!frame || !frame->IsValid()) return;
    frame->ExecuteJavaScript(kFocusScript, "scifi://focus", 0);
    char args[160];
    std::snprintf(args, sizeof(args), "(%d,%d,%d,%d,%d,%g)", viewW, viewH, kMaxScan, kMaxCards, kMaxMedia, kMaxMs);
    // The script is a function expression; append the argument list to call it.
    std::string code = kScript;
    code += args;
    frame->ExecuteJavaScript(code, "scifi://layer-analyzer", 0);
}

bool WebLayerAnalyzer::onConsole(const std::string& message) {
    static const char* kFocusTag = "__SCIFI_FOCUS__";
    if (message.compare(0, std::strlen(kFocusTag), kFocusTag) == 0) {
        editable_ = message.size() > std::strlen(kFocusTag) + 1 && message[std::strlen(kFocusTag) + 1] == '1';
        return true;
    }
    if (message.compare(0, std::strlen(kTag), kTag) != 0) return false;
    WebLayout next;
    std::string rest = message.substr(std::strlen(kTag));
    // Header fields separated by '|': ms, scanned, complex, items.
    size_t pos = 0;
    auto field = [&]() {
        if (pos >= rest.size() || rest[pos] != '|') return std::string();
        size_t end = rest.find('|', pos + 1);
        std::string f = rest.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        pos = end == std::string::npos ? rest.size() : end;
        return f;
    };
    next.analysisMs = std::atof(field().c_str());
    next.scanned = std::atoi(field().c_str());
    next.tooComplex = std::atoi(field().c_str()) != 0;
    std::string items = field();
    size_t start = 0;
    while (start < items.size()) {
        size_t end = items.find(';', start);
        std::string rec = items.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? items.size() : end + 1;
        int k, b, x, y, w, h, r, z;
        if (std::sscanf(rec.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d", &k, &b, &x, &y, &w, &h, &r, &z) != 8) continue;
        if (k < 0 || k > 2 || b < 0 || b >= kWebBandCount || w <= 0 || h <= 0) continue;
        WebElement e;
        e.kind = static_cast<WebElementKind>(k);
        e.band = b;
        e.x = static_cast<float>(x);
        e.y = static_cast<float>(y);
        e.w = static_cast<float>(w);
        e.h = static_cast<float>(h);
        e.radius = static_cast<float>(r);
        e.z = z;
        next.elements.push_back(e);
    }
    if (next.analysisMs > kMaxMs * 1.5) next.tooComplex = true;
    next.version = layout_.version + 1;
    layout_ = std::move(next);
    return true;
}

void WebLayerAnalyzer::clear() {
    editable_ = false;
    uint64_t v = layout_.version + 1;
    layout_ = WebLayout{};
    layout_.version = v;
}

}  // namespace sf
