# OASIS — Plan d'intégration LOD-24 : préchargement et résidence du patrimoine MG2

**Chantier actif : LOD-24 — INTÉGRATION À QUALIFIER SOUS WINDOWS. Un seul commit sur main.**

## 1. Contexte et état

LOD-21/23 : intégrés, base végétale acceptée. LOD-22 : intégré, première publication et substitution architecturale raster/RTX observées après le correctif 4b680df3 ; qualification complète encore requise. Le run d42bcf90 a produit des images mais a subi des saccades et s'est terminé sur DEVICE_REMOVED. Ne pas attribuer ce défaut au manque de VRAM sans preuve. Le patrimoine est préparé trop tard, sur demande de la caméra ; le cache source conserve déjà les blocs valides hors pression réelle. Ne pas revenir en arrière sur les acquis.

Historique intégral, tâches, décisions, références et baseline : [plan LOD-22 archivé par révision](https://github.com/Tabernacle666/PooLabVR/blob/524e1d0993d0eda7ab8e196f0441ba8af5c462e8/PLAN_LOD.md). Baseline complète : [LOD-23](diagnostics/sessions/20261128-030110-138-lod-15220/OASIS.log), dcc5e79440b0428a335252b6c782a24531a3b228.

## 2. Mandat

Préparer tout le patrimoine géométrique connu avant l'entrée dans le monde, le conserver et sélectionner au lieu de fabriquer pendant le gameplay. RTX 5060 Ti 16 Gio, DDR4 64 Gio, Quest 3S. OASIS prioritaire : aucun plafond VRAM, quota d'agrégats, de clusters ou de LOD, aucune réserve fixe destinée aux autres applications. Accepter et mesurer le débordement en mémoire système via WDDM, sans présumer que la pagination est gratuite ni garantie. Préserver qualité, seuils, destruction, optique et LOD-21/22/23.

## 3. Intervention unique

| Ordre | Statut | Opération |
|---:|---|---|
| 1 | Intégré, à qualifier | `GraphicsMg2Forest.cpp::PrepareTerritorialCatalog` : remplacer la demande architecturale dépendante de la caméra par l'énumération exhaustive des nœuds admissibles à chaque niveau, enfants inclus. Produire par lots sur le worker, indépendamment du choix de coupe. Conserver les sources valides, invalider localement par révision. Achever l'admission initiale et les mutations initiales avant de déclarer le catalogue stable. |
| 2 | Intégré, à qualifier | `Mg2Pages.h/.cpp` : énumérer les coupes canoniques valides uniques (racine, intermédiaires, feuilles, variantes structurelles), préserver ordre des clusters, identités, matériaux, géométrie et ABI. Pas d'énumération combinatoire des coupes adaptatives. |
| 3 | Intégré, à qualifier | `ForestSourcePool`, `EnsureForestSourceArenas`, `UploadForestPages` : extraire/réutiliser la procédure de mapping et d'upload des tuiles sources ; précharger tous les blocs vivants des segments en lots bornés par granularité de travail, pas par occupation. Conserver les offsets et fences ; réutiliser les blocs déjà présents. |
| 4 | Intégré, à qualifier | `ForestAccelerationCache`, `PackForestCut`, `UploadForestPages`, `ValidateForestGeneration` : préconstruire les accélérations des coupes canoniques sur le producteur sérialisé. Réutiliser les clés CLAS (cluster, offset primitif) et BLAS (séquence ordonnée), mutualiser les allocations ; insérer uniquement après validation GPU. Conserver les caches, pas des générations de scène entières. Ne jamais publier une génération de préchauffage comme coupe du monde. |
| 5 | Intégré, à qualifier | `UploadForestPages`, `TrimForestSourceBlocks`, `ForestAccelerationCache::TrimCold`, politique mémoire appelée en amont/aval : retirer les refus fondés sur réserve fixe (notamment Budget/20 et headroom arbitraire). Budget DXGI = télémétrie, pas plafond logiciel ; tenter les allocations légitimes, laisser WDDM gérer la résidence/pagination, conserver la source DDR4. Réserver les ressources de travail effectivement nécessaires par allocation réelle. Évincer uniquement sous pression réelle empêchant de progresser ou invalidation ; ne jamais évincer une ressource encore référencée. Traiter les échecs d'allocation réels sans boucle infinie ni dépassement ABI. |
| 6 | Intégré, à qualifier | `AppSession.cpp`, `GraphicsRender.cpp`, XR : présenter une couche de chargement légère, logo OASIS et progression issue du travail réel. Maintenir événements OpenXR et cadence xrWaitFrame/BeginFrame/EndFrame, swapchain de chargement indépendante des ressources de scène ; respecter shouldRender, acquisition/libération et fences. Aucun WaitStartup, get bloquant ou construction lourde sur le thread XR ; préparation graphique initiale et producteur hors de cette boucle, coordination explicite des propriétaires/queues. Fractionner les travaux GPU pour préserver le compositeur. Ne pas envoyer le monde ni démarrer la locomotion/benchmark avant admission et préchargement achevés, ou constat explicite d'impossibilité matérielle avec patrimoine DDR4 conservé. Basculer atomiquement à la scène réelle ; arrêter proprement si session perdue ou device removed. |
| 7 | Intégré, à qualifier | Sélection dynamique existante raster/RTX et parking des descendants : conserver fidélité et transactions. Fabrication en jeu limitée aux mutations, extensions et coupes adaptatives réellement absentes ; ne pas détruire les représentations valides au changement de distance. |
| 8 | Intégré, à qualifier | Diagnostic compact : durée et couverture du préchargement (nœuds, blocs, coupes, CLAS/BLAS), usage/budget local et nonlocal, échecs/évictions réels, progression XR, puis compteurs de constructions/transferts résiduels pendant le benchmark. Distinguer écran de chargement et première image réelle. |

## 4. Qualification et livraison

Un seul commit d'intégration LOD-24, incluant le statut actualisé de ce plan. Aucun fallback natif ajouté, aucun seuil modifié, aucune suppression d'effet. Les budgets d'exécution GPU par lot ne constituent pas des plafonds de patrimoine. Ne pas promettre toutes les combinaisons adaptatives ni une absence absolue de construction en jeu.

`pull_compile_run.bat lod` : parcours identique au dernier benchmark complet LOD-23, 20 extents, warmup 6 s à partir de la première image du monde, cinq blocs de 16 s, mêmes masques et segments. Comparer débit, saccades, sélection, Depth/Opaque.Resident, constructions et pagination ; contrôler qualité de proximité, agrégats et impacts. Qualification Windows par le testeur, pas de résultat inventé. En cas d'échec, conserver la révision précédente comme point de retour ; aucun revert automatique.

## 5. Ordre de dispatch

| Billet | Statut |
|---|---|
| MG2-A01 à A21 / LOD-00 / LOD-21 | INTÉGRÉS |
| LOD-23 | INTÉGRÉ, base acceptée ; qualification partielle |
| LOD-22 | INTÉGRÉ ; première publication observée ; qualification complète reportée au run LOD-24 |
| **LOD-24** | **INTÉGRÉ — QUALIFICATION REQUISE** |
| LOD-31/32/41/42/43 | NON INTÉGRÉS — en attente |

Ne pas ouvrir les chantiers suivants avant le résultat LOD-24. Les volets architecturaux LOD-31/32 sont déjà absorbés par LOD-22 ; le volet préchargement/résidence de LOD-42 est avancé à LOD-24.

## 6. Reprise

**Ordre actuel :** exécuter `pull_compile_run.bat lod`, vérifier le chargement XR, les traces `lod24_prewarm`, `lod24_world_gate`, la couverture du patrimoine, les constructions résiduelles et la stabilité du parcours. Aucun résultat Windows présumé. Ne pas ouvrir LOD-31 avant qualification.

## 7. LOD-31 — Généralisation de la sélection territoriale GPU

**Statut : NON INTÉGRÉ.** Étendre au terrain et aux composites restants les mécanismes établis dans LOD-23 et LOD-22 ; ne pas refaire l'intégration architecturale. Le terrain a été retiré du périmètre immédiat de LOD-21 pour prioriser les résultats LOD-00, mais n'est pas abandonné : compléter ici, si nécessaire, ses représentations secteur → continent, en réutilisant les pages 10/20/40/80 m déjà présentes. Terrain : simplification triangulaire continue, bords compatibles, préservation du relief et des matériaux. Les compositions de territoires doivent conserver la séparation des domaines optiques et des matériaux.
Sélection par racines puis coupes collectives ; erreur projetée stéréo (max des deux yeux), frusta, occlusion conservative, mouvement, validité et génération ; employer le seuil MG2 individuel existant (initialement 1,1 px, à vérifier) et des critères optiques adaptés plutôt que des distances seules. Éliminer la traversée des descendants lorsqu'une coupe collective est suffisante, compacter les résultats GPU sans readback individuel obligatoire, préserver publication asynchrone et fallbacks valides. Généraliser la consommation raster ; LOD-32 achève la publication complète des domaines restants. Points de départ : Mg2Pages.h/.cpp, mg2_select.hlsl, GraphicsMg2Forest.cpp, GraphicsInternal.h, WorldRegistry.*, GpuSceneCache.*, TerrainExpansion.h, TerrainTerritory.h. Qualification : baisse des candidats/nœuds/readback et du coût de sélection à population croissante, qualité inchangée.

## 8. LOD-32 — Généralisation de la publication raster et RTX

**Statut : NON INTÉGRÉ.** Étendre les coupes sélectionnées aux consommateurs et domaines non couverts par LOD-22/23 : raster/depth/opaque, CLAS/BLAS/PTLAS. Hiérarchie commune avec coupes indépendantes selon raster et rayons secondaires ; ne pas confondre hors champ et inutile aux rayons. Bascule parent/enfants atomique par génération sans trous ni doubles représentations. Partitions PTLAS liées au territoire et à la coupe active ; réemploi des CLAS/BLAS et mises à jour locales. Destruction locale, cohérence des identités et invalidations, résidence indépendante, pas de réexpansion des agrégats en instances originales par un consommateur legacy. Points de départ : GraphicsMg2Forest.cpp, GraphicsResidentTiles.cpp, GraphicsRayTracing.cpp, GraphicsPrepare.cpp, GraphicsInternal.h, GraphicsFoliageOmm.cpp. Qualification : performance et rendu du benchmark, coûts de publication, accélération, transitions et résidence.

## 9. LOD-41 — Élimination des parcours historiques

**Statut : NON INTÉGRÉ.** Traquer tous les coûts restants liés à la population totale dans les registres, visibility/depth occluders/depth/opaque resident, compaction, ExecuteIndirect, raster occlusion et anciens consommateurs. Remplacer les scans et reconstructions par travail proportionnel aux coupes sélectionnées et aux révisions locales. Préserver les occludeurs géométriquement conservatifs : ne pas boucher artificiellement les ouvertures/lettres concaves ; un logo invisible derrière la forêt ne doit pas entraîner le calcul de tout son détail. Points de départ : GraphicsRender.cpp, GraphicsResidentTiles.cpp, GraphicsPrepare.cpp, GpuSceneCache.cpp, compute.hlsl, camera_occlusion.hlsl, GraphicsFrameGraph.cpp. Qualification : réduction CPU/GPU des passes et soumissions, notamment en forêt vers le logo caché ; pas de perte de visibilité ni d'effets.

## 10. LOD-42 — Optique, accélération et mémoire

**Statut : NON INTÉGRÉ.** Éliminer les coûts résiduels proportionnels au territoire dans l'éclairage, soleil/ombres/reflets, rayons secondaires, mises à jour des accélérations, ressources et transferts. Les domaines optiques choisissent leurs propres coupes spatiales et ne réactivent pas tous les descendants. Préserver les effets, sources lumineuses et contributions hors champ réellement utiles. Utiliser le budget DXGI réel (16 Gio physiques sur la cible), garder en VRAM les ressources coûteuses tant que le budget le permet et en DDR4 les réserves pertinentes ; gérer les évictions selon pression réelle et coût de reconstruction, non selon un plafond arbitraire. Ne pas reconstruire ou transférer une source inchangée. Points de départ : GraphicsLighting.cpp, GraphicsRayTracing.cpp, GraphicsMg2Forest.cpp, GraphicsPrepare.cpp, GraphicsResources.cpp, GpuMemory.cpp, solar_samples.hlsl, scene.hlsl. Qualification : coûts optiques/RTX, transferts, résidence et stabilité, qualité conservée.

## 11. LOD-43 — Qualification finale

**Statut : NON INTÉGRÉ.** Vérifier la scalabilité à positions/angles/réglages identiques sur plusieurs tailles de territoire (au-delà du benchmark fixe de 20 extents), y compris regard vers le cosmos et logo masqué. Distinguer charge réellement visible, coût rayons, croissance, publication et coût indépendant de la vue. Corriger les scans/rebuilds/invalidations encore proportionnels à la population totale. Ne faire un commit LOD-43 que s'il existe une correction réelle ; sinon qualifier le chantier sans commit de code artificiel. Objectif : le coût de la vue ne croît plus mécaniquement avec le nombre d'extents hors de sa portée utile.

## Qualification LOD-24

La couche XR statique indique les jalons réels (configuration, admission, catalogue, sources, accélérations, prêt) sans nouvelle soumission sur la file de scène après sa création. Les coupes canoniques sont les coupes complètes par profondeur topologique de DAG et les variantes structurelles, dédupliquées par ordre des clusters. Le backend standard explicite ne possède pas le cache CLAS/BLAS : le préchauffage des accélérations y est signalé incomplet. En cas d'allocation impossible, le catalogue CPU est conservé et le journal indique la couverture et le motif. La compilation et le run VR restent à effectuer.

La progression du chargement représente les jalons, non un pourcentage temporel estimé. Une coupe canonique est un paquet GPU indivisible dans cette révision : la stabilité des très grosses coupes et du premier build d'admission reste à vérifier. Les préparations CPU effectuées avant la création de la session XR ne peuvent pas présenter de couche XR.

### Correctif du premier run LOD-24 (5a7cde48)

Constat : compilation réussie, présentation XR pendant plus de 4 681 frames, 7 218 nœuds et 66 018 pages ; perte du périphérique après la validation de la deuxième génération, avant le préchauffage. La cause exacte du DEVICE_REMOVED n'est pas démontrée par le journal initial.

Correctif intégré, à qualifier : préchauffer le catalogue stabilisé avant la sélection collective finale ; arènes virtuelles indépendantes dimensionnées selon la plage adressable d'une vue (aucun plafond sur le volume total) ; paquets CLAS de 1 024 entrées et BLAS de 64 entrées / 8 192 références (une BLAS indivisible peut dépasser ce dernier seuil), avec fences sur le producteur et conservation des clés et adresses ; DRED déclenché avant Fail() sur les opérations ciblées ; journal lod24_publish, lod24_gpu_packets et lod24_prewarm_order. Aucun changement de seuil de qualité ni de politique de résidence. Qualification Windows et VR requise avant LOD-31.

### LOD-24 — Admission opérationnelle avant ouverture (correctif du run 4250db11)

Le run valide 73 724 tuiles sources, 50 805 coupes, 934 694 CLAS et 51 104 BLAS sans éviction ni échec. Le signal prêt précédait toutefois la publication territoriale (41 753 descendants stationnés après l'ouverture) et la construction initiale des trois PTLAS. Le DEVICE_HUNG de rendu reste à qualifier.

Intégré, à tester : la sélection de départ utilise le même espace VIEW de diagnostic et la même portée optique que le rendu ; le registre stationné, les coupes raster/DXR, les révisions et les producteurs doivent être cohérents avant l'admission. Après le préchauffage, resampler la pose réelle ; exécuter le graphe complet sur les cibles stéréo privées, séquentiellement, avec fences direct/compute/copy. Les trois slots doivent être validés sur la même génération et les mêmes coupes, TLAS/PTLAS construit si nécessaire, sans nouveau travail MG2 en vol. La session OpenXR continue de présenter sa quad indépendante ; aucune projection de monde ni mesure de benchmark pendant ces rendus. Le signal final (stage 7) n'est publié qu'après le masque des trois slots (0x7) et la libération des cibles privées. Log lod24_operational_frame (slots, serial, stable, ray_ready, mémoire locale/non locale) puis lod24_world_gate ready=1 operational_complete=1 pending=0. DRED avant Fail sur la détection DEVICE_REMOVED/HUNG dans Prepare et les fences d'admission. Aucun plafond VRAM ni changement de qualité. Qualification Windows/VR et examen du DEVICE_HUNG restant avant LOD-31.
