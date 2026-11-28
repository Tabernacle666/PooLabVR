# OASIS — TODO ENGINE / CAPACITÉS

Backlog vivant des capacités générales qui rendent interactif le patrimoine de [todo_design.md](todo_design.md). Les 100 arcs de gameplay servent uniquement à découvrir les besoins ; aucun arc n'est à coder.

## Dispatch

- Priorité initiale : capacités qui débloquent le plus d'assets, puis phénomènes et systèmes spécialisés. Les dépendances priment sur l'ordre si nécessaire.
- [ ] = à intégrer ou à auditer ; [x] = intégré (indiquer commit et validation disponible). Une case ouverte n'implique pas l'absence de code existant.
- Ajouter librement des tâches E-xxx inédites. Un agent peut traiter une tâche ou une famille en un commit cohérent ; pas de découpage artificiel.
- Auditer l'amont/aval avant modification, exploiter les systèmes existants et préserver le contenu. Ne pas interrompre le chantier MG2 actif : [PLAN_LOD.md](PLAN_LOD.md) demeure son document de dispatch. Les travaux compatibles peuvent progresser sans réécrire ce plan.
- DX12 / OpenXR, RTX 5060 Ti 16 Gio, DDR4 64 Gio, Quest 3S 72 Hz (13,89 ms/image). Simulation locale/événementielle, sommeil des corps, agrégation et résidences adaptées ; ne pas supprimer les effets ou objets pour atteindre un chiffre. Approximer ce qui n'a pas de conséquence perceptible. Validation ciblée par diagnostic ou observation en VR ; distinguer intégré et testé.
- Simulation de jeu : propriétés et interactions génériques, pas de modèle détaillé de dispositifs réels dangereux. Les phénomènes peuvent exister logiquement avant leur rendu avancé.

## P01 — Socle universel (maximum d'assets débloqués)

- [ ] E-001 Identité persistante d'un objet et de ses composants ; état, position, appartenance, provenance. Références stables entre simulation, rendu MG2 et sauvegarde.
- [ ] E-002 Registre de matières physiques indépendant du matériau visuel : densité, friction, rigidité, fragilité, absorption, conductivité, inflammabilité.
- [ ] E-003 Corps rigides : masse, inertie, collision, impulsion, empilement, sommeil/réveil, représentation collision distincte du rendu.
- [ ] E-004 Manipulation VR générique : saisir, poser, lancer, porter à une/deux mains, utiliser les points de préhension ; contraintes et retours.
- [ ] E-005 Composants et assemblages : relation parent/enfant physique et sémantique, attaches amovibles, charnières, glissières, pivots et contraintes.
- [ ] E-006 Interfaces d'action génériques : bouton, poignée, levier, interrupteur, outil et cible compatible ; éviter les scripts par objet.
- [ ] E-007 Conteneurs physiques/logiques : ouverture, fermeture, contenu, capacité, masse cumulée, transfert.
- [ ] E-008 États matière/objet : sale, propre, humide, sec, chaud, froid, usé, endommagé, réparé ; événements de changement.
- [ ] E-009 Outillage universel : appliquer une action compatible avec une matière, une attache ou un composant ; usure et consommation abstraites si pertinentes.
- [ ] E-010 Démontage, assemblage et crafting générique : pièces, compatibilité, ressources consommées, résultat persistant ; pas de recettes spécifiques obligatoires par objet.

## P02 — Destruction, transformation et structure

- [ ] E-011 Dommages localisés : énergie/impulsion de jeu et résistance matière ; distinguer composant abîmé et composite détruit.
- [ ] E-012 Fracture de verre, bois et autres matériaux ; fragments visibles et réutilisables quand pertinents, agrégation des débris secondaires.
- [ ] E-013 Découpe/perforation générique de géométrie compatible ; réutiliser la destruction OASIS existante, actualiser collision et représentation MG2/RTX.
- [ ] E-014 Déformation locale des matériaux souples et tôles, sans exiger immédiatement une simulation continue de tous les objets.
- [ ] E-015 Structure et supports : perte de support, chute, stabilité, contraintes et effondrement local ; cohérence visuelle et physique.
- [ ] E-016 Réparation, remplacement, nettoyage et restauration ; conserver l'identité et l'historique des pièces pertinentes.

## P03 — Électricité et eau fonctionnelles (priorité de gameplay)

- [ ] E-017 Graphe électrique logique : sources, consommateurs, circuits, interrupteurs, panneaux et état alimenté ; connecter les équipements déjà présents.
- [ ] E-018 Pannes, protection et réarmement des circuits : états abstraits de défaut/surcharge ; dépendances entre appareils et panneaux.
- [ ] E-019 Réseau d'eau existant : auditer la topologie et exposer état ouvert/fermé, disponibilité, fuite et connexions des appareils ; ne pas reconstruire le réseau sans motif.
- [ ] E-020 Humidité et dégâts des eaux : état par surface/objet, zone de flaque abstraite, propagation locale simplifiée, effets visuels et séchage.
- [ ] E-021 Interactions eau-électricité : réactions de gameplay configurables (défaut, disjonction, zone dangereuse fictive), cohérence de l'état du réseau et du rendu ; aucune simulation électrique réaliste nécessaire.
- [ ] E-022 Consommation et autonomie : batteries, carburant, eau et autres consommables ; stockage et transfert abstraits.

## P04 — Éléments : feu, vent, eau, terre

- [ ] E-023 Feu : ignition de jeu, état en combustion, propagation locale par matière, extinction et résidus.
- [ ] E-024 Chaleur : température par objet/zone, chauffage, refroidissement, cuisson et détérioration ; transfert simplifié.
- [ ] E-025 Fumée : densité, opacité, visibilité, détection, dissipation ; représentation locale avant simulation volumétrique avancée.
- [ ] E-026 Vent : direction, intensité, volumes locaux, force sur objets légers, végétation et particules ; réutiliser les effets existants.
- [ ] E-027 Ventilation : connexions entre volumes, ventilateurs, portes/clapets ; transport abstrait de chaleur, fumée et humidité.
- [ ] E-028 Eau visible : flaques, gouttes, ruissellement, fuites, réactions aux surfaces ; découplée de la simulation intégrale.
- [ ] E-029 Eau en réseau : volume, débit, pression abstraite, pompes, réservoirs, vannes et conservation utile des ressources.
- [ ] E-030 Flottabilité, courant et immersion ; interaction locale avec objets et véhicules.
- [ ] E-031 Eau avancée : écoulement local volumétrique uniquement lorsque le gain de gameplay justifie le coût ; ne bloque pas E-019 à E-030.
- [ ] E-032 Terre : retrait/ajout de volume, excavation et remblai, mise à jour collision et géométrie ; distinguer sol et structures.
- [ ] E-033 Matière granulaire, érosion et déplacement de terre avancés ; extensions de E-032.
- [ ] E-034 Interaction croisée des éléments : extinction, humidité, chaleur, vent, fumée, poussière et terrain, avec conséquences cohérentes plutôt que scénarios prédéfinis.

## P05 — Habillement et interaction sociale

- [ ] E-035 Habillement modulaire : retirer, porter, échanger des vêtements ; compatibilité avatar, état visuel et équipement.
- [ ] E-036 Apparence, identité et reconnaissance : rôle apparent distinct de l'identité/faction réelle ; règles de perception et d'accès du monde fictif.
- [ ] E-037 Propriété, permissions et contrôle territorial : locaux, objets, véhicules, ressources et changements de contrôle.
- [ ] E-038 Échanges, contrats simples, valeur, inventaire partagé et information entre joueurs ; métiers émergents et facultatifs.
- [ ] E-039 Communication de proximité et radio, informations non omniscientes ; propagation et disponibilité selon les systèmes du jeu.

## P06 — Mécanique et véhicules

- [ ] E-040 Motorisation générique : couple, régime, consommation, activation et panne.
- [ ] E-041 Transmission, engrenage, direction, freinage, suspension et adhérence.
- [ ] E-042 Attelage, levage, traction, treuil, charge suspendue et arrimage.
- [ ] E-043 Hydraulique/pneumatique abstraite : pompe, vérin, articulation, force et charge.
- [ ] E-044 Véhicules : conduite, composants remplaçables, dégâts localisés, déformation de carrosserie, perte progressive de fonctions.
- [ ] E-045 Machinerie : actionneurs et outils compatibles avec les capacités universelles de levage, déplacement, excavation et destruction.
- [ ] E-046 Navigation aquatique et aérienne : comportement dynamique, commandes, énergie et dommages, en réutilisant la base véhicule.

## P07 — Monde vivant et infrastructures

- [ ] E-047 Avatar : mobilité, posture, blessures, fatigue, assistance, portage et soins simulés.
- [ ] E-048 Consommation, alimentation, confort thermique et repos ; granularité de jeu configurable.
- [ ] E-049 Informatique et données de jeu : appareils, supports, contenus persistants, réseau logique, perte/restauration de services.
- [ ] E-050 Caméras, capteurs, drones : observation et transmission d'information dans le monde.
- [ ] E-051 Acoustique : émission, distance, atténuation par matières/obstacles, localisation et événements.
- [ ] E-052 Navigation et accessibilité dynamiques : portes, ascenseurs, escaliers, obstacles, modifications du terrain et du bâtiment.
- [ ] E-053 Comportement animal et animation : déplacement, réactions aux joueurs, objets et environnement.
- [ ] E-054 Combat de jeu et dommages aux objets/avatars : réutiliser les capacités existantes, interactions avec matériaux et structures ; règles fictives configurables.

## P08 — Transversal (à intégrer progressivement, pas à reporter à la fin)

- [ ] E-055 Persistance des états et des modifications : sauvegarde, chargement, versionnement et références stables.
- [ ] E-056 Multijoueur : autorité, réplication des états pertinents, ownership, interactions concurrentes et conflits d'assemblage.
- [ ] E-057 Budget de simulation : spatialisation, sommeil, événements, niveaux de détail physiques et réseau ; découpler fréquence de simulation et fréquence de rendu.
- [ ] E-058 MG2 dynamique : composants actifs, fragments, assemblages et changements d'état ; réutiliser le catalogue et les mécanismes de publication existants, sans pipeline parallèle.
- [ ] E-059 Diagnostic ciblé : coût CPU/GPU, corps actifs/endormis, événements, résidences, changements géométriques et trafic réseau ; activer seulement ce qui est utile au chantier.
- [ ] E-060 Validation de composition : vérifier plusieurs objets d'une même famille et une interaction entre systèmes, sans implémenter de mission ou d'arc dédié.

## Ajouts libres

Ajouter sous une famille existante ou ici : `- [ ] E-061 Capacité générique : résultat observable et dépendances éventuelles.`
