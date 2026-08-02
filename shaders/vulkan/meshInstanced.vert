#version 450
#extension GL_GOOGLE_include_directive : require

// GPU-instanced counterpart to mesh.vert (Phase 3 vegetation, extended R4
// for the scatter system): identical varyings/FrameData/push-constant
// layout, but the model matrix comes from a per-instance vertex attribute
// (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE) instead of the push constant --
// one draw call renders every placed instance of a layer. `pc.model` is
// unused here (kept only so this shader can share mScenePipelineLayout with
// mesh.vert); `pc.textureIndex` is still used, since it's constant across
// all instances in one draw (same layer submesh = same material).
//
// R4: two more per-instance attributes (color jitter, biome weights baked
// at placement time -- see VulkanRenderer::VegInstanceGpu) pass through as
// varyings to meshInstanced.frag, which is why vegetation needs its own
// fragment shader fork instead of reusing mesh.frag directly.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inModelCol0;
layout(location = 4) in vec4 inModelCol1;
layout(location = 5) in vec4 inModelCol2;
layout(location = 6) in vec4 inModelCol3;
layout(location = 7) in vec4 inColorJitter;
layout(location = 8) in vec4 inBiomeWeights;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vViewZ;
layout(location = 4) out vec3 vColorJitter;
layout(location = 5) out vec3 vBiomeWeights;
// 0 at the instance's base, 1 at its tip, normalized by the plant's own
// world height so it means the same thing for a grass clump and a conifer.
// Drives both the wind falloff here and the root-darkening in the fragment
// stage (see ScatterLayer::groundOcclusion).
layout(location = 6) out float vHeightFrac;

#include "frameData.glsl"

layout(push_constant) uniform Push {
    mat4 model;        // unused in this shader -- see file comment
    uint textureIndex; // used in the fragment stage
    uint roughnessIndex;
    uint metallicIndex;
    uint aoIndex;
    float roughnessScalar;
    float metallicScalar;
    float aoScalar;
    uint materialFlags;
    // R5 wind. Declared here (past the fragment-only material block, which
    // this stage never reads) because push-constant members must be declared
    // in layout order -- a shorter prefix would put these at the wrong
    // offset. windStrength is meters of TIP displacement; 0 = static.
    float windStrength;
    float windSpeed;
    // Height of this batch's UNSCALED mesh, in mesh units. Used to turn
    // "height above base" into a 0..1 fraction of the plant's own height, so
    // windStrength means the same thing (meters at the tip) for a 0.6m grass
    // clump and a 25m conifer. Without it, a single quadratic falloff in
    // absolute world height would either leave grass motionless or tear
    // trees apart.
    float windMeshHeight;
    float groundOcclusion; // fragment stage only
} pc;

// Cheap 2D value-noise hash, used only to decorrelate neighboring instances'
// sway phase. Grass swaying in perfect unison is the single most obvious
// tell of vertex wind, and it costs one hash to avoid.
float windHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

void main() {
    mat4 instanceModel = mat4(inModelCol0, inModelCol1, inModelCol2, inModelCol3);
    vec4 world = instanceModel * vec4(inPos, 1.0);

    // Normalized height along the plant, shared by the wind falloff below
    // and by the fragment stage's root darkening. The instance matrix's Y
    // column carries the uniform scale times the vertical-only stretch (see
    // ScatterLayer::heightScaleMin), so this is the plant's actual
    // world-space height, and t01 means the same thing at any size.
    vec3 instanceOrigin = inModelCol3.xyz;
    float worldHeight = max(pc.windMeshHeight * length(inModelCol1.xyz), 1e-4);
    float t01 = clamp((world.y - instanceOrigin.y) / worldHeight, 0.0, 1.0);
    vHeightFrac = t01;

    // --- R5 vertex wind ---
    // Displacement is weighted by height above the instance's own origin, so
    // the base stays planted in the ground and the tips move most -- the
    // whole reason this is done per-vertex rather than by rotating the
    // instance matrix on the CPU.
    //
    // Deliberately NOT applied to the normal: recomputing a correct normal
    // for the bent pose costs a second transform, and at grass scale the
    // lighting error is invisible against the sway itself.
    if (pc.windStrength > 0.0001) {
        float phase = windHash(instanceOrigin.xz) * 6.2831853;
        float t = uFrame.miscParams.z * pc.windSpeed;

        // Two incommensurate frequencies so the motion never reads as a
        // clean sine loop, plus a slow spatial travelling wave across the
        // world so a field gusts in ripples rather than uniformly.
        float travel = dot(instanceOrigin.xz, vec2(0.06, 0.045));
        float sway = sin(t + phase + travel) * 0.65
                   + sin(t * 1.7 + phase * 2.3) * 0.35;

        // Quadratic falloff toward the base: a stiff stem, a loose tip.
        float bend = t01 * t01;
        // Fixed prevailing wind direction. A per-frame direction would have
        // to come from FrameData; the visual gain does not justify the
        // uniform-block churn.
        vec2 windDir = normalize(vec2(0.86, 0.51));

        world.xz += windDir * sway * pc.windStrength * bend;
        // Slight downward pull as the blade bends over, so it arcs instead
        // of stretching sideways.
        world.y -= abs(sway) * pc.windStrength * bend * 0.15;
    }

    gl_Position = uFrame.viewProj * world;
    vNormalWS = mat3(instanceModel) * inNormal;
    vUV = inUV;
    vWorldPos = world.xyz;
    vViewZ = -(uFrame.view * world).z; // positive distance in front of camera
    vColorJitter = inColorJitter.rgb;
    vBiomeWeights = inBiomeWeights.xyz;
}
