# OASIS — Plan directeur LOD / géométrie territoriale

**Révision du plan : 27 novembre 2026. Prochain billet à exécuter : LOD-23.**

Ce fichier est le document de dispatch du chantier. Il doit suffire à un agent sans historique de conversation. Le code et les journaux de main sont la source de vérité technique ; le plan fixe les objectifs, l'ordre et le périmètre, pas une liste d'implémentations intangibles.

## 1. Contexte et mandat global

OASIS est un moteur DX12 / OpenXR VR. Cible matérielle : RTX 5060 Ti 16 Gio, 64 Gio DDR4, Quest 3S à 72 Hz. L'objectif est une qualité visuelle AAA et un rendu stable à 72 Hz (13,89 ms/image), sans verrouillage volontaire à 36 FPS. Le chantier MG2-A01 à A21 a établi une infrastructure fonctionnelle RTX Mega Geometry : simplification, sélection, résidence, clusters/CLAS, BLAS/PTLAS, publication et accélération. Ne pas recommencer ce chantier ; le prolonger et corriger ce qui empêche son exploitation.

Constat ayant motivé le plan : à 29 extents, 319 168 nouvelles instances de végétation basse, 511 085 instances MG2, 5,37 millions de nœuds visités et environ 23 Mo de readback pour seulement 1 360 variantes géométriques sélectionnées ; le nombre de clusters uniques variait peu. MG2 mutualise les représentations, mais l'engin parcourt encore individuellement une population croissante.

Objectif architectural : passer de la représentation de l'objet à une hiérarchie du territoire, dont la complexité active suit le contenu visible et optiquement pertinent plutôt que la population logique totale. Distinguer strictement :
- existence logique (identités, recettes, simulation, collisions, destruction) ;
- résidence des sources et représentations en DDR4/VRAM ;
- sélection de la représentation utile à une vue ou à un domaine de rayons ;
- publication et participation effective au rendu et aux structures d'accélération.

Un objet masqué ou distant peut demeurer résident sans être calculé à son niveau détaillé. Ne pas diminuer la population, supprimer un effet, introduire de plafond VRAM arbitraire ni raccourcir artificiellement les distances pour gagner des FPS. Les approximations géométriques et optiques sont autorisées aux échelles où leur erreur est acceptable. Préserver silhouette, couverture, densité, couleurs, matériaux, hauteurs, distribution spatiale, transmission lumineuse et qualité de proximité ; maintenir la croissance procédurale, le cache persistant et les révisions/destructions locales.

Référentiel : [NVIDIA RTX Mega Geometry](https://github.com/NVIDIA-RTX/RTXMG), hiérarchies de clusters, réutilisation des CLAS, construction GPU des accélérations ; [DX Clusters / PTLAS](https://github.com/NVIDIA-RTX/Whitepapers/tree/main). Les exemples NVIDIA ne constituent pas une implémentation prête à l'emploi de sélection territoriale OASIS. Préférer des représentations triangulaires compatibles MG2/OMM, ou une solution mieux justifiée par le matériel et l'erreur optique ; ne pas introduire un moteur d'intersection voxel indépendant par défaut.

## 2. Ordre de dispatch et statut

| Ordre | Billet | Statut | Objet / référence |
|---:|---|---|---|
| Prérequis | MG2-A01 à A21 | INTÉGRÉ | Infrastructure MG2 antérieure au plan |
| 0 | LOD-00 | INTÉGRÉ, benchmark exécuté | Baseline vanilla du chantier LOD : bcb2df1972a1f78ee69b16e48c19ba6424eb49f1 |
| 1a | LOD-21A | INTÉGRÉ, qualification optique insuffisante | Colonies/couvertures basses : f03cb9ed01a7a990214994cc4fb40197e499f8b4 |
| 1b | LOD-21B | INTÉGRÉ, qualification optique insuffisante | Groupes/massifs forestiers : 97b91146b80862f15786d38372463dadf55a05c4 |
| 2 | **LOD-23** | **NON INTÉGRÉ — PROCHAIN** | Production corrigée ET exploitation territoriale de toute la végétation |
| 3 | LOD-22 | NON INTÉGRÉ | Patrimoine architectural, mobilier et composites |
| 4 | LOD-31 | NON INTÉGRÉ | Généralisation de la sélection territoriale GPU |
| 5 | LOD-32 | NON INTÉGRÉ | Généralisation de la publication raster / RTX |
| 6 | LOD-41 | NON INTÉGRÉ | Élimination des parcours historiques |
| 7 | LOD-42 | NON INTÉGRÉ | Optique, accélération et résidence mémoire |
| 8 | LOD-43 | NON INTÉGRÉ | Qualification finale de scalabilité ; commit seulement si correction nécessaire |

LOD-23 absorbe LOD-21A.1, LOD-31A, LOD-21B.1 et LOD-31B. Ces quatre identifiants ne sont plus des billets à dispatcher ni des commits distincts. Le séquencement initial par phases horizontales est révisé : terminer verticalement la végétation avant l'architecture. LOD-31/32/41/42 ne doivent pas refaire ce que LOD-23 aura réellement achevé ; ils généralisent les mécanismes et traitent les coûts résiduels.

## 3. Références et diagnostic reproductible (LOD-00 — intégré)

Commande : `pull_compile_run.bat lod` (ou `run.bat lod` si la version est déjà compilée).

- [Baseline LOD-00, journal](diagnostics/sessions/20261127-230703-911-lod-35088/OASIS.log) et [session](diagnostics/sessions/20261127-230703-911-lod-35088/session.json). Révision exécutée bcb2df1972a1f78ee69b16e48c19ba6424eb49f1, checkout propre, sortie 0.
- [Après LOD-21A/B, journal](diagnostics/sessions/20261127-233734-121-lod-29948/OASIS.log) et [session](diagnostics/sessions/20261127-233734-121-lod-29948/session.json). Révision exécutée 97b91146b80862f15786d38372463dadf55a05c4, checkout propre, résultat terminé. Journal ajouté à main par d3140d1f4dfd077e7ebb639dbb081fb2667c4c27.

Benchmark : 20 extents effectivement publiés avant préchauffage ; 6 s de préchauffage puis cinq blocs de 16 s, quatre segments de 4 s, durée nominale totale 86 s (objectif <= 90 s hors démarrage et blocages). Vue aérienne basse dirigée vers le logo, depuis le bord du continent, progression à 5 km/h dans les fenêtres de marche, sauts déterministes entre fenêtres pour couvrir la distance, dernière fenêtre en pause devant le logo. Même parcours pour les cinq blocs : complet, logo seul, continent seul, arbres seuls, végétation basse seule. Masques raster/MG2/rayons sans retirer les sources de la résidence ; lumière inchangée, slots RTX conservés. Exclure ou identifier les transitions de masque/pose lors des comparaisons. Journal en RAM, pas de capture d'écran ni écriture pendant la boucle de mesure. Garder les réglages comparables (soleil, DLSS, MSAA, résolution effective, pose, extent).

Points d'entrée : src/app/SessionLaunch.h, AppSession.cpp, VrInputMapping.*, main.cpp, tools/RunSession.ps1 et scripts BAT existants ; vérifier leurs chemins actuels.

## 4. LOD-21A et LOD-21B — état réel, dette à résoudre dans LOD-23

Code intégré : src/core/TerritorialPatrimony.h, WorldRegistry.h/.cpp, src/app/TerrainExpansion.h. Deux patrimoines CPU distincts ; cellules 10 m, parents 40 m puis 160 m ; sources enregistrées depuis les géométries existantes, surfaces directionnelles triangulaires, regroupements par matériau/type/orientation, conservation des identités feuilles, parenté spatiale. Cache persistant des sources et produits de placement, préparation de l'extension dans le worker, publication des branches modifiées et invalidation locale. Bois et feuillage séparés ; LOD-21B réemploie les coupes CanopyLodNode existantes et mesure l'alpha des deux moitiés de l'atlas de sprig. Aucun agrégat n'est encore inscrit dans le rendu, dans MG2 ou dans l'accélération RTX. Le code a compilé et le benchmark a terminé ; cela ne valide pas la fidélité des proxies.

### Diagnostic à 20 extents

Images soumises par segment nominal de quatre secondes (pas des FPS mesurés ; les blocages GPU peuvent décaler ou réduire les fenêtres) :

| Scène | Bord | Forêt | Approche | Pause |
|---|---:|---:|---:|---:|
| Complète | 14 | 38 | 130 | 169 |
| Logo seul | 227 | 113 | 239 | 288 |
| Continent seul | 270 | 246 | 249 | 288 |
| Arbres seuls | 51 | 72 | 110 | 123 |
| Végétation basse seule | 3 | 1 | 1 | 62 |

Bloc végétation, image 2689 : direct GPU 3 672,58 ms, Opaque.Resident 2 231,43 ms, Depth 1 426,40 ms ; 27 564 098 meshlets natifs en depth, 0 meshlet MG2. La sélection de ce domaine atteint 357,47 ms (prism_work=2690), registre 197 601, nœuds visités 1 605 457, readback 10 843 832 octets (lod_mg2=2691). Les numéros d'image de publication/sélection diffèrent : ne pas les assimiler à un unique échantillon GPU. La piste prioritaire est le chemin natif lors de l'attente d'une publication MG2 exploitable ; vérifier les causes exactes dans le code.

Patrimoine généré (lignes 206–211 du journal) :

| Domaine | Échelle | Triangles sources couverts | Triangles proxy | Surface proxy / source | Erreur géométrique max (unités scène) |
|---|---:|---:|---:|---:|---:|
| Végétation | 10 m | 1 638 079 240 | 681 825 | 13,79 % | 6,82 |
| Végétation | 40 m | 1 671 549 410 | 113 457 | 7,20 % | 16,90 |
| Végétation | 160 m | 1 750 852 702 | 9 045 | 0,86 % | 59,49 |
| Forêt | 10 m | 167 082 236 | 1 128 002 | 47,12 % | 34,30 |
| Forêt | 40 m | 171 640 792 | 91 612 | 50,61 % | 45,65 |
| Forêt | 160 m | 181 591 294 | 4 730 | 19,33 % | 89,33 |

197 601 routes de végétation et 22 500 routes d'arbres au total ; les lignes par niveau décrivent les nœuds modifiés et n'ont donc pas exactement le même ensemble couvert. Les triangles sources sont des sommes instanciées, pas des triangles uniques. Le rapport de surface est un indicateur de surface géométrique pondérée par alpha, pas la couverture visible, ni une preuve de transmission. Pour la forêt, la mesure est calculée sur les coupes de référence déjà simplifiées (40 433 426 triangles de référence au niveau 160 m) et non directement sur toute la géométrie originale. Ces ratios signalent une compression excessive et nécessitent une qualification optique plus robuste, pas une obligation de conserver toute la surface superposée.

Le producteur garde de la densité dans ses métadonnées, mais Fit limite la taille du triangle représentant à son support spatial. L'erreur peut rester trop élevée pour sélectionner ces proxies aux distances utiles. Les changements de source invalident des branches sans reconstruire les agrégats affectés. LOD-23 doit corriger ces lacunes et ne doit pas simplement activer les proxies existants.

## 5. LOD-23 — Intégration territoriale complète de la végétation

**Statut : NON INTÉGRÉ. Un seul billet, un seul commit substantiel, une seule version testable ; quatre opérations historiques absorbées.** L'agent choisit l'ordre de ses opérations internes, examine main et l'amont/aval, et tranche en fonction du comportement réel sur RTX, sans ajouter des sous-billets de dispatch.

### Production géométrique

1. Végétation basse en premier : plante/patch → colonie → couverture → secteur. Exploiter les sources existantes de 44 espèces, variantes, herbes, fougères, arbustes, mousses et bois mort ; ne pas supprimer d'espèces, de population ou de hauteur. Traiter les géométries discontinues comme une distribution volumétrique de surfaces, avec orientation, hauteur, densité, couleur et matériau. Revoir le proxy actuel et sa compression : couches/patchs directionnels, représentations de couverture, géométrie simplifiée ou autre solution techniquement justifiée. Produire de véritables triangles de remplacement compatibles MG2, avec une densité optique rendue et pas uniquement mémorisée en métadonnées.
2. Forêt : arbre → groupe → massif → secteur. Réutiliser les hiérarchies MG2, CanopyAssembly et les coupes structurelles valides ; traiter séparément troncs/branches et feuillage OMM. Préserver silhouette, couvert, variations de hauteur et couleur, transmission et vides.
3. Compléter les niveaux intermédiaires utiles ; les dimensions 10/40/160 m étaient un choix initial révisable, pas un invariant. Assurer des transitions sans trous/doublons ; mesurer une erreur géométrique conservative et une erreur optique pertinente (couverture directionnelle, silhouette, couleur, transmittance approximative), tenir compte des textures/alpha et des transformations non uniformes. Les coupes dont l'erreur est trop forte doivent raffiner vers un niveau valable.
4. Patrimoine déterministe, mis en cache indépendamment des sources inchangées ; production incrémentale lors des extents, invalidation et reconstruction locale des branches modifiées. Ne pas dupliquer massivement la géométrie exacte par instance en VRAM ou dans le cache.

### Exploitation effective dans MG2, raster et RTX

5. Installer une sélection hiérarchique à partir des racines territoriales, sur GPU autant que pertinent. Utiliser l'erreur projetée des deux yeux, la distance et les critères optiques ; raffiner seulement les branches utiles. Les représentations collectives remplacent réellement les descendants dans les listes sélectionnées. Éviter la traversée de toutes les instances et leur readback obligatoire. Garder les résultats compacts, asynchrones, cohérents par génération. Les branches hors champ utiles aux ombres/reflets doivent disposer d'une sélection rayons indépendante.
6. Publier la coupe végétale dans les voies MG2 raster/depth/opaque et CLAS/BLAS/PTLAS ; la même hiérarchie peut produire des coupes raster et RTX différentes. Un parent et ses descendants ne doivent pas être rendus simultanément dans un même domaine, hors transition explicitement contrôlée. Réutiliser les clusters/CLAS, ne modifier que les partitions touchées. Préserver sources proches, identité, résidence et état de destruction.
7. Supprimer le basculement massif et prolongé sur les meshlets natifs lorsque MG2 est en attente : analyser démarrage, changement de masque, génération, cache et publication. Utiliser une coupe collective valide ou la dernière coupe compatible ; ne jamais masquer des objets simplement pour éviter le coût. Le fallback de sécurité ne doit pas devenir le fonctionnement normal. Traiter les chemins natifs résiduels directement liés à la végétation dans ce billet.
8. Ne pas introduire de dépendance à un parcours CPU individuel par frame ou de synchronisation GPU globale. Garder les caches existants et utiliser le budget réel de VRAM ; le gain doit provenir de la réduction du travail utile, pas d'une réduction de contenu.

### Emplacements de départ (à vérifier dans main)

- Producteurs : src/app/Understory.h, UnderstoryGeometry.inl, ForestFloor.h, OrganicForest.cpp, BotanicalBake.h, TerrainExpansion.h, TerrainTerritory.h, Mg2Catalog.* / Mg2Runtime.cpp.
- Patrimoine : src/core/TerritorialPatrimony.h, Assets.h, DiskBake.h, WorldRegistry.*, Mg2Pages.*, Mg2Canopy.h, GpuSceneCache.*.
- Sélection/publication : shaders/mg2_select.hlsl (localiser le chemin réel), src/core/GraphicsMg2Forest.cpp, GraphicsResidentTiles.cpp, GraphicsRayTracing.cpp, GraphicsPrepare.cpp, GraphicsInternal.h, GraphicsFoliageOmm.cpp, GraphicsRender.cpp, GraphicsFrameGraph.cpp ; shaders de visibility/depth/opaque selon l'implémentation réelle.

### Qualification de LOD-23

Compiler et publier une version cohérente, puis le testeur exécute le benchmark lod existant. Comparer aux deux journaux du §3, par scène et segment, en tenant compte des transitions. Démontrer un gain mesurable sur la végétation basse ET les arbres ; vérifier les métriques de sélection (candidats, nœuds, readback), meshlets natifs vs MG2, temps Depth/Opaque.Resident, publication et PTLAS, ainsi que le rendu complet. Vérifier visuellement couverture/couleur/silhouette aux distances utiles et absence de perte à proximité. Aucun seuil arbitraire de FPS propre à ce billet ; la cible moteur demeure 72 Hz. Ne pas déclarer le billet qualifié sur la seule baisse du nombre de triangles ou sur une compilation. Rapporter toute limite non vérifiée (notamment VR/optique) ; le testeur décide de la validation visuelle.

## 6. LOD-22 — Patrimoine architectural et composites

**Statut : NON INTÉGRÉ.** Après LOD-23. Mobilier → pièce → étage ; architecture → bâtiment → lettre → logo OASIS ; composites par domaine spatial et matériau. Produire les représentations à partir des recettes sémantiques disponibles (éviter de matérialiser tout le mobilier pour simplifier ensuite). Préserver silhouette, ouvertures, vitres, vides des lettres, matériaux, densité intérieure, séparation des parties mobiles/destructibles/translucides. Les collisions, la destruction et les identités originales restent autoritaires ; dépendances/révisions et invalidations locales, cache réutilisable. Compléter le patrimoine sans activer de nouvelles voies de rendu architectural prématurément ; réutiliser les producteurs généralisables de LOD-23.
Points de départ : src/app/BuildingSession.cpp, BuildingCampus.h, building/BuildingGeometry.cpp, BuildingPlan.cpp, SectorBake.h, Mg2Catalog.cpp ; src/core/Assets.h, Mg2Pages.cpp. Qualification : nombre de représentations, réductions par niveau, erreur et couverture, cache/reconstruction locale ; rendu courant inchangé.

## 7. LOD-31 — Généralisation de la sélection territoriale GPU

**Statut : NON INTÉGRÉ.** Étendre à l'architecture, au terrain et aux composites le mécanisme validé sur la végétation. Le terrain a été retiré du périmètre immédiat de LOD-21 pour prioriser les résultats LOD-00, mais n'est pas abandonné : compléter ici, si nécessaire, ses représentations secteur → continent, en réutilisant les pages 10/20/40/80 m déjà présentes. Terrain : simplification triangulaire continue, bords compatibles, préservation du relief et des matériaux. Les compositions de territoires doivent conserver la séparation des domaines optiques et des matériaux.
Sélection par racines puis coupes collectives ; erreur projetée stéréo (max des deux yeux), frusta, occlusion conservative, mouvement, validité et génération ; employer le seuil MG2 individuel existant (initialement 1,1 px, à vérifier) et des critères optiques adaptés plutôt que des distances seules. Éliminer la traversée des descendants lorsqu'une coupe collective est suffisante, compacter les résultats GPU sans readback individuel obligatoire, préserver publication asynchrone et fallbacks valides. Généraliser la consommation raster ; LOD-32 achève la publication complète des domaines restants. Points de départ : Mg2Pages.h/.cpp, mg2_select.hlsl, GraphicsMg2Forest.cpp, GraphicsInternal.h, WorldRegistry.*, GpuSceneCache.*, TerrainExpansion.h, TerrainTerritory.h. Qualification : baisse des candidats/nœuds/readback et du coût de sélection à population croissante, qualité inchangée.

## 8. LOD-32 — Généralisation de la publication raster et RTX

**Statut : NON INTÉGRÉ.** Étendre les coupes sélectionnées aux consommateurs restants : raster/depth/opaque, CLAS/BLAS/PTLAS. Hiérarchie commune avec coupes indépendantes selon raster et rayons secondaires ; ne pas confondre hors champ et inutile aux rayons. Bascule parent/enfants atomique par génération sans trous ni doubles représentations. Partitions PTLAS liées au territoire et à la coupe active ; réemploi des CLAS/BLAS et mises à jour locales. Destruction locale, cohérence des identités et invalidations, résidence indépendante, pas de réexpansion des agrégats en instances originales par un consommateur legacy. Points de départ : GraphicsMg2Forest.cpp, GraphicsResidentTiles.cpp, GraphicsRayTracing.cpp, GraphicsPrepare.cpp, GraphicsInternal.h, GraphicsFoliageOmm.cpp. Qualification : performance et rendu du benchmark, coûts de publication, accélération, transitions et résidence.

## 9. LOD-41 — Élimination des parcours historiques

**Statut : NON INTÉGRÉ.** Traquer tous les coûts restants liés à la population totale dans les registres, visibility/depth occluders/depth/opaque resident, compaction, ExecuteIndirect, raster occlusion et anciens consommateurs. Remplacer les scans et reconstructions par travail proportionnel aux coupes sélectionnées et aux révisions locales. Préserver les occludeurs géométriquement conservatifs : ne pas boucher artificiellement les ouvertures/lettres concaves ; un logo invisible derrière la forêt ne doit pas entraîner le calcul de tout son détail. Points de départ : GraphicsRender.cpp, GraphicsResidentTiles.cpp, GraphicsPrepare.cpp, GpuSceneCache.cpp, compute.hlsl, camera_occlusion.hlsl, GraphicsFrameGraph.cpp. Qualification : réduction CPU/GPU des passes et soumissions, notamment en forêt vers le logo caché ; pas de perte de visibilité ni d'effets.

## 10. LOD-42 — Optique, accélération et mémoire

**Statut : NON INTÉGRÉ.** Éliminer les coûts résiduels proportionnels au territoire dans l'éclairage, soleil/ombres/reflets, rayons secondaires, mises à jour des accélérations, ressources et transferts. Les domaines optiques choisissent leurs propres coupes spatiales et ne réactivent pas tous les descendants. Préserver les effets, sources lumineuses et contributions hors champ réellement utiles. Utiliser le budget DXGI réel (16 Gio physiques sur la cible), garder en VRAM les ressources coûteuses tant que le budget le permet et en DDR4 les réserves pertinentes ; gérer les évictions selon pression réelle et coût de reconstruction, non selon un plafond arbitraire. Ne pas reconstruire ou transférer une source inchangée. Points de départ : GraphicsLighting.cpp, GraphicsRayTracing.cpp, GraphicsMg2Forest.cpp, GraphicsPrepare.cpp, GraphicsResources.cpp, GpuMemory.cpp, solar_samples.hlsl, scene.hlsl. Qualification : coûts optiques/RTX, transferts, résidence et stabilité, qualité conservée.

## 11. LOD-43 — Qualification finale

**Statut : NON INTÉGRÉ.** Vérifier la scalabilité à positions/angles/réglages identiques sur plusieurs tailles de territoire (au-delà du benchmark fixe de 20 extents), y compris regard vers le cosmos et logo masqué. Distinguer charge réellement visible, coût rayons, croissance, publication et coût indépendant de la vue. Corriger les scans/rebuilds/invalidations encore proportionnels à la population totale. Ne faire un commit LOD-43 que s'il existe une correction réelle ; sinon qualifier le chantier sans commit de code artificiel. Objectif : le coût de la vue ne croît plus mécaniquement avec le nombre d'extents hors de sa portée utile.

## 12. Journal de qualification et fonctionnement du dispatch

Garder les journaux compacts et exploitables : résumés par scène/segment, FPS et frametimes CPU/GPU (P95/P99 lorsque possible), population logique/résidente/sélectionnée, nœuds visités/raffinés et agrégats, triangles/meshlets/clusters uniques, sélection/compaction/readback/publication, passes visibility/depth/opaque, ExecuteIndirect et PSO, CLAS/BLAS reconstruits et partitions PTLAS modifiées, rayons et coûts soleil/reflets, VRAM/budget/DDR4/transferts, hits/misses et reconstruction du cache, invalidations/générations et erreurs de couverture. Éviter les répétitions par frame sans valeur ; ne pas supprimer un compteur qui permet de localiser le coût. Le benchmark existant reste la référence, pas une succession de nouvelles batteries de QA.

Un billet = une version cohérente testable = un commit substantiel sur main (sauf qualification LOD-43 sans correction). Pas de branches, PR ni micro-commits de chaque opération interne. L'agent inspecte main, repère les emplacements réels, évalue les dépendances amont/aval, choisit les solutions adaptées au matériel NVIDIA sans déborder inutilement sur les billets suivants. Ne pas imposer une procédure de développement supplémentaire. Compilation lorsque disponible, sinon signaler explicitement la limite ; indiquer au testeur ce qu'il doit observer. Ne pas déclarer un gain non mesuré.

Ce plan est maintenu dans main : après chaque billet, mettre à jour son statut et sa référence dans ce même fichier (dans le commit du billet si possible, référence de commit complétée par le prochain agent si nécessaire). Distinguer intégré, testé et qualifié ; les décisions importantes ou limites restantes doivent être inscrites ici, pas seulement dans le chat. Le prochain agent reçoit uniquement l'identifiant du billet et le chemin de ce fichier.
