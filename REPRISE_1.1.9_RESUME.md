# ConsoleVR — repères de reprise après 1.1.9

**État d'import :** la sauvegarde de l'état récent est publiée. La source exacte 1.1.9 est préparée dans un bundle Git vérifié (161 fichiers) et un restaurateur autonome ; son commit sur `main` dépend encore de l'exécution du restaurateur dans le clone Windows, car l'outil GitHub connecté ne peut pas transférer directement les octets de l'archive jointe ici. Ne pas confondre ce document avec un retour de code déjà appliqué.

**Point de retour :** archive `ConsoleVR_1.1.9_fix2_capture_XY.zip`, `VERSION.txt = 1.1.9.fix2.captureXY`, fichiers datés des **8–9 octobre 2026**. Les captures du 9 octobre montrent la console murale jaune/orange, l'éclairage intérieur et les ombres du ventilateur. C'est une **référence visuelle**, pas une preuve que toutes les parties du moteur étaient exemptes de défaut.

**État de l'historique suivant :** plusieurs livraisons ont été vérifiées seulement statiquement et certaines séries étaient des plans ; ce tableau ne les présente pas comme des résultats VR acquis.

| Versions | Travaux à retrouver et réintroduire sélectivement |
|---|---|
| **1.1.10–1.1.11** | Premiers outils de créateur extérieur, Worlds nommés, registre d'objets/composants, remapping des entrées et sauvegardes atomiques. Plusieurs éléments n'étaient encore que des tickets ou des implémentations partielles. |
| **1.1.12–1.1.17** | Cartographie de diagnostic SONAR/ROADS/AERIAL et réception MESH/PHYSICS/AUDIO ; brouillard général et zones locales persistantes ; correctifs du mur-écran (visée/sélection/activité), feuillus procéduraux et ombres. Validation VR insuffisante. |
| **1.1.18–1.1.19** | Habitats d'insectes, simulation CUDA et morphotypes ; audio spatial/agrégé ; bus MASTER/FAUNA/FLORA/WEATHER/TOOLS, sélection de sortie et réglages audio persistants. |
| **1.1.20–1.1.21** | Ergonomie du créateur, visée et réglage d'altitude A/B, snap/raccords, lampadaires, maison 8×8 et mur-écran ; graphes routiers, terrain/relief, collisions PhysX, sauvegarde World. Plusieurs points de compilation et de rendu demeuraient incertains. |
| **1.1.22–1.1.29** | Travaux autour du tracé/édition des routes, intersections, géométrie du terrain, berges et rivières ; intentions et tâches conservées, exécution exacte à revalider. |
| **1.1.30–1.1.49** | Forêt procédurale : instances déterministes, espèces/matériaux, identités persistantes, sélection et modifications locales, exclusion routes/eau, PhysX et index CUDA, mises à jour par blocs ; progression des outils de paysage, raycast, Undo et documents World. |
| **1.1.50–1.1.52** | Rattrapage du build, de l'entrée VR et de l'ergonomie ; audio des rivières et identités persistantes ; consolidation des contrats ABI et de l'ownership GPU/ressources. La couverture de test restait incomplète. |
| **1.1.53.x** | Grande refonte proposée/partiellement livrée de la faune et de l'audio spatial : voix virtuelles, ordonnanceur, agrégation, cycles de chants, transitions, empreintes XYZ et comportements. **La série n'avait jamais été réellement testée en VR.** |
| **1.1.54.x** | Dessin de zones peintes, union des couvertures, paramètres persistants, prévisualisation, Undo, intégration aux forêts et reliefs ; **plan suspendu**, ne pas supposer tous les billets réalisés. |
| **1.1.55.x** | Migration ambitieuse vers ABI C entre ConsoleVR, World et Labo ; DLL et cycle de vie du laboratoire, séparation physique/rendu/audio, publication GPU/forêt, PhysX, modes de développement et commandes. Beaucoup de validations statiques, puis régressions de compilation et de démarrage dans les versions tardives. |

**Reprise :** revalider d'abord le rendu, l'interface murale, les ombres, la maison, la locomotion et la capture de `1.1.9`. Réintroduire ensuite les fonctionnalités par domaine, en comparant les images et les journaux à chaque étape ; ne pas importer d'un bloc `1.1.53–55` ou qualifier de « fonctionnel » ce qui n'a connu qu'une QA statique.

**Préservation Git :** l'état actuel de `CooLabVR/main` avant retour a été figé dans la branche `archive/pre-rollback-1.1.9-20261030`. L'archive originale n'est **pas** remplacée, et les travaux 1.1.50–55 restent consultables dans leur historique.
