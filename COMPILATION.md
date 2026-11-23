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

| C++ `center was not declared` dans `StreamWorktops` | Le retrait du rayon de mobilier avait supprimé le point également utilisé pour placer les accessoires. | Recalculer la position depuis le support au moment de la publication et compiler `BuildingSession.cpp`. |

## Vérification ciblée

- Lire la première erreur de l’unité de compilation concernée et les notes de types qui la suivent. Corriger cette cause avant de traiter les diagnostics suivants.
- Pour du C++ Windows ou DX12, privilégier `build.bat` sur Windows. Si seul un contrôle local avec des en-têtes partiels est possible, indiquer précisément les unités contrôlées et réserver la confirmation du build complet à MSVC.
- Pour du HLSL, compiler les points d’entrée et les variantes touchés avec DXC. Indiquer séparément le résultat des shaders et celui des unités C++.

## Parcelle de forêt paginée

Le build standard utilise Agility 1.619.6. La parcelle de devant emploie le sapin `tree_fir_hero01` du pack. Les deux yeux déterminent ensemble la sélection dans la hiérarchie de clusters. Un worker décode la nouvelle sélection. Les passes de profondeur, d’ombre et de couleur utilisent les mêmes pages que les BLAS. Les pages et les BLAS deviennent utilisables ensemble à la fin du transfert GPU.

Sur le build standard, `commit_and_loop.bat "mg2"` interroge le pilote NVAPI de l’adaptateur OpenXR. Si le pilote expose les opérations clusterisées, OASIS construit un lot de CLAS, les assemble en BLAS et publie leurs adresses pour le TLAS. Le moteur autorise la traversée clusterisée à la création des PSO et utilise une clé de cache distincte. DXR standard sert de repli si la capacité est absente ou si la publication clusterisée est refusée. [SDK NVIDIA](https://github.com/NVIDIA/nvapi) · [Exemple RTX Mega Geometry](https://github.com/NVIDIA-RTX/RTXMG#dxr-clusters)

La commande suivante demande DXR standard avec la même scène et les mêmes pages. Remettez la variable à vide pour revenir à la sélection automatique.

```bat
set "OASIS_MG2_CLUSTER_API=standard"
commit_and_loop.bat "mg2"
set "OASIS_MG2_CLUSTER_API="
```

Agility 1.722.1 est une préversion optionnelle pour utiliser les appels DXR natifs sur un pilote compatible avec le Mode développeur Windows. Dans ce build, le diagnostic choisit DXR natif en premier quand la capacité est présente. `OASIS_MG2_CLUSTER_API=nvapi` ou `dxr` sélectionne explicitement l’API clusterisée à essayer. [Préversion DXR](https://devblogs.microsoft.com/directx/clas-ptlas-preview/)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOASIS_MG2_PREVIEW=ON
commit_and_loop.bat "mg2"
```

La ligne `mg2_cluster_gpu` indique l’API sélectionnée ou la raison du repli. `mg2_foret_pilote` situe la parcelle. Dans `mg2_foret_live`, `routes_tlas_ok` compte les descripteurs d’instance échantillonnés après la construction du TLAS qui référencent un lot clusterisé valide. `clas_ms` et `blas_ms` donnent les temps GPU du dernier lot publié. `clusters_soumis_pic` donne le maximum de groupes de mesh shaders paginés effectivement retenus dans les commandes opaques échantillonnées. `octets_geometrie` mesure le tampon de la sélection, hors structures de rayons. Chaque nouvelle coupe transfère son propre tampon. Les coupes précédentes restent vivantes le temps des lectures déjà soumises.

Sur le parcours VR, les fenêtres `mg2` donnent séparément `tlas_build_p95_ms` et `tlas_update_p95_ms`. La valeur `-1` indique qu’aucune opération de ce type n’a été mesurée dans la fenêtre. Ces mesures servent à décider si un essai PTLAS est justifié.

Pour revenir au build standard, réutilisez la commande CMake avec `-DOASIS_MG2_PREVIEW=OFF`.
