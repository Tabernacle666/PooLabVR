from pathlib import Path
import json, math, struct

root = Path(__file__).resolve().parent.parent / 'assets'
meshes = []

def norm(v):
    length = math.sqrt(sum(x*x for x in v)) or 1
    return tuple(x/length for x in v)

def surface(name, rows, cols, point):
    vertices, indices = [], []
    for j in range(rows+1):
        for i in range(cols+1):
            u, v = i/cols, j/rows
            p, n = point(u, v)
            vertices.append((p, norm(n), (u, v)))
    for j in range(rows):
        for i in range(cols):
            a = j*(cols+1)+i
            indices.extend((a, a+cols+1, a+1, a+1, a+cols+1, a+cols+2))
    return (name, vertices, indices)

def ellipsoid(name, dimensions, detail=0):
    def point(u, v):
        t, a = math.pi*v, 2*math.pi*u
        n = (math.sin(t)*math.cos(a), math.cos(t), math.sin(t)*math.sin(a))
        ripple = 1 + detail*math.sin(14*a)*math.sin(t)**2
        return tuple(n[k]*dimensions[k]*ripple for k in range(3)), tuple(n[k]/dimensions[k] for k in range(3))
    name, vertices, indices = surface(name, 24, 48, point)
    indices = [indices[i+k] for i in range(0,len(indices),3) for k in (0,2,1)]
    return name, vertices, indices

def lathe(name, profile):
    slopes = [norm((profile[i+1][0]-profile[i][0], -(profile[i+1][1]-profile[i][1]))) for i in range(len(profile)-1)]
    normals = [slopes[0]]+[norm((slopes[i-1][0]+slopes[i][0],slopes[i-1][1]+slopes[i][1])) for i in range(1,len(profile)-1)]+[slopes[-1]]
    def point(u, v):
        s = min(len(profile)-2, int(v*(len(profile)-1)))
        t = v*(len(profile)-1)-s
        y = profile[s][0]*(1-t)+profile[s+1][0]*t
        r = profile[s][1]*(1-t)+profile[s+1][1]*t
        a = u*2*math.pi
        dy = profile[s+1][0]-profile[s][0]
        dr = profile[s+1][1]-profile[s][1]
        nx = normals[s][0]*(1-t)+normals[s+1][0]*t
        ny = normals[s][1]*(1-t)+normals[s+1][1]*t
        return (r*math.cos(a), y, r*math.sin(a)), (nx*math.cos(a), ny, nx*math.sin(a))
    return surface(name, (len(profile)-1)*3, 48, point)

def merge(name, parts):
    verts, idx = [], []
    for _, v, i in parts:
        base = len(verts)
        verts.extend(v)
        idx.extend(base+k for k in i)
    return name, verts, idx

def tube(points, radius):
    vertices, indices = [], []
    for k, p in enumerate(points):
        tangent = norm(tuple(points[min(k+1,len(points)-1)][i]-points[max(k-1,0)][i] for i in range(3)))
        a = norm((-tangent[1],tangent[0],0)) if abs(tangent[2])<.9 else (1,0,0)
        b = (tangent[1]*a[2]-tangent[2]*a[1],tangent[2]*a[0]-tangent[0]*a[2],tangent[0]*a[1]-tangent[1]*a[0])
        for j in range(9):
            angle = j*math.pi/4
            n = tuple(a[i]*math.cos(angle)+b[i]*math.sin(angle) for i in range(3))
            vertices.append((tuple(p[i]+radius*n[i] for i in range(3)), n, (j/8,k/max(1,len(points)-1))))
    for k in range(len(points)-1):
        for j in range(8):
            a = k*9+j
            indices.extend((a,a+1,a+9,a+1,a+10,a+9))
    return '',vertices,indices

# Geometry order is frozen by HomePhysicsMesh in HomePhysics.cpp.
path = []
for i in range(65):
    a = 2*math.pi*i/64
    path.append((.30*math.cos(a), .46*math.sin(a)+(.15 if math.sin(a)>0 else -.15), 0))
meshes.append(merge('ForgedOvalLink',[tube(path,.075)]))
socket = [(-.5,0),(-.5,.36),(-.38,.39),(-.34,.33)]
for i in range(9):
    y = -.3+i*.075
    socket.extend(((y,.34),(y+.018,.38),(y+.037,.34)))
socket.extend(((.45,.38),(.5,.46),(.54,.46),(.54,0)))
meshes.append(lathe('RibbedBrassSocket',socket))
meshes.append(lathe('OpalGlassBulb',[(-.78,0),(-.76,.16),(-.70,.29),(-.55,.42),(-.32,.5),(-.08,.51),(.13,.46),(.28,.34),(.38,.21),(.54,.18),(.57,0)]))
meshes.append(ellipsoid('SculptedThorax',(.45,.38,.60),.025))
meshes.append(ellipsoid('HeadCarapace',(.48,.4,.38),.015))
meshes.append(ellipsoid('SegmentedAbdomen',(.38,.34,.72),.045))
meshes.append(ellipsoid('FacetedCompoundEye',(.45,.5,.33),.06))

for side in (-1,1):
    vertices, indices = [], []
    outline = []
    for i in range(49):
        a = 2*math.pi*i/48
        x = -.45 + .52*(1-math.cos(a))
        z = .29*math.sin(a)*(1+.22*math.cos(a))
        outline.append((side*x, .012*math.sin(a), z))
    vertices.append(((0,0,0),(0,1,0),(.5,.5)))
    for p in outline:
        vertices.append((p,(0,1,0),(p[0]+.5,p[2]+.5)))
    for i in range(48):
        triangle = (0,i+1,i+2)
        indices.extend(triangle if side==1 else triangle[::-1])
    meshes.append(('LeftMembrane' if side<0 else 'RightMembrane', vertices, indices))
    veins = [tube(outline,.009)]
    hinge = (-side*.45,0,0)
    for end in (6,12,19,25,32,39):
        tip = outline[end]
        veins.append(tube([tuple(hinge[k]*(1-t)+tip[k]*t for k in range(3)) for t in (0,.25,.5,.75,1)],.006))
    meshes.append(merge('LeftWingVeins' if side<0 else 'RightWingVeins',veins))

meshes.append(merge('JointedLeg',[tube([(0,.5,0),(.04,.32,0),(0,-.42,0),(0,-.5,0)],.065)]))
meshes.append(merge('TarsalHook',[tube([(0,.12,0),(0,0,0),(.1,-.09,.05),(.12,-.1,.17),(.04,-.05,.24)],.022)]))
meshes.append(merge('Antennae',[tube([(-.16,0,.12),(-.2,.15,.3),(-.24,.23,.35)],.024),tube([(.16,0,.12),(.2,.15,.3),(.24,.23,.35)],.024)]))

proxy=surface('PopulationProxy',4,6,lambda u,v: ((.45*math.sin(math.pi*v)*math.cos(2*math.pi*u),.28*math.cos(math.pi*v),.8*math.sin(math.pi*v)*math.sin(2*math.pi*u)),(math.sin(math.pi*v)*math.cos(2*math.pi*u),math.cos(math.pi*v),math.sin(math.pi*v)*math.sin(2*math.pi*u))))
proxy=(proxy[0],proxy[1],[proxy[2][i+k] for i in range(0,len(proxy[2]),3) for k in (0,2,1)])
meshes.append(proxy)
# Fly assets use meters. Thorax/abdomen/head offsets in PopulationCuda are
# metric too; the coarse hull spans the same 7 mm total body length.
for k in range(3,len(meshes)):
    name,vertices,indices=meshes[k]
    factor=.004375 if name=='PopulationProxy' else .003
    meshes[k]=(name,[(tuple(x*factor for x in p),n,uv) for p,n,uv in vertices],indices)

# The pot remains hollow: the inside surface and the open rim are visible.
# Its separate PhysX wall segments are attached to the same moving base.
meshes.append(lathe('OpenTerracottaPot',[
    (0,0), (0,.071), (.014,.082), (.155,.105), (.18,.112),
    (.18,.092), (.155,.084), (.020,.064), (.017,0)]))

def faceted_rock():
    points=[(-.85,-.7,-.60),(.72,-.75,-.78),(1.0,-.62,.58),(-.63,-.7,.86),
            (-.68,.64,-.48),(.58,.75,-.56),(.78,.52,.40),(-.53,.66,.65)]
    faces=[(0,2,1),(0,3,2),(4,5,6),(4,6,7),
           (0,1,5),(0,5,4),(1,2,6),(1,6,5),
           (2,3,7),(2,7,6),(3,0,4),(3,4,7)]
    vertices,indices=[],[]
    for face in faces:
        a,b,c=(points[k] for k in face)
        u=tuple(b[i]-a[i] for i in range(3))
        v=tuple(c[i]-a[i] for i in range(3))
        n=norm((u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]))
        for p in (a,b,c):
            indices.append(len(vertices));vertices.append((p,n,(0,0)))
    return 'FacetedRock',vertices,indices
meshes.append(faceted_rock())
data, views, accessors, output = bytearray(), [], [], []
def add(values, fmt, kind, component, bounds=False):
    while len(data)%4:data.append(0)
    start = len(data)
    for value in values:data.extend(struct.pack('<'+fmt,*value))
    view = len(views)
    views.append({'buffer':0,'byteOffset':start,'byteLength':len(data)-start})
    accessor = {'bufferView':view,'componentType':component,'count':len(values),'type':kind}
    if bounds:
        accessor['min']=[min(p[k] for p in values) for k in range(3)]
        accessor['max']=[max(p[k] for p in values) for k in range(3)]
    accessors.append(accessor)
    return len(accessors)-1
for name, vertices, indices in meshes:
    p=add([v[0] for v in vertices],'3f','VEC3',5126,True)
    n=add([v[1] for v in vertices],'3f','VEC3',5126)
    uv=add([v[2] for v in vertices],'2f','VEC2',5126)
    i=add([(v,) for v in indices],'I','SCALAR',5125)
    output.append({'name':name,'primitives':[{'attributes':{'POSITION':p,'NORMAL':n,'TEXCOORD_0':uv},'indices':i,'mode':4}]})
(root/'home_physics.bin').write_bytes(data)
(root/'home_physics.gltf').write_text(json.dumps({'asset':{'version':'2.0','generator':'ConsoleVR'},'buffers':[{'uri':'home_physics.bin','byteLength':len(data)}],'bufferViews':views,'accessors':accessors,'meshes':output},separators=(',',':')))
