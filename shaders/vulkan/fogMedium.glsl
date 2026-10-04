#ifndef FOG_MEDIUM_GLSL
#define FOG_MEDIUM_GLSL
vec3 fogGroundScatteringAlbedo() {
    float day=smoothstep(-.12,.18,uFrame.sunRadiance.w);
    return uAtmosphere.groundAlbedo.rgb*mix(vec3(1),clamp(mix(uFrame.fogNightColor.rgb,uFrame.fogDayColor.rgb,day),0.0,1.0),uAtmosphere.artistic.x);
}
float fogBlizzardExtinction(vec3 position) {
    if(uFrame.blizzardParams.x<=.001) return 0;
    float speed=uFrame.blizzardParams.y,time=uFrame.miscParams.z;
    vec3 p=(position+vec3(time*speed,time*(-speed*.2),time*(speed*.4)))*.08;
    float streak=fogNoise2D(vec2(p.x*.35+p.z*.15,p.y*2.8+p.x*.7));
    return clamp(uFrame.blizzardParams.x,0.0,1.0)*(.012+.022*streak);
}
float fogTerrainExtinction(vec3 position,float depth) {
    if(uAtmosphere.terrainField.z<=0.0||uAtmosphere.terrain.w<.5) return 0;
    vec2 uv=(position.xz-uAtmosphere.terrainField.xy)/uAtmosphere.terrainField.z;
    if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1)))) return 0;
    vec4 field=textureLod(uFogTerrain,uv,0);
    float fade=1.0-smoothstep(.85,1.0,depth/uAtmosphere.grid.w);
    float height=max(position.y-field.x,0.0);
    float ordinary=uAtmosphere.terrain.x*exp(-height*uAtmosphere.terrain.y)
        *(1.0+uAtmosphere.terrain.z*field.y)*mix(.6,1.0,field.z);
    float pool=min(max(field.w-1.0,0.0),uAtmosphere.valley.y);
    float valley=uAtmosphere.valley.z>.5?uAtmosphere.valley.x*smoothstep(0.0,1.0,pool)*
        exp(-max(height-pool,0.0)*uAtmosphere.terrain.y):0.0;
    return (ordinary+valley)*step(.5,field.w)*fade;
}
#endif
