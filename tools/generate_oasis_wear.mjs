
/* OASIS contextual wear geometry, original prototypes.
 * node tools/generate_oasis_wear.mjs [output-file]
 * The scene contains 7 named selectable micro-detail meshes, not enabled by default.
 */
function enc(s){const q=unescape(encodeURIComponent(s)),b=new Uint8Array(q.length);for(let i=0;i<q.length;i++)b[i]=q.charCodeAt(i);return b}
function cat(a){let n=a.reduce((q,x)=>q+x.length,0),b=new Uint8Array(n),p=0;for(const x of a){b.set(x,p);p+=x.length}return b}
function w32(n){const b=new Uint8Array(4);new DataView(b.buffer).setUint32(0,n>>>0,true);return b}
function f32(a){const b=new Uint8Array(a.length*4),d=new DataView(b.buffer);a.forEach((v,i)=>d.setFloat32(i*4,v,true));return b}
function u16(a){const b=new Uint8Array(a.length*2),d=new DataView(b.buffer);a.forEach((v,i)=>d.setUint16(i*2,v,true));return b}
function pad(a,fill=0){return a.length%4?cat([a,new Uint8Array(Array(4-a.length%4).fill(fill))]):a}
const mats={
 wall_scuff:{color:[.13,.11,.10,.42],rough:.98,alpha:"BLEND"},
 floor_wear:{color:[.57,.54,.48,.26],rough:.89,alpha:"BLEND"},
 raw_metal:{color:[.33,.35,.36,1],rough:.36,metal:.84,alpha:"OPAQUE"},
 dent_metal:{color:[.49,.51,.52,1],rough:.28,metal:.88,alpha:"OPAQUE"},
 crack:{color:[.042,.035,.033,.91],rough:.92,alpha:"BLEND"},
 rusty:{color:[.36,.19,.1,.49],rough:.94,alpha:"BLEND"},
 pale_wood:{color:[.66,.56,.41,.32],rough:.83,alpha:"BLEND"},
 carpet_fade:{color:[.69,.67,.60,.29],rough:.98,alpha:"BLEND"},
 ceramic_chips:{color:[.70,.67,.60,.95],rough:.72,alpha:"OPAQUE"}
};
const groups=new Map();
function group(name,mat){const key=name+"|"+mat;if(!groups.has(key))groups.set(key,{node:name,mat,pos:[],norm:[],uv:[],idx:[]});return groups.get(key)}
function quad(name,mat,a,b,c,d){
 const g=group(name,mat),off=g.pos.length/3,xx=b.map((v,i)=>v-a[i]),yy=c.map((v,i)=>v-a[i]),
 n=[xx[1]*yy[2]-xx[2]*yy[1],xx[2]*yy[0]-xx[0]*yy[2],xx[0]*yy[1]-xx[1]*yy[0]],len=Math.hypot(...n)||1;
 [[a,[0,0]],[b,[1,0]],[c,[1,1]],[d,[0,1]]].forEach(([p,uv])=>{g.pos.push(...p);g.norm.push(...n.map(v=>v/len));g.uv.push(...uv)});
 g.idx.push(off,off+1,off+2,off,off+2,off+3);
}
function tube(name,mat,a,b,r,N=12){
 const dir=b.map((q,i)=>q-a[i]),L=Math.hypot(...dir)||1,u=dir.map(v=>v/L),q=Math.abs(u[1])<.9?[0,1,0]:[1,0,0],
 s=[u[1]*q[2]-u[2]*q[1],u[2]*q[0]-u[0]*q[2],u[0]*q[1]-u[1]*q[0]],sl=Math.hypot(...s)||1;
 const v=s.map(x=>x/sl),t=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2]],g=group(name,mat),off=g.pos.length/3;
 for(let j=0;j<=1;j++)for(let k=0;k<=N;k++){
   let theta=2*Math.PI*k/N,n=v.map((x,i)=>x*Math.cos(theta)+t[i]*Math.sin(theta)),
       pt=(j?b:a).map((x,i)=>x+n[i]*r);
   g.pos.push(...pt);g.norm.push(...n);g.uv.push(k/N,j);
 }
 for(let k=0;k<N;k++){let a=off+k,b=a+N+1;g.idx.push(a,b,a+1,a+1,b,b+1)}
}
function line(name,mat,points,r=.003){for(let i=1;i<points.length;i++)tube(name,mat,points[i-1],points[i],r,8)}
function buildWear(){
 // Wall scuff at chair-back elevation, anchored tangent to a +Z wall.
 for(let i=0;i<19;i++){
   let x=-.47+i*.047,y=.18+.07*Math.sin(i*1.48)+.002*i;
   let r=.014+.009*(i%4);
   quad("chair_back_wall_scuffs","wall_scuff",[x-r,y-.014,.003],[x+r,y-.018,.003],
     [x+r*.7,y+.023,.003],[x-r*.8,y+.019,.003]);
 }
 for(let i=0;i<7;i++){
   let x=-.42+i*.125;
   quad("chair_back_wall_scuffs","wall_scuff",[x,.04,.002],[x+.06,.04,.002],[x+.035,.44,.002],[x-.014,.44,.002]);
 }
 // Recessed elevator metal skin, actual inward indentation.
 const name="elevator_panel_dent",mat="dent_metal",g=group(name,mat),base=g.pos.length/3,N=20;
 for(let j=0;j<=N;j++)for(let i=0;i<=N;i++){
   const x=(i/N-.5)*.63,y=(j/N-.5)*.93,dx=(x-.07)/.14,dy=(y-.04)/.22,
     f=Math.exp(-2.0*(dx*dx+dy*dy)),z=-.018*f,
     ddx=.036*dx/.14*f,ddy=.036*dy/.22*f,
     n=[-ddx,-ddy,1],l=Math.hypot(...n)||1;
   g.pos.push(x,y,z);g.norm.push(...n.map(q=>q/l));g.uv.push(i/N,j/N);
 }
 for(let j=0;j<N;j++)for(let i=0;i<N;i++){let a=base+j*(N+1)+i,b=a+1,c=a+N+1,d=c+1;g.idx.push(a,b,d,a,d,c)}
 // Flowerpot ceramic microfracture: surface-relative curved dark scar.
 let points=[];for(let i=0;i<=23;i++){
   const a=-.94+i*.043,y=.30-i*.0062+Math.sin(i*2.2)*.003,r=.153;
   points.push([Math.sin(a)*r,y,Math.cos(a)*r+.002]);
 }
 line("pot_ceramic_crack","crack",points,.0021);
 for(let j=0;j<4;j++){let k=4+j*5,p=points[k],q=[p[0]+.007*(j%2?1:-1),p[1]-.035,p[2]+.003];line("pot_ceramic_crack","crack",[p,q],.0018)}
 // One protruding fastener: rare event, real raised geometry.
 tube("rare_protruding_nail","raw_metal",[0,0,0],[0,0,.016],.0024,12);
 tube("rare_protruding_nail","raw_metal",[0,0,.014],[0,0,.020],.0050,12);
 quad("rare_protruding_nail","rusty",[-.013,-.01,.001],[.013,-.01,.001],[.011,.017,.001],[-.013,.015,.001]);
 // Longitudinal scratches, comparable to feet and tracked dirt on parquet.
 for(let j=0;j<29;j++){
   let x=-.30+j*.021,dx=.011*Math.sin(j*2.1);
   quad("parquet_walkway_scratches","pale_wood",[x,0,-1.2],[x+.004,0,-1.2],
     [x+dx+.001,0,1.2],[x+dx-.003,0,1.2]);
 }
 // Carpet flattened centre, small carpet underlay, edges remain original host.
 for(let j=0;j<17;j++){
   let x=-.45+j*.055,r=.020+.008*(j%4);
   quad("carpet_center_fade","carpet_fade",[x-r,.002,-1.25],[x+r,.002,-1.25],
     [x+r*.76,.002,1.25],[x-r*.76,.002,1.25]);
 }
 // Threshold chipping: exposes metal and paint at the contact height.
 for(let i=0;i<28;i++){
   let x=-.35+i*.026,y=.004+.014*Math.abs(Math.sin(i*1.99)),dx=.007+.006*(i%3);
   quad("door_edge_scrape","raw_metal",[x,y,.003],[x+dx,y,.003],
     [x+dx*.8,y+.018,.003],[x-dx*.3,y+.024,.003]);
 }
 const buf=[],views=[],access=[],meshes=[],nodes=[],byNode={};let size=0;
 function bin(a,target){const p=(4-size%4)%4;if(p){buf.push(new Uint8Array(p));size+=p}
   const i=views.length;views.push({buffer:0,byteOffset:size,byteLength:a.length,...(target?{target}:{})});buf.push(a);size+=a.length;return i}
 function acc(a,n,type,component,target,range){const v={bufferView:bin(a,target),componentType:component,count:n,type,...(range||{})};access.push(v);return access.length-1}
 const names=Object.keys(mats),index=Object.fromEntries(names.map((n,i)=>[n,i]));let tris=0;
 for(const g of groups.values()){
   const min=[Infinity,Infinity,Infinity],max=[-Infinity,-Infinity,-Infinity];
   for(let i=0;i<g.pos.length;i+=3)for(let j=0;j<3;j++){min[j]=Math.min(min[j],g.pos[i+j]);max[j]=Math.max(max[j],g.pos[i+j])}
   const prim={attributes:{
     POSITION:acc(f32(g.pos),g.pos.length/3,"VEC3",5126,34962,{min,max}),
     NORMAL:acc(f32(g.norm),g.norm.length/3,"VEC3",5126,34962),
     TEXCOORD_0:acc(f32(g.uv),g.uv.length/2,"VEC2",5126,34962)},
     indices:acc(u16(g.idx),g.idx.length,"SCALAR",5123,34963),material:index[g.mat],mode:4};
   (byNode[g.node]||(byNode[g.node]=[])).push(prim);tris+=g.idx.length/3;
 }
 for(const [node,primitives] of Object.entries(byNode)){let m=meshes.length;meshes.push({name:node,primitives});nodes.push({name:node,mesh:m,extras:{host_relative:true,authoring_only:true}})}
 const materials=names.map(n=>({name:n,alphaMode:mats[n].alpha, doubleSided:mats[n].alpha==="BLEND",
   pbrMetallicRoughness:{baseColorFactor:mats[n].color,metallicFactor:mats[n].metal||0,roughnessFactor:mats[n].rough}}));
 const json={asset:{version:"2.0",generator:"Original OASIS context wear geometry v1"},
   scene:0,scenes:[{nodes:nodes.map((_,i)=>i)}],nodes,meshes,materials,accessors:access,bufferViews:views,buffers:[{byteLength:size}],
   extras:{units:"meters",up:"Y",requires_node_selection:true,requires_host_attachment:true,source:"OASIS contextual wear"}};
 const js=pad(enc(JSON.stringify(json)),32),binary=pad(cat(buf));
 const glb=cat([w32(0x46546c67),w32(2),w32(12+8+js.length+8+binary.length),
 w32(js.length),w32(0x4e4f534a),js,w32(binary.length),w32(0x004e4942),binary]);
 return {glb,nodes:nodes.map(n=>n.name),triangles:tris,materials:materials.length};
}
import {mkdirSync,writeFileSync} from "node:fs";
import {dirname} from "node:path";
const path=process.argv[2]||"assets/packs/oasis_dressing_v1/geometry/wear_examples01.glb";
mkdirSync(dirname(path),{recursive:true});
const a=buildWear();writeFileSync(path,a.glb);
console.log(JSON.stringify({path,bytes:a.glb.length,nodes:a.nodes,triangles:a.triangles}));
