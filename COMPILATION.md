# Carnet de compilation OASIS

À consulter avant de modifier les types CPU/GPU, les appels au SDK Windows ou les shaders. Pour chaque nouveau blocage, ajouter le premier diagnostic, sa cause, le correctif minimal et une vérification qui aurait détecté l’oubli.

| Diagnostic observé | Cause | Correctif et vérification préventive |
| --- | --- | --- |
| MSVC `C2664` sur `XMLoadFloat4`, suivi de `C2660` sur `XMStoreFloat4`, dans `GraphicsRender.cpp` | `GpuInstance::rotation` est un `GpuFloat4` du tampon GPU. Les fonctions DirectXMath prennent des pointeurs vers `DirectX::XMFLOAT4`. | Copier les quatre composantes dans un `XMFLOAT4`, appliquer DirectXMath, puis recopier le résultat. Garder la disposition de `GpuInstance` intacte. Compiler le passage avec les vrais en-têtes `GpuMeshTypes.h` et DirectXMath pour vérifier la conversion. [Révision du blocage](https://github.com/Tabernacle666/PooLabVR/commit/911f138872707082efaaf9c185d642e5f676b965) |
| MSVC `C3861` sur `XAudio2CutoffFrequencyToRadians` dans `ShuttleAudio.cpp` | Les fonctions auxiliaires XAudio2 sont déclarées quand `XAUDIO2_HELPER_FUNCTIONS` est défini avant la première inclusion de `xaudio2.h` dans l’unité de compilation. | Reprendre le motif de `LunarBattleAudio.cpp` dans l’unité qui appelle le helper. Un substitut local de l’en-tête qui expose le helper en permanence masque cet oubli. Vérifier avec le SDK Windows ou avec un substitut qui respecte la même condition. [Correctif](https://github.com/Tabernacle666/PooLabVR/commit/34adc9cd222581a6133e7a8c4773d2468d30d823) |
| DXC `no member named 'visualData' in 'CutInstance'` dans `impact_shared.hlsli` | Le constructeur d’impact manipule le format compact `CutInstance`. Le nouvel entier visuel appartient à `GpuInstance`. | Garder `CutInstance` et son initialisation intacts. Compiler `StepProjectiles` en plus de `MeshMain` et des passes de visibilité lors d’un changement d’instance. |
| DXC `conditional operator only supports results with numeric scalar, vector, or matrix types` dans `StepProjectiles` | Une branche du chemin de secours sélectionnait un `CutTriangle` avec le ternaire HLSL. | Charger le struct avec une branche explicite. Compiler les points d’entrée de `projectiles.hlsl` avec DXC. |
| DXC `use of undeclared identifier distance` dans la section de paroi laser | Le calcul des distances signées a été déplacé dans un helper qui ne déclarait plus cet alias de la distance au plan. | Lire `plane.w` directement et compiler `GatherLaserSections` et les points d’entrée partageant `cut_mesh.hlsl`. |
| MSVC `C2678` dans `GraphicsSphereLod.cpp` lors du passage à un temps de vol par corps | Les deux chemins de choix du niveau de détail comparaient encore le membre entier à `0.f`. | Indexer par le corps évalué ou le corps natif après contrôle de sa borne, et prendre en compte les impacts à vider. Rechercher tous les lecteurs du membre puis compiler `Core` avec MSVC. |

| DXC `modifiers must appear before type` dans `JointMesh` | `line` est un mot réservé HLSL utilisé pour les primitives. | Utiliser `intensity` et compiler `PhosphorOrbitPixel` avec les trois entrées de la console. |
| DXC `modifiers must appear before type` dans `RoomLightReaches` | `point` désigne une primitive HLSL. | Nommer le paramètre `position` et compiler les passes de scène qui incluent `common.hlsli`. |
| MSVC `C2064` sur `useTexture` dans `GraphicsRender.cpp` | La lecture de l’atlas de normales omettait le paramètre de disposition D3D12. | Déclarer la disposition shader resource avec la même fonction pour les atlas de couleur et de normales. Compiler `Core` avec MSVC pour contrôler la signature des appels au graphe. |
| Erreur de syntaxe sur les champs `near` et `far` du parcours Relief | Les macros historiques de `minwindef.h` effacent ces deux identifiants. | Nommer les champs `nearLevel` et `farLevel`. Compiler avec les en-têtes Windows, ou reproduire les deux définitions de macros dans une vérification locale. |

| DXC `expected member name` dans le résultat d’un rayon de coupe | `triangle` est réservé pour les paramètres de primitive HLSL. | Nommer le membre `triangleIndex` et compiler `PixelMain` avec les permutations de matériau. |

| DXC `use of undeclared identifier ProjectRoomThroughPane` | La fonction de projection se trouvait dans la branche du shader CRT. | Placer la fonction dans le chemin de scène standard et compiler `PixelMain` pour les variantes générique et CRT. |
| MSVC `C2660` sur `Mg2Worktop::Fit` dans `BuildingSession.cpp` | La variation déterministe des accessoires a ajouté une clé d’hôte obligatoire. Le choix du bureau au démarrage appelait encore la version à trois arguments. | Calculer les dimensions et la clé d’hôte au même endroit pour le point de départ et la publication du mobilier. Vérifier l’unité `BuildingSession.cpp` avec la déclaration réelle de `Mg2Worktop::Fit`. |

## Vérification ciblée

- Lire la première erreur de l’unité de compilation concernée et les notes de types qui la suivent. Corriger cette cause avant de traiter les diagnostics suivants.
- Pour du C++ Windows ou DX12, privilégier `build.bat` sur Windows. Si seul un contrôle local avec des en-têtes partiels est possible, indiquer précisément les unités contrôlées et réserver la confirmation du build complet à MSVC.
- Pour du HLSL, compiler les points d’entrée et les variantes touchés avec DXC. Indiquer séparément le résultat des shaders et celui des unités C++.

## Parcelle de forêt paginée

Le build standard utilise Agility 1.619.6. La parcelle de devant emploie le sapin `tree_fir_hero01` du pack. Les deux yeux déterminent ensemble la sélection dans la hiérarchie de clusters. Un worker décode la nouvelle sélection. Les passes de profondeur, d’ombre et de couleur utilisent les mêmes pages que les BLAS standard. Les pages deviennent visibles à la fin du transfert GPU.

Pour construire de vrais CLAS sur l’adaptateur OpenXR et les utiliser dans le TLAS du jeu, configurez Agility 1.722.1. Cette préversion demande un pilote compatible et le Mode développeur Windows. Les shaders SM 6.6 de l’application optent pour les clusters dans leur PSO. Le chemin standard prend le relais si la capacité est absente ou si la publication clusterisée est refusée. [Préversion DXR](https://devblogs.microsoft.com/directx/clas-ptlas-preview/) · [Exemple NVIDIA](https://github.com/NVIDIA-RTX/RTXMG#dxr-clusters)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOASIS_MG2_PREVIEW=ON
commit_and_loop.bat "mg2"
```

La ligne `mg2_foret_pilote` situe la parcelle. La ligne `mg2_foret_live` compte les images qui ont lié les pages au raster, celles dont le TLAS référence les BLAS paginés, et celles qui utilisent des CLAS. `clusters_soumis_pic` est le nombre maximal de groupes de mesh shaders paginés dans les commandes opaques effectivement retenues par le GPU parmi les images échantillonnées. `octets_geometrie` mesure le tampon de la sélection, hors structures de rayons. Une nouvelle coupe de la hiérarchie transfère son propre tampon. Les coupes précédentes restent vivantes le temps des lectures déjà soumises.

Sur le parcours VR, les fenêtres `mg2` donnent séparément `tlas_build_p95_ms` et `tlas_update_p95_ms`. La valeur `-1` indique qu’aucune opération de ce type n’a été mesurée dans la fenêtre. Ces mesures servent à décider si un essai PTLAS est justifié.

Pour revenir au build standard, réutilisez la commande CMake avec `-DOASIS_MG2_PREVIEW=OFF`.
