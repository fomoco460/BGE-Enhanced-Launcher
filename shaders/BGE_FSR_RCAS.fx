// AMD FidelityFX Super Resolution 1.0 - RCAS (Robust Contrast Adaptive Sharpening)
// Port for ReShade, written for the BG&E launcher. Algorithm (c) AMD, MIT licence.
#include "ReShade.fxh"

uniform float Sharpness <
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.05;
    ui_label = "Sharpness (stops, 0 = max)";
    ui_tooltip = "FSR RCAS sharpness in stops. 0.0 is strongest, 2.0 is subtle.";
> = 0.2;

uniform bool Denoise < ui_label = "Denoise (reduce grain amplification)"; > = false;

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

float3 Fetch(int2 p) { return tex2Dfetch(ReShade::BackBuffer, p).rgb; }

float3 PS_RCAS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    int2 sp = int2(pos.xy);
    float3 b = Fetch(sp + int2( 0,-1));
    float3 d = Fetch(sp + int2(-1, 0));
    float3 e = Fetch(sp);
    float3 f = Fetch(sp + int2( 1, 0));
    float3 h = Fetch(sp + int2( 0, 1));

    // luma times 2
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    nz = saturate(abs(nz) * rcp(max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL) + 1e-5));
    nz = -0.5 * nz + 1.0;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float2 peakC = float2(1.0, -4.0);
    float3 hitMin = mn4 * rcp(4.0 * mx4 + 1e-5);
    float3 hitMax = (peakC.x - mx4) * rcp(4.0 * mn4 + peakC.y);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * exp2(-Sharpness);
    if (Denoise) lobe *= nz;

    float rcpL = rcp(4.0 * lobe + 1.0);
    return saturate((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL);
}

technique FSR_RCAS < ui_label = "AMD FSR 1 RCAS Sharpening"; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_RCAS; }
}
