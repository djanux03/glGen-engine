// volumetric_fog.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D depthTex;
uniform sampler2D colorTex;
uniform vec3 sunColor;
uniform vec3 uLightDir;
uniform float nearPlane;
uniform float farPlane;
uniform float fogDensity;
uniform float lightExposure;
uniform float lightDecay;
uniform float lightWeight;
uniform int sampleCount;
uniform float sunVisible;
uniform float sunPosX;
uniform float sunPosY;
uniform float uTime;

float linearizeDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * nearPlane * farPlane) /
           max(farPlane + nearPlane - z * (farPlane - nearPlane), 0.0001);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) +
           (d - b) * u.x * u.y;
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

// Interleaved Gradient Noise for better dithering
float ign(vec2 p) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(p, magic.xy)));
}

void main() {
    float depth = texture(depthTex, TexCoords).r;
    float linearDepth = linearizeDepth(depth);
    float depth01 = clamp(linearDepth / farPlane, 0.0, 1.0);

    vec3 viewDirApprox = normalize(vec3(TexCoords * 2.0 - 1.0, -1.0));
    vec3 sunDir = normalize(-uLightDir);
    float phase = pow(max(dot(viewDirApprox, sunDir), 0.0), 6.0);

    // Base aerial perspective fog term.
    vec2 wind = normalize(vec2(0.82, 0.57));
    float drift = fbm(TexCoords * vec2(3.0, 1.7) + wind * uTime * 0.025);
    float lowBand = fbm(TexCoords * vec2(8.0, 2.0) - wind.yx * uTime * 0.018);
    float livingDensity = mix(0.78, 1.24, drift) * mix(0.90, 1.14, lowBand);
    float fog = 1.0 - exp(-depth01 * (fogDensity * 45.0) * livingDensity);
    if (depth >= 0.99999) {
        fog = 0.0;
    }
    vec3 fogColor = mix(vec3(0.52, 0.60, 0.70), vec3(0.72, 0.78, 0.86), phase);
    fogColor *= mix(0.94, 1.08, livingDensity);
    vec3 volumetric = fogColor * fog * 0.35;

    // Screen-space radial light shafts from sun position.
    vec2 sunUV = vec2(sunPosX, sunPosY);
    vec2 delta = (TexCoords - sunUV) / float(max(sampleCount, 1));
    float illumDecay = 1.0;
    vec2 coord = TexCoords;
    vec3 shafts = vec3(0.0);

    int taps = clamp(sampleCount, 8, 128);
    float jitter = (ign(gl_FragCoord.xy) - 0.5) * 0.01; 
    
    // Offset starting coord slightly to hide banding
    coord -= delta * jitter;

    for (int i = 0; i < 128; ++i) {
        if (i >= taps) break;
        coord -= delta;

        if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0)
            continue;

        float d = texture(depthTex, coord).r;
        vec3 col = texture(colorTex, coord).rgb;
        
        // Luminance of the pixel
        float lum = dot(col, vec3(0.299, 0.587, 0.114));
        
        // Occlusion is based on depth (terrain blocks fully) 
        // and luminance (thick clouds block sunlight).
        // The sun in HDR is extremely bright (> 10 typically), while clouds are darker.
        float occlusion = 1.0;
        if (d < 0.9995) {
            occlusion = 0.18; // Terrain/objects block mostly
        } else {
            // It's the sky. Are there thick clouds?
            // A pure sun pixel is very bright. A thick cloud is less bright.
            occlusion = smoothstep(1.0, 15.0, lum);
            occlusion = clamp(occlusion, 0.1, 1.0); 
        }

        shafts += sunColor * occlusion * illumDecay * lightWeight;
        illumDecay *= lightDecay;
    }

    shafts *= lightExposure * sunVisible * phase;
    volumetric += shafts;

    FragColor = vec4(volumetric, 1.0);
}
