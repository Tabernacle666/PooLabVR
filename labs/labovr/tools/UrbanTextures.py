#!/usr/bin/env python3
"""Deterministic PBR map baker for PooLabVR's winter street.
Only Python stdlib. Output is reproducible; no downloads, PIL or CUDA needed.
Usage: python UrbanTextures.py OUTPUT_DIRECTORY [RESOLUTION]
"""
from pathlib import Path
import json
import math
import struct
import sys
import zlib

MATERIALS = {
    "asphalt":       ((49, 51, 53), .87, .0, .68, "aggregate"),
    "asphalt_patch": ((37, 39, 43), .77, .0, .54, "aggregate"),
    "yellow_line":   ((189, 150, 35), .73, .0, .26, "paint"),
    "concrete":      ((131, 131, 124), .84, .0, .43, "concrete"),
    "brick":         ((111, 54, 43), .85, .0, .62, "brick"),
    "mortar":        ((118, 114, 101), .95, .0, .20, "concrete"),
    "cast_iron":     ((57, 51, 47), .67, .75, .51, "iron"),
    "rust":          ((116, 54, 30), .92, .20, .63, "iron"),
    "lamp_enamel":   ((35, 47, 46), .47, .68, .21, "paint"),
    "bark":          ((78, 59, 45), .91, .0, .83, "bark"),
    "pine_bark":     ((72, 65, 57), .88, .0, .76, "bark"),
    "winter_leaf":   ((103, 42, 32), .80, .0, .36, "leaf"),
    "conifer":       ((44, 72, 56), .87, .0, .43, "needle"),
    "grass":         ((65, 81, 47), .92, .0, .61, "grass"),
    "forest_litter": ((70, 56, 38), .96, .0, .64, "litter"),
    "soil":          ((73, 60, 47), .97, .0, .53, "aggregate"),
    "snow":          ((213, 221, 223), .91, .0, .34, "snow"),
    "slush":         ((108, 111, 110), .43, .0, .39, "snow"),
    "water":         ((66, 73, 78), .07, .0, .06, "water"),
}

def splitmix(v):
    v = (v + 0x9e3779b97f4a7c15) & 0xffffffffffffffff
    v = ((v ^ (v >> 30)) * 0xbf58476d1ce4e5b9) & 0xffffffffffffffff
    v = ((v ^ (v >> 27)) * 0x94d049bb133111eb) & 0xffffffffffffffff
    return v ^ (v >> 31)

def noise(x, y, period, seed):
    # Exactly tileable value noise. Fractional interpolation uses quintic fade.
    a = x * period
    b = y * period
    i = math.floor(a)
    j = math.floor(b)
    u = a-i
    v = b-j
    u = u*u*u*(u*(u*6-15)+10)
    v = v*v*v*(v*(v*6-15)+10)
    mask = period - 1
    def val(ix, iy):
        h = splitmix((ix & mask) * 0x9e3779b1
                     ^ (iy & mask) * 0x85ebca6b ^ seed)
        return (h & 65535)/65535
    p = val(i,j)*(1-u)+val(i+1,j)*u
    q = val(i,j+1)*(1-u)+val(i+1,j+1)*u
    return p*(1-v)+q*v

def clamp(v, lo=0., hi=1.):
    return max(lo,min(hi,v))

def generate_maps(name, base, roughness, metalness, relief, kind, resolution):
    # Texture-space noise, large mottling and local grains stay independent.
    hmap = [0.] * (resolution*resolution)
    colors = bytearray(resolution*resolution*4)
    orm = bytearray(resolution*resolution*4)
    seed = splitmix(sum((i+1)*ord(c) for i,c in enumerate(name)))
    for y in range(resolution):
        v = y / resolution
        for x in range(resolution):
            u = x / resolution
            n0 = noise(u,v,4,seed)
            n1 = noise(u,v,16,seed^911)
            n2 = noise(u,v,64,seed^173)
            grain = (.44*n0+.35*n1+.21*n2)
            micro = n2 - .5
            height = grain
            color_shift = (grain-.5)*.44
            rgh = roughness
            met = metalness
            if kind == "aggregate":
                pebbles = noise(u,v,64,seed^341)
                height = .55*grain+.45*pebbles
                color_shift += (pebbles-.5)*.35
            elif kind == "brick":
                # 8x16 running bond with real mortar grooves.
                row = math.floor(v*16)
                bx = (u*8 + (row%2)*.5)%1
                by = (v*16)%1
                groove = min(bx,1-bx,by*.5,(1-by)*.5)
                mortar = clamp(groove*55)
                height = .11+.78*mortar+.11*grain
                color_shift += (mortar-.65)*.48
                rgh = clamp(roughness+.06*(1-mortar))
            elif kind == "concrete":
                pores = noise(u,v,64,seed^113)
                height = grain*.7+pores*.3
                color_shift += (pores-.5)*.2
            elif kind == "paint":
                chips = noise(u,v,32,seed^295)
                height = .67*grain+.33*chips
                if chips<.18:
                    color_shift -= .26
                    rgh = clamp(roughness+.19)
                    met *= .45
            elif kind == "iron":
                corrosion = noise(u,v,16,seed^1673)
                height = grain*.45+corrosion*.55
                color_shift += (corrosion-.5)*.43
                rgh = clamp(roughness+.15*(corrosion-.5))
            elif kind == "bark":
                grooves = abs(math.sin(2*math.pi*(u*17+noise(u,v,4,seed^13)*1.8)))
                height = .22*grain+.78*grooves
                color_shift += (grooves-.5)*.5
            elif kind == "leaf":
                veins = abs(math.sin(2*math.pi*(u*14 + v*2.5)))
                height = .46*grain+.54*(1-veins)
                color_shift += (veins-.5)*.16
            elif kind == "needle":
                strand = abs(math.sin(2*math.pi*(u*42+v*3)))
                height = grain*.45+strand*.55
                color_shift += (strand-.5)*.29
            elif kind == "grass":
                blade = abs(math.sin(2*math.pi*(u*46 + v*17)))
                height = .35*grain+.65*blade
                color_shift += (blade-.5)*.35
            elif kind == "litter":
                veins = noise(u,v,32,seed^2524)
                height = grain*.44+veins*.56
                color_shift += (veins-.5)*.33
            elif kind == "snow":
                height = .55*grain+.45*noise(u,v,32,seed^27)
                color_shift *= .30
            elif kind == "water":
                height = .5+.015*math.sin(u*math.pi*6+v*math.pi*3)
                color_shift *= .09
            index = y*resolution+x
            hmap[index] = clamp(height)
            cidx = index*4
            # Palette is sRGB. Independent tint by channel avoids flat gray.
            for k in range(3):
                channel = base[k]*(1+color_shift*(.87+.07*k))
                colors[cidx+k] = int(clamp(channel,0,255))
            colors[cidx+3] = 255
            ao = clamp(.93-.16*(1-height))
            orm[cidx] = int(255*ao)
            orm[cidx+1] = int(255*clamp(rgh+.12*micro))
            orm[cidx+2] = int(255*clamp(met))
            orm[cidx+3] = 255
    normal = bytearray(resolution*resolution*4)
    height_bytes = bytearray(resolution*resolution*4)
    bump_scale = relief*.8
    for y in range(resolution):
        for x in range(resolution):
            i = y*resolution+x
            left=hmap[y*resolution+(x-1)%resolution]
            right=hmap[y*resolution+(x+1)%resolution]
            up=hmap[((y-1)%resolution)*resolution+x]
            down=hmap[((y+1)%resolution)*resolution+x]
            nx=(left-right)*bump_scale
            ny=(up-down)*bump_scale
            nz=1.
            norm=math.sqrt(nx*nx+ny*ny+nz*nz)
            q=4*i
            normal[q:q+4]=bytes((int(127.5*(1+nx/norm)),int(127.5*(1+ny/norm)),
                                 int(127.5*(1+nz/norm)),255))
            value=round(hmap[i]*255)
            height_bytes[q:q+4]=bytes((value,value,value,255))
    return {
        "basecolor": colors, "normal": normal,
        "orm": orm, "height": height_bytes
    }

def png_chunk(tag, data):
    return struct.pack(">I",len(data))+tag+data+struct.pack(">I",zlib.crc32(tag+data)&0xffffffff)

def write_png(path, rgba, size):
    pitch=size*4
    scanlines=b"".join(b"\0"+rgba[i:i+pitch] for i in range(0,len(rgba),pitch))
    header=struct.pack(">IIBBBBB",size,size,8,6,0,0,0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"+png_chunk(b"IHDR",header)
                     +png_chunk(b"IDAT",zlib.compress(scanlines,6))
                     +png_chunk(b"IEND",b""))

def main():
    if len(sys.argv)<2:
        raise SystemExit("usage: UrbanTextures.py OUTPUT_DIRECTORY [RESOLUTION]")
    destination=Path(sys.argv[1])
    size=int(sys.argv[2]) if len(sys.argv)>2 else 256
    if size<32 or size>1024 or size&(size-1):
        raise SystemExit("resolution must be power-of-two between 32 and 1024")
    destination.mkdir(parents=True,exist_ok=True)
    manifest={"version":1,"resolution":size,"units":"sRGB basecolor; tangent normal; linear occlusion-roughness-metallic; linear height","materials":{}}
    for name, (base,rough,metal,relief,kind) in MATERIALS.items():
        maps=generate_maps(name,base,rough,metal,relief,kind,size)
        names={}
        for channel,data in maps.items():
            filename=f"{name}_{channel}.png"
            write_png(destination/filename,data,size)
            names[channel]=filename
        manifest["materials"][name]={"maps":names,"roughness":rough,"metallic":metal,
                                     "height_scale":relief,"pattern":kind}
    (destination/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print(f"{len(MATERIALS)} PBR materials, {len(MATERIALS)*4} PNG maps: {destination}")

if __name__ == "__main__":
    main()
