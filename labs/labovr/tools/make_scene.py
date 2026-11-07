from pathlib import Path
import json,struct,math
root=Path(__file__).resolve().parent.parent/'assets'
root.mkdir(exist_ok=True)
meshes=[]
v=[];idx=[]
faces=[((1,0,0),(0,0,-1),(0,1,0)),((-1,0,0),(0,0,1),(0,1,0)),((0,1,0),(1,0,0),(0,0,-1)),((0,-1,0),(1,0,0),(0,0,1)),((0,0,1),(1,0,0),(0,1,0)),((0,0,-1),(-1,0,0),(0,1,0))]
for n,u,w in faces:
 b=len(v)
 for x,y in [(-1,-1),(1,-1),(1,1),(-1,1)]:v.append((tuple((n[k]+x*u[k]+y*w[k])*.5 for k in range(3)),n,((x+1)*.5,(y+1)*.5)))
 idx.extend([b,b+1,b+2,b,b+2,b+3])
meshes.append(('Box',v,idx))
v=[];idx=[]
for y in range(13):
 theta=math.pi*y/12
 for x in range(25):
  phi=2*math.pi*x/24;n=(math.sin(theta)*math.cos(phi),math.cos(theta),math.sin(theta)*math.sin(phi));v.append((tuple(c*.5 for c in n),n,(x/24,y/12)))
for y in range(12):
 for x in range(24):
  a=y*25+x;b=a+1;c=a+25;d=c+1;idx.extend([a,b,c,b,d,c])
meshes.append(('Sphere',v,idx))
v=[];idx=[]
for z in [-.5,.5]:
 for i in range(25):
  a=2*math.pi*i/24;n=(math.cos(a),math.sin(a),0);v.append(((n[0]*.5,n[1]*.5,z),n,(i/24,z+.5)))
for i in range(24):idx.extend([i,i+25,i+1,i+1,i+25,i+26])
for z,sign in [(-.5,-1),(.5,1)]:
 center=len(v);v.append(((0,0,z),(0,0,sign),(.5,.5)))
 for i in range(25):
  a=2*math.pi*i/24;x=math.cos(a)*.5;y=math.sin(a)*.5;v.append(((x,y,z),(0,0,sign),(x+.5,y+.5)))
 for i in range(24):idx.extend([center,center+1+i,center+2+i] if sign>0 else [center,center+2+i,center+1+i])
meshes.append(('Cylinder',v,idx))
data=bytearray();views=[];access=[];out=[]
def add(values,fmt,kind,component,minimum=None,maximum=None):
 while len(data)%4:data.append(0)
 offset=len(data)
 for a in values:data.extend(struct.pack('<'+fmt,*a))
 vi=len(views);views.append({'buffer':0,'byteOffset':offset,'byteLength':len(data)-offset})
 a={'bufferView':vi,'componentType':component,'count':len(values),'type':kind}
 if minimum is not None:a['min']=minimum;a['max']=maximum
 access.append(a);return len(access)-1
for name,verts,indices in meshes:
 p=add([a[0] for a in verts],'3f','VEC3',5126,[-.5]*3,[.5]*3);n=add([a[1] for a in verts],'3f','VEC3',5126);uv=add([a[2] for a in verts],'2f','VEC2',5126);i=add([(a,) for a in indices],'I','SCALAR',5125)
 out.append({'name':name,'primitives':[{'attributes':{'POSITION':p,'NORMAL':n,'TEXCOORD_0':uv},'indices':i,'mode':4}]})
(root/'primitives.bin').write_bytes(data)
(root/'primitives.gltf').write_text(json.dumps({'asset':{'version':'2.0','generator':'LaboVR'},'buffers':[{'uri':'primitives.bin','byteLength':len(data)}],'bufferViews':views,'accessors':access,'meshes':out,'nodes':[{'mesh':i} for i in range(3)],'scenes':[{'nodes':[0,1,2]}],'scene':0},separators=(',',':')))
