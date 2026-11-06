cbuffer Constants
{
    float4x4 g_MVP;
    float4x4 g_World;
    float4   g_Color;
    float4   g_Params;
    float4   g_Light;
    float4   g_Accent;
    float4   g_Alt;
    float4   g_Lights[24];  // interior light points: xyz position, w intensity
    float4   g_Tune[4];     // lighting sliders: [0] spot gain, wall cap, wall base, ambient; [1].x falloff, .y fog density, .z fog height, .w fog brightness
    float4   g_Eye;
};

Texture2D g_Tex;
SamplerState g_Tex_sampler;
#if SCIFI_REFLECTION
Texture2D g_Refl;
SamplerState g_Refl_sampler;
#endif

float hash12(float2 p)
{
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

float vnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), f.x),
                lerp(hash12(i + float2(0, 1)), hash12(i + float2(1, 1)), f.x), f.y);
}

// Soft light from the nearest emitters (ceiling strips, door frames, file previews) onto a surface point.
float spillLight(float3 p, float3 n)
{
    float s = 0.0;
    [unroll]
    for (int i = 0; i < 24; ++i)
    {
        float3 d = g_Lights[i].xyz - p;
        float r2 = dot(d, d);
        float facing = saturate(dot(n, d * rsqrt(r2 + 1e-4)) * 0.75 + 0.25);
        float fo = 1.0 + r2 * g_Tune[1].x;
        s += g_Lights[i].w * facing / (fo * fo);
    }
    return s * g_Tune[0].x;
}

// Cheap gloss: mirror-reflected highlights of the hanging lights (one dot product and a power per light, no extra textures).
float3 glossLights(float3 p, float3 r, float sharp)
{
    float s = 0.0;
    [unroll]
    for (int i = 0; i < 24; ++i)
    {
        float3 d = g_Lights[i].xyz - p;
        float r2 = dot(d, d);
        float a = saturate(dot(r, d * rsqrt(r2 + 1e-4)));
        s += g_Lights[i].w * pow(a, sharp) / (1.0 + r2 * 0.004);
    }
    return float3(0.75, 0.9, 1.0) * s * g_Tune[0].x;
}
struct PSInput
{
    float4 Pos  : SV_POSITION;
    float3 Nrm  : NORMAL;
    float2 UV   : TEXCOORD0;
    float3 WPos : TEXCOORD1;
    float3 LN   : TEXCOORD2;
};

float2 hash22(float2 p)
{
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.xx + q.yz) * q.zy);
}

// Colour that slowly cycles through the spectrum, pulled towards the theme colours so themes still read.
float3 cloudHue(float h, float3 accent, float3 alt)
{
    // Theme base colour only; the clouds just breathe between a dim and a bright shade of it.
    float m = 0.5 + 0.5 * sin(6.2831 * h);
    return accent * (0.4 + 0.6 * m);
}

// Colour of the distant haze; the floor fades into it and the sky meets it at the horizon.
float3 mistColor(float3 accent)
{
    return float3(0.002, 0.003, 0.006) + accent * 0.016;
}

// Seamless ring of far mountains: periodic value noise around the horizon (P = whole cells per full turn).
float ringNoise(float u, float P, float seed)
{
    float x = u * P;
    float i = floor(x);
    float f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = frac(sin((i >= P ? i - P : i) * 127.1 + seed * 311.7) * 43758.5453);
    float b = frac(sin((i + 1.0 >= P - 0.5 ? i + 1.0 - P : i + 1.0) * 127.1 + seed * 311.7) * 43758.5453);
    return lerp(a, b, f);
}

// Elevation (as the sine of the angle) of the skyline in this direction.
float skylineHeight(float3 d)
{
    float u = atan2(d.x, d.z) / 6.2831853 + 0.5;
    // Ridged octaves give pointed peaks with concave slopes (value noise alone makes flat-topped cliffs).
    float base = ringNoise(u, 5.0, 1.0);
    float r = 0.55 * (1.0 - abs(2.0 * ringNoise(u, 7.0, 5.0) - 1.0)) + 0.28 * (1.0 - abs(2.0 * ringNoise(u, 19.0, 6.0) - 1.0)) + 0.12 * (1.0 - abs(2.0 * ringNoise(u, 47.0, 7.0) - 1.0)) + 0.05 * (1.0 - abs(2.0 * ringNoise(u, 113.0, 8.0) - 1.0));
    return 0.010 + 0.012 * base + 0.085 * pow(saturate(r), 1.5) * (0.45 + 0.55 * base);
}

// Sky: drifting soft puffs ("particles") on two layers, slowly shifting colour, plus twinkling motes.
float3 skyColor(float3 d, float t, float3 accent, float3 alt)
{
    float3 base = mistColor(accent) + accent * 0.07 * (exp(-max(d.y, 0.0) * 9.0) - 1.0);
    base += accent * 0.045 * exp(-abs(d.y) * 28.0);  // thin glowing horizon line
    base *= 1.0 - 0.35 * saturate(d.y * 1.4);  // the zenith sinks into near black
    // Far mountain silhouettes ringing the whole horizon: darker than the glowing haze behind them, melting into it at the foot.
    const float skyline = skylineHeight(d);
    const float ridge = 1.0 - smoothstep(skyline - 0.0012, skyline + 0.0012, d.y);
    const float3 rock = mistColor(accent) * 0.15 + accent * 0.0015;
    const float3 mountain = lerp(base * 0.85, rock, smoothstep(0.0, 0.035, d.y));
    if (d.y < 0.015) return lerp(base, mountain, ridge * step(0.0, d.y));
    float up = d.y;
    float2 plane = d.xz / (up + 0.12);
    float3 sum = float3(0, 0, 0);
    for (int layer = 0; layer < 2; ++layer)
    {
        float fl = (float)layer;
        float2 q = plane * (0.55 + fl * 0.45) + float2(t * (0.025 + fl * 0.015), -t * 0.012 * (1.0 + fl)) + fl * 17.0;
        float2 ic = floor(q);
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                float2 cell = ic + float2(dx, dy);
                float2 r = hash22(cell + fl * 31.0);
                float2 r2 = hash22(cell * 1.7 + 5.0);
                float2 ctr = cell + 0.2 + 0.6 * r + 0.18 * sin(t * (0.15 + 0.2 * r2) + r2 * 6.2831);
                float2 df = q - ctr;
                float rad = 0.45 + 0.55 * r2.x;
                float w = exp(-dot(df, df) / (rad * rad * 0.45));
                float presence = smoothstep(0.25, 0.7, r2.y);
                float3 c = cloudHue(r.x + t * 0.025 + fl * 0.3, accent, alt);
                sum += c * w * presence * (0.55 - fl * 0.12);
            }
        }
    }
    float vis = smoothstep(0.015, 0.3, up);
    float3 col = sum * vis * 0.26;

    float2 sp = plane * 14.0;
    float2 sc = floor(sp);
    float2 sr = hash22(sc);
    float2 sf = frac(sp) - 0.2 - 0.6 * sr;
    float tw = 0.5 + 0.5 * sin(t * (1.0 + 2.0 * sr.x) + sr.y * 40.0);
    col += step(0.985, sr.x) * exp(-dot(sf, sf) * 700.0) * tw * vis * 1.2;
    return lerp(base + col, mountain, ridge);
}

// City buildings (axis-aligned boxes). Params: x = emergence 0..1, y = fill fraction, z = flash (+add / -remove),
// w = kind (3 tower, 4 fill bar, 5 road, 6 platform, 7 beam). Colour: rgb neon, a = per-building random seed.
static float gSolid = 0.0;

float3 buildingColor(PSInput In)
{
    float tm = g_Accent.w;
    int kind = (int)(g_Params.w + 0.5);
    float emerge = g_Params.x, fillF = g_Params.y, flash = g_Params.z;
    float3 neon = g_Color.rgb;
    float seed = g_Color.a;

    float3 ctr = float3(g_World[0][3], g_World[1][3], g_World[2][3]);
    float3 dimS = float3(g_World[0][0], g_World[1][1], g_World[2][2]);
    float3 dim = max(abs(dimS), 1e-3);
    float3 lp = (In.WPos - ctr) / (dimS + 1e-6);
    float3 n = normalize(In.Nrm);
    float3 an = abs(n);
    bool top = an.y > 0.5 && lp.y > 0.0;
    bool bottomF = an.y > 0.5 && lp.y <= 0.0;
    float dist = length(In.WPos - g_Light.xyz);

    // Distance to the nearest edge of this face in metres.
    float3 em = (0.5 - abs(lp)) * dim;
    float edgeM;
    if (an.y > 0.5) edgeM = min(em.x, em.z);
    else if (an.x > 0.5) edgeM = min(em.y, em.z);
    else edgeM = min(em.x, em.y);
    float rimW = max(0.12, dist * 0.0016);
    float rim = 1.0 - smoothstep(rimW * 0.5, rimW, edgeM);

    float3 dark = float3(0.006, 0.010, 0.018) + neon * 0.018;
    float3 col = dark;
    float grow = kind == 10 ? 0.0 : 1.0 - emerge;

    if (kind == 3)
    {
        if (!top && !bottomF)
        {
            float u = (an.x > 0.5) ? (lp.z + 0.5) * dim.z : (lp.x + 0.5) * dim.x;
            float v = (lp.y + 0.5) * dim.y;
            float2 cell = floor(float2(u / 2.4, v / 3.2));
            float2 f = frac(float2(u / 2.4, v / 3.2));
            float h = hash12(cell + seed * 97.0 + (an.x > 0.5 ? 13.0 : 0.0));
            float win = step(0.18, f.x) * step(f.x, 0.82) * step(0.22, f.y) * step(f.y, 0.78);
            float lit = step(0.42, h);
            float tw = 0.75 + 0.25 * sin(tm * (0.6 + h * 2.0) + h * 40.0);
            float alias = saturate(1.0 - dist * 0.004);
            // Procedural facade: plated panels with their own tone, mullion ribs, floor spandrels and grime rising from the street.
            float detail = saturate(1.0 - dist * 0.0016);
            float2 plate = float2(u / 7.2, v / 9.6);
            float pt = hash12(floor(plate) + seed * 53.0 + (an.x > 0.5 ? 31.0 : 0.0));
            float2 pf = min(frac(plate), 1.0 - frac(plate));
            float pseam = 1.0 - smoothstep(0.0, 0.025 + dist * 0.0002, min(pf.x, pf.y));
            float rib = 1.0 - smoothstep(0.0, 0.05, abs(f.x - 0.5) - 0.46);
            float spandrel = smoothstep(0.78, 0.84, f.y) * (1.0 - smoothstep(0.94, 1.0, f.y));
            float grime = vnoise(float2(u, v) * 0.35) * 0.6 + vnoise(float2(u, v) * 2.1) * 0.4;
            float street = 1.0 - smoothstep(0.0, 14.0, v);
            float3 skin = float3(0.030, 0.040, 0.055) * (0.4 + pt) * (0.5 + 0.9 * grime) + neon * 0.05 * pt;
            skin *= 1.0 + spandrel * 1.6 - pseam * 0.9 + rib * 0.9;
            skin += neon * (0.16 * pseam + 0.10 * rib + 0.07 * spandrel) * (0.4 + pt);
            skin *= 1.0 - 0.6 * street * grime;
            col = lerp(col, col + skin, detail);
            float3 wc = lerp(neon, float3(1, 0.95, 0.85), 0.35 * h);
            col += wc * win * lit * tw * (1.0 - 0.5 * spandrel) * (1.0 - 0.35 * rib) * (0.3 + 0.4 * (1.0 - v / dim.y)) * alias;
            // a slow scan band climbs each tower, like data being read off it
            col += neon * 0.5 * exp(-pow((frac(tm * 0.07 + seed * 7.0) * 1.3 - 0.15 - v / dim.y) * 14.0, 2.0));
            col += neon * 0.04 * (1.0 - alias) * lit;
        }
        col += neon * rim * 0.8;
        if (top) col += neon * (0.08 + 0.5 * grow) + neon * rim * 0.45;
    }
    else if (kind == 4)
    {
        float y01 = lp.y + 0.5;
        float on = step(y01, fillF);
        float marker = 1.0 - smoothstep(0.0, 0.012 + dist * 0.00004, abs(y01 - fillF));
        float bands = 0.8 + 0.2 * sin(y01 * dim.y * 1.6 - tm * 2.0);
        col += neon * on * 0.5 * bands + neon * 0.04;
        col += neon * (rim * 0.9 + marker * 1.2);
        if (top) col += neon * (0.08 + 0.6 * grow + (fillF > 0.99 ? 0.4 : 0.0));
    }
    else if (kind == 5)
    {
        if (top)
        {
            float along = dim.x > dim.z ? (lp.x + 0.5) * dim.x : (lp.z + 0.5) * dim.z;
            float dir = seed > 0.5 ? 1.0 : -1.0;
            float d = frac(along * 0.12 - tm * 0.25 * dir);
            float packet = pow(saturate(1.0 - d * 5.0), 2.0);
            float across = dim.x > dim.z ? abs(lp.z) : abs(lp.x);
            float centre = 1.0 - smoothstep(0.0, 0.12, across);
            col += neon * packet * centre * 2.0 + neon * centre * 0.08;
        }
        col += neon * rim * 0.7;
    }
    else if (kind == 6)
    {
        // fillF = 0: the drive's footprint slab; fillF > 0: the lit "used space" pad on top of it.
        // Drawn as a glass pane: no body colour, just a faint tint, the grid and a clear outline.
        col = float3(0, 0, 0);
        float rimMul = 0.55 + 0.7 * min(fillF, 1.0);
        float3 rimCol = neon;
        gSolid = top ? smoothstep(0.0, 1.0, emerge) : 0.0;  // region floors hide the world grid beneath
        if (top)
        {
            float2 wp2 = In.WPos.xz;
            if (fillF > 5.5)
            {
                // Pond: deep water with slow ripples.
                float rp = 0.5 + 0.5 * sin(length(wp2 - ctr.xz) * 2.4 - tm * 1.4);
                col += float3(0.10, 0.45, 0.80) * (0.09 + 0.06 * pow(rp, 6.0));
                rimCol = float3(0.2, 0.6, 1.0);
                rimMul = 0.5;
            }
            else if (fillF > 4.5)
            {
                // Lawn: a faint green wash with fine stipple.
                float2 sc = frac(wp2 * 0.8) - 0.5;
                float fws = max(fwidth(wp2.x) * 0.8, 1e-4);
                float st = (1.0 - smoothstep(0.07, 0.07 + fws * 1.5, length(sc))) * saturate(1.0 - fws * 4.0);
                col += float3(0.15, 0.80, 0.35) * (0.045 + 0.16 * st);
                rimCol = float3(0.2, 0.85, 0.45);
                rimMul = 0.35;
            }
            else if (fillF > 3.5)
            {
                // Path: small paving squares, a simple cousin of the street hexagons.
                float2 pp = wp2 / 1.6;
                float2 gp = abs(frac(pp) - 0.5);
                float fwp = max(fwidth(pp.x) + fwidth(pp.y), 1e-4);
                float pl = saturate(1.0 - (0.5 - max(gp.x, gp.y)) / (fwp * 1.2)) * saturate(1.0 - fwp * 3.0);
                col += neon * (0.04 + 0.22 * pl) + float3(0.5, 0.55, 0.6) * 0.012;
                rimMul = 0.9;
            }
            else if (fillF > 2.5)
            {
                // The computer's own region: solid pale panel plating, 8 m panels with fine seams and a dot at each corner.
                float2 pp = wp2 / 8.0;
                float2 gp = abs(frac(pp) - 0.5);
                float fwp = max(fwidth(pp.x) + fwidth(pp.y), 1e-4);
                float pl = saturate(1.0 - (0.5 - max(gp.x, gp.y)) / (fwp * 1.2)) * saturate(1.0 - fwp * 3.0);
                float2 pf = wp2 / 1.0;
                float2 gf = abs(frac(pf) - 0.5);
                float fwf = max(fwidth(pf.x) + fwidth(pf.y), 1e-4);
                float pf1 = saturate(1.0 - (0.5 - max(gf.x, gf.y)) / (fwf * 1.2)) * saturate(1.0 - fwf * 3.0);
                float2 dc = (frac(pp + 0.5) - 0.5) * 8.0;
                float dd = (1.0 - smoothstep(0.12, 0.12 + fwp * 8.0 * 1.5, length(dc))) * saturate(1.0 - fwp * 3.0);
                col += float3(0.92, 0.96, 1.0) * (0.20 + 0.05 * hash12(floor(pp)) + 0.07 * pl + 0.025 * pf1 + 0.12 * dd);
                rimCol = float3(0.92, 0.96, 1.0);
                rimMul = 0.9;
            }
            else if (fillF > 1.5)
            {
                // Pavement: muted grey with faint slab joints.
                float2 pp = wp2 / 1.2;
                float2 gp = abs(frac(pp) - 0.5);
                float fwp = max(fwidth(pp.x) + fwidth(pp.y), 1e-4);
                float pl = saturate(1.0 - (0.5 - max(gp.x, gp.y)) / (fwp * 1.2)) * saturate(1.0 - fwp * 3.0);
                col += float3(0.62, 0.65, 0.68) * (0.09 + 0.08 * pl);
                rimCol = float3(0.62, 0.65, 0.68);
                rimMul = 0.12;
            }
            else if (fillF > 0.5)
            {
                // Main road and region floor: a plain, dark tinted bed.
                col += neon * 0.05 + float3(0.5, 0.55, 0.6) * 0.008;
                rimMul = 0.5;
            }
            else
            {
                // Empty ground: a pale fill with a sparse dot lattice.
                float2 dp = wp2 / 3.0;
                float2 dc = frac(dp) - 0.5;
                float fwd = max(fwidth(dp.x), 1e-4);
                float dt = (1.0 - smoothstep(0.05, 0.05 + fwd * 1.5, length(dc))) * saturate(1.0 - fwd * 5.0);
                col += float3(0.70, 0.82, 0.88) * 0.02 + neon * dt * 0.16;
            }
        }
        col += rimCol * rim * rimMul;
    }
    else if (kind == 10)
    {
        // Race track: a glass deck on a grid like the city floor. Works in the box's own axes so pieces can lie at any angle.
        float3 c0 = float3(g_World[0][0], g_World[1][0], g_World[2][0]);
        float3 c1 = float3(g_World[0][1], g_World[1][1], g_World[2][1]);
        float3 c2 = float3(g_World[0][2], g_World[1][2], g_World[2][2]);
        float3 dmn = max(float3(length(c0), length(c1), length(c2)), 1e-4);
        float3 dd = In.WPos - ctr;
        float3 q = float3(dot(dd, c0), dot(dd, c1), dot(dd, c2)) / (dmn * dmn);
        float3 nl = float3(dot(n, c0), dot(n, c1), dot(n, c2)) / dmn;
        float3 e3 = (0.5 - abs(q)) * dmn;
        float edgeT = abs(nl.y) > 0.5 ? e3.x : (abs(nl.x) > 0.5 ? min(e3.y, e3.z) : min(e3.x, e3.y));
        float rimT = 1.0 - smoothstep(rimW * 0.5, rimW * 1.2, edgeT);
        float strut = fillF;
        if (strut < 0.5 && abs(nl.y) > 0.5)
        {
            float2 uv = float2((q.x + 0.5) * dmn.x, (q.z + 0.5) * dmn.z + seed) / 1.2;
            float2 gp = abs(frac(uv) - 0.5);
            float2 fwT = max(fwidth(uv), 1e-4);
            float gl = 1.0 - smoothstep(0.0, 1.5, min((0.5 - gp.x) / fwT.x, (0.5 - gp.y) / fwT.y));
            gl *= saturate(1.0 - max(fwT.x, fwT.y) * 1.6);
            float across = abs(q.x) * dmn.x;
            float dash = step(0.5, frac(((q.z + 0.5) * dmn.z + seed) / 4.0)) * (1.0 - smoothstep(0.05, 0.12, across));
            float face = nl.y > 0.0 ? 1.0 : 0.35;
            col += neon * (0.05 + 0.34 * gl + 0.55 * dash) * face;
            gSolid = (nl.y > 0.0 ? 0.55 : 0.25) * smoothstep(0.0, 1.0, emerge);
        }
        col += neon * rimT * (strut > 0.5 ? 0.5 : 1.25);
    }
    else if (kind == 8)
    {
        // Hologram model: fresnel outline, slow scan bands, tinted by the neon colour.
        float3 V = normalize(g_Light.xyz - In.WPos);
        float fres = pow(1.0 - abs(dot(n, V)), 2.2);
        float lit = 0.35 + 0.65 * saturate(n.y * 0.5 + 0.5);
        float band = 0.5 + 0.5 * sin(In.WPos.y * 9.0 - tm * 2.5);
        float sweepB = exp(-pow(frac(tm * 0.25 + seed) * 2.0 - 0.5 - frac(In.WPos.y * 0.07), 2.0) * 40.0);
        col = neon * (0.10 * lit + fres * 1.5 + 0.12 * pow(band, 6.0) + sweepB * 0.25) + dark;
    }
    else if (kind == 9)
    {
        // Textured model: the glTF atlas, lit, with a faint neon rim.
        float3 V = normalize(g_Light.xyz - In.WPos);
        float fres = pow(1.0 - abs(dot(n, V)), 3.0);
        float diff = saturate(dot(n, normalize(float3(0.4, 0.8, 0.5))));
        float3 tex = g_Tex.Sample(g_Tex_sampler, In.UV).rgb;
        float mxc = max(tex.r, max(tex.g, tex.b)), mnc = min(tex.r, min(tex.g, tex.b));
        float emis = step(0.97, mxc) * smoothstep(0.35, 0.7, mxc - mnc);  // saturated tiles are emissive materials
        col = tex * (0.3 + 0.7 * diff) * (1.0 - emis) + tex * emis * 2.2 + neon * fres * 0.25;
        return col;  // skip the city haze: the model sits in lit rooms
    }
    else
    {
        float sweep = 0.6 + 0.4 * sin(In.WPos.y * 0.7 - tm * 6.0);
        col = neon * (1.4 + 1.2 * abs(flash)) * sweep;
    }

    // Change flash: green-white for additions, red-orange for removals, soft blue-ish pulse for edits.
    float3 fc = flash >= 0.0 ? float3(0.7, 1.0, 0.8) : float3(1.0, 0.25, 0.08);
    float fa = abs(flash);
    if (kind != 7) col += fc * fa * (0.35 + rim * 1.5 + (top ? 0.6 : 0.0));

    // The tops flare while a building is still rising out of the grid.
    if (grow > 0.001 && kind != 7 && (top || rim > 0.0)) col += neon * grow * 1.2 * (top ? 1.0 : rim);

    float mist = smoothstep(650.0, 980.0, dist);  // dark ring that masks the edge of the map
    // Light ground-hugging fog: a pale bank that thickens with distance and thins out within a few metres of the street.
    // Seen from above, the ground bank is a thin layer: fade it out with height and view steepness so floors stay dark.
    float steep = saturate(abs(In.WPos.y - g_Light.y) / max(dist, 1.0) * 1.6) * saturate((g_Light.y - In.WPos.y) / 35.0);
    float viewFog = lerp(1.0, g_Tune[2].w, steep);
    float gfog = saturate((dist - g_Tune[2].y) / 70.0) * exp(-max(In.WPos.y, 0.0) * 0.04 / g_Tune[1].z) * 0.6 * g_Tune[2].z * g_Tune[1].y * viewFog;
    if (kind == 6 || kind == 10) return (col * (1.0 - gfog) + float3(0.12, 0.24, 0.3) * gfog * 0.5 * lerp(1.0, 0.15, steep)) * (1.0 - mist) * smoothstep(0.0, 1.0, emerge);  // glass: additive light only, no haze (premultiplied blend)
    // Dense low fog: barely there up close, building to a thick glowing bank with distance, deepest near the ground and
    // lit by the neon of whatever is inside it.
    float fogD = 1.0 - exp(-pow(max(dist - g_Tune[2].y, 0.0) * 0.0042 * g_Tune[1].y, 1.2));
    float fog = saturate(fogD * (0.35 + 0.65 * exp(-max(In.WPos.y, 0.0) * 0.045 / g_Tune[1].z)))  * lerp(1.0, 0.7, steep);
    float ft = g_Tune[2].x;  // fog tint: 0 violet, 0.33 blue, 0.66 teal, 1 grey
    float3 fogHue = ft < 0.33 ? lerp(float3(0.4, 0.25, 0.55), float3(0.2, 0.3, 0.55), ft / 0.33) : (ft < 0.66 ? lerp(float3(0.2, 0.3, 0.55), float3(0.22, 0.4, 0.5), (ft - 0.33) / 0.33) : lerp(float3(0.22, 0.4, 0.5), float3(0.4, 0.42, 0.44), (ft - 0.66) / 0.34));
    float3 fogCol = (fogHue * 0.3 + neon * g_Tune[3].x) * g_Tune[1].w;  // dark teal haze: keeps the glow and depth of the fog without lightening dark floors
    col = lerp(col, fogCol, saturate(fog * 0.62 + gfog * 1.1)) + neon * 0.05 * fog * (1.0 - fog);
    return col * (1.0 - mist) + mistColor(g_Accent.rgb) * mist;
}

void main(in PSInput In, out float4 Color : SV_TARGET)
{
    float tm = g_Accent.w;
    if (g_Params.w > 11.5 && g_Params.w < 12.5)
    {
        // Solid hologram body: lit flat colour with a soft rim; only the summon dissolve punches holes (emerge < 1).
        float3 V = normalize(g_Light.xyz - In.WPos);
        float3 hn = normalize(In.Nrm);
        float fres = pow(1.0 - abs(dot(hn, V)), 3.0);
        float n = hash22(floor(In.WPos.xz * 60.0) + floor(In.WPos.y * 60.0)).x;
        float emg = g_Params.x;
        float edge = 0.0;
        if (emg < 0.999)
        {
            if (n > emg * 1.15 - 0.1) discard;
            edge = smoothstep(emg * 1.15 - 0.25, emg * 1.15 - 0.1, n);
        }
        float shade = 0.5 + 0.5 * saturate(dot(hn, normalize(float3(0.3, 0.8, 0.5))) * 0.5 + 0.5);
        float spec = pow(saturate(dot(reflect(-normalize(float3(0.3, 0.8, 0.5)), hn), V)), 24.0);
        float3 c = g_Color.rgb * (shade + fres * 0.35) + spec * 0.35 + float3(0.7, 1.0, 1.0) * edge * 0.8;
        c += g_Color.rgb * g_Params.z;
        Color = float4(c, 1.0);
        return;
    }
    if (g_Params.w > 2.5)
    {
        gSolid = 0.0;
        const float3 bc = buildingColor(In);
        const bool glass = (int)(g_Params.w + 0.5) == 6 || (int)(g_Params.w + 0.5) == 10;
        Color = float4(bc, glass ? max(gSolid, saturate(0.06 + 0.9 * max(bc.r, max(bc.g, bc.b)))) : 1.0);
        return;
    }
    // Cloud dome (cube centred on the eye, seen from inside).
    if (g_Params.w > 1.5)
    {
        float3 d = normalize(In.WPos - g_Light.xyz);
        Color = float4(skyColor(d, tm, g_Accent.rgb, g_Alt.rgb), 1.0);
        return;
    }
    // Dark glass floor with glowing grid lines, brighter every 5 m, fading into the void.
    if (g_Params.w > 0.5)
    {
        // The floor is a thin cube with no culling and no depth write: drop its underside and sides so only the y=0 top
        // face draws. Otherwise a second, offset copy of the grid shows through (the "double vision").
        if (In.WPos.y < -0.01) discard;
        float2 wp = In.WPos.xz;
        if (g_Params.z > 1.5)
        {
            // Interior floor: large dark stone tiles with fine seams and a soft sheen. No grid, no accent colour.
            float2 tc = wp / 1.8;
            float2 tid = floor(tc);
            float2 tf = abs(frac(tc) - 0.5);
            float tr = hash12(tid);
            float2 tfw = max(fwidth(tc), 1e-4);
            float seam = 1.0 - smoothstep(0.0, 1.5, min((0.5 - tf.x) / tfw.x, (0.5 - tf.y) / tfw.y));
            seam *= saturate(1.0 - max(tfw.x, tfw.y) * 2.0);
            float fine = vnoise(wp * 9.0 + tr * 40.0);
            float cloud = vnoise(wp * 1.3 + tr * 17.0);
            float3 tile = float3(0.030, 0.032, 0.036) * (0.75 + 0.5 * tr) * (0.85 + 0.25 * fine) * (0.8 + 0.4 * cloud);
            tile *= 1.0 - 0.7 * seam;
            float3 vd = normalize(In.WPos - g_Light.xyz);
            float sheen = pow(1.0 - saturate(abs(vd.y)), 4.0) * 0.5;
            float3 c = tile * 0.35 * (1.0 + sheen) + float3(0.0004, 0.0004, 0.0005);
            c += float3(0.8, 0.85, 0.9) * spillLight(In.WPos, float3(0, 1, 0)) * 0.06 * (0.5 + 0.5 * tr) * (1.0 - 0.6 * seam);
            // Glossy polish: faint, slightly blurred mirror of the ceiling lights plus a soft horizon gradient, stronger on some tiles.
            float3 rv = float3(vd.x, -vd.y, vd.z);
            float gl = 0.6 + 0.8 * hash12(tid + 7.0);
            float3 gloss = glossLights(In.WPos, rv, 28.0 + 40.0 * tr) * (0.5 + 0.5 * sheen) + float3(0.10, 0.13, 0.16) * pow(1.0 - saturate(abs(vd.y)), 3.0) * 0.5;
            c += gloss * gl * (1.0 - 0.8 * seam) * 0.3;
            Color = float4(c, 1.0);
            return;
        }
        float2 gm = abs(frac(wp) - 0.5);
        float2 gM = abs(frac(wp * 0.2) - 0.5);
        float2 fw = max(fwidth(wp), 1e-4);
        float2 dm = (0.5 - gm) / fw;  // per-axis distance in pixels so lines stay crisp in both directions
        float minor = 1.0 - smoothstep(0.0, 1.5, min(dm.x, dm.y));
        float2 dM = (0.5 - gM) / (fw * 0.2);
        float majX = 1.0 - smoothstep(0.0, 2.2, dM.x);
        float majZ = 1.0 - smoothstep(0.0, 2.2, dM.y);
        float major = max(majX, majZ);
        float majorGlow = 1.0 - smoothstep(0.0, 14.0, min(dM.x, dM.y));
        float r = length(wp);
        float3 eyeP = g_Light.xyz;
        float dist = length(In.WPos - eyeP);
        float mist = smoothstep(650.0, 980.0, dist);  // dark ring at the limit of view
        float fade = (1.0 - mist) * (1.0 - mist) * (1.0 - mist);
        float fwMax = max(fw.x, fw.y);
        minor *= saturate(1.0 - fwMax * 1.5);
        float aa = saturate(1.0 - fwMax * 0.25);
        major *= aa;
        majX *= aa;
        majZ *= aa;
        majorGlow *= aa;
        float pulse = 0.92;

        // Power pulses: bright packets racing along the major lines, plus a slow expanding ring.
        float2 id = floor(wp * 0.2);
        float phX = frac(wp.y * 0.02 + tm * 0.10 + frac(sin(id.x * 12.9898) * 43758.5) );
        float phZ = frac(wp.x * 0.02 - tm * 0.08 + frac(sin(id.y * 78.233) * 24634.6));
        float pk = pow(saturate(1.0 - phX * 6.0), 2.0) * majX + pow(saturate(1.0 - phZ * 6.0), 2.0) * majZ;
        float wave = exp(-pow((r - frac(tm * 0.04) * 500.0) * 0.25, 2.0)) * (minor * 0.5 + major);

        float3 col = float3(0.004, 0.008, 0.016);
        col += g_Accent.rgb * (minor * 0.10 + major * 0.9 + majorGlow * 0.07) * pulse;
        col += lerp(g_Accent.rgb, float3(1, 1, 1), 0.35) * (pk * 1.6);

        // Brushed-metal floor. The room is rendered mirrored into g_Refl (half-res, same camera) and sampled here by screen
        // position: blurred along the brush direction, roughened by grain, weighted by Fresnel, tinted like metal.
        float3 vdir = normalize(In.WPos - g_Light.xyz);
        float cosT = saturate(abs(vdir.y));
        float fres = 0.5 + 0.5 * pow(1.0 - cosT, 5.0);  // F0 ~0.5: metals reflect strongly even head-on
        float detail = 1.0 - smoothstep(0.012, 0.06, fwMax);  // fade grain out before it aliases in the distance
        float2 plate = floor(wp * 0.2);
        float plateRnd = hash12(plate);
        float streak = vnoise(float2(wp.x * 22.0 + plateRnd * 90.0, wp.y * 1.6 + plateRnd * 30.0));
        float grain = vnoise(wp * 70.0);
        float rough = lerp(0.5, streak, detail);
        float3 metal = float3(0.01, 0.0115, 0.014) * (0.8 + 0.5 * plateRnd) * (0.85 + 0.3 * lerp(0.5, streak * 0.7 + grain * 0.3, detail));
        // Dark grooves between 1 m tiles.
        float groove = (1.0 - smoothstep(0.0, 1.2, min(dm.x, dm.y))) * detail;
        metal *= 1.0 - 0.6 * groove;

        float3 reflSum = float3(0, 0, 0);
        float cov = 0.0;
        if (g_Color.z > 0.5)
        {
            float2 uv = In.Pos.xy * g_Color.xy;
            float blurV = (0.004 + 0.016 * rough) * (0.4 + 0.6 * (1.0 - cosT));
            float jx = (grain - 0.5) * 0.004 * detail;
            float wsum = 0.0;
            [unroll]
            for (int i = -3; i <= 3; ++i)
            {
                float w = exp(-0.28 * i * i);
                #if SCIFI_REFLECTION
                float4 s = g_Refl.SampleLevel(g_Refl_sampler, uv + float2(jx, i * blurV), 0);
                #else
                float4 s = float4(0, 0, 0, 0);
                #endif
                reflSum += s.rgb * w;
                cov += s.a * w;
                wsum += w;
            }
            reflSum /= wsum;
            cov /= wsum;
        }

        float3 metalTint = lerp(float3(0.8, 0.85, 0.9), g_Accent.rgb, 0.25);
        float rk = fres * fade * (1.0 - 0.45 * rough);
        float3 sky = skyColor(float3(vdir.x, -vdir.y, vdir.z), tm, g_Accent.rgb, g_Alt.rgb);
        float3 reflection = (reflSum + sky * (1.0 - saturate(cov))) * metalTint;
        float gd = length(In.WPos.xz - eyeP.xz) + 0.35 * max(eyeP.y, 0.0);
        col *= fade * exp(-gd * 0.008) / (1.0 + gd * gd / 6000.0);  // grid and pulses dim steadily with distance from the player
        col += metal * (1.0 - fres) * fade + reflection * rk * 0.15;
        col += float3(0.14, 0.42, 0.55) * spillLight(In.WPos, float3(0, 1, 0)) * 0.3 * fade * (0.6 + 0.4 * rough);
        // Distance haze: beyond ~95 m the floor is an opaque patch of exactly the horizon colour, so the grid has no visible edge.
        col = col * (1.0 - mist) + mistColor(g_Accent.rgb) * mist;
        Color = float4(col, 1.0);
        return;
    }

    float3 n = normalize(In.Nrm);
    float3 l = normalize(-g_Light.xyz);
    float  diffuse = saturate(dot(n, l));
    float  light = 0.35 + 0.65 * diffuse;

    float4 label = g_Tex.Sample(g_Tex_sampler, In.UV);
    if (In.LN.z < 0.5) label = float4(0, 0, 0, 0);
    float3 base = lerp(g_Color.rgb, float3(1.0, 0.95, 0.85), label.a);
    // g_Params.y: 0 = lit text over colour, 1 = lit image, 2 = unlit image, 3 = unlit text over colour
    if (g_Params.y > 0.5 && g_Params.y < 2.5)
    {
        base = label.rgb * g_Color.r;
        light = (g_Params.y > 1.5) ? 1.0 : 0.85 + 0.15 * diffuse;
    }
    else if (g_Params.y > 2.5)
    {
        light = 1.0;
    }

    float3 spillAdd = float3(0, 0, 0);
    bool shellWall = false;
    // Interior shell blocks (g_Params.z == 2): dark panelled walls with seams, per-panel tone, grime and a gloomy falloff.
    if (g_Params.z > 1.5 && g_Params.x > 3.5 && g_Params.y < 0.5)
    {
        // Glossy black (picture frames): no texture, just a faint sheen where the spot lights fall.
        shellWall = true;
        float sh = spillLight(In.WPos, n);
        sh = 0.2 * sh / (0.2 + sh);
        base = float3(0.004, 0.0045, 0.005);
        light = 1.0;
        spillAdd = float3(0.6, 0.8, 0.9) * sh * 0.12;
    }
    else if (g_Params.z > 1.5 && g_Params.y < 0.5 && max(g_Color.r, max(g_Color.g, g_Color.b)) < 0.2)
    {
        shellWall = true;
        float2 uv2 = abs(n.y) > 0.5 ? In.WPos.xz : (abs(n.x) > 0.5 ? In.WPos.zy : In.WPos.xy);
        float2 cell = uv2 / float2(1.6, 1.2);
        float2 fc = frac(cell);
        float2 fwp = max(fwidth(cell), 1e-4);
        float2 sd = min(fc, 1.0 - fc) / fwp;
        float seam = 1.0 - smoothstep(0.0, 1.6, min(sd.x, sd.y) );
        float2 pid = floor(cell);
        float tone = 0.75 + 0.5 * hash12(pid + floor(In.WPos.x * 0.13 + In.WPos.z * 0.29) * 7.0);
        float detailK = 1.0 - smoothstep(0.02, 0.12, max(fwp.x, fwp.y));
        float grime = vnoise(uv2 * 3.0) * 0.6 + vnoise(uv2 * 17.0) * 0.4;
        base *= lerp(1.0, tone * (0.6 + 0.8 * grime), detailK);
        base *= 1.0 - 0.7 * seam * detailK;
        // Hairline accent light running along every fourth seam row of the walls.
        float lineRow = (fmod(pid.y, 3.0) < 0.5 && abs(n.y) < 0.5) ? 1.0 : 0.0;
        // Compressed so crowded small rooms cannot blow out, plus a faint floor so walls far from every light are not black.
        float sp = spillLight(In.WPos, n);
        sp = g_Tune[0].y * sp / (g_Tune[0].y + sp) + g_Tune[0].z;
        spillAdd = float3(0.14, 0.45, 0.6) * sp * (0.3 + base * 8.0) * (1.0 - 0.5 * seam * detailK);
        float3 incident = normalize(In.WPos - g_Eye.xyz);
        float3 reflected = reflect(incident, n);
        float gloss = (0.6 + 0.8 * hash12(pid + 7.0)) * (1.0 - 0.8 * seam * detailK);
        spillAdd += glossLights(In.WPos, reflected, 28.0 + 40.0 * hash12(pid)) * gloss * 0.3;
        base += float3(0.2, 0.8, 1.0) * 0.05 * seam * lineRow * detailK;
        float below = smoothstep(0.0, 2.6, In.WPos.y);
        float ceilGlow = smoothstep(4.5, 8.5, In.WPos.y);
        light = (0.02 + 0.05 * diffuse) * g_Tune[0].w * (abs(n.y) > 0.5 ? 1.0 : (0.45 + 0.55 * below));
        base += float3(0.25, 0.8, 1.0) * ceilGlow * 0.03 * (abs(n.y) < 0.5 ? 1.0 : 0.0);
    }

    float3 outCol = base * light + spillAdd;
    // g_Params.x >= 0 (and not an unlit image/flat block): glowing cyan edge on the card faces; 1 = hovered.
    if (!shellWall && g_Params.x > -0.5 && g_Params.x < 2.5 && g_Params.y < 1.5 && abs(n.z) > 0.5)
    {
        float2 e = min(In.UV, 1.0 - In.UV);
        float edge = min(e.x * 1.55, e.y);
        float rim = 1.0 - smoothstep(0.012, 0.034, edge);
        float halo = 1.0 - smoothstep(0.0, 0.12, edge);
        float glow = 0.55 + 0.9 * g_Params.x;
        outCol = outCol * (1.0 - 0.35 * halo) + (g_Params.z > 1.5 ? float3(0.2, 0.85, 1.0) : g_Accent.rgb) * (rim * glow + halo * halo * 0.18 * glow);
    }

    Color = float4(outCol, 1.0);
}
