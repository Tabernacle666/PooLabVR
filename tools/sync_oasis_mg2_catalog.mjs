// Synchronise le manifeste de production sans toucher aux règles de jeu ni aux recettes.
// node tools/sync_oasis_mg2_catalog.mjs [--write]
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
const repo=join(dirname(fileURLToPath(import.meta.url)),"..");
const root=process.env.OASIS_MG2_PACK?resolve(process.env.OASIS_MG2_PACK):
  join(repo,"assets","packs","oasis_mg2_v1");
const path=join(root,"pack.json");
const pack=JSON.parse(readFileSync(path,"utf8"));
let files=0,triangles=0,changed=0;
const write=process.argv.includes("--write");
const revisionArg=process.argv.find(arg=>arg.startsWith("--revision="));
const newRevision=revisionArg?Number(revisionArg.slice("--revision=".length)):null;
if(newRevision!==null&&(!Number.isSafeInteger(newRevision)||newRevision<1))throw Error("Invalid --revision");
if(pack.schema_version!==1||!Array.isArray(pack.assets))throw Error("Invalid MG2 pack");
const runtimePath=join(root,"runtime.json");
if(existsSync(runtimePath)){
  const runtime=JSON.parse(readFileSync(runtimePath,"utf8"));
  const keys=new Set(pack.assets.map(entry=>entry.asset_key));
  const recipe=name=>{
    if(typeof name!=="string"||!/^recipes\/[\w.-]+\.json$/.test(name))throw Error("Invalid runtime recipe: "+name);
    return JSON.parse(readFileSync(join(root,name),"utf8"));
  };
  const placement=(item,position)=>{
    if(!keys.has(item.asset))throw Error("Unknown runtime asset: "+item.asset);
    if(position!==undefined&&(!Array.isArray(position)||position.length!==3||!position.every(Number.isFinite)))
      throw Error("Invalid runtime coordinates: "+item.asset);
  };
  if(runtime.schema_version!==1||!Array.isArray(runtime.surfaces)||!Array.isArray(runtime.forest))
    throw Error("Invalid runtime bindings");
  for(const binding of runtime.surfaces){
    if(!["office_desktop","meeting_table"].includes(binding.target))throw Error("Unknown runtime target: "+binding.target);
    const placements=binding.recipe
      ? recipe(binding.recipe).placements.filter(item=>item.socket===binding.socket)
      : binding.placements;
    if(!Array.isArray(placements)||!placements.length)throw Error("Empty runtime target: "+binding.target);
    for(const item of placements)placement(item,item.offset_m);
    for(const item of binding.additional??[]){
      placement(item,item.offset_m);
      if(!Array.isArray(item.offset_m)||item.offset_m.length!==3||
         !Number.isFinite(item.chance)||item.chance<0||item.chance>1)
        throw Error("Invalid optional worktop candidate: "+item.asset);
    }
  }
  if(runtime.room_profiles){
    if(typeof runtime.room_profiles!=="string"||!/^room_profiles_[\w.-]+\.json$/.test(runtime.room_profiles))
      throw Error("Invalid room profiles path");
    const layout=JSON.parse(readFileSync(join(root,runtime.room_profiles),"utf8"));
    if(layout.schema_version!==1||!Array.isArray(layout.profiles))throw Error("Invalid room profiles");
    const accepted=new Set(["organise","bazar","direction","creatif","technique","quasi_vide","abandonne","communs",
      "atelier_design","archives","startup","comptabilite","consultation","informatique","production","accueil"]);
    const seen=new Set();
    for(const profile of layout.profiles){
      if(!accepted.has(profile.id)||seen.has(profile.id)||!Array.isArray(profile.placements))
        throw Error("Invalid room profile id: "+profile.id);
      seen.add(profile.id);
      for(const p of profile.placements){
        placement(p);
        const v=p.at_normalized;
        if(!Array.isArray(v)||v.length!==2||!v.every(x=>Number.isFinite(x)&&Math.abs(x)<=1)||
           !Number.isFinite(p.chance)||p.chance<0||p.chance>1)
          throw Error("Invalid room placement: "+p.asset);
      }
    }
  }
  for(const row of runtime.forest){
    recipe(row.recipe);
    for(const item of row.additional??[]){
      if(!item.at_m)throw Error("Missing forest position: "+item.asset);
      placement(item,item.at_m);
      if(item.jitter_m!==undefined&&(!Number.isFinite(item.jitter_m)||item.jitter_m<0))
        throw Error("Invalid forest jitter: "+item.asset);
    }
  }
}
for(const entry of pack.assets){
  const b=readFileSync(join(root,entry.glb));
  if(b.length<28||b.readUInt32LE(0)!==0x46546c67||b.readUInt32LE(4)!==2||b.readUInt32LE(8)!==b.length)throw Error("Invalid GLB "+entry.glb);
  const json=JSON.parse(b.subarray(20,20+b.readUInt32LE(12)).toString("utf8").trim());
  const count=(json.meshes||[]).flatMap(m=>m.primitives).reduce((n,p)=>n+(json.accessors[p.indices]?.count||0)/3,0);
  if(!Number.isInteger(count)||!count)throw Error("Invalid triangles "+entry.glb);
  const metaPath=join(root,entry.metadata||entry.sidecar),meta=JSON.parse(readFileSync(metaPath,"utf8"));
  if(meta.asset_key!==entry.asset_key||meta.asset_revision!==entry.asset_revision)throw Error("Key/revision mismatch "+entry.asset_key);
  const next={...meta.geometry,triangles:count,bytes:b.length,embedded_textures:(json.images||[]).length,material_slots:(json.materials||[]).length};
  const digest=createHash("sha1").update("blob "+b.length+"\0").update(b).digest("hex");
  if(JSON.stringify(next)!==JSON.stringify(meta.geometry)||digest!==entry.git_blob_sha1){
    changed++;
    if(write){
      if(digest!==entry.git_blob_sha1&&newRevision===null)throw Error("Changed GLB requires --revision=N: "+entry.asset_key);
      if(digest!==entry.git_blob_sha1){entry.asset_revision=newRevision;meta.asset_revision=newRevision;}
      meta.geometry=next;entry.git_blob_sha1=digest;
      writeFileSync(metaPath,JSON.stringify(meta,null,2)+"\n");
    }
  }
  files++;triangles+=count;
}
if(write){if(newRevision!==null&&changed)pack.pack_revision=Math.max(pack.pack_revision,newRevision);writeFileSync(path,JSON.stringify(pack,null,2)+"\n");}
console.log(JSON.stringify({assets:files,triangles,needs_sync:changed,written:write}));
