# OASIS — Plan directeur LOD / géométrie territoriale

**Révision du plan : 28 novembre 2026. LOD-23 en cours : refonte du catalogue MG2 ouvert autorisée ; prochain travail interne : capacité multi-arènes et raccord des routes collectives aux consommateurs, après récupération des payloads obsolètes.**

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
| 2 | **LOD-23** | **PARTIEL — NON QUALIFIÉ — À TERMINER** | [f9e349e7](https://github.com/Tabernacle666/PooLabVR/commit/f9e349e7643b6d7dd3bc95b253c3b4b65e5541fb) ; refonte du catalogue ouvert autorisée, puis sélection/publication territoriales. Voir suivi §5. |
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

**Statut : PARTIEL — NON QUALIFIÉ.** L'intégration partielle ci-dessous ne clôt pas le billet et ne déclenche pas LOD-22. Les quatre opérations historiques restent absorbées dans LOD-23. L'agent choisit l'ordre de ses opérations internes, examine main et l'amont/aval, et tranche en fonction du comportement réel sur RTX, sans ajouter des sous-billets de dispatch.

### État de l'intégration partielle (f9e349e7643b6d7dd3bc95b253c3b4b65e5541fb)

- Producteur : sous-cellules directionnelles à demi-pas ; une fusion n'est conservée que si l'erreur de surface projetée sur chacun des trois axes n'augmente pas par rapport aux sous-patches. Les triangles supplémentaires gardent leurs UV, matériaux et supports ; les groupes sont déterministes. Cache territorial versionné `lod23-v2` (les caches source MG2 et terrain restent intacts). Cela ne valide ni la transmittance ni la couleur et peut augmenter le coût CPU / mémoire de préparation.
- Reconstruction locale : après une mutation/effacement du registre, retrouver les identités vivantes des feuilles touchées via l'index résident, reconstruire ces feuilles depuis les sources partagées, supprimer les parents vides et recalculer les ancêtres affectés. Cette reconstruction est synchrone sur le thread de session lors d'une mutation, pas un parcours par frame ; il reste à qualifier son coût et le cas des nouvelles insertions en dehors du producteur d'extents.
- Fallback diagnostic : retirer le masque de scène du registre de sélection MG2 ; la coupe complète reste compatible avec les masques successifs, qui sont appliqués dans les consommateurs raster et rayons existants. Le journal `lod_mg2_mask` indique `mask_independent_cut=1`. Les métriques de sélection concernent désormais la coupe complète, pas la seule catégorie visible ; comparer le travail raster/RTX par scène comme avant. Le fallback initial, les nouvelles routes non encore publiées et la reconstruction de descripteurs RTX lors du changement de masque restent à traiter.
- **Non réalisé :** publication dynamique des vrais triangles collectifs dans le catalogue MG2 et ses accélérations, sélection territoriale GPU par racines avec coupes raster/rayons indépendantes, substitution atomique des descendants, élimination du parcours et du readback individuels, cache des produits parents et qualification optique incluant silhouette/transmittance/couleur. Les proxies CPU ne sont toujours pas rendus ; ne pas attribuer de gain visuel ou de FPS à leur production.
- Validation : inspection statique des consommateurs et des transactions ; compilation Windows/DX12, test de mutation et benchmark VR non exécutés dans cet environnement. Aucun test intermédiaire n'est demandé au joueur : le benchmark `pull_compile_run.bat lod` est réservé à la version complète de LOD-23. Aucun gain n'est déclaré sans mesure.

### Décision architecturale autorisée — catalogue MG2 ouvert

Le monde extensible est arrivé après la conception du catalogue géométrique fermé. Le propriétaire du projet autorise explicitement la levée de ces invariants historiques, y compris les modifications transversales nécessaires pour LOD-23. Ne pas construire un second moteur de rendu spécialisé dans les forêts. Le catalogue doit accepter des représentations individuelles ou collectives produites après le démarrage, sans reconstruire le patrimoine inchangé.

**Contrat joueur :** même lancement OASIS.exe, commandes, réglages, monde, population, interactions, collisions, destruction et effets. Pas de manipulation ou de test supplémentaire demandé au joueur pour cette migration. Les gains attendus concernent le travail de sélection/publication et la scalabilité ; la transparence visuelle, le démarrage, la stabilité et les performances restent à vérifier, pas à présumer.

| Invariant historique à lever | Invariant de remplacement |
|---|---|
| Catalogue complet au démarrage (ConfigureForest / forestHierarchy) | Catalogue ouvert, segments immuables individuellement, publication locale et asynchrone |
| Ressource sparse unique dimensionnée par le catalogue initial | Stockage extensible indépendant du catalogue global ; segments ou espaces virtuels stables, allocation physique à la demande |
| SourceIndexOffset dépend du nombre total de sommets | Adresses et références stables pour les segments déjà publiés ; pas de déplacement des indices à chaque croissance |
| Identifiants de clusters issus d'une numérotation locale au bake | Identité stable et non ambiguë des clusters / révisions, compatible avec les caches CLAS/BLAS |
| Route MG2 = objet physique individuel | Route de représentation distincte de l'identité physique ; agrégat sans nouvel objet de simulation |
| Sélection démarrant aux instances, coupe raster/RTX imposée commune | Racines territoriales et coupes par domaine ; publication cohérente mais indépendance des choix optiques |

Ne pas lever l'immuabilité du contenu des clusters publiés : une modification produit une nouvelle révision. Conserver les identités et sources physiques, les caches persistants réutilisables, les CLAS/BLAS valides, la résidence selon le budget DXGI réel et les garanties de durée de vie par fence. Le stockage doit permettre de conserver les ressources coûteuses tant que la pression réelle ne justifie pas leur éviction. Pas de nouveau plafond arbitraire de VRAM ou de population.

Références : [NVIDIA RTX Mega Geometry](https://github.com/NVIDIA-RTX/RTXMG), [NVIDIA RTX whitepapers](https://github.com/NVIDIA-RTX/Whitepapers), [D3D12 reserved resources](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource). Ces mécanismes permettent de choisir l'architecture ; ne pas prétendre disposer du code propriétaire de la végétation de The Witcher 4.

### Suivi interne de LOD-23 (pas de nouveaux billets de dispatch)

Ordre technique indicatif, ajustable selon les dépendances. Mettre à jour les statuts dans ce fichier au fur et à mesure des changements effectifs sur main. INTÉGRÉ signifie présent dans le code, pas compilé ou qualifié ; PARTIEL signifie incomplet ; AUDITÉ signifie inspecté sans implémentation. Référencer le commit ou le commit contenant la mise à jour. Les étapes ne prescrivent ni un commit par ligne ni un test utilisateur par ligne.

| Ordre | Travail interne | Statut / référence |
|---:|---|---|
| 0 | Audit du catalogue fermé et de ses dépendances ; levée des invariants autorisée | AUDITÉ — décision du présent plan |
| 1 | Adressage géométrique indépendant de la taille globale ; allocation/résidence extensible et durée de vie GPU | PARTIEL — 9da34fed espaces sparse indépendants ; récupération des pages mortes et conservation des pages réutilisables selon pression intégrées dans le commit contenant cette révision ; multi-arènes reste à réaliser ; non compilé |
| 2 | Catalogue Mg2Pages extensible, identités/révisions stables, enregistrement et retrait local de segments, snapshots de producteurs | PARTIEL — append/snapshots 32c8b322 ; admission CPU et retrait logique 1b9aa2c5 ; libération des payloads morts, des références cache et des tuiles devenues inutiles intégrée ; récupération des plages virtuelles et métadonnées tombstonées / multi-arènes à faire |
| 3 | Adaptation des consommateurs communs raster, CLAS/BLAS, OMM, hit shading, PTLAS et caches à l'adressage ouvert | NON INTÉGRÉ — prochain avec finalisation des arènes |
| 4 | Correction du patrimoine optique bas et forestier, cache des produits et reconstruction incrémentale | PARTIEL — f9e349e7 ; reconstruction des feuilles touchées présente mais non testée, couverture/transmittance/couleur et coût restant à traiter |
| 5 | Routes collectives distinctes de la simulation, sélection hiérarchique GPU et suppression de la traversée/readback des descendants couverts | NON INTÉGRÉ |
| 6 | Publication effective et atomique des agrégats et coupes raster/RTX, invalidation locale et retour sûr aux enfants | NON INTÉGRÉ |
| 7 | Élimination du fallback natif massif, y compris démarrage et nouvelles routes | PARTIEL — f9e349e7 ; conservation de la coupe lors des masques diagnostic, autres causes non traitées |
| 8 | Inspection amont/aval finale, compilation si disponible, qualification benchmark et visuelle de LOD-23 complet | NON QUALIFIÉ |

### Progression — séparation de l'adressage source (commit contenant cette révision)

- `Mg2Pages` : suppression de SourceIndexOffset ; deux flux logiques Vertices/Indices, chacun avec sa propre taille utile et ses propres offsets. Le packing des tuiles 64 Kio conserve les octets source, sans modifier les indices locaux des clusters ni l'ABI shader.
- `ForestSourcePool` : deux ressources sparse réservées indépendamment ; capacité virtuelle dérivée de l'ABI 32 bits et du support D3D12, réduction de la seule réservation virtuelle en cas de refus du pilote jusqu'à la taille nécessaire au catalogue. Aucune réduction de contenu et aucun plafond de VRAM physique ajouté. Les heaps physiques, clés de pages et références de génération distinguent maintenant flux + tuile ; les deux flux restent détenus par la génération ou son geometryOwner jusqu'à la retraite. Les mêmes pages alimentent raster, CLAS et standard DXR. Les descripteurs d'indices commencent à zéro dans leur propre ressource ; pas de changement HLSL, de matériaux, d'OMM ou de clés CLAS/BLAS.
- Diagnostic `mg2_a06_residency` : conserve les champs existants et ajoute `index_gpu_va`, `vertex_virtual_bytes`, `index_virtual_bytes`. Les compteurs de résidence/demande/upload restent physiques et agrégés ; les deux réservations virtuelles ne sont pas de la VRAM consommée.
- **Limite réelle** : le catalogue reste construit au démarrage. Lors de l'ouverture du catalogue, réserver des offsets append-only et sceller les dernières tuiles de chaque segment (y compris l'alignement des éléments Vertex et des indices), afin de ne jamais modifier une tuile encore référencée. Prévoir une arène/segment supplémentaire lorsque la réservation effective ou l'ABI d'adressage est épuisée ; le code actuel refuse explicitement une croissance hors capacité plutôt que relocaliser les données actives. Cette admission multi-segments et les snapshots de catalogue restent à intégrer. L'éviction des blocs froids conserve la politique existante à ce stade.
- Validation : revue statique des correspondances entre demande, packing, mappings, CLAS, BLAS standard, descripteurs et durée de vie. Compilation Windows/DX12, validation GPU/VR et gain de performance non vérifiés. Aucun test joueur intermédiaire.

### Progression — snapshots et append de segments (commit contenant cette révision)

- `Mg2Pages::Extend` accepte un segment déjà baké indépendant et, optionnellement, une correspondance entre ses meshes locaux et des IDs de représentation jamais utilisés. Copie les métadonnées du catalogue et partage les anciennes pages (aucune copie de leur payload vertex/index ni rebake). Les IDs et offsets existants restent stables ; seuls les nouveaux IDs, références DAG, plages GPU et offsets sont translatés. Les deux flux commencent le segment sur une frontière commune élément/tuile 64 Kio (PPCM), évitant de réécrire une tuile partiellement remplie d'une ancienne génération. Refus explicite des collisions et des dépassements d'ABI.
- `forestHierarchy` et les requêtes sélection/packing/build utilisent des snapshots partagés const. Le worker RTX lit exactement le catalogue qui a produit la coupe, et non le pointeur global courant ; les descripteurs/CLAS utilisent ce snapshot. Le worker de sélection renouvelle les tables GPU lorsque le snapshot change, conserve les préfixes GPU par copie GPU et envoie seulement les suffixes CPU en cas d'append ; les tables inchangées sont réutilisées. La voie OMM initiale conserve le même mécanisme de bake.
- **Reste à faire :** aucun producteur territorial n'appelle encore Extend ; il faut construire les segments à partir des patches réellement publiables, allouer des IDs de représentation et des bounds/materials indépendants des meshes natifs, admettre les segments sur la capacité réelle des deux espaces sparse, et publier le snapshot sur le propriétaire rendu entre travaux. Gérer le remplacement/retrait logique et la récupération des segments morts sans réutiliser prématurément des IDs. Prévoir un adressage multi-arènes si la réservation virtuelle a été réduite par le pilote ou l'ABI est épuisée. Les consommateurs et les sélections de descendants restent à adapter ; aucune nouvelle géométrie territoriale n'est rendue.
- Validation : revue statique des translations d'IDs et d'offsets, du packing et des références de workers ; compilation Windows/DX12 et exécution GPU/VR non réalisées. Aucun test intermédiaire joueur.

### Progression — admission CPU du patrimoine territorial dans MG2 (commit contenant cette révision)

- `TerritorialPatrimony` partage les nœuds modifiés avec un journal latest-only (clé territoriale, nouveau snapshot ou tombstone) ; publication des branches locales et effacements alimentent le journal. Les patches, routes et enfants ne sont pas copiés pour le transfert au worker. La validité est modifiée exclusivement par le propriétaire du registre et n'est jamais lue par le worker. `WorldRegistry::Contributions` expose le producteur au rendu sans transférer la simulation.
- `Mg2Pages::FromAuthoredTriangles` transforme les triangles déjà ajustés en clusters MG2 exacts (126 sommets non partagés / 42 triangles max, groupes de 32 clusters), sans nouvelle simplification optique. `PrepareTerritorialCatalog` regroupe chaque nœud par matériau/type, utilise un repère local et des bounds conservatifs, attribue des identités de représentation descendantes distinctes des meshes natifs, conserve les erreurs géométriques/optiques territoriales et étend le catalogue sur un worker CPU. L'erreur interne de ces pages est nulle relativement au proxy fourni, pas aux descendants.
- Le propriétaire rendu admet les snapshots après vérification de la capacité virtuelle des flux existants et du préfixe stable ; le registre actif est mis à jour par clé avec le snapshot des routes/enfants, les anciennes révisions sont retirées logiquement sans réutiliser leurs IDs. Le journal invalide immédiatement les candidats modifiés ; une publication supersédée ne réactive pas la représentation obsolète. Le transfert des nœuds de map est préalloué ; un échec préserve l'ancien catalogue et remet les changements en attente, les plus récents priment. Le worker territorial démarre après la première publication MG2 native pour ne pas concurrencer son démarrage. Une nouvelle liaison de producteur ou réinitialisation du renderer repart d'un snapshot de nœuds même si le journal a été consommé. Un diagnostic `lod23_catalog` rapporte les publications/rejets.
- **Limites :** pas encore de sélection ni de route collective vers le raster/RTX (active_routes=0). L'admission est CPU : les triangles ne sont pas encore construits en CLAS/BLAS et ne sont pas affichés. Les révisions mortes restent dans les snapshots append-only ; mettre en place la récupération locale/multi-arènes avant de déclarer la scalabilité. La capacité insuffisante est un rejet explicite et non une baisse de contenu. Les métadonnées CPU sont copiées à chaque batch et les tables GPU peuvent croître ; mesurer leur coût réel et éviter les copies intégrales à mesure de la croissance. Les matériaux alpha-tested conservent leurs UV, mais OMM/UV secondaire/qualification optique ne sont pas résolus. L'admission invalide les candidats obsolètes dès consommation du journal ; à la prochaine étape, propager ces tombstones/révisions jusqu'aux coupes raster/RTX et préserver une coupe descendante valide jusqu'à publication.
- Validation : inspection statique seulement ; aucune compilation Windows/DX12, aucun benchmark, aucun gain déclaré. Aucun test utilisateur intermédiaire.

### Progression — retrait des payloads et résidence selon pression (commit contenant cette révision)

- `Mg2Pages::RetireMeshes` détache les pages source des représentations obsolètes du nouveau snapshot, retire leur entrée du registre et du packing. Les anciens snapshots restent propriétaires tant que leurs workers les utilisent. Les IDs de clusters restent monotones même après retrait, les IDs de représentation ne sont jamais recyclés, les préfixes GPU sont identifiés par les segments et les offsets antérieurs ne changent pas. Les métadonnées GPU de pages mortes sont encore des tombstones et les high-water marks virtuels ne diminuent pas. Les payloads vivants ne sont pas rebakés.
- Le journal de retrait accompagne les remplacements, suppressions et candidats supersédés ; le worker prépare un snapshot de retrait même sans nouveaux triangles. La sélection déclenchée par le changement de catalogue élimine les entrées CLAS/BLAS obsolètes de leurs caches sans toucher aux générations en vol ; les allocations communes restent détenues par les entrées ou générations qui en ont besoin. Les comptes cache sont recalculés sans rajeunir les entrées.
- `TrimForestSourceBlocks` traite les blocs 64 Kio physiques agrégés : un bloc intégralement mort est libéré dès qu'aucune génération ne le référence, les blocs encore réutilisables restent résidents sauf pression DXGI réelle ; éviction des plus anciens blocs froids selon le besoin. Le démapping reste sur la queue producteur et les heaps survivent à sa fence. Les blocs partagés avec des pages vivantes restent réutilisables ; un bloc détenu par une génération n'est jamais évincé. Diagnostic `lod23_source_reclaimed` pour les retraits hors build.
- **Limites :** pas de récupération des adresses virtuelles ni des métadonnées GPU mortes à ce stade ; les capacités des deux arènes et l'ABI uint32 restent des limites, donc multi-arènes nécessaire. Les scans de cache et des blocs sont sur le worker série ; la collecte se produit à la prochaine sélection/build et progression des retraites, pas sur un timer. Les agrégats ne sont toujours pas sélectionnés ni rendus. Compilation Windows/DX12, exécution et mesures non réalisées. Aucun test utilisateur intermédiaire.

### Dépendances transversales à protéger lors de la refonte

- `Mg2Pages.h/.cpp` : `SourceIndexOffset` supprimé ; `SourceVirtualBytes(stream)`, `PackSourceTile(stream, ...)`, offsets et IDs attribués au bake, tables GPU de sélection. Les identifiants de clusters ne doivent jamais entrer en collision entre segments ou révisions ; éviter de réécrire une page physique encore lue par une génération.
- `GraphicsMg2Forest.cpp` / `GraphicsInternal.h` : `ConfigureForest`, `forestHierarchy`, `ForestSourcePool`, `PackForestCut`, `UploadForestPages`, `ForestGeneration`, workers et fences. Le catalogue et les tables consommés par une génération doivent rester cohérents jusqu'à sa retraite ; aucun accès concurrent à des vecteurs mutables. Les caches CLAS utilisent actuellement (ID de cluster, offset de primitive dans le cut), les BLAS une suite ordonnée d'IDs ; préserver la sémantique de GeometryIndex ou adapter ensemble construction et lecture. Ne pas invalider tout le cache lors d'un append.
- `common.hlsli`, `scene.hlsl`, `compute.hlsl` et `GraphicsRender.cpp` : `MeshResource`, `MeshletTriangle`, `LoadMeshRange`, `LoadInstanceStable`, `LoadCameraInstance`, `RayTexturedOpacity`, `LoadRayTriangle`, tables de descripteurs et indices de hit. Le chemin natif, les matériaux, la géométrie coupée et les objets déformables doivent continuer à fonctionner.
- `GraphicsFoliageOmm.cpp` : `FindFoliageOmm(mesh)` lie actuellement l'OMM au mesh source ; vérifier la compatibilité UV/matériau des nouvelles représentations, ne pas traiter le feuillage masqué comme opaque faute de micromap.
- `WorldRegistry.*`, `GpuSceneCache.*`, `GraphicsResidentTiles.cpp`, `GraphicsRayTracing.cpp` : identité physique et slots PTLAS actuels associés aux objets résidents ; distinguer la représentation collective de cette identité, maintenir les identités de collision/destruction, les masques et les contributions hors champ ; ne pas laisser les anciens chemins réactiver les enfants sous un parent actif.
- Préserver le cache existant quand ses données restent valides, les versions de format quand elles changent, le lancement et la croissance asynchrones ; vérifier les cas de remplacement, suppression, réapparition et publication interrompue. Si une décision technique reste bloquée, formuler une question précise à transmettre au propriétaire plutôt que de pousser une solution incomplète en prétendant le billet terminé.

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

Un billet vise une version cohérente testable sur main (sauf qualification LOD-43 sans correction). Exception explicitement autorisée pour la refonte LOD-23 : le plan est actualisé et les travaux internes intégrés sont marqués au fur et à mesure ; les commits de progression nécessaires doivent être cohérents, ne constituent pas de nouveaux billets et ne déclenchent aucun benchmark utilisateur avant l'intégration complète. Pas de qualification fictive ni de micro-commits décoratifs. Pas de branches, PR ni micro-commits de chaque opération interne. L'agent inspecte main, repère les emplacements réels, évalue les dépendances amont/aval, choisit les solutions adaptées au matériel NVIDIA sans déborder inutilement sur les billets suivants. Ne pas imposer une procédure de développement supplémentaire. Compilation lorsque disponible, sinon signaler explicitement la limite ; indiquer au testeur ce qu'il doit observer. Ne pas déclarer un gain non mesuré.

Ce plan est maintenu dans main : après chaque billet, mettre à jour son statut et sa référence dans ce même fichier (dans le commit du billet si possible, référence de commit complétée par le prochain agent si nécessaire). Distinguer intégré, testé et qualifié ; les décisions importantes ou limites restantes doivent être inscrites ici, pas seulement dans le chat. Le prochain agent reçoit uniquement l'identifiant du billet et le chemin de ce fichier.
