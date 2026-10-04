"""Bake engine-owned, periodic cellular cloud detail; no external image inputs.

Requires NumPy. Integer hashing fixes lattice sites across runs. Samples are at
voxel centres and neighbours wrap in every axis, matching a repeat sampler.
The GLVOL1 format is documented by pack_cloud_volumes.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import numpy as np


def sites(x, y, z, cells, seed):
    # Arithmetic modulo 2^32 is explicit; no platform RNG mapping is involved.
    h=((x % cells).astype(np.uint32)*np.uint32(0x8da6b343) ^
       (y % cells).astype(np.uint32)*np.uint32(0xd8163841) ^
       (z % cells).astype(np.uint32)*np.uint32(0xcb1ab31f) ^ np.uint32(seed))
    h ^= h >> np.uint32(16);h *= np.uint32(0x7feb352d)
    h ^= h >> np.uint32(15);h *= np.uint32(0x846ca68b);h ^= h >> np.uint32(16)
    return .25+.5*(h.astype(np.float64)/4294967296.)


def cellular(size, cells, seed):
    result=np.empty((size,size,size),np.float64)
    # Small slabs bound temporary memory; vectorization changes no arithmetic.
    for start in range(0,size,8):
        z,y,x=np.meshgrid((np.arange(start,min(start+8,size))+.5)*cells/size,
                         (np.arange(size)+.5)*cells/size,
                         (np.arange(size)+.5)*cells/size,indexing='ij')
        ix,iy,iz=np.floor(x).astype(np.int32),np.floor(y).astype(np.int32),np.floor(z).astype(np.int32)
        distance=np.full(x.shape,np.inf)
        for dz in (-1,0,1):
            for dy in (-1,0,1):
                for dx in (-1,0,1):
                    a,b,c=ix+dx,iy+dy,iz+dz
                    sx=sites(a,b,c,cells,seed)
                    sy=sites(a,b,c,cells,seed+101)
                    sz=sites(a,b,c,cells,seed+211)
                    distance=np.minimum(distance,(a+sx-x)**2+(b+sy-y)**2+(c+sz-z)**2)
        result[start:start+8]=np.clip(1-np.sqrt(distance)/np.sqrt(3),0,1)
    return result


def bake(size, destination):
    f4,f8,f16,f32=[cellular(size,c,0x12345+c*17) for c in (4,8,16,32)]
    channels=np.stack((.6*f4+.3*f8+.1*f16,.4*f8+.4*f16+.2*f32,f8,f16),axis=-1)
    pixels=np.floor(np.clip(channels,0,1)*255+.5).astype(np.uint8)
    payload=b'GLVOL1\0\0'+struct.pack('<4I',size,size,size,4)+pixels.tobytes()
    destination.parent.mkdir(parents=True,exist_ok=True);destination.write_bytes(payload)
    provenance={'generator':'Tools/bake_cloud_detail.py','algorithm':'Periodic cellular F1; fixed integer lattice hashing',
                'dimensions':[size,size,size,4],'seed':0x12345,'externalInputs':[],
                'sha256':hashlib.sha256(payload).hexdigest()}
    destination.with_suffix('.json').write_text(json.dumps(provenance,indent=2)+'\n')
    print(json.dumps(provenance),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--size',type=int,default=128)
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'assets/clouds/cloud_detail_periodic.glvol')
    args=parser.parse_args();bake(args.size,args.output)
