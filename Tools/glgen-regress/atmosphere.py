"""Private hidden-process atmosphere transport and performance regression.

No settings or goldens are rewritten. Probe values are actual GPU half floats
copied after compute and composition, retired by the normal frame fence.
Run synchronization validation separately from performance measurements.
"""
import argparse
import base64
import json
import math
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Tools/glgen-client'))
from glgen_client import GlGenClient
from PIL import Image


def wait_frames(c, count):
    target = c.call('engine.info')['frame'] + count
    deadline = time.monotonic() + 120
    while c.call('engine.info')['frame'] < target:
        if time.monotonic() > deadline:
            raise TimeoutError('Atmosphere test stopped advancing')
        time.sleep(.025)


def parameters(c, **values):
    c.call('render.setParams', values)
    wait_frames(c, 8)


def probe(c):
    value = c.call('render.atmosphereProbe')
    assert value and value.get('boundaries'), 'GPU probe has not retired'
    return value


def sky_checks(c, output):
    """Read all six GPU tables; compare transmission to dense independent quadrature."""
    original=c.call('render.getParams');report={'transmittance':[],'irradiance':[]}
    def reference(x,y,haze):
        bottom,top=6371.,6451.;H=math.sqrt(top*top-bottom*bottom);rho=H*y/63
        radius=math.hypot(rho,bottom);distance=(top-radius)+(x/255)*(rho+H-(top-radius))
        mu=1 if distance<1e-12 else (H*H-rho*rho-distance*distance)/(2*radius*distance)
        # Uniform fine quadrature differs from the engine's quadratic samples.
        steps=8192;tau=[0,0,0]
        for i in range(steps):
            t=distance*(i+.5)/steps;h=math.sqrt(radius*radius+t*t+2*radius*mu*t)-bottom
            r,m,o=math.exp(-max(0,h)/8.5),math.exp(-max(0,h)/1.2),max(0,1-abs(h-25)/15)
            for channel,(ray,ozone) in enumerate(zip((.005802,.013558,.0331),(.00065,.001881,.000085))):
                tau[channel]+=(ray*r+(.002+.022*haze*haze)*1.11*m+ozone*o)*distance/steps
        return [math.exp(-v) for v in tau]
    c.call('render.atmosphereProbe',{'enabled':True})
    try:
        parameters(c,fixedTime=15,temporalAA=False,camPitch=15,
                   atmosphere={'enabled':True,'physicalSky':True,'referenceExtinction':-1})
        for haze in (0,.4,1):
            parameters(c,atmosphereHaze=haze)
            actual=probe(c)['sky'];assert abs(actual['haze']-haze)<1e-6
            assert all(v['finite'] for v in actual['tables']),actual
            for index in (0,3,5):
                assert min(actual['tables'][index]['minimum'])>=0
                assert max(actual['tables'][index]['maximum'])<=1.0001
            error=0
            for sample in actual['transmittanceSamples']:
                expected=reference(*sample['texel'],haze)
                error=max(error,max(abs(a-b) for a,b in zip(sample['value'],expected)))
            assert error<.015,(haze,error)
            assert all(abs(v-1)<1e-7 for v in actual['aerialTransmission'][0]['value'])
            assert max(abs(v) for v in actual['aerialScattering'][0]['value'])==0
            report['transmittance'].append({'haze':haze,'maximumAbsoluteError':error})
        first=probe(c)['sky'];wait_frames(c,16);second=probe(c)['sky']
        assert (first['staticBuilds'],first['viewBuilds'],first['aerialBuilds'])==(second['staticBuilds'],second['viewBuilds'],second['aerialBuilds'])
        parameters(c,camYaw=original['camYaw']+20);turned=probe(c)['sky']
        assert turned['staticBuilds']==second['staticBuilds']
        assert turned['viewBuilds']==second['viewBuilds']
        assert turned['aerialBuilds']>second['aerialBuilds']
        parameters(c,sunPitch=35);lit=probe(c)['sky']
        assert lit['staticBuilds']==turned['staticBuilds'] and lit['viewBuilds']>turned['viewBuilds']
        report['rebuildRules']=True
        for name,pitch,haze in (('clear_noon',60,0),('hazy_sunset',3,1),('twilight',-8,.4),('night',-25,.4)):
            parameters(c,sunPitch=pitch,atmosphereHaze=haze,camPitch=18,camYaw=215)
            assert all(v['finite'] for v in probe(c)['sky']['tables'])
            sh=probe(c)['skyIrradiance'];assert sh['finite'],sh
            # Compare band-2 Lambert convolution to an independent direct
            # cosine integral, normalized to the scene's brightest direction.
            peak=max(max(v['quadrature']) for v in sh['diffuse']);error=max(
                abs(a-b) for v in sh['diffuse'] for a,b in zip(v['harmonic'],v['quadrature']))/max(peak,1e-6)
            assert error<.08,(name,error,sh)
            report['irradiance'].append({'scene':name,'maximumPeakRelativeError':error,'gpu':sh})
            c.call('render.capture',{'path':str(output/(name+'.png')),'maxDimension':1920})
        report['tables']=probe(c)['sky']
    finally:
        c.call('render.atmosphereProbe',{'enabled':False});c.call('render.setParams',original)
        (output/'sky_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def cloud_shadow_checks(c, output):
    original=c.call('render.getParams');report={}
    terrain_exists=c.eval('return terrain.exists()')
    c.call('render.atmosphereProbe',{'enabled':True})
    try:
        parameters(c,fixedTime=15,sunPitch=60,cloudCoverage=0,cloudStrength=1,
                   paintedClouds=False,atmosphere={'enabled':True,'cloudShadows':True})
        clear=probe(c)['cloudShadow'];assert clear['finite'] and clear['minimum']>.999,clear
        parameters(c,cloudCoverage=1)
        dense=probe(c)['cloudShadow']
        assert dense['finite'] and 0<=dense['minimum']<.95 and dense['maximum']<=1,dense
        c.call('render.capture',{'path':str(output/'cloud_shadows_on.png'),'maxDimension':1920})
        parameters(c,atmosphere={'cloudShadows':False})
        c.call('render.capture',{'path':str(output/'cloud_shadows_off.png'),'maxDimension':1920})
        report={'clear':clear,'dense':dense,'bounded':True,'switching':True}
        assert c.call('render.getParams')['atmosphere']['cloudShadows'] is False
        if terrain_exists:c.eval('terrain.destroy()')
        c.eval('cloud_shadow_qa=world.spawn("__primitive_cube",{pos={0,1000,0},scale={100,100,.01},name="__cloud_shadow_qa"})')
        parameters(c,camPos=[0,1000,50],camYaw=180,camPitch=0,sunYaw=180,sunPitch=60,
          sunIntensity=4,ambientIntensity=0,pointLights=[],water=False,temporalAA=False,
          fogAerialStrength=0,fogNoiseStrength=0,fogMaxOpacity=1,cloudCoverage=1,
          cloudDensityMultiplier=.1,atmosphere={'cloudShadows':False,'physicalSky':False,
          'history':False,'terrainMist':False,'dustExtinction':0,'groundExtinction':0,
          'groundFalloff':0,'start':0,'referenceExtinction':-1,'referenceSceneRadiance':-1})
        surface_clear=probe(c)['hdr']
        parameters(c,atmosphere={'cloudShadows':True});surface_shadow=probe(c)['hdr']
        assert sum(surface_clear)>0 and sum(surface_shadow)<sum(surface_clear)*.8,(surface_clear,surface_shadow)
        parameters(c,atmosphere={'cloudShadows':False,'groundExtinction':.005,'directionalShadows':False,'skyVisibility':False})
        fog_clear=probe(c)['medium'][4][:3]
        parameters(c,atmosphere={'cloudShadows':True});fog_shadow=probe(c)['medium'][4][:3]
        assert sum(fog_clear)>0 and sum(fog_shadow)<sum(fog_clear)*.8,(fog_clear,fog_shadow)
        report['surfaceLighting']={'clear':surface_clear,'shadow':surface_shadow}
        report['fogLighting']={'clear':fog_clear,'shadow':fog_shadow}
    finally:
        c.eval('if cloud_shadow_qa then cloud_shadow_qa:destroy() cloud_shadow_qa=nil end')
        if terrain_exists and not c.eval('return terrain.exists()'):c.eval('terrain.regenerate()')
        c.call('render.atmosphereProbe',{'enabled':False});c.call('render.setParams',original)
        (output/'cloud_shadow_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def height_integral(y, dy, distance, density, falloff, reference):
    # Independent midpoint quadrature, deliberately not the engine formula.
    count = 20000
    return sum(density * math.exp(-max(0, y + dy*(i+.5)*distance/count-reference)*falloff)
               for i in range(count)) * distance/count


def exposure_checks(c, output):
    original=c.call('render.getParams');report={};exists=c.eval('return terrain.exists()')
    if exists:c.eval('terrain.destroy()')
    c.eval('exposure_qa=world.spawn("__primitive_cube",{pos={0,1000,0},scale={100,100,.005},name="__exposure_qa"})')
    try:
        parameters(c,camPos=[0,1000,50],camYaw=180,camPitch=0,water=False,temporalAA=False,
          autoExposure=True,autoExposureSpeed=1,pointLights=[],sunIntensity=0,ambientIntensity=0,
          atmosphere={'enabled':True,'physicalSky':False,'history':False,'terrainMist':False,
          'groundExtinction':0,'dustExtinction':0,'referenceExtinction':0,
          'histogramExposure':True,'exposureKey':.18,'exposureMin':.001,'exposureMax':64})
        for level in (.125,1,8):
            parameters(c,atmosphere={'referenceSceneRadiance':level});wait_frames(c,8)
            stats=c.eval('return render.stats()');params=c.call('render.getParams')
            assert abs(stats['meteredLuminance']-level)<level*.06,stats
            assert abs(params['exposure']*level-.18)<.18*.06,params['exposure']
            report[str(level)]={'meteredLuminance':stats['meteredLuminance'],'exposure':params['exposure']}
        parameters(c,atmosphere={'referenceSceneRadiance':0});wait_frames(c,8)
        assert c.call('render.getParams')['exposure']==64
        parameters(c,autoExposure=False,exposure=.73,atmosphere={'referenceSceneRadiance':1})
        wait_frames(c,8);assert abs(c.call('render.getParams')['exposure']-.73)<1e-6
        report['blackBoundedAndManualStable']=True
    finally:
        c.eval('if exposure_qa then exposure_qa:destroy() exposure_qa=nil end')
        if exists:c.eval('terrain.regenerate()')
        c.call('render.setParams',original)
        (output/'exposure_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def cloud_history_checks(c, output):
    original=c.call('render.getParams');report={}
    c.call('render.atmosphereProbe',{'enabled':True})
    try:
        parameters(c,fixedTime=15,temporalAA=False,autoExposure=False,camPitch=40,camYaw=135,
                   cloudCoverage=.6,paintedClouds=False,cloudStrength=1,
                   atmosphere={'enabled':True,'cloudHistory':True})
        wait_frames(c,32);rows=[]
        for _ in range(32):
            wait_frames(c,1);value=probe(c)['cloudHistory'];assert value['finite'];rows.append(value)
        assert all(r['reused'] for r in rows), 'Still-camera cloud history was unexpectedly reset'
        def deviation(key):
            return math.sqrt(sum(statistics.pvariance(r['samples'][i][key][channel] for r in rows)
                for i in range(64) for channel in range(4))/256)
        raw,filtered=deviation('raw'),deviation('filtered')
        assert raw>0 and filtered<raw*.5,(raw,filtered)
        assert any(v['depth']>0 for v in rows[-1]['samples']), 'No scattering depth was written'
        assert all(v['depth']==v['historyDepth'] for v in rows[-1]['samples'])
        report['stillNoise']={'raw':raw,'filtered':filtered,'reduction':1-filtered/raw}
        c.call('render.capture',{'path':str(output/'cloud_history_still.png'),'maxDimension':1920})
        first=probe(c)['cloudHistory']['resets'];parameters(c,camYaw=260)
        assert probe(c)['cloudHistory']['resets']>first
        report['cutResets']=True
        for i in range(8):
            c.call('render.setParams',{'camYaw':260+i*.4,'fixedTime':15+i/30});wait_frames(c,4)
            assert probe(c)['cloudHistory']['finite']
        c.call('render.capture',{'path':str(output/'cloud_history_moving.png'),'maxDimension':1920})
        parameters(c,cloudCoverage=0)
        clear=probe(c)['cloudHistory']
        assert all(v['depth']==0 and v['filtered']==[0,0,0,1] for v in clear['samples']),clear
        report['clearRejectsHistory']=True
        parameters(c,cloudCoverage=.6,atmosphere={'cloudHistory':False})
        assert all(v['raw']==v['filtered'] for v in probe(c)['cloudHistory']['samples'])
        report['disabledCopiesRaw']=True
    finally:
        c.call('render.atmosphereProbe',{'enabled':False});c.call('render.setParams',original)
        (output/'cloud_history_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def transport_checks(c, output):
    original = c.call('render.getParams')
    report = {'homogeneous': [], 'height': []}
    terrain_exists=c.eval('return terrain.exists()')
    if terrain_exists:c.eval('terrain.destroy()')
    c.eval('atmosphere_qa=world.spawn("__primitive_cube",{pos={0,1000,0},scale={100,100,.005},name="__atmosphere_qa"})')
    c.call('render.atmosphereProbe', {'enabled': True})
    try:
        parameters(c, camPos=[0,1000,50], camYaw=180, camPitch=0,
                   fixedTime=15, temporalAA=False, autoExposure=False,
                   water=False, pointLights=[], sunIntensity=0, ambientIntensity=0,
                   fogNoiseStrength=0, fogAerialStrength=0, fogStart=0, fogMaxOpacity=1,
                   atmosphere={'terrainMist': False, 'directionalShadows': False,
                               'skyVisibility': False, 'history': False, 'range':180})
        for quality in (0,1):
            for sigma in (0,1e-10,1e-6,.001,.02,.1):
                parameters(c, atmosphere={'quality':quality, 'referenceExtinction':sigma,
                                          'referenceRadiance':[.7,.5,.2]})
                actual = probe(c)
                error = 0
                for row in actual['boundaries']:
                    distance = row['depth']*actual['depthScale']
                    expected_t = math.exp(-sigma*distance)
                    error = max(error, abs(row['transmittance']-expected_t))
                    for radiance,value in zip((.7,.5,.2),row['scattering']):
                        expected = radiance*(-math.expm1(-sigma*distance))
                        assert math.isfinite(value)
                        error = max(error, abs(value-expected)/max(.01,expected))
                assert error < .01, (quality,sigma,error)
                report['homogeneous'].append({'quality':quality,'extinction':sigma,'maximumError':error})
        parameters(c, atmosphere={'quality':0,'referenceExtinction':-1,'groundExtinction':.02,
                                  'groundFalloff':.08,'groundReference':1000,'dustExtinction':0})
        for y,pitch in ((990,0),(1010,0),(990,30),(1010,-30),(1000,0)):
            parameters(c,camPos=[0,y,50],camPitch=pitch)
            actual=probe(c)
            row=actual['boundaries'][-1]
            distance=row['depth']*actual['depthScale']
            tau=height_integral(y,actual['direction'][1],distance,.02,.08,1000)
            expected=math.exp(-tau)
            error=abs(row['transmittance']-expected)/max(expected,.01)
            assert error<.03,(y,pitch,error)
            report['height'].append({'y':y,'pitch':pitch,'relativeError':error})
        # The thin target is at 50 m. A whole-slice lookup or fog behind it
        # produces a noticeably different HDR value from partial integration.
        parameters(c,camPos=[0,1000,50],camPitch=0,
                   atmosphere={'referenceExtinction':.01,'referenceRadiance':[.7,.7,.7],
                               'referenceSceneRadiance':1,'history':False})
        actual=probe(c)
        expected=math.exp(-.01*50)+.7*(1-math.exp(-.01*50))
        assert max(abs(v-expected) for v in actual['hdr'])<.01,actual['hdr']
        report['opaquePartialSlice']={'actual':actual['hdr'],'expected':expected}
        # Water intersects near 50 m, while the opaque target is moved behind
        # it. A depth-prepass lookup would fog the water to the target instead.
        c.eval('atmosphere_qa:set_position(0,1000,-30)')
        parameters(c,water=True,waterLevel=1000,camPos=[0,1003,50],camPitch=-math.degrees(math.atan(3/50)))
        actual=probe(c)
        expected=math.exp(-.01*math.hypot(3,50))+.7*(1-math.exp(-.01*math.hypot(3,50)))
        assert max(abs(v-expected) for v in actual['hdr'])<.015,actual['hdr']
        report['actualWaterHit']={'actual':actual['hdr'],'expected':expected}
        # Actual emissive raw scene, without the diagnostic colour override.
        # Nearly clear water and a flat normal make the Fresnel mix analytic.
        # Fogging the refraction copy first would darken this a second time.
        c.eval('atmosphere_qa:set_material{color={0,0,0},emissive={1,1,1},emissiveStrength=1}')
        parameters(c,waveAmplitude=0,waterClarity=1e6,waterReflectionStrength=0,
          waterFoamStrength=0,atmosphere={'referenceSceneRadiance':-1})
        actual=probe(c)
        nv=3/math.hypot(3,50)
        fresnel=.02+.98*(1-nv)**5
        raw=(1-fresnel)+fresnel*.7
        t=math.exp(-.01*math.hypot(3,50))
        expected=raw*t+.7*(1-t)
        assert max(abs(v-expected) for v in actual['hdr'])<.008,(actual['hdr'],expected)
        report['unfoggedRefraction']={'actual':actual['hdr'],'expected':expected}
        parameters(c,water=False,atmosphere={'referenceExtinction':0,'referenceSceneRadiance':2,
                                            'bloomFireflySuppression':False,'bloomStrength':.04})
        actual=probe(c)
        error=max(abs(a-b)/max(a,.01) for a,b in zip(actual['hdr'],actual['bloom']))
        assert error<.01,actual
        report['uniformBloomEnergyError']=error
        parameters(c, fogDensity=.09, atmosphere={'groundExtinction':.004,'historyWeight':4})
        settings=c.call('render.getParams')
        assert abs(settings['fogDensity']-.004)<1e-7
        assert abs(settings['atmosphere']['historyWeight']-.9)<1e-6
        parameters(c,**settings)
        assert c.call('render.getParams')['atmosphere']==settings['atmosphere']
        report['nestedPrecedenceAndRoundTrip']=True
        for quality in (1,0,1,0):
            parameters(c, atmosphere={'quality':quality,'enabled':False})
            parameters(c, atmosphere={'enabled':True})
        report['qualityAndDisableSwitching']=True
        (output/'transport_probe.json').write_text(json.dumps(probe(c),indent=2)+'\n')
    finally:
        (output/'transport_checks.json').write_text(json.dumps(report,indent=2)+'\n')
        c.eval('if atmosphere_qa then atmosphere_qa:destroy() atmosphere_qa=nil end')
        c.call('render.atmosphereProbe',{'enabled':False})
        if terrain_exists:c.eval('terrain.regenerate()')
        c.call('render.setParams',original)
    return report


def engine_cpu_seconds(process):
    if os.name!='nt':return None
    import ctypes
    import ctypes.wintypes as wt
    kernel32=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel32.GetProcessTimes.argtypes=[wt.HANDLE]+[ctypes.POINTER(wt.FILETIME)]*4
    times=[wt.FILETIME() for _ in range(4)]
    if not kernel32.GetProcessTimes(process._handle,*(ctypes.byref(t) for t in times)):
        raise ctypes.WinError(ctypes.get_last_error())
    return sum((t.dwHighDateTime<<32)|t.dwLowDateTime for t in times[2:])/1e7


def measure(c, output, process):
    result={}
    for name,x,z,yaw,pitch in (('shore',22,-19,190,-3),('marsh',-188,-51,150,-5)):
        y=c.eval(f'return terrain.height_at({x},{z})')+1.65
        parameters(c,camPos=[x,y,z],camYaw=yaw,camPitch=pitch,
                   atmosphere={'quality':0,'enabled':True,'referenceExtinction':-1,
                               'referenceSceneRadiance':-1})
        wait_frames(c,120)
        begin_cpu=engine_cpu_seconds(process)
        begin_frame=c.call('engine.info')['frame']
        rows=[]
        for _ in range(60):
            wait_frames(c,1)
            rows.append(c.eval('return render.stats()'))
        sampled_frames=c.call('engine.info')['frame']-begin_frame
        end_cpu=engine_cpu_seconds(process)
        costs=[sum(row[key] for key in ('gpuFogVisibilityMs','gpuFogInjectionMs',
                                       'gpuFogHistoryMs','gpuFogIntegrationMs')) for row in rows]
        # Water evaluates fog in its material shader. Counting its entire
        # pass is a conservative upper bound, including pre-existing waves,
        # SSR and lighting; it cannot hide camera/reflection fog cost.
        upper=[cost+row['gpuWaterMs'] for cost,row in zip(costs,rows)]
        ordered=sorted(upper)
        result[name]={'fogMedianMs':statistics.median(costs),'fogP95Ms':sorted(costs)[math.ceil(.95*len(costs))-1],
          'fogIncludingWaterUpperMedianMs':statistics.median(upper),
          'fogIncludingWaterUpperP95Ms':ordered[math.ceil(.95*len(ordered))-1],
          'fogVolumeAndOpaqueP95Ms':sorted(costs)[math.ceil(.95*len(costs))-1],
          'waterMedianMs':statistics.median(row['gpuWaterMs'] for row in rows),
          'bloomMedianMs':statistics.median(row['gpuBloomMs'] for row in rows),
          'skyLutsMedianMs':statistics.median(row['gpuSkyLutsMs'] for row in rows),
          'cloudShadowMedianMs':statistics.median(row['gpuCloudShadowMs'] for row in rows),
          'fullFrameMedianMs':statistics.median(row['gpuTotalMs'] for row in rows),
          'cpuFogMedianMs':statistics.median(row['cpuFogMs'] for row in rows),
          'cpuBloomMedianMs':statistics.median(row['cpuBloomMs'] for row in rows),
          'engineCpuMsPerFrame':None if begin_cpu is None else (end_cpu-begin_cpu)*1000/max(sampled_frames,1),
          'atmosphereBytes':max(row['atmosphereAllocatedBytes'] for row in rows),'samples':rows}
        c.call('render.capture',{'path':str(output/f'{name}.png'),'maxDimension':1920})
        with Image.open(output/f'{name}.png') as capture:
            assert capture.size==(1920,1080),f'Incorrect benchmark resolution: {capture.size}'
    return result


def cutout_fixture(output):
    """A real imported alpha-masked card exercises CPU RT micromaps."""
    directory=output/'cutout_fixture';directory.mkdir(exist_ok=True)
    image=Image.new('RGBA',(32,4),(70,180,40,0))
    for x in range(16,32):
        for y in range(4):image.putpixel((x,y),(70,180,40,255))
    image.save(directory/'cutout.png')
    binary=struct.pack('<12f12f8f6H',-2,0,-2,2,0,-2,2,0,2,-2,0,2,
        *((0,1,0)*4),0,0,1,0,1,1,0,1,0,2,1,0,3,2)
    gltf={'asset':{'version':'2.0'},'scene':0,'scenes':[{'nodes':[0]}],
      'nodes':[{'mesh':0}], 'buffers':[{'byteLength':len(binary),'uri':'data:application/octet-stream;base64,'+base64.b64encode(binary).decode()}],
      'bufferViews':[{'buffer':0,'byteOffset':offset,'byteLength':length} for offset,length in ((0,48),(48,48),(96,32),(128,12))],
      'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3','min':[-2,0,-2],'max':[2,0,2]},
                   {'bufferView':1,'componentType':5126,'count':4,'type':'VEC3'},
                   {'bufferView':2,'componentType':5126,'count':4,'type':'VEC2'},
                   {'bufferView':3,'componentType':5123,'count':6,'type':'SCALAR'}],
      'images':[{'uri':'cutout.png'}],'textures':[{'source':0}],
      'materials':[{'doubleSided':True,'alphaMode':'MASK','alphaCutoff':.5,
        'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'roughnessFactor':1,'metallicFactor':0}}],
      'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},'indices':3,'material':0}]}]}
    path=directory/'cutout.gltf';path.write_text(json.dumps(gltf))
    return path.as_posix()


def light_and_history_checks(c, output):
    original=c.call('render.getParams')
    existed=c.eval('return terrain.exists()')
    if existed:c.eval('terrain.destroy()')
    c.eval('atmosphere_lights={} local e=world.spawn("__primitive_plane",{pos={0,1000,0},scale={16,1,16},name="__atmosphere_lamp_floor"}) table.insert(atmosphere_lights,e)')
    c.call('render.atmosphereProbe',{'enabled':True})
    report={}

    def column():
        value=probe(c)
        return [sum(row[:3])/3 for row in value['medium']]

    def noise():
        rows=[]
        for _ in range(32):
            wait_frames(c,1);rows.append(column())
        return math.sqrt(sum(statistics.pvariance(row[z] for row in rows)
                             for z in range(len(rows[0])))/len(rows[0]))

    try:
        light={'position':[-3,1004,1],'color':[1,.8,.6],'radius':20,'intensity':80,
               'volumetricParticipation':1,'volumetricShadows':True}
        parameters(c,camPos=[0,1002,12],camYaw=180,camPitch=0,sunIntensity=0,
                   ambientIntensity=0,water=False,temporalAA=False,autoExposure=False,
                   fixedTime=15,fogAerialStrength=0,fogNoiseStrength=0,
                   pointLights=[light],atmosphere={'enabled':True,'quality':0,'range':40,
                   'history':False,'groundExtinction':.03,'groundFalloff':0,
                   'groundReference':1000,'start':0,'groundAnisotropy':0,
                   'dustExtinction':0,'terrainMist':False,'skyVisibility':False,
                   'directionalShadows':False,'referenceExtinction':-1,'referenceSceneRadiance':-1})
        p=probe(c)
        centres=[(p['boundaries'][z]['depth']+p['boundaries'][z+1]['depth'])*.5 for z in range(len(p['medium']))]
        # Equal-distance emitter positions isolate the actual shader phase.
        phase_cell=min(range(len(centres)),key=lambda z:abs(centres[z]-6))
        phase_z=12-centres[phase_cell]
        phase_light=dict(light,position=[0,1002,phase_z-3],volumetricShadows=False)
        parameters(c,pointLights=[phase_light],atmosphere={'groundAnisotropy':.55})
        forward=column()[phase_cell]
        phase_light['position']=[0,1002,phase_z+3]
        parameters(c,pointLights=[phase_light])
        backward=column()[phase_cell]
        assert forward>backward*10,(forward,backward)
        report['gpuForwardPhaseRatio']=forward/max(backward,1e-9)
        parameters(c,pointLights=[light],atmosphere={'groundAnisotropy':0})
        cell=min(range(len(centres)),key=lambda z:abs(centres[z]-12))
        open_value=column()[cell]
        assert open_value>1e-6
        c.eval('atmosphere_blocker=world.spawn("__primitive_cube",{pos={-1.5,1003,.5},scale={1,1,1},name="__fog_blocker"})')
        wait_frames(c,16);blocked=column()[cell]
        assert blocked<open_value*.5,(open_value,blocked)
        c.eval('atmosphere_blocker:set_position(-4.5,1005,1.5)')
        wait_frames(c,16);behind=column()[cell]
        assert abs(behind-open_value)/open_value<.1,(open_value,behind)
        report['emitterBoundedFogShadows']={'unblocked':open_value,'blocked':blocked,'behindEmitter':behind}
        c.eval('atmosphere_blocker:destroy() atmosphere_blocker=nil')
        card=cutout_fixture(output)
        c.eval('atmosphere_cutout=world.spawn('+json.dumps(card)+', {pos={-2.5,1003,0},name="__fog_cutout"})')
        wait_frames(c,24);solid=column()[cell]
        c.eval('atmosphere_cutout:set_position(-.5,1003,0)')
        wait_frames(c,24);gap=column()[cell]
        assert solid<open_value*.2 and gap>open_value*.7,(open_value,solid,gap)
        report['alphaMaskedFoliageGaps']={'solid':solid,'gap':gap,'unblocked':open_value}
        c.eval('atmosphere_cutout:destroy() atmosphere_cutout=nil')
        parameters(c,atmosphere={'groundAnisotropy':.55,'history':False})
        off=noise()
        parameters(c,atmosphere={'history':True})
        wait_frames(c,32);on=noise()
        assert off>0 and on<=off*.5,(off,on)
        report['historyNoise']={'off':off,'on':on,'reduction':1-on/off}
        parameters(c,pointLights=[])
        assert max(column())<1e-5,'Changed local light left persistent fog illumination'
        report['lightTrailClearsWithinEightFrames']=True
        parameters(c,pointLights=[light],camPos=[0,1002,-80],camYaw=0)
        assert max(column())<1e-5,'Teleport reused illumination from the old light region'
        report['teleportRejectsHistory']=True
        c.eval('for _,v in ipairs({{-8,1003,0,.2,6,16},{8,1003,0,.2,6,16},{0,1003,-8,16,6,.2},{0,1006,0,16,.2,16}}) do '
               'local e=world.spawn("__primitive_cube",{pos={v[1],v[2],v[3]},scale={v[4],v[5],v[6]},name="__fog_room"}) '
               'e:set_material{color={.3,.32,.35},roughness=.8} table.insert(atmosphere_lights,e) end')
        parameters(c,camPos=[0,1002,12],camYaw=180,pointLights=[
          {'position':[x,1003,z],'color':color,'radius':20,'intensity':100,
           'volumetricParticipation':1,'volumetricShadows':True}
          for x,z,color in ((-3,2,[1,.65,.3]),(3,2,[.3,.6,1]),(-3,-3,[1,.4,.2]),(3,-3,[.4,1,.6]))])
        wait_frames(c,40)
        c.call('render.atmosphereProbe',{'enabled':False})
        samples=[]
        for _ in range(60):wait_frames(c,1);samples.append(c.eval('return render.stats()'))
        costs=[sum(row[k] for k in ('gpuFogVisibilityMs','gpuFogInjectionMs','gpuFogHistoryMs','gpuFogIntegrationMs')) for row in samples]
        report['fourLightPerformance']={'fogMedianMs':statistics.median(costs),'fogP95Ms':sorted(costs)[56],
            'bloomMedianMs':statistics.median(row['gpuBloomMs'] for row in samples),
            'atmosphereBytes':max(row['atmosphereAllocatedBytes'] for row in samples),
            'fullFrameMedianMs':statistics.median(row['gpuTotalMs'] for row in samples)}
        c.call('render.capture',{'path':str(output/'four_lights.png'),'maxDimension':1920})
        # A resolved emissive point translated by subpixel increments. Raster
        # coverage can change slightly; normalized bloom must not add a pulse.
        parameters(c,pointLights=[],skyBrightness=0,nightSkyBrightness=0,
          starIntensity=0,moonIntensity=0,moonGlowIntensity=0,sunDiscIntensity=0,
          iblSpecularIntensity=0,cloudDensityMultiplier=0,cloudCoverage=0,
          cameraGrade=False,exposure=1,fxaaEnabled=False,
          atmosphere={'enabled':False,'bloomStrength':.04,'bloomFireflySuppression':False})
        c.eval('atmosphere_bright=world.spawn("__primitive_cube",{pos={0,1002,0},scale={.3,.3,.01},name="__bloom_point"}) '
               'atmosphere_bright:set_material{color={0,0,0},emissive={1,1,1},emissiveStrength=3}')
        energies=[]
        for i in range(8):
            c.eval(f'atmosphere_bright:set_position({(i-3.5)*.003},1002,0)')
            wait_frames(c,8)
            path=output/f'bloom_point_{i:02}.png'
            c.call('render.capture',{'path':str(path),'maxDimension':1920})
            with Image.open(path) as im:
                from PIL import ImageStat
                energies.append(sum(ImageStat.Stat(im.convert('RGB')).sum))
        pulse=(max(energies)-min(energies))/max(statistics.mean(energies),1)
        assert pulse<.05,pulse
        report['movingBloomPointEnergyVariation']=pulse
    finally:
        c.eval('if atmosphere_bright then atmosphere_bright:destroy() atmosphere_bright=nil end if atmosphere_cutout then atmosphere_cutout:destroy() atmosphere_cutout=nil end if atmosphere_blocker then atmosphere_blocker:destroy() atmosphere_blocker=nil end if atmosphere_lights then for _,e in ipairs(atmosphere_lights) do e:destroy() end atmosphere_lights=nil end')
        c.call('render.atmosphereProbe',{'enabled':False})
        if existed:c.eval('terrain.regenerate()')
        c.call('render.setParams',original)
        (output/'light_history_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def terrain_checks(c, output):
    original=c.call('render.getParams');report={}
    assert c.eval('return terrain.exists()')
    c.call('render.atmosphereProbe',{'enabled':True})
    try:
        y=c.eval('return terrain.height_at(22,-19)')+1.65
        parameters(c,camPos=[22,y,-19],camPitch=0,camYaw=190,water=False,
          fogAerialStrength=0,fogNoiseStrength=0,atmosphere={'history':False,
          'referenceExtinction':-1,'referenceSceneRadiance':-1,'groundExtinction':0,
          'dustExtinction':0,'terrainMist':True,'terrainExtinction':.02,
          'terrainFalloff':.4,'waterBoost':0,'valleyPooling':False})
        wait_frames(c,64)
        before=probe(c);assert before['terrainField'][2]>0
        density=before['medium'][0][3];assert density>0
        # Multiple edits can overtake a running snapshot. Publication must
        # describe the newest height, not the first completed worker job.
        for _ in range(3):
            c.eval('assert(terrain.brush_height(22,-19,64,.3,false))')
            wait_frames(c,1)
        deadline=time.monotonic()+90
        while True:
            wait_frames(c,8);after=probe(c)
            if after['terrainRevision']>before['terrainRevision'] and after['terrainField'][2]>0 and after['medium'][0][3]>density*1.35:break
            if time.monotonic()>deadline:raise TimeoutError('Terrain edit was not published to fog')
        # Three 0.3 m edits imply exp(0.9 * 0.4) ~= 1.43. One/two stale
        # edits would give 1.13/1.27; interpolation slightly reduces the lift.
        ratio=after['medium'][0][3]/density
        assert 1.35<ratio<1.52,(density,after['medium'][0][3])
        report['editedField']={'beforeRevision':before['terrainRevision'],
          'afterRevision':after['terrainRevision'],'beforeDensity':density,'afterDensity':after['medium'][0][3]}
        c.eval('terrain.regenerate()');wait_frames(c,96)
        restored=probe(c);assert restored['terrainRevision']>after['terrainRevision']
        assert abs(restored['medium'][0][3]-density)<density*.1
        report['regenerationRestoresField']=True
        # Form a closed basin through the same main-thread edit path used by
        # the editor; its spill-height potential must appear in GPU extinction.
        prior=restored['terrainRevision']
        c.eval('assert(terrain.brush_height(22,-19,48,6,true))')
        c.eval('assert(terrain.brush_height(22,-19,48,6,true))')
        basin_y=c.eval('return terrain.height_at(22,-19)')+4
        parameters(c,camPos=[22,basin_y,-19],atmosphere={'terrainExtinction':0,
          'valleyPooling':True,'valleyExtinction':.02,'valleyDepth':16})
        deadline=time.monotonic()+90
        while True:
            wait_frames(c,8);pooled=probe(c)
            if pooled['terrainRevision']>prior and pooled['medium'][0][3]>.005:break
            if time.monotonic()>deadline:raise TimeoutError('Basin potential was not published to fog')
        # The transport probe sits inside the basin. Review the occupied layer
        # from its rim as well, so the image shows its relationship to terrain.
        wait_frames(c,64)
        c.call('render.capture',{'path':str(output/'valley_pooling_probe.png'),'maxDimension':1920})
        rim_y=c.eval('return terrain.height_at(78,-19)')+6
        pitch=math.degrees(math.atan2(basin_y-4-rim_y,56))
        parameters(c,camPos=[78,rim_y,-19],camYaw=270,camPitch=pitch);wait_frames(c,64)
        c.call('render.capture',{'path':str(output/'valley_pooling.png'),'maxDimension':1920})
        parameters(c,atmosphere={'valleyPooling':False});unpooled=probe(c)['medium'][0][3]
        assert unpooled<1e-6,unpooled
        report['valleyPooling']={'enabledExtinction':pooled['medium'][0][3],'disabledExtinction':unpooled}
        c.eval('terrain.regenerate()');wait_frames(c,96)
    finally:
        c.call('render.atmosphereProbe',{'enabled':False});c.call('render.setParams',original)
        (output/'terrain_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def lifecycle_checks(c, process, output, swamp_only=False):
    """Only resize the private hidden process; never activate its window."""
    original=c.call('render.getParams');scenery=c.eval('return render.scenery_id()')
    report={'resize':[],'scenery':[],'playStopCycles':0}
    snapshot_path=output/'private_play_snapshot.json'
    saved_snapshot=snapshot_path.read_bytes() if snapshot_path.exists() else None
    if os.name!='nt':raise RuntimeError('Hidden resize test currently requires Windows')
    import ctypes
    import ctypes.wintypes as wt
    user32=ctypes.WinDLL('user32',use_last_error=True)
    callback=ctypes.WINFUNCTYPE(wt.BOOL,wt.HWND,wt.LPARAM)
    windows=[]
    user32.GetWindowThreadProcessId.argtypes=[wt.HWND,ctypes.POINTER(wt.DWORD)]
    user32.GetWindowTextW.argtypes=[wt.HWND,wt.LPWSTR,ctypes.c_int]
    user32.IsWindowVisible.argtypes=[wt.HWND]
    @callback
    def visit(hwnd,_):
        pid=wt.DWORD();user32.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        title=ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd,title,len(title))
        if pid.value==process.pid and title.value=='glGen (Vulkan)':windows.append(hwnd)
        return True
    user32.EnumWindows(visit,0)
    assert windows,'Private engine window not found'
    hwnd=windows[0]
    user32.SetWindowPos.argtypes=[wt.HWND,wt.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_uint]
    c.call('render.atmosphereProbe',{'enabled':True})
    try:
        for width,height in ((960,540),(1280,720),(1920,1080),(960,540),(1920,1080)):
            assert user32.SetWindowPos(hwnd,None,0,0,width,height,0x0010|0x0004|0x0002)
            wait_frames(c,24)
            actual=probe(c)['viewport'];assert actual==[width,height],actual
            report['resize'].append(actual)
        maps=('woodland_swamp',)*5 if swamp_only else ('meadows','bleak_winter','woodland_swamp','bleak_winter','woodland_swamp')
        for name in maps:
            c.eval(f'assert(render.scenery("{name}"))');wait_frames(c,64)
            assert c.eval('return render.scenery_id()')==name
            for quality in (1,0):
                parameters(c,atmosphere={'quality':quality,'enabled':False})
                parameters(c,atmosphere={'enabled':True})
            report['scenery'].append(name)
        for _ in range(3):
            # A fresh private scene deliberately has no gameplay actor.
            c.eval('game.spawn_player{pos={22,0,-19},onGround=true}')
            c.eval('game.play()');wait_frames(c,24);assert c.eval('return game.is_playing()')
            c.eval('game.stop()');wait_frames(c,24);assert not c.eval('return game.is_playing()')
            report['playStopCycles']+=1
        assert not user32.IsWindowVisible(hwnd),'Regression window became visible'
    finally:
        c.call('render.atmosphereProbe',{'enabled':False})
        c.eval(f'assert(render.scenery("{scenery}"))');c.call('render.setParams',original)
        if saved_snapshot is None:snapshot_path.unlink(missing_ok=True)
        else:snapshot_path.write_bytes(saved_snapshot)
        (output/'lifecycle_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def visual_checks(c, output, swamp_only=False):
    original=c.call('render.getParams');scenery=c.eval('return render.scenery_id()')
    report={}
    def capture(name,x,z,above,yaw,pitch,**settings):
        y=c.eval(f'return terrain.height_at({x},{z})')+above
        parameters(c,camPos=[x,y,z],camYaw=yaw,camPitch=pitch,**settings);wait_frames(c,96)
        c.call('render.capture',{'path':str(output/(name+'.png')),'maxDimension':1920})
        report[name]={'camera':[x,y,z,yaw,pitch],'settings':settings}
    try:
        c.eval('assert(render.scenery("woodland_swamp"))')
        capture('swamp_daylight',22,-19,1.65,190,-3,sunPitch=32,sunYaw=225)
        capture('canopy_low_sun',-18,24,2,150,8,sunPitch=8,sunYaw=150)
        capture('rocky_rise',118,10,1.7,145,15)
        capture('open_sky',22,-19,18,215,24)
        capture('night',22,-19,1.65,190,-3,sunPitch=-25)
        capture('moving_start',22,-19,1.65,190,-3,sunPitch=12)
        for i in range(8):
            x=22+i*.25;y=c.eval(f'return terrain.height_at({x},-19)')+1.65
            c.call('render.setParams',{'camPos':[x,y,-19]});wait_frames(c,1)
            c.call('render.capture',{'path':str(output/f'moving_{i:02}.png'),'maxDimension':1920})
        if not swamp_only:
            c.eval('assert(render.scenery("bleak_winter"))')
            capture('winter_blizzard',-18,24,3,150,-5)
    finally:
        c.eval(f'assert(render.scenery("{scenery}"))');c.call('render.setParams',original)
        (output/'visual_checks.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def cloud_quality_review(c, output):
    """Isolate density, lighting and history in a fixed cloud view; never update goldens."""
    original=c.call('render.getParams');scenery=c.eval('return render.scenery_id()');report={}
    try:
        c.eval('assert(render.scenery("woodland_swamp"))')
        y=c.eval('return terrain.height_at(22,-19)')+18
        parameters(c,camPos=[22,y,-19],camYaw=215,camPitch=24,sunPitch=8,sunYaw=150,
                   autoExposure=False,exposure=1,temporalAA=False,fixedTime=15)
        base=c.call('render.getParams')
        variants={'baseline':{},'raw':{'atmosphere':{'cloudHistory':False}},
                  'noon':{'sunPitch':32},
                  'sparse_cumulus':{'cloudCoverage':.35,'cloudSoftness':.18,'sunPitch':32},
                  'swamp_cloud_bank':{'cloudCoverage':.55,'cloudSoftness':.08,'cloudTypeBias':.35,
                     'cloudDensityMultiplier':.01,'cloudShapeScale':9000,'cloudDetailScale':1800,
                     'cloudDeckHeight':1000,'cloudLayerThickness':2200,'sunPitch':32},
                  'dense_samples':{'cloudMaxSteps':192},
                  'larger_billows':{'cloudDetailScale':720,'cloudMaxSteps':192,'cloudLightTaps':6},
                  'no_detail':{'cloudDetailStrength':0,'cloudCurlStrength':0},
                  'no_curl':{'cloudCurlStrength':0},
                  'unshadowed':{'cloudLightAbsorption':0,'cloudSunOcclusion':0}}
        for name,settings in variants.items():
            c.call('render.setParams',base);parameters(c,**settings);wait_frames(c,64)
            c.call('render.capture',{'path':str(output/('cloud_'+name+'.png')),'maxDimension':1920})
            report[name]=settings
    finally:
        c.eval(f'assert(render.scenery("{scenery}"))');c.call('render.setParams',original)
        (output/'cloud_quality_review.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sync-validation',action='store_true')
    parser.add_argument('--performance-only',action='store_true')
    parser.add_argument('--light-check',action='store_true')
    parser.add_argument('--sky-check',action='store_true')
    parser.add_argument('--cloud-shadow-check',action='store_true')
    parser.add_argument('--cloud-history-check',action='store_true')
    parser.add_argument('--exposure-check',action='store_true')
    parser.add_argument('--lifecycle-check',action='store_true')
    parser.add_argument('--terrain-check',action='store_true')
    parser.add_argument('--visual-review',action='store_true')
    parser.add_argument('--swamp-only',action='store_true',help='Keep visual and lifecycle review in woodland_swamp; exclude other maps')
    parser.add_argument('--cloud-quality-review',action='store_true')
    parser.add_argument('--graphics-check',action='store_true')
    parser.add_argument('--asset-regression',action='store_true')
    parser.add_argument('--output',type=Path,default=ROOT/'captures/atmosphere')
    args=parser.parse_args();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    with socket.socket() as socket_:
        socket_.bind(('127.0.0.1',0));port=socket_.getsockname()[1]
    with tempfile.NamedTemporaryFile(mode='w',suffix='.lua',delete=False) as noop:
        noop.write('-- Private atmosphere regression.\n')
    env=os.environ.copy();env.update(GLGEN_BACKGROUND='1',GLGEN_BACKGROUND_FPS='30',
        GLGEN_AGENT_PORT=str(port),GLGEN_SCRIPT=noop.name,GLGEN_FIXED_TIME='15',
        GLGEN_SCENERY='woodland_swamp',GLGEN_TERRAIN_ON_START='1',
        GLGEN_SMOKE_FRAMES='10000',GLGEN_WINDOW_SIZE='1920,1080',
        GLGEN_PLAY_SNAPSHOT=str(output/'private_play_snapshot.json'))
    if args.sync_validation:env['VK_VALIDATION_VALIDATE_SYNC']='1'
    else:env.pop('VK_VALIDATION_VALIDATE_SYNC',None)
    report={'resolution':[1920,1080],'synchronizationValidation':args.sync_validation,'releaseComplete':False}
    process=None
    try:
        with (output/'runtime.log').open('w') as log:
            process=subprocess.Popen([str(ROOT/'Build-vs18/bin/Release/glGenVk.exe')],cwd=ROOT,
                env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            deadline=time.monotonic()+120
            while True:
                try:c=GlGenClient(port=port,timeout=120);break
                except OSError:
                    if process.poll() is not None or time.monotonic()>deadline:raise
                    time.sleep(.2)
            with c:
                wait_frames(c,120)
                c.call('render.atmosphereProbe',{'enabled':True})
                wait_frames(c,8)
                report['verifiedResolution']=probe(c)['viewport']
                assert report['verifiedResolution']==[1920,1080],report['verifiedResolution']
                c.call('render.atmosphereProbe',{'enabled':False})
                if not args.performance_only:report['transport']=transport_checks(c,output)
                if args.light_check:report['lightsAndHistory']=light_and_history_checks(c,output)
                if args.sky_check:report['sky']=sky_checks(c,output)
                if args.cloud_shadow_check:report['cloudShadows']=cloud_shadow_checks(c,output)
                if args.cloud_history_check:report['cloudHistory']=cloud_history_checks(c,output)
                if args.exposure_check:report['exposure']=exposure_checks(c,output)
                if args.lifecycle_check:report['lifecycle']=lifecycle_checks(c,process,output,args.swamp_only)
                if args.terrain_check:report['terrain']=terrain_checks(c,output)
                if args.visual_review:report['visual']=visual_checks(c,output,args.swamp_only)
                if args.cloud_quality_review:report['cloudQuality']=cloud_quality_review(c,output)
                if args.graphics_check:
                    from graphics_checks import run_checks
                    report['graphicsChecks']=run_checks(c,output/'graphics_checks')
                if args.asset_regression:
                    with (output/'asset-regression.log').open('w') as regression_log:
                        regression=subprocess.run([sys.executable,str(ROOT/'Tools/glgen-regress/regress.py'),'--port',str(port),'--report',str(output/'asset-regression.json')],
                            cwd=ROOT,stdout=regression_log,stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                    report['assetRegressionExitCode']=regression.returncode
                if not args.sync_validation:report['performance']=measure(c,output,process)
                diagnostics=c.eval('return render.stats()')
                report['allocations']={key:diagnostics[key] for key in (
                    'atmosphereAllocatedBytes','cloudHistoryAllocatedBytes','cloudDetailAllocatedBytes')}
        report['vulkanValidationErrors']=(output/'runtime.log').read_text(errors='replace').count('[Vulkan][ERROR]')
        assert report['vulkanValidationErrors']==0,'Vulkan validation failed; inspect runtime.log'
        if 'performance' in report:
            benchmarks=list(report['performance'].values())
            if 'lightsAndHistory' in report:benchmarks.append(report['lightsAndHistory']['fourLightPerformance'])
            report['performanceGates']=all(v['fogMedianMs']<=3 and v['fogP95Ms']<=4 and
                v['bloomMedianMs']<=.5 and v['atmosphereBytes']<=64*1024*1024 for v in benchmarks)
            report['performanceGates']=report['performanceGates'] and all(
                v['fogIncludingWaterUpperMedianMs']<=3 and v['fogIncludingWaterUpperP95Ms']<=4
                for v in report['performance'].values())
    except Exception as error:
        report['failure']=str(error)
        raise
    finally:
        if process:
            process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait()
        Path(noop.name).unlink(missing_ok=True)
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='performance'},indent=2),flush=True)
    if 'performance' in report:
        print(json.dumps({name:{k:v for k,v in row.items() if k!='samples'} for name,row in report['performance'].items()},indent=2),flush=True)


if __name__=='__main__':main()
