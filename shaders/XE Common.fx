
// XE Common.fx
// MGE XE 0.9
// Shared structures and functions



//------------------------------------------------------------
// Uniform variables

shared float2 rcpres;
shared matrix view, proj, world;
shared matrix vertexblendpalette[4];
shared matrix shadowviewproj[2];
shared bool hasalpha, hasbones;
shared float alpharef;
shared int vertexblendstate;

shared float3 EyePos, FootPos;
shared float3 SunVec, SunCol, SunAmb;
shared float3 SkyCol, FogCol1, FogCol2;
shared float FogStart, FogRange;
shared float nearFogStart, nearFogRange;
shared float dissolveRange;
shared float3 SunPos;
shared float SunVis;
shared float2 WindVec;
shared float niceWeather;
shared float time;


//------------------------------------------------------------
// Textures

shared texture tex0, tex1, tex2, tex3;

sampler sampBaseTex = sampler_state { texture = <tex0>; minfilter = anisotropic; magfilter = linear; mipfilter = linear; addressu = wrap; addressv = wrap; };
sampler sampNormals = sampler_state { texture = <tex1>; minfilter = linear; magfilter = linear; mipfilter = linear; addressu = wrap; addressv = wrap; };
sampler sampDetail = sampler_state { texture = <tex2>; minfilter = linear; magfilter = linear; mipfilter = linear; addressu = wrap; addressv = wrap; };
sampler sampWater3d = sampler_state { texture = <tex1>; minfilter = linear; magfilter = linear; mipfilter = none; addressu = wrap; addressv = wrap; addressw = wrap; };
sampler sampDepth = sampler_state { texture = <tex3>; minfilter = linear; magfilter = linear; mipfilter = none; addressu = clamp; addressv = clamp; };


//------------------------------------------------------------
// Distant land statics / grass
// diffuse in color, emissive in normal.w

struct StatVertIn
{
    float4 pos : POSITION;
    float4 normal : NORMAL;
    float4 color : COLOR0;
    float2 texcoords : TEXCOORD0;
};

struct StatVertInstIn
{
    float4 pos : POSITION;
    float4 normal : NORMAL;
    float4 color : COLOR0;
    float2 texcoords : TEXCOORD0;
    float4 world0 : TEXCOORD1;
    float4 world1 : TEXCOORD2;
    float4 world2 : TEXCOORD3;
};

struct StatVertOut
{
    float4 pos : POSITION;
    float4 color : COLOR0;
    float3 texcoords_range : TEXCOORD0;
    float4 fog : TEXCOORD1;
};

//------------------------------------------------------------
// Full-screen deferred pass, used for reconstructing position from depth

struct DeferredOut
{
    float4 pos : POSITION;
    float4 tex : TEXCOORD0;
    float3 eye : TEXCOORD1;
};

//------------------------------------------------------------
// Morrowind FVF (sans vertex colour)

struct MorrowindVertIn
{
    float4 pos : POSITION;
    float4 normal : NORMAL;
    float4 blendweights : BLENDWEIGHT;
    float2 texcoords : TEXCOORD0;
};

//------------------------------------------------------------
// Fogging functions, horizon to sky colour approximation

#ifdef USE_EXPFOG

float fogScalar(float dist)
{
    float x = (dist - FogStart) / (FogRange - FogStart);
    return saturate(exp(-x));
}

float fogMWScalar(float dist)
{
    return saturate((nearFogRange - dist) / (nearFogRange - nearFogStart));
}

#else

float fogScalar(float dist)
{
    return saturate((nearFogRange - dist) / (nearFogRange - nearFogStart));
}

float fogMWScalar(float dist)
{
    return fogScalar(dist);
}
  
#endif

#ifdef USE_SCATTERING
    static const float3 scatter = {0.07, 0.36, 0.76};
    static const float3 scatter2 = {0.16, 0.37, 0.62};
    static const float3 newskycol = 0.38 * SkyCol + float3(0.23, 0.39, 0.68);
    static const float sunaltitude = pow(1 + SunPos.z, 10);
    static const float sunaltitude_a = 2.8 + 4.3 / sunaltitude;
    static const float sunaltitude_b = saturate(1 - exp2(-1.9 * sunaltitude));
    static const float sunaltitude2 = saturate(exp(-2 * SunPos.z)) * saturate(sunaltitude);

    float4 fogColour(float3 dir, float dist)
    {
        float fogdist;
        float fog;
        float3 fFogCol2 = lerp(FogCol2, SkyCol, 1 - pow(saturate(1 - 2.22 * saturate(dir.z - 0.075)), 1.15));
        
        if(dist < 0)
        {
            fogdist = 1;
            fog = 0;
        }
        else
        {
            fogdist = (dist - FogStart) / (FogRange - FogStart);
            fog = saturate(exp(-fogdist));
            fFogCol2 *= 1 - fog;
            fogdist = saturate(0.21 * fogdist);
        }
        
        if(niceWeather > 0.001 && EyePos.z > /*WaterLevel*/-1)
        {
            float suncos = dot(dir, SunPos);
            float mie = (1.62 / (1.3 - suncos)) * sunaltitude2;
            float rayl = 1 - 0.09 * mie;
            
            float atmdep = 1.33 * exp(-2 * saturate(dir.z));
            float3 att = atmdep * lerp(scatter2, scatter, suncos) * (sunaltitude_a + mie);
            att = (1 - exp(-fogdist * att)) / att;
            
            float3 colour = 0.125 * mie + newskycol * rayl;
            colour *= att * (1.1*atmdep + 0.5) * sunaltitude_b;
            colour = lerp(fFogCol2, colour, niceWeather);

            return float4(colour, fog);
        }
        else
        {
            return float4(fFogCol2, fog);
        }
    }
    
    float4 fogColourSky(float3 dir)
    {
        return fogColour(dir, -1);
    }
#else
    float4 fogColour(float3 dir, float dist)
    {
        float3 c = lerp(FogCol2, SkyCol, 1 - pow(saturate(1 - 2.22 * saturate(dir.z - 0.075)), 1.15));
        float f = fogScalar(dist);
        return float4((1 - f) * c, f);
    }
    
    float4 fogColourSky(float3 dir)
    {
        float3 c = lerp(FogCol2, SkyCol, 1 - pow(saturate(1 - 2.22 * saturate(dir.z - 0.075)), 1.15));
        return float4(c, 0);
    }
#endif

float4 fogMWColour(float dist)
{
    float f = fogMWScalar(dist);
    return float4((1 - f) * FogCol1, f);
}

float3 fogApply(float3 c, float4 f)
{
    return f.a * c + f.rgb;
}

//------------------------------------------------------------
// Distant land height bias to prevent low lod meshes from clipping

float landBias(float dist)
{
    return -(16 + 60000 * saturate(1 - dist/6000));
}

//------------------------------------------------------------
// Is point above water function, for exteriors only

bool isAboveSeaLevel(float3 pos)
{
    return (pos.z > -1);
}

//------------------------------------------------------------
// Grass, wind displacement function

float2 grassDisplacement(float4 worldpos, float h)
{
    float v = length(WindVec);
    float2 displace = 2 * WindVec + 0.1;
    float2 harmonics = 0;
    
    harmonics += (1 - 0.10*v) * sin(1.0*time + worldpos.xy / 1100);
    harmonics += (1 - 0.04*v) * cos(2.0*time + worldpos.xy / 750);
    harmonics += (1 + 0.14*v) * sin(3.0*time + worldpos.xy / 500);
    harmonics += (1 + 0.28*v) * sin(5.0*time + worldpos.xy / 200);

    float d = length(worldpos.xy - FootPos.xy);
    float2 stomp = 0;
    
    if(d < 150)
        stomp = (60 / d - 0.4) * (worldpos.xy - FootPos.xy);

    return saturate(0.02 * h) * (harmonics * displace + stomp);
}

//------------------------------------------------------------
// Instancing matrix decompression and multiply

float4 instancedMul(float4 pos, float4 m0, float4 m1, float4 m2)
{
    float4 v;
    v.x = dot(pos, m0);
    v.y = dot(pos, m1);
    v.z = dot(pos, m2);
    v.w = pos.w;

    return v;
}

//------------------------------------------------------------
// Skinning, fixed-function

float4 skin(float4 pos, float4 blend)
{
    if(vertexblendstate == 1)
        blend[1] = 1 - blend[0];
    else if(vertexblendstate == 2)
        blend[2] = 1 - (blend[0] + blend[1]);
    else if(vertexblendstate == 3)
        blend[3] = 1 - (blend[0] + blend[1] + blend[2]);

    float4 worldpos = mul(pos, vertexblendpalette[0]) * blend[0];

    if(vertexblendstate >= 1)
        worldpos += mul(pos, vertexblendpalette[1]) * blend[1];
    if(vertexblendstate >= 2)
        worldpos += mul(pos, vertexblendpalette[2]) * blend[2];
    if(vertexblendstate >= 3)
        worldpos += mul(pos, vertexblendpalette[3]) * blend[3];
        
    return worldpos;
}

//------------------------------------------------------------
