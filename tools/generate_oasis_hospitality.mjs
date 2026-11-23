// OASIS hotel / building life kit. Original project-owned geometry.
// Regenerate only this asset family; never overwrite locally acquired PBR GLBs.
// In production, staged assets can receive 2K CC0 maps through pbr_lot1.bat.
const HOSPITALITY=[
 ["gas_range01","hospitality"],["treadmill01","hospitality"],
 ["weights_rack01","hospitality"],["sauna_bench01","hospitality"],
 ["sauna_heater01","hospitality"],["hot_tub01","hospitality"],
 ["market_fridge01","hospitality"],["phone_booth01","hospitality"],
 ["elevator_portal01","hospitality"],["roof_hvac01","hospitality"],
 ["hotel_bed01","hospitality"],["luggage_cart01","hospitality"],
 ["coffee_mug_liberte55","props"]
];
const H_MATS={
 steel_brushed:[[.61,.68,.73,1],1,.27],
 steel_painted:[[.21,.27,.33,1],.78,.43],
 chrome:[[.76,.82,.87,1],1,.12],
 wood_light:[[.72,.47,.24,1],0,.42],
 wood_oiled:[[.37,.22,.12,1],0,.38],
 ceramic:[[.93,.93,.89,1],0,.19],
 porcelain:[[.97,.97,.94,1],0,.14],
 stone:[[.37,.39,.40,1],0,.72],
 fabric_dark:[[.14,.22,.30,1],0,.86],
 fabric_blue:[[.17,.31,.49,1],0,.80],
 rubber_dark:[[.04,.05,.06,1],0,.95],
 plastic_dark:[[.10,.13,.15,1],0,.67],
 glass:[[.61,.81,.95,.32],0,.12],
 glass_tinted:[[.27,.50,.63,.24],0,.18],
 water:[[.28,.67,.78,.45],0,.10],
 light_emissive:[[1,.65,.30,1],0,.17],
 blue_print:[[.11,.19,.60,1],0,.65],
 white_print:[[.97,.95,.81,1],0,.60],
 red_detail:[[.58,.10,.07,1],0,.48],
 copper:[[.64,.33,.12,1],1,.30]
};
function hb(s){const x=[];for(let i=0;i<s.length;i++)x.push(s.charCodeAt(i));return Uint8Array.from(x)}
function h32(v){const b=new Uint8Array(4);new DataView(b.buffer).setUint32(0,v>>>0,true);return b}
function hfloat(a){const b=new Uint8Array(a.length*4),d=new DataView(b.buffer);a.forEach((v,i)=>d.setFloat32(i*4,v,true));return b}
function hu32(a){const b=new Uint8Array(a.length*4),d=new DataView(b.buffer);a.forEach((v,i)=>d.setUint32(i*4,v>>>0,true));return b}
function hcat(arr){const n=arr.reduce((s,x)=>s+x.length,0),b=new Uint8Array(n);let o=0;for(const x of arr){b.set(x,o);o+=x.length}return b}
function hpad(a,c=0){return a.length%4?hcat([a,new Uint8Array(4-a.length%4).fill(c)]):a}
function hg(){return {groups:new Map()}}
function hp(g,node,material){const key=node+"/"+material;if(!g.groups.has(key))g.groups.set(key,{name:node,material,v:[],idx:[]});return g.groups.get(key)}
function hquad(g,n,m,a,b,c,d){const p=hp(g,n,m),i=p.v.length/8;
 const u=[b[0]-a[0],b[1]-a[1],b[2]-a[2]],v=[c[0]-a[0],c[1]-a[1],c[2]-a[2]];
 let normal=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
 const len=Math.hypot(...normal)||1;normal=normal.map(q=>q/len);
 for(const [pt,uv] of [[a,[0,0]],[b,[1,0]],[c,[1,1]],[d,[0,1]]])p.v.push(...pt,...normal,...uv);
 p.idx.push(i,i+1,i+2,i,i+2,i+3)
}
function hbox(g,n,m,x,y,z,w,h,d){
 const X=x-w/2,R=x+w/2,B=y-h/2,T=y+h/2,F=z+d/2,K=z-d/2;
 hquad(g,n,m,[X,B,F],[R,B,F],[R,T,F],[X,T,F]);
 hquad(g,n,m,[R,B,K],[X,B,K],[X,T,K],[R,T,K]);
 hquad(g,n,m,[X,B,K],[X,B,F],[X,T,F],[X,T,K]);
 hquad(g,n,m,[R,B,F],[R,B,K],[R,T,K],[R,T,F]);
 hquad(g,n,m,[X,T,F],[R,T,F],[R,T,K],[X,T,K]);
 hquad(g,n,m,[X,B,K],[X,B,F],[R,B,F],[R,B,K]);
}
function htube(g,n,m,a,b,r,N=16){
 const dir=b.map((v,i)=>v-a[i]),length=Math.hypot(...dir)||1,up=dir.map(x=>x/length);
 const axis=Math.abs(up[1])<.92?[0,1,0]:[1,0,0];
 let e=[up[1]*axis[2]-up[2]*axis[1],up[2]*axis[0]-up[0]*axis[2],up[0]*axis[1]-up[1]*axis[0]];
 const el=Math.hypot(...e)||1;e=e.map(v=>v/el);
 const f=[up[1]*e[2]-up[2]*e[1],up[2]*e[0]-up[0]*e[2],up[0]*e[1]-up[1]*e[0]];
 const p=hp(g,n,m),off=p.v.length/8;
 for(let k=0;k<=N;k++){const th=2*Math.PI*k/N,normal=e.map((v,i)=>Math.cos(th)*v+Math.sin(th)*f[i]);
  for(let t=0;t<2;t++){const c=t?b:a;p.v.push(...c.map((v,i)=>v+r*normal[i]),...normal,k/N,t)}
 }
 for(let k=0;k<N;k++){const q=off+k*2;p.idx.push(q,q+2,q+1,q+1,q+2,q+3)}
 for(let t=0;t<2;t++){const c=t?b:a,normal=t?up:up.map(v=>-v),q=p.v.length/8;
  p.v.push(...c,...normal,.5,.5);
  for(let k=0;k<=N;k++){const th=2*Math.PI*k/N,rad=e.map((v,i)=>Math.cos(th)*v+Math.sin(th)*f[i]);
   p.v.push(...c.map((v,i)=>v+r*rad[i]),...normal,.5+.5*Math.cos(th),.5+.5*Math.sin(th))
  }
  for(let k=0;k<N;k++)if(t)p.idx.push(q,q+1+k,q+2+k);else p.idx.push(q,q+2+k,q+1+k)
 }
}
function htorus(g,n,m,c,r,t,N=36,M=10){
 const p=hp(g,n,m),off=p.v.length/8;
 for(let i=0;i<=N;i++){const a=2*Math.PI*i/N,u=Math.cos(a),v=Math.sin(a);
  for(let j=0;j<=M;j++){const b=2*Math.PI*j/M,w=Math.cos(b),f=Math.sin(b);
   p.v.push(c[0]+(r+t*w)*u,c[1]+t*f,c[2]+(r+t*w)*v,u*w,f,v*w,i/N,j/M)
  }
 }
 for(let i=0;i<N;i++)for(let j=0;j<M;j++){const q=off+i*(M+1)+j,b=q+M+1;p.idx.push(q,q+1,b,q+1,b+1,b)}
}
function horb(g,n,m,x,y,z,r,N=12,M=8){
 const p=hp(g,n,m),off=p.v.length/8;
 for(let j=0;j<=M;j++){const a=Math.PI*j/M;
  for(let i=0;i<=N;i++){const b=2*Math.PI*i/N,u=Math.sin(a)*Math.cos(b),v=Math.cos(a),w=Math.sin(a)*Math.sin(b);
   p.v.push(x+r*u,y+r*v,z+r*w,u,v,w,i/N,j/M)
  }
 }
 for(let j=0;j<M;j++)for(let i=0;i<N;i++){const q=off+j*(N+1)+i,b=q+N+1;p.idx.push(q,q+1,b,q+1,b+1,b)}
}
function hdisc(g,n,m,x,y,z,r,N=40){
 const p=hp(g,n,m),o=p.v.length/8;
 p.v.push(x,y,z,0,1,0,.5,.5);
 for(let k=0;k<=N;k++){const t=2*Math.PI*k/N;p.v.push(x+r*Math.cos(t),y,z+r*Math.sin(t),0,1,0,.5+.5*Math.cos(t),.5+.5*Math.sin(t))}
 for(let k=0;k<N;k++)p.idx.push(o,o+1+k,o+2+k)
}
function hline(g,n,m,a,b,w=.015){htube(g,n,m,a,b,w,10)}
function hasset(id){
 const g=hg(),B=(n,m,x,y,z,w,h,d)=>hbox(g,n,m,x,y,z,w,h,d),
 T=(n,m,a,b,r,N=16)=>htube(g,n,m,a,b,r,N),
 O=(n,m,x,y,z,r)=>horb(g,n,m,x,y,z,r),
 R=(n,m,c,r,t)=>htorus(g,n,m,c,r,t),D=(n,m,x,y,z,r)=>hdisc(g,n,m,x,y,z,r);
 if(id==="gas_range01"){
  B("housing","steel_painted",0,.44,0,.89,.86,.68);B("hob","steel_brushed",0,.89,0,.92,.065,.71);
  B("oven_glass","glass_tinted",0,.42,.347,.67,.42,.026);B("oven_frame","chrome",0,.42,.37,.77,.51,.025);
  B("oven_window","glass_tinted",0,.42,.391,.60,.32,.012);
  T("handle","chrome",[-.33,.69,.42],[.33,.69,.42],.018);
  for(const x of [-.25,.25])for(const z of [-.19,.19]){
   R("burner_ring","steel_brushed",[x,.945,z],.13,.017);
   D("burner_cap","rubber_dark",x,.949,z,.083);
   for(let k=0;k<6;k++){let a=2*Math.PI*k/6;
    T("grate","steel_painted",[x-.15*Math.cos(a),.97,z-.15*Math.sin(a)],[x+.15*Math.cos(a),.97,z+.15*Math.sin(a)],.009,8)
   }
  }
  for(let i=0;i<6;i++){const x=-.32+i*.128;
   T("knobs","plastic_dark",[x,.79,.355],[x,.79,.405],.038,16);
   B("knob_marker","white_print",x,.809,.409,.015,.025,.009)
  }
  B("kick","steel_brushed",0,.07,.35,.79,.07,.05);
 } else if(id==="treadmill01"){
  B("deck","steel_painted",0,.18,0,.91,.21,1.82);
  B("belt","rubber_dark",0,.297,0,.76,.022,1.57);
  for(const z of [-.79,.79])T("rollers","steel_brushed",[-.43,.24,z],[.43,.24,z],.073,20);
  for(const x of [-.46,.46]){
   B("rail","steel_brushed",x,.33,0,.035,.11,1.82);
   T("upright","steel_painted",[x,.2,-.63],[x,1.30,-.75],.031,14);
   T("handle","rubber_dark",[x,1.22,-.68],[x*.60,1.28,-.30],.033,12);
   B("feet","rubber_dark",x,.065,.69,.14,.11,.25)
  }
  B("console","plastic_dark",0,1.32,-.73,.72,.27,.15);
  B("display","glass_tinted",0,1.34,-.639,.48,.14,.009);
  for(let i=0;i<8;i++)B("buttons","steel_brushed",-.27+i*.077,1.23,-.624,.045,.022,.008);
 } else if(id==="weights_rack01"){
  for(let x of [-.58,.58]){
   T("legs","steel_painted",[x,0,.0],[x,1.25,-.10],.032);
   T("base","steel_painted",[x,0,-.3],[x,0,.35],.032);
  }
  for(let level=0;level<3;level++){
   const y=.35+level*.37;
   T("shelf_bar","steel_brushed",[-.60,y,.06],[.60,y,.06],.025);
   for(let i=0;i<4;i++){const x=-.43+i*.28,z=.04;
    T("grip","chrome",[x-.13,y+.07,z],[x+.13,y+.07,z],.026,12);
    for(let side of [-1,1]){
     const end=x+side*.12;
     T("plates","rubber_dark",[end-.045,y+.07,z],[end+.045,y+.07,z],.072,18);
     T("collars","steel_brushed",[end-side*.065,y+.07,z],[end-side*.04,y+.07,z],.04,12)
    }
   }
  }
 } else if(id==="sauna_bench01"){
  for(let level=0;level<2;level++){const y=.44+level*.37,z=.22-level*.22;
   for(let k=0;k<11;k++)B("slats","wood_light",- .82+k*.164,y,z, .145,.035,.57);
   for(let x of [-.76,.76])B("legs","wood_oiled",x,y*.5,z,.09,y,.48);
  }
  for(let y of [.21,.57, .95])B("back_support","wood_oiled",0,y,-.39,1.78,.035,.052);
  for(let x of [-.79,.79])B("back_posts","wood_oiled",x,.62,-.4,.075,1.2,.075);
 } else if(id==="sauna_heater01"){
  B("casing","steel_painted",0,.36,0,.55,.70,.47);
  for(let x of [-.21,.21])for(let z of [-.17,.17])B("legs","steel_brushed",x,.055,z,.045,.11,.055);
  for(let i=0;i<8;i++){const z=-.19+i*.055;B("grille","steel_brushed",0,.45,z,.53,.022,.018)}
  for(let i=0;i<14;i++){const a=2*Math.PI*i/14;
   O("stones","stone",.16*Math.cos(a),.72+.032*(i%3),.14*Math.sin(a),.055+.008*(i%2))
  }
  T("thermostat","chrome",[.28,.20,.1],[.30,.20,.1],.045);
 } else if(id==="hot_tub01"){
  D("basin","ceramic",0,.11,0,1.00);
  for(let i=0;i<24;i++){const a=2*Math.PI*i/24,b=2*Math.PI*(i+1)/24;
   const x=Math.cos(a),z=Math.sin(a),xx=Math.cos(b),zz=Math.sin(b);
   hquad(g,"shell","ceramic",[x*.94,.10,z*.94],[xx*.94,.10,zz*.94],
       [xx*1.05,.82,zz*1.05],[x*1.05,.82,z*1.05]);
   hquad(g,"interior","ceramic",[xx*.86,.16,zz*.86],[x*.86,.16,z*.86],
       [x*.94,.79,z*.94],[xx*.94,.79,zz*.94]);
   if(i%2===0)T("jet_nozzles","chrome",[x*.90,.45,z*.90],[x*.94,.45,z*.94],.025,12);
  }
  R("rim","chrome",[0,.82,0],1.00,.06);
  D("water_surface","water",0,.63,0,.89);
  for(let i=0;i<4;i++){const a=2*Math.PI*i/4;T("handrail","chrome",
     [1.07*Math.cos(a),.85,1.07*Math.sin(a)],[1.07*Math.cos(a),1.27,1.07*Math.sin(a)],.028)}
 } else if(id==="market_fridge01"){
  B("case","steel_painted",0,1.09,0,1.20,2.18,.76);
  B("cavity","plastic_dark",0,1.15,.38,1.03,1.95,.018);
  B("glass_front","glass",0,1.12,.41,1.03,1.92,.014);
  for(let x of [-.55,0,.55])B("door_frame","chrome",x,1.10,.426,.035,1.97,.048);
  for(let y of [.16,.63,1.10,1.57,2.02])B("shelves","steel_brushed",0,y,.10,1.03,.025,.56);
  for(let level=0;level<4;level++)for(let i=0;i<7;i++){
   const x=-.42+i*.14,y=.19+level*.48,z=.14;
   T("beverages","ceramic",[x,y,z],[x,y+.25,z],.045,10);
   B("beverage_labels","red_detail",x,y+.14,z+.045,.07,.095,.008)
  }
  for(let x of [-.43,.43])B("lighting","light_emissive",x,1.14,.30,.012,1.89,.01);
  B("compressor_grille","steel_brushed",0,.07,.40,.92,.09,.019);
 } else if(id==="phone_booth01"){
  for(let x of [-.41,.41])for(let z of [-.41,.41])B("frame","steel_painted",x,1.12,z,.045,2.24,.045);
  B("roof","steel_painted",0,2.24,0,.91,.08,.91);
  B("base","steel_painted",0,.04,0,.91,.08,.91);
  for(let x of [-.43,.43])B("side_glass","glass",x,1.15,0,.017,2.03,.78);
  B("back_glass","glass",0,1.15,-.42,.78,2.03,.017);
  B("telephone","plastic_dark",0,1.35,-.32,.40,.48,.14);
  B("telephone_screen","glass_tinted",0,1.43,-.235,.24,.11,.01);
  for(let i=0;i<12;i++){const x=-.11+(i%3)*.11,y=1.35-Math.floor(i/3)*.09;
   B("keypad","steel_brushed",x,y,-.238,.061,.05,.009)
  }
  T("handset","plastic_dark",[-.25,1.53,-.235],[-.25,1.17,-.235],.038,10);
  B("overhead_sign","blue_print",0,2.12,.424,.82,.15,.022);
 } else if(id==="elevator_portal01"){
  for(let x of [-1.08,1.08])B("jamb","steel_brushed",x,1.34,0,.12,2.68,.16);
  B("header","steel_brushed",0,2.64,0,2.27,.14,.18);
  B("sill","steel_brushed",0,.045,.05,2.29,.09,.16);
  for(let x of [-.51,.51])B("door_leaf","steel_painted",x,1.35,.045,1.00,2.47,.072);
  for(let x of [-.97,-.04,.04,.97])B("vertical_seam","chrome",x,1.35,.10,.013,2.47,.018);
  B("floor_display","glass_tinted",.83,2.47,.109,.24,.12,.011);
  B("arrow_up","light_emissive",.83,2.48,.12,.05,.07,.011);
  for(let y of [1.17,1.3])T("call_buttons","chrome",[1.15,y,.12],[1.19,y,.12],.043,12);
 } else if(id==="roof_hvac01"){
  B("housing","steel_painted",0,.62,0,1.64,1.22,1.35);
  B("top_grille","steel_brushed",0,1.25,0,1.66,.055,1.38);
  R("fan_rim","chrome",[0,1.31,0],.45,.028);
  for(let i=0;i<8;i++){const a=2*Math.PI*i/8;
   const r=.20;B("fan_blade","steel_brushed",r*Math.cos(a),1.32,r*Math.sin(a),.31,.013,.10);
  }
  T("fan_hub","steel_brushed",[0,1.29,0],[0,1.37,0],.10);
  for(let i=0;i<13;i++){const z=-.54+i*.09;
   B("louvers","steel_brushed",0,.62,z,1.65,.034,.025)
  }
  for(let x of [-.65,.65])for(let z of [-.48,.48])B("feet","rubber_dark",x,.035,z,.17,.07,.17);
  B("service_hatch","steel_brushed",0,.52,.69,.55,.57,.035);
 } else if(id==="hotel_bed01"){
  B("bed_frame","wood_oiled",0,.26,0,2.00,.34,2.12);
  for(let x of [-.88,.88])for(let z of [-.96,.96])B("legs","wood_oiled",x,.12,z,.12,.24,.12);
  B("mattress","ceramic",0,.50,.02,1.87,.22,1.99);
  B("blanket","fabric_blue",0,.635,.34,1.86,.049,1.19);
  B("throw","fabric_dark",0,.663,.86,1.86,.020,.21);
  B("headboard","wood_light",0,.74,-1.08,2.12,1.04,.11);
  for(let x of [-.51,.51]){
   B("pillow","ceramic",x,.67,-.69,.74,.16,.48);
   B("pillow_trim","fabric_dark",x,.65,-.445,.71,.021,.026)
  }
 } else if(id==="luggage_cart01"){
  for(let x of [-.48,.48])for(let z of [-.30,.30]){
   T("casters","rubber_dark",[x,.115,z-.072],[x,.115,z+.072],.081,18);
  }
  B("lower_tray","wood_oiled",0,.25,0,1.10,.09,.76);
  B("upper_tray","fabric_dark",0,.35,0,.93,.08,.67);
  for(let x of [-.46,.46]){
   T("uprights","chrome",[x,.30,-.26],[x,1.55,-.26],.030);
   T("handle","chrome",[x,1.55,-.26],[x,1.62,.22],.029)
  }
  T("top_rail","chrome",[-.46,1.62,.22],[.46,1.62,.22],.032);
  B("suitcase","fabric_blue",0,.65,0,.73,.56,.47);
  B("case_trim","steel_brushed",0,.80,.25,.73,.022,.018);
  T("case_handle","plastic_dark",[-.14,.97,0],[.14,.97,0],.029);
 } else if(id==="coffee_mug_liberte55"){
  // Original 3D porcelain mug with project lettering, no third-party branding.
  D("cup_base","ceramic",0,.025,0,.086);
  for(let i=0;i<24;i++){const a=2*Math.PI*i/24,b=2*Math.PI*(i+1)/24;
   const u=Math.cos(a),v=Math.sin(a),uu=Math.cos(b),vv=Math.sin(b);
   hquad(g,"cup_wall","ceramic",[u*.086,.025,v*.086],[uu*.086,.025,vv*.086],
      [uu*.101,.20,vv*.101],[u*.101,.20,v*.101]);
  }
  R("lip","chrome",[0,.20,0],.098,.010);
  R("handle","ceramic",[.145,.111,0],.063,.013,24,8);
  D("coffee","wood_oiled",0,.183,0,.088);
  // 5x7 letter grid on front-facing blue label, deliberately small hero art.
  B("printed_plate","white_print",0,.119,.101,.155,.10,.006);
  const letters={
   L:["10000","10000","10000","10000","10000","10000","11111"],
   I:["11111","00100","00100","00100","00100","00100","11111"],
   B:["11110","10001","10001","11110","10001","10001","11110"],
   E:["11111","10000","10000","11110","10000","10000","11111"],
   R:["11110","10001","10001","11110","10100","10010","10001"],
   T:["11111","00100","00100","00100","00100","00100","00100"],
   "5":["11111","10000","10000","11110","00001","00001","11110"]
  };
  const label="LIBERTE55",px=.0028,py=.0076;
  for(let k=0;k<label.length;k++)for(let y=0;y<7;y++)for(let x=0;x<5;x++){
   if(letters[label[k]]?.[y][x]!=="1")continue;
   B("liberte55_ink","blue_print",-.075+k*.017+x*px,.150-y*py,.107,.0024,.006,.0015)
  }
 } else throw Error("Unknown hospitality model "+id);
 return g
}
function hfinish(id,g){
 let parts=Array.from(g.groups.values()).filter(p=>p.idx.length);
 const names=[...new Set(parts.map(p=>p.material))],mats=names.map(n=>{
  const [color,metal,rough]=H_MATS[n],trans=color[3]<.999;
  return {name:n,pbrMetallicRoughness:{baseColorFactor:color,metallicFactor:metal,roughnessFactor:rough},
   ...(trans?{alphaMode:"BLEND",doubleSided:true}:{}),
   ...(n==="light_emissive"?{emissiveFactor:[1,.52,.14]}:{})}
 });
 let chunks=[],views=[],access=[],meshes=[],nodes=[],offset=0,tri=0;
 const addBuf=(b,target)=>{const pad=(-offset)&3;if(pad){chunks.push(new Uint8Array(pad));offset+=pad}
   const i=views.length;views.push({buffer:0,byteOffset:offset,byteLength:b.length,...(target?{target}:{})});
   chunks.push(b);offset+=b.length;return i};
 const addAcc=(b,count,type,component,target,min,max)=>{const a={bufferView:addBuf(b,target),componentType:component,count,type};
  if(min){a.min=min;a.max=max}const i=access.length;access.push(a);return i};
 const globalMin=[Infinity,Infinity,Infinity],globalMax=[-Infinity,-Infinity,-Infinity];
 for(const [i,p] of parts.entries()){
  const pos=[],norm=[],uv=[],low=[Infinity,Infinity,Infinity],high=[-Infinity,-Infinity,-Infinity];
  for(let k=0;k<p.v.length;k+=8){
   for(let j=0;j<3;j++){
    const v=p.v[k+j];pos.push(v);norm.push(p.v[k+j+3]);
    low[j]=Math.min(low[j],v);high[j]=Math.max(high[j],v);
    globalMin[j]=Math.min(globalMin[j],v);globalMax[j]=Math.max(globalMax[j],v)
   }
   uv.push(p.v[k+6],p.v[k+7])
  }
  const vertexCount=pos.length/3;
  const prim={attributes:{
   POSITION:addAcc(hfloat(pos),vertexCount,"VEC3",5126,34962,low,high),
   NORMAL:addAcc(hfloat(norm),vertexCount,"VEC3",5126,34962),
   TEXCOORD_0:addAcc(hfloat(uv),vertexCount,"VEC2",5126,34962)},
   indices:addAcc(hu32(p.idx),p.idx.length,"SCALAR",5125,34963),
   material:names.indexOf(p.material),mode:4};
  const n=p.name+"_"+p.material+"_"+i;
  meshes.push({name:n,primitives:[prim]});nodes.push({name:n,mesh:i});
  tri+=p.idx.length/3
 }
 const doc={asset:{version:"2.0",generator:"OASIS hospitality authored source v1"},scene:0,
   scenes:[{nodes:nodes.map((_,i)=>i)}],nodes,meshes,materials:mats,accessors:access,
   bufferViews:views,buffers:[{byteLength:offset}],
   extras:{units:"m",up:"Y",project:"OASIS",source:"tools/generate_oasis_hospitality.mjs"}};
 const json=hpad(hb(JSON.stringify(doc)),32),bin=hpad(hcat(chunks));
 const bytes=hcat([h32(0x46546c67),h32(2),h32(12+8+json.length+8+bin.length),
   h32(json.length),h32(0x4e4f534a),json,h32(bin.length),h32(0x004e4942),bin]);
 return {id,bytes,triangles:tri,bounds:{min:globalMin,max:globalMax},
   nodes:nodes.map(x=>x.name),materials:names}
}
function generateHospitality(){return HOSPITALITY.map(([id,family])=>({
 ...hfinish(id,hasset(id)),asset_key:"oasis."+family+"."+id
}))}
function hospitalityMeta(a,sha){
 return {schema_version:1,asset_key:a.asset_key,asset_revision:1,
  glb:"geometry/"+a.id+".glb",reference_lod:"source_detail",
  bounds_m:a.bounds,
  nodes:Object.fromEntries(a.nodes.map(n=>[n,n])),
  materials:Object.fromEntries(a.materials.map(n=>[n,n])),
  collision:{mode:"none",nodes:[]},sockets:{},
  visibility:{cast_shadow:true,ray_class:"mixed_opaque_transparent"},
  cut:{mode:"none"},geometry:{triangles:a.triangles,bytes:a.bytes.length,
    embedded_textures:0,material_slots:a.materials.length},
  provenance:"provenance/"+a.id+".json",status:"authoring_reference_awaiting_pbr"}
}
function hospitalityProvenance(a){
 return {original_project_geometry:true,editable_source:"tools/generate_oasis_hospitality.mjs",
  source_revision:1,license:"Original OASIS project work",third_party_content:[],
  status:"source_reference",intent:a.asset_key}
}
// ---- NODE CLI ----
export {generateHospitality,hospitalityMeta,hospitalityProvenance};
if(typeof process!=="undefined"&&process.argv[1]?.replaceAll("\\","/").endsWith("/generate_oasis_hospitality.mjs")){
 const fs=await import("node:fs"),crypto=await import("node:crypto"),path=await import("node:path");
 const root=path.resolve(process.argv[2]||"assets/packs/oasis_mg2_v1"),packPath=path.join(root,"pack.json");
 const pack=JSON.parse(fs.readFileSync(packPath,"utf8"));
 for(const a of generateHospitality()){
  const glb="geometry/"+a.id+".glb",metadata="meta/"+a.id+".json",provenance="provenance/"+a.id+".json";
  const digest=crypto.createHash("sha1").update("blob "+a.bytes.length+"\0").update(a.bytes).digest("hex");
  const row={asset_key:a.asset_key,asset_revision:1,glb,metadata,provenance,git_blob_sha1:digest};
  const old=pack.assets.find(x=>x.asset_key===a.asset_key);
  if(old)Object.assign(old,row);else pack.assets.push(row);
  for(const rel of [glb,metadata,provenance])fs.mkdirSync(path.dirname(path.join(root,rel)),{recursive:true});
  fs.writeFileSync(path.join(root,glb),a.bytes);
  fs.writeFileSync(path.join(root,metadata),JSON.stringify(hospitalityMeta(a,digest),null,2)+"\n");
  fs.writeFileSync(path.join(root,provenance),JSON.stringify(hospitalityProvenance(a),null,2)+"\n");
  console.log(a.id,a.triangles,a.bytes.length)
 }
 pack.pack_revision=Math.max(pack.pack_revision,12);
 pack.hospitality_generator="tools/generate_oasis_hospitality.mjs";
 fs.writeFileSync(packPath,JSON.stringify(pack,null,2)+"\n")
}
