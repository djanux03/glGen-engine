#ifndef PAINT_MATERIAL_GLSL
#define PAINT_MATERIAL_GLSL

const float PAINT_PI = 3.14159265359;
#include "skyIrradiance.glsl"

float paintHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float paintDistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float nh = max(dot(N, H), 0.0);
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / max(PAINT_PI * d * d, 1e-9);
}

// Height-correlated Smith visibility (Filament's standard GGX model).
// The former separable Schlick fit darkened smooth surfaces at grazing angles.
// This returns G/(4 NoL NoV) directly, avoiding an unstable divide at the rim.
float paintVisibilityGGX(float nv, float nl, float roughness) {
    float a2=pow(roughness,4.0);
    float gv=nl*sqrt(nv*nv*(1.0-a2)+a2);
    float gl=nv*sqrt(nl*nl*(1.0-a2)+a2);
    return 0.5/max(gv+gl,1e-6);
}

// Filter the specular lobe, rather than blurring colour after it sparkles.
// Screen-space normal variance (Filament / geometric specular AA) broadens
// subpixel normal-map bumps; a cap protects silhouettes and hard creases.
float paintNormalVariance(vec3 N) {
    vec3 dx=dFdx(N),dy=dFdy(N);
    return min(.15*(dot(dx,dx)+dot(dy,dy)),.12);
}
float paintFilterRoughness(float roughness,float variance) {
    return pow(clamp(pow(clamp(roughness,.06,1.0),4.0)+variance,0.0,1.0),.25);
}

vec3 paintFresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

// A mip is a box-filtered reflection, not diffuse irradiance. Integrate a
// cosine-weighted hemisphere so a hillside does not inherit a sharp horizon
// stripe from whichever cubemap face its normal happens to point at.
vec3 paintDiffuseEnvironment(samplerCube environment, vec3 N) {
    if(uFrame.skyLutParams.x>.5)return skyDiffuseIrradiance(N);
    vec3 T=normalize(cross(abs(N.y)<.95?vec3(0,1,0):vec3(1,0,0),N));
    vec3 B=cross(N,T);
    vec3 sum=vec3(0);
    for(int i=0;i<8;++i) {
        float r=sqrt((float(i)+.5)/8.0);
        float a=float(i)*2.39996323;
        vec3 d=T*(r*cos(a))+B*(r*sin(a))+N*sqrt(1.0-r*r);
        sum+=textureLod(environment,d,uFrame.iblParams.x).rgb;
    }
    // Cosine importance sampling returns E/pi, ready for Lambert albedo.
    return sum*.125;
}

// Analytic fit of the split-sum DFG term (Karis 2014, "Physically Based
// Shading on Mobile"): x scales F0, y is the Fresnel bias. It replaces a
// 2D LUT and is within a few percent of it over the whole (NdotV, roughness)
// square -- far closer than the hand-tuned `mix(0.92, 0.55, roughness)`
// energy factor this file used before, which over-reflected rough grazing
// surfaces and under-reflected smooth ones.
vec2 paintEnvBRDF(float nv, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Single-scattering GGX loses energy at high roughness (light that would
// bounce between microfacets is dropped), so rough metals come out too dark.
// Fdez-Aguera 2019: scale by 1 + F0 * (1/(A+B) - 1).
vec3 paintMultiScatter(vec2 dfg, vec3 f0) {
    return 1.0 + f0 * (1.0 / max(dfg.x + dfg.y, 1e-3) - 1.0);
}

// Specular occlusion from ambient occlusion (Lagarde & de Rousiers 2014).
// AO darkening only the diffuse term left crevices and the undersides of
// canopies reflecting full open sky, which is what makes SSAO'd geometry
// look "lit from inside". Rougher lobes are occluded like diffuse; sharp
// lobes only where the view is grazing.
float paintSpecularOcclusion(float nv, float ao, float roughness) {
    return clamp(pow(nv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao,
                 0.0, 1.0);
}

// Multi-bounce AO (Jimenez et al. 2016, GTAO): light that enters a crevice
// bounces off its walls before escaping, so bright surfaces are occluded
// less than raw AO says and dark ones more. Without it, AO greys out snow
// and white plaster and barely touches dark soil -- the opposite of reality.
vec3 paintMultiBounceAO(float ao, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}

vec3 physicalEnvironmentSpecular(vec3 albedo, vec3 N, vec3 V,
                                 float roughness, float metallic, float ao,
                                 vec3 environment) {
    roughness = clamp(roughness, 0.06, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float nv = max(dot(N, V), 1e-3);
    vec2 dfg = paintEnvBRDF(nv, roughness);
    vec3 spec = (f0 * dfg.x + dfg.y) * paintMultiScatter(dfg, f0);
    // realismParams.z blends from the old flat AO response to physically
    // derived specular occlusion.
    float occlusion = mix(mix(0.55, 1.0, ao),
                          paintSpecularOcclusion(nv, ao, roughness),
                          uFrame.realismParams.z);
    return max(environment, vec3(0.0)) * spec * occlusion;
}

// Reflection vectors from a normal-mapped surface can point BELOW the
// geometric surface, where they sample the ground half of the cubemap
// (or, worse, the sky through the mesh). Fade them out as they cross the
// horizon of the interpolated vertex normal (Frostbite / Jimenez).
float paintHorizonOcclusion(vec3 R, vec3 geometricN) {
    float h = clamp(1.0 + 1.1 * dot(R, geometricN), 0.0, 1.0);
    return mix(1.0, h * h, uFrame.realismParams.z);
}

// Reconstruct a tangent frame from position/UV derivatives. MeshData does not
// carry tangents, and expanding every vertex for a map used by only some
// materials is wasteful. This cotangent frame also handles mirrored UV islands
// locally, avoiding the usual imported-model seam from a guessed world basis.
vec3 paintPerturbNormal(vec3 N,vec3 worldPos,vec2 uv,vec3 tangentNormal){
    vec3 dp1=dFdx(worldPos),dp2=dFdy(worldPos);
    vec2 duv1=dFdx(uv),duv2=dFdy(uv);
    vec3 dp2Perp=cross(dp2,N),dp1Perp=cross(N,dp1);
    vec3 T=dp2Perp*duv1.x+dp1Perp*duv2.x;
    vec3 B=dp2Perp*duv1.y+dp1Perp*duv2.y;
    float invMax=inversesqrt(max(max(dot(T,T),dot(B,B)),1e-8));
    mat3 tbn=mat3(T*invMax,B*invMax,N);
    return normalize(tbn*normalize(tangentNormal));
}

// Shared physical response. Painterly character belongs to albedo, roughness,
// normals and post-processing; direct light remains Cook-Torrance GGX.
vec3 physicalPaintSurface(vec3 albedo, vec3 N, vec3 V, vec3 L,
                          float roughness, float metallic, float visibility,
                          float ao, vec3 irradiance, float directMultiplier,
                          float ambientMultiplier) {
    roughness = clamp(roughness, 0.06, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    vec3 H = (V + L) / max(length(V + L), 1e-5);
    float rawNl = dot(N, L);
    float nlPbr = max(rawNl, 0.0);
    float rampDial = clamp(uFrame.tldStyleParams.x, 0.0, 1.0);
    float nl;
    if (rampDial > 0.001) {
        float wrapNl = rawNl * 0.5 + 0.5;
        float step1 = smoothstep(0.32, 0.44, wrapNl);
        float step2 = smoothstep(0.64, 0.76, wrapNl);
        float nlPainterly = step1 * 0.45 + step2 * 0.55;
        nl = mix(nlPbr, nlPainterly, rampDial);
    } else {
        nl = nlPbr;
    }
    float nv = max(dot(N, V), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = paintFresnelSchlick(max(dot(H, V), 0.0), f0);
    float D = paintDistributionGGX(N, H, roughness);
    vec3 specular = D * paintVisibilityGGX(nv,nlPbr,roughness) * F;
    specular *= paintMultiScatter(paintEnvBRDF(max(nv, 1e-3), roughness), f0);
    vec3 diffuse = (1.0 - F) * (1.0 - metallic) * albedo / PAINT_PI;
    vec3 direct = (diffuse + specular) * uFrame.sunRadiance.rgb * nl *
                  visibility * directMultiplier;
    // lightParams.x is the public ambient-intensity control. It was uploaded
    // every frame but never consumed, so changing it in the editor or scripts
    // had no visible effect and night scenes could not be balanced honestly.
    // Conductors have no diffuse lobe. Previously polished metal received
    // the full albedo diffuse term, so it looked like bright painted plastic.
    vec3 aoTerm = mix(vec3(ao), paintMultiBounceAO(ao, albedo),
                      uFrame.realismParams.z);
    vec3 ambient = irradiance * albedo * (1.0-metallic) * (1.0-f0) *
                   aoTerm * ambientMultiplier *
                   max(uFrame.lightParams.x, 0.0);
    float coolBias = clamp(uFrame.tldStyleParams.y, 0.0, 1.0);
    if (coolBias > 0.001) {
        float shadowFactor = 1.0 - clamp(nl * visibility, 0.0, 1.0);
        vec3 coldShadowTint = vec3(0.70, 0.82, 1.15);
        ambient *= mix(vec3(1.0), coldShadowTint, coolBias * shadowFactor);
    }
    return max(direct + ambient, vec3(0.0));
}

// Practical point lights authored through render.params. Intensity follows an
// inverse-square falloff with a smooth finite-radius window; this avoids the
// hard spherical boundary and unbounded near-field fireflies of 1/d^2 alone.
vec3 physicalPointLights(vec3 albedo,vec3 N,vec3 V,vec3 worldPos,
                         float roughness,float metallic){
    vec3 result=vec3(0.0);
    roughness=clamp(roughness,.06,1.0);
    metallic=clamp(metallic,0.0,1.0);
    int count=clamp(int(uFrame.pointLightParams.x+0.5),0,4);
    for(int i=0;i<count;++i){
        vec3 toLight=uFrame.pointLightPositionRadius[i].xyz-worldPos;
        float distanceToLight=max(length(toLight),1e-4);
        float distanceSquared=max(distanceToLight*distanceToLight,0.16);
        float radius=max(uFrame.pointLightPositionRadius[i].w,0.1);
        float x=distanceToLight/radius;
        float window=pow(clamp(1.0-pow(x,4.0),0.0,1.0),2.0);
        if(window<=0.0)continue;
        vec3 L=toLight/distanceToLight;
        vec3 H=(V+L)/max(length(V+L),1e-5);
        float nl=max(dot(N,L),0.0),nv=max(dot(N,V),0.0);
        vec3 f0=mix(vec3(0.04),albedo,metallic);
        vec3 F=paintFresnelSchlick(max(dot(H,V),0.0),f0);
        float D=paintDistributionGGX(N,H,roughness);
        if(nl<=0.0)continue;
        vec3 specular=D*paintVisibilityGGX(nv,nl,roughness)*F;
        specular*=paintMultiScatter(paintEnvBRDF(max(nv,1e-3),roughness),f0);
        vec3 diffuse=(1.0-F)*(1.0-metallic)*albedo/PAINT_PI;
        vec4 source=uFrame.pointLightColorIntensity[i];
        float visibility=1.0;
        if(uFrame.lightParams.y>.001)
            visibility=mix(1.0,traceShadowRange(worldPos+N*.015,L,
                max(distanceToLight-.03,.003)),uFrame.lightParams.y);
        result+=(diffuse+specular)*source.rgb*(source.w*window/distanceSquared)*nl*visibility;
    }
    return max(result,vec3(0.0));
}

vec3 foliageTransmission(vec3 albedo, vec3 N, vec3 V, vec3 L,
                         float visibility, float strength) {
    float back = pow(clamp(dot(-N, L), 0.0, 1.0), 1.7);
    float viewWrap = 0.35 + 0.65 * pow(1.0 - abs(dot(N, V)), 2.0);
    return albedo * uFrame.sunRadiance.rgb * back * viewWrap *
           visibility * max(strength, 0.0) * 0.22;
}

#endif
