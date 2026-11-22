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


function hashTile(x,y,seed=1){
  let z=(Math.imul(x|0,374761393)+Math.imul(y|0,668265263)+Math.imul(seed|0,1442695041))|0;
  z=Math.imul(z^(z>>>13),1274126177);return ((z^(z>>>16))>>>0)/4294967295;
}
function tileNoise(u,v,period,seed){
  const x=u*period,y=v*period,ix=Math.floor(x),iy=Math.floor(y),fx=x-ix,fy=y-iy;
  const wrap=n=>(n%period+period)%period, smooth=a=>a*a*(3-2*a);
  const a=hashTile(wrap(ix),wrap(iy),seed),b=hashTile(wrap(ix+1),wrap(iy),seed),
        c=hashTile(wrap(ix),wrap(iy+1),seed),d=hashTile(wrap(ix+1),wrap(iy+1),seed);
  const p=a+(b-a)*smooth(fx),q=c+(d-c)*smooth(fx);return p+(q-p)*smooth(fy);
}
function surfaceHeight(u,v,kind){
  const coarse=tileNoise(u,v,4,21)*.52+tileNoise(u,v,13,39)*.33+tileNoise(u,v,41,57)*.15;
  if(kind==="wood_light"||kind==="wood_oiled"){
    const phase=u*31+1.2*Math.sin(v*6.2831853)*.35+(coarse-.5)*3.5;
    return .43+.3*Math.sin(phase*6.2831853)+.17*(coarse-.5);
  }
  if(kind==="bark"||kind==="bark_birch"){
    if(kind==="bark_birch")return .57+.2*Math.sin(v*18.84955+coarse*2.2)+.3*(coarse-.5);
    return .50+.23*Math.sin((u*17+coarse*3)*6.2831853)+.36*(coarse-.5);
  }
  if(kind==="fabric_dark"||kind==="fabric_blue"){
    return .52+.18*Math.cos(u*6.2831853*55)*Math.cos(v*6.2831853*55)+.22*(coarse-.5);
  }
  if(kind==="steel_brushed")return .50+.19*Math.sin(u*6.2831853*58+.3*Math.sin(v*2))+ .15*(coarse-.5);
  if(kind==="stone"||kind==="soil"){
    return .47+.45*(coarse-.5)+.11*(hashTile(Math.floor(u*128),Math.floor(v*128),17)-.5);
  }
  if(kind==="grass"||kind==="needle_green"||kind==="leaf_green")return .50+.29*(coarse-.5);
  return .53+.12*(coarse-.5);
}
function pngPixels(width,height,pixel){
  const raw=new Uint8Array(height*(width*4+1));let offset=0;
  for(let y=0;y<height;y++){raw[offset++]=0;for(let x=0;x<width;x++){
    const v=pixel((x+.5)/width,(y+.5)/height,x,y);
    for(let i=0;i<4;i++)raw[offset++]=Math.round(Math.max(0,Math.min(255,v[i])));
  }}
  const z=[0x78,0x01];let pos=0;
  while(pos<raw.length){
    const len=Math.min(65535,raw.length-pos),last=pos+len===raw.length;
    z.push(last?1:0,len&255,len>>>8,(~len)&255,((~len)>>>8)&255);
    for(let i=pos;i<pos+len;i++)z.push(raw[i]);pos+=len;
  }
  let a=1,b=0;for(const v of raw){a=(a+v)%65521;b=(b+a)%65521}
  z.push(...be32((b<<16)|a));
  function chunk(name,bytes){
    const t=utf8(name);return cat([be32(bytes.length),t,bytes,be32(crc32(cat([t,bytes])))]);
  }
  return cat([Uint8Array.from([137,80,78,71,13,10,26,10]),
    chunk("IHDR",cat([be32(width),be32(height),Uint8Array.from([8,6,0,0,0])])),
    chunk("IDAT",Uint8Array.from(z)),chunk("IEND",new Uint8Array())]);
}
function pbrTexture(kind,channel,dim){
  const eps=1/dim,clamp=x=>Math.max(0,Math.min(255,Math.round(x)));
  return pngPixels(dim,dim,(u,v,x,y)=>{
    const h=surfaceHeight(u,v,kind),n=tileNoise(u,v,29,81),fine=hashTile(x,y,92);
    if(channel==="color"){
      const w=.73+.42*h+.06*(fine-.5);
      let rgb=[w*255,w*255,w*255];
      if(kind==="bark_birch"){const scar=Math.abs(Math.sin(v*36+u*2))<.14&&n>.44;rgb=scar?[82,78,73]:[clamp(215*w),clamp(212*w),clamp(209*w)];}
      else if(kind==="wood_light"||kind==="wood_oiled")rgb=[clamp(245*w),clamp(236*w),clamp(222*w)];
      else if(kind==="fabric_blue"||kind==="fabric_dark")rgb=[clamp(233*w),clamp(245*w),clamp(251*w)];
      else if(kind==="grass"||kind==="needle_green"||kind==="leaf_green")rgb=[clamp(230*w),clamp(244*w),clamp(225*w)];
      return [...rgb,255];
    }
    if(channel==="orm"){
      const ao=240-35*Math.max(0,.5-h),rough=clamp(192+68*(.5-h)+30*(n-.5));
      const metal=kind.startsWith("steel")?255:0;
      return [clamp(ao),rough,metal,255];
    }
    const dx=surfaceHeight((u+eps)%1,v,kind)-surfaceHeight((u-eps+1)%1,v,kind);
    const dy=surfaceHeight(u,(v+eps)%1,kind)-surfaceHeight(u,(v-eps+1)%1,kind);
    const strength=kind.startsWith("wood")||kind.startsWith("bark")?1.35:.6;
    let nx=-dx*strength,ny=-dy*strength,nz=1;const len=Math.hypot(nx,ny,nz);
    return [clamp(128+127*nx/len),clamp(128+127*ny/len),clamp(128+127*nz/len),255];
  });
}

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

}else if(id==="door_meeting_glass01"){
for(let x of [-.037,1.037])bx("frame","steel_painted",x,1.08,0,.075,2.16,.14);bx("frame","steel_painted",.5,2.12,0,1.08,.08,.14);
for(let x of [.09,.91])bx("leaf","steel_brushed",x,1.08,0,.055,2.01,.055);
for(let y of [.19,1.99])bx("leaf","steel_brushed",.5,y,0,.83,.07,.055);
bx("glazing","glass",.5,1.10,0,.75,1.73,.009);for(let z of [-.07,.07]){tb("handle","chrome",[.82,1.05,z],[.89,1.05,z],.017,.016,12);tb("handle","chrome",[.82,1.00,z],[.82,1.11,z],.010,.011,10)}
socket(g,"socket_hinge",[0,0,0],"hinge");socket(g,"socket_threshold",[.5,0,0],"portal");socket(g,"col_leaf",[.5,1.07,0],"collision");
}else if(id==="door_service_heavy01"){
for(let x of [-.055,1.10])bx("frame","steel_painted",x,1.16,0,.11,2.28,.19);bx("frame","steel_painted",.52,2.28,0,1.26,.13,.19);
bx("leaf","steel_painted",.52,1.14,0,1.09,2.13,.11);bx("reinforcement","steel_brushed",.52,.19,.063,.99,.07,.03);
bx("reinforcement","steel_brushed",.52,1.89,.063,.99,.09,.03);bx("pushbar","steel_brushed",.56,1.00,.10,.56,.065,.035);
for(let y of [.29,1.16,1.96])bx("hinges","chrome",.018,y,-.005,.07,.14,.08);
socket(g,"socket_hinge",[0,0,0],"hinge");socket(g,"socket_threshold",[.54,0,0],"portal");socket(g,"col_leaf",[.53,1.14,0],"collision");
}else if(id==="door_restroom01"){
for(let x of [-.038,.98])bx("frame","steel_painted",x,1.04,0,.074,2.08,.12);bx("frame","steel_painted",.47,2.08,0,1.07,.075,.12);
bx("leaf","wood_light",.47,1.025,0,.94,1.97,.043);bx("leaf_trim","wood_oiled",.47,1.02,.026,.84,1.87,.008);
for(let z of [-.058,.058])rg("lock_ring","chrome",[.82,1.04,z],.035,.007,14,6);
socket(g,"socket_hinge",[0,0,0],"hinge");socket(g,"socket_threshold",[.47,0,0],"portal");
}else if(id==="partition_solid01"){
bx("core","stone",0,1.35,0,3.0,2.70,.145);for(let z of [-.085,.085])bx("finish","ceramic",0,1.35,z,3,2.70,.011);
bx("baseboard","wood_light",0,.055,.104,3,.11,.018);bx("baseboard","wood_light",0,.055,-.104,3,.11,.018);
socket(g,"socket_host",[0,0,0],"solid_host");socket(g,"socket_neighbor_left",[-1.5,0,0],"edge");socket(g,"socket_neighbor_right",[1.5,0,0],"edge");
}else if(id==="partition_glass01"){
for(let x of [-1.5,0,1.5])bx("mullion","steel_painted",x,1.35,0,.055,2.7,.09);
for(let y of [.045,2.66])bx("frame","steel_painted",0,y,0,3,.09,.10);
for(let x of [-.75,.75])bx("glass","glass",x,1.35,0,1.43,2.55,.014);
socket(g,"socket_host",[0,0,0],"partition_host");socket(g,"socket_neighbor_left",[-1.5,0,0],"edge");socket(g,"socket_neighbor_right",[1.5,0,0],"edge");
}else if(id==="slab_floor01"){
bx("structural","stone",0,-.14,0,3,.28,3);bx("finish","wood_oiled",0,.012,0,3,.024,3);
bx("soffit","ceramic",0,-.282,0,3,.008,3);socket(g,"socket_floor",[0,.024,0],"floor_support");socket(g,"socket_ceiling",[0,-.29,0],"underside");socket(g,"socket_host",[0,-.14,0],"solid_host");
}else if(id==="ceiling_tile01"){
bx("panel","ceramic",0,.0,0,1.18,.017,1.18);for(let s of [-.6,.6]){bx("grid","steel_painted",s,0,0,.028,.027,1.23);bx("grid","steel_painted",0,0,s,1.23,.027,.028)}
socket(g,"socket_ceiling",[0,0,0],"ceiling_support");
}else if(id==="staircase01"){
for(let i=0;i<12;i++){let y=(i+.5)*.17,z=(i+.5)*.29;bx("treads","stone",0,y,z,1.15,.17,.29);bx("nosings","steel_brushed",0,(i+1)*.17,z+.136,1.15,.012,.02)}
bx("stringer","steel_painted",-.58,1.02,1.73,.05,2.0,3.45);bx("stringer","steel_painted",.58,1.02,1.73,.05,2.0,3.45);
socket(g,"socket_lower",[0,0,0],"stair_lower");socket(g,"socket_upper",[0,2.04,3.48],"stair_upper");
}else if(id==="rail_stair01"){
for(let i=0;i<4;i++){let z=i*1.14,y=.91+z*(2.04/3.45);tb("posts","steel_brushed",[-.55,y-.91,z],[-.55,y,z],.025,.025,12)}
tb("handrail","steel_brushed",[-.55,.91,0],[-.55,2.95,3.45],.041,.041,14);
for(let i=0;i<4;i++){let z=i*1.14,y=.45+z*(2.04/3.45);tb("guard","steel_painted",[-.55,y,z],[-.55,y+1.00,z],.018,.017,8)}
socket(g,"socket_stair_lower",[-.55,0,0],"stair_side");socket(g,"socket_stair_upper",[-.55,2.04,3.45],"stair_side");
}else if(id==="reception_counter01"){
bx("carcass","wood_oiled",0,.52,0,2.2,1.02,.65);bx("countertop","wood_light",0,1.06,.05,2.35,.065,.78);
bx("knee_space","plastic_dark",0,.33,-.32,1.35,.65,.014);
for(let i=-4;i<=4;i++)bx("face_slats","wood_light",i*.225,.54,.34,.16,.94,.028);
socket(g,"socket_reception",[0,1.10,.05],"surface");socket(g,"socket_power",[0,.26,-.33],"electric");
}else if(id==="shelf_office01"){
for(let x of [-.45,.45])bx("frame","steel_painted",x,.90,0,.037,1.8,.37);for(let y of [.03,.52,1.04,1.55,1.77])bx("shelves","wood_light",0,y,0,.91,.036,.38);
bx("back","plastic_mid",0,.9,-.195,.94,1.80,.018);
socket(g,"socket_floor",[0,0,0],"floor");socket(g,"socket_wall",[0,1.5,-.21],"wall_tie");
}else if(id==="cabinet_tall01"){
bx("carcass","steel_painted",0,1.0,0,.9,2,.48);for(let x of [-.216,.216]){bx("doors","plastic_mid",x,1.03,.247,.42,1.88,.022);bx("handles","chrome",x+(x>0?-.10:.10),1.11,.267,.020,.18,.018)}
for(let y of [.42,.83,1.28,1.64])bx("shelves","steel_brushed",0,y,0,.82,.025,.40);
socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="chair_meeting01"){
for(let x of [-.22,.22])for(let z of [-.20,.20])tb("legs","steel_brushed",[x,0,z],[x,.47,z],.014,.014,8);
ob("seat","fabric_blue",[0,.50,.0],[.27,.065,.26],20,9);ob("back","fabric_blue",[0,.84,-.22],[.26,.29,.065],18,10);
tb("spine","steel_brushed",[0,.49,-.23],[0,1.06,-.23],.019,.019,10);socket(g,"socket_seat",[0,.55,0],"seat");
}else if(id==="desktop_tower01"){
bx("chassis","plastic_dark",0,.20,0,.19,.38,.37);bx("panel","steel_painted",0,.20,.190,.16,.34,.008);
for(let i=0;i<8;i++)bx("vent","plastic_mid",-.045+i*.013,.30,.199,.006,.040,.004);
tb("power_led","light_emissive",[.062,.10,.201],[.062,.115,.201],.005,.005,6);
socket(g,"socket_power",[0,.12,-.19],"electric");socket(g,"socket_data",[.04,.20,-.19],"data");socket(g,"socket_display",[-.045,.20,-.19],"data");
}else if(id==="docking_station01"){
bx("body","plastic_dark",0,.023,0,.18,.044,.088);bx("lid","plastic_mid",0,.045,0,.15,.002,.070);
for(let i=0;i<5;i++)bx("ports","steel_brushed",-.063+i*.031,.020,.045,.02,.009,.004);
socket(g,"socket_power",[-.04,.02,-.045],"electric");socket(g,"socket_data",[.04,.02,-.045],"data");
}else if(id==="keyboard_compact01"){
bx("chassis","plastic_dark",0,.022,0,.31,.032,.123);
for(let r=0;r<4;r++)for(let k=0;k<11;k++)bx("keycaps","plastic_mid",-.135+k*.027,.041,-.043+r*.028,.022,.013,.022);
bx("spacebar","plastic_mid",0,.041,.056,.13,.013,.023);
socket(g,"socket_usb",[.13,.022,-.063],"data");
}else if(id==="outlet_dual01"){
bx("plate","ceramic",0,.07,0,.083,.14,.009);for(let y of [.034,.104]){for(let x of [-.013,.013])bx("slots","plastic_dark",x,y,.008,.005,.017,.002);bx("slots","plastic_dark",0,y-.013,.008,.005,.005,.002)}
socket(g,"socket_power",[0,.07,-.006],"electric");socket(g,"socket_wall",[0,.07,0],"wall");
}else if(id==="mirror_wall01"){
bx("frame","chrome",0,.50,0,.62,1,.024);bx("mirror","glass",0,.5,.016,.59,.97,.004);
socket(g,"socket_wall",[0,.5,-.018],"wall");
}else if(id==="soap_dispenser01"){
bx("body","ceramic",0,.11,0,.115,.22,.09);bx("window","plastic_dark",0,.14,.05,.043,.07,.003);
tb("pump","chrome",[0,.23,.005],[0,.29,.005],.013,.011,10);
tb("spout","chrome",[0,.29,.005],[0,.29,.055],.010,.009,10);
socket(g,"socket_wall",[0,.16,-.05],"wall");
}else if(id==="grabbar_wall01"){
tb("rail","chrome",[-.36,.08,.115],[.36,.08,.115],.025,.025,16);
for(let x of [-.34,.34]){tb("standoff","chrome",[x,.08,.115],[x,.08,.008],.027,.025,12);tb("flange","chrome",[x,.08,.008],[x,.08,-.002],.053,.053,14)}
socket(g,"socket_wall",[0,.08,0],"wall_load");
}else if(id==="toilet_wall01"){
ob("bowl","ceramic",[0,.46,.16],[.29,.23,.37],24,16);rg("seat","ceramic",[0,.665,.19],.248,.046,28,10);rg("lid","ceramic",[0,.714,-.13],.20,.025,24,8);
bx("flush_plate","steel_brushed",0,1.1,-.245,.22,.14,.012);bx("button","chrome",0,1.1,-.238,.12,.07,.004);
socket(g,"socket_wall",[0,.48,-.22],"wall_support");socket(g,"socket_waste",[0,.39,-.23],"drain");
}else if(id==="tree_fir01"||id==="tree_maple01"){
const pine=id==="tree_fir01",H=pine?10:11.2;tb("trunk",pine?"bark":"bark_birch",[0,0,0],[0,H*.88,0],.26,.055,20);
for(let i=0;i<19;i++){let y=pine?1.1+i*.39:2.4+i*.39,L=pine?(2.4*(1-y/H)+.25):(1.8+.3*Math.sin(i*1.9));for(let j=0;j<(pine?9:7);j++){let a=i*.77+j*2*Math.PI/(pine?9:7),dx=Math.cos(a),dz=Math.sin(a);tb("branches","bark",[0,y,0],[dx*L,y+(pine?-.21:.27),dz*L],.042,.008,7);for(let k=1;k<=4;k++){let s=.25+k*.175,p=[dx*L*s,y+(pine?-.19:.33)*s,dz*L*s];leaf(g,pine?"needles":"foliage",pine?"needle_mask":"foliage_mask",p,pine?[.23,.40]:[.42,.57],a+k*.22)}}}
socket(g,"socket_root",[0,0,0],"terrain");socket(g,"socket_wind",[0,H*.65,0],"wind_pivot");socket(g,"col_trunk",[0,H*.42,0],"collision");
}else if(id==="shrub01"){
for(let i=0;i<16;i++){let a=i*2.39996,h=.45+(i%5)*.16,r=.32+(i%4)*.09,x=Math.cos(a)*r,z=Math.sin(a)*r;
tb("twigs","bark",[0,.03,0],[x,h,z],.018,.005,6);for(let j=0;j<4;j++)leaf(g,"foliage","foliage_mask",[x*(.65+j*.12),h*(.6+j*.12),z*(.65+j*.12)],[.28,.35],a+j*.45)}
socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="rock_cluster01"){
for(let i=0;i<7;i++){let a=i*2.39996,x=.36*Math.cos(a),z=.33*Math.sin(a);ob("stone","stone",[x,.10+(i%3)*.02,z],[.20+(i%4)*.035,.16+(i%3)*.03,.22],9,6)}socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="deadlog01"){
tb("log","bark",[-1.08,.20,0],[1.08,.21,0],.21,.16,18);tb("cut_end","wood_light",[1.08,.21,0],[1.095,.21,0],.155,.155,18);
for(let i=0;i<6;i++){let x=-.9+i*.33;tb("twig","bark",[x,.32,0],[x+.09,.52,((i%3)-1)*.18],.022,.004,7)}
socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="planter_tall01"){
tb("pot","ceramic",[0,0,0],[0,.60,0],.19,.24,20);tb("soil","soil",[0,.60,0],[0,.615,0],.205,.205,18);
for(let i=0;i<22;i++){let a=i*2.4,r=.10+.17*(i%6)/6,h=.80+(i%8)*.10,x=Math.cos(a)*r,z=Math.sin(a)*r;tb("stems","needle_green",[0,.56,0],[x,h,z],.009,.004,7);leaf(g,"foliage","foliage_mask",[x,h,z],[.19,.26],a)}
socket(g,"socket_ground",[0,0,0],"floor");
}else if(id==="lamp_desk01"){
bx("base","steel_brushed",0,.018,0,.18,.035,.16);tb("arm","steel_brushed",[0,.04,0],[.09,.46,0],.016,.015,12);tb("arm","steel_brushed",[.09,.46,0],[.27,.62,0],.015,.015,12);
tb("shade","steel_painted",[.27,.51,0],[.27,.63,0],.13,.072,20);tb("emitter","light_emissive",[.27,.51,0],[.27,.525,0],.098,.098,16);
socket(g,"socket_power",[0,.01,-.08],"electric");socket(g,"socket_light",[.27,.48,0],"light_down");
}else if(id==="duct_vent01"){
bx("duct","steel_painted",0,.18,0,1.1,.35,.42);bx("flange","steel_brushed",-.52,.18,0,.06,.42,.50);bx("flange","steel_brushed",.52,.18,0,.06,.42,.50);
for(let i=0;i<8;i++)bx("grille","plastic_dark",.56,.18,-.16+i*.045,.012,.29,.016);
socket(g,"socket_air_in",[-.55,.18,0],"air_in");socket(g,"socket_air_out",[.55,.18,0],"air_out");socket(g,"socket_plenum",[0,.34,0],"plenum");
}else if(id==="cable_tray01"){
bx("tray","steel_painted",0,.025,0,1.3,.025,.25);for(let z of [-.13,.13])bx("sides","steel_painted",0,.083,z,1.3,.13,.02);
for(let i=0;i<8;i++)tb("cables","plastic_dark",[-.62+i*.03,.05,-.07],[.62,.05,-.07+i*.018],.008,.008,6);
socket(g,"socket_data_in",[-.65,.07,0],"data");socket(g,"socket_data_out",[.65,.07,0],"data");socket(g,"socket_support",[0,.12,0],"plenum");
}else if(id==="downlight01"){
tb("trim","chrome",[0,.04,0],[0,.075,0],.09,.09,20);tb("emitter","light_emissive",[0,.039,0],[0,.043,0],.075,.075,18);
socket(g,"socket_ceiling",[0,.08,0],"ceiling");socket(g,"socket_light",[0,.022,0],"light_down");
}else if(id==="roller_blinds01"){
tb("roller","steel_painted",[-.74,1.78,0],[.74,1.78,0],.035,.035,16);
bx("fabric","fabric_dark",0,.96,0,1.46,1.58,.006);bx("bottom_bar","steel_brushed",0,.17,0,1.46,.022,.022);
socket(g,"socket_lintel",[0,1.8,0],"lintel");socket(g,"socket_open",[0,1.78,0],"animated_slider");
}else if(id==="trash_bin01"){
tb("body","plastic_dark",[0,.01,0],[0,.38,0],.14,.18,18);rg("rim","steel_brushed",[0,.382,0],.16,.016,20,7);
socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="clock_wall01"){
tb("case","plastic_mid",[0,0,-.02],[0,0,.02],.19,.19,24);
tb("dial","ceramic",[0,0,.024],[0,0,.03],.17,.17,24);
tb("hand","plastic_dark",[0,0,.035],[.09,.06,.035],.006,.006,8);tb("hand","plastic_dark",[0,0,.036],[-.03,.12,.036],.004,.004,8);
socket(g,"socket_wall",[0,0,-.03],"wall");
}else if(id==="whiteboard01"){
bx("frame","steel_brushed",0,.55,0,1.45,1.10,.04);bx("board","ceramic",0,.55,.024,1.39,1.04,.005);
bx("marker_tray","steel_painted",0,.03,.09,.65,.026,.14);socket(g,"socket_wall",[0,.55,-.03],"wall");
}else if(id==="potted_succulent01"){
tb("pot","ceramic",[0,0,0],[0,.14,0],.09,.115,16);tb("soil","soil",[0,.138,0],[0,.146,0],.098,.098,14);
for(let i=0;i<24;i++){let a=i*2.39996,r=.028+(i%5)*.014,y=.16+(i%5)*.023;ob("leaves","needle_green",[Math.cos(a)*r,y,Math.sin(a)*r],[.034,.053,.018],8,6)}
socket(g,"socket_surface",[0,0,0],"surface");
}else throw Error("asset "+id);
g.textured=/desk_office|chair_ergonomic|monitor_24|door_office|door_meeting|toilet_floor|sink_wall|tree_|fern|meeting_table|partition_solid|reception_counter|shelf_office|cabinet_tall|chair_meeting|forest_patch|shrub|planter_tall|staircase|slab_floor|rock_cluster|deadlog/.test(id);
g.textureSize= /desk_office|door_office|tree_|meeting_table|chair_ergonomic/.test(id)?128:64;
return g}

const pbrCache=new Map();
function pngTextureCache(kind,channel,dim){const key=kind+":"+channel+":"+dim;if(!pbrCache.has(key))pbrCache.set(key,pbrTexture(kind,channel,dim));return pbrCache.get(key)}
function buildGLB(g){const used=new Set(Object.values(g.groups).map(p=>p.material));const mats=Object.values(g.materials).filter(m=>used.has(m.name)),matIndex=Object.fromEntries(mats.map((m,i)=>[m.name,i]));let bytes=[],views=[],acc=[],meshes=[],nodes=[],imgs=[],tx=[],samplers=[],texturesUsed={};let byteLen=0;function bufferAdd(raw,target){let padding=(4-byteLen%4)%4;if(padding){bytes.push(new Uint8Array(padding));byteLen+=padding}let index=views.length;views.push({buffer:0,byteOffset:byteLen,byteLength:raw.length,...(target?{target}:{})});bytes.push(raw);byteLen+=raw.length;return index}function accessor(raw,count,type,component,min,max,target){let idx=acc.length;acc.push({bufferView:bufferAdd(raw,target),componentType:component,count,type,...(min?{min,max}:{})});return idx}
for(const kind of ["foliage_mask","needle_mask"].filter(n=>Object.values(g.groups).some(p=>p.material===n))){let p=pngMask(kind==="needle_mask"?"needle":"leaf"),iv=bufferAdd(p),i=imgs.length;imgs.push({mimeType:"image/png",bufferView:iv,name:kind+"_alpha"});if(!samplers.length)samplers.push({magFilter:9729,minFilter:9987,wrapS:10497,wrapT:10497});texturesUsed[kind]=tx.length;tx.push({source:i,sampler:0})}

for(const m of mats){
  if(texturesUsed[m.name]!==undefined){
    m.pbrMetallicRoughness.baseColorTexture={index:texturesUsed[m.name],texCoord:0};
    continue;
  }
  if(!g.textured || !["wood_light","wood_oiled","bark","bark_birch","stone","soil","fabric_blue","fabric_dark","steel_brushed","steel_painted","plastic_dark","plastic_mid","grass","ceramic","needle_green","leaf_green"].includes(m.name))continue;
  const dim=g.textureSize||64;
  const add=(channel)=>{
    const bytes=pngTextureCache(m.name,channel,dim),iv=bufferAdd(bytes),i=imgs.length;
    imgs.push({mimeType:"image/png",bufferView:iv,name:m.name+"_"+channel});
    if(!samplers.length)samplers.push({magFilter:9729,minFilter:9987,wrapS:10497,wrapT:10497});
    tx.push({source:i,sampler:0});return tx.length-1;
  };
  m.pbrMetallicRoughness.baseColorTexture={index:add("color"),texCoord:0};
  m.pbrMetallicRoughness.metallicRoughnessTexture={index:add("orm"),texCoord:0};
  m.occlusionTexture={index:tx.length-1,texCoord:0,strength:.35};
  m.normalTexture={index:add("normal"),texCoord:0,scale:.5};
}

let byNode={};for(const p of Object.values(g.groups)){if(!p.idx.length)continue;let attrs={POSITION:[],NORMAL:[],TEXCOORD_0:[]},low=[Infinity,Infinity,Infinity],high=[-Infinity,-Infinity,-Infinity];for(let i=0;i<p.v.length;i+=8){for(let k=0;k<3;k++){attrs.POSITION.push(p.v[i+k]);low[k]=Math.min(low[k],p.v[i+k]);high[k]=Math.max(high[k],p.v[i+k]);attrs.NORMAL.push(p.v[i+k+3])}attrs.TEXCOORD_0.push(p.v[i+6],p.v[i+7])}const prim={attributes:{POSITION:accessor(f32(attrs.POSITION),attrs.POSITION.length/3,"VEC3",5126,low,high,34962),NORMAL:accessor(f32(attrs.NORMAL),attrs.NORMAL.length/3,"VEC3",5126,null,null,34962),TEXCOORD_0:accessor(f32(attrs.TEXCOORD_0),attrs.TEXCOORD_0.length/2,"VEC2",5126,null,null,34962)},indices:accessor(u32(p.idx),p.idx.length,"SCALAR",5125,null,null,34963),material:matIndex[p.material],mode:4};(byNode[p.node]||(byNode[p.node]=[])).push(prim)}
for(const [name,prims] of Object.entries(byNode)){const mi=meshes.length;meshes.push({name,primitives:prims});nodes.push({name,mesh:mi})}
for(const s of Object.values(g.sockets))nodes.push({name:s.node,translation:s.translation,extras:{oasis_role:s.role}});
let json={asset:{version:"2.0",generator:"OASIS authored parametric geometry v1"},scene:0,scenes:[{nodes:nodes.map((_,i)=>i)}],nodes,meshes,materials:mats,accessors:acc,bufferViews:views,buffers:[{byteLength:byteLen}],images:imgs,textures:tx,samplers,extras:{units:"meters",up:"Y",pack:"oasis_mg2_v1"}},js=pad4(utf8(JSON.stringify(json)),32),bin=pad4(cat(bytes));let glb=cat([le32(0x46546c67),le32(2),le32(12+8+js.length+8+bin.length),le32(js.length),le32(0x4e4f534a),js,le32(bin.length),le32(0x004e4942),bin]);let lo=[Infinity,Infinity,Infinity],hi=[-Infinity,-Infinity,-Infinity],tris=0;for(const p of Object.values(g.groups)){tris+=p.idx.length/3;for(let i=0;i<p.v.length;i+=8)for(let k=0;k<3;k++){lo[k]=Math.min(lo[k],p.v[i+k]);hi[k]=Math.max(hi[k],p.v[i+k])}}return {glb,triangles:tris,nodes:nodes.map(n=>n.name),bounds:{min:lo,max:hi},materials:Object.keys(g.materials).filter(n=>Object.values(g.groups).some(p=>p.material===n)),images:imgs.length}}

import {mkdirSync,writeFileSync} from "node:fs";
import {dirname,join} from "node:path";
const ROOT=process.argv[2]||"assets/packs/oasis_mg2_v1";
const ids=["desk_office01","chair_ergonomic01","monitor_24in01","keyboard_full01","mouse_office01","door_office_solid01","toilet_floor01","sink_wall01","plant_potted01","tree_spruce01","tree_birch01","fern01","cabinet_2drawer01","meeting_table01","light_panel01","window_bay01","supply_set01","forest_patch01","door_meeting_glass01","door_service_heavy01","door_restroom01","partition_solid01","partition_glass01","slab_floor01","ceiling_tile01","staircase01","rail_stair01","reception_counter01","shelf_office01","cabinet_tall01","chair_meeting01","desktop_tower01","docking_station01","keyboard_compact01","outlet_dual01","mirror_wall01","soap_dispenser01","grabbar_wall01","toilet_wall01","tree_fir01","tree_maple01","shrub01","rock_cluster01","deadlog01","planter_tall01","lamp_desk01","duct_vent01","cable_tray01","downlight01","roller_blinds01","trash_bin01","clock_wall01","whiteboard01","potted_succulent01"];
for(const id of ids){const g=makeAsset(id),{glb,triangles,bounds}=buildGLB(g),file=join(ROOT,"geometry",id+".glb");mkdirSync(dirname(file),{recursive:true});writeFileSync(file,glb);console.log(id,triangles,glb.length,bounds)}
