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
    # Thin, dirty wet film. Its roughness keeps the pre-existing ray reflection
    # path active at VR near field only; asphalt remains unmodified elsewhere.
    "wet_asphalt":   ((28, 32, 36), .48, .0, .25, "aggregate"),
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
    "overcast_sky":  ((36, 47, 66), 1.0, .0, .0, "clouds"),
}

# The unit is metres, not a mesh-dependent arbitrary "bump" strength.
# Tile dimensions match the metric UVs baked by UrbanMeshes.
TILE_METERS = {
    "asphalt": (3.0, 3.0), "asphalt_patch": (3.0, 3.0),
    "wet_asphalt": (3.0, 3.0), "yellow_line": (1.5, 1.5),
    "concrete": (1.5, 1.5), "brick": (1.76, 1.04),
    "mortar": (1.76, 1.04), "cast_iron": (.80, .80),
    "rust": (.80, .80), "lamp_enamel": (.80, .80),
    "bark": (.65, 1.10), "pine_bark": (.65, 1.10),
    "winter_leaf": (.20, .20), "conifer": (.60, .60),
    "grass": (.80, .80), "forest_litter": (4.0, 4.0),
    "soil": (3.0, 3.0), "snow": (1.5, 1.5),
    "slush": (1.5, 1.5), "water": (1.5, 1.5),
    "overcast_sky": (1.0, 1.0),
}
HEIGHT_METERS = {
    "asphalt": .012, "asphalt_patch": .016, "wet_asphalt": .003,
    "yellow_line": .001, "concrete": .006, "brick": .014,
    "mortar": .006, "cast_iron": .003, "rust": .004,
    "lamp_enamel": .001, "bark": .012, "pine_bark": .014,
    "winter_leaf": .001, "conifer": .002, "grass": .007,
    "forest_litter": .016, "soil": .014, "snow": .012,
    "slush": .005, "water": .0004, "overcast_sky": 0.0,
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
                # Small visible minerals are distinct from the coarse noise.
                count=128 if name in ("asphalt","asphalt_patch","wet_asphalt") else 96
                qx=u*count;qy=v*count
                ix=int(qx);iy=int(qy)
                key=splitmix(seed ^ ix*0x9e3779b1 ^ iy*0x85ebca6b)
                cx=.25+((key&255)/255.)*.5
                cy=.25+(((key>>8)&255)/255.)*.5
                radius=.16+(((key>>16)&255)/255.)*.28
                mineral=clamp((radius-math.hypot(qx-ix-cx,qy-iy-cy))*11.)
                particle_tone=.60+((key>>24)&255)/255.*.50
                pebbles=noise(u,v,64,seed^341)
                height=clamp(.33*grain+.40*pebbles+.27*mineral)
                color_shift+=(pebbles-.5)*.22+mineral*(particle_tone-.82)*.58
                rgh=clamp(roughness+.08*mineral)
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
                # Fine pores and an occasional hairline fissure.
                qx=u*192.;qy=v*192.
                ix=int(qx);iy=int(qy)
                key=splitmix(seed ^ ix*0x9e3779b1 ^ iy*0x85ebca6b)
                dx=qx-ix-(.2+((key&255)/255.)*.6)
                dy=qy-iy-(.2+(((key>>8)&255)/255.)*.6)
                radius=.08+(((key>>16)&255)/255.)*.16
                pore=clamp((radius-math.hypot(dx,dy))*14.)
                line=.46+.026*math.sin(v*math.tau*3.)+.009*math.sin(v*math.tau*11.)
                crack=clamp((.0035-abs(u-line))*250.) if .12<v<.86 else 0.
                pores=noise(u,v,64,seed^113)
                height=clamp(.48*grain+.40*pores+.12*(1.-pore)-.18*crack)
                color_shift+=(pores-.5)*.2-.30*pore-.22*crack
                rgh=clamp(roughness+.12*pore+.06*crack)
            elif kind == "paint":
                chips = noise(u,v,32,seed^295)
                height = .67*grain+.33*chips
                if chips<.18:
                    color_shift -= .26
                    rgh = clamp(roughness+.19)
                    met *= .45
            elif kind == "iron":
                corrosion=.68*noise(u,v,16,seed^1673)+.32*noise(u,v,64,seed^341)
                pitting=noise(u,v,64,seed^1127)
                oxide=clamp((corrosion-.38)*3.)
                height=clamp(.32*grain+.37*corrosion+.31*pitting-.13*oxide)
                color_shift+=(corrosion-.5)*.43-.24*oxide
                rgh=clamp(roughness+.23*oxide)
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
            elif kind == "clouds":
                # Broad soft cloud bands and low winter overcast.
                height = .20*n0+.65*n1+.15*n2
                color_shift = (n0-.5)*.55+(n1-.5)*.48
                color_shift -= .28*math.sin(math.pi*v)**4
                rgh = 1.0
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
            # The existing packed ORM texture has a free alpha channel.
            # Publish the height here so the main DX12 mesh shader actually
            # consumes it; the separate R16 height map remains available.
            orm[cidx+3] = int(255*clamp(height))
    normal = bytearray(resolution*resolution*4)
    height_bytes = bytearray(resolution*resolution*4)
    # A texel spans tile_metres / resolution: the normal is the
    # derivative of actual metre-scale height, not a resolution-dependent
    # magic multiplier. The normal maps and height/POM share one field.
    tile_u, tile_v = TILE_METERS[name]
    slope_u = HEIGHT_METERS[name] * resolution / (2.0 * tile_u)
    slope_v = HEIGHT_METERS[name] * resolution / (2.0 * tile_v)
    for y in range(resolution):
        for x in range(resolution):
            i = y*resolution+x
            left=hmap[y*resolution+(x-1)%resolution]
            right=hmap[y*resolution+(x+1)%resolution]
            up=hmap[((y-1)%resolution)*resolution+x]
            down=hmap[((y+1)%resolution)*resolution+x]
            nx=(left-right)*slope_u
            ny=(up-down)*slope_v
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
    manifest={"version":2,"resolution":size,"units":"sRGB basecolor; tangent normal; linear ORM + height in alpha; linear standalone height; sizes and heights in metres","materials":{}}
    for name, (base,rough,metal,relief,kind) in MATERIALS.items():
        maps=generate_maps(name,base,rough,metal,relief,kind,size)
        names={}
        for channel,data in maps.items():
            filename=f"{name}_{channel}.png"
            write_png(destination/filename,data,size)
            names[channel]=filename
        manifest["materials"][name]={"maps":names,"roughness":rough,"metallic":metal,
                                     "height_scale":relief,
                                     "height_metres":HEIGHT_METERS[name],
                                     "tile_metres":TILE_METERS[name],
                                     "packed_height_channel":"orm.a","pattern":kind}
    (destination/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print(f"{len(MATERIALS)} PBR materials, {len(MATERIALS)*4} PNG maps: {destination}")

if __name__ == "__main__":
    main()
