#!/usr/bin/env python3
"""Build the ordered urban mesh library as glTF 2.0, using the Python stdlib.
Every mesh is in meters and contains positions, normals and repeating UVs.
"""
from pathlib import Path
import json
import math
import random
import struct
import sys

destination=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parent.parent/"assets"/"urban"
destination.mkdir(parents=True,exist_ok=True)
meshes=[]
def norm(v):
    length=math.sqrt(sum(q*q for q in v)) or 1
    return tuple(q/length for q in v)
def cross(a,b):
    return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def add(a,b):
    return tuple(a[i]+b[i] for i in range(3))
def scale(a,f):
    return tuple(q*f for q in a)
def merge(name,parts):
    v,i=[],[]
    for _,vv,ii in parts:
        start=len(v)
        v.extend(vv)
        i.extend(start+k for k in ii)
    return name,v,i
def surface(name,rows,cols,point):
    vertices=[]
    for j in range(rows+1):
        for i in range(cols+1):
            vertices.append(point(i/cols,j/rows))
    triangles=[]
    for j in range(rows):
        for i in range(cols):
            a=j*(cols+1)+i
            triangles.extend((a,a+cols+1,a+1,a+1,a+cols+1,a+cols+2))
    return name,vertices,triangles
def quad(name,points,uvs):
    v1=tuple(points[1][i]-points[0][i] for i in range(3))
    v2=tuple(points[3][i]-points[0][i] for i in range(3))
    n=norm(cross(v1,v2))
    return name,[(p,n,t) for p,t in zip(points,uvs)],[0,1,2,0,2,3]
def box(name,lo,hi,repeat=1):
    x,y,z=lo;X,Y,Z=hi
    faces=[
        [(x,y,z),(x,Y,z),(X,Y,z),(X,y,z)],
        [(X,y,Z),(X,Y,Z),(x,Y,Z),(x,y,Z)],
        [(x,y,Z),(x,Y,Z),(x,Y,z),(x,y,z)],
        [(X,y,z),(X,Y,z),(X,Y,Z),(X,y,Z)],
        [(x,Y,z),(x,Y,Z),(X,Y,Z),(X,Y,z)],
        [(x,y,Z),(x,y,z),(X,y,z),(X,y,Z)]]
    return merge(name,[quad('',face,[(0,0),(0,repeat),(repeat,repeat),(repeat,0)]) for face in faces])
def tube(name,path,radii,sides=8):
    vertices=[]
    for k,p in enumerate(path):
        prev=path[max(0,k-1)];nxt=path[min(len(path)-1,k+1)]
        tangent=norm(tuple(nxt[j]-prev[j] for j in range(3)))
        axis=norm(cross((0,1,0),tangent)) if abs(tangent[1])<.94 else (1.,0.,0.)
        bitangent=norm(cross(tangent,axis))
        for s in range(sides+1):
            theta=2*math.pi*s/sides
            radial=add(scale(axis,math.cos(theta)),scale(bitangent,math.sin(theta)))
            vertices.append((add(p,scale(radial,radii[k])),radial,(s/sides,k/3)))
    indices=[]
    for k in range(len(path)-1):
        for s in range(sides):
            a=k*(sides+1)+s
            indices.extend((a,a+1,a+sides+1,a+1,a+sides+2,a+sides+1))
    return name,vertices,indices
def lathe(name,profile,sides=28):
    vertices=[]
    for k,(y,r) in enumerate(profile):
        before=profile[max(0,k-1)]
        after=profile[min(len(profile)-1,k+1)]
        dr=after[1]-before[1]
        dy=after[0]-before[0]
        for j in range(sides+1):
            a=j*2*math.pi/sides
            n=norm((dy*math.cos(a),-dr,dy*math.sin(a)))
            vertices.append(((r*math.cos(a),y,r*math.sin(a)),n,(j/sides,k/2)))
    idx=[]
    for k in range(len(profile)-1):
        for j in range(sides):
            a=k*(sides+1)+j
            idx.extend((a,a+sides+1,a+1,a+1,a+sides+1,a+sides+2))
    return name,vertices,idx
def ellipse(name,center,dimensions,detail=12):
    cx,cy,cz=center;rx,ry,rz=dimensions
    def p(u,v):
        az=u*2*math.pi
        el=v*math.pi
        n=(math.sin(el)*math.cos(az),math.cos(el),math.sin(el)*math.sin(az))
        point=(cx+rx*n[0],cy+ry*n[1],cz+rz*n[2])
        return point,n,(u*2,v*2)
    return surface(name,detail,detail*2,p)
def road(name,width=12,length=108):
    def point(u,v):
        x=(u-.5)*length;z=(v-.5)*width
        # Camber and shallow repair depressions are geometric, not painted.
        y=.055*(abs(z)/(width*.5))**2-.012*math.exp(-((x-11)**2+(z+1)**2)/6)
        dydz=.11*z/(width*.5)**2
        return (x,y,z),norm((0,1,-dydz)),(x*.35,z*.35)
    return surface(name,18,128,point)
# Library order is mirrored in UrbanScene.h. Do not insert in the middle.
meshes.append(road("Road108"))
meshes.append(road("Intersection12",12,12))
meshes.append(surface("YellowCenterStripe",1,128,
    lambda u,v: (((u-.5)*108,.032,(v-.5)*.14),(0,1,0),(u*54,v))))
meshes.append(box("Sidewalk108",(-54,-.05,-1),(54,.14,1),54))
meshes.append(box("Curb108",(-54,-.12,-.115),(54,.17,.115),54))
# The grate has real open slots, unlike an opaque dark rectangle.
grate=[box("",(-.48,-.07,-.39),(.48,-.02,-.32)),
       box("",(-.48,-.07,.32),(.48,-.02,.39)),
       box("",(-.48,-.07,-.32),(-.41,-.02,.32)),
       box("",(.41,-.07,-.32),(.48,-.02,.32))]
for k in range(16):
    x=-.38+k*.051
    grate.append(box("",(x,-.04,-.29),(x+.016,0,.29)))
meshes.append(merge("OpenStreetDrain",grate))
meshes.append(merge("CastIronLampPole",[
    lathe("",[(0,.22),(.12,.22),(.3,.16),(5.5,.085),(6.3,.080)]),
    tube("",[(0,6.21,0),(.16,6.52,0),(.68,6.64,0),(1.35,6.42,0)],
         [.083,.080,.069,.056],12),
    box("",(1.18,6.28,-.22),(1.79,6.53,.22)),
    box("",(-.12,0,-.12),(.12,.16,.12))]))
meshes.append(box("LampDiffuser",(1.28,6.255,-.18),(1.71,6.275,.18)))
# Wall runs 100 m at height 10 m, with staggered courses physically relieved.
wall_parts=[box("",(-50,0,-.19),(50,9.65,.19),50),
            box("",(-50,9.65,-.24),(50,10.0,.24),50)]
# Along the facade, brick relief is sparse geometry; fine courses live in PBR.
for j in range(7):
    for k in range(20):
        x=-49+5*k+(2.5 if j%2 else 0)
        if x+4.75>50:continue
        y=.48+j*1.26
        wall_parts.append(box("",(x,y,.19),(x+4.73,y+.10,.213)))
meshes.append(merge("TenMeterBrickWall",wall_parts))
# Vines are planar facade modules at x=0, y=0..10, z=0; no tree geometry reused.
rng=random.Random(3711)
vine=[]
for branch in range(19):
    p=[(rng.uniform(-.3,.3),0,.0)]
    lean=rng.uniform(-.8,.8)
    for step in range(1,11):
        t=step/10
        p.append((p[0][0]+lean*t*1.65+.26*math.sin(t*math.pi*4+branch),
                  9.5*t,rng.uniform(.025,.055)))
    vine.append(tube("",p,[.020*(1-step/12) for step in range(11)],6))
meshes.append(merge("WallVines",vine))
leaves=[]
for branch in range(95):
    y=.4+9*rng.random()
    x=rng.uniform(-1.9,1.9)
    size=.09+rng.random()*.14
    leaves.append(quad("",[(x-size,y,.06),(x,y+size,.07),
                            (x+size,y,.06),(x,y-size,.05)],
                       [(0,0),(.5,1),(1,0),(.5,0)]))
meshes.append(merge("ResidualRedVineLeaves",leaves))
# Trees have a distinct upright trunk and branched skeleton, not a scaled bonsai.
branches=[tube("",[(0,0,0),(0,2.4,0),(.11,5.1,-.07),(.19,8.0,-.09)],
               [.34,.27,.18,.075],12)]
for k in range(15):
    a=math.tau*k/7.17
    h=3.0+(k%6)*.68
    length=2.2+float(k%4)*.23
    p=[(.05,h,0),(.3*math.cos(a),h+.45,.3*math.sin(a)),
       (length*.65*math.cos(a),h+1.4,length*.65*math.sin(a)),
       (length*math.cos(a),h+2.3,length*math.sin(a))]
    branches.append(tube("",p,[.10,.085,.053,.013],7))
meshes.append(merge("DeciduousTrunkBranches",branches))
tree_leaves=[]
for k in range(170):
    a=rng.random()*math.tau
    radial=1.3+2.0*rng.random()
    y=5+5*rng.random()
    center=(math.cos(a)*radial,y,math.sin(a)*radial)
    size=.07+.20*rng.random()
    tree_leaves.append(quad("",[
        (center[0]-size,center[1],center[2]),
        (center[0],center[1]+size,center[2]+size*.2),
        (center[0]+size,center[1],center[2]),
        (center[0],center[1]-size,center[2]-size*.2)],
        [(0,0),(.5,1),(1,0),(.5,0)]))
meshes.append(merge("SparseWinterLeaves",tree_leaves))
pine=[tube("",[(0,0,0),(0,2,0),(0,8.6,0)],[.29,.25,.028],12)]
for level in range(12):
    y=1.3+level*.56
    extent=3.0*(1-level/13)
    for ray in range(9):
        a=math.tau*ray/9+level*.43
        end=(extent*math.cos(a),y-.4,extent*math.sin(a))
        pine.append(tube("",[(0,y+.2,0),end],[.06,.01],5))
        for off in (.27,.53,.78):
            c=(end[0]*off,y+.2-(.6*off),end[2]*off)
            d=.32*(1-off)+.08
            pine.append(quad("",[
                (c[0]-d,c[1]-.12,c[2]-d),
                (c[0]+d,c[1]-.12,c[2]-d),
                (c[0]+d,c[1]-.12,c[2]+d),
                (c[0]-d,c[1]-.12,c[2]+d)],
                [(0,0),(1,0),(1,1),(0,1)]))
meshes.append(merge("ConiferWithTieredBoughs",pine))
grasses=[]
for k in range(38):
    x=rng.uniform(-.45,.45);z=rng.uniform(-.45,.45)
    h=rng.uniform(.07,.35)
    grasses.append(quad("",[(x-.018,0,z),(x,.0,z+.01),
                             (x+.04,h,z+.08),(x,h,z-.03)],
                       [(0,0),(.2,0),(.9,1),(0,1)]))
meshes.append(merge("SparseWinterGrass",grasses))
meshes.append(surface("IrregularSnowPatch",14,16,lambda u,v:(
    ((u-.5)*2.4,
     .025+.13*math.sin(math.pi*u)*math.sin(math.pi*v)
          +.012*math.sin(28*u+4*v),
     (v-.5)*1.8),
    norm((-.13*math.cos(math.pi*u),1,-.13*math.cos(math.pi*v))),
    (u*2,v*2))))
meshes.append(surface("ShallowPuddle",8,22,lambda u,v:(
    ((u-.5)*3.0*(.90+.1*math.sin(math.pi*v*5)),
     .003+.001*math.sin(math.tau*3*u+math.tau*2*v),
     (v-.5)*1.8*(.93+.07*math.cos(math.tau*u*4))),
    (0,1,0),(u*2,v*2))))
# Broad cloud dome remains inside the 40 m XR far plane.
def cloud_dome():
    detail=32
    rings=18
    verts=[]
    idx=[]
    for j in range(rings+1):
        v=j/rings
        elev=math.pi*v
        for k in range(detail+1):
            u=k/detail
            az=math.tau*u
            n=(math.sin(elev)*math.cos(az),math.cos(elev),
               math.sin(elev)*math.sin(az))
            verts.append((tuple(34.*q for q in n),tuple(-q for q in n),(u,v)))
    for j in range(rings):
        for k in range(detail):
            a=j*(detail+1)+k
            idx.extend((a,a+1,a+detail+1,a+1,a+detail+2,a+detail+1))
    return "OvercastSkyDome",verts,idx
meshes.append(cloud_dome())
assert len(meshes)==18

data=bytearray()
views=[]
accessors=[]
out=[]
def accessor(values,fmt,kind,component,bounds=False):
    while len(data)%4:data.append(0)
    offset=len(data)
    for value in values:data.extend(struct.pack('<'+fmt,*value))
    view=len(views)
    views.append({"buffer":0,"byteOffset":offset,"byteLength":len(data)-offset})
    a={"bufferView":view,"componentType":component,"count":len(values),"type":kind}
    if bounds:
        a["min"]=[min(v[k] for v in values) for k in range(3)]
        a["max"]=[max(v[k] for v in values) for k in range(3)]
    accessors.append(a)
    return len(accessors)-1
for name,vertices,indices in meshes:
    p=accessor([v[0] for v in vertices],"3f","VEC3",5126,True)
    n=accessor([v[1] for v in vertices],"3f","VEC3",5126)
    uv=accessor([v[2] for v in vertices],"2f","VEC2",5126)
    ix=accessor([(int(i),) for i in indices],"I","SCALAR",5125)
    out.append({"name":name,"primitives":[{
        "attributes":{"POSITION":p,"NORMAL":n,"TEXCOORD_0":uv},
        "indices":ix,"mode":4
    }]})
(destination/"urban.bin").write_bytes(data)
(destination/"urban.gltf").write_text(json.dumps({
    "asset":{"version":"2.0","generator":"PooLabVR UrbanMeshes"},
    "buffers":[{"uri":"urban.bin","byteLength":len(data)}],
    "bufferViews":views,"accessors":accessors,"meshes":out
},separators=(',',':'))+"\n")
print(f"{len(meshes)} urban meshes, {len(data)} bytes of geometry: {destination}")
