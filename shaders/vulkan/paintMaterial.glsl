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
    vec3 ambient = irradiance * albedo * ao * ambientMultiplier;
    return max(direct + ambient, vec3(0.0));
}

vec3 foliageTransmission(vec3 albedo, vec3 N, vec3 V, vec3 L,
                         float visibility, float strength) {
    float back = pow(clamp(dot(-N, L), 0.0, 1.0), 1.7);
    float viewWrap = 0.35 + 0.65 * pow(1.0 - abs(dot(N, V)), 2.0);
    return albedo * uFrame.sunRadiance.rgb * back * viewWrap *
           mix(0.35, 1.0, visibility) * max(strength, 0.0) * 0.22;
}

#endif
