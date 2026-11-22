# OASIS — Plan d'intégration photoréaliste, diversité et interactions (2026-11-22)

**Décision : privilégier les ressources photogrammétriques et PBR existantes, légalement réutilisables, plutôt que dessiner artificiellement des matériaux qui existent déjà.** La variation procédurale sert aux états d'usage, au placement, à la personnalité d'une pièce et aux rares accidents, jamais comme substitut obligatoire aux textures de référence.

## 0. État observé et limites

À la lecture du dépôt au 22 novembre 2026 :
- `assets/packs/oasis_mg2_v1/pack.json` annonce **116 GLB / 28 recettes** (révision 8), dont mobilier, architecture, services, forêt et props.
- `assets/packs/oasis_mg2_v1/runtime.json` connecte actuellement `office_desktop`, `meeting_table` et deux rangées `forest`. D'autres recettes sont des descriptions préparatoires, **pas** des salles déjà peuplées en jeu.
- `assets/packs/oasis_dressing_v1/` possède **100 affiches originales**, 80 nuances de blanc, et `wear_rules.json`. Pas encore de sélection/affichage des posters par salle dans le runtime.
- `assets/packs/oasis_characters_v1/` possède un humain riggé de **19 os, 7 clips** et un raton gris de **22 os, 4 clips**. Les clips sont procéduraux et les poids sont mono-articulation : leur présence dans le dépôt ne prouve pas leur animation en jeu ni une qualité photoréaliste.
- `src/core/Assets.cpp` charge déjà le coeur glTF metallic-roughness (base color, normal, metal/rough, AO, emissive), les animations, les skins, les transformations UV et les textures BasisU si décodables. Il faut cependant vérifier en jeu la fidélité du shader, des couleurs, des reflets et des extensions avancées.
- Les captures OASIS montrent des volumes cohérents, mais des matières encore trop plates, des pièces peu meublées, des feuillages répétitifs et des ombres trop agressives.

**Le présent document est un plan et un catalogue de sources. Il n'affirme pas que les nouvelles textures externes ont été téléchargées, importées ni visibles.**

## 1. Source de vérité et droits

Voir **[sources_reelles_candidates.json](sources_reelles_candidates.json)** : 27 pages ou bibliothèques réelles identifiées, avec destinataires, liens, licences et statut explicite `not_imported`.

Priorité :
1. **Poly Haven** — CC0 pour textures, HDRI et modèles : https://polyhaven.com/license
2. **ambientCG** — CC0 pour fichiers téléchargeables et prévisualisations : https://docs.ambientcg.com/license/
3. **cgbookcase** — matériaux CC0, utile pour rayures/fingerprints et matériaux industriels : https://www.cgbookcase.com/textures
4. **Fab** — seulement quand le contenu CC0 n'est pas à la hauteur. Vérifier la licence *de l'offre acquise*; le contenu Fab Standard peut être incorporé à un jeu, **pas distribué séparément en tant que pack d'assets public** : https://www.fab.com/eula
5. **Mixamo / Adobe** — animations pour humanoïdes bipèdes, autorisées en jeu suivant la FAQ; accès par compte et restrictions sur la distribution des fichiers sources à vérifier : https://helpx.adobe.com/creative-cloud/faq/mixamo-faq.html

Pour chaque fichier réellement acquis : écrire `source_url`, `provider`, `asset_id`, `license_name`, `license_url`, `download_date`, `sha256`, `master_resolution`, `maps_present`, `author`, `acquisition_entitlement`, `distribution_scope`. Un catalogue de pages n'est **pas** une preuve d'acquisition.

**Ne jamais committer des fichiers Fab/Mixamo achetés ou soumis à conditions en clair dans un dépôt public**. Les sources CC0 peuvent rester dans Git LFS (ou le stockage d'assets lié) avec leur provenance. Pour les médias CC-BY, conserver l'attribution. Ne pas copier sans droit des couvertures de magazines, photos de familles, marques ou écrans Microsoft.

## 2. Lot 1 — Bibliothèque de matériaux PBR réels

Construire un master pour chaque famille, puis des variantes d'état (neuf, utilisé, usé, réparé), **à partir de scans PBR existants**. Premières sources précises :

| Famille | Source de départ | Affectations OASIS |
| --- | --- | --- |
| Parquet propre | https://polyhaven.com/a/herringbone_parquet | bureaux, salle de réunion |
| Parquet usé | https://ambientcg.com/view?id=WoodFloor003 ; https://polyhaven.com/a/old_wooden_floor_02 | couloir, centre de passage |
| Bois de meuble | https://polyhaven.com/a/wood_table_worn | dessus de bureau, bibliothèque |
| Porcelaine | https://ambientcg.com/view?id=Porcelain001 ; https://ambientcg.com/view?id=Porcelain002 | tasse, lavabo, sanitaires |
| Métal brossé | https://ambientcg.com/view?id=Metal010 | penture, poignée, électroménager |
| Métal peint rayé | https://www.cgbookcase.com/textures/scratched-painted-metal-01 | porte d'ascenseur, armoire |
| Moquette / tapis | https://ambientcg.com/view?id=Carpet001 ; https://polyhaven.com/a/dirty_carpet | bureaux et zones de circulation |
| Béton | https://ambientcg.com/view?id=Concrete003 ; https://polyhaven.com/a/concrete_floor | sous-sols, cages, structures |
| Gypse | https://ambientcg.com/view?id=PaintedPlaster017 ; https://polyhaven.com/a/painted_plaster_wall | peinture murale, reprises |
| Plastique | https://ambientcg.com/view?id=Plastic004 | claviers, souris, carters |
| Tissus | https://ambientcg.com/view?id=Fabric010 ; https://polyhaven.com/a/rough_linen | fauteuils et canapé |
| Imperfections | https://ambientcg.com/view?id=Fingerprints001 ; https://ambientcg.com/view?id=Scratches005 | poignées, vitres, couloirs |

**Pipeline d'import** : télécharger le jeu complet de cartes de la source, inspecter la physique du matériau et les dimensions de scan, puis construire un asset PBR commun. Base color et emissive en sRGB; normal/roughness/metallic/AO en linéaire. Pour glTF, metallicRoughness : **G = roughness**, **B = metallic**, AO facultatif en **R**; comparer orientation normal OpenGL/DirectX avec le shader effectif. Préparer mipmaps et compression GPU BC7 (albedo), BC5 (normales), BC4/BC7 ou packing ORM selon le support moteur. Les masters peuvent être en haute résolution; seules les pages/mips nécessaires deviennent résidentes en VR.

Respecter l'échelle réelle des texels (veinage du bois et fibres de tapis) et continuité UV entre modules. Ne pas affecter une texture de béton extérieur à un gypse intérieur sous prétexte que les deux sont gris. Ne pas ajouter `KHR_materials_clearcoat`, transmission ou anisotropie en supposant que le shader courant les interprète : prévoir un contrat moteur explicite ou un repli honnête glTF standard.

**Point d'arrêt visuel :** un bureau pilote, un couloir de parquet, un lavabo et une porte métallique sous lumière variée avant la généralisation.

## 3. Lot 2 — Vieillissement basé sur l'usage

Réemployer `assets/packs/oasis_dressing_v1/wear_rules.json` et `white_shades_80.json` : ne pas recréer un système rival.

- `room_id + host_object_id + surface_role + event_channel` → graine persistante; mêmes défauts tant que le monde n'a pas changé.
- **Parquet** : passage et mouvements de chaises, zones centrales plus ternes; usure des seuils.
- **Tapis** : fibres écrasées au centre, pâlissement, bordures propres ou poussiéreuses selon entretien.
- **Gypse** : contact au dos des chaises, coin abîmé, trace de fixation.
- **Métal** : contact des doigts, sens du brossage, choc de chariot et bosse d'ascenseur localisée.
- **Porcelaine** : calcaire proche du robinet, micro-rayures et éclat rare sur tasse/pot.
- **Détail exceptionnel** : clou apparent uniquement près d'une jonction compatible, jamais tiré au sort par pixel.

Sous forme de **paramètres par instance** (nuance, âge, humidité, contact, accident) et décals héritant d'une surface réelle; jamais une nouvelle copie de matériau complet pour chaque tasse. Les impacts de jeu, trous et modifications ont priorité sur le décor initial. Ne pas dessiner les décals d'un objet non résident.

## 4. Lot 3 — Densification des bureaux et variété utile

Réutiliser les **116 GLB existants**, puis acquérir/adopter des modèles réels (ex. https://polyhaven.com/a/fancy_picture_frame_01 et https://polyhaven.com/a/metal_office_desk). Pour des props animables du commerce, exemple réel : https://www.fab.com/listings/43149bbb-fb54-485c-b5d8-53c730eb3fa3 — tiroirs/chaises à roulettes séparés mais droits et téléchargement à vérifier avant usage.

Profils d'occupation : `organise`, `bazar`, `direction`, `creatif`, `technique`, `quasi_vide`, `abandonne`. Les profils répartissent des **familles** d'objets, pas des répétitions identiques : chaise gaming à roulettes, souris + tapis, portable, casque, cadre personnel, veste, sac/valise, revues, outils (pinces, tournevis, pelle ponctuelle), mugs, câbles, mobilier d'appoint, magazines, dossiers, plantes et poussière.

Chaque entrée inclut une **probabilité conditionnelle** (poste occupé, fonction de salle, meuble hôte, âge, accessibilité) et un placement stable respectant les dégagements et les portes. Aucune obligation de remplir 100 % des pièces; la rareté participe aussi à la crédibilité. Collision et usage futur influencent la place des objets (ne pas encombrer l'ouverture des tiroirs).

Le moteur expose déjà `office_desktop` et `meeting_table` dans `runtime.json`. En premier, ajouter les objets utiles à ces deux surfaces, par petits **lots cohérents** plutôt qu'un commit par objet. Quand les nouvelles cibles seront disponibles, activer successivement `office_room`, `corridor_wall`, `floor`, `door`, `cabinet_shelf`, `lounge`, `kitchenette` avec les 28 recettes; **ces noms sont des demandes de contrat, pas des cibles runtime déjà actives**.

## 5. Lot 4 — Quincaillerie et objets prenables

Articuler les masters d'origine : porte et dormant séparés, charnières et pivots physiques, poignée indépendante; tiroir coulissant, lampe à segments, roulettes de chaises. Pour chaque objet, sidecar **à faire adopter par le moteur** :
- `asset_key`, révision, unités Y-up, pivot local;
- `interaction_class` : `rigid_grab`, `hinge`, `slider`, `button`, `screen`, `static`;
- `socket_grab_left/right`, `socket_handle`, `socket_hinge`, `socket_open_axis`, `socket_screen`;
- masse, centre de masse, inertie (ou procédure d'estimation), collision proxy fermée, frottement/restitution, bornes d'ouverture, angle de repos;
- hiérarchie d'articulations, limites de force/couple, états persistants (ouvert/fermé, brisé, déplacé).

**Séquence d'intégration de physique** : tasse → revue → petite boîte → outil → valise → tiroir → porte. Les objets restent de simples entités de décor tant qu'ils ne sont pas réveillés; création/résidence physique à la prise ou à l'approche. Pas d'aller-retour CPU-GPU permanent imposé à tous les objets. Un objet lancé doit garder son identité, son matériau et son état. Poignées et accès doivent fonctionner aussi en VR assise.

Les sockets écrits dans les GLB ne rendent pas la poignée interactive à eux seuls. C'est un contrat à relier au contrôle et au moteur physique.

## 6. Lot 5 — Affiches, cadres, écrans et environnement

**100 affiches existent déjà** dans `oasis_dressing_v1`; d'abord les brancher sans afficher les vingt variantes de chaque atlas simultanément. Installer les cadres d'origines CC0, puis enrichir en images dont les droits sont prouvés et qui correspondent à l'histoire d'OASIS : loups, paysages, sécurité, tableaux d'entreprise, citations, organigrammes et propagande dystopique fictive. Pas de reproduction sans licence d'affiches réelles.

Écrans : couches d'affichage indépendantes de la géométrie (écran éteint, bureau fictif, terminal, état d'erreur, veille rétro : labyrinthes, étoiles, tuyaux ou rebonds **originaux**, sans redistribuer les ressources de Windows 95). `socket_screen`, UV dédiés, signal émissif à intensité crédible, fréquence de mise à jour contextuelle basse pour les écrans hors attention. Derrière une fenêtre distante, afficher une représentation de pièce par parallaxe/émission, **sans charger l'intérieur réel**.

## 7. Lot 6 — Humain et raton laveur

Le pack `oasis_characters_v1` fournit déjà deux **GLB animés de référence**; ne pas reconstruire ces deux squelettes pour le principe. Les révisions photoréalistes exigent meilleur maillage, UV et matériaux réels, poids de skinning fluides sur plusieurs os, contrôle IK/contact des pieds et retargeting.

**Humain** : conserver identifiants d'état marche, escaliers haut/bas, assis, transition debout. Sélectionner un humain de meilleure qualité aux droits vérifiés; animation bipède à sélectionner et exporter depuis https://www.mixamo.com/ (FAQ https://helpx.adobe.com/creative-cloud/faq/mixamo-faq.html). Vérifier chaque clip souhaité, notamment **escalier**; pas de fausse équivalence entre « marche sur place » et montée réelle. Les escaliers exigent contact pied/marche et déplacement racine gérés par le moteur.

**Raton gris** : conserver les clips de base. Candidat de modèle riggé/animé existant (licence/format à valider) : https://www.fab.com/listings/b6b046c5-f471-4397-8137-80c31ff3fac6 . Fourrure crédible, silhouette basse, masque facial, queue annelée; inertie du corps, pose de marche, reniflement. Animation quadrupède séparée du pipeline Mixamo, qui ne prend en charge que les bipèdes.

Le moteur importe les données glTF animées mais doit encore brancher le système de personnages/IA; ne pas intégrer les acteurs au pack statique d'immeubles.

## 8. Lot 7 — Environnement urbain, grande forêt et résidence

Séparer cinq budgets : population logique, représentation apparente, géométrie résidente, physique active et géométrie traversée par les rayons. Une ville et des millions d'arbres ne doivent pas devenir des millions de BLAS, textures, mises à jour ou draw calls par image.

- Bâtiment/étage/pièce : requêtes de visibilité et ouverture **avant** promotion de la pièce en contenu 3D; budget transitoire sur seuils, trous et ascenseurs; calendrier de préchargement fondé sur vitesse d'approche. Les fenêtres éloignées gardent un imposteur/parallaxe cohérent avec l'éclairage réel.
- Forêt : population stable par tuile, tailles et espèces immuables, variantes sources communes, streaming de clusters MG2, arbres proches animés et ombre simplifiée/résidente selon signification perceptuelle. Les modèles hero restent hors peuplement standard avant mesure.
- Transparence, ombres, imposteurs et rayons : décider explicitement quels objets participent aux ray structures; priorité aux éléments visibles et modifiables.
- Détruire une paroi ou prendre un objet invalide **localement** la représentation, plutôt que réinitialiser toute la pièce/la forêt.

Le SDK RTX MG2 montre le streaming de clusters de triangles sur DX12/Vulkan (https://github.com/NVIDIA-RTX/RTXMG); il ne décide pas tout seul de la vie des pièces, de leurs accès ou de la physique.

## 9. Ordre d'exécution et responsabilités

| Ordre | Livrable concret | Équipe asset | Équipe moteur | Critère d'admission |
| --- | --- | --- | --- | --- |
| **0** | Catalogue de provenance et 12 premières sources CC0 | sources, droits, maps complètes | vérifier import glTF PBR existant | preuve de licence et rendu fidèle d'un matériau |
| **1** | Bureau + couloir + sanitaire PBR pilote | UV/texel density, matériaux maîtrisés | shader/reflets, pages textures, mapping gamma/normal | matériaux identifiables de près ET de loin |
| **2** | Usure contextualisée et blancs variés | décals/masques issus de sources réelles | graine stable et paramètres d'instance | défauts rares, répétition non évidente |
| **3** | Lot bureaux, réunion, communs | objets réels et profils de salle | nouvelles cibles de `runtime.json` | salles réellement meublées, dégagements libres |
| **4** | Interactions physiques | pivots, proxy, sockets, metadata | grab, haptique, PhysX/contraintes, état persistent | objet pris/lancé; tiroir/porte actionnables |
| **5** | Affiches et écrans | index 100 variantes, photo/surface écran | atlas/submesh et contenu dynamique des écrans | une seule image par cadre, pas 20 coplanaires |
| **6** | Humain et raton | vrai mesh déformable, clips/retarget | skinning, navigation, IK, animation | marche/escaliers/sit sans glissement excessif |
| **7** | Généralisation à la ville/forêt | variantes à qualité constante | visibilités, streaming, MG2/BLAS, parallaxe | charge liée à l'observable plutôt qu'au monde entier |

**Livraisons** : groupes cohérents sur `main`, génération et provenance reproductibles, pas de commits isolés « juste ajouter un objet », pas de retouche du moteur par l'équipe asset. Chaque groupe est installable et attaché à une cible déjà disponible ou marqué `pending_engine_target`.

**Gates existantes uniquement** : `node tools/sync_oasis_mg2_catalog.mjs` pour les métadonnées et `commit_and_loop.bat "mg2"` pour les vues et la télémétrie quand le moteur bouge. Contrôle visuel en VR 72 Hz sur Quest 3S / RTX 5060 Ti 16 Go avant généralisation; noter charge CPU/GPU, VRAM, sources physiques préparées, matériel résidant et temps de première apparition. Aucune réduction arbitraire du nombre d'arbres ou de contenu n'est un correctif de pipeline.

### Contrat de livraison entre équipes

Pour chaque lot, l'équipe asset écrit dans le répertoire du pack un `HANDOFF_<lot>.md` contenant : sources exactes et statut de licence, inventaire, identités/sockets, matériaux, pivots, masses et collision suggérées, recettes cibles, conditions de promotion à la résidence, mode de prévisualisation et dépendances moteur. Le moteur répond par les cibles nouvelles dans `runtime.json` ou un contrat annexé. On peut alors raccorder les assets directement sans refaire un importeur pour chaque nouveau meuble.

### Limites et dépendances non résolues

- Les textures listées sont **des choix repérés en ligne**, pas des fichiers téléchargés ou examinés en 3D.
- Les matériaux de seconde couche (clearcoat porcelaine, anisotropie des métaux, transmission verre) demandent support shader explicite, sinon repli glTF standard.
- Les collisions, joints, mains et retarget sont des **objectifs moteur**, pas des conséquences du nom des sockets.
- Les recettes préparatoires peuvent rester inactives jusqu'à création de leurs cibles.
- La densification des forêts, les 100 affiches et les personnages n'impliquent pas automatiquement une performance ou une fidélité AAA.
