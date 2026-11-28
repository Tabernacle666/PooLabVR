# OASIS — Plan d'intégration LOD-22 : patrimoine architectural MG2

**Révision : 28 novembre 2026 — chantier actif LOD-22, non commencé. LOD-23 intégré et accepté comme base de travail ; qualification territoriale à grande distance reportée.**

Ce fichier remplace le document de dispatch LOD-23. Il constitue le mandat autonome du prochain agent. L'historique technique détaillé de LOD-23 reste disponible dans [la dernière révision de l'ancien plan](https://github.com/Tabernacle666/PooLabVR/blob/281d6f47038679e332a99266150b7dd97117fd70/PLAN_LOD.md). Ne pas rouvrir ce chantier pour faire apparaître des agrégats artificiellement.

## 1. Mandat

OASIS : DX12 / OpenXR, RTX 5060 Ti 16 Gio, DDR4 64 Gio, Quest 3S à 72 Hz. Cible AAA et 13,89 ms/image, sans verrou volontaire à 36 FPS. Priorité immédiate : immeuble OASIS à moyenne distance et pendant les déplacements. Le joueur rapporte un comportement conforme-lent-conforme (environ 72 → 22 → 72 FPS lorsque le mouvement cesse) dans le contexte du logo ; c'est une observation à isoler, pas une causalité démontrée. La qualité de proximité et la stabilité végétale sont jugées satisfaisantes par le joueur.

LOD-22 doit passer de l'organisation sémantique existante à une hiérarchie géométrique réellement sélectionnée et consommée par MG2, raster et RTX. Un bâtiment distant ou masqué peut demeurer résident sans que tous ses composants soient évalués ou dessinés à leur détail original. À proximité, conserver la géométrie et l'information des objets, y compris intérieurs et destruction. Aucun quota d'agrégats sélectionnés, aucun plafond VRAM arbitraire, aucune suppression de contenu, de lumière, d'ombre ou de reflet. Une coupe individuelle valide demeure le choix légitime si une approximation collective n'est pas suffisante.

Référentiel : [NVIDIA RTX Mega Geometry](https://github.com/NVIDIA-RTX/RTXMG), [DX Clusters / PTLAS](https://github.com/NVIDIA-RTX/Whitepapers/tree/main). Réutiliser le catalogue ouvert, les segments/arènes et caches CLAS/BLAS de LOD-23 ; ne pas créer un moteur architectural parallèle. Les exemples NVIDIA sont un référentiel technique et non une spécification de hiérarchie sémantique OASIS.

## 2. Ordre de dispatch et statut global

| Ordre | Billet | Statut | Objet |
|---|---|---|---|
| Prérequis | MG2-A01 à A21 | INTÉGRÉ | Infrastructure MG2 |
| 0 | LOD-00 | INTÉGRÉ / TESTÉ | Baseline bcb2df1972a1f78ee69b16e48c19ba6424eb49f1 |
| 1 | LOD-21A/B | INTÉGRÉ / TESTÉ | Patrimoine végétal CPU f03cb9ed01a7a990214994cc4fb40197e499f8b4 / 97b91146b80862f15786d38372463dadf55a05c4 |
| 2 | LOD-23 | INTÉGRÉ / TESTÉ / ACCEPTÉ POUR LA SUITE ; qualification partielle | Catalogue ouvert, coupes territoriales végétales, suppression du fallback natif végétal ; dcc5e79440b0428a335252b6c782a24531a3b228 |
| 3 | **LOD-22** | **ACTIF — NON INTÉGRÉ** | Patrimoine architectural, sélection et publication verticales |
| 4 | LOD-31 | NON INTÉGRÉ | Généralisation aux domaines restants |
| 5 | LOD-32 | NON INTÉGRÉ | Publication raster/RTX des domaines restants |
| 6 | LOD-41 | NON INTÉGRÉ | Élimination des parcours historiques |
| 7 | LOD-42 | NON INTÉGRÉ | Optique, accélération, résidence |
| 8 | LOD-43 | NON INTÉGRÉ | Qualification finale de scalabilité |

LOD-22 absorbe le volet architectural des LOD-31 et LOD-32 : ne pas dispatcher ces volets séparément. Les opérations internes ci-dessous sont un suivi de réalisation, pas autant de billets ou de commits.

## 3. Base acceptée : LOD-23

Dernier benchmark complet : [journal LOD-23](diagnostics/sessions/20261128-030110-138-lod-15220/OASIS.log), révision dcc5e79440b0428a335252b6c782a24531a3b228. Références antérieures : [LOD-00](diagnostics/sessions/20261127-230703-911-lod-35088/OASIS.log), [LOD-21](diagnostics/sessions/20261127-233734-121-lod-29948/OASIS.log). Les journaux de crash ne constituent pas des baselines de performance.

- Catalogue MG2 ouvert, segments immuables, arènes source multiples, compaction des métadonnées, résidence selon budget DXGI : intégrés.
- Patrimoine végétal collectif, sélection territoriale GPU, coupes raster/RTX indépendantes et parking des descendants : intégrés ; bénéfice de la substitution collective non démontré dans le benchmark de proximité.
- Admission végétale MG2 obligatoire, préadmission transactionnelle des mutations et extents, maintien de la coupe MG2 valide pendant la production : intégrés. Le dernier benchmark complet ne reproduit plus les blocages catastrophiques du fallback natif ; qualité proche jugée satisfaisante.
- Fractionnement des dispatchs de sélection sur le worker GPU et diagnostic de phases : intégré et testé.
- Absence de sélection collective au parcours actuel : ne pas interpréter comme défaut à corriger sur la seule base du compteur. La fidélité proche prime ; qualifier la sélection à des distances où son erreur visuelle devient réellement admissible.
- Dettes reportées : qualification optique lointaine et transmittance/OMM, qualification des mutations et croissance à grande échelle, récupération sûre des plages virtuelles et index morts, coût résiduel des parcours. LOD-41/42/43, selon nature. Ne pas prétendre que tout LOD-23 a été qualifié ni bloquer LOD-22 pour ces dettes.
- Chargement initial VR : idée d'une scène légère avec progression pendant préparation du monde, à traiter séparément si nécessaire ; pas un prétexte pour modifier le benchmark ni masquer un problème de coordination.

## 4. État architectural réel avant LOD-22

`src/core/building/BuildingPlan.h` : Definition (lettre, dimensions, niveaux, atrium), Plan/Level/Room, Sector par niveau, Part (identité, source, matériau, façade, room, paneRoom, état destructible), Mount, Joint, ServiceNode, RoomLight. Ces recettes et identités sont le point de départ. Le BVH de contacts n'est pas une hiérarchie de LOD.

`src/app/BuildingSession.cpp` : publication des secteurs, pièces et composants, mobilier/placements, streaming des worktops, éléments mobiles, matériaux et vitres. Le cube structurel opaque est déjà inscrit dans mg2ForestParts ; les vitrages translucides restent distincts. Cela ne prouve pas que tous les composants du bâtiment disposent d'une source MG2 correcte ou qu'ils utilisent MG2 en toutes circonstances. Auditer la couverture réellement consommée, y compris assets du catalogue et contributions statiques/dynamiques.

`src/app/BuildingCampus.h` : cinq lettres, boîtes d'occlusion par bâtiment, bandes spécifiques du S ; ces boîtes ne sont pas des silhouettes de rendu et ne prouvent pas une occlusion correcte dans les ouvertures de O/A/S. La géométrie détaillée et ses vides restent autoritaires.

`src/core/GraphicsMg2Forest.cpp` : ForestRouteEligible accepte les meshes enregistrés opaques admissibles, mais ForestRouteRequired impose actuellement l'admission MG2 aux routes végétales seulement. Le catalogue territorial actuel utilise deux domaines végétaux (bool forest) et une parenté spatiale 10/40/160 m. Généraliser l'identité, les relations de parenté et les producteurs pour accepter les domaines architecturaux sémantiques, sans régression de la végétation.

| Niveau architectural | Organisation sémantique | Représentation collective MG2 |
|---|---|---|
| Objet / composant | EXISTANTE | PARTIELLE, audit requis |
| Pièce | EXISTANTE | À PRODUIRE |
| Étage | EXISTANTE | À PRODUIRE |
| Façade / enveloppe | EXISTANTE en géométrie et recettes | À PRODUIRE |
| Lettre / bâtiment | EXISTANTE | À PRODUIRE |
| Logo OASIS | EXISTANTE | À PRODUIRE |

## 5. Plan d'intégration LOD-22 — suivi interne

| Ordre | Tâche | Statut |
|---:|---|---|
| 1 | Couverture MG2 du patrimoine architectural et profil de coût | NON INTÉGRÉ |
| 2 | Modèle hiérarchique et identités de représentation | NON INTÉGRÉ |
| 3 | Patrimoine pièces / mobilier | NON INTÉGRÉ |
| 4 | Patrimoine étages / façades | NON INTÉGRÉ |
| 5 | Patrimoine lettres / silhouette du logo | NON INTÉGRÉ |
| 6 | Admission catalogue, cache, révisions et destruction | NON INTÉGRÉ |
| 7 | Sélection GPU, stabilité en déplacement et occlusion | NON INTÉGRÉ |
| 8 | Publication raster et RTX, compaction des descendants | NON INTÉGRÉ |
| 9 | Qualification du chantier | NON EXÉCUTÉ |

### 1 — Couverture MG2 et coût architectural

Recenser les meshes, flags, matériaux et routes effectives des structures, mobilier, services, vitres et parties mobiles. Distinguer statique, résident, dynamique, déformé, destructible et alpha blend. Identifier les chemins natifs résiduels et les informations manquantes (source complète, matériaux/UV, bornes, identité, révision, rattachement pièce/niveau/lettre). Identifier les postes de coût du logo aux fenêtres du benchmark : sélection, publication, Depth, Opaque.Resident, occlusion, rayons, accélérations, CPU et transferts. Ne pas présumer que la chute en mouvement est exclusivement une absence de LOD. Conserver les exceptions techniquement justifiées (verre réellement translucide, déformation, etc.) dans un chemin cohérent ; ne pas confondre cette séparation de matériau avec un fallback natif pour une route MG2 obligatoire.

### 2 — Modèle hiérarchique et identités

Construire une hiérarchie sémantique, stable et révisable depuis les recettes : composant → pièce → étage → lettre → logo. La façade/enveloppe est un domaine complémentaire de l'étage et de la lettre, pas un parent unique de tout le mobilier. Partitionner les ensembles par matériau, opacité, type de mouvement et autorité destructible ; chaque consommateur doit couvrir ses descendants une seule fois. Préserver rattachement aux IDs physiques, source, bornes, révision et dépendances ; les agrégats ne sont pas de nouveaux objets de simulation. Generaliser la clé de domaine et la parenté du catalogue territorial sans utiliser un second moteur de sélection. Réutiliser les sources compatibles inchangées.

### 3 — Pièces et mobilier

Produire des représentations composées des murs, sol, plafond, mobilier, services et volumes réellement présents, depuis les recettes sémantiques, sans matérialisation répétée de tout le patrimoine. Fusionner les surfaces coplanaires compatibles et mutualiser les ensembles répétitifs ; simplifier les objets selon leur erreur propre. Préserver volumes, ouvertures, couleurs, UV, matériaux, silhouettes, profondeur visible et luminaires réels. Garder portes, éléments mobiles, vitres et parties destructibles dans les domaines adaptés ; une pièce vue par une fenêtre ne doit pas devenir une paroi pleine ou un intérieur vide. Conserver les sources détaillées pour les interactions et vues proches.

### 4 — Étages et façades

Composer les pièces, corridors, dalles, structures, cages, gaines et enveloppes à l'échelle du niveau. Exploiter régularité des surfaces et répétition des étages pour limiter les routes et clusters actifs ; préserver ouvertures, trame des fenêtres, montants, reliefs, matériaux, vides et profondeur intérieure visible. Distinguer peau opaque, transmission du vitrage, volumes internes et émetteurs ; ne jamais remplacer du verre par une surface opaque pour économiser les rayons. Les lumières gardent leurs contributions utiles ; ne pas dupliquer les luminaires dans les représentations collectives.

### 5 — Lettres et logo

Produire des coupes géométriques d'enveloppe et silhouette à l'échelle de chaque lettre et du logo, issues des bandes et volumes réels (y compris toiture, dessous, atriums, concavités et passages traversants). Ne pas employer une boîte englobante comme substitut de silhouette, occludeur opaque ou volume plein de O/A/S. Conserver les proportions, séparations entre lettres, ouvertures et trame architecturale encore perceptible. Conserver les descendants pertinents si la coupe supérieure ne suffit pas. Priorité moyenne distance : façade/étage/lettre doivent être réellement disponibles, pas seulement une silhouette extrême.

### 6 — Catalogue, cache, révisions

Inscrire les sources originales admissibles et représentations collectives dans le catalogue ouvert MG2 ; conserver identités de clusters stables, références par segment et ressources de générations vivantes. Utiliser le cache persistant et mutualiser les recettes répétées, avec révisions de sources/materials/placements/damage. Une mutation, porte ou destruction invalide seulement les branches affectées ; maintenir les collisions et dégâts sur les identités originales. Ne pas conserver une représentation obsolète qui masquerait la destruction : transition vers une coupe MG2 cohérente, puis publication de la branche reconstruite. Conserver les coupes valides jusqu'à remplacement atomique ; pas de fallback natif pour les routes déclarées MG2 obligatoires, pas de retrait de détails requis. Aucune attente de producteur sur le thread XR ; pas de reconstruction globale pour un changement local.

### 7 — Sélection GPU, mouvement et occlusion

Réutiliser le sélecteur territorial et les coupes par domaine. Parcourir les racines sémantiques puis les enfants uniquement lorsque leur erreur visuelle ou optique exige un raffinement. Utiliser erreur projetée stéréo, frustum, distance au support réel, mouvement prédit et occlusion conservative ; les seuils doivent représenter la fidélité effective, pas provoquer artificiellement des agrégats. L'absence d'agrégat près du joueur est normale. Séparer sélection caméra et besoins rayons hors champ. Stabiliser les coupes pendant le déplacement avec réutilisation des représentations, demandes latest-only et transitions cohérentes ; ne pas reconstruire ou relire tous les descendants pour un simple changement de pose lorsque la coupe demeure valable. Mesurer la différence marche/pause et les coûts de publication. Employer occludeurs conservatifs pour les parties effectivement opaques, sans remplir les vides ni faire disparaître les intérieurs visibles. Si le logo est caché derrière la forêt, éviter son détail caméra tout en conservant ses contributions RTX utiles et sa résidence.

### 8 — Publication raster et RTX

Intégrer verticalement la coupe architecturale aux consommateurs Depth/Visibility/Opaque, indirect, CLAS/BLAS/PTLAS, ombres et reflets. Les coupes raster/RTX peuvent être indépendantes dans les limites de leur erreur respective. Substitution atomique parent/enfants sans trou ni double rendu, avec parking des descendants et compaction réelle des routes lorsque leur détail n'est plus requis. Réutiliser les accélérations et slots valides, invalider localement les parties modifiées, préserver verre et matériaux, identité des impacts et destruction. Aucun consommateur historique ne doit réexpanser les descendants déjà remplacés. Conserver les sources en DDR4/VRAM tant que le budget et leur utilité le permettent ; ne pas introduire de quota d'occupation. Ne pas altérer le fonctionnement végétal acquis.

### 9 — Qualification

Une version cohérente à compiler/tester à l'issue de l'intégration verticale. Utiliser `pull_compile_run.bat lod` : 20 extents, 6 s warmup puis cinq blocs de 16 s (complet, logo, continent, arbres, végétation), quatre segments de 4 s, marche à 5 km/h et pauses/sauts existants, mêmes réglages. Comparer au dernier run complet LOD-23 (et LOD-00/21 si utile), pas au crash. Isoler logo seul et scène complète par segment bord/forêt/approche/pause ; distinguer temps GPU de débit d'images soumises et transitions. Diagnostic compact : candidats et descendants architecturaux actifs/parkés, agrégats par niveau et par domaine, raisons de raffinement si utile, meshlets natifs/MG2, Depth/Opaque.Resident, CPU/GPU, sélection/readback/publication, CLAS/BLAS/PTLAS, résidence et cache. Relever la stabilité en déplacement et à l'arrêt, notamment le creux rapporté 72→22→72. Contrôle visuel de l'utilisateur : qualité proche inchangée, silhouette et fenêtres à moyenne distance, intérieurs visibles, absence de trous ou doubles représentations. Ne pas imposer un nombre d'agrégats ni sacrifier la fidélité pour un compteur. Si le parcours ne couvre pas les distances où une coupe est admissible, signaler la limite et qualifier la distance utile séparément sans changer la baseline. Vérifier absence de régression végétale ; pas de batterie QA disproportionnée.

### Points d'entrée à vérifier dans main

- `src/app/BuildingSession.*`, `BuildingCampus.h`, `Mg2Catalog.*`, `Mg2Runtime.cpp`.
- `src/core/building/BuildingPlan.*`, `BuildingGeometry.cpp`, `SectorBake.h` : recettes, secteurs, cache.
- `src/core/Assets.h`, `WorldRegistry.*`, `GpuSceneCache.*`, `TerritorialPatrimony.h`, `Mg2Pages.*` : identités, sources, versions, hiérarchie.
- `src/core/GraphicsMg2Forest.cpp`, `GraphicsInternal.h`, `GraphicsResidentTiles.cpp`, `GraphicsRayTracing.cpp`, `GraphicsPrepare.cpp`, `GraphicsRender.cpp`, `GraphicsFrameGraph.cpp`, shaders `mg2_select.hlsl`, `mg2_territorial.hlsl` et consommateurs depth/opaque/ray.
- `src/app/AppSession.cpp` : benchmark et journal ; conserver le parcours et les masques existants.

## 6. Fonctionnement du dispatch

Un seul chantier LOD-22, sur main exclusivement, commits cohérents de progression si nécessaires, pas un commit par ligne du tableau. L'agent détermine les opérations concrètes après lecture du code et analyse amont/aval. Les tâches du tableau sont un ordre de dépendances, pas des sous-billets. Actualiser ce fichier dans chaque commit significatif : statut exact (non intégré / partiel / intégré / testé / qualifié), emplacement ou décision technique, limite et prochaine opération. Pas de tests utilisateur intermédiaires systématiques ; demander le benchmark lorsqu'une version verticale est testable. Ne pas prétendre compiler sous Windows si ce n'est pas possible. Ne pas réintroduire les limitations ou les fallbacks végétaux supprimés. Le dernier paragraphe de chaque intervention doit contenir l'ordre précis à copier au prochain agent, en cas de perte du contexte.

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


## 12. Reprise

**Ordre actuel :** lire `PLAN_LOD.md` sur `Tabernacle666/PooLabVR/main`, engager LOD-22 à partir de la première tâche non intégrée du §5, vérifier le code et ses dépendances amont/aval, intégrer une progression cohérente sur main avec actualisation des statuts du plan ; ne pas retoucher la végétation LOD-23 ni déclencher LOD-31/32 avant la qualification de la version architecturale.
