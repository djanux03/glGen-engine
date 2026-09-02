#ifndef PAINT_MATERIAL_GLSL
#define PAINT_MATERIAL_GLSL

const float PAINT_PI = 3.14159265359;

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
    return a2 / max(PAINT_PI * d * d, 1e-5);
}

float paintGeometrySchlickGGX(float nv, float roughness) {
    float r = roughness + 1.0;
    float k = r * r * 0.125;
    return nv / max(nv * (1.0 - k) + k, 1e-5);
}

float paintGeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return paintGeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
           paintGeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 paintFresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

// Split-sum IBL is overkill for the small, dynamically rendered sky cubemap,
// but omitting environment specular entirely made every material read as dry
// clay. This energy-conserving approximation restores broad sky reflections
// and lets roughness/metalness remain legible without a BRDF LUT.
vec3 physicalEnvironmentSpecular(vec3 albedo, vec3 N, vec3 V,
                                 float roughness, float metallic, float ao,
                                 vec3 environment) {
    roughness = clamp(roughness, 0.06, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = paintFresnelSchlick(max(dot(N, V), 0.0), f0);
    // Rough surfaces still reflect the low-frequency sky, just with less
    // peak energy. AO affects only the broad indirect lobe, never direct sun.
    float energy = mix(0.92, 0.32, roughness);
    return max(environment, vec3(0.0)) * F * energy * mix(0.55, 1.0, ao);
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
    vec3 H = normalize(V + L);
    float nl = max(dot(N, L), 0.0);
    float nv = max(dot(N, V), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = paintFresnelSchlick(max(dot(H, V), 0.0), f0);
    float D = paintDistributionGGX(N, H, roughness);
    float G = paintGeometrySmith(N, V, L, roughness);
    vec3 specular = D * G * F / max(4.0 * nl * nv, 1e-4);
    vec3 diffuse = (1.0 - F) * (1.0 - metallic) * albedo / PAINT_PI;
    vec3 direct = (diffuse + specular) * uFrame.sunRadiance.rgb * nl *
                  visibility * directMultiplier;
    // lightParams.x is the public ambient-intensity control. It was uploaded
    // every frame but never consumed, so changing it in the editor or scripts
    // had no visible effect and night scenes could not be balanced honestly.
    vec3 ambient = irradiance * albedo * ao * ambientMultiplier *
                   max(uFrame.lightParams.x, 0.0);
    return max(direct + ambient, vec3(0.0));
}

// Practical point lights authored through render.params. Intensity follows an
// inverse-square falloff with a smooth finite-radius window; this avoids the
// hard spherical boundary and unbounded near-field fireflies of 1/d^2 alone.
vec3 physicalPointLights(vec3 albedo,vec3 N,vec3 V,vec3 worldPos,
                         float roughness,float metallic){
    vec3 result=vec3(0.0);
    int count=clamp(int(uFrame.pointLightParams.x+0.5),0,4);
    for(int i=0;i<count;++i){
        vec3 toLight=uFrame.pointLightPositionRadius[i].xyz-worldPos;
        float distanceSquared=max(dot(toLight,toLight),0.16);
        float distanceToLight=sqrt(distanceSquared);
        float radius=max(uFrame.pointLightPositionRadius[i].w,0.1);
        float x=distanceToLight/radius;
        float window=pow(clamp(1.0-pow(x,4.0),0.0,1.0),2.0);
        if(window<=0.0)continue;
        vec3 L=toLight/distanceToLight;
        vec3 H=normalize(V+L);
        float nl=max(dot(N,L),0.0),nv=max(dot(N,V),0.0);
        vec3 f0=mix(vec3(0.04),albedo,metallic);
        vec3 F=paintFresnelSchlick(max(dot(H,V),0.0),f0);
        float D=paintDistributionGGX(N,H,roughness);
        float G=paintGeometrySmith(N,V,L,roughness);
        vec3 specular=D*G*F/max(4.0*nl*nv,1e-4);
        vec3 diffuse=(1.0-F)*(1.0-metallic)*albedo/PAINT_PI;
        vec4 source=uFrame.pointLightColorIntensity[i];
        result+=(diffuse+specular)*source.rgb*(source.w*window/distanceSquared)*nl;
    }
    return max(result,vec3(0.0));
}

vec3 foliageTransmission(vec3 albedo, vec3 N, vec3 V, vec3 L,
                         float visibility, float strength) {
    float back = pow(clamp(dot(-N, L), 0.0, 1.0), 1.7);
    float viewWrap = 0.35 + 0.65 * pow(1.0 - abs(dot(N, V)), 2.0);
    return albedo * uFrame.sunRadiance.rgb * back * viewWrap *
           mix(0.35, 1.0, visibility) * max(strength, 0.0) * 0.22;
}

#endif
