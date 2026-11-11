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
def road_shape(x,z,width=12,variant=0):
    # Physical millimetre-level road profile, distinct from the terrestrial
    # altitude. Four separate wheelpaths follow the two traffic lanes.
    # ~23-cm FWHM ruts, 7.5-mm depth, 1.5-m wheel gauge.
    crown=.042*(z/(width*.5))**2
    drift=.045*math.sin(x*.041+variant*.27)+.018*math.sin(x*.19)
    ruts=sum(math.exp(-((z-(lane+wheel+drift))/.10)**2)
             for lane in (-3.,3.) for wheel in (-.76,.76))
    x0=(-31.,-12.,17.,37.)[variant%4]
    z0=(-2.4,2.7,-3.15,2.9)[variant%4]
    dx=(x-x0)/(.62+.12*(variant%3))
    dz=(z-z0)/(.42+.07*(variant%2))
    r=math.sqrt(dx*dx+dz*dz)
    hole=-.034*math.exp(-(r/.69)**4)
    broken_lip=.0042*math.exp(-((r-.95)/.20)**2)
    settled=-.009*math.exp(-((x+18.-variant*5.)/9.)**2
                           -((z-1.4)/1.65)**2)
    patch=.003*math.sin(x*.27+z*.41+variant)*math.exp(-((x-12.)/16.)**2)
    return crown-.0075*ruts+hole+broken_lip+settled+patch

def road(name,width=12,length=108,variant=0):
    def point(u,v):
        x=(u-.5)*length
        z=(v-.5)*width
        h=.032
        height=road_shape(x,z,width,variant)
        dx=(road_shape(x+h,z,width,variant)-road_shape(x-h,z,width,variant))/(2*h)
        dz=(road_shape(x,z+h,width,variant)-road_shape(x,z-h,width,variant))/(2*h)
        return (x,height,z),norm((-dx,1.,-dz)),(x/3.0,z/3.0)
    return surface(name,96,160,point)

def repair_mesh(name):
    # Polygonal resurfacing areas follow exactly the road's tyre ruts.
    parts=[]
    for phase,(xa,xb,za,zb) in enumerate((
            (-39.,-13.,-4.6,-1.15),(14.,35.,1.25,4.75))):
        def point(u,v):
            x=xa+(xb-xa)*u
            jitter=.075*math.sin(x*.81+phase)+.036*math.cos(x*1.9)
            z=(za+jitter)*(1.-v)+(zb+jitter)*v
            return (x,road_shape(x,z)+.0035,z),(0,1,0),(x/3.,z/3.)
        parts.append(surface("",12,56,point))
    return merge(name,parts)
# Same tile-relative height profile as UrbanLayout::GroundHeight.
def ground_height(x,z):
    k=math.tau/120.
    return (.0025*(x+z)
            +.085*(1.-math.cos(k*x))+.055*(1.-math.cos(k*z))
            +.025*(1.-math.cos(4*k*x))+.018*(1.-math.cos(4*k*z))
            +.006*math.sin(8*k*x)*math.sin(8*k*z))

def conform(mesh,center,yaw=0.,normal_displacement=False):
    """Conform earth-grade and road-specific relief in independent layers.

    When normal_displacement is true, local road microrelief follows the
    local terrain normal (for future imported height sources), rather than
    becoming an always-vertical offset from world Y.
    """
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
        if normal_displacement:
            denominator=math.sqrt(1.+gx*gx+gz*gz)
            position=(x-y*gx/denominator,
                      height+y/denominator,
                      z-y*gz/denominator)
        else:
            position=(x,y+height,z)
        result.append((position,norm((nx-gx*ny,ny,nz-gz*ny)),uv))
    return name,result,indices

def sidewalk_slabs(name):
    # One supporting structure; every worn slab has a height field, not a
    # floating planar decal. Joints and differential settlement remain metric.
    parts=[box("",(-54.,-.12,-1.),(54.,.123,1.),
               tile_metres=(1.5,1.5,1.5))]
    for i in range(72):
        a=-54.+i*1.5+.010
        b=-54.+(i+1)*1.5-.010
        def point(u,v):
            x=a+(b-a)*u
            z=-1.+2.*v
            settled=-.006*math.sin(i*1.191)**2
            heave=.0035*math.sin(i*.67+2.5*u)*math.sin(math.pi*v)
            crack=-.0028*math.exp(-((u-(.3+.35*math.sin(i*3.1)))/.018)**2)
            y=.141+settled+heave+crack
            return (x,y,z),(0.,1.,0.),(x/1.5,z/1.5)
        parts.append(surface("",4,6,point))
    return merge(name,parts)

def curb_stones(name):
    parts=[box("",(-54.,-.15,-.115),(54.,.155,.115),
               tile_metres=(1.5,1.5,1.5))]
    for i in range(54):
        a=-54.+i*2.+.012
        b=-54.+(i+1)*2.-.012
        def point(u,v):
            x=a+(b-a)*u
            z=-.115+.23*v
            y=.171-.004*math.sin(i*1.73)**2+.001*math.sin(u*math.tau)
            return (x,y,z),(0.,1.,0.),(x/1.5,z/1.5)
        parts.append(surface("",3,5,point))
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
# Masonry with deep bays, pilasters, a projecting continuous cap and
# an irregular parapet. Geometry describes masonry mass; brick bond
# and mortar grain remain in the metric PBR maps rather than thousands
# of microscopic boxes submitted as separate meshlets.
wall_parts=[
    box("",(-50.,-.10,-.19),(50.,2.25,.19),
        tile_metres=(1.76,1.04,1.76)),
    box("",(-50.,2.24,-.27),(50.,2.37,.27),
        tile_metres=(1.76,1.04,1.76))
]
for k in range(13):
    x=-48.+k*8.
    wall_parts.append(box("",(x-.235,-.08,-.285),
                              (x+.235,2.29,.285),
                              tile_metres=(1.76,1.04,1.76)))
    wall_parts.append(box("",(x-.305,2.27,-.335),
                              (x+.305,2.43,.335),
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
                  2.20*t,rng.uniform(.025,.055)))
    vine.append(tube("",p,[.020*(1-step/12) for step in range(11)],6))
meshes.append(merge("WallVines",vine))
leaves=[]
for branch in range(95):
    y=.18+2.04*rng.random()
    x=rng.uniform(-1.9,1.9)
    size=.09+rng.random()*.14
    leaves.append(quad("",[(x-size,y,.06),(x,y+size,.07),
                            (x+size,y,.06),(x,y-size,.05)],
                       [(0,0),(.5,1),(1,0),(.5,0)]))
meshes.append(merge("ResidualRedVineLeaves",leaves))
# A branched winter skeleton: woody forks share tapered geometry, with
# stable but nonrepeating azimuths. Bare seasonal trees need no floating
# alpha quads to imply a canopy.
branches=[tube("",[(0.,0.,0.),(.025,2.2,.01),(.12,4.9,-.08),
                  (.23,7.8,-.17)],[.36,.29,.17,.065],10)]
for k in range(21):
    a=math.tau*k*.61803398875
    h=2.8+(k%8)*.56
    reach=1.7+.16*(k%6)
    dx,dz=math.cos(a),math.sin(a)
    origin=(.05,h,0.)
    elbow=(.17+dx*.47,h+.55,dz*.47)
    tip=(.17+dx*reach,h+1.8+.16*(k%3),dz*reach)
    branches.append(tube("",[origin,elbow,tip],
                         [.096,.052,.009],6))
    for fork in range(2):
        twist=a+(-.39 if fork==0 else .42)
        end=(tip[0]+.65*math.cos(twist),tip[1]+.65+.10*(k%3),
             tip[2]+.65*math.sin(twist))
        branches.append(tube("",[elbow,tip,end],
                             [.041,.018,.0035],5))
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
def needle_bud(center,direction,length,radius):
    # A closed eight-triangle needle tuft. Unlike crossed paper cards it
    # has a volumetric silhouette and an actual face normal on every facet.
    tangent=norm(direction)
    reference=(0.,1.,0.) if abs(tangent[1])<.9 else (1.,0.,0.)
    right=norm(cross(tangent,reference))
    up=norm(cross(tangent,right))
    tip=add(center,scale(tangent,length))
    root=add(center,scale(tangent,-length*.28))
    rim=[add(center,add(scale(right,math.cos(math.tau*k/4)*radius),
                        scale(up,math.sin(math.tau*k/4)*radius)))
         for k in range(4)]
    verts=[];triangles=[]
    for k in range(4):
        for corners in ((tip,rim[k],rim[(k+1)%4]),
                        (root,rim[(k+1)%4],rim[k])):
            e0=tuple(corners[1][j]-corners[0][j] for j in range(3))
            e1=tuple(corners[2][j]-corners[0][j] for j in range(3))
            normal=norm(cross(e0,e1))
            first=len(verts)
            verts.extend((point,normal,
                          ((point[0]-center[0])/.6+.5,
                           (point[1]-center[1])/.6+.5))
                         for point in corners)
            triangles.extend((first,first+1,first+2))
    return "",verts,triangles

pine=[tube("",[(0.,0.,0.),(0.,2.,0.),(0.,8.6,0.)],
           [.29,.23,.012],10)]
for level in range(11):
    y=1.16+level*.67
    reach=2.8*(1.-level/12.4)
    for arm in range(8):
        angle=math.tau*(arm/8.+level*.071)
        dx,dz=math.cos(angle),math.sin(angle)
        root=(.04*dx,y+.13,.04*dz)
        elbow=(reach*.45*dx,y-.12,reach*.45*dz)
        end=(reach*dx,y-.52,reach*dz)
        pine.append(tube("",[root,elbow,end],
                         [.055,.032,.006],5))
        for fraction in (.34,.70):
            point=(reach*fraction*dx,
                   y+.13-.65*fraction,
                   reach*fraction*dz)
            axis=(dx*.74,-.22,dz*.74)
            pine.append(needle_bud(point,axis,
                         .95*(1.-level/16.),.24*(1.-level/18.)))
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
def organic_puddle(name):
    # Irregular nested contours: no rectangular puddle or hard polygon ends.
    sectors=40
    rings=5
    vertices=[((0.,.003,0.),(0.,1.,0.),(1.,1.))]
    indices=[]
    def contour(angle):
        return (1.0+.115*math.sin(5*angle+.4)+.065*math.sin(11*angle+1.7),
                1.0+.10*math.cos(7*angle+.8)+.07*math.sin(13*angle+.2))
    for ring in range(1,rings+1):
        radius=ring/rings
        for k in range(sectors):
            angle=math.tau*k/sectors
            rx,rz=contour(angle)
            x=1.5*radius*rx*math.cos(angle)
            z=.90*radius*rz*math.sin(angle)
            y=.003+.0008*math.sin(x*3.+z*2.)
            vertices.append(((x,y,z),(0.,1.,0.),(1.+x/1.5,1.+z/.9)))
    for k in range(sectors):
        nxt=(k+1)%sectors
        indices.extend((0,1+nxt,1+k))
    for ring in range(1,rings):
        inner=1+(ring-1)*sectors
        outer=inner+sectors
        for k in range(sectors):
            nxt=(k+1)%sectors
            indices.extend((inner+k,inner+nxt,outer+k,
                            inner+nxt,outer+nxt,outer+k))
    return name,vertices,indices
meshes.append(organic_puddle("ShallowPuddle"))
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
# Converging metric runoff: a single mesh with feeding channels instead of
# sixteen separately translated rectangles per grate. Geometry moves no
# faster than the hydrology; only its normals are advected in the shader.
def drain_runoff(name):
    branches=(
        (-5.2,-1.12,-1.45,-.22,.105,1.2),
        (-4.65,1.18,-1.35,.22,.100,2.8),
        (-3.60,-1.70,-.96,-.14,.081,4.3),
        (-3.35,1.72,-.91,.14,.085,5.9),
        (-2.65,-.58,-1.12,-.09,.072,7.1),
        (-2.3,.75,-.91,.06,.076,8.6),
        (-2.3,0.,.32,0.,.26,9.3))
    vertices=[]
    triangles=[]
    for first_x,first_z,last_x,last_z,base_width,key in branches:
        first=len(vertices)
        steps=40
        for i in range(steps+1):
            t=i/steps
            blend=t*t*(3.-2.*t)
            x=first_x+(last_x-first_x)*t
            z=first_z+(last_z-first_z)*blend
            z+=math.sin(math.pi*t)*(.17*math.sin(t*7.7+key)+
                                     .055*math.sin(t*19.1+key*.57))
            widening=(.32+.96*blend)
            if key>9:
                widening=.66+.44*blend
            width=base_width*widening*(.94+.10*math.sin(t*23.+key))
            # Conform to road camber from the curb inlet (5.65 m) toward
            # the centre line. Zero denotes the inlet's road surface height.
            lateralRoad=max(0.,5.65+x)
            crownDelta=.042*((lateralRoad/6.)**2-(5.65/6.)**2)
            y=crownDelta+.003+.0009*math.sin(t*17.2+key)
            for side in (-1,1):
                zz=z+width*side*(1.+.14*math.sin(t*13.+key+side))
                vertices.append(((x,y,zz),(0.,1.,0.),(x/1.5,zz/1.5)))
            if i<steps:
                a=first+2*i
                triangles.extend((a,a+1,a+2,a+1,a+3,a+2))
    return name,vertices,triangles
meshes.append(drain_runoff("RunoffStrip"))
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
             (0,1,0),((xa+(xb-xa)*u)/.8,(za+(zb-za)*v)/.8)))))
    return conform(merge("ContinuousGroundLab",parts),(60.,60.))

meshes.append(laboratory_ground())
meshes.append(conform(repair_mesh("RoadRepairEast"),(60.,0.)))
meshes.append(conform(repair_mesh("RoadRepairNorth"),(0.,60.),math.pi*.5))
# A small library of physically different roads avoids identical potholes
# repeating every 120-m block. Each remains an independent shared mesh.
for variant in range(4):
    meshes.append(conform(road(f"RoadEastVariant{variant}",variant=variant),
                         (60.,0.),normal_displacement=True))
for variant in range(4):
    meshes.append(conform(road(f"RoadNorthVariant{variant}",variant=variant),
                         (0.,60.),math.pi*.5,normal_displacement=True))

def edge_markings(name):
    parts=[]
    for z in (-5.44,5.44):
        parts.append(surface("",1,160,lambda u,v:(
            (((u-.5)*108.,road_shape((u-.5)*108.,z)+.004,z+(v-.5)*.12),
             (0.,1.,0.),((u-.5)*108./1.5,(z+(v-.5)*.12)/1.5)))))
    return merge(name,parts)
meshes.append(conform(edge_markings("EdgeLinesEast"),(60.,0.),
                     normal_displacement=True))
meshes.append(conform(edge_markings("EdgeLinesNorth"),(0.,60.),math.pi*.5,
                     normal_displacement=True))

# The traffic hardware uses authored metric dimensions. STOP P-10 face:
# 750 mm across flats, fitted on a 70-mm galvanized roadside post.
def octagonal_face(name,z=-.028,radius=.406):
    center=(0.,2.49,z)
    vert=[(center,(0.,0.,-1.),(.5,.5))]
    for i in range(8):
        angle=math.tau*(i+.5)/8.
        x=radius*math.cos(angle)
        y=radius*math.sin(angle)+2.49
        vert.append(((x,y,z),(0.,0.,-1.),
                     (.5+x/.812,.5+(y-2.49)/.812)))
    ix=[]
    for i in range(8):
        ix.extend((0,1+(i+1)%8,1+i))
    return name,vert,ix

bitmap={
 'S':("01111","10000","10000","01110","00001","00001","11110"),
 'T':("11111","00100","00100","00100","00100","00100","00100"),
 'O':("01110","10001","10001","10001","10001","10001","01110"),
 'P':("11110","10001","10001","11110","10000","10000","10000"),
 'C':("01111","10000","10000","10000","10000","10000","01111"),
 'A':("01110","10001","10001","11111","10001","10001","10001"),
 'N':("10001","11001","10101","10101","10011","10001","10001"),
 'D':("11110","10001","10001","10001","10001","10001","11110"),
 'E':("11111","10000","10000","11110","10000","10000","11111"),
}
def face_text(name,string,centerY,z,pixel):
    parts=[]
    textWidth=(len(string)*6.-1.)*pixel
    x0=-textWidth*.5
    for k,letter in enumerate(string):
        if letter not in bitmap:continue
        for j,row in enumerate(bitmap[letter]):
            for i,cell in enumerate(row):
                if cell!='1':continue
                x=x0+(k*6+i)*pixel
                y=centerY+(3-j)*pixel
                parts.append(quad("",[(x,y,z),(x,y+pixel*.94,z),
                        (x+pixel*.94,y+pixel*.94,z),
                        (x+pixel*.94,y,z)],
                        [(0,0),(0,1),(1,1),(1,0)]))
    return merge(name,parts)
meshes.append(merge("StopPost",[
    tube("",[(0.,-.12,.09),(0.,1.62,.09),(0.,3.02,.09)],
         [.044,.038,.038],12),
    box("",(-.045,1.88,.015),(.045,1.94,.12)),
    box("",(-.045,2.80,.015),(.045,2.86,.12))
]))
meshes.append(octagonal_face("StopFace"))
border=[]
for i in range(8):
    a=math.tau*(i+.5)/8.
    b=math.tau*(i+1.5)/8.
    def p(rad,angle):
        return (rad*math.cos(angle),2.49+rad*math.sin(angle),-.039)
    border.append(quad("",[p(.385,a),p(.355,a),
                           p(.355,b),p(.385,b)],
                       [(0,0),(0,1),(1,1),(1,0)]))
border.append(face_text("STOP","STOP",2.485,-.043,.025))
meshes.append(merge("StopWhiteMarkings",border))

# Mueller-style dry-barrel hydrant, approximately 1.07 m exposed height,
# separate upper bonnet, cap hardware and side outlet flanges.
meshes.append(merge("HydrantBody",[
    lathe("",[(0,.19),(.055,.21),(.12,.16),(.20,.155),(.67,.15),
              (.75,.196),(.85,.175),(.96,.137),(1.04,.04)],24),
    tube("",[(-.32,.58,0),(-.15,.58,0),(.15,.58,0),(.32,.58,0)],
         [.105,.12,.12,.105],12),
    tube("",[(0,.62,-.29),(0,.62,-.13),(0,.62,.12)],
         [.103,.114,.113],12)
]))
meshes.append(merge("HydrantFittings",[
    lathe("",[(.77,.195),(.79,.216),(.835,.210)],24),
    lathe("",[(1.02,.075),(1.11,.07)],8),
    tube("",[(-.37,.58,0),(-.32,.58,0)],[.124,.124],10),
    tube("",[(.32,.58,0),(.37,.58,0)],[.124,.124],10),
    tube("",[(0,.62,-.34),(0,.62,-.29)],[.13,.13],10)
]))

# Red street letterbox with a curved upper shoulder, mail slot, raised frame
# and dark ribbed pedestal. Approximate physical dimensions 62 x 48 x 139 cm.
meshes.append(merge("PostesBoxBody",[
    box("",(-.31,.18,-.24),(.31,1.26,.24)),
    box("",(-.298,1.26,-.225),(.298,1.37,.225)),
    box("",(-.27,.09,-.22),(.27,.18,.22)),
    box("",(-.286,1.15,-.259),(.286,1.20,-.242)),
    box("",(-.22,.30,-.255),(.22,.345,-.239))
]))
meshes.append(merge("PostesBoxBluePanel",[
    box("",(-.295,.95,-.265),(.295,1.10,-.253)),
    box("",(-.270,.70,-.250),(.270,.717,-.240))
]))
meshes.append(face_text("PostesBoxLetters","POSTESCANADA",1.047,
                        -.269,.0068))

# Pole-mounted green 35-litre basket: open slatted barrel, galvanized
# mounting straps and rim, no opaque cylinder pretending to have openings.
meshes.append(merge("UrbanBasketPole",[
    tube("",[(0.,-.07,0.),(0.,.75,0.),(0.,2.02,0.)],
         [.042,.039,.039],12),
    box("",(-.035,1.00,-.28),(.035,1.06,-.035)),
    box("",(-.035,1.27,-.28),(.035,1.33,-.035))
]))
def move_mesh(mesh,offset):
    name,verts,triangles=mesh
    return name,[(add(p,offset),normal,uv) for p,normal,uv in verts],triangles
basket=[move_mesh(lathe("",[(.90,.172),(.925,.187)],20),(0.,0.,-.30)),
        move_mesh(lathe("",[(1.33,.194),(1.352,.194)],20),(0.,0.,-.30))]
for i in range(20):
    a=math.tau*i/20.
    x=.184*math.cos(a);z=-.30+.184*math.sin(a)
    basket.append(box("",(x-.009,.925,z-.010),
                           (x+.009,1.33,z+.010)))
basket.append(box("",(-.165,.90,-.465),(.165,.925,-.135)))
meshes.append(merge("UrbanBasketGreenSlats",basket))

# Manhole cover: continuous top, cast-iron peripheral rings and
# discrete reinforcing ribs. Do not leave a hollow ring pretending to be
# a solid municipal utility cover.
def manhole_disk():
    sectors=48
    verts=[((0.,.004,0.),(0.,1.,0.),(.5,.5))]
    tris=[]
    for i in range(sectors):
        angle=math.tau*i/sectors
        x=.335*math.cos(angle)
        z=.335*math.sin(angle)
        verts.append(((x,.004,z),(0.,1.,0.),(.5+x/.67,.5+z/.67)))
        tris.extend((0,1+(i+1)%sectors,1+i))
    return "",verts,tris
meshes.append(merge("ManholeCover",[
    manhole_disk(),
    lathe("",[(-.022,.355),(-.006,.36),(0.,.335),(.004,.31)],32),
    *[box("",(-.28,-.004,-.012+i*.073),(.28,.004,.004+i*.073))
      for i in range(4)],
    *[box("",(-.28,-.004,-.012-i*.073),(.28,.004,.004-i*.073))
      for i in range(1,4)]
]))
meshes.append(lathe("ManholeSeat",[
    (-.033,.39),(-.015,.39),(-.009,.35)],32))
assert len(meshes)==58


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
