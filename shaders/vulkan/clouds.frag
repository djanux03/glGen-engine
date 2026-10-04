#version 450
#extension GL_GOOGLE_include_directive : require

// ---------------------------------------------------------------------------
// Volumetric cloudscape -- half-resolution raymarch.
// ---------------------------------------------------------------------------
// Replaces the analytic five-octave fbm deck that used to live in sky.frag.
// A Perlin-Worley profile is carved by height/weather and periodic cellular
// erosion. Light queries integrate that same density toward the emitter;
// six diminishing scattering octaves approximate the dense-cloud bounce.
// Atmosphere-off asset reviews retain their established Nubis noise/lighting
// path. Both use the active sun/moon irradiance supplied by the renderer.
//
// Output is premultiplied: rgb = scattered light, a = transmittance. sky.frag
// reconstructs it and blends cloud.rgb + sky.rgb * cloud.a before grading.
//
// Coordinate note: the layer is a pair of spheres concentric with the
// atmosphere planet (skyModel.glsl), so distant clouds compress into a band
// and end at a real horizon; but noise is sampled at TRUE world XZ
// (camPosWS + rd*t), not at the planet-local position, so the field is
// continuous across the world and does not swim as the camera moves.

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outCloud;
layout(location = 1) out float outScatteringDepth;

#define FRAME_DATA_SET 1
#include "frameData.glsl"
#include "skyModel.glsl"

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 jitter; // x = temporal dither phase, yz = cloud viewport dimensions
} pc;

float clSegmentFootprint=0;
#define CLOUD_FOOTPRINT clSegmentFootprint
// Adaptive marching diverges between pixels. Specify weather/curl footprints
// explicitly so those samples do not depend on undefined implicit derivatives.
#define CLOUD_SAMPLE_2D_LOD(image,uv,lod) (uFrame.atmosphereParams.x>.5?textureLod(image,uv,lod):texture(image,uv))
#include "cloudDensity.glsl"

// --- sun transmittance ---------------------------------------------------
// Cone-tap march toward the light. Six taps spread in a widening cone (the
// spread is what softens self-shadowing into something that reads as
// multiple scattering rather than a hard shadow), plus one long tap that
// catches a distant bank blocking the sun. Profile density only -- running
// the detail erosion inside the light loop costs 6x the texture fetches for
// a difference that the exp() flattens anyway.
//
// The result is DIMENSIONLESS: a weighted mean of the profile density along
// the cone, in 0..1, which the caller turns into an optical depth by scaling
// with cloudLightAbsorption. Integrating real metres here instead is what a
// first pass did, and it does not work: the cone is roughly a layer thickness
// long, so a half-dense path accumulated ~1500 density-metres, every exp()
// downstream underflowed to zero, and the clouds rendered as black cutouts
// against the sky. Keeping the light term unit-free means one dial sets how
// deep self-shadowing goes regardless of how thick the layer is authored.
const vec3 kConeOffsets[6] = vec3[6](
    vec3( 0.20,  0.10,  0.30), vec3(-0.25,  0.15, -0.10),
    vec3( 0.10, -0.20, -0.30), vec3(-0.15, -0.10,  0.25),
    vec3( 0.30,  0.25, -0.20), vec3( 0.00,  0.00,  0.00));

float clDensityToLight(vec3 wpos, vec3 toLight, int taps) {
    // Cone length is tied to the layer, not to the view step: how far light
    // has to travel through cloud to reach this point is a property of the
    // cloud, and has nothing to do with how finely this ray happens to march.
    float coneLen = clLayerThickness() * 0.55;
    float total = 0.0;
    float weight = 0.0;
    for (int i = 0; i < 6; ++i) {
        if (i >= taps)
            break;
        // Taps bunch toward the sample point, where occlusion matters most.
        float u = (float(i) + 0.5) / float(taps);
        float t = coneLen * u * u;
        vec3 p = wpos + toLight * t + kConeOffsets[i] * t;
        float h = (p.y - clLayerBottom()) / clLayerThickness();
        float w = 1.0 - 0.5 * u;
        if (h >= 0.0 && h <= 1.0) {
            // Mip climbs with cone distance: far taps are blurry by
            // construction, which is both cheaper and closer to the truth.
            total += clProfileDensity(p, h, clSampleWeather(p), float(i) * 0.5) * w;
        }
        weight += w;
    }
    // One long tap for a distant bank standing between this point and the
    // sun -- the thing that puts a whole cloud into another's shadow.
    vec3 far = wpos + toLight * coneLen * 4.0;
    float hf = (far.y - clLayerBottom()) / clLayerThickness();
    if (hf >= 0.0 && hf <= 1.0)
        total += clProfileDensity(far, hf, clSampleWeather(far), 3.0) * 0.6;
    weight += 0.6;

    return total / max(weight, 1e-4);
}

// --- layer intersection --------------------------------------------------
// Returns (tEnter, tExit) against the two concentric shells, handling the
// camera being below, inside, or above the layer. tExit < tEnter means miss.
vec2 clLayerInterval(vec3 ro, vec3 rd, float camHeight) {
    float rInner = kAtmRg + clLayerBottom();
    float rOuter = kAtmRg + clLayerTop();
    vec2 inner = atmRaySphere(ro, rd, rInner);
    vec2 outer = atmRaySphere(ro, rd, rOuter);

    if (camHeight < clLayerBottom()) {
        // Below: enter at the inner shell's far root, leave at the outer's.
        if (outer.y <= 0.0)
            return vec2(1.0, -1.0);
        return vec2(max(inner.y, 0.0), max(outer.y, 0.0));
    }
    if (camHeight > clLayerTop()) {
        // Above, looking down: enter at the outer shell's near root and stop
        // at the inner shell if the ray reaches it, otherwise at the outer's
        // far root (a grazing ray that passes through and out again).
        if (outer.y <= 0.0)
            return vec2(1.0, -1.0);
        float enter = max(outer.x, 0.0);
        float exitT = (inner.y > 0.0 && inner.x > 0.0) ? inner.x : max(outer.y, 0.0);
        return vec2(enter, exitT);
    }
    // Inside the layer.
    float exitT = max(outer.y, 0.0);
    if (inner.y > 0.0 && inner.x > 0.0)
        exitT = min(exitT, inner.x);
    return vec2(0.0, exitT);
}

// Optical depth uses the same eroded medium and metre-based extinction as
// the view ray. The legacy weighted-density cone hid path length and could
// barely distinguish a sunlit boundary from kilometres of cloud interior.
float clOpticalDepthToLight(vec3 wpos,vec3 planetPos,vec3 toLight,int taps) {
    float height=length(planetPos)-kAtmRg;
    vec2 interval=clLayerInterval(planetPos,toLight,height);
    float end=max(interval.y,0.0);
    float tau=0.0,oldFootprint=clSegmentFootprint;
    float normalization=exp2(float(taps))-1.0;
    for(int i=0;i<taps;++i) {
        // Exponential boundaries resolve the local silhouette first while
        // still traversing the complete spherical layer toward the emitter.
        float begin=end*(exp2(float(i))-1.0)/normalization;
        float finish=end*(exp2(float(i+1))-1.0)/normalization;
        float ds=finish-begin;
        clSegmentFootprint=max(ds*.25,oldFootprint);
        float shapeLod=log2(max(clSegmentFootprint*float(textureSize(uShapeNoise,0).x)/max(uFrame.styleCloud2.y,50.0),1.0));
        float detailLod=log2(max(clSegmentFootprint*float(textureSize(uPeriodicDetailNoise,0).x)/max(uFrame.styleCloud2.z,5.0),1.0));
        for(int j=0;j<2;++j) {
            float distance=mix(begin,finish,(float(j)+.5)*.5);
            vec3 p=wpos+toLight*distance;
            float h=(length(planetPos+toLight*distance)-kAtmRg-clLayerBottom())/clLayerThickness();
            vec3 weather=clSampleWeather(p);
            float profile=clProfileDensity(p,h,weather,shapeLod);
            tau+=clDetailDensity(p,h,profile,weather.z,10000.0,detailLod)*ds*.5;
        }
    }
    clSegmentFootprint=oldFootprint;
    // The absorption dial retains its authored contrast range; 4 is the
    // reference setting, at which light and view extinction agree exactly.
    return tau*max(uFrame.styleCloud3.x,0.0)*max(uFrame.styleCloud3.y,0.0)*.25*
           max(uFrame.styleCloud1.w,0.0);
}

void main() {
    vec4 farW = pc.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 camPos = uFrame.camPosWS.xyz;
    vec3 rd = normalize(farW.xyz / farW.w - camPos);
    vec4 adjacentX=pc.invViewProj*vec4(vNdc+vec2(2.0/max(pc.jitter.y,1.0),0),1,1);
    vec4 adjacentY=pc.invViewProj*vec4(vNdc+vec2(0,2.0/max(pc.jitter.z,1.0)),1,1);
    float pixelAngle=max(length(normalize(adjacentX.xyz/adjacentX.w-camPos)-rd),
                         length(normalize(adjacentY.xyz/adjacentY.w-camPos)-rd));

    // Planet-local origin drives the layer intersection (so the deck curves
    // to a horizon); world position drives the noise (so the field is stable
    // and continuous as the camera translates).
    vec3 ro = atmPlanetPos(camPos.y);
    float camHeight = camPos.y;

    vec2 interval = clLayerInterval(ro, rd, camHeight);
    float tEnter = interval.x;
    float tExit = interval.y;

    outCloud = vec4(0.0, 0.0, 0.0, 1.0);
    outScatteringDepth=0.0;
    if (tExit <= tEnter)
        return;

    // A ray a few degrees above the horizon crosses an effectively unbounded
    // slab; without this cap the step size explodes and the deck dissolves
    // into stripes exactly where it should be densest.
    float maxDist = max(uFrame.styleCloud5.x, 1000.0);
    tExit = min(tExit, tEnter + maxDist);

    int maxSteps = clamp(int(uFrame.styleCloud5.y + 0.5), 24, 192);
    int lightTaps = clamp(int(uFrame.styleCloud5.z + 0.5), 1, 6);

    float span = tExit - tEnter;
    // Step size is tied to the LAYER, not to the span. Dividing the span by
    // the step budget (the obvious first move) means a near-horizon ray --
    // which crosses tens of kilometres of layer -- gets half-kilometre steps
    // and dissolves into horizontal stripes exactly where the deck should be
    // densest and most detailed. Instead: a fine step sized to resolve the
    // layer, grown linearly with distance so the far half of the ray is cheap.
    // That growth IS Nubis3's adaptive step size, and it is why the budget
    // reaches the horizon at all.
    bool modernSampling=uFrame.atmosphereParams.x>.5;
    // Atmosphere-off canonical reviews retain their established background;
    // its sparse clouds are incidental to material/geometry comparisons.
    float fineStep = max(clLayerThickness() / (modernSampling?float(maxSteps):48.0), span / float(maxSteps * 4));
    const float kCoarseMul = 3.0;
    // Doubles the step roughly every 10 km, so the near field stays crisp and
    // a 40 km ray still finishes inside the loop bound.
    const float kStepGrowth = 1.0 / 10000.0;
    // Air extinction between eye and cloud. 1/16 km washes out distant horizon
    // clouds (15-30 km) into the pale horizon sky color while keeping near clouds sharp.
    const float kAerialRate = 1.0 / 16000.0;

    vec3 toLight = normalize(-uFrame.lightDir.xyz);
    float mu = dot(rd, toLight);

    float g = clamp(uFrame.styleCloud4.x, 0.0, 0.95);
    float silverIntensity = uFrame.styleCloud4.y;
    float silverSpread = uFrame.styleCloud4.z;
    // Dual lobe: a broad forward lobe for the general brightening toward the
    // sun, plus a tight one that only fires within a few degrees of it --
    // that second term is the silver lining on a backlit cloud edge.
    float phase = max(clHG(mu, g),
                      silverIntensity * clHG(mu, clamp(0.99 - silverSpread, -0.95, 0.95)));

    float densityMul = max(uFrame.styleCloud3.x, 0.0);
    float lightAbsorb = max(uFrame.styleCloud3.y, 0.0);
    float powderStrength = clamp(uFrame.styleCloud4.w, 0.0, 1.0);

    // Ambient: the sky's own radiance straight up, which is the dominant term
    // lighting a cloud's flanks and underside. Six steps is plenty for a
    // single scalar, and it means the clouds inherit the engine's atmosphere
    // (haze dial, twilight, moonlight) rather than carrying a second sky model.
    AtmSample ambientSample =
        atmScatter(ro, vec3(0.0, 1.0, 0.0), toLight, vec3(uFrame.lightDir.w),
                   toLight, vec3(0.0), uFrame.styleSkyZenith.w, 6);
    vec3 skyAmbient = ambientSample.radiance * uFrame.styleSkyHorizon.w *
                      max(uFrame.styleCloud3.z, 0.0);
    // Undersides see the ground, not the zenith -- darker and warmer.
    vec3 groundAmbient = skyAmbient * vec3(0.42, 0.40, 0.36) * 0.35;

    vec3 sunColor = uFrame.sunRadiance.rgb;

    // Dither the entry point so the step lattice does not band, using a sub-step
    // jitter amplitude to avoid high-frequency pixel stippling on cloud edges.
    float dither = (clHash12(gl_FragCoord.xy + pc.jitter.x) - 0.5) * 0.35;

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;
    float scatteringMoment=0.0,scatteringWeight=0.0;
    float opacityMoment=0.0,opacityWeight=0.0;

    float t = tEnter + fineStep * dither;
    bool refining = false;
    int misses = 0;
    float tFirstHit = tExit;
    bool hasHit = false;

    for (int i = 0; i < maxSteps * 3 && t < tExit; ++i) {
        // Adaptive: the same fine/coarse pair, both scaled up with distance.
        float grow = 1.0 + t * kStepGrowth;
        float baseStep = fineStep * grow;
        float coarseStep = baseStep * kCoarseMul;
        float step = refining ? baseStep : coarseStep;
        if(modernSampling)step=min(step,tExit-t);

        vec3 wpos = camPos + rd * t;
        // Height comes from the SPHERE, not from wpos.y: a cloud 40 km away
        // must sit lower in the layer than one overhead, or the deck reads as
        // an infinite flat ceiling.
        float planetH = length(ro + rd * t) - kAtmRg;
        float h = (planetH - clLayerBottom()) / clLayerThickness();
        if (h < 0.0 || h > 1.0) {
            t += step;
            continue;
        }

        clSegmentFootprint=max(pixelAngle*max(t,0),step*.25);
        vec3 weather = clSampleWeather(wpos);
        float mip = clRemapClamped(t, 2000.0, 40000.0, 0.0, 3.0);
        float profile = clProfileDensity(wpos, h, weather, mip);

        if (profile <= 0.0) {
            if (refining && ++misses >= 6)
                refining = false;
            t += step;
            continue;
        }

        if (!refining) {
            // First hit on a coarse step: back up and re-enter at the fine
            // rate, so the cloud's leading edge is not chopped off at 3x the
            // step. Costs one wasted iteration per cloud entered.
            refining = true;
            misses = 0;
            t = max(t - coarseStep, tEnter);
            continue;
        }
        misses = 0;

        // The detail texture's voxels can be only a few metres wide while a
        // march segment spans tens of metres. A single unfiltered fetch made
        // that mismatch visible as salt-and-pepper opacity, even with history.
        // Four stratified density samples and a footprint-derived detail mip
        // integrate those unresolved features instead of choosing one voxel.
        float pixelSpan=pixelAngle*max(t,0);
        float detailTexels=max(step*.25,pixelSpan)*float(textureSize(uDetailNoise,0).x)/max(uFrame.styleCloud2.z,5.0);
        float detailMip=max(mip,log2(max(detailTexels,1.0)));
        float density=modernSampling?0.0:clDetailDensity(wpos,h,profile,weather.z,t,mip);
        for(int sampleIndex=0;sampleIndex<(modernSampling?4:0);++sampleIndex) {
            float sampleDistance=t+step*(float(sampleIndex)+.5)*.25;
            vec3 sampleWorld=camPos+rd*sampleDistance;
            float sampleHeight=(length(ro+rd*sampleDistance)-kAtmRg-clLayerBottom())/clLayerThickness();
            vec3 sampleWeather=clSampleWeather(sampleWorld);
            float sampleProfile=clProfileDensity(sampleWorld,sampleHeight,sampleWeather,mip);
            density+=clDetailDensity(sampleWorld,sampleHeight,sampleProfile,sampleWeather.z,sampleDistance,detailMip)*.25;
        }
        if (density <= 0.0005) {
            t += step;
            continue;
        }

        if (!hasHit) {
            tFirstHit = t;
            hasHit = true;
        }

        float sigmaT = density * densityMul;
        float segT = exp(-sigmaT * step);

        vec3 luminance;
        if(!modernSampling) {
        // Canonical asset-review lighting (Nubis compatibility path).
        float densityToLight = clDensityToLight(wpos, toLight, lightTaps) *
                               lightAbsorb * uFrame.styleCloud1.w;

        // Attenuation: Beer's law, but floored by a second, shallower curve.
        // Pure Beer goes black in cloud cores; real cores are lit by multiply
        // scattered light, and this two-term form is the cheap stand-in the
        // Nubis slides use.
        float primary = exp(-densityToLight);
        float secondary = exp(-densityToLight * 0.25) * 0.7;
        float attenuation = max(primary,
                                clRemapClamped(mu, 0.7, 1.0, secondary, secondary * 0.25));

        // In-scatter probability: deeper in the cloud and higher in the layer
        // means more chances for a photon to have scattered toward the eye.
        float depthProb = mix(0.05 + pow(clamp(profile, 0.0, 1.0),
                                         clRemapClamped(h, 0.3, 0.85, 0.5, 2.0)),
                              1.0, clamp(densityToLight * 2.0, 0.0, 1.0));
        float verticalProb = pow(clRemapClamped(h, 0.07, 0.30, 0.12, 1.0), 0.8);
        float inScatter = clamp(depthProb * verticalProb, 0.0, 1.0);

        // Powder: thin edges scatter less back at you than their optical
        // depth alone predicts, which darkens the rims of clouds seen against
        // the sun. Only applied on the sun-facing side, hence the mu ramp.
        float powder = 1.0 - exp(-sigmaT * 6.0);
        powder = mix(1.0, powder, powderStrength * clamp(-mu * 0.5 + 0.5, 0.0, 1.0));

        vec3 sunLight = sunColor * attenuation * inScatter * phase * powder;

        // Multiple scattering, Wrenninge-style: two extra octaves with
        // progressively weaker extinction and flatter phase. Costs no new
        // samples and is most of what keeps dense cores from going flat grey.
        float msAtten = exp(-densityToLight * 0.35) * 0.45;
        float msPhase = mix(phase, 0.25, 0.5);
        sunLight += sunColor * msAtten * msPhase * 0.5;
        float msAtten2 = exp(-densityToLight * 0.12) * 0.18;
        sunLight += sunColor * msAtten2 * 0.15;

        vec3 ambient = mix(groundAmbient, skyAmbient, clamp(h * 1.3, 0.0, 1.0));

        // Authored tints, applied to a physical result rather than replacing
        // it -- same division of labour the old analytic deck settled on.
        vec3 tint = mix(uFrame.styleCloudBase.rgb, uFrame.styleCloudMid.rgb,
                        smoothstep(0.15, 0.75, profile));
        tint = mix(tint, uFrame.styleCloudLit.rgb, clamp(attenuation, 0.0, 1.0) * 0.6);

        luminance = (sunLight + ambient) * tint;
        } else {
            float lightTau=clOpticalDepthToLight(wpos,ro+rd*t,toLight,lightTaps);
            // Wrenninge's octave approximation: each additional scattering
            // order loses energy, sees reduced extinction and tends toward
            // an isotropic normalized phase. It is a bounded approximation,
            // not a path-traced solution or an arbitrary radiance floor.
            float weight=1.0,extinctionScale=1.0,anisotropy=g;
            float lightEnergy=0.0;
            for(int order=0;order<6;++order) {
                float normalizedPhase=mix(clHG(mu,anisotropy),clHG(mu,-.2*pow(.5,float(order))),.15);
                lightEnergy+=weight*exp(-lightTau*extinctionScale)*normalizedPhase;
                weight*=.5;extinctionScale*=.25;anisotropy*=.5;
            }
            // A weak powder adjustment is an explicit style control. It
            // depends on actual light optical depth, not the march step.
            lightEnergy*=mix(1.0,1.0-exp(-2.0*lightTau),powderStrength*.5);
            vec3 ambient=mix(groundAmbient,skyAmbient,clamp(h*1.3,0.0,1.0));
            vec3 tint=mix(uFrame.styleCloudBase.rgb,uFrame.styleCloudMid.rgb,smoothstep(.15,.75,profile));
            tint=mix(tint,uFrame.styleCloudLit.rgb,exp(-lightTau)*.6);
            luminance=(sunColor*lightEnergy+ambient)*tint;
        }

        // Energy-conserving segment integration (Frostbite): the analytic
        // integral of in-scatter across the step, not a point sample of it.
        vec3 integrated = (luminance * sigmaT - luminance * sigmaT * segT) /
                          max(sigmaT, 1e-6);

        // Aerial perspective. Fifty kilometres of air between the eye and a
        // cloud is not clear: it extinguishes the cloud's light and replaces
        // it with its own glow, which is why a distant bank reads as a pale
        // silhouette rather than a small sharp one. Doing this per step
        // rather than once at the end also fixes what the horizon looked like
        // without it -- the far field is where step size has grown the most,
        // and the raw march there resolves into horizontal stripes; washing
        // it toward the sky colour dissolves them into haze, which is both
        // cheaper and more correct than paying for more steps out there.
        // Aerial perspective extinction along the ray path.
        float aerial = exp(-t * kAerialRate);
        // The final contrast fade already transports cloud light through air.
        // Applying the same attenuation here squared it, turning distant lit
        // clouds into dark silhouettes. Preserve the canonical legacy path.
        if(!modernSampling) integrated *= aerial;

        vec3 contribution=transmittance*integrated;
        float lightWeight=max(dot(contribution,vec3(.2126,.7152,.0722)),0.0);
        scatteringMoment+=lightWeight*t;scatteringWeight+=lightWeight;
        float removed=transmittance*(1.0-segT);
        opacityMoment+=removed*t;opacityWeight+=removed;
        scattered += contribution;
        transmittance *= segT;

        if (transmittance < 0.005) {
            transmittance = 0.0;
            break;
        }
        t += step;
    }

    // Apply distance atmospheric haze fade: as ray distance to the cloud climbs
    // toward the horizon (15 km - 35 km), atmospheric extinction washes out the
    // cloud's contrast and opacity, dissolving it into the background horizon sky color.
    if (hasHit) {
        float aerialFade = exp(-tFirstHit * kAerialRate);
        float cloudAlpha = (1.0 - clamp(transmittance, 0.0, 1.0)) * aerialFade;
        outCloud = vec4(scattered * aerialFade, clamp(1.0 - cloudAlpha, 0.0, 1.0));
        // Darkness still needs a volume depth. Opacity weighting provides a
        // stable fallback when direct and ambient illumination both vanish.
        outScatteringDepth=scatteringWeight>1e-7?scatteringMoment/scatteringWeight:
            opacityWeight>1e-7?opacityMoment/opacityWeight:0.0;
    } else {
        outCloud = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
