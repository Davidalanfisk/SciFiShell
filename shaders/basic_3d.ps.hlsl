cbuffer Constants
{
    float4x4 g_MVP;
    float4x4 g_World;
    float4   g_Color;
    float4   g_Params;
    float4   g_Light;
    float4   g_Accent;
    float4   g_Alt;
};

Texture2D g_Tex;
SamplerState g_Tex_sampler;
Texture2D g_Refl;
SamplerState g_Refl_sampler;

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
    return float3(0.001, 0.0015, 0.003) + accent * 0.006;
}

// Sky: drifting soft puffs ("particles") on two layers, slowly shifting colour, plus twinkling motes.
float3 skyColor(float3 d, float t, float3 accent, float3 alt)
{
    float3 base = mistColor(accent) + accent * 0.07 * (exp(-max(d.y, 0.0) * 9.0) - 1.0);
    base += accent * 0.045 * exp(-abs(d.y) * 28.0);  // thin glowing horizon line
    base *= 1.0 - 0.35 * saturate(d.y * 1.4);  // the zenith sinks into near black
    if (d.y < 0.015) return base;
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
    return base + col;
}

// City buildings (axis-aligned boxes). Params: x = emergence 0..1, y = fill fraction, z = flash (+add / -remove),
// w = kind (3 tower, 4 fill bar, 5 road, 6 platform, 7 beam). Colour: rgb neon, a = per-building random seed.
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
    float grow = 1.0 - emerge;

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
            float3 wc = lerp(neon, float3(1, 0.95, 0.85), 0.35 * h);
            col += wc * win * lit * tw * (0.45 + 0.6 * (1.0 - v / dim.y)) * alias;
            // a slow scan band climbs each tower, like data being read off it
            col += neon * 0.5 * exp(-pow((frac(tm * 0.07 + seed * 7.0) * 1.3 - 0.15 - v / dim.y) * 14.0, 2.0));
            col += neon * 0.04 * (1.0 - alias) * lit;
        }
        col += neon * rim * 1.1;
        if (top) col += neon * (0.10 + 0.5 * grow) + neon * rim * 0.6;
    }
    else if (kind == 4)
    {
        float y01 = lp.y + 0.5;
        float on = step(y01, fillF);
        float marker = 1.0 - smoothstep(0.0, 0.012 + dist * 0.00004, abs(y01 - fillF));
        float bands = 0.8 + 0.2 * sin(y01 * dim.y * 1.6 - tm * 2.0);
        col += neon * on * 0.65 * bands + neon * 0.05;
        col += neon * (rim * 1.2 + marker * 1.6);
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
        col += neon * rim * 0.9;
    }
    else if (kind == 6)
    {
        // fillF = 0: the drive's footprint slab; fillF > 0: the lit "used space" pad on top of it.
        if (top)
        {
            float2 wp = In.WPos.xz;
            float2 g = abs(frac(wp / 10.0) - 0.5);
            float2 fw = max(fwidth(wp / 10.0), 1e-4);
            float gl = 1.0 - smoothstep(0.0, 2.0, min((0.5 - g.x) / fw.x, (0.5 - g.y) / fw.y));
            gl *= saturate(1.0 - max(fw.x, fw.y) * 3.0);
            col += neon * gl * (0.10 + 0.5 * fillF);
            col += neon * (0.03 + 0.10 * fillF);
        }
        col += neon * rim * (0.7 + 0.9 * fillF);
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

    float mist = smoothstep(150.0, 1000.0, dist);
    return col * (1.0 - mist) + mistColor(g_Accent.rgb) * mist;
}

void main(in PSInput In, out float4 Color : SV_TARGET)
{
    float tm = g_Accent.w;
    if (g_Params.w > 2.5)
    {
        Color = float4(buildingColor(In), 1.0);
        return;
    }
    // Cloud dome (cube centred on the eye, seen from inside).
    if (g_Params.w > 1.5)
    {
        float3 d = normalize(In.WPos - g_Light.xyz);
        Color = float4(skyColor(d, tm, g_Accent.rgb, g_Alt.rgb), 1.0);
        return;
    }
    // Tron-style floor: dark glass with glowing grid lines, brighter every 5 m, fading into the void.
    if (g_Params.w > 0.5)
    {
        // The floor is a thin cube with no culling and no depth write: drop its underside and sides so only the y=0 top
        // face draws. Otherwise a second, offset copy of the grid shows through (the "double vision").
        if (In.WPos.y < -0.01) discard;
        float2 wp = In.WPos.xz;
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
        float mist = smoothstep(20.0, 700.0, dist);  // distance haze: everything melts into the horizon colour
        float fade = (1.0 - mist) * (1.0 - mist) * (1.0 - mist);
        float fwMax = max(fw.x, fw.y);
        minor *= saturate(1.0 - fwMax * 1.5);
        float aa = saturate(1.0 - fwMax * 0.25);
        major *= aa;
        majX *= aa;
        majZ *= aa;
        majorGlow *= aa;
        float pulse = 0.85 + 0.15 * sin(tm * 1.3 - r * 0.6);

        // Power pulses: bright packets racing along the major lines, plus a slow expanding ring.
        float2 id = floor(wp * 0.2);
        float phX = frac(wp.y * 0.02 + tm * 0.10 + frac(sin(id.x * 12.9898) * 43758.5) );
        float phZ = frac(wp.x * 0.02 - tm * 0.08 + frac(sin(id.y * 78.233) * 24634.6));
        float pk = pow(saturate(1.0 - phX * 6.0), 2.0) * majX + pow(saturate(1.0 - phZ * 6.0), 2.0) * majZ;
        float wave = exp(-pow((r - frac(tm * 0.04) * 500.0) * 0.25, 2.0)) * (minor * 0.5 + major);

        float3 col = float3(0.004, 0.008, 0.016);
        col += g_Accent.rgb * (minor * 0.10 + major * 0.9 + majorGlow * 0.07) * pulse;
        col += lerp(g_Accent.rgb, float3(1, 1, 1), 0.35) * (pk * 1.6 + wave * 0.9);

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
                float4 s = g_Refl.SampleLevel(g_Refl_sampler, uv + float2(jx, i * blurV), 0);
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
        col *= fade * exp(-gd * 0.018) / (1.0 + gd * gd / 900.0);  // grid and pulses dim steadily with distance from the player
        col += metal * (1.0 - fres) * fade + reflection * rk * 0.3;
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

    float3 outCol = base * light;
    // g_Params.x >= 0 (and not an unlit image/flat block): glowing cyan edge on the card faces; 1 = hovered.
    if (g_Params.x > -0.5 && g_Params.y < 1.5 && abs(n.z) > 0.5)
    {
        float2 e = min(In.UV, 1.0 - In.UV);
        float edge = min(e.x * 1.55, e.y);
        float rim = 1.0 - smoothstep(0.012, 0.034, edge);
        float halo = 1.0 - smoothstep(0.0, 0.12, edge);
        float glow = 0.55 + 0.9 * g_Params.x;
        outCol = outCol * (1.0 - 0.35 * halo) + g_Accent.rgb * (rim * glow + halo * halo * 0.18 * glow);
    }

    Color = float4(outCol, 1.0);
}
