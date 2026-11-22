# OASIS — Handoff lot 1 PBR CC0 (pipeline d'acquisition)

## Livré dans ce commit

- `tools/oasis_pbr_pilot.py` acquiert les cartes officielles CC0 à l'auteur de contenu, les convertit au contrat glTF et **remplace uniquement les matériaux nommés** de 11 GLB pilotes existants. Il conserve triangles, sockets, pivots, indices et objets de `runtime.json`.
- `pbr_lot1.bat` lance une intégration unique sur la machine Windows de production; l'exécution normale de `run.bat` ne dépend jamais d'Internet.
- Import sans nouvel allocateur/ressource au runtime : couleurs en sRGB, normales glTF `NormalGL`, ORM en linéaire (R AO, G roughness, B metallic), mips pris en charge par l'importeur existant. `KHR_texture_transform` est utilisé pour le répétition UV, et le shader OASIS le prend déjà en charge.
- Résolution de source par défaut **2K**. La géométrie actuelle reste la limite d'échelle physique des UV. L'optimisation de compression KTX2/BC5/BC7 reste une opération moteur/asset distincte, avant généralisation.

**État à la publication :** acquisition et GLB réécrits **non exécutés dans la présente session** (pas d'accès aux ZIP de textures ni à Windows/DX12 ici). Ce commit installe le pipeline exécutable, **pas de nouveaux pixels déjà visibles**. Les statuts `not_imported` du catalogue sont laissés exacts. Le script les met à jour seulement après acquisition, injection et réécriture.

## Sur la machine de production

1. Depuis la racine du dépôt, vérifier la disponibilité de Python 3 et Pillow (`python -m pip install Pillow` si absent).
2. Lancer `pbr_lot1.bat`. Le traitement va chercher les archives officielles sur ambientCG et les cartes Poly Haven, les réutiliser sous `assets/packs/oasis_mg2_v1/.pbr_cache/` (ignoré de Git), puis adapter les GLB et leurs sidecars.
3. Contrôle statique : `node tools/sync_oasis_mg2_catalog.mjs`. Contrôle in-game au seul moment où le matériau est visible : `commit_and_loop.bat "mg2"`, puis Quest 3S en mode normal.
4. Réviser le résultat, surtout les répétitions UV sur sol/cloison, intensité des normales, reflets, VRAM et colorimétrie. Un élément architectural non ciblé par `runtime.json` n'apparaîtra pas spontanément en jeu.
5. Après validation visuelle, committer **ensemble** les GLB modifiés, les `meta/`, `provenance/`, le manifeste `pack.json`, le catalogue sourcé et `pbr_acquisitions_lot1.json`, et non un commit par objet.

## Cibles matérielles du pilote

| Objets | Matériaux remplacés | Source CC0 |
|---|---|---|
| Bureau, table réunion, porte | bois oiled/light | Poly Haven `wood_table_worn` |
| Chaise ergonomique | fabric dark/blue | ambientCG `Fabric010` |
| Table, porte, lampe | steel brushed | ambientCG `Metal010` |
| Sol de référence | wood oiled; stone | ambientCG `WoodFloor003` ; `Concrete003` |
| Cloison | ceramic (usage dans ce GLB : finition murale) | ambientCG `PaintedPlaster017` |
| Lavabo, toilette, tasse | ceramic | ambientCG `Porcelain001` |
| Tapis lounge | fabric dark | ambientCG `Carpet001` |

**Distinction de provenance :** certains matériaux ambientCG ci-dessus sont procéduraux ou des approximations, même s'ils constituent de véritables cartes PBR externes. Ne pas les décrire comme photogrammétriques. `PaintedPlaster017` est une photogrammétrie de surface. Les sources restent CC0; l'usage du service API Poly Haven comporte un crédit visible « Powered by Poly Haven » dans la documentation/outillage. Pas de Fab/Mixamo dans ce lot.

## Dépendances moteur

- Cibles actuellement raccordées : `office_desktop`, `meeting_table`, `forest` seulement. Sol, cloison, sanitaire et portes nécessitent encore leurs cibles de placement et chargement en jeu; la retouche des GLB ne constitue pas leur activation runtime.
- **Pas d'interaction physique dans ce lot.** Proxies, saisie, pivots et haptique demeurent lot 4.
- Aucun nettoyage de textures procédurales préexistantes hors des GLB pilotes. Préserver les anciennes géométries, atlas, affiches et recettes.
- Les ressources PBR crues et les téléchargements restent dans le cache local; leur `sha256` et URL sont inscrits dans le manifeste généré.
