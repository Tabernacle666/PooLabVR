#!/usr/bin/env node
// Repair binary GLB payloads in a staged MG2 pack, NEVER in git-tracked source.
// GitHub previously stored some binary GLBs with expanded UTF-8 bytes; their
// header-declared length no longer matched the file size. This rebuilds exactly
// the four authored hero assets from the repository's source geometry code.
import {readFileSync,writeFileSync,mkdirSync,existsSync} from "node:fs";
import {resolve,join,dirname} from "node:path";
import {createHash} from "node:crypto";
import {makeAsset,buildGLB} from "./generate_oasis_mg2_assets.mjs";

const root=resolve(process.argv[2]||"");
if(!process.argv[2])throw Error("Usage: node tools/mg2_hero_repair.mjs <staged-pack-path>");
const packPath=join(root,"pack.json");
const pack=JSON.parse(readFileSync(packPath,"utf8"));
const heroes=["fire_extinguisher01","light_panel01","door_office_solid01","desk_office01"];
const key="MG2 hero GLB";
const gitSha=b=>createHash("sha1").update("blob "+b.length+"\0").update(b).digest("hex");
function valid(raw){
  if(raw.length<28||raw.toString("ascii",0,4)!=="glTF"||
     raw.readUInt32LE(4)!==2||raw.readUInt32LE(8)!==raw.length)return false;
  let offset=12;
  for(const type of [0x4E4F534A,0x004E4942]){
    if(offset+8>raw.length)return false;
    const length=raw.readUInt32LE(offset),kind=raw.readUInt32LE(offset+4);
    if(kind!==type||length%4||offset+8+length>raw.length)return false;
    offset+=8+length;
  }
  return offset===raw.length;
}
let repaired=0;
for(const id of heroes){
  const entry=pack.assets.find(row=>row.glb==="geometry/"+id+".glb");
  if(!entry)throw Error(key+": manifest lacks "+id);
  const path=join(root,entry.glb);
  const existing=existsSync(path)?readFileSync(path):Buffer.alloc(0);
  if(valid(existing))continue; // retain correctly packed PBR acquired overlays
  const generated=buildGLB(makeAsset(id));
  const data=Buffer.from(generated.glb);
  if(!valid(data))throw Error(key+": generator produced invalid binary for "+id);
  const metaPath=join(root,entry.metadata);
  const meta=JSON.parse(readFileSync(metaPath,"utf8"));
  const provenancePath=join(root,entry.provenance);
  const provenance=JSON.parse(readFileSync(provenancePath,"utf8"));
  const docLength=data.readUInt32LE(12);
  const doc=JSON.parse(data.subarray(20,20+docLength).toString("utf8").trim());
  const sha=gitSha(data);
  mkdirSync(dirname(path),{recursive:true});
  writeFileSync(path,data);
  entry.git_blob_sha1=sha;
  meta.geometry={...meta.geometry,triangles:generated.triangles,
    bytes:data.length,embedded_textures:(doc.images||[]).length,
    material_slots:(doc.materials||[]).length};
  meta.geometry.triangles=generated.triangles;
  if("hero_geometry_git_blob_sha1" in provenance)
    provenance.hero_geometry_git_blob_sha1=sha;
  writeFileSync(metaPath,JSON.stringify(meta,null,2)+"\n");
  writeFileSync(provenancePath,JSON.stringify(provenance,null,2)+"\n");
  ++repaired;
  console.log("[MG2 HERO] Rebuilt",id,"triangles",generated.triangles,"bytes",data.length);
}
if(repaired)writeFileSync(packPath,JSON.stringify(pack,null,2)+"\n");
for(const row of pack.assets){
  const path=join(root,row.glb);
  const raw=readFileSync(path);
  if(!valid(raw))
    throw Error("[MG2] Invalid source GLB: "+row.glb+" (bytes "+raw.length+
      ", declared "+(raw.length>=12?raw.readUInt32LE(8):"missing")+")");
}
console.log("[MG2 HERO] Source pack validated:",pack.assets.length,
  "GLBs; rebuilt",repaired,"hero GLBs. Git source untouched.");
