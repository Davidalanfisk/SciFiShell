cbuffer Constants
{
    float4x4 g_MVP;
    float4x4 g_World;
    float4   g_Color;  // accent colour; a = opacity
    float4   g_Params; // x = time, y = corner radius, z = panel aspect, w = selected (0..1)
    float4   g_Light;  // x = unused, y = label aspect
    float4   g_Accent;
    float4   g_Alt;
};

Texture2D g_Tex;
SamplerState g_Tex_sampler;

struct PSInput
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEXCOORD0;
};

// Modern "glass" panel: pill shape, translucent dark gradient, hairline border, accent dot.
void main(in PSInput In, out float4 Color : SV_TARGET)
{
    float aspect = g_Params.z;
    float sel    = g_Params.w;
    if (sel < -0.5)
    {
        // Text only (HUD): the label fills the quad, white, with a soft dark drop shadow so it reads on any background.
        float a = g_Tex.Sample(g_Tex_sampler, In.UV).a;
        float sa = g_Tex.Sample(g_Tex_sampler, In.UV - float2(0.004, -0.04)).a * 0.7;
        float oa = a + sa * (1.0 - a);
        if (oa <= 0.003) discard;
        Color = float4(float3(1, 1, 1) * (a / oa), oa * g_Color.a);
        return;
    }
    float2 halfSize = float2(aspect, 1.0) * 0.5;
    float2 p = (In.UV - 0.5) * float2(aspect, 1.0);

    float r = 0.5; // full pill
    float2 q = abs(p) - halfSize + r;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float aa = 0.012;
    float mask = 1.0 - smoothstep(-aa, aa, d);
    if (mask <= 0.001) discard;

    float3 accent = g_Color.rgb;
    float3 top = float3(0.022, 0.03, 0.05);
    float3 bot = float3(0.008, 0.012, 0.022);
    float3 col = lerp(top, bot, In.UV.y);
    col = lerp(col, accent * 0.45, sel * 0.55);

    // Hairline border, brighter when selected.
    float border = smoothstep(-0.035, -0.012, d);
    col = lerp(col, lerp(float3(0.55, 0.6, 0.72), accent, sel), border * (0.28 + 0.6 * sel));

    // Soft sheen along the top edge.
    col += float3(1, 1, 1) * 0.05 * smoothstep(0.5, 0.0, In.UV.y);

    // Accent dot.
    float2 dc = p - float2(-halfSize.x + 0.5, 0.0);
    float dotMask = 1.0 - smoothstep(0.075, 0.095, length(dc));
    float pulse = 0.85 + 0.15 * sin(g_Params.x * 3.0);
    col = lerp(col, accent * (1.0 + 0.4 * sel * pulse), dotMask);

    // Label, left aligned after the dot, aspect preserved.
    float startX = 0.9;
    float boxW = aspect - startX - 0.45;
    float boxH = 0.56;
    float lw = min(boxW, boxH * g_Light.y);
    float lh = lw / max(g_Light.y, 0.001);
    float2 tuv;
    tuv.x = (p.x + halfSize.x - startX) / lw;
    tuv.y = (In.UV.y - 0.5) / lh + 0.5;
    if (tuv.x >= 0.0 && tuv.x <= 1.0 && tuv.y >= 0.0 && tuv.y <= 1.0)
    {
        float a = g_Tex.Sample(g_Tex_sampler, tuv).a;
        col = lerp(col, lerp(float3(0.88, 0.91, 0.97), float3(1, 1, 1), sel), a);
    }

    Color = float4(col, mask * 0.92 * g_Color.a);
}
