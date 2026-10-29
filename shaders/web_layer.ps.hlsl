// Pixel shader for the web depth compositor.
//  mode 0 (g_Params.x): a rectangular region of the page texture (uv rect in g_Light) with rounded corners,
//                       a soft rim light and an optional glow tint.
//  mode 1             : a soft drop shadow / glow card drawn behind an element.
cbuffer Constants
{
    float4x4 g_MVP;
    float4x4 g_World;
    float4   g_Color;   // mode 0: rgb = rim/glow tint, a = opacity.  mode 1: rgb = glow colour, a = intensity
    float4   g_Params;  // x = mode, y = corner radius (in units of the quad height), z = quad aspect (w/h), w = rim strength
    float4   g_Light;   // mode 0: xy = uv offset, zw = uv scale.  mode 1: xy = inner rect half extent (0..1 of quad)
    float4   g_Accent;
    float4   g_Alt;
};

Texture2D g_Tex;
SamplerState g_Tex_sampler;

struct PSInput
{
    float4 Pos  : SV_POSITION;
    float3 Nrm  : NORMAL;
    float2 UV   : TEXCOORD0;
    float3 WPos : TEXCOORD1;
};

void main(in PSInput In, out float4 Color : SV_TARGET)
{
    // mode 2: the radial tool ring (Tron identity-disc look). g_Params = (2, segments, hover index (-1 none), time),
    // g_Light = (rotation, open 0..1, selection pulse), g_Color.rgb = accent.
    if (g_Params.x > 2.5)
    {
        // mode 3: glass card. g_Params = (3, corner radius in quad heights, aspect, 0), g_Color.a = opacity.
        float aspect = g_Params.z;
        float2 ext = float2(0.5 * aspect, 0.5);
        float2 pp = (In.UV - 0.5) * float2(aspect, 1.0);
        float r = min(g_Params.y, min(ext.x, ext.y));
        float2 qq = abs(pp) - (ext - r);
        float sd = length(max(qq, 0.0)) + min(max(qq.x, qq.y), 0.0) - r;
        float px = max(fwidth(sd), 1e-5);
        float cover = 1.0 - smoothstep(-px, px, sd);
        float edge = 1.0 - smoothstep(0.0, px * 2.2, -sd);
        float sheen = saturate(1.0 - In.UV.y) * 0.5 + 0.5 * saturate(1.0 - length(In.UV - float2(0.1, 0.0)) * 1.4);
        float3 glass = lerp(float3(0.012, 0.03, 0.05), float3(0.03, 0.08, 0.12), sheen);
        float3 rgb = glass + g_Accent.rgb * edge * 0.7;
        Color = float4(rgb, cover * saturate(g_Color.a + edge * 0.5));
        return;
    }
    if (g_Params.x > 1.5)
    {
        const float TAU = 6.2831853;
        float2 p = (In.UV - 0.5) * 2.0;
        float r = length(p);
        float segs = g_Params.y;
        float open = g_Light.y;
        float ang = atan2(p.x, -p.y) + g_Light.x;
        ang = ang - TAU * floor(ang / TAU);
        float u = ang / TAU * segs;
        float idx = floor(u);
        float f = frac(u);
        float hv = g_Params.z;
        float dIdx = abs(idx - hv);
        dIdx = min(dIdx, segs - dIdx);
        float hover = (hv < -0.5) ? 0.0 : saturate(1.0 - dIdx);
        float3 acc = g_Color.rgb;

        const float rIn = 0.60, rOut = 0.90;
        // Angular gap between segments, measured as distance in "arc" units.
        float arc = min(f, 1.0 - f) * (TAU / segs) * r;
        float seg = smoothstep(0.012, 0.03, arc) * smoothstep(rIn - 0.01, rIn + 0.005, r) * (1.0 - smoothstep(rOut - 0.005, rOut + 0.01, r));

        float3 glass = lerp(float3(0.004, 0.012, 0.022), float3(0.01, 0.035, 0.055), saturate((r - rIn) / (rOut - rIn)));
        float3 col = glass + acc * 0.10 * seg;
        col += acc * hover * seg * (0.55 + 0.25 * g_Light.z);

        // Edge lines on the segment rims and thin concentric rails.
        float rim = 1.0 - smoothstep(0.0, 0.012, abs(r - rIn));
        rim += 1.0 - smoothstep(0.0, 0.012, abs(r - rOut));
        float rail = 1.0 - smoothstep(0.0, 0.006, abs(r - 0.955));
        float rail2 = 1.0 - smoothstep(0.0, 0.005, abs(r - 0.52));
        float inner = (1.0 - smoothstep(0.0, 0.01, abs(r - 0.18)));
        float3 lines = acc * (rim * (0.8 + hover) + rail * 0.5 + rail2 * 0.35 + inner * 0.6);

        // Outer ticks, slowly circling like a disc readout.
        float tk = frac((ang + g_Params.w * 0.15) / TAU * 72.0);
        float tick = (1.0 - smoothstep(0.0, 0.1, abs(tk - 0.5) - 0.2)) * step(0.965, r) * (1.0 - step(0.995, r));
        lines += acc * tick * 0.5;

        // Soft bloom around the hovered segment and the whole ring.
        float bloom = (1.0 - smoothstep(0.0, 0.18, abs(r - lerp(rIn, rOut, 0.5)) - 0.5 * (rOut - rIn))) * (0.12 + 0.35 * hover);
        float3 glow = acc * bloom;

        // Centre: faint disc with a pulsing core.
        float core = 1.0 - smoothstep(0.0, 0.06 + 0.01 * sin(g_Params.w * 3.0), r - 0.05);
        float3 centre = acc * (core * 0.9 + (1.0 - smoothstep(0.0, 0.18, r)) * 0.08);

        float alpha = saturate(max(seg * 0.82, max(max(rim, rail), max(tick, inner))) + bloom + core + rail2 * 0.4);
        // Sweep open from the top.
        float reveal = smoothstep(0.0, 1.0, saturate(open * 1.6 - (1.0 - ang / TAU) * 0.6));
        alpha *= reveal * (r < 1.0 ? 1.0 : 0.0);
        Color = float4(col * seg + lines + glow + centre, alpha);
        return;
    }
    if (g_Params.x > 0.5)
    {
        // Distance outside the inner rectangle, normalised by the available margin.
        float2 p = abs(In.UV - 0.5) * 2.0;
        float2 margin = max(1.0 - g_Light.xy, 1e-4);
        float2 q = max(p - g_Light.xy, 0.0) / margin;
        float d = saturate(length(q));
        float a = pow(1.0 - smoothstep(0.0, 1.0, d), 2.0) * g_Color.a;
        Color = float4(g_Color.rgb, a);
        return;
    }

    float2 uv = g_Light.xy + In.UV * g_Light.zw;
    float4 tex = g_Tex.Sample(g_Tex_sampler, uv);

    float aspect = g_Params.z;
    float2 ext = float2(0.5 * aspect, 0.5);
    float2 pp = (In.UV - 0.5) * float2(aspect, 1.0);
    float r = min(g_Params.y, min(ext.x, ext.y));
    float2 qq = abs(pp) - (ext - r);
    float sd = length(max(qq, 0.0)) + min(max(qq.x, qq.y), 0.0) - r;

    float px = fwidth(sd);
    float cover = 1.0 - smoothstep(-px, px, sd);

    // Rim light: a thin bright band along the edge, stronger on the upper-left like a bevel.
    float rimBand = 1.0 - smoothstep(0.0, 0.02, -sd);
    float bias = saturate(0.55 + 0.45 * dot(normalize(pp + 1e-5), float2(-0.7, -0.7)));
    // A negative rim strength marks a transparent icon: no rim, and the texture's own alpha shapes it.
    float ua = g_Params.w < 0.0 ? tex.a : 1.0;
    float3 rgb = tex.rgb + g_Color.rgb * max(g_Params.w, 0.0) * rimBand * bias;
    Color = float4(rgb, cover * g_Color.a * ua);
}
