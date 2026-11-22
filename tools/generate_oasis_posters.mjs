
/* OASIS original wall-poster atlas producer: 100 different source posters.
 * node tools/generate_oasis_posters.mjs [output_directory]
 * Five sheets, 20 posters each; glTF node selection is required at runtime.
 * Text and graphic design are intentionally original procedural placeholders.
 */
function utf8(s){const q=unescape(encodeURIComponent(s)),a=new Uint8Array(q.length);for(let i=0;i<q.length;i++)a[i]=q.charCodeAt(i);return a}
function cat(a){let n=a.reduce((x,b)=>x+b.length,0),o=new Uint8Array(n),j=0;for(const b of a){o.set(b,j);j+=b.length}return o}
function be(v){let a=new Uint8Array(4);new DataView(a.buffer).setUint32(0,v>>>0,false);return a}
function le(v){let a=new Uint8Array(4);new DataView(a.buffer).setUint32(0,v>>>0,true);return a}
function f32(a){let b=new Uint8Array(a.length*4),d=new DataView(b.buffer);a.forEach((v,i)=>d.setFloat32(i*4,v,true));return b}
function u16(a){let b=new Uint8Array(a.length*2),d=new DataView(b.buffer);a.forEach((v,i)=>d.setUint16(i*2,v,true));return b}
function pad(a,k=0){return a.length%4?cat([a,new Uint8Array(Array(4-a.length%4).fill(k))]):a}
function crc(a){let c=-1;for(const x of a){c^=x;for(let i=0;i<8;i++)c=(c>>>1)^((c&1)?0xedb88320:0)}return (c^-1)>>>0}
function b64(b){const cs="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",out=[];for(let i=0;i<b.length;i+=3){let n=(b[i]<<16)|((b[i+1]||0)<<8)|(b[i+2]||0);out.push(cs[n>>>18],cs[(n>>>12)&63],i+1<b.length?cs[(n>>>6)&63]:"=",i+2<b.length?cs[n&63]:"=")}return out.join("")}
function png(rgba,W,H){
 const row=W*4,raw=new Uint8Array(H*(row+1));
 for(let y=0;y<H;y++)raw.set(rgba.subarray(y*row,(y+1)*row),y*(row+1)+1);
 let z=[0x78,0x01];for(let p=0;p<raw.length;){let n=Math.min(65535,raw.length-p),last=p+n===raw.length;
   z.push(last?1:0,n&255,n>>>8,(~n)&255,((~n)>>>8)&255);
   for(let i=p;i<p+n;i++)z.push(raw[i]);p+=n;
 }
 let a=1,b=0;for(const v of raw){a=(a+v)%65521;b=(b+a)%65521}
 z.push(...be((b<<16)|a));
 function chunk(type,data){const c=utf8(type);return cat([be(data.length),c,data,be(crc(cat([c,data])))])}
 return cat([Uint8Array.from([137,80,78,71,13,10,26,10]),
 chunk("IHDR",cat([be(W),be(H),Uint8Array.from([8,6,0,0,0])])),
 chunk("IDAT",Uint8Array.from(z)),chunk("IEND",new Uint8Array())]);
}
const VW=160,VH=224,COLS=5,ROWS=4,W=VW*COLS,H=VH*ROWS;
const clamp=x=>Math.max(0,Math.min(255,Math.round(x)));
function hash(x,y,seed){let n=(Math.imul(x+43,374761393)+Math.imul(y+1,668265263)+Math.imul(seed+3,1442695041))|0;
 n=Math.imul(n^(n>>>13),1274126177);return ((n^(n>>>16))>>>0)/4294967295}
const roots=["NORTH","SILENT","LAST","IRON","WINTER","BRAVE","NEW","GLASS","OLD","MIDNIGHT"];
const nouns=["WOLF","CITY","HORIZON","ORDER","FOREST","MEMORY","FUTURE","CONTROL","DAWN","SILENCE"];
const captions=["FOLLOW NO SHADOW","REMEMBER THE EARTH","KEEP MOVING","TRUST THE SIGNAL","STAY WITH US",
 "THE FUTURE IS HERE","OWN THE TOMORROW","NOTHING STANDS STILL","BUILT TO ENDURE","LOOK BEYOND"];
const groupName=["wildlife","motivation","corporate","safety","abstract"];
function poster(n){
 const theme=n%5,i=Math.floor(n/5),title=roots[(n*3)%roots.length]+" "+nouns[(i+Math.floor(n/10))%nouns.length];
 return {id:"poster_"+String(n+1).padStart(3,"0"),title,category:groupName[theme],subtitle:captions[(n*7+3)%captions.length],seed:0x415349+n*101,aging_grade:n%6};
}
const font={
 "A":["01110","10001","10001","11111","10001","10001","10001"],
 "B":["11110","10001","10001","11110","10001","10001","11110"],
 "C":["01111","10000","10000","10000","10000","10000","01111"],
 "D":["11110","10001","10001","10001","10001","10001","11110"],
 "E":["11111","10000","10000","11110","10000","10000","11111"],
 "F":["11111","10000","10000","11110","10000","10000","10000"],
 "G":["01111","10000","10000","10111","10001","10001","01110"],
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
 "V":["10001","10001","10001","10001","10001","01010","00100"],
 "W":["10001","10001","10001","10101","10101","11011","10001"],
 "X":["10001","10001","01010","00100","01010","10001","10001"],
 "Y":["10001","10001","01010","00100","00100","00100","00100"],
 "Z":["11111","00001","00010","00100","01000","10000","11111"],
 "0":["01110","10011","10101","10101","11001","10001","01110"],
 "1":["00100","01100","00100","00100","00100","00100","01110"],
 "2":["01110","10001","00001","00010","00100","01000","11111"],
 "3":["11110","00001","00001","01110","00001","00001","11110"],
 "4":["00010","00110","01010","10010","11111","00010","00010"],
 "5":["11111","10000","10000","11110","00001","00001","11110"],
 "6":["00111","01000","10000","11110","10001","10001","01110"],
 "7":["11111","00001","00010","00100","01000","01000","01000"],
 "8":["01110","10001","10001","01110","10001","10001","01110"],
 "9":["01110","10001","10001","01111","00001","00010","11100"],
 " ":["00000","00000","00000","00000","00000","00000","00000"],
 "-":["00000","00000","00000","11111","00000","00000","00000"]
};
function makeSheet(sheet){
 if(sheet<0||sheet>4)throw Error("sheet range");
 const pix=new Uint8Array(W*H*4),posters=Array.from({length:20},(_,j)=>poster(sheet*20+j));
 function set(x,y,c){if(x<0||y<0||x>=W||y>=H)return;let k=(y*W+x)*4;pix[k]=clamp(c[0]);pix[k+1]=clamp(c[1]);pix[k+2]=clamp(c[2]);pix[k+3]=255}
 function line(x0,y0,x1,y1,c,w=1){let steps=Math.max(1,Math.ceil(Math.hypot(x1-x0,y1-y0)*1.5));for(let i=0;i<=steps;i++){let t=i/steps,x=Math.round(x0+(x1-x0)*t),y=Math.round(y0+(y1-y0)*t);for(let a=-w;a<=w;a++)for(let b=-w;b<=w;b++)if(a*a+b*b<=w*w)set(x+a,y+b,c)}}
 function glyph(x,y,text,size,color){
   let xx=x;for(const ch of text.toUpperCase()){
     const rows=font[ch]||font[" "];for(let r=0;r<7;r++)for(let col=0;col<5;col++)if(rows[r][col]==="1")
       for(let dx=0;dx<size;dx++)for(let dy=0;dy<size;dy++)set(xx+col*size+dx,y+r*size+dy,color);
     xx+=size*6;
   }
 }
 function fillPoly(points,color){
  let minX=Math.max(0,Math.floor(Math.min(...points.map(p=>p[0])))),maxX=Math.min(W-1,Math.ceil(Math.max(...points.map(p=>p[0])))),
  minY=Math.max(0,Math.floor(Math.min(...points.map(p=>p[1])))),maxY=Math.min(H-1,Math.ceil(Math.max(...points.map(p=>p[1]))));
  for(let y=minY;y<=maxY;y++)for(let x=minX;x<=maxX;x++){
   let inside=false;for(let i=0,j=points.length-1;i<points.length;j=i++){
    const a=points[i],b=points[j];if((a[1]>y)!==(b[1]>y)&&x<(b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0])inside=!inside;
   }if(inside)set(x,y,color);
  }
 }
 for(let j=0;j<20;j++){
  const n=sheet*20+j,p=posters[j],x0=(j%COLS)*VW,y0=Math.floor(j/COLS)*VH,theme=n%5;
  const v=hash(n,31,412),warm=hash(n,51,143),base=[[24,35,46],[35,42,49],[27,29,33],[47,51,47],[46,35,43]][theme].map((q,k)=>q*(.71+.52*v)+[4,-3,6][k]*(warm-.5)*3);
  const accent=[[171,188,174],[200,175,113],[190,91,67],[215,186,83],[132,167,191]][theme].map((q,k)=>clamp(q*(.75+.45*warm)+[12,-8,14][k]*(v-.5)));
  const pale=[232,225,205];
  for(let v=0;v<VH;v++)for(let u=0;u<VW;u++){
    const grain=hash(u,v,n*13)-.5,edge=Math.min(u,VW-1-u,v,VH-1-v),fade=p.aging_grade*.023,
      pat=.4*Math.sin((u+n*17)*.04)+.25*Math.cos(v*.056+n),
      light=.73+.21*(v/VH)+.12*pat+grain*.09+(.14*fade)*Math.min(1,edge/20);
    const c=base.map((q,k)=>q*light+(pale[k]-q)*fade*.32);
    if(edge<4){c[0]+=12;c[1]+=12;c[2]+=12}
    if(hash(u+42,v+72,n*7)>.9992&&p.aging_grade>2){c[0]+=36;c[1]+=32;c[2]+=28}
    set(x0+u,y0+v,c);
  }
  const ox=x0,oy=y0;
  // Distinct fine pattern, stamps, linework and abrasion for every authored poster.
  for(let k=0;k<3+n%9;k++){
    const x=ox+15+Math.floor(hash(k,n,11)*130), y=oy+73+Math.floor(hash(n,k,17)*96),
       len=8+Math.floor(hash(k,n,93)*40),theta=hash(k,n,124)*6.28318;
    const ink=accent.map((c,j)=>clamp(c*(.37+.52*hash(n,j+k,78))));
    line(x,y,x+Math.cos(theta)*len,y+Math.sin(theta)*len,ink,k%7===0?2:1);
  }
  const sig=String((n*397+41)%10000).padStart(4,"0");
  glyph(ox+12,oy+VH-15,sig,1,accent);
  line(ox+11,oy+12,ox+VW-11,oy+12,accent,1);
  line(ox+11,oy+VH-35,ox+VW-11,oy+VH-35,accent,1);
  if(theme===0){
   // Original stylised profile of a forest wolf, with ear, muzzle and two forelegs.
   const wolf=[[21,152],[27,112],[47,97],[62,85],[71,58],[86,82],[103,73],
    [116,97],[140,103],[145,119],[133,129],[116,129],[109,155],[100,158],
    [96,140],[76,140],[68,169],[52,169],[48,141],[36,154]];
   fillPoly(wolf.map(([x,y])=>[ox+x,oy+y]),accent.map(v=>v*.78));
   fillPoly([[52,95],[67,69],[72,99]].map(([x,y])=>[ox+x,oy+y]),accent);
   set(ox+124,oy+109,pale);
   for(let k=0;k<9;k++){
    let x=17+k*16,y=155-(k%3)*10;
    line(ox+x,oy+y,ox+x-6,oy+y-23,[42,61,55],1);
   }
  }else if(theme===1){
   for(let k=0;k<5;k++){
    let t=k/4,x=ox+24+k*24,y=oy+55+k*16;
    line(x,oy+158,x+34,y,accent,k===0?3:1);
   }
   fillPoly([[36,139],[69,80],[86,105],[120,44],[120,162]].map(([x,y])=>[ox+x,oy+y]),accent.map(x=>x*.70));
  }else if(theme===2){
   const cx=ox+80,cy=oy+115;
   for(let k=0;k<6;k++){
    let radius=48-k*7;for(let a=0;a<36;a++){
      let th=a*2*Math.PI/36,nth=(a+1)*2*Math.PI/36;
      line(cx+Math.cos(th)*radius,cy+Math.sin(th)*radius,cx+Math.cos(nth)*radius,cy+Math.sin(nth)*radius,accent,1);
    }
   }
   fillPoly([[80,69],[119,138],[40,138]].map(([x,y])=>[ox+x,oy+y]),base.map(x=>x*.5));
  }else if(theme===3){
   for(let k=0;k<5;k++){
     const y=oy+60+k*22;
     line(ox+23,y,ox+134,y,accent,k%2?1:2);
   }
   fillPoly([[65,75],[97,75],[124,121],[37,121]].map(([x,y])=>[ox+x,oy+y]),accent.map(x=>x*.61));
  }else{
   for(let k=0;k<24;k++){
    let a=k*2.39996,r=11+4*k,x=ox+80+Math.cos(a)*r,y=oy+117+Math.sin(a)*r*.62;
    line(ox+80,oy+117,x,y,accent,k%5===0?2:1);
   }
  }
  const title=p.title.length>14?p.title.split(" "):[p.title];
  if(title.length>1){glyph(ox+15,oy+24,title[0],2,pale);glyph(ox+15,oy+42,title.slice(1).join(" ").slice(0,15),2,pale)}
  else glyph(ox+15,oy+27,p.title.slice(0,15),2,pale);
  glyph(ox+13,oy+VH-28,p.subtitle.slice(0,20),1,pale);
  glyph(ox+VW-39,oy+VH-16,String(n+1).padStart(3,"0"),1,accent);
 }
 const posterImage=png(pix,W,H),bufferBytes=[],views=[],accessors=[],meshes=[],nodes=[];let byteSize=0;
 function put(data,target){const p=(4-byteSize%4)%4;if(p){bufferBytes.push(new Uint8Array(p));byteSize+=p}const i=views.length;
  views.push({buffer:0,byteOffset:byteSize,byteLength:data.length,...(target?{target}:{})});
  bufferBytes.push(data);byteSize+=data.length;return i}
 function acc(raw,count,type,component,target,range=null){const v={bufferView:put(raw,target),componentType:component,count,type,...(range||{})};
  accessors.push(v);return accessors.length-1}
 const imView=put(posterImage);
 function surface(material,a,b,c,d,uv){
   const coords=[...a,...b,...c,...d],normal=[0,0,1,0,0,1,0,0,1,0,0,1],
     uv0=uv||[0,1,1,1,1,0,0,0],indices=[0,1,2,0,2,3];
   return {attributes:{POSITION:acc(f32(coords),4,"VEC3",5126,34962,{min:[Math.min(a[0],b[0]),Math.min(a[1],d[1]),a[2]],max:[Math.max(b[0],c[0]),Math.max(c[1],d[1]),a[2]]}),
   NORMAL:acc(f32(normal),4,"VEC3",5126,34962),TEXCOORD_0:acc(f32(uv0),4,"VEC2",5126,34962)},
   indices:acc(u16(indices),6,"SCALAR",5123,34963),material,mode:4};
 }
 for(let i=0;i<20;i++){
   const n=sheet*20+i,col=i%5,row=Math.floor(i/5),uv0=(col*VW+1)/W,uv1=((col+1)*VW-1)/W,
     vv0=(row*VH+1)/H,vv1=((row+1)*VH-1)/H,mesh=meshes.length;
   const paper=surface(0,[-.242,-.347,.019],[.242,-.347,.019],[.242,.347,.019],[-.242,.347,.019],[uv0,vv1,uv1,vv1,uv1,vv0,uv0,vv0]);
   const frame=surface(1,[-.27,-.38,.007],[.27,-.38,.007],[.27,.38,.007],[-.27,.38,.007]);
   meshes.push({name:posters[i].id,primitives:[frame,paper]});
   nodes.push({name:posters[i].id,mesh,extras:{variant_id:posters[i].id,poster_title:posters[i].title,select_single_node:true}});
 }
 const json={asset:{version:"2.0",generator:"OASIS original procedural poster designs"},
 scene:0,scenes:[{nodes:nodes.map((_,i)=>i)}],nodes,meshes,
 materials:[
 {name:"printed_paper",pbrMetallicRoughness:{baseColorTexture:{index:0},metallicFactor:0,roughnessFactor:.89},doubleSided:false},
 {name:"matte_frame",pbrMetallicRoughness:{baseColorFactor:[.115,.105,.098,1],metallicFactor:.1,roughnessFactor:.65},doubleSided:false}],
 images:[{name:"original_poster_atlas_"+sheet,mimeType:"image/png",bufferView:imView}],
 textures:[{source:0,sampler:0}],samplers:[{magFilter:9729,minFilter:9987,wrapS:33071,wrapT:33071}],
 accessors,bufferViews:views,buffers:[{byteLength:byteSize}],
 extras:{units:"meters",up:"Y",source_variants:20,category:"poster_dressing",requires_node_selection:true}};
 const js=pad(utf8(JSON.stringify(json)),32),bin=pad(cat(bufferBytes));
 const glb=cat([le(0x46546c67),le(2),le(12+8+js.length+8+bin.length),le(js.length),le(0x4e4f534a),js,le(bin.length),le(0x004e4942),bin]);
 return {glb,posters,embedded_png_bytes:posterImage.length,total_triangles:20*4,dimensions_px:[W,H]};
}
import {mkdirSync,writeFileSync} from "node:fs";
import {join as pathJoin} from "node:path";
const dir=process.argv[2]||"assets/packs/oasis_dressing_v1/geometry";
mkdirSync(dir,{recursive:true});
for(let i=0;i<5;i++){const data=makeSheet(i);writeFileSync(pathJoin(dir,"poster_sheet_0"+(i+1)+".glb"),data.glb);
 console.log(JSON.stringify({sheet:i+1,posters:data.posters.length,bytes:data.glb.length,triangles:data.total_triangles}));}
