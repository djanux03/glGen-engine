// Surface-only snow: world coordinates keep patches stable across chunks/LODs.
// Water uses the same uploaded TerrainWater field as the water pass.
float snowNoise(vec2 p) {
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(paintHash13(vec3(i,7)),paintHash13(vec3(i+vec2(1,0),7)),f.x),
               mix(paintHash13(vec3(i+vec2(0,1),7)),paintHash13(vec3(i+1.0,7)),f.x),f.y);
}

float applySnow(vec3 pos, vec3 geometricNormal, float reception,
                inout vec3 albedo, inout vec3 normal, inout float roughness) {
    if(uFrame.snowParams.x<=0.0 || reception<=0.0) return 0.0;
    float water=uFrame.waterParams0.x;
    vec2 uv=(pos.xz-uFrame.waterParams3.yz)/max(uFrame.waterParams3.w,1.0);
    if(uFrame.snowTextures.w!=0xffffffffu && uFrame.waterParams3.w>0.0 &&
       all(greaterThanEqual(uv,vec2(0))) && all(lessThanEqual(uv,vec2(1))))
        water=texture(uTextures[nonuniformEXT(uFrame.snowTextures.w)],uv).r;
    float dry=smoothstep(water+0.08,water+0.45,pos.y);
    float slope=1.0-clamp(geometricNormal.y,0.0,1.0);
    float limit=max(uFrame.snowParams.z,0.01);
    float facing=1.0-smoothstep(limit*.45,limit,slope);
    vec2 p=pos.xz/max(uFrame.snowParams.y,0.5);
    float patches=snowNoise(p)*.75+snowNoise(p*3.13)*.25;
    float coverage=clamp(uFrame.snowParams.x+uFrame.snowParams.w*
        clamp((pos.y-water)/80.0,0.0,1.0),0.0,1.0);
    // At full coverage even the lowest patch must be covered. Keeping the
    // transition inside [0,1] also avoids residual snow as coverage approaches 0.
    float threshold=mix(1.09,-.09,coverage);
    float amount=smoothstep(threshold-.09,threshold+.09,patches)*facing*dry*reception;
    if(amount<=0.001) return 0.0;

    vec2 materialUV=pos.xz/max(uFrame.snowExtra.x,.5);
    // Broad wind-scoured washes and sastrugi wind ripples, anchored in world space.
    vec2 driftUV=vec2(pos.x*.19+pos.z*.07,pos.z*.045);
    float drift=snowNoise(driftUV);

    // The Long Dark directional sastrugi wave pattern
    vec2 sastrugiUV=vec2(pos.x*.18+pos.z*.08,pos.z*.07-pos.x*.03);
    float sastrugi=sin(sastrugiUV.x*6.283185+snowNoise(sastrugiUV*3.5)*3.14159)*0.5+0.5;
    float windMod=clamp(uFrame.snowParams2.z,0.0,1.0);
    float driftTotal=mix(drift,sastrugi,windMod*0.7);

    vec3 snow=uFrame.snowColor.rgb*(.93+.07*driftTotal);

    // Subsurface cyan scattering in shadows, crevices, and grazing angles
    vec3 cyanScattering=vec3(0.65, 0.88, 0.98);
    float creviceScattering=clamp(uFrame.snowParams2.w*(1.0-clamp(geometricNormal.y,0.0,1.0)*0.6),0.0,0.85);
    snow=mix(snow,snow*cyanScattering*1.15,creviceScattering);

    if(uFrame.snowTextures.x!=0xffffffffu)
        snow*=texture(uTextures[nonuniformEXT(uFrame.snowTextures.x)],materialUV).rgb;

    // Normal perturbation with sastrugi wind ridges
    vec3 sastrugiRidge=vec3((sastrugi-0.5)*0.08*windMod, 0.0, (drift-0.5)*0.04);
    vec3 snowN=normalize(geometricNormal+vec3(drift-.5,0,
                                         snowNoise(driftUV+8.0)-.5)*.025 + sastrugiRidge);
    if(uFrame.snowTextures.y!=0xffffffffu)
        snowN=paintPerturbNormal(geometricNormal,pos,materialUV,
            texture(uTextures[nonuniformEXT(uFrame.snowTextures.y)],materialUV).xyz*2.0-1.0);

    float snowR=uFrame.snowColor.w;
    if(uFrame.snowTextures.z!=0xffffffffu)
        snowR=texture(uTextures[nonuniformEXT(uFrame.snowTextures.z)],materialUV).r;

    // Crystalline micro-specular sparkles (The Long Dark crisp snow glints)
    float sparkleScale=max(uFrame.snowParams2.y,20.0);
    vec3 sparkleCoord=pos*sparkleScale;
    float sparkleHash=paintHash13(floor(sparkleCoord));
    float sparkleCutoff=1.0-0.035*clamp(uFrame.snowParams2.x,0.0,2.0);
    float isSparkle=step(sparkleCutoff,sparkleHash);
    if(isSparkle>0.0){
        float sparkleGlint=(sparkleHash-sparkleCutoff)/max(1.0-sparkleCutoff,0.001);
        snow+=vec3(sparkleGlint*3.0*uFrame.snowParams2.x);
        snowR=mix(snowR,0.06,0.9);
    }

    albedo=mix(albedo,snow,amount);
    normal=normalize(mix(normal,snowN,amount));
    roughness=mix(roughness,clamp(snowR,.05,1.0),amount);
    return amount;
}
