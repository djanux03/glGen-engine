// ssao.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D depthTex;
uniform sampler2D noiseTex;
uniform mat4 uProjection;
uniform mat4 uInvProjection;
uniform vec3 samples[32];
uniform int sampleCount;
uniform float radius;
uniform float bias;
uniform float power;
uniform vec2 uSsaoResolution;

vec3 reconstructViewPos(vec2 uv, float depth) {
    float z = depth * 2.0 - 1.0;
    vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 view = uInvProjection * clip;
    return view.xyz / max(view.w, 0.0001);
}

void main() {
    float depth = texture(depthTex, TexCoords).r;
    if (depth >= 0.999999) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 fragPos = reconstructViewPos(TexCoords, depth);
    vec3 normal = normalize(cross(dFdx(fragPos), dFdy(fragPos)));

    vec2 noiseScale = uSsaoResolution / 4.0;
    vec3 randVec = normalize(texture(noiseTex, TexCoords * noiseScale).xyz * 2.0 - 1.0);
    vec3 tangent = normalize(randVec - normal * dot(randVec, normal));
    vec3 bitangent = normalize(cross(normal, tangent));
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    int count = clamp(sampleCount, 1, 32);
    for (int i = 0; i < count; ++i) {
        vec3 samplePos = fragPos + TBN * samples[i] * radius;
        vec4 offset = uProjection * vec4(samplePos, 1.0);
        offset.xyz /= max(offset.w, 0.0001);
        vec2 sampleUV = offset.xy * 0.5 + 0.5;
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            continue;
        }

        float sampleDepthRaw = texture(depthTex, sampleUV).r;
        if (sampleDepthRaw >= 0.999999) {
            continue;
        }
        vec3 sampleDepthPos = reconstructViewPos(sampleUV, sampleDepthRaw);

        float fragDepth = -fragPos.z;
        float sampleDepth = -sampleDepthPos.z;
        float expectedDepth = -samplePos.z;

        float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(fragDepth - sampleDepth), 0.0001));
        float contributes = sampleDepth <= expectedDepth - bias ? 1.0 : 0.0;
        occlusion += contributes * rangeCheck;
    }

    float ao = 1.0 - (occlusion / float(count));
    ao = pow(clamp(ao, 0.0, 1.0), max(power, 0.01));
    FragColor = vec4(vec3(ao), 1.0);
}
