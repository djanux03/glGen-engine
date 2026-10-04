#include "fogMedium.glsl"
#define SKY_ENVIRONMENT_SET 3
#include "skyIrradiance.glsl"
// Water reflection rays begin at the water, not at the camera. Month one
// uses unshadowed analytic global transport here; terrain/local-light media
// and reflected visibility are deliberately left to the advanced-media phase.
vec3 applyReflectionAtmosphere(vec3 color,vec3 origin,vec3 ray,float distance) {
    if(uAtmosphere.history.w==0.0||distance<=0.0) return color;
    float ground=fogHeightIntegral(origin.y,ray.y,distance,uAtmosphere.ground.x,
        uAtmosphere.ground.y,uAtmosphere.ground.z)/distance;
    ground+=fogBlizzardExtinction(origin+ray*distance*.5);
    float dust=fogHeightIntegral(origin.y,ray.y,distance,uAtmosphere.dust.x,
        uAtmosphere.ground.y*uAtmosphere.dust.y,uAtmosphere.ground.z)/distance;
    float sigma=ground+dust;
    vec3 ambient=(uFrame.skyLutParams.x>.5?skyMeanRadiance():textureLod(uEnvironment,vec3(0,1,0),4).rgb)*uFrame.lightParams.x;
    float mu=dot(ray,normalize(-uFrame.lightDir.xyz));
    vec3 source=ground*fogGroundScatteringAlbedo()*(ambient+uFrame.sunRadiance.rgb*fogHG(mu,uAtmosphere.groundAlbedo.w))
        +dust*uAtmosphere.dustAlbedo.rgb*(ambient+uFrame.sunRadiance.rgb*fogHG(mu,uAtmosphere.dust.z));
    if(uAtmosphere.reference.a>=0) {sigma=uAtmosphere.reference.a;source=uAtmosphere.reference.rgb*sigma;}
    vec3 transmission=vec3(exp(-sigma*distance));
    vec3 scattering=source*fogSegmentWeight(sigma,distance);
    vec3 ahead=textureLod(uEnvironment,ray,2).rgb;
    FogSample air=fogAlongSegment(origin,origin+ray*distance,0,0,1,vec3(1),ahead,ahead);
    scattering+=transmission*air.inscatter;transmission*=air.transmittance;
    vec3 admitted=max(transmission,vec3(1-clamp(uFrame.fogParams.z,0,1)));
    scattering*=min(vec3(1),(1-admitted)/max(1-transmission,vec3(1e-5)));
    return color*admitted+scattering;
}
// Requires atmosphereData.glsl, frameData.glsl, skyModel.glsl and fog.glsl.
// Integrated values live at boundaries; final partial slices are analytic.
vec4 sampleCameraFog(vec3 position) {
    if(uAtmosphere.history.w==0.0) return vec4(0,0,0,1);
    vec3 delta=position-uAtmosphere.camera.xyz;
    float distance=length(delta);
    if(distance<1e-6) return vec4(0,0,0,1);
    if(uAtmosphere.history.w<0.0) {
        if(uAtmosphere.reference.a>=0) {
            float sigma=uAtmosphere.reference.a;
            return vec4(uAtmosphere.reference.rgb*sigma*fogSegmentWeight(sigma,distance),exp(-sigma*distance));
        }
        vec3 ray=delta/distance;
        float start=clamp(uAtmosphere.ground.w,0.0,distance);
        float ground=fogHeightIntegral(uAtmosphere.camera.y+ray.y*start,ray.y,distance-start,uAtmosphere.ground.x,uAtmosphere.ground.y,uAtmosphere.ground.z)/distance;
        ground+=fogBlizzardExtinction(uAtmosphere.camera.xyz+ray*distance*.5);
        float dust=fogHeightIntegral(uAtmosphere.camera.y,ray.y,distance,uAtmosphere.dust.x,uAtmosphere.ground.y*uAtmosphere.dust.y,uAtmosphere.ground.z)/distance;
        vec3 ambient=(uFrame.skyLutParams.x>.5?skyMeanRadiance():textureLod(uEnvironment,vec3(0,1,0),4).rgb)*uFrame.lightParams.x;
        vec3 light=uFrame.sunRadiance.rgb;
        float mu=dot(ray,normalize(-uFrame.lightDir.xyz));
        vec3 source=ground*fogGroundScatteringAlbedo()*(ambient+light*fogHG(mu,uAtmosphere.groundAlbedo.w))+
                    dust*uAtmosphere.dustAlbedo.rgb*(ambient+light*fogHG(mu,uAtmosphere.dust.z));
        return vec4(source*fogSegmentWeight(ground+dust,distance),exp(-(ground+dust)*distance));
    }
    float depth=max(-(uAtmosphere.view*vec4(position,1)).z,0.0);
    vec4 projected=uAtmosphere.viewProj*vec4(position,1);
    vec2 uv=projected.xy/max(projected.w,1e-6)*.5+.5;
    float slice=clamp(fogSlice(min(depth,uAtmosphere.grid.w)),0.0,uAtmosphere.grid.z);
    float boundary=floor(slice);
    vec4 accumulated=texture(uFogIntegrated,vec3(uv,(boundary+.5)/(uAtmosphere.grid.z+1.0)));
    if(boundary<uAtmosphere.grid.z) {
        vec4 medium=texture(uFogFiltered,vec3(uv,(boundary+.5)/uAtmosphere.grid.z));
        float partial=max(0.0,depth-fogBoundary(boundary))*distance/max(depth,1e-6);
        accumulated.rgb+=accumulated.a*medium.rgb*fogSegmentWeight(medium.a,partial);
        accumulated.a*=exp(-medium.a*partial);
    }
    // Beyond the finite grid, the global profiles continue. Terrain mist
    // is confined to the grid and fades before its far boundary.
    if(depth>uAtmosphere.grid.w) {
        vec3 ray=delta/distance;
        float startDistance=uAtmosphere.grid.w*distance/max(depth,1e-6);
        vec3 start=uAtmosphere.camera.xyz+ray*startDistance;
        float length=distance-startDistance;
        float skip=clamp(uAtmosphere.ground.w-startDistance,0.0,length);
        float tau=fogHeightIntegral(start.y+ray.y*skip,ray.y,length-skip,uAtmosphere.ground.x,uAtmosphere.ground.y,uAtmosphere.ground.z);
        float sigma=tau/max(length,1e-6);
        sigma+=fogBlizzardExtinction(start+ray*length*.5);
        float dust=fogHeightIntegral(start.y,ray.y,length,uAtmosphere.dust.x,uAtmosphere.ground.y*uAtmosphere.dust.y,uAtmosphere.ground.z)/max(length,1e-6);
        vec3 ambient=(uFrame.skyLutParams.x>.5?skyMeanRadiance():textureLod(uEnvironment,vec3(0,1,0),4).rgb)*uFrame.lightParams.x;
        vec3 source=sigma*fogGroundScatteringAlbedo()*(ambient+uFrame.sunRadiance.rgb*fogHG(dot(ray,normalize(-uFrame.lightDir.xyz)),uAtmosphere.groundAlbedo.w));
        source+=dust*uAtmosphere.dustAlbedo.rgb*(ambient+uFrame.sunRadiance.rgb*fogHG(dot(ray,normalize(-uFrame.lightDir.xyz)),uAtmosphere.dust.z));
        if(uAtmosphere.reference.a>=0) {sigma=uAtmosphere.reference.a;dust=0;source=uAtmosphere.reference.rgb*sigma;}
        accumulated.rgb+=accumulated.a*source*fogSegmentWeight(sigma+dust,length);
        accumulated.a*=exp(-(sigma+dust)*length);
    }
    return accumulated;
}
vec3 applyCameraAtmosphere(vec3 color,vec3 position,bool sky) {
    if(uAtmosphere.history.w==0.0) return color;
    vec3 direction=normalize(position-uAtmosphere.camera.xyz);
    vec4 fog=sampleCameraFog(position);
    if(sky) {
        float transmission=pow(max(fog.a,1e-8),max(uAtmosphere.artistic.y,0.0));
        fog.rgb*=min(10.0,(1.0-transmission)/max(1.0-fog.a,1e-5));
        fog.a=transmission;
    }
    // Month one retains separate spectral air. Ground density is zeroed by
    // FOG_AERIAL_ONLY in this caller, so this cannot apply ground fog twice.
    vec3 transmission=vec3(fog.a),scattering=fog.rgb;
    if(!sky) {
        if(uFrame.skyLutParams.x>.5&&uAtmosphere.reference.a<0) {
            vec4 clip=uAtmosphere.viewProj*vec4(position,1);
            vec2 uv=clip.xy/clip.w*.5+.5;vec3 air,airT;
            skySampleAerial(uv,max(-(uAtmosphere.view*vec4(position,1)).z,0),air,airT);
            float strength=max(uFrame.fogParams2.x,0); // retain the aerial artistic dial
            vec3 scaledT=pow(max(airT,vec3(1e-8)),vec3(strength));
            air*=min(vec3(10),(1-scaledT)/max(1-airT,vec3(1e-6)));
            transmission*=scaledT;scattering+=fog.a*air*uFrame.styleSkyHorizon.w;
        } else {
            vec3 ahead=textureLod(uEnvironment,direction,2).rgb;
            FogSample aerial=fogAlongView(position,0,1,vec3(1),ahead,ahead);
            transmission*=aerial.transmittance;
            scattering+=fog.a*aerial.inscatter;
        }
    }
    vec3 admittedT=max(transmission,vec3(1.0-clamp(uFrame.fogParams.z,0.0,1.0)));
    scattering*=min(vec3(1),(vec3(1)-admittedT)/max(vec3(1)-transmission,vec3(1e-5)));
    return color*admittedT+scattering;
}
