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
def box(name,lo,hi,repeat=1,tile_metres=None):
    x,y,z=lo;X,Y,Z=hi
    faces=[
        [(x,y,z),(x,Y,z),(X,Y,z),(X,y,z)],
        [(X,y,Z),(X,Y,Z),(x,Y,Z),(x,y,Z)],
        [(x,y,Z),(x,Y,Z),(x,Y,z),(x,y,z)],
        [(X,y,z),(X,Y,z),(X,Y,Z),(X,y,Z)],
        [(x,Y,z),(x,Y,Z),(X,Y,Z),(X,Y,z)],
        [(x,y,Z),(x,y,z),(X,y,z),(X,y,Z)]]
    if tile_metres is None:
        return merge(name,[quad('',face,[(0,0),(0,repeat),(repeat,repeat),(repeat,0)]) for face in faces])
    # Project each face in its own metric plane. A 108 x 2 m sidewalk
    # must not repeat a texture 54 times across its two-metre width.
    sx,sy,sz=tile_metres
    def uv(face,side):
        if side in (0,1):  # constant Z: horizontal X, vertical Y
            return [(p[0]/sx,p[1]/sy) for p in face]
        if side in (2,3):  # constant X: horizontal Z, vertical Y
            return [(p[2]/sz,p[1]/sy) for p in face]
        return [(p[0]/sx,p[2]/sz) for p in face]
    return merge(name,[quad('',face,uv(face,k)) for k,face in enumerate(faces)])
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
        return (x,y,z),norm((0,1,-dydz)),(x/3.0,z/3.0)
    return surface(name,18,128,point)
# Same tile-relative height profile as UrbanLayout::GroundHeight.
def ground_height(x,z):
    k=math.tau/120.
    return (.0025*(x+z)
            +.085*(1.-math.cos(k*x))+.055*(1.-math.cos(k*z))
            +.025*(1.-math.cos(4*k*x))+.018*(1.-math.cos(4*k*z))
            +.006*math.sin(8*k*x)*math.sin(8*k*z))

def conform(mesh,center,yaw=0.):
    """Conform each mesh to the common ground profile for its road side/axis."""
    name,vertices,indices=mesh
    c=math.cos(yaw);s=math.sin(yaw);base=ground_height(*center)
    result=[]
    for (x,y,z),normal,uv in vertices:
        wx=center[0]+c*x+s*z
        wz=center[1]-s*x+c*z
        height=ground_height(wx,wz)-base
        h=.01
        gx=(ground_height(wx+c*h,wz-s*h)-ground_height(wx-c*h,wz+s*h))/(2*h)
        gz=(ground_height(wx+s*h,wz+c*h)-ground_height(wx-s*h,wz-c*h))/(2*h)
        nx,ny,nz=normal
        result.append(((x,y+height,z),norm((nx-gx*ny,ny,nz-gz*ny)),uv))
    return name,result,indices

def sidewalk_slabs(name):
    # 1.5-m slabs, joints 16 mm wide and 6 mm lower than the tops.
    parts=[]
    for i in range(72):
        a=-54.+i*1.5;b=a+1.5
        parts.append(box("",(a,-.05,-1.),(b,.134,1.),tile_metres=(1.5,1.5,1.5)))
        parts.append(box("",(a+.008,.134,-1.),(b-.008,.14,1.),
                         tile_metres=(1.5,1.5,1.5)))
    return merge(name,parts)

def curb_stones(name):
    parts=[]
    for i in range(54):
        a=-54.+i*2.;b=a+2.
        parts.append(box("",(a,-.12,-.115),(b,.157,.115),
                         tile_metres=(1.5,1.5,1.5)))
        parts.append(box("",(a+.012,.157,-.115),(b-.012,.17,.115),
                         tile_metres=(1.5,1.5,1.5)))
    return merge(name,parts)

# Keep the initial library indices stable for all existing authored assets.
meshes.append(conform(road("Road108"),(60.,0.)))
meshes.append(conform(road("Intersection12",12,12),(0.,0.)))
meshes.append(conform(surface("YellowCenterStripe",1,128,
    lambda u,v: (((u-.5)*108,.032,(v-.5)*.14),(0,1,0),(u*72,v*.14/1.5))),
    (60.,0.)))
meshes.append(conform(sidewalk_slabs("Sidewalk108"),(60.,7.)))
meshes.append(conform(curb_stones("Curb108"),(60.,6.07)))
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
# An independent 3.2-m street wall with a cap and real masonry relief.
wall_parts=[box("",(-50,0,-.19),(50,3.0,.19),tile_metres=(1.76,1.04,1.76)),
            box("",(-50,3.0,-.24),(50,3.2,.24),tile_metres=(1.76,1.04,1.76))]
for j in range(12):
    for k in range(20):
        x=-49+5*k+(2.5 if j%2 else 0)
        if x+4.75>50:continue
        y=.18+j*.235
        wall_parts.append(box("",(x,y,.19),(x+4.73,y+.075,.213),
                              tile_metres=(1.76,1.04,1.76)))
meshes.append(merge("LowBrickWall",wall_parts))
# Vines climb the independent wall, not the laboratory.
rng=random.Random(3711)
vine=[]
for branch in range(19):
    p=[(rng.uniform(-.3,.3),0,.0)]
    lean=rng.uniform(-.8,.8)
    for step in range(1,11):
        t=step/10
        p.append((p[0][0]+lean*t*1.65+.26*math.sin(t*math.pi*4+branch),
                  3.0*t,rng.uniform(.025,.055)))
    vine.append(tube("",p,[.020*(1-step/12) for step in range(11)],6))
meshes.append(merge("WallVines",vine))
leaves=[]
for branch in range(95):
    y=.2+2.8*rng.random()
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
# A closed scalloped shoreline, not a rectangular snow decal. Snow cover
# evolves in HomeWorld; this reusable mesh only controls the footprint.
def snow_drift(name):
    sectors=36
    rings=5
    # Center fan and four successive annuli share a deterministic shoreline.
    verts=[((0.,.072,0.),(0.,1.,0.),(.5,.5))]
    for j in range(1,rings+1):
        t=j/rings
        for i in range(sectors):
            angle=math.tau*i/sectors
            edge=(1+.15*math.sin(3*angle+.4)
                    +.085*math.cos(7*angle-.9)
                    +.05*math.sin(11*angle+2.1))
            x=1.15*t*edge*math.cos(angle)
            z=.82*t*edge*math.sin(angle)
            # The outer annulus nearly meets the litter, while its center
            # lifts gently. No hard rectangular border or vertical wall.
            height=.004+.068*(1-t*t)+.006*(1-t)*math.sin(5*angle)
            normal=norm((.055*t*math.cos(angle),1.,.077*t*math.sin(angle)))
            verts.append(((x,height,z),normal,(x/3+.5,z/3+.5)))
    triangles=[]
    for i in range(sectors):
        nxt=(i+1)%sectors
        triangles.extend((0,1+nxt,1+i))
    for ring in range(rings-1):
        inner=1+ring*sectors
        outer=inner+sectors
        for i in range(sectors):
            nxt=(i+1)%sectors
            triangles.extend((inner+i,inner+nxt,outer+i,
                              inner+nxt,outer+nxt,outer+i))
    return name,verts,triangles
meshes.append(snow_drift("IrregularSnowPatch"))
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
            verts.append((tuple(98.*q for q in n),tuple(-q for q in n),(u,v)))
    for j in range(rings):
        for k in range(detail):
            a=j*(detail+1)+k
            idx.extend((a,a+1,a+detail+1,a+1,a+detail+2,a+detail+1))
    return "OvercastSkyDome",verts,idx
meshes.append(cloud_dome())
# One metre of flowing meltwater, with real narrow geometry and wet UVs.
meshes.append(surface("RunoffStrip",1,16,lambda u,v:(
    ((u-.5)*1.15,.003+.002*math.sin(math.tau*u*3),
     (v-.5)*.10*(.78+.22*math.sin(math.tau*u*5))),
    (0,1,0),(u*2,v))))
# Low-detail variants retain outer silhouettes, not merely bounding boxes.
lodDeciduous=[tube("",[(0,0,0),(0,2.4,0),(.11,5.1,-.07),(.19,8.0,-.09)],
                   [.34,.27,.18,.075],7)]
for k in range(6):
    a=k*math.tau/6.0
    y=3.4+(k%3)*1.1
    end=(2.2*math.cos(a),y+2.6,2.2*math.sin(a))
    lodDeciduous.append(tube("",[(0,y,0),(end[0]*.65,y+1.2,end[2]*.65),end],
                             [.11,.055,.014],5))
meshes.append(merge("DeciduousLODBranches",lodDeciduous))
meshes.append(lathe("ConiferLODCrown",[(0,.29),(1.6,.22),(2.1,2.4),
      (2.3,.9),(3.0,2.0),(3.4,.65),(4.0,1.7),(4.6,.5),(5.4,1.2),
      (6.0,.35),(7.1,.7),(8.6,0)],16))
meshes.append(merge("LampLODSilhouette",[
    lathe("",[(0,.21),(.2,.18),(6.3,.08)],10),
    tube("",[(0,6.2,0),(.3,6.5,0),(1.6,6.38,0)],
         [.08,.07,.05],7)]))
vinesLod=[]
for k in range(6):
    a=math.tau*k/6
    path=[(.15*math.cos(a),0,.03),(math.cos(a)*.65,1.1,.03),
          (math.cos(a)*1.4,2.2,.03),(math.cos(a)*1.8,3.05,.03)]
    vinesLod.append(tube("",path,[.025,.018,.011,.006],4))
meshes.append(merge("VinesLODBranches",vinesLod))
# Forest soil covers the parcel, grass covers the complete underlying tile.
meshes.append(conform(surface("BlockTerrain",40,40,lambda u,v:(
    (((u-.5)*100.,-.018,(v-.5)*100.),(0,1,0),(u*25,v*25)))),(60.,60.)))
meshes.append(conform(road("RoadNorth108"),(0.,60.),math.pi*.5))
meshes.append(conform(surface("YellowNorthStripe",1,128,
    lambda u,v: (((u-.5)*108,.032,(v-.5)*.14),(0,1,0),(u*72,v*.14/1.5))),
    (0.,60.),math.pi*.5))
meshes.append(conform(sidewalk_slabs("SidewalkEastNegative"),(60.,-7.)))
meshes.append(conform(sidewalk_slabs("SidewalkNorthPositive"),(7.,60.),math.pi*.5))
meshes.append(conform(sidewalk_slabs("SidewalkNorthNegative"),(-7.,60.),math.pi*.5))
meshes.append(conform(curb_stones("CurbEastNegative"),(60.,-6.07)))
meshes.append(conform(curb_stones("CurbNorthPositive"),(6.07,60.),math.pi*.5))
meshes.append(conform(curb_stones("CurbNorthNegative"),(-6.07,60.),math.pi*.5))
meshes.append(conform(surface("ContinuousGround",48,48,lambda u,v:(
    (((u-.5)*120.,-.035,(v-.5)*120.),(0,1,0),
     (u*120./.8,v*120./.8)))),(60.,60.)))
# Cut the soil away under the original 8 x 8 m lab floor, preserving
# its existing physical slab and furniture at y=0. Tile (0,-1) starts
# at (-16,-107), so the building occupies x 11.75..20.25 and
# z 102.75..111.25 in tile coordinates.
def laboratory_ground():
    parts=[]
    for xa,xb,za,zb in ((0.,11.75,0.,120.),
                        (20.25,120.,0.,120.),
                        (11.75,20.25,0.,102.75),
                        (11.75,20.25,111.25,120.)):
        columns=max(1,math.ceil((xb-xa)/2.5))
        rows=max(1,math.ceil((zb-za)/2.5))
        parts.append(surface("",rows,columns,lambda u,v:(
            ((xa+(xb-xa)*u-60.,-.035,za+(zb-za)*v-60.),
             (0,1,0),((xa+(xb-xa)*u)/.8,(za+(zb-za)*v)/.8))))
    return conform(merge("ContinuousGroundLab",parts),(60.,60.))

meshes.append(laboratory_ground())
assert len(meshes)==34

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
