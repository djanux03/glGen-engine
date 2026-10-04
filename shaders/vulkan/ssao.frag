#version 450

// Screen-space ambient occlusion (Crysis/LearnOpenGL-style hemisphere
// kernel). Samples a depth-only Z-prepass (see VulkanRenderer's
// createDepthPrepassPipelines) -- reconstructs view-space position from
// depth, and the surface normal from screen-space derivatives of that
// position (no separate normal G-buffer). Outputs grayscale occlusion;
// mesh.frag/terrain.frag sample the blurred result and multiply it into
// their ambient term only (never direct light).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepth;

layout(push_constant) uniform Push {
    mat4 invProj; // NDC+depth -> view-space position
    mat4 proj;    // view-space -> NDC, to re-project kernel samples
    float radius;
    float bias;
    float strength;
    float pad;
} pc;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 viewPosFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.invProj * clip;
    return view.xyz / view.w;
}

float depthAt(vec2 uv) {
    // Linear depth filtering invents surfaces between a narrow blade and
    // the soil behind it. Contacts must use an actual depth-prepass sample.
    ivec2 size=textureSize(uDepth,0);
    return texelFetch(uDepth,clamp(ivec2(uv*vec2(size)),ivec2(0),size-1),0).r;
}

void main() {
    float depth = depthAt(vUV);
    if (depth >= 1.0) {
        outColor = vec4(1.0);
        return;
    }

    vec3 P = viewPosFromDepth(vUV, depth);
    // Reconstruct from the closer neighbor on each axis. Derivatives across
    // silhouettes used foreground/background triangles and made dark halos.
    vec2 px=1.0/vec2(textureSize(uDepth,0));
    vec3 left=viewPosFromDepth(vUV-vec2(px.x,0),depthAt(vUV-vec2(px.x,0)));
    vec3 right=viewPosFromDepth(vUV+vec2(px.x,0),depthAt(vUV+vec2(px.x,0)));
    vec3 down=viewPosFromDepth(vUV-vec2(0,px.y),depthAt(vUV-vec2(0,px.y)));
    vec3 up=viewPosFromDepth(vUV+vec2(0,px.y),depthAt(vUV+vec2(0,px.y)));
    vec3 dx=abs(left.z-P.z)<abs(right.z-P.z)?P-left:right-P;
    vec3 dy=abs(down.z-P.z)<abs(up.z-P.z)?P-down:up-P;
    vec3 N=normalize(cross(dx,dy));
    if(N.z<0.0)N=-N;
    // A fixed stratified kernel replaces world-position hashes, which changed
    // all directions when the camera moved a fraction of a pixel.
    float rotation=hash13(vec3(mod(gl_FragCoord.xy,4.0),0))*6.2831853;
    vec3 tangent=normalize(cross(abs(N.z)<.95?vec3(0,0,1):vec3(0,1,0),N));
    vec3 bitangent=cross(N,tangent);
    mat3 TBN=mat3(tangent,bitangent,N);

    // Half the kernel resolves centimetre-scale sticks/roots; the other half
    // shades wider clump pockets. A single 65cm kernel and 2.5cm bias missed
    // almost all low ground debris, even when its strength was increased.
    const int kSamples = 24;
    float occlusion = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float fi = float(i % 12) / 12.0;
        float radial=sqrt((float(i % 12)+.5)/12.0);
        float angle=float(i)*2.39996323+rotation;
        vec3 h=vec3(radial*cos(angle),radial*sin(angle),sqrt(1.0-radial*radial));
        vec3 sampleDir=TBN*h;
        // Concentrate samples closer to the origin (denser near the surface).
        float scale = mix(0.1, 1.0, fi * fi);
        float radius = i < 12 ? min(pc.radius, .16) : pc.radius;
        vec3 samplePos = P + sampleDir * radius * scale;

        vec4 offset = pc.proj * vec4(samplePos, 1.0);
        if(offset.w<=0.0)continue;
        offset.xyz /= offset.w;
        vec2 sampleUV = offset.xy * 0.5 + 0.5;

        // Clamped offscreen samples fabricate walls along the viewport edge.
        if(any(lessThan(sampleUV,vec2(0)))||any(greaterThan(sampleUV,vec2(1))))continue;
        float sampleDepthNDC = depthAt(sampleUV);
        if(sampleDepthNDC>=1.0)continue;
        vec3 occluderViewPos = viewPosFromDepth(sampleUV, sampleDepthNDC);

        float rangeCheck =
            smoothstep(0.0, 1.0, radius / max(abs(P.z - occluderViewPos.z), 1e-4));
        occlusion += (occluderViewPos.z >= samplePos.z + min(pc.bias,radius*.02) ? 1.0 : 0.0) *
                    rangeCheck;
    }

    // A power curve strengthens contacts without clipping broad pockets to
    // flat black when the artist asks for stronger-than-unit occlusion.
    occlusion = pow(max(1.0-occlusion/float(kSamples),.05),max(pc.strength,0.0));
    outColor = vec4(vec3(clamp(occlusion, 0.0, 1.0)), 1.0);
}
