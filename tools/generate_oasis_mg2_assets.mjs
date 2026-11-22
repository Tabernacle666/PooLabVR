function utf8(s){const q=unescape(encodeURIComponent(s)),a=new Uint8Array(q.length);for(let i=0;i<q.length;i++)a[i]=q.charCodeAt(i);return a}

function b64(b){const cs="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",o=[];for(let i=0;i<b.length;i+=3){let n=(b[i]<<16)|((b[i+1]||0)<<8)|(b[i+2]||0);o.push(cs[n>>>18],cs[(n>>>12)&63],i+1<b.length?cs[(n>>>6)&63]:"=",i+2<b.length?cs[n&63]:"=")}return o.join("")}

function cat(arr){let n=arr.reduce((s,a)=>s+a.length,0),r=new Uint8Array(n),p=0;for(const a of arr){r.set(a,p);p+=a.length}return r}

function le32(v){let x=new Uint8Array(4);new DataView(x.buffer).setUint32(0,v>>>0,true);return x}

function be32(v){let x=new Uint8Array(4);new DataView(x.buffer).setUint32(0,v>>>0,false);return x}

function f32(v){let x=new Uint8Array(v.length*4),d=new DataView(x.buffer);v.forEach((n,i)=>d.setFloat32(i*4,n,true));return x}

function u32(v){let x=new Uint8Array(v.length*4),d=new DataView(x.buffer);v.forEach((n,i)=>d.setUint32(i*4,n,true));return x}

function pad4(x,fill=0){return x.length%4?cat([x,new Uint8Array(Array(4-x.length%4).fill(fill))]):x}

function crc32(a){let c=-1;for(const b of a){c^=b;for(let k=0;k<8;k++)c=(c>>>1)^((c&1)?0xedb88320:0)}return (c^(-1))>>>0}

function pngMask(kind){const W=48,H=48,raw=[];for(let y=0;y<H;y++){raw.push(0);for(let x=0;x<W;x++){let u=(x+.5-W/2)/(W/2),v=(y+.5-H/2)/(H/2),a;if(kind==="needle")a=Math.abs(u)<.17*(1-Math.abs(v))+.02&&Math.abs(v)<.94?255:0;else a=(u*u/.58+v*v)<.86 && Math.abs(u)<(.9-.12*Math.abs(v))?255:0;raw.push(250,252,249,a)}}const len=raw.length,z=[120,1],d=new Uint8Array(raw);let pos=0;while(pos<d.length){let chunk=Math.min(65535,d.length-pos);z.push(pos+chunk===d.length?1:0,chunk&255,chunk>>>8,(~chunk)&255,((~chunk)>>>8)&255);for(let i=pos;i<pos+chunk;i++)z.push(d[i]);pos+=chunk}let a=1,b=0;for(const v of d){a=(a+v)%65521;b=(b+a)%65521}z.push(...be32((b<<16)|a));function ck(type,bytes){let t=utf8(type);return cat([be32(bytes.length),t,bytes,be32(crc32(cat([t,bytes])))])}return cat([Uint8Array.from([137,80,78,71,13,10,26,10]),ck("IHDR",cat([be32(W),be32(H),Uint8Array.from([8,6,0,0,0])])),ck("IDAT",Uint8Array.from(z)),ck("IEND",new Uint8Array())])}

function mk(){return {groups:{},sockets:{},materials:{}}}

function mat(g,n,rgba,metal=.0,rough=.7,mode="OPAQUE"){g.materials[n]={name:n,pbrMetallicRoughness:{baseColorFactor:rgba,metallicFactor:metal,roughnessFactor:rough},alphaMode:mode,doubleSided:mode==="MASK"};if(mode==="MASK")g.materials[n].alphaCutoff=.48}

function part(g,n,m){const key=n+"|"+m;return g.groups[key]||(g.groups[key]={node:n,material:m,v:[],idx:[]})}

function quad(g,n,m,a,b,c,d){const p=part(g,n,m),off=p.v.length/8,u=[b[0]-a[0],b[1]-a[1],b[2]-a[2]],v=[c[0]-a[0],c[1]-a[1],c[2]-a[2]];let nn=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]],ln=Math.hypot(...nn)||1;nn=nn.map(x=>x/ln);for(const [pt,uv] of [[a,[0,0]],[b,[1,0]],[c,[1,1]],[d,[0,1]]])p.v.push(...pt,...nn,...uv);p.idx.push(off,off+1,off+2,off,off+2,off+3)}

function box(g,n,m,x,y,z,w,h,d){let X=x-w/2,R=x+w/2,B=y-h/2,T=y+h/2,F=z+d/2,K=z-d/2;quad(g,n,m,[X,B,F],[R,B,F],[R,T,F],[X,T,F]);quad(g,n,m,[R,B,K],[X,B,K],[X,T,K],[R,T,K]);quad(g,n,m,[X,B,K],[X,B,F],[X,T,F],[X,T,K]);quad(g,n,m,[R,B,F],[R,B,K],[R,T,K],[R,T,F]);quad(g,n,m,[X,T,F],[R,T,F],[R,T,K],[X,T,K]);quad(g,n,m,[X,B,K],[R,B,K],[R,B,F],[X,B,F])}

function tube(g,n,m,a,b,r0,r1,N=12,cap=true){let p=part(g,n,m),off=p.v.length/8,axis=b.map((v,i)=>v-a[i]),al=Math.hypot(...axis)||1,up=axis.map(v=>v/al),arb=Math.abs(up[1])<.92?[0,1,0]:[1,0,0],s=[up[1]*arb[2]-up[2]*arb[1],up[2]*arb[0]-up[0]*arb[2],up[0]*arb[1]-up[1]*arb[0]],sl=Math.hypot(...s)||1;s=s.map(x=>x/sl);const t=[up[1]*s[2]-up[2]*s[1],up[2]*s[0]-up[0]*s[2],up[0]*s[1]-up[1]*s[0]];for(let k=0;k<=N;k++){let ang=2*Math.PI*k/N,rad= s.map((v,i)=>v*Math.cos(ang)+t[i]*Math.sin(ang));for(let j=0;j<2;j++){const cen=j?b:a,r=j?r1:r0,vertex=cen.map((v,i)=>v+r*rad[i]);p.v.push(...vertex,...rad,k/N,j)}}for(let k=0;k<N;k++){let q=off+2*k;p.idx.push(q,q+2,q+1,q+1,q+2,q+3)}if(cap){for(let end=0;end<2;end++){const cen=end?b:a,normal=end?up:up.map(v=>-v),r=end?r1:r0,st=p.v.length/8;p.v.push(...cen,...normal,0.5,0.5);for(let k=0;k<=N;k++){let th=2*Math.PI*k/N,rad=s.map((v,i)=>v*Math.cos(th)+t[i]*Math.sin(th));p.v.push(...cen.map((v,i)=>v+r*rad[i]),...normal,.5+.5*Math.cos(th),.5+.5*Math.sin(th))}for(let k=0;k<N;k++)if(end)p.idx.push(st,st+1+k,st+2+k);else p.idx.push(st,st+2+k,st+1+k)}}}

function orb(g,n,m,c,sz,N=12,M=8){const p=part(g,n,m),off=p.v.length/8;for(let j=0;j<=M;j++){let lat=Math.PI*j/M;for(let k=0;k<=N;k++){let lon=2*Math.PI*k/N,sx=Math.sin(lat)*Math.cos(lon),sy=Math.cos(lat),sz0=Math.sin(lat)*Math.sin(lon),normal=[sx/sz[0],sy/sz[1],sz0/sz[2]],ln=Math.hypot(...normal)||1;p.v.push(c[0]+sx*sz[0],c[1]+sy*sz[1],c[2]+sz0*sz[2],...normal.map(v=>v/ln),k/N,j/M)}}for(let j=0;j<M;j++)for(let k=0;k<N;k++){const a=off+j*(N+1)+k,b=a+N+1;p.idx.push(a,a+1,b,a+1,b+1,b)}}

function torus(g,n,m,c,rad,thick,N=24,M=8){const p=part(g,n,m),off=p.v.length/8;for(let i=0;i<=N;i++){let a=2*Math.PI*i/N;for(let j=0;j<=M;j++){let b=2*Math.PI*j/M,v=Math.cos(b),w=Math.sin(b),u=Math.cos(a),t=Math.sin(a);p.v.push(c[0]+(rad+thick*v)*u,c[1]+thick*w,c[2]+(rad+thick*v)*t,u*v,w,t*v,i/N,j/M)}}for(let i=0;i<N;i++)for(let j=0;j<M;j++){let a=off+i*(M+1)+j,b=a+M+1;p.idx.push(a,a+1,b,a+1,b+1,b)}}

function leaf(g,node,ma,c,s,rot=0){let [x,y,z]=c,w=s[0]/2,h=s[1]/2,ca=Math.cos(rot),sa=Math.sin(rot),axis=[ca,0,sa],d=[-sa,0,ca];let a=[x-w*axis[0],y-h,z-w*axis[2]],b=[x+w*axis[0],y-h,z+w*axis[2]],cc=[x+w*axis[0]+.03*d[0],y+h,z+w*axis[2]+.03*d[2]],dd=[x-w*axis[0]+.03*d[0],y+h,z-w*axis[2]+.03*d[2]];quad(g,node,ma,a,b,cc,dd)}

function socket(g,n,p,role){g.sockets[n]={node:n,translation:p,role}}

function makeAsset(id){let g=mk();const palette=[
["plastic_dark",[.045,.052,.065,1],.02,.65],["plastic_mid",[.13,.16,.19,1],.0,.58],["steel_painted",[.20,.23,.26,1],.65,.4],["steel_brushed",[.52,.55,.59,1],.82,.28],["wood_oiled",[.28,.17,.095,1],0,.56],["wood_light",[.56,.39,.23,1],0,.58],["screen_black",[.012,.017,.025,1],.07,.19],["screen_neutral",[.08,.16,.19,1],0,.35],["fabric_dark",[.17,.20,.21,1],0,.95],["fabric_blue",[.085,.16,.20,1],0,.89],["ceramic",[.86,.89,.84,1],0,.17],["chrome",[.7,.72,.73,1],.96,.18],["soil",[.23,.17,.11,1],0,.96],["bark",[.18,.115,.083,1],0,.94],["bark_birch",[.63,.60,.54,1],0,.9],["needle_green",[.095,.23,.16,1],0,.85],["leaf_green",[.18,.32,.16,1],0,.84],["grass",[.2,.30,.16,1],0,.98],["stone",[.33,.32,.29,1],0,.93],["glass",[.55,.69,.78,.26],0,.10],["light_emissive",[1,.9,.72,1],0,.38]];
for(const [n,c,m,r] of palette)mat(g,n,c,m,r,n==="glass"?"BLEND":"OPAQUE");mat(g,"foliage_mask",[.44,.75,.36,1],0,.87,"MASK");mat(g,"needle_mask",[.22,.48,.28,1],0,.9,"MASK");
const bx=(n,m,...a)=>box(g,n,m,...a),tb=(n,m,...a)=>tube(g,n,m,...a),ob=(n,m,...a)=>orb(g,n,m,...a),rg=(n,m,...a)=>torus(g,n,m,...a);
if(id==="desk_office01"){
bx("top","wood_light",0,.735,0,1.48,.045,.74);bx("top","wood_oiled",0,.704,0,1.45,.015,.72);
for(const x of [-.66,.66])for(const z of [-.29,.29])tb("frame","steel_painted",[x,.045,z],[x,.70,z],.026,.026,12);
bx("frame","steel_painted",0,.49,-.27,1.32,.042,.035);bx("frame","steel_painted",0,.19,-.29,1.32,.035,.035);
bx("cable_tray","plastic_dark",0,.625,-.275,.75,.08,.14);bx("grommet","plastic_dark",.43,.76,-.24,.075,.007,.075);
socket(g,"socket_desktop",[0,.762,0],"surface");socket(g,"socket_power",[.40,.64,-.31],"electric");socket(g,"socket_data",[.33,.64,-.31],"data");
}else if(id==="chair_ergonomic01"){
for(let i=0;i<5;i++){let a=i*2*Math.PI/5;tb("base","plastic_dark",[0,.11,0],[Math.sin(a)*.32,.095,Math.cos(a)*.32],.039,.036,10);ob("caster","plastic_dark",[Math.sin(a)*.35,.055,Math.cos(a)*.35],[.045,.05,.045],10,8)}
tb("lift","chrome",[0,.10,0],[0,.46,0],.05,.042,16);ob("seat","fabric_dark",[0,.49,.04],[.29,.075,.265],20,10);
bx("seat_shell","plastic_dark",0,.435,.03,.56,.035,.48);
for(let j=0;j<4;j++){let y=.65+j*.14,w=.51-.025*j;bx("back_shell","plastic_dark",0,y,-.24,w,.14,.050);bx("back_cushion","fabric_blue",0,y,-.206,w-.035,.12,.034)}
for(let x of [-.27,.27]){tb("arm","plastic_dark",[x,.46,-.04],[x,.71,-.04],.022,.022,10);bx("arm","plastic_dark",x,.715,.01,.065,.035,.29)}
socket(g,"socket_seat",[0,.54,.02],"seat");
}else if(id==="monitor_24in01"){
bx("shell","plastic_dark",0,.392,0,.572,.348,.044);bx("bezel","plastic_mid",0,.394,.026,.548,.324,.006);bx("panel","screen_black",0,.4,.030,.52,.293,.004);
bx("status_led","light_emissive",.243,.241,.029,.009,.005,.003);tb("stand","steel_painted",[0,.217,-.03],[0,.075,-.03],.018,.028,12);bx("base","steel_painted",0,.023,-.035,.25,.025,.19);bx("ports","plastic_mid",0,.265,-.027,.17,.036,.016);
socket(g,"socket_power",[.08,.27,-.035],"electric");socket(g,"socket_display",[-.09,.27,-.035],"data");socket(g,"socket_vesa",[0,.39,-.026],"mount");
}else if(id==="keyboard_full01"){
bx("chassis","plastic_dark",0,.022,0,.445,.035,.155);
const rows=[13,13,12,12,12,17];for(let r=0;r<5;r++)for(let k=0;k<rows[r];k++){let x=-.205+k*.0305, z=-.058+r*.029;bx("keycaps","plastic_mid",x,.044,z,.026,.014,.024);bx("keytops","plastic_dark",x,.052,z,.022,.002,.020)}
bx("spacebar","plastic_mid",0,.044,.082,.18,.014,.024);bx("key_legends","steel_brushed",-.18,.054,-.06,.007,.001,.005);
socket(g,"socket_usb",[.18,.025,-.08],"data");
}else if(id==="mouse_office01"){
ob("body","plastic_dark",[0,.027,0],[.038,.025,.062],20,12);
box(g,"top_left","plastic_mid",-.018,.049,.027,.032,.004,.051);
box(g,"top_right","plastic_mid",.018,.049,.027,.032,.004,.051);
tb("wheel","steel_brushed",[0,.052,.027],[0,.059,.027],.009,.009,10);socket(g,"socket_usb",[0,.02,-.063],"data");
}else if(id==="door_office_solid01"){
bx("frame","steel_painted",.495,2.095,0,1.05,.068,.15);
bx("frame","steel_painted",-.035,1.075,0,.070,2.11,.15);bx("frame","steel_painted",1.025,1.075,0,.070,2.11,.15);
bx("leaf","wood_oiled",.496,1.075,0,.98,2.05,.053);bx("leaf_trim","wood_light",.496,1.075,.028,.86,1.91,.008);
for(let y of [.18,1.96])for(let x of [.08,.91])bx("leaf_panels","wood_oiled",x,y,.036,.04,.045,.01);
for(let y of [.29,1.82])bx("hinge","steel_brushed",.006,y,-.002,.035,.11,.065);
for(let z of [-.048,.048]){tb("handle","chrome",[.84,1.02,z],[.92,1.02,z],.014,.012,12);bx("handle","chrome",.84,1.02,z*.7,.05,.038,.026)}
bx("threshold","steel_brushed",.5,.035,0,1.01,.022,.18);
socket(g,"socket_hinge",[0,0,0],"hinge");socket(g,"socket_threshold",[.5,.02,0],"portal");socket(g,"socket_handle",[.87,1.02,.047],"grip");socket(g,"col_leaf",[.495,1.07,0],"collision");
}else if(id==="toilet_floor01"){
ob("bowl","ceramic",[0,.34,.09],[.30,.30,.38],24,16);ob("bowl_interior","plastic_mid",[0,.575,.115],[.20,.04,.29],20,8);
rg("seat","ceramic",[0,.61,.11],.25,.052,28,10);rg("cover","ceramic",[0,.666,-.22],.19,.037,28,8);
bx("tank","ceramic",0,.74,-.33,.48,.48,.22);bx("tank_lid","ceramic",0,.992,-.33,.50,.045,.25);
tb("flush_button","chrome",[.12,1.012,-.33],[.12,1.028,-.33],.024,.024,12);
socket(g,"socket_supply",[-.17,.22,-.38],"water_in");socket(g,"socket_waste",[0,.06,.05],"drain");
}else if(id==="sink_wall01"){
ob("basin","ceramic",[0,.84,.1],[.36,.105,.245],24,12);ob("basin_inside","plastic_mid",[0,.929,.12],[.24,.009,.15],20,8);
tb("tap","chrome",[0,.945,-.035],[0,1.13,-.035],.021,.020,12);tb("spout","chrome",[0,1.13,-.035],[0,1.13,.11],.019,.017,12);
for(let x of [-.13,.13])tb("handles","chrome",[x,.94,-.03],[x,.995,-.03],.028,.018,12);
tb("drain","chrome",[0,.73,.1],[0,.44,.1],.036,.034,12);
socket(g,"socket_water_hot",[.12,.56,-.13],"water_hot");socket(g,"socket_water_cold",[-.12,.56,-.13],"water_cold");socket(g,"socket_drain",[0,.44,.1],"drain");
}else if(id==="plant_potted01"){
tb("pot","ceramic",[0,.02,0],[0,.31,0],.115,.16,20);tb("soil","soil",[0,.306,0],[0,.312,0],.132,.132,18);
for(let i=0;i<14;i++){let a=i*2.39996,y=.35+(i%6)*.085,r=.08+(i%4)*.03,x=Math.sin(a)*r,z=Math.cos(a)*r;tb("stems","needle_green",[0,.26,0],[x,y,z],.009,.005,6);leaf(g,"foliage","foliage_mask",[x,y,z],[.145,.22],a)}
socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="tree_spruce01"||id==="tree_birch01"){
let spruce=id==="tree_spruce01",height=spruce?8.4:9.4;tb("trunk",spruce?"bark":"bark_birch",[0,0,0],[0,height*.86,0],spruce?.20:.24,spruce?.06:.07,18);
for(let i=0;i<(spruce?18:14);i++){let y=spruce?.85+i*.37:2.25+i*.37,spread=spruce?(2.3*(1-y/height)+.15):(1.5+.4*Math.sin(i));for(let j=0;j<(spruce?8:6);j++){let a=j*2*Math.PI/(spruce?8:6)+i*1.1,dx=Math.cos(a),dz=Math.sin(a),bend=spruce?-.22:.38,end=[dx*spread,y+bend,dz*spread];tb("branches",spruce?"bark":"bark_birch",[0,y,0],end,.045,.012,7);for(let k=1;k<=3;k++){let r=.4+.2*k,c=[dx*spread*r,y+bend*r,dz*spread*r];if(spruce){leaf(g,"needles","needle_mask",c,[.21,.38],a+Math.PI*.2);leaf(g,"needles","needle_mask",[c[0]+.07,c[1]+.07,c[2]],[.23,.32],a-Math.PI*.2)}else{leaf(g,"foliage","foliage_mask",c,[.42,.60],a+Math.PI*.2);leaf(g,"foliage","foliage_mask",[c[0],c[1]+.23,c[2]+.09],[.38,.53],a-Math.PI*.25)}}}}
socket(g,"socket_root",[0,0,0],"terrain");socket(g,"socket_wind",[0,height*.6,0],"wind_pivot");socket(g,"col_trunk",[0,height*.43,0],"collision");
}else if(id==="fern01"){
for(let i=0;i<9;i++){let a=i*2*Math.PI/9;let end=[Math.cos(a)*.57,.32+Math.sin(i*1.3)*.09,Math.sin(a)*.57];tb("fronds","needle_green",[0,.025,0],end,.009,.004,6);for(let k=1;k<=6;k++){let t=k/7,mid=end.map((v,j)=>v*t),p=[mid[0],mid[1]+.09*Math.sin(t*Math.PI),mid[2]];leaf(g,"leaflets","foliage_mask",p,[.13,.20],a+k*.25)}}
socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="cabinet_2drawer01"){
bx("cabinet","steel_painted",0,.32,0,.43,.62,.52);for(let y of [.17,.47]){bx("drawer","plastic_mid",0,y,.266,.40,.275,.018);bx("handle","chrome",0,y,.286,.14,.025,.022)}
bx("feet","plastic_dark",-.165,.018,-.16,.06,.036,.06);bx("feet","plastic_dark",.165,.018,-.16,.06,.036,.06);
socket(g,"socket_top",[0,.64,0],"surface");
}else if(id==="meeting_table01"){
bx("top","wood_oiled",0,.735,0,2.3,.055,1.08);for(let x of [-.89,.89]){bx("frame","steel_painted",x,.37,0,.085,.68,.65);bx("feet","steel_brushed",x,.05,0,.12,.035,.74)}
bx("cable_hatch","plastic_dark",0,.77,0,.34,.008,.13);socket(g,"socket_power",[0,.71,0],"electric");
}else if(id==="light_panel01"){
bx("housing","steel_painted",0,.055,0,1.2,.11,.6);bx("emitter","light_emissive",0,-.003,0,1.12,.01,.52);bx("diffuser","ceramic",0,-.012,0,1.11,.003,.50);
socket(g,"socket_ceiling",[0,.12,0],"ceiling");socket(g,"socket_light",[0,-.04,0],"light_down");
}else if(id==="window_bay01"){
bx("frame","steel_painted",0,1.1,0,1.8,.075,.11);bx("frame","steel_painted",0,.05,0,1.8,.075,.11);for(let x of [-.87,0,.87])bx("frame","steel_painted",x,.575,0,.075,1.13,.11);
for(let x of [-.435,.435])bx("glass","glass",x,.577,0,.79,1.0,.012);
socket(g,"socket_facade",[0,0,0],"host");socket(g,"socket_lintel",[0,1.14,0],"architecture");
}else if(id==="supply_set01"){
for(let i=0;i<7;i++)bx("books","wood_light",-.17+i*.057,.038,0,.05,.076,.19);
for(let i=0;i<4;i++)tb("pens","plastic_mid",[.13+i*.018,.013,.075],[.13+i*.018,.14,.075],.006,.004,8);
tb("cup","ceramic",[.23,.007,-.03],[.23,.105,-.03],.043,.043,14);rg("cup_rim","ceramic",[.23,.107,-.03],.039,.006,16,6);
bx("paper","ceramic",-.08,.006,-.16,.20,.008,.14);
socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="forest_patch01"){
bx("soil","soil",0,-.09,0,8,.18,8);bx("path","stone",0,.006,0,1.45,.018,8);
for(let i=0;i<65;i++){let a=i*2.39996,r=2.9*Math.sqrt((i+.5)/65),x=Math.cos(a)*r,z=Math.sin(a)*r;if(Math.abs(x)<.85)continue;let h=.15+.10*(i%4)/4;tb("grass","grass",[x,.01,z],[x+.04,.01+h,z-.03],.018,.002,4);if(i%8===0)ob("stone","stone",[x,.025,z],[.12,.06,.15],8,5)}
socket(g,"socket_neighbor_n",[0,0,-4],"tile_edge");socket(g,"socket_neighbor_s",[0,0,4],"tile_edge");socket(g,"socket_neighbor_e",[4,0,0],"tile_edge");socket(g,"socket_neighbor_w",[-4,0,0],"tile_edge");
}else throw Error("asset "+id);return g}

function buildGLB(g){const mats=Object.values(g.materials),matIndex=Object.fromEntries(mats.map((m,i)=>[m.name,i]));let bytes=[],views=[],acc=[],meshes=[],nodes=[],imgs=[],tx=[],samplers=[],texturesUsed={};let byteLen=0;function bufferAdd(raw,target){let padding=(4-byteLen%4)%4;if(padding){bytes.push(new Uint8Array(padding));byteLen+=padding}let index=views.length;views.push({buffer:0,byteOffset:byteLen,byteLength:raw.length,...(target?{target}:{})});bytes.push(raw);byteLen+=raw.length;return index}function accessor(raw,count,type,component,min,max,target){let idx=acc.length;acc.push({bufferView:bufferAdd(raw,target),componentType:component,count,type,...(min?{min,max}:{})});return idx}
for(const kind of ["foliage_mask","needle_mask"].filter(n=>Object.values(g.groups).some(p=>p.material===n))){let p=pngMask(kind==="needle_mask"?"needle":"leaf"),iv=bufferAdd(p),i=imgs.length;imgs.push({mimeType:"image/png",bufferView:iv,name:kind+"_alpha"});if(!samplers.length)samplers.push({magFilter:9729,minFilter:9987,wrapS:10497,wrapT:10497});texturesUsed[kind]=tx.length;tx.push({source:i,sampler:0})}
for(const m of mats)if(texturesUsed[m.name]!==undefined)m.pbrMetallicRoughness.baseColorTexture={index:texturesUsed[m.name],texCoord:0};
let byNode={};for(const p of Object.values(g.groups)){if(!p.idx.length)continue;let attrs={POSITION:[],NORMAL:[],TEXCOORD_0:[]},low=[Infinity,Infinity,Infinity],high=[-Infinity,-Infinity,-Infinity];for(let i=0;i<p.v.length;i+=8){for(let k=0;k<3;k++){attrs.POSITION.push(p.v[i+k]);low[k]=Math.min(low[k],p.v[i+k]);high[k]=Math.max(high[k],p.v[i+k]);attrs.NORMAL.push(p.v[i+k+3])}attrs.TEXCOORD_0.push(p.v[i+6],p.v[i+7])}const prim={attributes:{POSITION:accessor(f32(attrs.POSITION),attrs.POSITION.length/3,"VEC3",5126,low,high,34962),NORMAL:accessor(f32(attrs.NORMAL),attrs.NORMAL.length/3,"VEC3",5126,null,null,34962),TEXCOORD_0:accessor(f32(attrs.TEXCOORD_0),attrs.TEXCOORD_0.length/2,"VEC2",5126,null,null,34962)},indices:accessor(u32(p.idx),p.idx.length,"SCALAR",5125,null,null,34963),material:matIndex[p.material],mode:4};(byNode[p.node]||(byNode[p.node]=[])).push(prim)}
for(const [name,prims] of Object.entries(byNode)){const mi=meshes.length;meshes.push({name,primitives:prims});nodes.push({name,mesh:mi})}
for(const s of Object.values(g.sockets))nodes.push({name:s.node,translation:s.translation,extras:{oasis_role:s.role}});
let json={asset:{version:"2.0",generator:"OASIS authored parametric geometry v1"},scene:0,scenes:[{nodes:nodes.map((_,i)=>i)}],nodes,meshes,materials:mats,accessors:acc,bufferViews:views,buffers:[{byteLength:byteLen}],images:imgs,textures:tx,samplers,extras:{units:"meters",up:"Y",pack:"oasis_mg2_v1"}},js=pad4(utf8(JSON.stringify(json)),32),bin=pad4(cat(bytes));let glb=cat([le32(0x46546c67),le32(2),le32(12+8+js.length+8+bin.length),le32(js.length),le32(0x4e4f534a),js,le32(bin.length),le32(0x004e4942),bin]);let lo=[Infinity,Infinity,Infinity],hi=[-Infinity,-Infinity,-Infinity],tris=0;for(const p of Object.values(g.groups)){tris+=p.idx.length/3;for(let i=0;i<p.v.length;i+=8)for(let k=0;k<3;k++){lo[k]=Math.min(lo[k],p.v[i+k]);hi[k]=Math.max(hi[k],p.v[i+k])}}return {glb,triangles:tris,nodes:nodes.map(n=>n.name),bounds:{min:lo,max:hi},materials:Object.keys(g.materials).filter(n=>Object.values(g.groups).some(p=>p.material===n)),images:imgs.length}}

import {mkdirSync,writeFileSync} from "node:fs";
import {dirname,join} from "node:path";
const ROOT=process.argv[2]||"assets/packs/oasis_mg2_v1";
const ids=["desk_office01","chair_ergonomic01","monitor_24in01","keyboard_full01","mouse_office01","door_office_solid01","toilet_floor01","sink_wall01","plant_potted01","tree_spruce01","tree_birch01","fern01","cabinet_2drawer01","meeting_table01","light_panel01","window_bay01","supply_set01","forest_patch01"];
for(const id of ids){const g=makeAsset(id),{glb,triangles,bounds}=buildGLB(g),file=join(ROOT,"geometry",id+".glb");mkdirSync(dirname(file),{recursive:true});writeFileSync(file,glb);console.log(id,triangles,glb.length,bounds)}
