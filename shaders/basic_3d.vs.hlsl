cbuffer Constants
{
    float4x4 g_MVP;
    float4x4 g_World;
    float4   g_Color;
    float4   g_Params; // x = time, y = corner radius, z = panel aspect, w = selected (0..1)
    float4   g_Light;  // 3D: xyz = light direction. Panel: x = segment count, y = label aspect
    float4   g_Accent; // theme accent (rgb), w = time
    float4   g_Alt;    // theme alert colour (rgb)
};

struct VSInput
{
    float3 Pos : ATTRIB0;
    float3 Nrm : ATTRIB1;
    float2 UV  : ATTRIB2;
};

struct PSInput
{
    float4 Pos  : SV_POSITION;
    float3 Nrm  : NORMAL;
    float2 UV   : TEXCOORD0;
    float3 WPos : TEXCOORD1;
    float3 LN   : TEXCOORD2;
};

void main(in VSInput In, out PSInput Out)
{
    float4 world = mul(g_World, float4(In.Pos, 1.0));
    Out.Pos  = mul(g_MVP, float4(In.Pos, 1.0));
    Out.Nrm  = mul((float3x3)g_World, In.Nrm);
    Out.UV   = In.UV;
    Out.WPos = world.xyz;
    Out.LN   = In.Nrm;
}
