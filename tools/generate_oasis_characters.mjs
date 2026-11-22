
/* Original OASIS actors, GLB 2.0 skin + animation authoring source.
 * node tools/generate_oasis_characters.mjs [output_directory]
 * Y up, meters, face +Z, all locomotion clips in place.
 */
function u8(s){const q=unescape(encodeURIComponent(s)),a=new Uint8Array(q.length);for(let i=0;i<q.length;i++)a[i]=q.charCodeAt(i);return a}
function join(a){let n=a.reduce((s,x)=>s+x.length,0),o=new Uint8Array(n),p=0;for(const x of a){o.set(x,p);p+=x.length}return o}
function w32(n){let b=new Uint8Array(4);new DataView(b.buffer).setUint32(0,n>>>0,true);return b}
function f32(a){let b=new Uint8Array(a.length*4),d=new DataView(b.buffer);for(let i=0;i<a.length;i++)d.setFloat32(i*4,a[i],true);return b}
function i16(a){let b=new Uint8Array(a.length*2),d=new DataView(b.buffer);for(let i=0;i<a.length;i++)d.setUint16(i*2,a[i],true);return b}
function i32(a){let b=new Uint8Array(a.length*4),d=new DataView(b.buffer);for(let i=0;i<a.length;i++)d.setUint32(i*4,a[i],true);return b}
function padded(a,fill=0){return a.length%4?join([a,new Uint8Array(Array(4-a.length%4).fill(fill))]):a}
function b64(a){const c="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",o=[];for(let i=0;i<a.length;i+=3){let n=(a[i]<<16)|((a[i+1]||0)<<8)|(a[i+2]||0);o.push(c[n>>>18],c[(n>>>12)&63],i+1<a.length?c[(n>>>6)&63]:"=",i+2<a.length?c[n&63]:"=")}return o.join("")}
function J(name,parent,p){return {name,parent,p}}
function rig(which){
 const human=which==="human";
 const skeleton=human?[
 J("Hips",-1,[0,.94,0]),J("Spine",0,[0,.2,0]),J("Chest",1,[0,.25,0]),J("Neck",2,[0,.22,0]),J("Head",3,[0,.105,0]),
 J("LShoulder",2,[-.19,.15,0]),J("LArm",5,[-.07,-.035,0]),J("LForearm",6,[-.03,-.28,0]),J("LHand",7,[-.015,-.235,.005]),
 J("RShoulder",2,[.19,.15,0]),J("RArm",9,[.07,-.035,0]),J("RForearm",10,[.03,-.28,0]),J("RHand",11,[.015,-.235,.005]),
 J("LThigh",0,[-.115,-.045,0]),J("LShin",13,[0,-.43,.01]),J("LFoot",14,[0,-.385,.03]),
 J("RThigh",0,[.115,-.045,0]),J("RShin",16,[0,-.43,.01]),J("RFoot",17,[0,-.385,.03])
 ]:[
 J("Hips",-1,[0,.39,-.18]),J("Spine",0,[0,.015,.25]),J("Chest",1,[0,.015,.24]),J("Neck",2,[0,.02,.11]),J("Head",3,[0,.06,.16]),
 J("FLUpper",2,[-.17,-.07,.025]),J("FLLower",5,[0,-.13,.035]),J("FLPaw",6,[0,-.11,.045]),
 J("FRUpper",2,[.17,-.07,.025]),J("FRLower",8,[0,-.13,.035]),J("FRPaw",9,[0,-.11,.045]),
 J("RLUpper",0,[-.17,-.08,-.06]),J("RLLower",11,[0,-.14,.02]),J("RLPaw",12,[0,-.12,.055]),
 J("RRUpper",0,[.17,-.08,-.06]),J("RRLower",14,[0,-.14,.02]),J("RRPaw",15,[0,-.12,.055]),
 J("TailA",0,[0,.045,-.20]),J("TailB",17,[0,.045,-.17]),J("TailC",18,[0,.055,-.17]),J("TailD",19,[0,.045,-.16]),J("TailTip",20,[0,.01,-.15])
 ];
 const world=[],idx=Object.fromEntries(skeleton.map((v,i)=>[v.name,i]));
 for(const j of skeleton){const p=j.parent<0?[0,0,0]:world[j.parent];world.push(j.p.map((v,k)=>v+p[k]))}
 const W=n=>world[idx[n]],O=(n,d)=>W(n).map((v,i)=>v+d[i]);
 const colors=human?{
 cloth:[.18,.21,.23,1],trousers:[.12,.13,.15,1],shirt:[.55,.55,.52,1],skin:[.54,.36,.26,1],hair:[.13,.105,.09,1],shoes:[.075,.08,.09,1],detail:[.40,.45,.48,1],eyes:[.045,.05,.06,1]
 }:{
 fur:[.34,.35,.35,1],light:[.54,.52,.49,1],mask:[.095,.105,.11,1],ring:[.18,.19,.20,1],nose:[.07,.07,.075,1],eyes:[.035,.04,.04,1]
 };
 const groups={};
 function part(bone,mat,verts,tri){
   const g=groups[mat]||(groups[mat]={p:[],n:[],t:[],j:[],w:[],f:[]}),base=g.p.length/3;
   for(const v of verts){g.p.push(...v.p);g.n.push(...v.n);g.t.push(...v.uv);g.j.push(idx[bone],0,0,0);g.w.push(1,0,0,0)}
   for(const a of tri)g.f.push(...a.map(q=>q+base));
 }
 function ellipsoid(bone,mat,center,r,steps=18,stacks=12){
   const verts=[],tri=[];
   for(let j=0;j<=stacks;j++)for(let i=0;i<=steps;i++){
     const a=j*Math.PI/stacks,b=i*2*Math.PI/steps,x=Math.sin(a)*Math.cos(b),y=Math.cos(a),z=Math.sin(a)*Math.sin(b),
     n=[x/r[0],y/r[1],z/r[2]],len=Math.hypot(...n)||1;
     verts.push({p:[center[0]+r[0]*x,center[1]+r[1]*y,center[2]+r[2]*z],n:n.map(q=>q/len),uv:[i/steps,j/stacks]});
   }
   for(let j=0;j<stacks;j++)for(let i=0;i<steps;i++){let a=j*(steps+1)+i,b=a+steps+1;tri.push([a,a+1,b],[a+1,b+1,b])}
   part(bone,mat,verts,tri);
 }
 function tube(bone,mat,a,b,r1,r2,n=14){
   const axis=b.map((v,k)=>v-a[k]),len=Math.hypot(...axis)||1,u=axis.map(v=>v/len),
   arbitrary=Math.abs(u[1])<.9?[0,1,0]:[1,0,0],
   s=[u[1]*arbitrary[2]-u[2]*arbitrary[1],u[2]*arbitrary[0]-u[0]*arbitrary[2],u[0]*arbitrary[1]-u[1]*arbitrary[0]],
   sl=Math.hypot(...s)||1,v=s.map(q=>q/sl),w=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2]];
   const verts=[],faces=[];
   for(let j=0;j<=4;j++){let t=j/4,r=r1+(r2-r1)*t;
     for(let i=0;i<=n;i++){
       let theta=i*2*Math.PI/n,normal=v.map((q,k)=>q*Math.cos(theta)+w[k]*Math.sin(theta));
       verts.push({p:a.map((q,k)=>q+axis[k]*t+r*normal[k]),n:normal,uv:[i/n,t]});
     }
   }
   for(let j=0;j<4;j++)for(let i=0;i<n;i++){let a=j*(n+1)+i,b=a+n+1;faces.push([a,b,a+1],[a+1,b,b+1])}
   part(bone,mat,verts,faces);
 }
 if(human){
   ellipsoid("Hips","trousers",O("Hips",[0,-.024,0]),[.20,.15,.117]);
   ellipsoid("Spine","cloth",O("Spine",[0,.10,0]),[.21,.24,.117],20,14);
   ellipsoid("Chest","cloth",O("Chest",[0,.030,0]),[.235,.16,.123]);
   ellipsoid("Chest","shirt",O("Chest",[0,.11,.116]),[.074,.06,.012],10,8);
   tube("Neck","skin",W("Neck"),W("Head"),.054,.054);
   ellipsoid("Head","skin",O("Head",[0,.05,0]),[.105,.135,.101],20,16);
   ellipsoid("Head","hair",O("Head",[0,.16,-.01]),[.107,.053,.106]);
   for(const x of [-1,1]){
     ellipsoid("Head","skin",O("Head",[x*.108,.03,0]),[.025,.043,.021],10,8);
     ellipsoid("Head","eyes",O("Head",[x*.042,.069,.100]),[.009,.010,.009],10,7);
   }
   ellipsoid("Head","skin",O("Head",[0,.02,.108]),[.021,.027,.027],10,7);
   for(const [a,f,h,t,s,foot] of [["LArm","LForearm","LHand","LThigh","LShin","LFoot"],["RArm","RForearm","RHand","RThigh","RShin","RFoot"]]){
     tube(a,"cloth",W(a),W(f),.078,.064);ellipsoid(a,"cloth",W(a),[.085,.082,.085]);
     tube(f,"cloth",W(f),W(h),.065,.046);ellipsoid(h,"skin",O(h,[0,-.055,.01]),[.05,.078,.034],12,9);
     tube(t,"trousers",W(t),W(s),.111,.075,18);ellipsoid(t,"trousers",W(t),[.111,.12,.108]);
     tube(s,"trousers",W(s),W(foot),.079,.053);ellipsoid(s,"trousers",W(s),[.076,.075,.076]);
     ellipsoid(foot,"shoes",O(foot,[0,-.030,.085]),[.081,.059,.148],20,11);
   }
 }else{
   ellipsoid("Spine","fur",O("Spine",[0,0,0]),[.208,.17,.38],22,14);
   ellipsoid("Hips","fur",O("Hips",[0,0,-.05]),[.212,.18,.24],22,14);
   ellipsoid("Chest","light",O("Chest",[0,-.068,.06]),[.17,.10,.23],18,12);
   ellipsoid("Neck","light",O("Neck",[0,.015,.02]),[.12,.12,.18]);
   ellipsoid("Head","fur",O("Head",[0,.022,.03]),[.144,.137,.162],22,16);
   ellipsoid("Head","light",O("Head",[0,-.042,.15]),[.115,.071,.12],18,12);
   ellipsoid("Head","light",O("Head",[0,-.017,.22]),[.068,.05,.085]);
   ellipsoid("Head","nose",O("Head",[0,-.018,.28]),[.031,.026,.029],12,9);
   for(const x of [-1,1]){
     ellipsoid("Head","mask",O("Head",[x*.078,.027,.16]),[.056,.046,.030],12,9);
     ellipsoid("Head","eyes",O("Head",[x*.073,.037,.188]),[.020,.022,.011],12,9);
     ellipsoid("Head","fur",O("Head",[x*.112,.134,-.010]),[.052,.065,.037],12,9);
     ellipsoid("Head","light",O("Head",[x*.112,.133,.012]),[.027,.039,.014],11,8);
   }
   for(const n of ["FL","FR","RL","RR"]){
     tube(n+"Upper","fur",W(n+"Upper"),W(n+"Lower"),.077,.06,14);
     ellipsoid(n+"Upper","fur",W(n+"Upper"),[.074,.08,.074],12,8);
     tube(n+"Lower","ring",W(n+"Lower"),W(n+"Paw"),.059,.043,12);
     ellipsoid(n+"Paw","ring",O(n+"Paw",[0,-.012,.053]),[.058,.037,.087],13,9);
   }
   const tail=["TailA","TailB","TailC","TailD","TailTip"];
   for(let i=0;i<tail.length;i++){
     let a=W(tail[i]),b=i===tail.length-1?O(tail[i],[0,.02,-.13]):W(tail[i+1]),r=.10-i*.010;
     tube(tail[i],i%2?"fur":"ring",a,b,r,r*.87,18);
     ellipsoid(tail[i],i%2?"fur":"ring",a,[r,r,r],13,8);
   }
 }
 const bytes=[],views=[],accessors=[];let cursor=0;
 function addBuffer(buf,target){const pad=(4-cursor%4)%4;if(pad){bytes.push(new Uint8Array(pad));cursor+=pad}
  const i=views.length;views.push({buffer:0,byteOffset:cursor,byteLength:buf.length,...(target?{target}:{})});bytes.push(buf);cursor+=buf.length;return i}
 function acc(arr,kind,comp,count,target,range){const out={bufferView:addBuffer(arr,target),componentType:comp,count,type:kind,...(range||{})};accessors.push(out);return accessors.length-1}
 const prim=[],mats=Object.keys(colors);let vertices=0,triangles=0;
 for(const [name,g] of Object.entries(groups)){
   const lo=[Infinity,Infinity,Infinity],hi=[-Infinity,-Infinity,-Infinity];
   for(let i=0;i<g.p.length;i+=3)for(let k=0;k<3;k++){lo[k]=Math.min(lo[k],g.p[i+k]);hi[k]=Math.max(hi[k],g.p[i+k])}
   prim.push({attributes:{
     POSITION:acc(f32(g.p),"VEC3",5126,g.p.length/3,34962,{min:lo,max:hi}),
     NORMAL:acc(f32(g.n),"VEC3",5126,g.n.length/3,34962),
     TEXCOORD_0:acc(f32(g.t),"VEC2",5126,g.t.length/2,34962),
     JOINTS_0:acc(i16(g.j),"VEC4",5123,g.j.length/4,34962),
     WEIGHTS_0:acc(f32(g.w),"VEC4",5126,g.w.length/4,34962)
   },indices:acc(i32(g.f),"SCALAR",5125,g.f.length,34963),material:mats.indexOf(name),mode:4});
   vertices+=g.p.length/3;triangles+=g.f.length/3;
 }
 const nodes=skeleton.map(j=>({name:j.name,translation:j.p}));
 for(let i=0;i<skeleton.length;i++){let parent=skeleton[i].parent;if(parent>=0)(nodes[parent].children||(nodes[parent].children=[])).push(i)}
 const meshNode=nodes.length;nodes.push({name:(human?"Human":"Raccoon")+"_Mesh",mesh:0,skin:0});
 const bind=[];for(const w of world)bind.push(1,0,0,0,0,1,0,0,0,0,1,0,-w[0],-w[1],-w[2],1);
 const skins=[{name:"OASIS_"+which+"_skeleton",skeleton:0,joints:skeleton.map((_,i)=>i),
 inverseBindMatrices:acc(f32(bind),"MAT4",5126,skeleton.length)}];
 const defs=human?[
 ["idle",2,10],["walk_in_place",1.0,12],["stairs_up_in_place",1.2,12],["stairs_down_in_place",1.1,12],
 ["sit_down",1.25,10],["seated_idle",2.0,10],["stand_up",1.25,10]
 ]:[["idle",2,10],["walk_in_place",.90,12],["sniff",2.2,12],["tail_sway",1.6,12]];
 const Q=(axis,ang)=>{let x=Math.sin(ang/2),c=Math.cos(ang/2);return axis==="x"?[x,0,0,c]:axis==="y"?[0,x,0,c]:[0,0,x,c]};
 function pose(name,t){
   let r={},v={},phase=2*Math.PI*t,osc=Math.sin(phase),bob=.5*(1-Math.cos(phase*2));
   const rot=(bone,axis,angle)=>r[bone]=Q(axis,angle),tr=(bone,delta)=>v[bone]=skeleton[idx[bone]].p.map((x,i)=>x+delta[i]);
   if(human){
     if(name.includes("walk")||name.includes("stairs")){
       const up=name.includes("up"),down=name.includes("down"),amp=up?.73:down?.60:.48;
       rot("LThigh","x",-amp*osc);rot("RThigh","x",amp*osc);
       rot("LShin","x",Math.max(0,osc)*(up?1.03:.70));rot("RShin","x",Math.max(0,-osc)*(up?1.03:.70));
       rot("LArm","x",.38*osc);rot("RArm","x",-.38*osc);
       rot("LForearm","x",-.16+.10*osc);rot("RForearm","x",-.16-.10*osc);
       rot("LFoot","x",-.12*osc);rot("RFoot","x",.12*osc);
       rot("Chest","y",.045*osc);tr("Hips",[0,.014+.035*bob+(up?.024:0),0]);
     }else if(name==="sit_down"||name==="seated_idle"||name==="stand_up"){
       const value=name==="seated_idle"?1:name==="sit_down"?t:1-t,blend=value*value*(3-2*value);
       tr("Hips",[0,-.41*blend,-.10*blend]);
       rot("LThigh","x",-1.22*blend);rot("RThigh","x",-1.22*blend);
       rot("LShin","x",1.10*blend);rot("RShin","x",1.10*blend);
       rot("LFoot","x",-.22*blend);rot("RFoot","x",-.22*blend);
       rot("Spine","x",.13*blend);
       rot("LArm","x",-.12*blend);rot("RArm","x",-.12*blend);
       if(name==="seated_idle")rot("Chest","x",.015*osc);
     }else rot("Chest","x",.015*osc);
   }else{
     if(name==="walk_in_place"){
       rot("FLUpper","x",.45*osc);rot("FRUpper","x",-.45*osc);
       rot("RLUpper","x",-.45*osc);rot("RRUpper","x",.45*osc);
       for(const bone of ["FLLower","FRLower","RLLower","RRLower"])rot(bone,"x",.15+.10*Math.cos(phase*2));
       tr("Hips",[0,.017*bob,0]);
     }else if(name==="sniff"){rot("Head","y",.24*osc);rot("Neck","x",.1+.10*Math.sin(phase*.5))}
     else rot("Chest","x",.01*osc);
     for(const [i,bone] of ["TailA","TailB","TailC","TailD","TailTip"].entries())rot(bone,"y",.10*Math.sin(phase-i*.44));
   }
   return {r,v};
 }
 const animations=[];
 for(const [name,duration,frames] of defs){
   const times=Array.from({length:frames},(_,i)=>duration*i/(frames-1));
   const timeId=acc(f32(times),"SCALAR",5126,frames,null,{min:[0],max:[duration]});
   const sequence=Array.from({length:frames},(_,i)=>pose(name,i/(frames-1)));
   const clip={name,samplers:[],channels:[]};
   for(const [kind,path,component] of [["r","rotation",4],["v","translation",3]]){
     const bones=Object.keys(sequence[0][kind]);
     for(const bone of bones){
       let out=[];for(const sample of sequence){if(!sample[kind][bone])throw Error("missing track "+bone);out.push(...sample[kind][bone])}
       const outId=acc(f32(out),component===4?"VEC4":"VEC3",5126,frames);
       const i=clip.samplers.length;clip.samplers.push({input:timeId,output:outId,interpolation:"LINEAR"});
       clip.channels.push({sampler:i,target:{node:idx[bone],path}});
     }
   }
   animations.push(clip);
 }
 const json={asset:{version:"2.0",generator:"OASIS authored animated actor v1"},scene:0,
 scenes:[{nodes:[0,meshNode]}],nodes,meshes:[{name:which+"_skinned_geometry",primitives:prim}],
 materials:mats.map(name=>({name,pbrMetallicRoughness:{baseColorFactor:colors[name],metallicFactor:0,roughnessFactor:name==="eyes"?.18:.88}})),
 skins,animations,accessors,bufferViews:views,buffers:[{byteLength:cursor}],
 extras:{meters:true,up:"Y",forward:"+Z",animation_movement:"in_place",source:"OASIS authored characters"}};
 const js=padded(u8(JSON.stringify(json)),32),binChunk=padded(join(bytes));
 const glb=join([w32(0x46546c67),w32(2),w32(12+8+js.length+8+binChunk.length),w32(js.length),w32(0x4e4f534a),js,w32(binChunk.length),w32(0x004e4942),binChunk]);
 return {glb,vertices,triangles,bones:skeleton.length,clips:defs.map(d=>d[0]),materials:mats.length};
}
import { mkdirSync,writeFileSync } from "node:fs";
import {join as pathJoin} from "node:path";
const out=process.argv[2]||"assets/packs/oasis_characters_v1/geometry";
mkdirSync(out,{recursive:true});
for(const [kind,id] of [["human","human_adult01"],["raccoon","raccoon_gray01"]]){
 const {glb,vertices,triangles,bones,clips}=rig(kind);
 writeFileSync(pathJoin(out,id+".glb"),glb);
 console.log(JSON.stringify({id,bytes:glb.length,vertices,triangles,bones,clips}));
}
