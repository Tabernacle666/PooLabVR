// Synchronise le manifeste de production sans toucher aux règles de jeu ni aux recettes.
// node tools/sync_oasis_mg2_catalog.mjs [--write]
import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
const repo=join(dirname(fileURLToPath(import.meta.url)),"..");
const root=join(repo,"assets","packs","oasis_mg2_v1");
const path=join(root,"pack.json");
const pack=JSON.parse(readFileSync(path,"utf8"));
let files=0,triangles=0,changed=0;
if(pack.schema_version!==1||!Array.isArray(pack.assets))throw Error("Invalid MG2 pack");
for(const entry of pack.assets){
  const b=readFileSync(join(root,entry.glb));
  if(b.length<28||b.readUInt32LE(0)!==0x46546c67||b.readUInt32LE(4)!==2||b.readUInt32LE(8)!==b.length)throw Error("Invalid GLB "+entry.glb);
  const json=JSON.parse(b.subarray(20,20+b.readUInt32LE(12)).toString("utf8").trim());
  const count=(json.meshes||[]).flatMap(m=>m.primitives).reduce((n,p)=>n+(json.accessors[p.indices]?.count||0)/3,0);
  if(!Number.isInteger(count)||!count)throw Error("Invalid triangles "+entry.glb);
  const metaPath=join(root,entry.metadata||entry.sidecar),meta=JSON.parse(readFileSync(metaPath,"utf8"));
  if(meta.asset_key!==entry.asset_key||meta.asset_revision!==entry.asset_revision)throw Error("Key/revision mismatch "+entry.asset_key);
  const next={...meta.geometry,triangles:count,bytes:b.length,embedded_textures:(json.images||[]).length,material_slots:(json.materials||[]).length};
  const digest=createHash("sha1").update("blob "+b.length+"\\0").update(b).digest("hex");
  if(JSON.stringify(next)!==JSON.stringify(meta.geometry)||digest!==entry.git_blob_sha1){
    changed++;
    if(process.argv.includes("--write")){
      meta.geometry=next;entry.git_blob_sha1=digest;
      writeFileSync(metaPath,JSON.stringify(meta,null,2)+"\\n");
    }
  }
  files++;triangles+=count;
}
if(process.argv.includes("--write"))writeFileSync(path,JSON.stringify(pack,null,2)+"\\n");
console.log(JSON.stringify({assets:files,triangles,needs_sync:changed,written:process.argv.includes("--write")}));
