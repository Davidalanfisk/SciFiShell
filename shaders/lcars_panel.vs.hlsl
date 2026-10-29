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

struct VSInput
{
    float3 Pos : ATTRIB0;
    float3 Nrm : ATTRIB1;
    float2 UV  : ATTRIB2;
};

struct PSInput
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEXCOORD0;
};

void main(in VSInput In, out PSInput Out)
{
    Out.Pos = mul(g_MVP, float4(In.Pos, 1.0));
    Out.UV  = In.UV;
}
