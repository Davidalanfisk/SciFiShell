cbuffer Constants
{
    float4x4 g_MVP;
    float4x4 g_World;
    float4   g_Color;   // xy = 1 / target size, z = time
    float4   g_Params;
    float4   g_Light;
    float4   g_Accent;  // theme accent (rgb)
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
    float3 LN   : TEXCOORD2;
};

float3 fetch(float2 uv)
{
    return g_Tex.SampleLevel(g_Tex_sampler, uv, 0).rgb;
}

// Only the genuinely bright parts of the frame (neon edges, lit windows) feed the glow.
float3 bright(float3 c)
{
    float l = max(c.r, max(c.g, c.b));
    return c * saturate((l - 0.28) * 2.2);
}

float hash(float2 p)
{
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

void main(in PSInput In, out float4 Color : SV_TARGET)
{
    float2 px = g_Color.xy;
    float2 uv = In.Pos.xy * px;
    if (g_Params.x > 0.5) { Color = float4(fetch(uv), 1.0); return; }
    float2 c = uv - 0.5;
    float r2 = dot(c, c);

    // Chromatic fringe that grows towards the screen edges.
    float2 off = c * (0.0006 + 0.005 * r2);
    float3 col;
    col.r = fetch(uv + off).r;
    col.g = fetch(uv).g;
    col.b = fetch(uv - off).b;

    // Bloom: a golden-angle spiral of taps, brightness-thresholded, wider than a pixel blur so neon lines bleed light.
    float aspect = px.y / px.x;
    float3 glow = float3(0, 0, 0);
    const int taps = 24;
    for (int i = 0; i < taps; ++i)
    {
        float a = (float)i * 2.39996;
        float rad = sqrt((i + 0.5) / taps);
        float2 d = float2(cos(a), sin(a)) * rad * float2(1.0, aspect) * 0.0075;
        glow += bright(fetch(uv + d));
    }
    glow /= taps;

    // Anamorphic streak: a thin horizontal smear of the brightest highlights, the classic sci-fi lens flare.
    float3 streak = float3(0, 0, 0);
    [unroll]
    for (int k = -8; k <= 8; ++k)
    {
        float w = exp(-0.05 * k * k);
        streak += bright(fetch(uv + float2(k * 0.0018, 0))) * w;
    }
    streak /= 8.0;

    col += glow * 0.5 + streak * 0.08 * lerp(float3(1, 1, 1), g_Accent.rgb, 0.6);

    // Cool teal grade in the shadows, a touch more punch overall.
    float l = dot(col, float3(0.299, 0.587, 0.114));
    col += float3(0.0, 0.004, 0.012) * (1.0 - saturate(l * 3.0));
    col = lerp(float3(l, l, l), col, 1.12);

    // Moody grade: deepen the mid-tones and lower exposure so the city stays dark with the neon standing out.
    col = pow(max(col, 0.0), 1.35) * 0.9;

    // Vignette.
    col *= 1.0 - saturate(r2 * 1.5);

    // Dither the dark gradients so fog and haze do not band, plus a whisper of film grain.
    float n = hash(In.Pos.xy + frac(g_Color.z) * 61.0) - 0.5;
    col += n * 0.0015;

    Color = float4(max(col, 0.0), 1.0);
}
