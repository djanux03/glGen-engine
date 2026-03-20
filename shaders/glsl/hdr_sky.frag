#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDR;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform float uExposure;
uniform float uGamma;
uniform float uTime;

// solid sky
uniform bool uUseSolidSky;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;


uniform mat3 uSkyRot; // rotates worldDir before sampling

// Sun uniforms
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunSize; // e.g. 0.9995 for small disc
uniform float uSunDiscIntensity;
uniform float uSunHaloIntensity;
uniform float uSunRaysIntensity;
uniform float uNightFactor;
uniform float uStarIntensity;
uniform float uMilkyWayIntensity;
uniform vec3 uNightHorizonGlow;
uniform float uNightDither;

// Lightweight procedural sky cloud controls
uniform bool uSkyCloudsEnabled;
uniform float uSkyCloudScale;
uniform float uSkyCloudCoverage;
uniform float uSkyCloudDensity;
uniform float uSkyCloudSoftness;
uniform float uSkyCloudSpeed;
uniform vec3 uSkyCloudColor;

const float PI = 3.14159265359;



float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * noise(p);
        p = p * 2.03 + vec2(11.7, 3.1);
        amp *= 0.5;
    }
    return v;
}

vec2 dirToEquirectUV(vec3 d)
{
    d = normalize(d);
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return vec2(u, v);
}

float starField(vec3 dir)
{
    vec2 uv = dirToEquirectUV(dir);
    vec2 g = uv * 2048.0;
    vec2 cell = floor(g);
    float h = hash21(cell);
    float star = smoothstep(0.9975, 1.0, h);
    float twinkle = 0.6 + 0.4 * sin(uTime * 2.0 + h * 123.4);
    return star * twinkle;
}

// Compute HDR value that produces the desired LDR color after ACES + gamma.
// Input: target color in sRGB (what the user picks in the UI).
// Output: linear HDR value that, after ACES tonemapping + pow(1/2.2),
//         closely reproduces the input sRGB color.
vec3 inverseACES(vec3 srgb)
{
    // sRGB -> linear target (what we want after ACES + gamma)
    vec3 t = pow(max(srgb, vec3(0.0)), vec3(2.2));
    // Solve the ACES curve: t = (x*(2.51*x+0.03)) / (x*(2.43*x+0.59)+0.14)
    // Rearranged: (2.43*t - 2.51)*x^2 + (0.59*t - 0.03)*x + 0.14*t = 0
    // Use quadratic formula on each channel.
    vec3 A = 2.43 * t - 2.51;
    vec3 B = 0.59 * t - 0.03;
    vec3 C = 0.14 * t;
    vec3 disc = max(B * B - 4.0 * A * C, vec3(0.0));
    // A is negative for normal color values (t < ~1.03), so use the
    // (-B - sqrt) root to get a positive result.
    vec3 x = (-B - sqrt(disc)) / (2.0 * A);
    return max(x, vec3(0.0));
}


// --- Atmospheric Scattering Parameters ---
const float R_EARTH = 6360000.0;
const float R_ATMOSPHERE = 6420000.0;
const vec3 BETA_RAYLEIGH = vec3(5.8e-6, 13.5e-6, 33.1e-6);
const float BETA_MIE = 21e-6;
const float H_RAYLEIGH = 8000.0;
const float H_MIE = 1200.0;
const float G_MIE = 0.76;

// Intersection with a sphere centered at origin
vec2 sphereIntersect(vec3 rayOrigin, vec3 rayDir, float radius) {
    float b = dot(rayOrigin, rayDir);
    float c = dot(rayOrigin, rayOrigin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) return vec2(-1.0);
    float sqrtD = sqrt(d);
    return vec2(-b - sqrtD, -b + sqrtD);
}

// Single-scattering atmospheric raymarching
vec3 calculateAtmosphere(vec3 rayDir, vec3 sunDir) {
    vec3 rayOrigin = vec3(0.0, R_EARTH + 1.0, 0.0);
    vec2 atmIntersection = sphereIntersect(rayOrigin, rayDir, R_ATMOSPHERE);
    if (atmIntersection.y < 0.0) return vec3(0.0); // Looking out to space

    float tMin = max(0.0, atmIntersection.x);
    float tMax = atmIntersection.y;
    vec2 earthIntersection = sphereIntersect(rayOrigin, rayDir, R_EARTH);
    if (earthIntersection.x > 0.0) tMax = min(tMax, earthIntersection.x); // Blocked by earth

    int numSamples = 16;
    int numSamplesLight = 8;
    float segmentLength = (tMax - tMin) / float(numSamples);
    float tCurrent = tMin + segmentLength * 0.5;

    vec3 totalRayleigh = vec3(0.0);
    vec3 totalMie = vec3(0.0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    float mu = dot(rayDir, sunDir);
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    float phaseM = 3.0 / (8.0 * PI) * ((1.0 - G_MIE * G_MIE) * (1.0 + mu * mu)) / 
                   ((2.0 + G_MIE * G_MIE) * pow(1.0 + G_MIE * G_MIE - 2.0 * G_MIE * mu, 1.5));

    for (int i = 0; i < numSamples; ++i) {
        vec3 samplePos = rayOrigin + rayDir * tCurrent;
        float height = length(samplePos) - R_EARTH;
        
        float hr = exp(-height / H_RAYLEIGH) * segmentLength;
        float hm = exp(-height / H_MIE) * segmentLength;
        opticalDepthR += hr;
        opticalDepthM += hm;

        // Light marching
        vec2 lightAtmIntersect = sphereIntersect(samplePos, sunDir, R_ATMOSPHERE);
        float segmentLengthLight = lightAtmIntersect.y / float(numSamplesLight);
        float tCurrentLight = segmentLengthLight * 0.5;
        float opticalDepthLightR = 0.0;
        float opticalDepthLightM = 0.0;
        
        bool inEarthShadow = false;
        vec2 lightEarthIntersect = sphereIntersect(samplePos, sunDir, R_EARTH);
        if (lightEarthIntersect.x > 0.0) inEarthShadow = true;

        if (!inEarthShadow) {
            for (int j = 0; j < numSamplesLight; ++j) {
                vec3 samplePosLight = samplePos + sunDir * tCurrentLight;
                float heightLight = length(samplePosLight) - R_EARTH;
                opticalDepthLightR += exp(-heightLight / H_RAYLEIGH) * segmentLengthLight;
                opticalDepthLightM += exp(-heightLight / H_MIE) * segmentLengthLight;
                tCurrentLight += segmentLengthLight;
            }

            vec3 tau = BETA_RAYLEIGH * (opticalDepthR + opticalDepthLightR) + 
                       BETA_MIE * 1.1 * (opticalDepthM + opticalDepthLightM);
            vec3 attenuation = exp(-tau);
            
            totalRayleigh += hr * attenuation;
            totalMie += hm * attenuation;
        }
        tCurrent += segmentLength;
    }
    
    vec3 sunIntensity = vec3(22.0); // Base sun intensity
    return (totalRayleigh * BETA_RAYLEIGH * phaseR + totalMie * BETA_MIE * phaseM) * sunIntensity;
}

void main()
{
    // Reconstruct view ray in view space from screen UV
    vec2 ndc = vUV * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);

    vec4 viewPos = uInvProj * clip;
    viewPos /= viewPos.w;

    vec3 viewDir = normalize(viewPos.xyz);

    // Rotate into world (w=0 so translation is ignored)
    vec3 worldDir = normalize((uInvView * vec4(viewDir, 0.0)).xyz);

    // Apply user sky rotation (XYZ) then sample equirect HDR
    worldDir = normalize(uSkyRot * worldDir);
    vec3 mapped;
    vec3 sunDir = normalize(-uSunDir);

    if (uUseSolidSky)
    {
        mapped = calculateAtmosphere(worldDir, sunDir);
    }
    else
    {
        vec2 uv = dirToEquirectUV(worldDir);
        vec3 hdr = texture(uHDR, uv).rgb;
        mapped = hdr * uExposure;
    }

    // Volumetric 3D Clouds (Raymarched)
    if (uSkyCloudsEnabled)
    {
        // Simple sphere intersection for the cloud layer
        float cloudMinHeight = 1500.0;
        float cloudMaxHeight = 4000.0;
        vec3 rayOrigin = vec3(0.0, R_EARTH + 1.0, 0.0);
        
        vec2 cloudBottomIntersect = sphereIntersect(rayOrigin, worldDir, R_EARTH + cloudMinHeight);
        vec2 cloudTopIntersect = sphereIntersect(rayOrigin, worldDir, R_EARTH + cloudMaxHeight);
        
        float tMinC = max(0.0, cloudBottomIntersect.y); // Entering bottom of cloud layer
        float tMaxC = cloudTopIntersect.y;              // Exiting top
        
        // If looking slightly down from mountain tops, adjust
        if (cloudBottomIntersect.y < 0.0 && cloudTopIntersect.y > 0.0) {
            tMinC = 0.0;
            tMaxC = cloudTopIntersect.y;
        }

        if (tMaxC > 0.0 && tMaxC > tMinC) {
            int cloudSteps = 32;
            int lightSteps = 4;
            float stepSizeC = (tMaxC - tMinC) / float(cloudSteps);
            float tCurrentC = tMinC + stepSizeC * hash21(vUV); // Dither start to reduce banding
            
            float transmittance = 1.0;
            vec3 scatteredLight = vec3(0.0);
            
            // Base cloud color affected by environment (sun color)
            // If uSunColor is very dim or different, we use it directly instead of inverseACES 
            // to allow physical integration with the new sunset.
            vec3 ambientLight = inverseACES(uSkyCloudColor) * 0.2;
            vec3 sunLightColor = uSunColor * 15.0; // Boost sunlight interaction
            
            for (int i = 0; i < cloudSteps; ++i) {
                if (transmittance < 0.01) break;
                
                vec3 samplePos = rayOrigin + worldDir * tCurrentC;
                float heightFraction = (length(samplePos) - R_EARTH - cloudMinHeight) / (cloudMaxHeight - cloudMinHeight);
                
                // Cloud density shape
                vec2 windOffset = vec2(uTime * uSkyCloudSpeed, uTime * uSkyCloudSpeed * 0.5);
                vec2 worldXZ = samplePos.xz * uSkyCloudScale * 0.0001;
                
                float n1 = fbm(worldXZ + windOffset);
                float n2 = fbm(worldXZ * 2.0 - windOffset * 0.5);
                float baseNoise = mix(n1, n2, 0.5);
                
                // Height gradient shaping (round bottoms, wispy tops)
                float heightGradient = 1.0 - pow(abs(heightFraction * 2.0 - 1.0), 2.0);
                float cloudDensity = smoothstep(1.0 - uSkyCloudCoverage, 1.0 - uSkyCloudCoverage + uSkyCloudSoftness, baseNoise * heightGradient);
                
                if (cloudDensity > 0.0) {
                    float extCoeff = uSkyCloudDensity * 0.1;
                    float stepOpticalDepth = cloudDensity * extCoeff * stepSizeC;
                    
                    // March towards sun for self-shadowing
                    float lightOpticalDepth = 0.0;
                    float tLight = 0.0;
                    float lightStepSize = (cloudMaxHeight - cloudMinHeight) / float(lightSteps);
                    
                    for (int j = 0; j < lightSteps; ++j) {
                        vec3 lightPos = samplePos + sunDir * tLight;
                        float lhf = (length(lightPos) - R_EARTH - cloudMinHeight) / (cloudMaxHeight - cloudMinHeight);
                        if (lhf < 0.0 || lhf > 1.0) break;
                        
                        vec2 lwXZ = lightPos.xz * uSkyCloudScale * 0.0001;
                        float ln = mix(fbm(lwXZ + windOffset), fbm(lwXZ * 2.0 - windOffset*0.5), 0.5);
                        float lhg = 1.0 - pow(abs(lhf * 2.0 - 1.0), 2.0);
                        float lDens = smoothstep(1.0 - uSkyCloudCoverage, 1.0 - uSkyCloudCoverage + uSkyCloudSoftness, ln * lhg);
                        
                        lightOpticalDepth += lDens * extCoeff * lightStepSize;
                        tLight += lightStepSize;
                    }
                    
                    // Beer-Lambert + Powder effect for silver lining
                    float beer = exp(-lightOpticalDepth);
                    float powder = 1.0 - exp(-lightOpticalDepth * 2.0);
                    float phase = mix(beer * powder, beer, 0.5); // blend for forward scattering
                    
                    // Henyey-Greenstein phase approximation for sun halo
                    float mu = dot(worldDir, sunDir);
                    float hg = (1.0 - 0.5*0.5) / pow(1.0 + 0.5*0.5 - 2.0*0.5*mu, 1.5);
                    
                    vec3 S = ambientLight + sunLightColor * phase * (hg * 0.5 + 0.5);
                    vec3 Sint = (S - S * exp(-stepOpticalDepth)) / extCoeff;
                    
                    scatteredLight += transmittance * Sint;
                    transmittance *= exp(-stepOpticalDepth);
                }
                
                tCurrentC += stepSizeC;
            }
            
            mapped = mapped * transmittance + scatteredLight;
        }
    }

    // The sun disc and glow are now natively handled by the Mie scattering phase function
    // in calculateAtmosphere(), which creates a physically accurate bright sun center
    // and atmospheric glow around it.

    // Night sky: stars + milky way + horizon glow
    if (uNightFactor > 0.001) {
        float stars = starField(worldDir) * uStarIntensity;
        vec2 uv = dirToEquirectUV(worldDir);
        float band = exp(-pow(dot(worldDir, normalize(vec3(0.2, 0.7, 0.1))), 2.0) * 8.0);
        float dust = fbm(uv * 18.0 + vec2(0.0, uTime * 0.002));
        float milky = band * dust * uMilkyWayIntensity;
        float horizon = pow(clamp(1.0 - abs(worldDir.y), 0.0, 1.0), 3.5);
        vec3 glow = uNightHorizonGlow * horizon;
        mapped += (stars + milky) * uNightFactor;
        mapped += glow * uNightFactor;
    }

    // Subtle night dithering to reduce banding
    if (uNightFactor > 0.001 && uNightDither > 0.0) {
        float d = (hash21(vUV * vec2(1024.0, 512.0)) - 0.5) * uNightDither;
        mapped += vec3(d);
    }

    FragColor = vec4(mapped, 1.0);

}
