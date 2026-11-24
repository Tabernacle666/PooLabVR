# One non-blocking work ticket after the ordinary successful build.
param([Parameter(Mandatory=$true)][string]$SourceRoot,
      [Parameter(Mandatory=$true)][string]$StageRoot,
      [Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
function Prop($obj,[string]$name){
    if($null -eq $obj){return $null}
    $p=$obj.PSObject.Properties | Where-Object {$_.Name -ceq $name} | Select-Object -First 1
    if($null -ne $p){return $p.Value}
    return $null
}
function Read-GlbJson([string]$path){
    $stream=[IO.File]::OpenRead($path)
    try{
        $reader=New-Object IO.BinaryReader($stream)
        if($reader.ReadUInt32() -ne 0x46546C67 -or $reader.ReadUInt32() -ne 2){throw 'GLB header invalid'}
        if([int64]$reader.ReadUInt32() -ne $stream.Length){throw 'GLB length mismatch'}
        $size=$reader.ReadUInt32()
        if($reader.ReadUInt32() -ne 0x4E4F534A -or $size -gt $stream.Length-20 -or $size -gt 67108864){throw 'GLB JSON chunk invalid'}
        $bytes=$reader.ReadBytes([int]$size)
        if($bytes.Length -ne $size){throw 'GLB JSON chunk truncated'}
        return ([Text.Encoding]::UTF8.GetString($bytes).TrimEnd([char]0) | ConvertFrom-Json)
    }finally{$stream.Dispose()}
}
$library=Get-Content -LiteralPath (Join-Path $SourceRoot 'materials/library.json') -Raw | ConvertFrom-Json
$bindings=Get-Content -LiteralPath (Join-Path $SourceRoot 'materials/bindings.json') -Raw | ConvertFrom-Json
$known=Prop $library 'materials'
$sources=Prop $bindings 'sources'
$issues=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
$names=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
$files=0;$primitives=0
foreach($root in @($SourceRoot,$StageRoot)){
    if(!(Test-Path -LiteralPath $root)){continue}
    foreach($file in Get-ChildItem -LiteralPath $root -Filter '*.glb' -File -Recurse){
        $files++
        [void]$names.Add($file.Name)
        $relative=$file.FullName.Substring($root.Length).TrimStart('\','/')
        $scope=if($root -eq $StageRoot){'stage'}else{'source'}
        $src=Prop $sources $file.Name
        try{
            $doc=Read-GlbJson $file.FullName
            $nodes=@(Prop $doc 'nodes')
            $meshes=@(Prop $doc 'meshes')
            $materials=@(Prop $doc 'materials')
            foreach($node in $nodes){
                $meshIndex=Prop $node 'mesh'
                if($null -eq $meshIndex){continue}
                if($meshIndex -lt 0 -or $meshIndex -ge $meshes.Count){throw 'node mesh index invalid'}
                $nodeName=[string](Prop $node 'name')
                foreach($primitive in @(Prop $meshes[[int]$meshIndex] 'primitives')){
                    $primitives++
                    $index=Prop $primitive 'material'
                    $slot='$default'
                    if($null -ne $index){
                        if($index -lt 0 -or $index -ge $materials.Count){throw 'primitive material index invalid'}
                        $name=[string](Prop $materials[[int]$index] 'name')
                        if($name){$slot=$name}
                    }
                    $id=Prop (Prop (Prop $src 'nodes') $nodeName) $slot
                    if(!$id){$id=Prop (Prop $src 'slots') $slot}
                    if(!$id){[void]$issues.Add("CLASSER | $($file.Name) | $nodeName | $slot")}
                    elseif($null -eq (Prop $known ([string]$id))){[void]$issues.Add("CORRIGER ID | $($file.Name) | $nodeName | $slot | $id")}
                }
            }
            foreach($image in @(Prop $doc 'images')){
                $uri=[string](Prop $image 'uri')
                if($uri -and !$uri.StartsWith('data:')){
                    if($uri.Contains('://') -or !(Test-Path -LiteralPath (Join-Path $file.DirectoryName ([Uri]::UnescapeDataString($uri))))){
                        [void]$issues.Add("TEXTURE ABSENTE | $scope/$relative | $uri")
                    }
                }
            }
        }catch{[void]$issues.Add("GLB A REPARER | $scope/$relative | $($_.Exception.Message)")}
    }
}
foreach($entry in $sources.PSObject.Properties){
    if($entry.Name.EndsWith('.glb',[StringComparison]::OrdinalIgnoreCase) -and !$names.Contains($entry.Name)){
        [void]$issues.Add("SOURCE NON DISPONIBLE | $($entry.Name)")
    }
}
# Report actual staged catalogue omissions as well as dormant source files.
foreach($name in @('oasis_logo.glb','navette_organes.glb','phosphor_orbit.glb','glove_left.glb','glove_right.glb','basalt_equipped.glb','scoria_equipped.glb')){
    if(!(Test-Path -LiteralPath (Join-Path $StageRoot $name))){[void]$issues.Add("ASSET NON STAGE | $name")}
}
$packRoot=Join-Path $StageRoot 'packs/oasis_mg2_v1'
$manifest=Join-Path $packRoot 'pack.json'
if(Test-Path -LiteralPath $manifest){
    $pack=Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    foreach($entry in @(Prop $pack 'assets')){
        $glb=[string](Prop $entry 'glb')
        $meta=[string](Prop $entry 'metadata');if(!$meta){$meta=[string](Prop $entry 'sidecar')}
        foreach($item in @($glb,$meta)){
            if($item -and !(Test-Path -LiteralPath (Join-Path $packRoot $item))){[void]$issues.Add("MG2 NON STAGE | $(Prop $entry 'asset_key') | $item")}
        }
    }
}
$revision=(& git rev-parse HEAD).Trim()
$lines=@('# OASIS - billet de travail materiaux post-compilation','',("Revision : "+$revision),("GLB inspectes (sources et stage) : "+$files),("Primitives inspectees : "+$primitives),("Anomalies distinctes : "+$issues.Count),'','Les lignes CLASSER ne sont pas des substitutions de materiau. Corriger la source ou sa declaration canonique. Les ressources indisponibles restent non bloquantes.','')
if($issues.Count){$lines+=@($issues | Sort-Object | ForEach-Object {'- '+$_})}
else{$lines+='Aucune anomalie de correspondance detectee dans les GLB inspectes.'}
$lines+=@('','La compilation ne valide pas le comportement ingame. Les anomalies de session seront consolidees apres le premier lancement.')
[IO.File]::WriteAllLines($OutputPath,[string[]]$lines,[Text.UTF8Encoding]::new($false))
Write-Host ("Billet materiaux : "+$OutputPath+" ("+$issues.Count+" anomalies)")
