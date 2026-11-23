function utf8(s){const q=unescape(encodeURIComponent(s)),a=new Uint8Array(q.length);for(let i=0;i<q.length;i++)a[i]=q.charCodeAt(i);return a}

function b64(b){const cs="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",o=[];for(let i=0;i<b.length;i+=3){let n=(b[i]<<16)|((b[i+1]||0)<<8)|(b[i+2]||0);o.push(cs[n>>>18],cs[(n>>>12)&63],i+1<b.length?cs[(n>>>6)&63]:"=",i+2<b.length?cs[n&63]:"=")}return o.join("")}

function cat(arr){let n=arr.reduce((s,a)=>s+a.length,0),r=new Uint8Array(n),p=0;for(const a of arr){r.set(a,p);p+=a.length}return r}

function le32(v){let x=new Uint8Array(4);new DataView(x.buffer).setUint32(0,v>>>0,true);return x}

function be32(v){let x=new Uint8Array(4);new DataView(x.buffer).setUint32(0,v>>>0,false);return x}

function f32(v){let x=new Uint8Array(v.length*4),d=new DataView(x.buffer);v.forEach((n,i)=>d.setFloat32(i*4,n,true));return x}

function u32(v){let x=new Uint8Array(v.length*4),d=new DataView(x.buffer);v.forEach((n,i)=>d.setUint32(i*4,n,true));return x}

function pad4(x,fill=0){return x.length%4?cat([x,new Uint8Array(Array(4-x.length%4).fill(fill))]):x}

function crc32(a){let c=-1;for(const b of a){c^=b;for(let k=0;k<8;k++)c=(c>>>1)^((c&1)?0xedb88320:0)}return (c^(-1))>>>0}

function pngMask(kind){const W=kind==="maple"||kind==="birch"?96:48,H=W,raw=[];for(let y=0;y<H;y++){raw.push(0);for(let x=0;x<W;x++){let u=(x+.5-W/2)/(W/2),v=(y+.5-H/2)/(H/2),a;if(kind==="needle")a=Math.abs(u)<.17*(1-Math.abs(v))+.02&&Math.abs(v)<.94?255:0;else if(kind==="maple"){let t=Math.atan2(u,-v),r=Math.hypot(u,v),lobes=.62+.18*Math.cos(5*t)+.08*Math.cos(3*t+.5);a=r<lobes&&v<.85?255:0;}else if(kind==="birch"){let t=Math.atan2(u,-v),r=Math.hypot(u/.75,v),tooth=.79+.055*Math.cos(18*t);a=r<tooth&&v<.80?255:0;}else a=(u*u/.58+v*v)<.86 && Math.abs(u)<(.9-.12*Math.abs(v))?255:0;raw.push(250,252,249,a)}}const len=raw.length,z=[120,1],d=new Uint8Array(raw);let pos=0;while(pos<d.length){let chunk=Math.min(65535,d.length-pos);z.push(pos+chunk===d.length?1:0,chunk&255,chunk>>>8,(~chunk)&255,((~chunk)>>>8)&255);for(let i=pos;i<pos+chunk;i++)z.push(d[i]);pos+=chunk}let a=1,b=0;for(const v of d){a=(a+v)%65521;b=(b+a)%65521}z.push(...be32((b<<16)|a));function ck(type,bytes){let t=utf8(type);return cat([be32(bytes.length),t,bytes,be32(crc32(cat([t,bytes])))])}return cat([Uint8Array.from([137,80,78,71,13,10,26,10]),ck("IHDR",cat([be32(W),be32(H),Uint8Array.from([8,6,0,0,0])])),ck("IDAT",Uint8Array.from(z)),ck("IEND",new Uint8Array())])}


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

function mat(g,n,rgba,metal=.0,rough=.7,mode="OPAQUE"){g.materials[n]={name:n,pbrMetallicRoughness:{baseColorFactor:rgba,metallicFactor:metal,roughnessFactor:rough},alphaMode:mode,doubleSided:mode==="MASK"};if(mode==="MASK")g.materials[n].alphaCutoff=.48;if(n==="light_emissive")g.materials[n].emissiveFactor=[1,.88,.68]}

function part(g,n,m){const key=n+"|"+m;return g.groups[key]||(g.groups[key]={node:n,material:m,v:[],idx:[]})}

function quad(g,n,m,a,b,c,d){const p=part(g,n,m),off=p.v.length/8,u=[b[0]-a[0],b[1]-a[1],b[2]-a[2]],v=[c[0]-a[0],c[1]-a[1],c[2]-a[2]];let nn=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]],ln=Math.hypot(...nn)||1;nn=nn.map(x=>x/ln);for(const [pt,uv] of [[a,[0,0]],[b,[1,0]],[c,[1,1]],[d,[0,1]]])p.v.push(...pt,...nn,...uv);p.idx.push(off,off+1,off+2,off,off+2,off+3)}

function box(g,n,m,x,y,z,w,h,d){let X=x-w/2,R=x+w/2,B=y-h/2,T=y+h/2,F=z+d/2,K=z-d/2;quad(g,n,m,[X,B,F],[R,B,F],[R,T,F],[X,T,F]);quad(g,n,m,[R,B,K],[X,B,K],[X,T,K],[R,T,K]);quad(g,n,m,[X,B,K],[X,B,F],[X,T,F],[X,T,K]);quad(g,n,m,[R,B,F],[R,B,K],[R,T,K],[R,T,F]);quad(g,n,m,[X,T,F],[R,T,F],[R,T,K],[X,T,K]);quad(g,n,m,[X,B,K],[R,B,K],[R,B,F],[X,B,F])}

function tube(g,n,m,a,b,r0,r1,N=12,cap=true){let p=part(g,n,m),off=p.v.length/8,axis=b.map((v,i)=>v-a[i]),al=Math.hypot(...axis)||1,up=axis.map(v=>v/al),arb=Math.abs(up[1])<.92?[0,1,0]:[1,0,0],s=[up[1]*arb[2]-up[2]*arb[1],up[2]*arb[0]-up[0]*arb[2],up[0]*arb[1]-up[1]*arb[0]],sl=Math.hypot(...s)||1;s=s.map(x=>x/sl);const t=[up[1]*s[2]-up[2]*s[1],up[2]*s[0]-up[0]*s[2],up[0]*s[1]-up[1]*s[0]];for(let k=0;k<=N;k++){let ang=2*Math.PI*k/N,rad= s.map((v,i)=>v*Math.cos(ang)+t[i]*Math.sin(ang));for(let j=0;j<2;j++){const cen=j?b:a,r=j?r1:r0,vertex=cen.map((v,i)=>v+r*rad[i]);p.v.push(...vertex,...rad,k/N,j)}}for(let k=0;k<N;k++){let q=off+2*k;p.idx.push(q,q+2,q+1,q+1,q+2,q+3)}if(cap){for(let end=0;end<2;end++){const cen=end?b:a,normal=end?up:up.map(v=>-v),r=end?r1:r0,st=p.v.length/8;p.v.push(...cen,...normal,0.5,0.5);for(let k=0;k<=N;k++){let th=2*Math.PI*k/N,rad=s.map((v,i)=>v*Math.cos(th)+t[i]*Math.sin(th));p.v.push(...cen.map((v,i)=>v+r*rad[i]),...normal,.5+.5*Math.cos(th),.5+.5*Math.sin(th))}for(let k=0;k<N;k++)if(end)p.idx.push(st,st+1+k,st+2+k);else p.idx.push(st,st+2+k,st+1+k)}}}

function orb(g,n,m,c,sz,N=12,M=8){const p=part(g,n,m),off=p.v.length/8;for(let j=0;j<=M;j++){let lat=Math.PI*j/M;for(let k=0;k<=N;k++){let lon=2*Math.PI*k/N,sx=Math.sin(lat)*Math.cos(lon),sy=Math.cos(lat),sz0=Math.sin(lat)*Math.sin(lon),normal=[sx/sz[0],sy/sz[1],sz0/sz[2]],ln=Math.hypot(...normal)||1;p.v.push(c[0]+sx*sz[0],c[1]+sy*sz[1],c[2]+sz0*sz[2],...normal.map(v=>v/ln),k/N,j/M)}}for(let j=0;j<M;j++)for(let k=0;k<N;k++){const a=off+j*(N+1)+k,b=a+N+1;p.idx.push(a,a+1,b,a+1,b+1,b)}}

function torus(g,n,m,c,rad,thick,N=24,M=8){const p=part(g,n,m),off=p.v.length/8;for(let i=0;i<=N;i++){let a=2*Math.PI*i/N;for(let j=0;j<=M;j++){let b=2*Math.PI*j/M,v=Math.cos(b),w=Math.sin(b),u=Math.cos(a),t=Math.sin(a);p.v.push(c[0]+(rad+thick*v)*u,c[1]+thick*w,c[2]+(rad+thick*v)*t,u*v,w,t*v,i/N,j/M)}}for(let i=0;i<N;i++)for(let j=0;j<M;j++){let a=off+i*(M+1)+j,b=a+M+1;p.idx.push(a,a+1,b,a+1,b+1,b)}}

function leaf(g,node,ma,c,s,rot=0){let [x,y,z]=c,w=s[0]/2,h=s[1]/2,ca=Math.cos(rot),sa=Math.sin(rot),axis=[ca,0,sa],d=[-sa,0,ca];let a=[x-w*axis[0],y-h,z-w*axis[2]],b=[x+w*axis[0],y-h,z+w*axis[2]],cc=[x+w*axis[0]+.03*d[0],y+h,z+w*axis[2]+.03*d[2]],dd=[x-w*axis[0]+.03*d[0],y+h,z-w*axis[2]+.03*d[2]];quad(g,node,ma,a,b,cc,dd)}

function socket(g,n,p,role){g.sockets[n]={node:n,translation:p,role}}


function bevelBox(g,node,material,x,y,z,w,h,d,r=.012){
  const half=[w/2,h/2,d/2],rad=Math.min(r,...half.map(a=>a*.49));
  const faces=[
    [2,1,0,1,1,1],[2,-1,0,1,-1,1],[0,1,2,1,-1,1],
    [0,-1,2,1,1,1],[1,1,0,2,1,-1],[1,-1,0,2,1,1]
  ];
  const mesh=part(g,node,material);
  for(const [normalAxis,sign,ua,va,us,vs] of faces){
    const uslots=[-half[ua],-half[ua]+rad,half[ua]-rad,half[ua]],
          vslots=[-half[va],-half[va]+rad,half[va]-rad,half[va]];
    const base=mesh.v.length/8;
    for(let j=0;j<4;j++)for(let i=0;i<4;i++){
      const p=[0,0,0];p[normalAxis]=half[normalAxis]*sign;
      p[ua]=uslots[us>0?i:3-i];p[va]=vslots[vs>0?j:3-j];
      const nearest=p.map((v,k)=>Math.max(-half[k]+rad,Math.min(half[k]-rad,v)));
      const delta=p.map((v,k)=>v-nearest[k]),length=Math.hypot(...delta)||1;
      const n=delta.map(v=>v/length);
      const smooth=nearest.map((v,k)=>v+n[k]*rad);
      mesh.v.push(smooth[0]+x,smooth[1]+y,smooth[2]+z,...n,i/3,j/3);
    }
    for(let j=0;j<3;j++)for(let i=0;i<3;i++){
      const a=base+j*4+i,b=a+1,c=a+5,e=a+4;
      mesh.idx.push(a,b,c,a,c,e);
    }
  }
}
function cableRoute(g,node,material,points,thickness=.006){
  for(let i=1;i<points.length;i++)tube(g,node,material,points[i-1],points[i],thickness,thickness,8,i===1||i===points.length-1);
}


function terrainMesh(g,name,material,size=8,N=72){
 const p=part(g,name,material),base=p.v.length/8;
 function height(x,z){
   const u=(x/size+.5),v=(z/size+.5);
   const e=Math.pow(Math.sin(Math.PI*u)*Math.sin(Math.PI*v),2);
   return .052*e*(Math.sin(x*2.6)*.42+Math.cos(z*3.3)*.22+tileNoise(u,v,11,62)*.6);
 }
 const step=size/N;
 for(let j=0;j<=N;j++)for(let i=0;i<=N;i++){
   const x=-size/2+step*i,z=-size/2+step*j,y=height(x,z);
   const dx=(height(x+.012,z)-height(x-.012,z))/.024,dz=(height(x,z+.012)-height(x,z-.012))/.024;
   const L=Math.hypot(dx,1,dz),normal=[-dx/L,1/L,-dz/L];
   p.v.push(x,y,z,...normal,i/N*4,j/N*4);
 }
 for(let j=0;j<N;j++)for(let i=0;i<N;i++){
   let a=base+j*(N+1)+i,b=a+1,c=a+N+1,d=c+1;
   p.idx.push(a,c,b,b,c,d);
 }
}


function heroDetail(g,id){
 const ring=(name,material,c,r,t,N=48)=>torus(g,name,material,c,r,t,N,8);
 const bead=(name,material,a,b,r=.004,N=12)=>tube(g,name,material,a,b,r,r,N);
 const L=(name,material,points,r=.004,N=12)=>cableRoute(g,name,material,points,r);
 if(id==="fire_extinguisher01"){
   mat(g,"hero_red_enamel",[.72,.014,.022,1],.17,.26);
   mat(g,"hero_black_rubber",[.018,.022,.026,1],0,.8);
   mat(g,"hero_gauge_face",[.92,.95,.89,1],0,.75);
   mat(g,"hero_green",[.05,.53,.13,1],0,.5);
   mat(g,"hero_red_zone",[.79,.04,.035,1],0,.6);
   mat(g,"hero_instruction",[1,1,1,1],0,.9);
   // 96-sided cylinder, rounded shoulder and steel welded foot.
   tube(g,"hero_body","hero_red_enamel",[0,.125,0],[0,.517,0],.095,.095,96);
   orb(g,"hero_domeshoulder","hero_red_enamel",[0,.508,0],[.095,.030,.095],72,16);
   orb(g,"hero_domefoot","hero_red_enamel",[0,.126,0],[.095,.023,.095],64,12);
   ring("body_weld","steel_brushed",[0,.139,0],.095,.0013,80);
   ring("body_weld","steel_brushed",[0,.494,0],.095,.001,80);
   tube(g,"valve_assembly","chrome",[0,.54,0],[0,.595,0],.030,.028,36);
   bevelBox(g,"squeeze_lever","steel_brushed",0,.637,-.031,.18,.021,.045,.007);
   bevelBox(g,"lower_handle","hero_black_rubber",0,.607,-.041,.178,.028,.046,.006);
   bead("fulcrum","chrome",[-.062,.619,-.028],[-.062,.642,-.028],.010);
   bead("safety_pin","steel_brushed",[-.078,.615,.010],[.069,.615,.010],.004);
   ring("split_ring","chrome",[.081,.615,.010],.019,.0025,32);
   L("tamper_wire","hero_red_zone",[[.073,.615,.018],[.096,.593,.031],[.10,.567,.045],[.081,.550,.048]],.0018);
   L("hose","hero_black_rubber",[[.044,.55,-.007],[.090,.549,-.003],[.14,.481,.026],[.152,.386,.050],[.144,.271,.073],[.132,.219,.076]],.011);
   tube(g,"nozzle","hero_black_rubber",[.132,.219,.076],[.130,.187,.076],.021,.022,28);
   tube(g,"hose_crimp","chrome",[.048,.55,-.007],[.070,.55,-.006],.017,.017,26);
   tube(g,"pressure_gauge_housing","steel_brushed",[-.057,.558,.066],[-.057,.558,.077],.041,.041,48);
   tube(g,"pressure_gauge_face","hero_gauge_face",[-.057,.558,.078],[-.057,.558,.080],.032,.032,48);
   for(let i=0;i<=30;i++){const a=(-.84+1.68*i/30)*Math.PI;const r=i%5===0?.022:.026;
     bead("gauge_graduations",i<6||i>24?"hero_red_zone":"plastic_dark",
       [-.057+r*Math.cos(a),.558+r*Math.sin(a),.082],
       [-.057+.029*Math.cos(a),.558+.029*Math.sin(a),.082],i%5===0?.0012:.0007,6);}
   for(let i=0;i<13;i++){const a=-1.01+i*.155;
     bead("green_range","hero_green",
       [-.057+.024*Math.cos(a),.558+.024*Math.sin(a),.083],
       [-.057+.024*Math.cos(a+.155),.558+.024*Math.sin(a+.155),.083],.0014,7);}
   bead("gauge_needle","hero_red_zone",[-.057,.558,.085],[-.045,.580,.085],.0018);
   orb(g,"gauge_hub","chrome",[-.057,.558,.086],[.004,.004,.003],12,8);
   // Instruction sheet follows the can curvature rather than a floating square.
   const label=part(g,"instruction_print","hero_instruction"),N=32,o=label.v.length/8;
   for(let row=0;row<2;row++)for(let i=0;i<=N;i++){
      const t=i/N,a=(t-.5)*1.65,r=.096;
      label.v.push(Math.sin(a)*r,row?.462:.218,Math.cos(a)*r,Math.sin(a),0,Math.cos(a),t,row?0:1);
   }
   for(let i=0;i<N;i++){const a=o+i,b=a+N+1;label.idx.push(a,a+1,b,b,a+1,b+1);}
   bevelBox(g,"wall_mount","steel_brushed",0,.34,-.10,.06,.33,.019,.006);
   socket(g,"socket_pin",[.069,.615,.010],"pin_grip");
   socket(g,"socket_gauge",[-.057,.558,.087],"pressure_readout");
   g.heroLabel=true;
 }else if(id==="light_panel01"){
   mat(g,"hero_opal_dome",[.82,.87,.88,.44],0,.28,"BLEND");
   mat(g,"hero_phosphor",[1,.99,.92,1],0,.55);
   g.materials.hero_phosphor.emissiveFactor=[.93,.98,1];
   mat(g,"hero_ballast",[.028,.034,.039,1],.25,.52);
   bevelBox(g,"formed_housing","ceramic",0,.071,0,1.24,.093,.34,.019);
   bevelBox(g,"reflector","steel_brushed",0,-.006,0,1.19,.012,.31,.005);
   for(const z of [-.092,.092]){
     bead("fluorescent_tube","hero_phosphor",[-.55,-.050,z],[.55,-.050,z],.017,40);
     for(const x of [-.55,.55]){
       tube(g,"tube_connector","hero_ballast",[x,-.050,z],[x+Math.sign(x)*.032,-.050,z],.018,.018,24);
       bead("electrodes","chrome",[x,-.05,z-.01],[x,-.05,z+.01],.003,10);
     }
   }
   bevelBox(g,"black_ballast","hero_ballast",.54,.025,0,.095,.078,.29,.008);
   for(const z of [-.147,.147])bevelBox(g,"diffuser_retainer","steel_brushed",0,-.066,z,1.17,.016,.017,.004);
   bevelBox(g,"opal_polycarbonate_dome","hero_opal_dome",0,-.08,0,1.15,.037,.285,.015);
   for(const x of [-.571,.571])bevelBox(g,"endcap","ceramic",x,-.024,0,.053,.11,.33,.010);
 }else if(id==="door_office_solid01"){
   mat(g,"hero_seal",[.034,.038,.041,1],0,.88);
   for(const y of [.23,1.04,1.96]){
     tube(g,"precision_hinge","chrome",[.004,y-.080,.01],[.004,y+.080,.01],.011,.011,28);
     for(const yy of [y-.04,y+.04])
        bead("hinge_screws","steel_brushed",[.029,yy,.079],[.029,yy,.083],.005,12);
   }
   for(const x of [.116,.885])bevelBox(g,"solid_wood_stile","wood_light",x,1.065,.041,.095,1.94,.025,.009);
   for(const y of [.21,1.01,1.94])
      bevelBox(g,"machined_rails","wood_oiled",.50,y,.043,.84,.085,.028,.007);
   for(const y of [.59,1.49]){
      bevelBox(g,"recessed_panels","wood_light",.50,y,.066,.738,.63,.021,.012);
      bevelBox(g,"panel_edge","wood_oiled",.50,y-.284,.081,.74,.013,.012,.005);
   }
   for(const z of [-.080,.080]){
      tube(g,"lever_rosette","steel_brushed",[.84,1.032,z],[.84,1.032,z+.006],.044,.044,32);
      bead("lever_spindle","chrome",[.84,1.032,z],[.84,1.032,z+Math.sign(z)*.042],.015,24);
      bead("precision_lever","steel_brushed",[.84,1.032,z+Math.sign(z)*.042],
           [.666,1.032,z+Math.sign(z)*.064],.014,28);
      orb(g,"lever_end","chrome",[.666,1.032,z+Math.sign(z)*.064],[.016,.015,.015],16,10);
      tube(g,"lock_bezel","chrome",[.84,.943,z],[.84,.943,z+.005],.017,.017,24);
   }
   for(const x of [-.054,1.056])
      bevelBox(g,"jamb_architrave","wood_oiled",x,1.065,.086,.040,2.12,.030,.008);
   bevelBox(g,"top_architrave","wood_oiled",.50,2.124,.08,1.14,.033,.029,.008);
   for(const x of [.027,.97])bevelBox(g,"compression_seal","hero_seal",x,1.05,-.071,.009,2,.006,.003);
   bevelBox(g,"striker_plate","steel_brushed",.986,1.032,.063,.009,.095,.027,.003);
 }else if(id==="desk_office01"){
   mat(g,"hero_metal_black",[.038,.041,.045,1],.35,.62);
   mat(g,"hero_edge_veneer",[.52,.39,.28,1],0,.60);
   bevelBox(g,"precision_oak_laminate_top","wood_light",0,.746,0,1.53,.055,.78,.012);
   for(const z of [-.384,.384])
      bevelBox(g,"veneer_banding","hero_edge_veneer",0,.741,z,1.51,.037,.008,.003);
   for(const x of [-.755,.755])
      bevelBox(g,"end_grain","wood_oiled",x,.745,0,.007,.034,.765,.003);
   for(const x of [-.67,.67])for(const z of [-.303,.303]){
      tube(g,"tapered_steel_leg","hero_metal_black",[x,.042,z],[x,.704,z],.028,.021,32);
      tube(g,"leveling_foot","plastic_dark",[x,.013,z],[x,.044,z],.026,.026,24);
   }
   for(const z of [-.325,.325])bevelBox(g,"steel_apron","hero_metal_black",0,.639,z,1.36,.035,.030,.008);
   bevelBox(g,"two_drawer_carcase","hero_metal_black",.45,.467,0,.392,.420,.435,.010);
   for(const y of [.356,.526]){
      bevelBox(g,"oak_drawer_front","wood_oiled",.450,y,.232,.365,.151,.024,.008);
      bevelBox(g,"handle","steel_brushed",.450,y,.260,.139,.014,.021,.005);
      for(const x of [.393,.507])bead("drawer_screws","chrome",[x,y,.259],[x,y,.263],.005,12);
   }
   ring("cable_grommet","plastic_dark",[.48,.778,-.251],.037,.004,36);
   bevelBox(g,"underdesk_cable_tray","hero_metal_black",.04,.610,-.24,.69,.064,.13,.007);
   for(let i=0;i<17;i++){const x=-.28+i*.04;
      bead("basket_wires","steel_brushed",[x,.572,-.29],[x,.572,-.185],.0017,6);}
 }
}

function makeAsset(id){let g=mk();const palette=[
["plastic_dark",[.045,.052,.065,1],.02,.65],["plastic_mid",[.13,.16,.19,1],.0,.58],["steel_painted",[.20,.23,.26,1],.65,.4],["steel_brushed",[.52,.55,.59,1],.82,.28],["wood_oiled",[.28,.17,.095,1],0,.56],["wood_light",[.56,.39,.23,1],0,.58],["screen_black",[.012,.017,.025,1],.07,.19],["screen_neutral",[.08,.16,.19,1],0,.35],["fabric_dark",[.17,.20,.21,1],0,.95],["fabric_blue",[.085,.16,.20,1],0,.89],["ceramic",[.86,.89,.84,1],0,.17],["chrome",[.7,.72,.73,1],.96,.18],["soil",[.23,.17,.11,1],0,.96],["bark",[.18,.115,.083,1],0,.94],["bark_birch",[.63,.60,.54,1],0,.9],["needle_green",[.095,.23,.16,1],0,.85],["leaf_green",[.18,.32,.16,1],0,.84],["grass",[.2,.30,.16,1],0,.98],["stone",[.33,.32,.29,1],0,.93],["glass",[.55,.69,.78,.26],0,.10],["light_emissive",[1,.9,.72,1],0,.38]];
for(const [n,c,m,r] of palette)mat(g,n,c,m,r,n==="glass"?"BLEND":"OPAQUE");mat(g,"foliage_mask",[.44,.75,.36,1],0,.87,"MASK");mat(g,"needle_mask",[.22,.48,.28,1],0,.9,"MASK");
const bx=(n,m,...a)=>box(g,n,m,...a),tb=(n,m,...a)=>tube(g,n,m,...a),ob=(n,m,...a)=>orb(g,n,m,...a),rg=(n,m,...a)=>torus(g,n,m,...a);const rb=(n,m,...a)=>bevelBox(g,n,m,...a),cr=(n,m,...a)=>cableRoute(g,n,m,...a);
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

}else if(id==="desk_corner02"){
  rb("top","wood_light",-.42,.74,-.12,1.52,.05,.76,.025);
  rb("return_top","wood_light",.32,.74,-.60,.74,.05,1.70,.025);
  rb("modesty","plastic_mid",-.43,.52,-.47,1.38,.36,.018,.006);
  for(const [x,z] of [[-1.09,-.43],[.17,-.43],[.63,-1.30],[.63,.16]]){
    tb("frame","steel_painted",[x,.04,z],[x,.705,z],.031,.031,12);
    ob("feet","plastic_dark",[x,.026,z],[.058,.022,.058],12,7);
  }
  rb("cable_tray","steel_painted",-.31,.65,-.36,.71,.08,.16,.009);
  rb("grommet","plastic_dark",-.70,.768,-.36,.08,.007,.08,.008);
  socket(g,"socket_desktop",[-.42,.768,-.12],"surface");socket(g,"socket_return",[.32,.768,-.58],"surface");
  socket(g,"socket_power",[-.32,.62,-.34],"electric");socket(g,"socket_data",[-.45,.62,-.33],"data");
}else if(id==="desk_sitstand03"){
  rb("top","wood_oiled",0,.76,0,1.62,.052,.80,.023);
  for(const x of [-.68,.68]){
    rb("lower_leg","steel_painted",x,.28,-.05,.084,.49,.085,.012);
    rb("upper_leg","steel_brushed",x,.53,-.05,.067,.39,.07,.009);
    rb("foot","steel_painted",x,.045,-.05,.12,.036,.62,.015);
    rb("slider","plastic_dark",x,.54,-.05,.09,.035,.094,.006);
  }
  rb("crossbar","steel_painted",0,.61,-.27,1.38,.052,.06,.01);
  rb("control","plastic_dark",.56,.702,.27,.19,.034,.065,.009);
  rb("cable_tray","plastic_mid",0,.67,-.30,.86,.062,.12,.01);
  socket(g,"socket_desktop",[0,.79,0],"surface");
  socket(g,"socket_control",[.57,.70,.27],"control");
  socket(g,"socket_power",[.43,.66,-.31],"electric");
  socket(g,"socket_height_axis",[0,.42,0],"actuator");
}else if(id==="chair_task02"){
  for(let i=0;i<5;i++){
    const a=i*6.283185307/5,x=Math.cos(a)*.33,z=Math.sin(a)*.33;
    tb("star_base","steel_brushed",[0,.115,0],[x,.085,z],.043,.030,10);
    ob("castors","plastic_dark",[x,.051,z],[.042,.047,.043],12,7);
  }
  tb("gas_lift","chrome",[0,.1,0],[0,.49,0],.047,.042,16);
  ob("seat","fabric_blue",[0,.53,.045],[.295,.065,.265],26,13);
  rb("undershell","plastic_dark",0,.465,.024,.57,.04,.49,.02);
  for(let i=0;i<7;i++){
    const y=.64+i*.085,w=.49-.035*i;
    rb("back_shell","plastic_dark",0,y,-.23,w,.082,.06,.012);
    rb("back_mesh","fabric_dark",0,y,-.19,w-.03,.058,.018,.007);
  }
  for(const x of [-.29,.29]){
    tb("arm_pillar","steel_painted",[x,.48,-.02],[x,.73,-.02],.019,.016,10);
    rb("arm_pad","fabric_dark",x,.748,.03,.075,.038,.31,.013);
  }
  socket(g,"socket_seat",[0,.59,.05],"seat");
}else if(id==="monitor_32in02"){
  rb("back","plastic_dark",0,.45,0,.755,.457,.044,.013);
  rb("bezel","plastic_mid",0,.45,.025,.731,.425,.009,.007);
  bx("panel","screen_black",0,.45,.031,.709,.393,.003);
  for(let i=0;i<23;i++)bx("rear_vents","plastic_mid",-.27+i*.024,.63,-.025,.013,.005,.003);
  tb("support","steel_painted",[0,.22,-.035],[0,.075,-.035],.025,.029,12);
  rb("base","steel_brushed",0,.025,-.02,.28,.033,.23,.015);
  rb("logo","steel_brushed",0,.241,.031,.033,.006,.003,.001);
  socket(g,"socket_power",[.10,.28,-.03],"electric");socket(g,"socket_display",[-.10,.28,-.03],"data");
  socket(g,"socket_vesa",[0,.45,-.03],"mount");socket(g,"socket_desktop",[0,0,0],"surface");
}else if(id==="printer_office01"){
  rb("housing","plastic_mid",0,.18,0,.44,.27,.36,.027);
  rb("top_scanner","plastic_dark",0,.332,-.017,.44,.041,.36,.016);
  rb("scanner_glass","screen_black",0,.354,-.015,.35,.005,.26,.007);
  rb("output_tray","plastic_dark",0,.095,.228,.34,.019,.22,.01);
  rb("paper","ceramic",0,.115,.16,.21,.006,.19,.001);
  for(let i=0;i<6;i++)bx("vent","plastic_dark",-.16+i*.026,.234,-.183,.014,.06,.003);
  rb("control_panel","plastic_dark",.14,.294,.187,.12,.019,.05,.006);
  socket(g,"socket_power",[.15,.08,-.19],"electric");socket(g,"socket_data",[-.09,.08,-.19],"data");
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="phone_office01"){
  rb("base","plastic_dark",0,.039,0,.255,.07,.17,.02);
  rb("receiver","plastic_mid",0,.12,-.065,.215,.043,.053,.018);
  for(let k=0;k<12;k++)rb("keys","plastic_mid",-.083+(k%4)*.055,.078,-.023+Math.floor(k/4)*.044,.04,.014,.032,.006);
  rb("display","screen_neutral",0,.082,.063,.09,.006,.026,.002);
  cr("coiled_cord","plastic_dark",[[.1,.118,-.065],[.17,.115,-.06],[.18,.08,-.03],[.16,.05,.035]],.004);
  socket(g,"socket_data",[0,.02,-.089],"data");socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="paper_tray01"){
  rb("tray","plastic_mid",0,.023,0,.32,.041,.243,.013);
  for(const x of [-.153,.153])rb("sides","steel_painted",x,.065,0,.014,.10,.24,.004);
  rb("rear","plastic_mid",0,.058,-.118,.30,.09,.016,.005);
  for(let i=0;i<13;i++)rb("sheets","ceramic",0,.053+i*.003,.008,.252,.003,.196,.0005);
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="binder_row01"){
  for(let i=0;i<8;i++){
    const x=-.20+i*.057,z=(i%3)*.006,w=.055;
    rb("binding","fabric_blue",x,.165,z,w,.32,.24,.007);
    rb("label","ceramic",x,.18,.126+z,.039,.09,.003,.001);
    tb("ring_eyelet","steel_brushed",[x,.055,.13+z],[x,.057,.13+z],.006,.006,10);
  }
  socket(g,"socket_shelf",[0,0,0],"surface");
}else if(id==="water_bottle01"){
  tb("body","glass",[0,.035,0],[0,.235,0],.043,.04,24);
  rb("sleeve","plastic_mid",0,.125,0,.075,.067,.075,.009);
  tb("neck","chrome",[0,.23,0],[0,.265,0],.019,.019,16);
  tb("cap","plastic_dark",[0,.26,0],[0,.286,0],.025,.025,16);
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="notebook01"){
  rb("cover","wood_oiled",0,.014,0,.225,.025,.158,.005);
  for(let i=0;i<8;i++)bx("pages","ceramic",.004,.028+i*.0009,0,.207,.0008,.15);
  for(let i=0;i<14;i++)tb("spiral","steel_brushed",[-.107,.027,-.068+i*.01],[-.107,.032,-.068+i*.01],.004,.004,6);
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="power_strip01"){
  rb("body","ceramic",0,.031,0,.38,.056,.075,.012);
  for(let i=0;i<5;i++){
    const x=-.146+i*.073;
    rb("outlet","plastic_mid",x,.063,0,.054,.002,.049,.006);
    for(const dx of [-.012,.012])bx("contact","plastic_dark",x+dx,.065,0,.004,.002,.016);
  }
  rb("switch","plastic_dark",.168,.063,0,.023,.006,.026,.002);
  cr("cable","plastic_dark",[[.19,.02,0],[.29,.013,0],[.32,.010,.11],[.35,.010,.2]],.009);
  socket(g,"socket_power_in",[.35,.01,.20],"electric_in");socket(g,"socket_power_out",[0,.06,0],"electric_out");
}else if(id==="signage_exit01"){
  rb("housing","steel_painted",0,.13,0,.52,.26,.056,.009);
  rb("lightbox","light_emissive",0,.132,.03,.49,.23,.006,.004);
  for(const x of [-.11,-.052,.012,.069])bx("glyphs","plastic_dark",x,.154,.036,.022,.082,.002);
  tb("mount","steel_brushed",[0,.255,0],[0,.31,0],.021,.021,12);
  socket(g,"socket_ceiling",[0,.31,0],"ceiling");socket(g,"socket_power",[0,.29,0],"electric");
}else if(id==="floor_drain01"){
  rb("rim","steel_brushed",0,.007,0,.36,.016,.32,.01);
  rb("recess","plastic_dark",0,.019,0,.30,.010,.26,.003);
  for(let i=0;i<9;i++)rb("grating","steel_brushed",-.13+i*.032,.026,0,.014,.013,.27,.004);
  socket(g,"socket_drain",[0,-.02,0],"drain");socket(g,"socket_floor",[0,0,0],"host");
}else if(id==="stump01"){
  tb("stump","bark",[0,0,0],[0,.48,0],.31,.28,22);
  tb("cut_surface","wood_light",[0,.48,0],[0,.49,0],.27,.27,24);
  for(let i=1;i<=5;i++)rg("growth_rings","wood_oiled",[0,.495,0],.043*i,.003,24,6);
  socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="sapling01"){
  tb("stem","bark",[0,0,0],[0,2.06,0],.051,.015,12);
  for(let i=0;i<16;i++){
    let a=i*2.3999632,y=.43+(i%7)*.22,r=.15+(i%4)*.047;
    tb("twigs","bark",[0,y-.14,0],[r*Math.cos(a),y+.04,r*Math.sin(a)],.009,.002,6);
    leaf(g,"foliage","foliage_mask",[r*Math.cos(a),y+.10,r*Math.sin(a)],[.23,.34],a);
  }
  socket(g,"socket_root",[0,0,0],"terrain");socket(g,"socket_wind",[0,1.4,0],"wind_pivot");
}else if(/^tree_(spruce|fir|birch|maple)02$/.test(id)){
  const master=makeAsset(id.replace("02","01"));
  const variety=id.includes("birch")?1.13:id.includes("fir")?.93:id.includes("maple")?1.08:.86;
  for(const group of Object.values(master.groups)){
    for(let i=0;i<group.v.length;i+=8){
      const x=group.v[i],y=group.v[i+1],z=group.v[i+2];
      const spread=(.79+.20*Math.sin(y*1.9+.43))*variety;
      const bend=.07*Math.sin(y*.47);
      group.v[i]=x*spread+bend*y*.09;group.v[i+1]=y*variety;group.v[i+2]=z*(1.12-.11*Math.sin(y*1.3));
      const nx=group.v[i+3]/spread,ny=group.v[i+4]/variety,nz=group.v[i+5]/1.1,L=Math.hypot(nx,ny,nz)||1;
      group.v[i+3]=nx/L;group.v[i+4]=ny/L;group.v[i+5]=nz/L;
    }
  }
  for(const socket of Object.values(master.sockets))socket.translation[1]*=variety;
  return master;
}else if(id==="shrub02"||id==="shrub03"){
  for(let i=0;i<24;i++){
    const a=i*2.399963+(.5*(id==="shrub03")),r=(id==="shrub03"?.64:.39)*(0.6+(i%5)*.10),h=(id==="shrub03"?.82:1.27)*(0.6+(i%6)*.11);
    tb("twigs","bark",[0,.02,0],[r*Math.cos(a),h,r*Math.sin(a)],.018,.004,7);
    for(let j=0;j<4;j++)leaf(g,"foliage","foliage_mask",[r*Math.cos(a)*(.68+j*.1),h*(.65+j*.10),r*Math.sin(a)*(.68+j*.1)],[.29,.33],a+j*.35);
  }
  socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="grass_tuft01"){
  for(let i=0;i<43;i++){
    const a=i*2.399963,r=.022+Math.sqrt((i+.5)/43)*.115,h=.22+(i%7)*.056;
    const x=Math.cos(a)*r,z=Math.sin(a)*r;
    tb("stems","grass",[x,.008,z],[x*1.5,h,z*1.5],.007,.0015,5);
    leaf(g,"blades","foliage_mask",[x*1.5,h*.57,z*1.5],[.045,h*.92],a);
  }
  socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="moss_patch01"){
  for(let i=0;i<35;i++){
    const a=i*2.399963,r=Math.sqrt((i+.5)/35)*.46,x=Math.cos(a)*r,z=Math.sin(a)*r;
    ob("moss","grass",[x,.024,z],[.09,.035,.09],8,5);
  }
  socket(g,"socket_ground",[0,0,0],"terrain");
}else if(id==="toilet_paper01"){
  tb("spool","ceramic",[0,0,-.02],[0,0,.10],.068,.068,24);
  tb("core","wood_light",[0,0,.099],[0,0,.107],.021,.021,20);
  rb("holder","chrome",0,.092,-.01,.17,.024,.075,.011);
  tb("hinge","chrome",[-.07,.07,-.055],[.07,.07,-.055],.012,.012,14);
  socket(g,"socket_wall",[0,.105,-.07],"wall");
}else if(id==="server_rack_cluster01"){
  // 3 x 42U rack-mount compute cluster with credible stamped steel,
  // 19-inch rails, air grilles, DIN patch panels, blue power LEDs and cables.
  mat(g,"rack_coating",[.055,.066,.078,1],.64,.45);
  mat(g,"rack_front",[.105,.124,.136,1],.62,.39);
  mat(g,"rack_black",[.017,.021,.025,1],.15,.63);
  mat(g,"rack_blue_led",[.015,.16,.92,1],0,.38);
  mat(g,"rack_green_led",[.06,.73,.23,1],0,.43);
  mat(g,"rack_port",[.22,.26,.28,1],.34,.55);
  g.materials.rack_blue_led.emissiveFactor=[.02,.31,1.0];
  g.materials.rack_green_led.emissiveFactor=[.08,.7,.27];
  const rackWidth=.65;
  for(let rack=0;rack<1;rack++){
    const cx=0;
    for(const side of [-1,1]){
      rb("rack_vertical_chassis","rack_coating",cx+side*.31,1.073,0,.035,2.13,.76,.008);
      rb("rack_front_rail","steel_brushed",cx+side*.263,1.06,.343,.018,2.01,.026,.004);
      for(let hole=0;hole<22;hole++){
        const y=.10+hole*.046;
        tb("square_cage_nuts","rack_port",[cx+side*.263,y,.36],[cx+side*.263,y+.006,.36],.005,.005,8);
      }
    }
    for(const y of [.022,2.123]){
      rb("rack_top_bottom","rack_coating",cx,y,0,.665,.044,.78,.006);
    }
    for(const depth of [-.345,.345]){
      for(const y of [.09,2.07]){
        rb("back_frame","rack_front",cx,y,depth,.60,.040,.025,.006);
      }
    }
    // Rack-mounted 1U/2U chassis, distinct handles and 19-inch rail screws.
    for(let slot=0;slot<12;slot++){
      const y=.16+slot*.153;
      const dual=(slot%9===0),h=dual?.091:.050;
      if(slot%5===0){
        rb("patch_panel_face","rack_black",cx,y,.362,.545,h,.027,.004);
        for(let port=0;port<8;port++){
          const x=cx-.246+port*.037;
          rb("rj45_ethernet_jacks","rack_port",x,y,.381,.024,.019,.006,.002);
          rb("rj45_socket_void","rack_black",x,y,.385,.016,.010,.003,.0006);
        }
      }else{
        rb("compute_node_face",slot%4===0?"rack_front":"rack_black",cx,y,.366,.55,h,.032,.004);
        for(const side of [-1,1]){
          rb("node_handles","steel_brushed",cx+side*.225,y,.388,.019,h*.63,.018,.003);
          tb("security_screws","chrome",[cx+side*.270,y,.387],[cx+side*.270,y,.391],.004,.004,10);
        }
        for(let vent=0;vent<5;vent++){
          rb("mesh_vents","rack_front",cx-.165+vent*.024,y,.389,.009,.025,.003,.0007);
        }
        const led=slot%6===0?"rack_green_led":"rack_blue_led";
        rb("rack_status_led",led,cx+.179,y,.393,.010,.008,.006,.0016);
        rb("rack_activity_led",led,cx+.194,y,.393,.006,.006,.006,.001);
      }
    }
    // Visible power distribution / cross-rack network spine.
    rb("pdu_rear","rack_black",cx+.27,1.075,-.31,.041,1.95,.035,.006);
    for(let outlet=0;outlet<8;outlet++){
      const y=.19+outlet*.21;
      rb("rear_sockets","rack_port",cx+.27,y,-.289,.025,.021,.007,.002);
      cr("braided_fiber","plastic_mid",
        [[cx+.27,y,-.28],[cx+.31,y+.03,-.18],[cx+.22,y+.06,-.03],[cx+.18,y+.08,.15]],.0035);
    }
    // Transparent enclosure with hinges and perforated vents at top.
    for(const side of [-1,1]){
      rb("front_glass_door_hinge","chrome",cx+side*.308,1.01,.379,.018,.115,.026,.004);
    }
    rb("door_glass","glass",cx,1.08,.395,.575,1.96,.009,.006);
    rb("door_steel_latch","steel_brushed",cx+.258,1.06,.412,.020,.14,.021,.004);
    for(let vent=0;vent<5;vent++)for(let side of [-1,1])
      rb("top_ventilation","rack_black",cx+side*.23,.2+vent*.22,-.380,.058,.017,.007,.002);
  }
  socket(g,"socket_floor",[0,0,0],"floor");
  socket(g,"socket_power",[.95,.18,-.34],"electric");
  socket(g,"socket_data",[-.93,1.9,-.35],"network");
}else if(id==="stapler_office01"){
  mat(g,"stapler_ink",[.035,.043,.055,1],.12,.51);
  rb("lower_stapler_body","stapler_ink",0,.030,0,.142,.041,.051,.013);
  rb("upper_lid","plastic_dark",0,.068,-.006,.145,.043,.046,.010);
  rb("stapler_magazine","steel_brushed",0,.040,.009,.102,.010,.029,.004);
  tb("hinge_pin","chrome",[-.035,.047,-.030],[.035,.047,-.030],.007,.007,24);
  rb("anvil_plate","chrome",0,.013,.046,.037,.005,.025,.002);
  for(const x of [-.041,.041])tb("screw","steel_brushed",[x,.041,-.033],[x,.044,-.033],.003,.003,12);
  socket(g,"socket_desktop",[0,0,0],"surface");
}else if(id==="network_bundle01"){
  // Physically separate orange/blue Ethernet, flexible conduit and labelled
  // stainless saddles, repeated only along existing technical routes.
  mat(g,"ethernet_blue",[.022,.19,.69,1],0,.74);
  mat(g,"ethernet_orange",[.82,.25,.035,1],0,.71);
  mat(g,"insulated_cable",[.06,.07,.078,1],0,.76);
  mat(g,"galvanized_metal",[.41,.45,.48,1],.77,.54);
  for(let i=0;i<8;i++){
    const x=(i-3.5)*.017;
    const color=i%3===0?"ethernet_blue":i%3===1?"ethernet_orange":"insulated_cable";
    cr("flexible_cable_sheaths",color,[[x,0,0],[x+.004,.09,.025],
          [x-.007,.20,.045],[x+.004,.32,.041],[x,.48,.014],
          [x+.008,.64,-.015],[x,.82,0],[x,.99,.035]],.0045);
  }
  for(const y of [.04,.30,.56,.89]){
    rb("galvanized_cable_ties","galvanized_metal",0,y,.036,.156,.016,.018,.004);
    for(const x of [-.066,.066])
      tb("fixings","chrome",[x,y,.046],[x,y,.049],.004,.004,10);
  }
  socket(g,"socket_start",[0,0,0],"network");
  socket(g,"socket_end",[0,.99,.035],"network");
}else if(id==="service_pipe01"){
  tb("pipe","chrome",[0,0,0],[0,1.15,0],.043,.043,18);
  tb("elbow","chrome",[0,1.15,0],[.32,1.15,0],.043,.043,18);
  rg("flange","steel_brushed",[0,.03,0],.061,.015,20,6);
  for(const h of [.25,.85])rb("clamp","steel_painted",0,h,-.058,.125,.04,.028,.012);
  socket(g,"socket_water_in",[0,0,0],"fluid_in");socket(g,"socket_water_out",[.32,1.15,0],"fluid_out");

}else if(id==="tree_maple_hero01"){
  mat(g,"foliage_maple_mask",[.19,.34,.15,1],0,.78,"MASK");
  tb("trunk","bark",[0,0,0],[0,10.5,0],.43,.09,24);
  for(let i=0;i<16;i++){
    let a=i*2.3999632,y=2.0+i*.50,L=2.4+.7*Math.sin(i*.8),dx=Math.cos(a),dz=Math.sin(a);
    const start=[0,y,0],end=[dx*L,y+.8,dz*L];
    tb("primary_branches","bark",start,end,.105,.035,14);
    for(let j=0;j<7;j++){
      const t=.26+j*.105,origin=[dx*L*t,y+.8*t,dz*L*t],turn=a+(j%2?1:-1)*.62,
      len=.83+.20*Math.sin(j+i),endpoint=[origin[0]+len*Math.cos(turn),origin[1]+.36,origin[2]+len*Math.sin(turn)];
      tb("secondary_branches","bark",origin,endpoint,.034,.012,10);
      for(let k=0;k<5;k++){
        let phase=turn+(k-1.5)*.46,base=[origin[0]+(endpoint[0]-origin[0])*(.34+k*.15),origin[1]+.36*(.34+k*.15),origin[2]+(endpoint[2]-origin[2])*(.34+k*.15)],
        endTip=[base[0]+Math.cos(phase)*(.34+.10*k),base[1]+.2+.08*k,base[2]+Math.sin(phase)*(.34+.10*k)];
        tb("twigs","bark",base,endTip,.012,.0028,7);
        for(let q=0;q<6;q++){
          let ang=phase+q*1.11,h=.06*Math.sin(q+i),
          center=[endTip[0]+Math.cos(ang)*(.08+.045*(q%3)),endTip[1]+h,endTip[2]+Math.sin(ang)*(.08+.045*(q%3))];
          leaf(g,"foliage","foliage_maple_mask",center,[.21,.27],ang);
        }
      }
    }
  }
  for(let i=0;i<7;i++){let a=i*6.283185/7;tb("roots","bark",[0,.09,0],[Math.cos(a)*.85,.035,Math.sin(a)*.85],.13,.025,9)}
  socket(g,"socket_root",[0,0,0],"terrain");socket(g,"socket_wind",[0,6.2,0],"wind_pivot");socket(g,"col_trunk",[0,4.5,0],"collision");
}else if(id==="tree_fir_hero01"){
  tb("trunk","bark",[0,0,0],[0,12.4,0],.34,.028,24);
  for(let i=0;i<25;i++){
    const y=.86+i*.43,r=2.78*(1-y/13)+.12;
    for(let j=0;j<8;j++){
      let a=j*6.2831853/8+i*.8,dx=Math.cos(a),dz=Math.sin(a);
      tb("primary_branches","bark",[0,y,0],[dx*r,y-.12,dz*r],.045,.007,9);
      for(let k=0;k<3;k++){
        const t=.34+k*.21,orig=[dx*r*t,y-.12*t,dz*r*t],off=(k-1)*.42;
        const tip=[orig[0]+dx*.47+Math.cos(a+1.57)*off,orig[1]+.13,orig[2]+dz*.47+Math.sin(a+1.57)*off];
        tb("twigs","bark",orig,tip,.012,.0018,6);
        for(let q=0;q<5;q++){
          let f=.22+q*.15,c=[orig[0]+(tip[0]-orig[0])*f,orig[1]+(tip[1]-orig[1])*f,orig[2]+(tip[2]-orig[2])*f];
          leaf(g,"needle_clusters","needle_mask",c,[.18,.34],a+q*.4);
          leaf(g,"needle_clusters","needle_mask",[c[0]+.05,c[1]+.04,c[2]-.04],[.15,.28],a+.9+q*.3);
        }
      }
    }
  }
  socket(g,"socket_root",[0,0,0],"terrain");socket(g,"socket_wind",[0,7.6,0],"wind_pivot");socket(g,"col_trunk",[0,5.0,0],"collision");
}else if(id==="forest_patch_dense01"){
  terrainMesh(g,"terrain","soil",8,72);
  rb("path_surface","stone",0,.011,0,1.58,.022,7.95,.01);
  for(let i=0;i<200;i++){
    const angle=i*2.399963,u=hashTile(i,7,12),r=Math.sqrt((i+.5)/200)*3.72,
      x=Math.cos(angle)*r,z=Math.sin(angle)*r;
    if(Math.abs(x)<.87)continue;
    let rad=.045+.11*u;
    ob("stones","stone",[x,.024,z],[rad,.024+rad*.3,rad*.9],9,5);
  }
  for(let i=0;i<340;i++){
    let x=hashTile(i,16,11)*7.4-3.7,z=hashTile(i,23,21)*7.4-3.7;
    if(Math.abs(x)<.87)continue;
    let a=hashTile(i,6,12)*6.2831853,h=.075+hashTile(i,8,13)*.19,
    end=[x+.04*Math.cos(a),h,z+.04*Math.sin(a)];
    tb("grasses","grass",[x,.02,z],end,.009,.0025,5);
    if(i%3===0)leaf(g,"leaf_litter","foliage_mask",[x,.035,z],[.09,.12],a);
  }
  for(let i=0;i<48;i++){
    let x=hashTile(i,39,51)*7.0-3.5,z=hashTile(i,43,62)*7.0-3.5;
    if(Math.abs(x)<.93)continue;
    let a=i*2.399963;
    tb("fallen_twigs","bark",[x,.035,z],[x+.19*Math.cos(a),.044,z+.19*Math.sin(a)],.016,.003,6);
  }
  socket(g,"socket_neighbor_n",[0,0,-4],"tile_edge");
  socket(g,"socket_neighbor_s",[0,0,4],"tile_edge");
  socket(g,"socket_neighbor_e",[4,0,0],"tile_edge");
  socket(g,"socket_neighbor_w",[-4,0,0],"tile_edge");
  socket(g,"socket_terrain",[0,0,0],"terrain");

}else if(id==="coffee_mug02"){
  // Le café a un ménisque sous le bord, une anse évidée et un pied distinct.
  tb("bottom","ceramic",[0,.006,0],[0,.021,0],.047,.052,20);
  const N=32;
  for(let i=0;i<N;i++){
    const a=2*Math.PI*i/N,b=2*Math.PI*(i+1)/N,
    wall=(radius,a,y)=>[Math.cos(a)*radius,y,Math.sin(a)*radius];
    quad(g,"outside","ceramic",wall(.047,b,.016),wall(.047,a,.016),wall(.050,a,.103),wall(.050,b,.103));
    quad(g,"inside","ceramic",wall(.039,a,.099),wall(.039,b,.099),wall(.037,b,.029),wall(.037,a,.029));
    quad(g,"lip","ceramic",wall(.050,a,.102),wall(.039,a,.100),wall(.039,b,.100),wall(.050,b,.102));
  }
  tb("coffee","wood_oiled",[0,.087,0],[0,.088,0],.038,.038,24);
  for(let j=0;j<18;j++){
    let a=-1.27+2.54*j/17,b=-1.27+2.54*(j+1)/17,
        pt=t=>[.048+.038*Math.cos(t),.062+.038*Math.sin(t),0];
    tb("handle","ceramic",pt(a),pt(b),.0085,.0085,8,j===0||j===17);
  }
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="desk_lamp_arch02"){
  rb("base","steel_painted",0,.020,0,.15,.037,.16,.018);
  tb("pivot","chrome",[0,.043,0],[0,.075,0],.027,.024,18);
  tb("lower_arm","steel_brushed",[0,.072,0],[-.075,.36,-.055],.016,.014,14);
  ob("joint","chrome",[-.075,.36,-.055],[.029,.028,.029],12,8);
  tb("upper_arm","steel_brushed",[-.075,.36,-.055],[-.21,.53,-.13],.013,.012,14);
  ob("joint","chrome",[-.21,.53,-.13],[.023,.023,.023],12,8);
  tb("shade","plastic_dark",[-.21,.53,-.13],[-.245,.48,-.14],.067,.105,22);
  tb("reflector","ceramic",[-.245,.478,-.14],[-.245,.475,-.14],.082,.082,22);
  tb("diffuser","light_emissive",[-.245,.474,-.14],[-.245,.470,-.14],.074,.074,22);
  cr("cable","plastic_dark",[[0,.028,-.06],[.035,.012,-.10],[.105,.012,-.15]],.004);
  rb("touch_switch","plastic_mid",.041,.041,.038,.026,.004,.025,.003);
  socket(g,"socket_surface",[0,0,0],"surface");
  socket(g,"socket_light",[-.245,.45,-.14],"light_down");
  socket(g,"socket_power",[.105,.012,-.15],"electric");
}else if(id==="sticky_notes01"){
  rb("paper_stack","ceramic",0,.008,0,.123,.015,.121,.004);
  for(let i=0;i<4;i++)rb("paper","ceramic",i%3*.001,.016+i*.00085,i%4*.001,.124,.0012,.122,.0004);
  rb("colored_tab","wood_light",-.049,.021,.049,.020,.0008,.012,.0005);
  for(let i=0;i<4;i++)rb("ink_lines","plastic_mid",-.036,.019,i*.020-.039,.06,.0006,.0015,.0004);
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="desk_organizer02"){
  rb("base","wood_oiled",0,.014,0,.19,.024,.13,.010);
  rb("tray","steel_painted",-.033,.037,.013,.104,.021,.096,.008);
  rb("note_stack","ceramic",-.033,.052,.013,.082,.007,.074,.004);
  tb("pen_cup","steel_brushed",[.064,.022,-.012],[.064,.094,-.012],.034,.034,16,false);
  for(let i=0;i<9;i++){
    const a=i*2.39996,x=.064+.018*Math.sin(a),z=-.012+.018*Math.cos(a);
    tb("pens","plastic_dark",[x,.033,z],[x,.137+(i%4)*.015,z],.0035,.0035,6);
    tb("nibs","chrome",[x,.143+(i%4)*.015,z],[x,.149+(i%4)*.015,z],.003,.002,6);
  }
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="laptop_open01"){
  rb("base","steel_brushed",0,.014,.02,.347,.022,.239,.011);
  rb("keyboard_bed","plastic_dark",0,.027,.003,.31,.005,.175,.004);
  for(let r=0;r<5;r++)for(let i=0;i<12;i++)rb("keys","plastic_mid",-.148+i*.0269,.031,-.067+r*.027,.021,.005,.019,.003);
  rb("touchpad","steel_painted",0,.028,.090,.088,.003,.044,.003);
  const panel=[[-.17,.03,-.120],[.17,.03,-.120],[.17,.263,-.214],[-.17,.263,-.214]];
  const screen=[[-.156,.053,-.129],[.156,.053,-.129],[.156,.253,-.211],[-.156,.253,-.211]];
  quad(g,"display_back","plastic_dark",panel[0],panel[1],panel[2],panel[3]);
  quad(g,"display","screen_black",screen[0],screen[1],screen[2],screen[3]);
  tb("hinge","chrome",[-.135,.034,-.113],[.135,.034,-.113],.010,.010,16);
  rb("camera","plastic_mid",0,.264,-.214,.012,.007,.006,.002);
  socket(g,"socket_surface",[0,0,0],"surface");
  socket(g,"socket_power",[.175,.019,-.06],"electric");socket(g,"socket_data",[-.175,.019,-.06],"data");
}else if(id==="headset_stand01"){
  rb("base","wood_oiled",0,.018,0,.19,.030,.18,.012);
  tb("shaft","steel_brushed",[0,.026,0],[0,.255,0],.011,.012,14);
  rb("crown","plastic_dark",0,.259,0,.16,.026,.044,.012);
  for(const s of [-1,1]){
    tb("earcups","fabric_dark",[s*.075,.105,.012],[s*.075,.170,.012],.036,.038,20);
    tb("wire","plastic_dark",[s*.075,.168,.012],[s*.058,.239,.012],.012,.010,14);
  }
  rg("headband","plastic_dark",[0,.18,.011],.079,.011,24,8);
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="desk_plant_mini01"){
  tb("pot","ceramic",[0,.005,0],[0,.102,0],.065,.081,18);
  tb("soil","soil",[0,.100,0],[0,.104,0],.079,.079,18);
  for(let i=0;i<19;i++){
    const a=i*2.399963,y=.105+(i%4)*.036,r=.017+(i%6)*.009;
    ob("leaves","needle_green",[r*Math.cos(a),y,r*Math.sin(a)],[.014,.057,.019],10,6);
  }
  socket(g,"socket_surface",[0,0,0],"surface");
}else if(id==="sofa_lounge01"){
  rb("frame","wood_oiled",0,.29,0,1.84,.22,.82,.033);
  for(const x of [-.77,.77]){
    rb("arm_frame","wood_oiled",x,.56,-.03,.18,.44,.80,.026);
    rb("arm_fabric","fabric_dark",x,.77,-.04,.168,.055,.78,.020);
  }
  rb("back_frame","wood_oiled",0,.69,-.33,1.75,.62,.18,.026);
  for(let i=0;i<3;i++){
    const x=(i-1)*.52;
    rb("seat_cushions","fabric_blue",x,.468,.073,.507,.20,.65,.035);
    rb("back_cushions","fabric_dark",x,.705,-.216,.50,.49,.16,.038);
    rb("piping","plastic_dark",x,.57,.365,.47,.008,.008,.003);
  }
  for(const x of [-.77,.77])for(const z of [-.28,.28])rb("feet","steel_painted",x,.082,z,.073,.14,.075,.010);
  socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="coffee_table_round01"){
  tb("top","wood_oiled",[0,.43,0],[0,.478,0],.49,.49,48);
  rg("edge","steel_brushed",[0,.45,0],.487,.012,44,8);
  tb("pedestal","steel_painted",[0,.055,0],[0,.435,0],.070,.070,24);
  tb("base","steel_brushed",[0,.018,0],[0,.05,0],.275,.275,32);
  socket(g,"socket_top",[0,.482,0],"surface");
  socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="floor_lamp_arc01"){
  tb("base","steel_painted",[0,.006,0],[0,.046,0],.205,.205,32);
  let points=[];
  for(let i=0;i<15;i++){
    let t=i/14,a=Math.PI*.82*t;
    points.push([1.18*(1-Math.cos(a))*.5, .09+1.55*Math.sin(a/1.82), -.09+.08*Math.sin(t*6.28)]);
  }
  cr("curved_stem","steel_brushed",points,.020);
  const top=points[points.length-1];
  tb("shade","plastic_dark",top,[top[0],top[1]-.175,top[2]],.076,.215,28);
  tb("diffuser","light_emissive",[top[0],top[1]-.181,top[2]],[top[0],top[1]-.183,top[2]],.187,.187,28);
  cr("power_cable","plastic_dark",[[0,.021,-.13],[.19,.011,-.18],[.34,.010,-.2]],.005);
  socket(g,"socket_floor",[0,0,0],"floor");socket(g,"socket_light",[top[0],top[1]-.19,top[2]],"light_down");
  socket(g,"socket_power",[.34,.01,-.2],"electric");
}else if(id==="acoustic_panel01"){
  rb("base","fabric_dark",0,.65,0,1.15,1.28,.049,.017);
  for(let i=0;i<8;i++)rb("felt_ribs","fabric_blue",-.476+i*.136,.65,.029,.092,1.19,.025,.012);
  rb("mount","steel_painted",0,.65,-.035,1.05,1.17,.012,.006);
  socket(g,"socket_wall",[0,.65,-.044],"wall");
}else if(id==="conference_projector01"){
  rb("housing","plastic_mid",0,.11,0,.41,.18,.31,.025);
  tb("lens_tube","plastic_dark",[0,.095,.151],[0,.095,.182],.064,.064,24);
  tb("lens","screen_black",[0,.095,.182],[0,.095,.185],.047,.047,24);
  for(const x of [-.17,.17]){
    for(let i=0;i<9;i++)rb("vents","plastic_dark",x,.105,-.125+i*.027,.006,.069,.012,.002);
  }
  tb("ceiling_mount","steel_painted",[0,.20,0],[0,.37,0],.025,.025,12);
  rb("flange","steel_brushed",0,.374,0,.16,.021,.16,.014);
  socket(g,"socket_ceiling",[0,.39,0],"ceiling");
  socket(g,"socket_power",[-.18,.16,-.09],"electric");
  socket(g,"socket_data",[.18,.16,-.09],"data");
}else if(id==="conference_speaker01"){
  rb("chassis","plastic_dark",0,.037,0,.265,.059,.162,.022);
  rb("grille","steel_painted",0,.071,0,.207,.012,.118,.01);
  for(let i=0;i<9;i++)for(let j=0;j<5;j++){
    rb("perforations","plastic_mid",-.091+i*.0227,.078,-.049+j*.025,.006,.002,.010,.001);
  }
  rb("status","light_emissive",0,.080,.06,.017,.004,.005,.001);
  socket(g,"socket_surface",[0,0,0],"surface");socket(g,"socket_data",[0,.017,-.08],"data");
}else if(id==="coat_rack01"){
  tb("pedestal","steel_brushed",[0,.016,0],[0,.038,0],.210,.21,26);
  tb("shaft","wood_oiled",[0,.030,0],[0,1.79,0],.044,.035,18);
  for(let i=0;i<7;i++){
    let a=i*6.28318/7,y=1.6+(i%3)*.077;
    const start=[0,y,0],end=[.30*Math.cos(a),y+.115,.30*Math.sin(a)];
    tb("hooks","chrome",start,end,.015,.014,12);
    ob("tips","steel_brushed",end,[.026,.026,.026],10,7);
  }
  socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="water_cooler01"){
  rb("housing","plastic_mid",0,.47,0,.41,.87,.42,.030);
  rb("front","ceramic",0,.57,.212,.36,.52,.019,.013);
  tb("jug","glass",[0,.92,-.024],[0,1.34,-.024],.135,.157,24);
  tb("lid","plastic_mid",[0,1.34,-.024],[0,1.36,-.024],.15,.14,22);
  for(const x of [-.078,.078])tb("tap","chrome",[x,.74,.221],[x,.66,.221],.016,.016,12);
  rb("drip_tray","plastic_dark",0,.61,.264,.25,.025,.085,.010);
  rb("drip_grid","steel_brushed",0,.632,.27,.22,.004,.055,.002);
  socket(g,"socket_floor",[0,0,0],"floor");socket(g,"socket_power",[-.17,.075,-.18],"electric");
}else if(id==="cabinet_glass02"){
  rb("case","wood_oiled",0,.95,0,1.05,1.87,.43,.029);
  for(let i=0;i<4;i++)rb("shelves","wood_light",0,.37+i*.42,.03,.96,.027,.39,.008);
  for(const x of [-.246,.246]){
    rb("door_frame","steel_brushed",x,.99,.23,.49,1.70,.033,.012);
    rb("glazing","glass",x,.99,.249,.40,1.55,.009,.003);
    rb("door_handle","chrome",x+(x<0?.169:-.169),.96,.266,.014,.15,.016,.004);
  }
  for(const x of [-.43,.43])for(const z of [-.16,.16])rb("feet","steel_painted",x,.045,z,.06,.072,.06,.009);
  socket(g,"socket_floor",[0,0,0],"floor");
  socket(g,"socket_door_left",[-.48,.99,.23],"hinge");
  socket(g,"socket_door_right",[.48,.99,.23],"hinge");
}else if(id==="wall_thermostat01"){
  rb("body","ceramic",0,.092,0,.139,.172,.034,.014);
  rb("display","screen_black",0,.118,.020,.105,.072,.004,.005);
  for(let i=0;i<3;i++)rb("ticks","light_emissive",-.032+i*.032,.119,.024,.016,.003,.002,.001);
  for(const x of [-.035,.035])tb("buttons","plastic_mid",[x,.052,.021],[x,.053,.021],.015,.015,14);
  socket(g,"socket_wall",[0,.09,-.025],"wall");
}else if(id==="fire_extinguisher01"){
  tb("tank","steel_painted",[0,.11,0],[0,.54,0],.092,.092,24);
  tb("bottom","steel_brushed",[0,.09,0],[0,.113,0],.093,.092,24);
  tb("valve","chrome",[0,.53,0],[0,.59,0],.037,.032,16);
  rb("handle","plastic_dark",0,.606,-.032,.137,.022,.041,.009);
  tb("hose","plastic_dark",[.058,.56,-.01],[.11,.29,.058],.014,.012,12);
  tb("nozzle","plastic_dark",[.11,.29,.058],[.11,.23,.063],.016,.020,12);
  rb("label","ceramic",0,.355,.093,.118,.174,.005,.007);
  socket(g,"socket_floor",[0,0,0],"floor");socket(g,"socket_wall",[0,.29,-.092],"wall");
}else if(id==="security_reader01"){
  rb("housing","plastic_dark",0,.085,0,.095,.164,.033,.012);
  rb("screen","screen_black",0,.11,.019,.068,.058,.004,.005);
  rb("indicator","light_emissive",0,.057,.019,.054,.008,.003,.001);
  rb("contactless","plastic_mid",0,.031,.019,.057,.018,.004,.003);
  socket(g,"socket_wall",[0,.08,-.021],"wall");socket(g,"socket_power",[0,.035,-.021],"electric");

}else if(id==="bookcase_open02"){
  for(const x of [-.49,.49])rb("side_frames","wood_oiled",x,.95,0,.043,1.86,.38,.008);
  for(let i=0;i<6;i++){
    const y=.095+i*.35;
    rb("shelves","wood_light",0,y,0,1.01,.038,.38,.008);
    if(i<5){
      for(let j=0;j<10;j++){
        const w=.042+.022*hashTile(i,j,17),h=.19+.10*hashTile(i,j,22),
          x=-.438+j*.093,z=-.06+(.045*(j%3)),m=(i+j)%4===0?"fabric_blue":(i+j)%4===1?"wood_oiled":(i+j)%4===2?"plastic_mid":"ceramic";
        bx("book_spines",m,x,y+.021+h/2,z,w,h,.22);
        if(j%3===0)bx("paper_edges","ceramic",x+w*.42,y+.021+h/2,z+.007,.004,h*.9,.20);
      }
    }
  }
  rb("back","wood_oiled",0,.95,-.187,.98,1.88,.025,.007);
  for(const x of [-.45,.45])rb("feet","steel_brushed",x,.031,-.1,.064,.05,.08,.008);
  socket(g,"socket_floor",[0,0,0],"floor");socket(g,"socket_wall",[0,.95,-.20],"anchor");
}else if(id==="armchair_lounge02"){
  rb("seat_base","wood_oiled",0,.29,0,.82,.18,.79,.025);
  rb("seat_cushion","fabric_blue",0,.465,.06,.70,.19,.64,.036);
  rb("back_frame","wood_oiled",0,.69,-.31,.80,.65,.17,.033);
  rb("back_pillow","fabric_dark",0,.73,-.22,.68,.50,.155,.042);
  for(const x of [-.37,.37]){
    rb("armrest","wood_oiled",x,.57,-.015,.13,.44,.71,.026);
    rb("arm_pad","fabric_blue",x,.79,-.02,.14,.06,.68,.024);
  }
  for(const x of [-.31,.31])for(const z of [-.25,.25])tb("feet","steel_painted",[x,.01,z],[x,.20,z],.022,.022,12);
  socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="meeting_display_55in02"){
  rb("shell","plastic_dark",0,.50,0,1.275,.78,.062,.018);
  rb("bezel","plastic_mid",0,.50,.033,1.228,.735,.016,.012);
  bx("screen","screen_black",0,.5,.043,1.201,.692,.004);
  rb("status","light_emissive",.56,.115,.049,.014,.004,.003,.001);
  rb("wall_mount","steel_painted",0,.5,-.081,.32,.32,.040,.022);
  rb("supports","steel_brushed",0,.5,-.048,.42,.043,.032,.01);
  socket(g,"socket_wall",[0,.5,-.115],"wall");socket(g,"socket_power",[.3,.14,-.025],"electric");
  socket(g,"socket_data",[-.31,.14,-.025],"data");
}else if(id==="whiteboard_mobile02"){
  rb("frame","steel_brushed",0,1.30,0,1.55,.98,.054,.017);
  rb("board","ceramic",0,1.30,.032,1.47,.90,.011,.004);
  rb("pen_rail","steel_painted",0,.79,.072,1.27,.035,.095,.009);
  for(let i=0;i<4;i++)tb("markers",[-.55,-.18,.18,.55][i]>.0?"plastic_dark":"plastic_mid",
    [-.55+i*.30,.820,.085],[-.55+i*.30,.82,.173],.009,.009,10);
  for(const x of [-.58,.58]){
    tb("frame_legs","steel_brushed",[x,.78,-.025],[x,.15,-.025],.018,.018,12);
    rb("feet","steel_painted",x,.10,-.015,.068,.055,.45,.013);
    for(const z of [-.185,.185])ob("casters","plastic_dark",[x,.044,z],[.045,.042,.035],12,7);
  }
  socket(g,"socket_floor",[0,0,0],"floor");
  socket(g,"socket_board",[0,1.32,.042],"annotation");
}else if(id==="conference_camera01"){
  rb("housing","plastic_dark",0,.031,0,.19,.057,.065,.012);
  rb("lens_ring","steel_brushed",0,.047,.037,.060,.038,.009,.006);
  tb("lens","screen_black",[0,.05,.040],[0,.05,.046],.022,.022,16);
  ob("microphone","plastic_mid",[-.066,.045,.042],[.014,.015,.009],12,6);
  rb("privacy_led","light_emissive",.060,.051,.04,.009,.005,.002,.001);
  rb("clamp","steel_brushed",0,.005,-.024,.13,.024,.065,.008);
  socket(g,"socket_screen_top",[0,.003,-.027],"monitor_mount");socket(g,"socket_data",[-.08,.01,-.03],"data");
}else if(id==="coffee_machine01"){
  rb("housing","steel_brushed",0,.205,0,.277,.394,.29,.024);
  rb("front_panel","plastic_dark",0,.289,.148,.250,.170,.016,.008);
  rb("display","screen_black",0,.330,.159,.113,.052,.005,.006);
  for(const x of [-.066,.066])tb("buttons","chrome",[x,.265,.160],[x,.270,.160],.021,.021,18);
  tb("group_head","chrome",[0,.205,.152],[0,.153,.152],.041,.041,20);
  tb("spout","chrome",[0,.153,.152],[0,.102,.152],.009,.009,10);
  rb("drip_tray","plastic_dark",0,.055,.176,.207,.026,.18,.009);
  for(let i=0;i<7;i++)rb("grille","steel_brushed",-.075+i*.025,.071,.196,.009,.004,.14,.002);
  tb("water_tank","glass",[-.098,.25,-.130],[-.098,.406,-.130],.060,.062,18);
  rb("power_switch","light_emissive",.082,.387,.149,.019,.007,.003,.001);
  socket(g,"socket_countertop",[0,0,0],"surface");socket(g,"socket_power",[.135,.072,-.13],"electric");
  socket(g,"socket_water",[-.105,.082,-.13],"water_in");
}else if(id==="kitchenette_base01"){
  for(const x of [-.79,0,.79])rb("cabinet_body","wood_oiled",x,.42,0,.74,.78,.52,.022);
  rb("countertop","stone",0,.83,0,2.42,.055,.64,.017);
  for(const x of [-.77,.77])for(const z of [-.24,.24])rb("feet","steel_painted",x,.025,z,.078,.052,.078,.009);
  rb("sink_rim","steel_brushed",-.48,.861,.0,.44,.013,.40,.012);
  rb("sink_basin","plastic_dark",-.48,.830,.0,.36,.017,.32,.009);
  tb("tap","chrome",[-.48,.857,-.18],[-.48,1.11,-.18],.021,.021,16);
  tb("spout","chrome",[-.48,1.11,-.18],[-.48,1.12,.05],.019,.017,14);
  for(const x of [-.76,.0,.76]){
    rb("drawer_front","wood_light",x,.53,.273,.68,.51,.020,.010);
    rb("drawer_handle","chrome",x,.54,.289,.22,.019,.023,.007);
  }
  socket(g,"socket_countertop",[0,.865,0],"surface");
  socket(g,"socket_water_hot",[-.4,.12,-.30],"water_hot");
  socket(g,"socket_water_cold",[-.55,.12,-.30],"water_cold");
  socket(g,"socket_drain",[-.48,.52,0],"drain");
  socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="lounge_rug01"){
  rb("backing","fabric_dark",0,.007,0,2.30,.013,1.64,.009);
  rb("pile","fabric_blue",0,.018,0,2.26,.013,1.60,.008);
  for(let i=0;i<15;i++){
    let a=2*Math.PI*i/15,r=.48+.036*i;
    for(let j=0;j<3;j++){
      let x=Math.cos(a+j*.12)*r,z=Math.sin(a+j*.12)*r*.64;
      bx("pattern","wood_light",x,.026,z,.048,.002,.032);
    }
  }
  for(let i=0;i<21;i++){
    const x=-1.10+i*.11;
    bx("fringe","fabric_blue",x,.018,-.832,.012,.008,.050);
    bx("fringe","fabric_blue",x,.018,.832,.012,.008,.050);
  }
  socket(g,"socket_floor",[0,0,0],"floor");
}else if(id==="radiator_wall01"){
  rb("back_plate","steel_painted",0,.34,0,1.21,.56,.065,.014);
  for(let i=0;i<21;i++){
    const x=-.56+i*.056;
    rb("fins","steel_brushed",x,.35,.039,.035,.46,.053,.006);
  }
  for(let i=0;i<5;i++)rb("grille","plastic_dark",0,.616,-.036+i*.014,1.1,.012,.008,.003);
  tb("thermostatic_valve","chrome",[.61,.17,-.015],[.69,.17,-.015],.027,.027,14);
  tb("knob","ceramic",[.69,.17,-.015],[.72,.17,-.015],.038,.037,14);
  socket(g,"socket_wall",[0,.33,-.06],"wall");socket(g,"socket_heating",[.61,.17,-.015],"radiator");
}else if(id==="plant_bonsai02"){
  tb("pot","ceramic",[0,.010,0],[0,.117,0],.19,.21,24);
  tb("soil","soil",[0,.114,0],[0,.120,0],.195,.195,24);
  let nodes=[[.0,.125,.0],[.036,.22,-.01],[-.01,.34,.027],[.12,.45,.01],[.04,.54,-.02],[-.05,.59,.015]];
  for(let i=1;i<nodes.length;i++)tb("trunk","bark",nodes[i-1],nodes[i],.050-.006*i,.044-.005*i,12);
  for(let i=0;i<11;i++){
    let a=i*2.39996,y=.37+(i%5)*.055,r=.19+(i%4)*.041,
        start=nodes[Math.min(4,1+(i%4))],end=[Math.sin(a)*r,y+.06,Math.cos(a)*r];
    tb("branches","bark",start,end,.015,.003,7);
    for(let j=0;j<5;j++){
      let t=.55+j*.11,c=[start[0]+(end[0]-start[0])*t,y+.03+(j%3)*.020,start[2]+(end[2]-start[2])*t];
      leaf(g,"foliage","foliage_mask",c,[.092,.138],a+(j%3)*.31);
    }
  }
  socket(g,"socket_surface",[0,0,0],"surface");
  socket(g,"socket_wind",[0,.45,0],"wind_pivot");
}else if(id==="coat_hanger_wall01"){
  rb("back","wood_oiled",0,.10,0,.58,.19,.032,.014);
  for(let i=0;i<5;i++){
    let x=-.22+i*.11;
    tb("hooks","chrome",[x,.118,.020],[x,.095,.110],.012,.012,12);
    ob("ends","steel_brushed",[x,.095,.11],[.024,.023,.024],10,6);
  }
  socket(g,"socket_wall",[0,.10,-.022],"wall");
}else if(id==="stool_bar02"){
  tb("seat","wood_oiled",[0,.66,0],[0,.735,0],.21,.21,26);
  for(let i=0;i<4;i++){
    const a=Math.PI*.25+i*Math.PI*.5,x=.15*Math.cos(a),z=.15*Math.sin(a);
    tb("legs","steel_painted",[x*1.5,.022,z*1.5],[x,.65,z],.018,.017,12);
  }
  for(let i=0;i<4;i++){
    let a=Math.PI*.25+i*Math.PI*.5,b=a+Math.PI*.5,
      p=[.18*Math.cos(a),.29,.18*Math.sin(a)],q=[.18*Math.cos(b),.29,.18*Math.sin(b)];
    tb("footrest","steel_brushed",p,q,.012,.012,10);
  }
  socket(g,"socket_floor",[0,0,0],"floor");socket(g,"socket_seat",[0,.736,0],"seat");
}else if(id==="pinboard_cork01"){
  rb("frame","wood_oiled",0,.43,0,1.33,.86,.045,.019);
  rb("cork","wood_light",0,.43,.029,1.24,.77,.012,.006);
  for(let i=0;i<11;i++){
    const x=-.5+(i%5)*.24,y=.20+Math.floor(i/5)*.27,
          w=.14+(i%3)*.023,h=.13+(i%4)*.014;
    rb("pinned_papers","ceramic",x,y,.043,w,.001,h,.002);
    ob("pinheads","steel_brushed",[x,y+h*.45,.048],[.008,.008,.006],9,6);
    for(let j=0;j<3;j++)rb("lines","plastic_mid",x,y-.029+j*.019,.045,w*.64,.001,.003,.0005);
  }
  socket(g,"socket_wall",[0,.43,-.034],"wall");
}else if(id==="ceiling_fan01"){
  rb("canopy","steel_brushed",0,.09,0,.24,.065,.24,.018);
  tb("rod","steel_painted",[0,.05,0],[0,-.27,0],.028,.025,16);
  tb("motor","plastic_dark",[0,-.27,0],[0,-.4,0],.118,.11,26);
  for(let i=0;i<5;i++){
    const a=2*Math.PI*i/5,ux=Math.cos(a),uz=Math.sin(a),tx=-uz,tz=ux;
    let p=[ux*.12+tx*.09,-.34,uz*.12+tz*.09],
        q=[ux*.82+tx*.13,-.35,uz*.82+tz*.13],
        r=[ux*.82-tx*.13,-.35,uz*.82-tz*.13],
        t=[ux*.12-tx*.09,-.34,uz*.12-tz*.09];
    quad(g,"blades","wood_light",p,q,r,t);
    quad(g,"blade_back","wood_oiled",t,r,q,p);
  }
  tb("light","light_emissive",[0,-.40,0],[0,-.47,0],.092,.113,24);
  socket(g,"socket_ceiling",[0,.10,0],"ceiling");
  socket(g,"socket_motor",[0,-.34,0],"rotation");
  socket(g,"socket_light",[0,-.48,0],"light_down");
}else if(id==="desk_drawer_mobile02"){
  rb("body","steel_painted",0,.324,0,.43,.55,.49,.022);
  for(let i=0;i<3;i++){
    const y=.155+i*.164;
    rb("drawer_front","wood_light",0,y,.248,.382,.141,.014,.007);
    rb("handle","chrome",0,y+.009,.262,.19,.016,.023,.006);
  }
  rb("top","wood_oiled",0,.611,0,.445,.029,.51,.010);
  for(const x of [-.17,.17])for(const z of [-.18,.18]){
    tb("caster_pins","steel_brushed",[x,.015,z],[x,.048,z],.017,.017,10);
    ob("casters","plastic_dark",[x,.013,z],[.034,.019,.034],10,6);
  }
  socket(g,"socket_floor",[0,0,0],"floor");
  socket(g,"socket_top",[0,.632,0],"surface");
}else throw Error("asset "+id);
heroDetail(g,id);
g.textured=/server_rack|network_bundle|desk_(?!lamp_arch|organizer)|chair_|monitor_|door_office|door_meeting|toilet_floor|sink_wall|tree_|fern|meeting_table|partition_solid|reception_counter|shelf_office|cabinet_tall|forest_patch|shrub|planter_tall|staircase|slab_floor|rock_cluster|deadlog|sofa_lounge|coffee_table_round|floor_lamp_arc|acoustic_panel|cabinet_glass|bookcase_open|armchair_lounge|kitchenette_base|lounge_rug|plant_bonsai|stool_bar|pinboard_cork|desk_drawer_mobile/.test(id);
g.textureSize= /desk_office|door_office|tree_|meeting_table|chair_ergonomic/.test(id)?96:64;
if(id==="fire_extinguisher01")g.textured=false;
return g}

const pbrCache=new Map();
function pngTextureCache(kind,channel,dim){const key=kind+":"+channel+":"+dim;if(!pbrCache.has(key))pbrCache.set(key,pbrTexture(kind,channel,dim));return pbrCache.get(key)}

function heroInstructionPng(){
 const glyph={
 "A":["01110","10001","10001","11111","10001","10001","10001"],
 "B":["11110","10001","10001","11110","10001","10001","11110"],
 "C":["01111","10000","10000","10000","10000","10000","01111"],
 "D":["11110","10001","10001","10001","10001","10001","11110"],
 "E":["11111","10000","10000","11110","10000","10000","11111"],
 "F":["11111","10000","10000","11110","10000","10000","10000"],
 "G":["01111","10000","10000","10111","10001","10001","01111"],
 "H":["10001","10001","10001","11111","10001","10001","10001"],
 "I":["11111","00100","00100","00100","00100","00100","11111"],
 "J":["00111","00010","00010","00010","10010","10010","01100"],
 "K":["10001","10010","10100","11000","10100","10010","10001"],
 "L":["10000","10000","10000","10000","10000","10000","11111"],
 "M":["10001","11011","10101","10101","10001","10001","10001"],
 "N":["10001","11001","10101","10011","10001","10001","10001"],
 "O":["01110","10001","10001","10001","10001","10001","01110"],
 "P":["11110","10001","10001","11110","10000","10000","10000"],
 "Q":["01110","10001","10001","10001","10101","10010","01101"],
 "R":["11110","10001","10001","11110","10100","10010","10001"],
 "S":["01111","10000","10000","01110","00001","00001","11110"],
 "T":["11111","00100","00100","00100","00100","00100","00100"],
 "U":["10001","10001","10001","10001","10001","10001","01110"],
 "V":["10001","10001","10001","10001","01010","01010","00100"],
 "W":["10001","10001","10001","10101","10101","11011","10001"],
 "X":["10001","10001","01010","00100","01010","10001","10001"],
 "Y":["10001","10001","01010","00100","00100","00100","00100"],
 "Z":["11111","00001","00010","00100","01000","10000","11111"],
 "0":["01110","10011","10101","10101","11001","10001","01110"],
 "1":["00100","01100","00100","00100","00100","00100","01110"],
 "2":["11110","00001","00001","01110","10000","10000","11111"],
 "3":["11110","00001","00001","01110","00001","00001","11110"],
 "4":["10010","10010","10010","11111","00010","00010","00010"],
 ".":["00000","00000","00000","00000","00000","00110","00110"],
 " ":["00000","00000","00000","00000","00000","00000","00000"]};
 const labels=[["FIRE EXTINGUISHER",12,11,2],["ABC DRY POWDER",25,59,2],
               ["1 PULL PIN",35,117,2],["2 AIM AT BASE",32,150,2],
               ["3 SQUEEZE",38,183,2],["4 SWEEP SIDE",33,215,2]];
 const textOn=(s,ox,oy,scale,x,y)=>{
    let xx=Math.floor((x-ox)/scale),yy=Math.floor((y-oy)/scale);
    if(xx<0||yy<0||yy>=7||xx>=s.length*6)return false;
    const c=s[Math.floor(xx/6)];return xx%6<5&&glyph[c]&&glyph[c][yy][xx%6]==="1";
 };
 return pngPixels(256,256,(_u,_v,x,y)=>{
    for(const [t,x0,y0,k] of labels)if(textOn(t,x0,y0,k,x,y))return y0<58?[24,47,67,255]:[34,51,61,255];
    if(y<51)return [230,237,232,255];
    if(y<103)return [244,244,239,255];
    if(y<112)return [25,61,75,255];
    return [249,249,242,255];
 });
}

function buildGLB(g){const used=new Set(Object.values(g.groups).map(p=>p.material));const mats=Object.values(g.materials).filter(m=>used.has(m.name)),matIndex=Object.fromEntries(mats.map((m,i)=>[m.name,i]));let bytes=[],views=[],acc=[],meshes=[],nodes=[],imgs=[],tx=[],samplers=[],texturesUsed={};let byteLen=0;function bufferAdd(raw,target){let padding=(4-byteLen%4)%4;if(padding){bytes.push(new Uint8Array(padding));byteLen+=padding}let index=views.length;views.push({buffer:0,byteOffset:byteLen,byteLength:raw.length,...(target?{target}:{})});bytes.push(raw);byteLen+=raw.length;return index}function accessor(raw,count,type,component,min,max,target){let idx=acc.length;acc.push({bufferView:bufferAdd(raw,target),componentType:component,count,type,...(min?{min,max}:{})});return idx}
for(const kind of ["foliage_mask","needle_mask","foliage_maple_mask","foliage_birch_mask"].filter(n=>Object.values(g.groups).some(p=>p.material===n))){let p=pngMask(kind==="needle_mask"?"needle":kind==="foliage_maple_mask"?"maple":kind==="foliage_birch_mask"?"birch":"leaf"),iv=bufferAdd(p),i=imgs.length;imgs.push({mimeType:"image/png",bufferView:iv,name:kind+"_alpha"});if(!samplers.length)samplers.push({magFilter:9729,minFilter:9987,wrapS:10497,wrapT:10497});texturesUsed[kind]=tx.length;tx.push({source:i,sampler:0})}

for(const m of mats){
  if(g.heroLabel&&m.name==="hero_instruction"){
    const pixels=heroInstructionPng(),iv=bufferAdd(pixels),i=imgs.length;
    imgs.push({mimeType:"image/png",bufferView:iv,name:"OASIS_ABC_fire_instructions_256"});
    if(!samplers.length)samplers.push({magFilter:9729,minFilter:9987,wrapS:33071,wrapT:33071});
    m.pbrMetallicRoughness.baseColorTexture={index:tx.length,texCoord:0};
    tx.push({source:i,sampler:0});
    continue;
  }
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
import {dirname,join,resolve} from "node:path";
import {fileURLToPath} from "node:url";
export {makeAsset,buildGLB};
const ROOT=process.argv[2]||"assets/packs/oasis_mg2_v1";
const ids=["desk_office01","chair_ergonomic01","monitor_24in01","keyboard_full01","mouse_office01","door_office_solid01","toilet_floor01","sink_wall01","plant_potted01","tree_spruce01","tree_birch01","fern01","cabinet_2drawer01","meeting_table01","light_panel01","window_bay01","supply_set01","forest_patch01","door_meeting_glass01","door_service_heavy01","door_restroom01","partition_solid01","partition_glass01","slab_floor01","ceiling_tile01","staircase01","rail_stair01","reception_counter01","shelf_office01","cabinet_tall01","chair_meeting01","desktop_tower01","docking_station01","keyboard_compact01","outlet_dual01","mirror_wall01","soap_dispenser01","grabbar_wall01","toilet_wall01","tree_fir01","tree_maple01","shrub01","rock_cluster01","deadlog01","planter_tall01","lamp_desk01","duct_vent01","cable_tray01","downlight01","roller_blinds01","trash_bin01","clock_wall01","whiteboard01","potted_succulent01","desk_corner02","desk_sitstand03","chair_task02","monitor_32in02","printer_office01","phone_office01","paper_tray01","binder_row01","water_bottle01","notebook01","power_strip01","signage_exit01","floor_drain01","stump01","sapling01","tree_spruce02","tree_fir02","tree_birch02","tree_maple02","shrub02","shrub03","grass_tuft01","moss_patch01","toilet_paper01","service_pipe01","tree_maple_hero01","tree_fir_hero01","forest_patch_dense01","coffee_mug02","desk_lamp_arch02","sticky_notes01","desk_organizer02","laptop_open01","headset_stand01","desk_plant_mini01","sofa_lounge01","coffee_table_round01","floor_lamp_arc01","acoustic_panel01","conference_projector01","conference_speaker01","coat_rack01","water_cooler01","cabinet_glass02","wall_thermostat01","fire_extinguisher01","security_reader01","bookcase_open02","armchair_lounge02","meeting_display_55in02","whiteboard_mobile02","conference_camera01","coffee_machine01","kitchenette_base01","lounge_rug01","radiator_wall01","plant_bonsai02","coat_hanger_wall01","stool_bar02","pinboard_cork01","ceiling_fan01","desk_drawer_mobile02","server_rack_cluster01","stapler_office01","network_bundle01"];
// Imported by the staging repair tool without regenerating the entire pack.
if(process.argv[1]&&resolve(process.argv[1])===fileURLToPath(import.meta.url)){
  for(const id of ids){
    const g=makeAsset(id),{glb,triangles,bounds}=buildGLB(g);
    const file=join(ROOT,"geometry",id+".glb");
    mkdirSync(dirname(file),{recursive:true});
    writeFileSync(file,glb);
    console.log(id,triangles,glb.length,bounds);
  }
}
