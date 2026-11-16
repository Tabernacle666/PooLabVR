# OASIS — manifeste des démonstrations

OASIS est un moteur généraliste Windows / DirectX 12 et OpenXR. Le logo 3D est son banc de démonstration permanent, pas un jeu ni une scène spéciale. **À chaque cycle, un phénomène nouveau s'ajoute au même logo et reste présent dans les cycles suivants.** Au terme du parcours, la richesse du logo doit démontrer qu'un jeu visuellement ambitieux est possible avec les mêmes systèmes.

Ce document définit des **jalons à montrer**, et non une liste de fonctions, de fichiers à toucher ou de cérémonies d'ingénierie. Un travail d'infrastructure nécessaire fait partie du jalon qui l'utilise ; il ne donne pas droit, à lui seul, à une démonstration aux actionnaires.

## Contrat de démonstration

1. **Un phénomène de plus, aucun phénomène de moins.** Chaque cycle ajoute quelque chose que l'on voit, entend ou manipule sur le logo. Les démonstrations sont cumulatives ; on ne masque pas un effet précédent pour faire passer le nouveau.
2. **Le logo n'a aucun privilège technique.** Il est chargé comme un actif ordinaire. Aucune illumination peinte, animation vidéo substituée, réflexion truquée, collision préenregistrée ni code de rendu spécial au logo ne peut tenir lieu de capacité du moteur. La même fonctionnalité doit servir à d'autres maillages et scènes sans réécrire le moteur.
3. **Qualité réellement produite.** La densité, la géométrie, les ombres, les réflexions, les interactions et les deux vues VR présentées correspondent au calcul effectivement réalisé ; pas de suppression silencieuse de contenu, de bascule physique GPU vers CPU ou de résultat pré-calculé présenté comme temps réel.
4. **Cadence et réserve distinctes.** Le Quest 3S vise 72 images réelles/s à sa résolution configurée, sans verrou volontaire à 36 ni modification forcée de l'ASW. 71,99, 72,00 et 72,01 images/s ne sont pas trois niveaux de réussite différents. Une fréquence plafonnée par OpenXR ne démontre **aucune marge** : distinguer cadence soumise, temps CPU/GPU et capacité non plafonnée lorsqu'elle est mesurable. Ne jamais inventer une réserve de FPS.
5. **Une présentation courte.** Montrer le phénomène ajouté, sa réaction à une entrée ou à un changement du monde, et le coût de la scène complète. Compilation, refactorisation, plomberie GPU, synchronisation et gestion des ressources sont des moyens, pas des jalons autonomes.
6. **Validation en jeu.** Le cycle n'est accepté qu'après observation sur l'exécutable. Des compilations sont nécessaires, mais pas de suites de tests, tableaux QA, menus de diagnostics ni rapports permanents ajoutés au projet. Travail sur `main`, sans PR ni branche imposée ; validation visuelle avant de changer de phénomène.

## Phase 1 — Le logo devient un monde 3D sur écran

La sortie DX12 sur écran reste autonome : elle ne requiert ni casque ni runtime OpenXR actif. Le mode VR existant doit continuer à fonctionner pendant l'évolution. L'ordre de démarrage des modes n'est pas un critère de qualité.

| Cycle | Nouveau phénomène observable sur le logo | Capacité générale démontrée |
| --- | --- | --- |
| **E1 — Présence** | Le logo apparaît en vrai volume ; la caméra change de perspective et révèle faces, épaisseur et profondeur. | Sortie écran DX12, chargement glTF et scène rendue en 3D. |
| **E2 — Matière** | Certaines faces deviennent métal, d'autres rugueuses ou transparentes ; les reliefs et textures réagissent à la vue. | Matériaux PBR, normales, rugosité, métal, alpha, géométrie et ressources chargées normalement. |
| **E3 — Lumière** | Plusieurs sources lumineuses se déplacent autour du logo ; les ombres et les couleurs répondent à leur mouvement. | Éclairage dynamique et ombres pour toutes les sources pertinentes, sans limite arbitraire des quatre premières. |
| **E4 — Réflexion** | Un élément mobile du logo et les lumières se reflètent correctement dans ses parties métalliques. | Réflexions de géométrie et matériaux réels, y compris hors du champ direct ; alpha cohérent. |
| **E5 — Population** | Des milliers de fragments ou éléments secondaires apparaissent, circulent et disparaissent autour du logo sans le cacher. | Instances partageant leurs ressources, identités stables, création/destruction et publications GPU incrémentales. |
| **E6 — Mouvement articulé** | Des parties du logo se déploient, pivotent ou se déforment de façon continue, même quand la cadence d'affichage varie. | Animation de nœuds, squelette et morph, transformations hiérarchiques et horloge de simulation indépendante de la présentation. |
| **E7 — Matière physique** | Une pièce heurte le logo, rebondit, entraîne une articulation puis retombe : la scène répond réellement au contact. | Corps rigides, collisions et articulations sur **PhysX GPU**, sans repli CPU silencieux ; calcul graphique indépendant concurrent. |
| **E8 — Atmosphère active** | Vent, poussière, neige ou autre nuage de particules circule et interagit avec le logo, son mouvement et son éclairage. | Émission, simulation et rendu GPU à très grande échelle, jusqu'à **2 000 000 de particules actives** selon la mémoire disponible ; interactions et publication sans lecture CPU totale. |

**À la fin de la phase 1 :** une seule scène DX12 montre simultanément matières, éclairage, réflexions, instanciation, animation, physique et particules. Aucun « jalon de performance » séparé ne permet de réduire cette scène : la cadence et la réserve sont examinées sur le résultat cumulatif.

## Phase 2 — Le même logo devient un monde VR crédible

La VR ne remplace pas les effets de la phase 1 : elle les reprend intégralement. La démonstration se fait en position assise, sur Quest 3S, avec les contrôleurs prévus.

| Cycle | Nouveau phénomène observable sur le même logo | Capacité générale démontrée |
| --- | --- | --- |
| **V1 — Présence stéréo** | En bougeant la tête, on découvre la vraie profondeur et l'échelle du logo depuis chaque œil, sans dédoublement des effets. | OpenXR stéréo, poses et projections du runtime, localisation tardive, recentrage assis, synchronisation des deux vues et cible 72 Hz. |
| **V2 — Contact** | On touche, pousse et saisit une partie du logo ; elle résiste, réagit et transmet une vibration à la main concernée. | Contrôleurs, contacts physiques GPU, prise et relâchement, haptique et identité persistante des objets. |
| **V3 — Chaîne causale** | Un choc déplace une pièce, libère des particules et produit un son au même endroit et au bon moment. | Contacts, animation, particules et audio sur une chronologie commune ; événements bornés sans transfert bloquant de tout l'état GPU. |
| **V4 — Monde persistant** | On détourne le regard pendant qu'une pièce bouge ou que le nuage dérive ; au retour, le monde a réellement continué d'évoluer. | Simulation hors champ, identité et état conservés, approximation distante admissible et rattrapage sans saut perceptible. |
| **V5 — Son dans l'espace** | Une source attachée à une pièce mobile reste localisable à gauche, à droite et derrière la tête quand on se retourne. | Son spatialisé avec orientation de tête, distance et déplacement des sources. |
| **V6 — Occlusion vivante** | Une pièce ou une paroi passe entre la source et le joueur : le son s'étouffe puis revient, tandis que lumière, ombre et reflet restent cohérents dans les deux yeux. | Occlusion acoustique et réverbération dépendantes de la géométrie, mises à jour dynamiques ; cohérence stéréo, temporelle et matérielle intégrée. |

**Résultat final :** le même logo doit réunir tous les phénomènes, avec réactions physiques, stéréo correcte, son et persistance du monde. Ce n'est pas une nouvelle phase de plomberie ni un diaporama : c'est la démonstration que les capacités se composent en une expérience de jeu.

## Contraintes transversales — importantes, mais pas des jalons

- **Moteur générique :** charger des scènes arbitraires ; conserver les identités des objets et la durée de vie des ressources ; refuser explicitement les références invalides. Le logo reste le sujet visuel principal.
- **GPU et ordonnancement :** CPU et GPU travaillent en parallèle lorsque les dépendances le permettent. Les producteurs GPU signalent leurs résultats ; la première opération dépendante les attend, pas toute la frame. Ne jamais recopier l'état complet du GPU au CPU à chaque image.
- **Physique :** PhysX Direct GPU reste sur le GPU DX12 compatible CUDA. Aucun remplacement par une animation, aucune approximation CPU non autorisée ; échouer explicitement si l'opération requise n'est pas supportée.
- **Continuité :** simulation, animation, contacts, particules et audio partagent une chronologie cohérente. Une simplification lointaine peut réduire le coût de représentation, jamais falsifier l'état de référence ou effacer un événement.
- **Éclairage et VR :** pas de plafond numérique artificiel de lumières ; le premier œil ne décide pas de la visibilité du second ; géométrie et matériaux utiles aux réflexions restent accessibles. Pas de perte silencieuse d'ombres, de transparences ou d'effets.
- **Performance honnête :** aucune cible arbitraire de « 1 000 FPS » à vide ne remplace l'évaluation de la vraie scène. Mesurer séparément la fluidité effectivement présentée et la marge CPU/GPU disponible, sans confondre synchronisation et limite de calcul. Éviter toute instrumentation permanente hors besoin explicite.
- **Travail technique :** refactorisations, nettoyage d'anciens chemins, wrappers, fichiers, signatures, pipelines, fences, build et intégration sont du travail à accomplir dans le cycle pertinent. Ils ne doivent pas allonger artificiellement la liste des démonstrations. Un blocage technique réel est expliqué avant toute substitution.
- **Protection :** aucun outil distant n'écrit dans les fichiers personnels du joueur et aucune commande arbitraire n'est exposée publiquement par le dépôt.
