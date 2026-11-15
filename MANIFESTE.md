# OASIS

Moteur Windows DirectX 12. Démarrage dans un monde vide. Aucun jeu, décor ou contenu imposé.

## Phase 1 — Noyau sur écran

1. Démarrer et afficher une scène 3D sur écran sans casque ni runtime OpenXR actif.
2. Créer, déplacer et détruire des objets à l’exécution avec des identités stables.
3. Partager géométrie et ressources entre des milliers d’instances.
4. Charger les maillages, textures, matériaux, animations et sons sans dépendance à un jeu.
5. Produire des matériaux PBR : couleur, rugosité, métal, relief et transparence.
6. Faire coexister lumières, ombres et réflexions sans plafond arbitraire de lumières.
7. Animer transformations, squelettes et déformations en cohérence avec la physique.
8. Fournir collisions, corps rigides et articulations ; physique GPU sans repli CPU silencieux.
9. Simuler et afficher des millions de particules sur GPU avec les forces et interactions requises.
10. Garder simulation, contacts, animation et sons sur une même chronologie, indépendante des images affichées.
11. Faire travailler CPU et GPU en parallèle ; intégrer à l’image les résultats requis sans recopier tout l’état GPU vers le CPU à chaque image.
12. Ne pas imposer de plafond artificiel d’images par seconde ; viser 1 000 i/s sur une scène chargée définie, sans retirer silencieusement d’objets ou d’effets. La cible reste à mesurer, pas une garantie.

## Phase 2 — Réalité virtuelle perceptuelle

1. Reproduire la stéréo, l’échelle, la profondeur et le champ de vision fournis par OpenXR.
2. Viser 72 images réellement rendues par seconde sur Quest 3S, soit environ 13,9 ms par image ; ne pas forcer 36 i/s ni modifier l’ASW.
3. Prendre la pose de la tête le plus tard possible avant le rendu et la présentation.
4. Permettre le jeu assis, le recentrage et les contrôleurs sans imposer une posture.
5. Faire correspondre toucher, poussée et prise avec la réponse visible, physique et haptique.
6. Préserver la cohérence des causes proches : contact, mouvement, lumière et son.
7. Conserver l’évolution du monde lorsqu’on détourne le regard puis qu’on revient.
8. Réduire le coût et la fréquence des éléments imperceptibles ou éloignés ; rattraper leur état sans rupture à l’approche.
9. Garder mouvements, ombres, réflexions et effets cohérents entre les yeux et dans le temps.
10. Localiser les sons en direction et distance, y compris derrière soi, selon l’orientation de la tête.
11. Reproduire de façon plausible l’atténuation, l’occlusion par portes et murs et la réverbération.
12. Consacrer le calcul à ce qui est perceptible : mouvement, contact, contraste, lumière et son ; utiliser des approximations moins coûteuses ailleurs, sans disparition silencieuse.

## Trois problèmes conceptuels constatés

1. `src/app/main.cpp` et `src/core/GraphicsSetup.cpp` exigent aujourd’hui OpenXR pour créer et configurer le rendu : la sortie écran autonome n’existe pas encore. Correction : `P01`.
2. `src/core/GraphicsPrepare.cpp` prend le temps de présentation comme temps de scène ; une simulation indépendante de l’affichage n’est pas encore le propriétaire du temps. Correction : `P07–P09`.
3. Des chemins d’effets sans producteur subsistent ; `shaders/scene.hlsl` limite aussi certaines ombres aux quatre premières lumières. Correction : `P00`, puis `P05–P06`.

## Relais

- `Architecte` : fixe les technologies, contrats, ordre, dépendances, critères et limites ; tranche les blocages de conception ; reçoit un résultat par sous-étape.
- `Programmeur` : reçoit un seul identifiant, un seul fichier à modifier, les symboles et prérequis indiqués ; applique la technique prescrite ; ne choisit pas de technologie ; corrige ses erreurs de compilation.
- `Fiche attribuable` : SHA de base ; fichier unique ; symboles existants et déclarations exactes ; méthode privée et données disponibles ; opération précise ; résultat attendu ; entrées DXC s’il y a lieu. Fiche incomplète = aucune attribution.
- `Compilation` : avant remise à QA, construit sur Windows x64 avec MSVC, lie `OASISPackage`, compile la traduction C++ ou l’en-tête candidat même s’il n’est pas encore dans CMake, et compile les shaders de la configuration par défaut. Erreur, interruption, résultat absent ou base périmée = refus. Aucun résultat partiel n’est un succès.
- `QA` : reçoit uniquement les candidats compilés ; vérifie ensemble toutes les sous-sous-étapes d’une sous-étape, leurs interfaces et leurs interactions ; renvoie au programmeur les corrections locales et les recompilations ; ne choisit pas de technologie.
- `Escalade` : QA transmet à l’architecte uniquement une contradiction de contrat, une dépendance absente, une incompatibilité technique ou un critère impossible ; aucune faute de frappe ni répétition de correction locale.
- `Rapport QA` : identifiant de sous-étape ; accepté/refusé/bloqué ; critères non satisfaits ; décision d’architecture demandée seulement si nécessaire. Aucun rapport enregistré dans le dépôt.
- `Fichier` : une sous-sous-étape modifie exactement un fichier du dépôt. Les lectures nécessaires sont permises. Une sous-sous-étape de déclaration précède toute utilisation ; définition et enregistrement au build précèdent tout appel ; activation en dernier.
- `Livraison` : candidat construit sur le SHA courant de `main` ; si `main` avance, reconstruire le candidat sur la nouvelle base. Après compilation réussie, publication sur `main` ; QA clôt la sous-étape une fois toutes ses modifications présentes. Aucun PR ni branche de travail.
- `Cohérence` : transformations d’interface additives tant que les anciens appelants existent ; désactivation ou suppression seulement après migration de tous les appelants ; ne pas activer un contrat CPU/GPU tant que les deux côtés ne correspondent pas.
- `Vérification en jeu` : uniquement aux sous-étapes qui demandent un résultat visuel, physique ou sonore. Pas de suite de tests, menu de diagnostic, métriques permanentes, journal de QA ou contenu de jeu dans le dépôt.
- `Protection du travail` : pas d’écriture dans l’arbre personnel du joueur par le compilateur distant ; pas de commande arbitraire ni de runner attaché au PC dans le dépôt public.

## Porte de compilation

- Mécanique : `.github/workflows/oasis-compile.yml` ; machine Windows standard hébergée par GitHub, gratuite pour ce dépôt public ; Visual Studio 2022 ; `tools/Build.ps1 -Target all` ; C++ + liaison + shaders de la configuration par défaut.
- Préparation : créer un blob Git contenant le fichier candidat complet ; demander `workflow_dispatch` sur `main` avec `base_sha`, `path`, `operation=write` et `blob_sha`. Suppression : `operation=delete`, `blob_sha` vide.
- Shader `.hlsl` : renseigner `shader_entries` avec les couples de la fiche technique, par exemple `EmitCS:cs_6_6,AdvanceCS:cs_6_6` ; DXC compile chacun en plus de la compilation de la cible. Un nouvel include `.hlsli` est bloqué tant qu’un chemin explicite de compilation n’a pas été ajouté à la porte.
- Isolation : checkout du SHA de base ; vérification que `main` a encore ce SHA ; application d’un seul fichier ; contrôle du SHA du blob et du fichier indexé ; compilation dans la machine éphémère.
- Fichier C++ non encore enregistré dans la cible : compilation séparée avec les options et includes du noyau ; sa liaison à l’exécutable n’est vérifiée qu’après son enregistrement dans CMake.
- Admission : accepter uniquement `conclusion=success` pour cette exécution et ce candidat ; revérifier le SHA de `main` avant publication ; un changement de base annule le résultat.
- Transport : l’outil qui orchestre les agents doit pouvoir créer le blob, déclencher `workflow_dispatch` et lire la conclusion. Si une de ces capacités manque, le relais est bloqué ; ne pas contourner en publiant d’abord sur `main`.
- Limite : la compilation distante ne vérifie ni rendu, ni casque, ni GPU, ni audio ; elle ne prouve pas le comportement en jeu.
- Alternative PC ultérieure : agent Windows déclenché par un canal authentifié, isolé du dossier personnel, qui applique le même contrat et exécute la même compilation. Aucun accès entrant public ni installation supposée.

## Dépendances et contrats

- Ordre : `P00 → P01 → P02 → P03 → P04 → P05 → P06 → P07 → P08 → P09 → P10 → P11 → P12 → V01 → V02 → V03 → V04 → V05 → V06 → V07 → V08 → V09 → V10 → V11 → V12`.
- Par défaut, chaque sous-sous-étape dépend de la précédente. Une sous-étape dépend de l’acceptation QA de la précédente. Aucun parallélisme sur un même fichier.
- Référence : l’existant comporte déjà `WorldRegistry`, `SceneAnimation`, un chargeur glTF, DX12 et OpenXR ; il ne comporte pas encore de sortie écran autonome, de moteur PhysX, de système de particules ni de moteur audio.
- `P01` : découpler configuration graphique et présentation OpenXR avant la fenêtre. La prise en charge XR existante reste fonctionnelle pendant la migration.
- `P02–P04` : les objets référencent les ressources par indices valides ; la destruction invalide l’identité sans recycler silencieusement une identité vivante.
- `P05–P06` : texture et géométrie nécessaires aux réflexions restent résidentes même hors champ direct ; la visibilité du premier œil ne décide pas de celle du second.
- `P07–P09` : une horloge monotone à pas fixe commande les sous-systèmes ; aucun état physique CPU ne fait autorité en mode Direct GPU ; aucune attente CPU de fin de simulation avant le travail graphique indépendant.
- `P09` : PhysX GPU requiert CUDA compatible et le GPU choisi par DX12 ; échec explicite si indisponible. PhysX peut lui-même se replier sur CPU : interdire explicitement ce cas. Formats de collision et capacités GPU non pris en charge : refus explicite.
- `P09` : Direct GPU n’est exploitable qu’après le premier pas de simulation ; dimensionnement des tampons et signaux CUDA/DX12 à fixer avant activation. La disponibilité des binaires PhysX/CUDA et l’interopérabilité exacte sont un verrou d’architecture, pas une décision du programmeur.
- `P09/V04` : en Direct GPU, utiliser `PxD6Joint` ; les requêtes de scène, triggers et CCD CPU ne constituent pas un chemin valide vers l’état GPU. Cuire les maillages de collision avec `buildGPUData` ; toute paire ou fonction hors du chemin GPU défini est refusée. [Limites PhysX](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/DirectGPUAPI.html).
- `P10` : particules GPU et corps PhysX sont deux systèmes distincts ; leur synchronisation et le contrat de collision doivent être définis avant leur branchement.
- `V04–V05` : les données de contact nécessaires aux sons et vibrations peuvent remonter par petits lots asynchrones d’événements ; aucune lecture bloquante de tout l’état GPU à chaque image. Définir le chemin des contacts GPU avant activation.
- `V07–V11` : une approximation ne modifie pas l’identité ou l’état de référence ; aucune transition visible, incohérence binaurale ou discontinuité causale de proximité.
- `V09` : HRTF Windows : source mono PCM flottant 48 kHz, sortie stéréo 48 kHz ; toute conversion de format précède la voix spatiale. Traitement par blocs de 1 024 échantillons : compromis de latence à valider en jeu. [Format HRTF](https://learn.microsoft.com/en-us/windows/win32/api/hrtfapoapi/nf-hrtfapoapi-createhrtfapo).
- `P12/V12` : 1 000 i/s sur écran et 72 Hz VR sont des cibles distinctes, conditionnées par scène, résolution, matériel et charge ; compilation seule insuffisante.
- `Verrou` : toute sous-étape dont la version d’une dépendance, un type, une signature ou une méthode privée manque reste non distribuable. L’architecte complète le contrat avant attribution ; le programmeur et QA n’inventent rien.

## Sous-étapes techniques

Chaque ligne `Pxx.yy` ou `Vxx.yy` est une sous-sous-étape : un fichier à modifier. Les chemins nouveaux sont indiqués explicitement. Les noms de méthodes sont les contrats à déclarer avant leur définition ; ils ne prétendent pas tous exister aujourd’hui.

### P00 — Retrait des chemins d’effets sans producteur

QA : shaders et liaison compilent ; aucun effet actuellement alimenté n’est supprimé ; synchronisation générique des ressources conservée.

- `P00.01` · `shaders/scene.hlsl` · Retirer les deux appels `PopulationTransmission` et `PopulationSourceTransmission` ; leur facteur est actuellement toujours égal à un ; porte DXC `PixelMain:ps_6_6`.
- `P00.02` · `shaders/reflection_hit.hlsli` · Retirer l’appel `PopulationSourceTransmission` ; conserver l’émission du matériau.
- `P00.03` · `shaders/common.hlsli` · Retirer l’inclusion `optical_common.hlsli` après suppression de tous ses appelants.
- `P00.04` · `CMakeLists.txt` · Retirer `shaders/optical_common.hlsli` des dépendances des shaders.
- `P00.05` · `shaders/optical_common.hlsli` · Supprimer le fichier devenu inutilisé.
- `P00.06` · `src/core/GraphicsRender.cpp` · Retirer les trois recherches de `PopulationOpticalFields` et leurs dépendances de frame graph ; conserver la disposition des constantes GPU existante.
- `P00.07` · `src/core/GpuFrameResources.h` · Retirer `GpuFrameResourceId::PopulationOpticalFields` après suppression de ses utilisateurs.
- `P00.08` · `src/core/GraphicsFrameGraph.cpp` · Retirer l’exception de ressource nommée par effet dans `PublicationComplete` ; traiter tous les groupes par les dépendances génériques de leurs ressources.
- `P00.09` · `src/core/GraphicsDomains.cpp` · Valider les publications par leurs ressources requises et leurs groupes non vides ; ne plus exiger de groupe nommé pour un ancien effet.
- `P00.10` · `src/core/MeshSolidPublication.h` · Retirer les identifiants d’anciens effets devenus inutilisés ; conserver les constantes encore utilisées par les publications génériques.

### P01 — Sortie écran DX12

QA : fenêtre fermable et redimensionnable, scène vide puis glTF affichés sans runtime XR ; parcours XR antérieur encore disponible.

- Contrat public écran : `InitializeDesktop()` ; `ConfigureDesktop(HWND,const SceneData&,JobSystem&,RenderContributions)` ; `ResizeDesktop(uint32_t,uint32_t)` ; `BeginDesktopFrame(XRFrame&)` ; `PresentDesktop()`.
- Contrat interne : `InitializeDevice(IDXGIAdapter1*)` ; adaptateur XR imposé par son LUID ; adaptateur écran choisi par `EnumAdapterByGpuPreference` avec préférence haute performance et sans adaptateur logiciel ; mêmes exigences DX12 dans les deux cas.
- Contrat caméra écran : vue mono, pose initiale identité, champ vertical 90°, rapport largeur/hauteur courant, profondeur inversée avec `NearZ` et `FarZ` existants. Les tailles nulles suspendent le rendu jusqu’à restauration.
- `P01.01` · `src/core/GraphicsInternal.h` · Déclarer `OutputSurface` : dimensions, format, nombre de vues, tableaux de ressources couleur/profondeur et propriété de présentation ; déclarer `InitializeDevice`, `ConfigureOutput`, `ConfigureScene`.
- `P01.02` · `src/core/GraphicsSetup.cpp` · Extraire de `Initialize` la création du device et des files dans `InitializeDevice` ; l’appel XR conserve l’adaptateur imposé par OpenXR.
- `P01.03` · `src/core/GraphicsSetup.cpp` · Extraire de `Configure` la configuration des cibles dans `ConfigureOutput` et celle des ressources de scène dans `ConfigureScene` ; maintenir le chemin XR existant.
- `P01.04` · `src/core/Graphics.h` · Déclarer les cinq méthodes du contrat public écran sans retirer les méthodes XR.
- `P01.05` · `src/core/GraphicsInternal.h` · Déclarer `DesktopSurface` et ses méthodes privées `Create`, `Acquire`, `Present`, `Resize`, `Destroy` ; déclarer les cinq méthodes internes écran ; conserver la propriété du swapchain et des buffers.
- `P01.06` · `src/core/GraphicsSetup.cpp` · Définir `DesktopSurface::Create`, `Acquire`, `Resize`, `Destroy` avec DXGI flip-discard, un seul plan de vue, attente des fences avant redimensionnement et libération des anciennes vues.
- `P01.07` · `src/core/GraphicsSetup.cpp` · Définir `DesktopSurface::Present` : `Present(0,0)` après soumission de la transition de sortie, sans temporisation ni intervalle de synchronisation forcé.
- `P01.08` · `src/core/GraphicsSetup.cpp` · Définir les cinq méthodes internes écran ; configurer une vue, des buffers DXGI et une profondeur compatible ; réutiliser `ConfigureScene`.
- `P01.09` · `src/core/Graphics.cpp` · Définir les cinq wrappers publics écran à partir des méthodes internes déclarées.
- `P01.10` · `src/core/GraphicsRender.cpp` · Utiliser la cible `OutputSurface` sélectionnée ; transition `PRESENT` uniquement pour DXGI, contrat d’acquisition/libération XR inchangé.
- `P01.11` · `src/app/main.cpp` · Créer et pomper la fenêtre Win32 ; sélectionner écran par défaut et XR par option explicite ; construire une vue mono, gérer `WM_SIZE` et présenter sans attente artificielle.

### P02 — Identité et cycle de vie des objets

QA : création, déplacement, suppression et réutilisation des ressources sans identité fantôme.

- `P02.01` · `src/core/WorldRegistry.h` · Déclarer `Find`, `SetTransform`, `SetEnabled` et une table privée `std::unordered_map<uint64_t,WorldObject>` ; conserver `Create`, `Update`, `Erase`.
- `P02.02` · `src/core/WorldRegistry.cpp` · Alimenter la table dans `CreateBatch`, `Update` et `EraseBatch`, utilisés aussi par les opérations unitaires ; ne jamais réattribuer une identité encore vivante.
- `P02.03` · `src/core/WorldRegistry.cpp` · Définir `Find` avec échec explicite pour identité absente.
- `P02.04` · `src/core/WorldRegistry.cpp` · Définir `SetTransform` : recopier courant vers précédent, appliquer pose et échelle, déplacer l’enregistrement de tuile si nécessaire.
- `P02.05` · `src/core/WorldRegistry.cpp` · Définir `SetEnabled` sans détruire l’objet ; transmettre l’état à la publication GPU.

### P03 — Ressources de scène

QA : un glTF charge géométrie, matériaux et animation ; une référence invalide est refusée ; aucune ressource de jeu ajoutée au dépôt.

- `P03.01` · `src/core/Assets.h` · Déclarer `ValidateScene(const SceneData&)` et `LoadSceneGLTF` comme point d’entrée unique.
- `P03.02` · `src/core/Assets.cpp` · Définir `ValidateScene` : indices de mesh, matériau, texture, parent, articulation et animation dans leurs tableaux respectifs.
- `P03.03` · `src/core/Assets.cpp` · Appeler `ValidateScene` à la sortie de `LoadSceneGLTF` ; refuser tout index hors limites.
- `P03.04` · `src/app/main.cpp` · Transférer les objets chargés à `WorldRegistry` sans recopier leur géométrie ; garder un seul exemplaire de chaque ressource de `SceneData`.
- `P03.05` · `src/core/GraphicsSetup.cpp` · Vérifier la durée de vie des buffers et textures partagés jusqu’à la fin des fences de leurs derniers utilisateurs.

### P04 — Instances et publication

QA : plusieurs milliers d’identités distinctes partagent mesh et matériau sans duplication de géométrie.

- `P04.01` · `src/core/GpuSceneCache.cpp` · Maintenir les tuiles et plages modifiées à `UpsertTile` et `PatchTileObject` sans republier les tuiles inchangées.
- `P04.02` · `src/core/GraphicsResidentTiles.cpp` · Regrouper les instances opaques par mesh et matériau, sans réordonner les objets transparents.
- `P04.03` · `src/core/GraphicsPrepare.cpp` · Publier uniquement les plages d’instances modifiées et leurs dépendances de ressources.
- `P04.04` · `src/core/GraphicsRender.cpp` · Émettre les lots d’instances au moyen du chemin indirect existant ; n’affecter ni identité ni transformation.

### P05 — Matériaux et réflexions

QA : couleur, rugosité, métal, normale, relief et alpha cohérents en vue directe et en réflexion.

- `P05.01` · `src/core/GraphicsPrepare.cpp` · Ajouter aux pages résidentes les textures des objets éligibles aux rayons, y compris les objets dynamiques hors champ direct.
- `P05.02` · `src/core/Assets.cpp` · Conserver alpha, double face, normales, coordonnées UV et paramètres PBR du glTF dans `AssetMaterial`.
- `P05.03` · `shaders/scene.hlsl` · Évaluer les paramètres PBR et le mode alpha du matériau pour le rendu direct ; porte DXC `PixelMain:ps_6_6`.
- `P05.04` · `shaders/reflection_hit.hlsli` · Employer les mêmes textures, coordonnées et seuils alpha au point d’impact réfléchi.
- `P05.05` · `src/core/GraphicsRayTracing.cpp` · Inclure les instances et matériaux admissibles dans la structure de rayons, indépendamment de leur visibilité directe.

### P06 — Lumières, ombres et transparence

QA : ajout et retrait de lumières, ombres et réflexions fonctionnent sans plafond fixe de sources ; ordre des transparents préservé.

- `P06.01` · `src/core/GraphicsResources.cpp` · Dimensionner les buffers de lumières, de BVH et de listes selon le nombre effectif ; refuser explicitement tout dépassement matériel.
- `P06.02` · `shaders/compute.hlsl` · Parcourir toutes les lumières pertinentes par cluster via la BVH ; prévoir un chemin complet en cas de saturation de liste ; porte DXC `ClusterSpatialMain:cs_6_6`.
- `P06.03` · `shaders/scene.hlsl` · Retirer tout plafond numérique arbitraire de lumières prises en compte et ne pas limiter silencieusement les ombres aux quatre premières ; porte DXC `PixelMain:ps_6_6`.
- `P06.04` · `shaders/reflection_hit.hlsli` · Utiliser la même liste spatiale de lumières au point d’impact des réflexions.
- `P06.05` · `src/core/GraphicsRender.cpp` · Conserver la séquence des transparents lors du regroupement des opaques et des soumissions par vue.

### P07 — Temps de simulation

QA : simulation et animation évoluent à la même vitesse lorsque la fréquence d’affichage change.

- `P07.01` · `src/core/Core.h` · Déclarer `SimulationClock` avec compteur monotone QPC, accumulateur en secondes, tick 64 bits et pas fixe `1/240` s ; ajouter à `FrameInput` le tick et l’interpolation de simulation.
- `P07.02` · `src/core/Core.h` · Définir `SimulationClock::Advance` : accumuler le temps écoulé monotone sans perdre silencieusement de pas.
- `P07.03` · `src/core/Core.h` · Définir `SimulationClock::ConsumeTick` et `Alpha` ; borne d’interpolation `[0,1]`.
- `P07.04` · `src/app/main.cpp` · Créer une seule horloge ; avancer et consommer les pas avant publication ; renseigner le temps de simulation dans `FrameInput`.
- `P07.05` · `src/core/GraphicsPrepare.cpp` · Dans `Visuals` et `Prepare`, utiliser le temps, le tick et l’interpolation publiés ; réserver `targetDisplayQpc` à la présentation XR.

### P08 — Animation

QA : animation de nœud, squelette et morph suivent le même temps ; objets physiques non écrasés par l’animation.

- `P08.01` · `src/core/Animation.h` · Déclarer `EvaluateTick` et `SetPhysicsOwned` ; distinguer les nœuds pilotés par la physique.
- `P08.02` · `src/core/Animation.cpp` · Définir `EvaluateTick` à partir du temps de simulation ; conserver interpolation glTF et ordre parent-enfant.
- `P08.03` · `src/core/Animation.cpp` · Définir `SetPhysicsOwned` et exclure ces poses de l’écriture animation ; conserver squelette et morph autorisés.
- `P08.04` · `src/core/GraphicsPrepare.cpp` · Transmettre au GPU les matrices et poids du tick publié ; conserver les dépendances avant déformation et BLAS.
- `P08.05` · `src/core/WorldRegistry.cpp` · Garder les poses précédente et courante pour interpolation de rendu sans modifier l’état physique.

### P09 — Physique GPU

QA : corps et articulations fonctionnent sur le GPU choisi ; absence de CUDA ou forme non prise en charge = échec explicite ; rendu indépendant préparé pendant la simulation.

- `P09.01` · `CMakeLists.txt` · Ajouter la version PhysX et les bibliothèques CUDA/PhysX explicitement fixées par l’architecte après vérification de disponibilité ; refuser une dépendance absente.
- `P09.02` · `src/core/Physics.h` · Nouveau fichier ; déclarer `Physics`, ses handles stables, `Initialize`, `CreateBody`, `CreateJoint`, `BeginTick`, `Publish`, `Shutdown` et leurs données privées.
- `P09.03` · `src/core/Physics.cpp` · Nouveau fichier ; définir construction et destruction, contexte CUDA sur le LUID DX12 et refus explicite de tout repli CPU.
- `P09.04` · `CMakeLists.txt` · Ajouter `src/core/Physics.cpp` à la cible du noyau ; conserver la construction avant tout appel.
- `P09.05` · `src/core/Physics.cpp` · Créer la scène avec dynamique GPU, broadphase GPU et Direct GPU API ; échouer si un prérequis manque.
- `P09.06` · `src/core/Physics.cpp` · Définir création et destruction des corps et articulations `PxD6Joint` ; refuser formes et contacts non couverts par le chemin GPU.
- `P09.07` · `src/core/Physics.cpp` · Définir `BeginTick` avec pas fixe ; effectuer le premier pas requis avant toute lecture Direct GPU.
- `P09.08` · `src/core/Physics.cpp` · Définir `Publish` : poses GPU par identités, buffers partagés CUDA/DX12 et signal de production après soumission effective.
- `P09.09` · `src/app/main.cpp` · Démarrer le tick physique avant préparation du décor indépendant ; ne consommer la publication qu’au point de dépendance graphique.
- `P09.10` · `src/core/GraphicsFrameGraph.cpp` · Attendre la fence du producteur avant le premier consommateur des poses ; rendre le buffer après son dernier usage.

### P10 — Particules GPU

QA : deux millions de particules actives possibles selon mémoire disponible ; simulation, rendu et forces cohérents ; aucun ancien chemin de particules de jeu.

- `P10.01` · `src/core/Particles.h` · Nouveau fichier ; déclarer `ParticleSystem`, `Initialize`, `Emit`, `Advance`, `Publish`, buffers d’état et capacités privées.
- `P10.02` · `src/core/Particles.cpp` · Nouveau fichier ; définir allocation GPU dimensionnée par capacité, refus explicite si mémoire insuffisante et libération après fences.
- `P10.03` · `CMakeLists.txt` · Ajouter `src/core/Particles.cpp` à la cible du noyau.
- `P10.04` · `shaders/particles.hlsl` · Nouveau fichier ; définir `EmitCS` et `AdvanceCS` : position, vitesse, âge, durée de vie, gravité et vent au pas fixe ; porte DXC `EmitCS:cs_6_6,AdvanceCS:cs_6_6`.
- `P10.05` · `shaders/particles.hlsl` · Définir `ParticleVS` et `ParticlePS` : quad orienté vers chaque vue, couleur et alpha par particule ; porte DXC `ParticleVS:vs_6_6,ParticlePS:ps_6_6` ; aucune suppression de particule active.
- `P10.06` · `CMakeLists.txt` · Compiler les quatre points d’entrée de `particles.hlsl` avec DXC dans `OASISShaders` avant leur utilisation.
- `P10.07` · `src/core/GraphicsInternal.h` · Déclarer les états de pipeline et méthodes privées `CreateParticlePipelines` et `AddParticlePass`.
- `P10.08` · `src/core/GraphicsSetup.cpp` · Définir `CreateParticlePipelines` avec les shaders compilés et les formats de cible du rendu courant.
- `P10.09` · `src/core/Particles.cpp` · Soumettre émission et simulation ; publier buffer d’instances et fence après écriture.
- `P10.10` · `src/core/GraphicsRender.cpp` · Définir `AddParticlePass`, consommer la publication et dessiner par instances indirectes après la simulation, sans lecture CPU complète.
- `P10.11` · `src/core/Particles.cpp` · Ajouter uniquement les collisions GPU dont le contrat avec `Physics` a été fixé par l’architecte ; refuser les autres.

### P11 — Audio et événements communs

QA : source sonore et événement suivent la chronologie de simulation ; arrêt et libération propres.

- `P11.01` · `src/core/Audio.h` · Nouveau fichier ; déclarer `Audio`, `Initialize`, `LoadPcm`, `Play`, `Stop`, `Update`, `Shutdown` et ownership des voix.
- `P11.02` · `src/core/Audio.cpp` · Nouveau fichier ; initialiser XAudio2 2.9 et la voix maîtresse ; libérer voix, buffers et moteur à l’arrêt.
- `P11.03` · `CMakeLists.txt` · Ajouter `src/core/Audio.cpp` et l’édition de liens XAudio2.
- `P11.04` · `src/core/Audio.cpp` · Charger PCM, conserver la mémoire pendant la lecture, définir `Play` et `Stop` sans dépendance à un jeu.
- `P11.05` · `src/app/main.cpp` · Transmettre à `Audio::Update` le tick et le temps de la simulation ; arrêter l’audio avant destruction des ressources.

### P12 — Ordonnancement et cadence écran

QA : aucune attente CPU superflue entre producteurs indépendants ; aucune limite artificielle d’i/s ; mesure 1 000 i/s séparée et contextualisée.

- `P12.01` · `src/core/GraphicsFrameGraph.cpp` · Vérifier les dépendances copy/compute/direct par ressource ; attendre une publication seulement à son premier consommateur.
- `P12.02` · `src/core/GraphicsRender.cpp` · Soumettre préparation et profondeur du décor indépendant avant l’attente des poses physiques.
- `P12.03` · `src/core/GraphicsPrepare.cpp` · Éliminer les lectures synchrones GPU→CPU de l’état complet et les allocations répétées évitables.
- `P12.04` · `src/app/main.cpp` · Supprimer tout `Sleep` de la boucle écran active ; conserver les attentes événementielles en fenêtre minimisée.
- `P12.05` · `src/core/GraphicsSetup.cpp` · Présenter sans limite logicielle d’i/s ; conserver la synchronisation des buffers nécessaire à leur réutilisation.

### V01 — OpenXR stéréo

QA : OpenXR utilise l’adaptateur et les deux vues du runtime ; le mode écran reste autonome.

- `V01.01` · `src/core/XR.cpp` · Fournir à `OutputSurface` les images, dimensions, format et nombre de vues OpenXR.
- `V01.02` · `src/core/GraphicsSetup.cpp` · Configurer la même scène pour les vues XR et écran par `ConfigureOutput` et `ConfigureScene`.
- `V01.03` · `src/core/GraphicsRender.cpp` · Consommer toutes les vues annoncées ; respecter indices d’image et tranches couleur/profondeur.
- `V01.04` · `src/app/main.cpp` · Sélectionner XR uniquement sur demande ; refuser explicitement runtime absent en mode XR.

### V02 — Pose tardive et cadence VR

QA : pose prédite et projection viennent du runtime ; pas de verrou 36 i/s ni d’intervention ASW.

- `V02.01` · `src/core/XR.cpp` · Conserver `xrWaitFrame` et `predictedDisplayTime` comme référence de présentation.
- `V02.02` · `src/core/XR.cpp` · Localiser les vues au temps prédit dans `LocateLate`, après préparation indépendante et avant soumission de leur rendu.
- `V02.03` · `src/core/GraphicsRender.cpp` · Utiliser les matrices et positions des deux yeux de la même localisation tardive.
- `V02.04` · `src/app/main.cpp` · Ne demander ni verrou 36 i/s ni changement ASW ; traiter les images non rendues selon le contrat OpenXR.

### V03 — Assise, recentrage et contrôleurs

QA : session jouable assise ; recentrage conservant les objets du monde ; contrôleurs utilisables.

- `V03.01` · `src/app/VrInputMapping.h` · Déclarer l’action de recentrage et la transformation privée repère de suivi → monde.
- `V03.02` · `src/app/VrInputMapping.cpp` · Définir le recentrage à partir de la pose courante : position et lacet, sans déplacer les objets.
- `V03.03` · `src/core/XR.cpp` · Appliquer le même repère à la tête et aux deux mains ; gérer perte et retour de suivi.
- `V03.04` · `src/app/input.ini` · Associer le recentrage à une action existante du contrôleur sans rendre la station debout obligatoire.

### V04 — Prise, contact et haptique

QA : toucher, pousser et saisir un corps produisent mouvements et vibrations cohérents dans le monde proche.

- `V04.01` · `src/core/Physics.h` · Déclarer l’événement de contact avec tick, identités, position, normale et intensité ; déclarer `SetKinematicHand`, `BeginGrab`, `EndGrab` et `DrainContactEvents` avec handles et ownership.
- `V04.02` · `src/core/Physics.cpp` · Alimenter les corps cinématiques des mains depuis les poses suivies ; désactiver proprement une main sans suivi.
- `V04.03` · `src/core/Physics.cpp` · Attacher puis détacher un objet dynamique par `PxD6Joint` GPU ; conserver sa vitesse à la libération.
- `V04.04` · `src/core/Physics.cpp` · Publier les contacts utiles par lot asynchrone borné ; refuser tout dépassement non traité.
- `V04.05` · `src/app/VrInputMapping.cpp` · Pour une prise, choisir parmi les contacts actifs de la main l’objet au point le plus proche, puis son identité en cas d’égalité ; transformer prise et relâchement en début/fin de saisie et les contacts en `HapticEvent` bornés.
- `V04.06` · `src/core/XR.cpp` · Acheminer `HapticEvent` vers la main et l’action OpenXR correspondantes ; arrêter les vibrations à la perte de suivi.

### V05 — Causalité proche

QA : un contact proche conserve identité, temps, mouvement et son associés.

- `V05.01` · `src/core/Physics.h` · Déclarer l’ordre de consommation et les bornes du lot d’événements de contact déjà introduit en `V04.01`.
- `V05.02` · `src/core/Physics.cpp` · Remplir ces champs depuis les contacts GPU pris en charge ; écarter explicitement les doublons du même tick.
- `V05.03` · `src/core/Audio.h` · Déclarer `PlayContact` avec identité de source, position, intensité et tick.
- `V05.04` · `src/core/Audio.cpp` · Associer une voix au contact et à sa position sans changer son identité pendant sa durée.
- `V05.05` · `src/app/main.cpp` · Distribuer le même lot aux consommateurs audio et haptique après disponibilité, sans bloquer la file graphique.

### V06 — Continuité hors regard

QA : un objet qui évolue hors champ reprend son état réel au retour du regard.

- `V06.01` · `src/core/WorldRegistry.h` · Déclarer pour chaque identité un état de référence et le dernier tick simulé.
- `V06.02` · `src/core/WorldRegistry.cpp` · Conserver état et identité quand une tuile sort de la visibilité ; ne pas supprimer son propriétaire.
- `V06.03` · `src/core/WorldRegistry.cpp` · Appliquer au retour les événements et évolutions enregistrés dans l’ordre des ticks.
- `V06.04` · `src/core/GpuSceneCache.cpp` · Republier les plages modifiées au retour sans recréer une identité différente.

### V07 — Approximation et rattrapage

QA : transition progressive vers l’état détaillé à l’approche, sans saut ni perte d’événement proche.

- `V07.01` · `src/core/WorldRegistry.h` · Déclarer `DetailState`, dernier tick et priorité par identité ; distinguer état de référence et représentation.
- `V07.02` · `src/core/WorldRegistry.cpp` · Calculer la priorité à partir de distance, mouvement, interaction et visibilité des deux yeux.
- `V07.03` · `src/core/WorldRegistry.cpp` · Propager au temps courant les états hors champ dont l’évolution possède une solution déterministe fixée.
- `V07.04` · `src/core/GraphicsResidentTiles.cpp` · Préparer les représentations détaillées avant franchissement de leur seuil de visibilité.
- `V07.05` · `shaders/common.hlsli` · Appliquer une transition continue entre représentations sans supprimer une identité ou ses effets.

### V08 — Cohérence stéréo et temporelle

QA : aucune divergence entre yeux, scintillement systématique ou saut lors des transitions de représentation.

- `V08.01` · `src/core/GraphicsPrepare.cpp` · Construire la visibilité comme union des vues et des objets nécessaires aux réflexions.
- `V08.02` · `src/core/GraphicsRender.cpp` · Associer à chaque œil ses matrices présente et précédente, avec une réinitialisation cohérente après recentrage.
- `V08.03` · `shaders/scene.hlsl` · Employer le même état matériel et la même identité pour les deux vues ; porte DXC `PixelMain:ps_6_6`.
- `V08.04` · `shaders/reflection_hit.hlsli` · Utiliser la géométrie et les matériaux communs aux deux yeux sans dépendre du premier œil rendu.

### V09 — Audition spatiale

QA : déplacement et rotation de la tête modifient correctement direction et distance, y compris pour une source arrière.

- `V09.01` · `CMakeLists.txt` · Ajouter la liaison Windows `hrtfapo` et les en-têtes système requis.
- `V09.02` · `src/core/Audio.h` · Déclarer `SetListener`, `SetSourcePose` et l’état HRTF privé par voix.
- `V09.03` · `src/core/Audio.cpp` · Créer un HRTF par voix spatiale via `CreateHrtfApo` ; entrée mono float 48 kHz, sortie stéréo.
- `V09.04` · `src/core/Audio.cpp` · Transformer direction et distance dans le repère de la tête ; mettre à jour `IXAPOHrtfParameters` sans réinitialiser la voix.
- `V09.05` · `src/app/main.cpp` · Mettre à jour l’auditeur avec la pose de tête et le temps de présentation courants.

### V10 — Occlusion et acoustique

QA : mur et porte modifient le son ; leur ouverture rétablit progressivement le trajet direct.

- `V10.01` · `src/core/Audio.h` · Déclarer `AcousticScene`, `UpdateGeometry`, `TraceDirect` et `SetOcclusion` ; fixer les unités en mètres.
- `V10.02` · `src/core/Assets.cpp` · Alimenter `AppendAcousticGeometry` avec les triangles et transformations valides de la scène.
- `V10.03` · `src/core/Audio.cpp` · Construire une structure spatiale des triangles et calculer l’occlusion du trajet source–auditeur.
- `V10.04` · `src/core/Audio.cpp` · Lisser gain et filtre d’occlusion entre résultats ; appliquer une atténuation dépendant de la distance.
- `V10.05` · `src/core/Audio.cpp` · Appliquer la réverbération d’environnement prescrite par l’architecte ; mise à jour bornée sans interrompre les voix.
- `V10.06` · `src/core/WorldRegistry.cpp` · Invalider la géométrie acoustique d’une porte ou d’un objet mobile après changement de pose.

### V11 — Budget perceptuel

QA : priorité accrue aux interactions, mouvements et sons proches ; état conservé lorsque le coût éloigné baisse.

- `V11.01` · `src/core/WorldRegistry.h` · Déclarer les entrées de priorité : distance, vitesse, contraste, visibilité, contact et audibilité.
- `V11.02` · `src/core/WorldRegistry.cpp` · Calculer la priorité selon les coefficients fixés par l’architecte ; maintenir un minimum pour les objets interactifs.
- `V11.03` · `src/core/GraphicsResidentTiles.cpp` · Consommer cette priorité pour anticiper les ressources sans supprimer les objets.
- `V11.04` · `src/core/Audio.cpp` · Réduire la fréquence de calcul acoustique des sources non saillantes ; conserver leur position et leur lecture.
- `V11.05` · `src/core/Particles.cpp` · Réduire la fréquence de mise à jour admissible hors perception ; conserver nombre et état logique.

### V12 — Intégration VR

QA : scène active sur Quest 3S en position assise ; 72 images réellement rendues/s visées ; deux yeux, contacts, sons et continuité vérifiés ensemble.

- `V12.01` · `src/app/main.cpp` · Ordonner entrée → ticks → publications → préparation indépendante → pose tardive → rendu → présentation → événements.
- `V12.02` · `src/core/GraphicsFrameGraph.cpp` · Vérifier les fences et durées de vie des publications physique, particules, géométrie et éclairage.
- `V12.03` · `src/core/XR.cpp` · Libérer chaque image acquise et terminer chaque frame commencée, y compris à l’arrêt ou à la perte de suivi.
- `V12.04` · `src/core/Audio.cpp` · Arrêter les voix et libérer leurs données après arrêt propre des producteurs d’événements.
- `V12.05` · `src/core/Graphics.cpp` · Attendre les files et libérer les ressources uniquement après le dernier utilisateur GPU.
