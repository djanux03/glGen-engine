#ifndef CLOUD_SHADOW_SET
#define CLOUD_SHADOW_SET 4
#endif
layout(set=CLOUD_SHADOW_SET,binding=1) uniform sampler2D uCloudShadow;
float cloudTransmission(vec3 position) {
 if(uFrame.cloudShadowField.w<=0)return 1;
 if(position.y>=uFrame.styleCloud1.x+uFrame.styleCloud2.x)return 1;
 vec3 light=normalize(-uFrame.lightDir.xyz);
 if(light.y<=.02)return 1;
 // The table is projected onto y=0. Reproject a receiver at its real height
 // rather than sliding its shadow as hills or froxels change altitude.
 vec2 ground=position.xz-light.xz*(position.y/light.y);
 vec2 uv=(ground-uFrame.cloudShadowField.xy)/uFrame.cloudShadowField.z;
 if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1))))return 1;
 float fade=smoothstep(0,.04,min(min(uv.x,uv.y),min(1-uv.x,1-uv.y)));
 return mix(1,texture(uCloudShadow,uv).r,fade*uFrame.cloudShadowField.w);
}
