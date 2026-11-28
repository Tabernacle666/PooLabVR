# OASIS — TODO DESIGN / ASSETS

Backlog vivant de production des assets. Les arcs de gameplay sont jetables ; les objets, composants et matières qu'ils révèlent constituent le patrimoine permanent.

## Dispatch

- Ordre initial : composants réutilisables → objets simples → composites → équipements spécialisés. Les groupes sont des priorités indicatives, pas des lots de commit imposés.
- Une case = un objet ou une petite famille cohérente de variantes. Ajouter librement des lignes avec un identifiant inédit D-xxx ; ne pas renuméroter les tâches existantes.
- [ ] = à produire ou à vérifier ; [x] = asset intégré (indiquer chemin / référence et commit). Une case ouverte ne signifie pas que l'objet est absent du dépôt. Vérifier et réutiliser l'existant avant de produire.
- Le prochain agent peut recevoir un identifiant, une plage ou une famille entière. Ne pas attendre une capacité Engine pour livrer un asset préparé.
- Aucun arc, métier, personnage ou mission obligatoire. Les objets sont utilisables par toutes les factions.

## Contrat asset commun

Respecter le format et la chaîne MG2 existants (voir [PLAN_LOD.md](PLAN_LOD.md)). Dimensions réelles, matières PBR, échelle, orientation, pivots et composants sémantiques stables. Préparer selon la pertinence : pièces mobiles ou amovibles, intérieur réellement exposé, interfaces d'attache/énergie/fluides, surfaces et états d'usure, collision, masse et points de préhension VR. Ne pas fusionner irréversiblement les parties interactives. Les petites pièces décoratives peuvent être agrégées ; ne pas imposer une instance physique indépendante par vis. Distinguer géométrie visuelle et représentation physique. Réutiliser les composants existants ; pas de refonte de pipeline sans mandat. Les capacités correspondantes sont dans [todo_engine.md](todo_engine.md).

## P01 — Matières et composants universels

- [ ] D-001 Roche, gravier, sable, terre, argile.
- [ ] D-002 Brique, bloc de béton, béton armé, gypse peint.
- [ ] D-003 Bois : bûche, planche, contreplaqué, panneau.
- [ ] D-004 Métal : barre, tôle, fil, profilé, tube.
- [ ] D-005 Verre : vitre, panneau, fragments génériques.
- [ ] D-006 Papier, carton, plastique, caoutchouc.
- [ ] D-007 Textile, corduroy, cuir, mousse, rembourrage.
- [ ] D-008 Vis, boulons, écrous, rondelles, rivets, clous.
- [ ] D-009 Charnières, axes, pivots, roulements, ressorts.
- [ ] D-010 Poignées, boutons, leviers, manivelles.
- [ ] D-011 Serrures, loquets, cadenas, clés, crochets.
- [ ] D-012 Corde, sangle, câble, chaîne, boucle, fermeture éclair.
- [ ] D-013 Roue, roulette, pneu, essieu.
- [ ] D-014 Tuyau, coude, raccord, joint, valve, robinet.
- [ ] D-015 Fil électrique, fiche, prise, interrupteur, fusible.
- [ ] D-016 Batterie, pile, alimentation, connecteur.
- [ ] D-017 Moteur électrique, engrenage, courroie, poulie.
- [ ] D-018 Vérin, piston, flexible hydraulique, pompe.
- [ ] D-019 Grille, filtre, membrane, joint d'étanchéité.
- [ ] D-020 Circuit imprimé, écran, capteur, voyant.
- [ ] D-021 Seau, bouteille, bidon, caisse, boîte, sac.
- [ ] D-022 Savon, chiffon, éponge, colle, adhésif, lubrifiant, peinture ; conditionnements et contenus.
- [ ] D-023 Carburant, huile, eau et autres consommables : contenants et représentations visuelles, pas de simulation imposée.

## P02 — Outillage et objets manipulables

- [ ] D-024 Marteau, masse, maillet.
- [ ] D-025 Tournevis, clés, douilles, pince.
- [ ] D-026 Ciseaux, couteau utilitaire, scie manuelle.
- [ ] D-027 Perceuse, visseuse, forets, accessoires.
- [ ] D-028 Scie électrique, meuleuse.
- [ ] D-029 Multimètre, thermomètre, ruban à mesurer.
- [ ] D-030 Cric, palan, treuil, étau.
- [ ] D-031 Établi, coffre et boîte à outils.
- [ ] D-032 Pelle, pioche, râteau, brouette.
- [ ] D-033 Balai, vadrouille, brosse, aspirateur.
- [ ] D-034 Assiette, tasse, verre, couverts.
- [ ] D-035 Casserole, poêle, ustensiles de cuisine.
- [ ] D-036 Aliments, conserves, emballages, boissons.
- [ ] D-037 Documents, cahier, crayon, enveloppe, classeur.
- [ ] D-038 Sac à dos, valise, portefeuille.
- [ ] D-039 Carrosse pour bébé : cadre, roues, textile, panier, attaches.
- [ ] D-040 Poupée, jouets, objets personnels.

## P03 — Mobilier et aménagement

- [ ] D-041 Vaisselier bois massif : vitrages, portes, tiroirs, tablettes, charnières ; réutiliser le travail existant.
- [ ] D-042 Causeuse corduroy : ossature, coussins amovibles, tissu, mousse, ressorts ; réutiliser le travail existant.
- [ ] D-043 Fauteuil, chaise, tabouret, banc.
- [ ] D-044 Table, bureau, table basse.
- [ ] D-045 Armoire, commode, bibliothèque, étagère.
- [ ] D-046 Classeur, casier, coffre de rangement.
- [ ] D-047 Lit, matelas, oreiller, couverture.
- [ ] D-048 Miroir, tableau, rideau, store, tapis.
- [ ] D-049 Comptoir, armoire de cuisine, évier.
- [ ] D-050 Poubelle, bac de recyclage, distributeur.
- [ ] D-051 Palette, rayonnage, caisse de transport.

## P04 — Architecture et équipements du bâtiment

- [ ] D-052 Portes pleines/vitrées, cadres, quincaillerie, ferme-portes.
- [ ] D-053 Fenêtres, façades vitrées, cadres, joints.
- [ ] D-054 Dalles, poutres, colonnes, murs, armatures.
- [ ] D-055 Cloisons, gypse, faux plafond, panneaux modulaires.
- [ ] D-056 Escaliers, échelles, rampes, garde-corps.
- [ ] D-057 Trappes, gaines, grilles, conduits.
- [ ] D-058 Ascenseur : cabine, rails, portes, motorisation, commandes.
- [ ] D-059 Signalisation, clôtures, barrières, mobilier extérieur.
- [ ] D-060 Réception, postes de travail, salle de conférence : assemblages réutilisables.
- [ ] D-061 Cuisine, salle de bain, chambre : assemblages réutilisables.
- [ ] D-062 Cabinet médical, pharmacie, salle d'examen : assemblages réutilisables.
- [ ] D-063 Atelier, garage, commerce, entrepôt et quai de livraison : assemblages réutilisables.

## P05 — Électricité, informatique et appareils

- [ ] D-064 Panneau électrique, disjoncteurs, transformateur, coffret.
- [ ] D-065 Prises, rallonge, multiprise, câblage apparent.
- [ ] D-066 Génératrice, chargeur, onduleur, batterie stationnaire.
- [ ] D-067 Ampoule, luminaire, lampe, ballast, interrupteur.
- [ ] D-068 Cafetière, bouilloire, micro-ondes, cuisinière.
- [ ] D-069 Réfrigérateur, congélateur.
- [ ] D-070 Ordinateur, clavier, souris, écran, imprimante.
- [ ] D-071 Serveur, rack, stockage, routeur, commutateur.
- [ ] D-072 Téléphone, radio, antenne, relais.
- [ ] D-073 Caméra, détecteur, capteur, terminal, moniteur de surveillance.
- [ ] D-074 Drone civil, batterie, radiocommande.
- [ ] D-075 Imprimante 3D, consommables, pièces génériques.

## P06 — Eau, feu, ventilation et chaleur

Réutiliser le réseau d'eau et les équipements déjà présents lorsque disponibles ; vérifier leur géométrie et leurs interfaces avant duplication.

- [ ] D-076 Tuyauterie du bâtiment, vannes, raccords, collecteurs.
- [ ] D-077 Toilette, lavabo, douche, baignoire, robinetterie.
- [ ] D-078 Pompe, filtre, réservoir, jauge.
- [ ] D-079 Chaudière, radiateur, thermostat, échangeur.
- [ ] D-080 Gicleur, extincteur, détecteur de fumée, alarme.
- [ ] D-081 Ventilateur, turbine, gaine, clapet, grille.
- [ ] D-082 Climatiseur, compresseur, unité de refroidissement.
- [ ] D-083 Chaufferie, local des pompes, salle électrique et local de ventilation : assemblages techniques.
- [ ] D-084 Représentations visuelles génériques : flaque, humidité, fuite, gouttes, fumée, cendre, suie, brûlure, poussière ; raccordables aux états Engine.

## P07 — Habillement, identité et santé

- [ ] D-085 Vêtements civils modulaires : pantalon, chemise, manteau, chaussures.
- [ ] D-086 Tenue de construction : casque, salopette, bottes, gilet.
- [ ] D-087 Tenue entretien/mécanicien : uniforme, gants, ceinture.
- [ ] D-088 Tenue corporative : complet, cravate, carte d'employé.
- [ ] D-089 Tenue médicale : sarrau, uniforme, accessoires.
- [ ] D-090 Tenue sécurité : uniforme, radio, accessoires.
- [ ] D-091 Tenue militaire : treillis, casque, gilet, sac.
- [ ] D-092 Lunettes, chapeau, masque, montre, accessoires de tête.
- [ ] D-093 Fauteuil roulant, brancard, béquilles, table d'examen.
- [ ] D-094 Pansements, attelles, trousse de premiers soins, flacons et emballages.

## P08 — Véhicules et machinerie (composants avant composites)

- [ ] D-095 Châssis, suspension, frein, roue, direction.
- [ ] D-096 Moteur, transmission, différentiel, radiateur, alternateur.
- [ ] D-097 Réservoir, batterie, siège, vitrage, tableau de bord, commandes.
- [ ] D-098 Portières, capot, coffre, carrosserie et points d'attache.
- [ ] D-099 Chariot manuel, diable, remorque.
- [ ] D-100 Vélo.
- [ ] D-101 Automobile (base roulante et variantes).
- [ ] D-102 Fourgonnette, camion utilitaire, camion-benne.
- [ ] D-103 Dépanneuse, plateau, treuil.
- [ ] D-104 Chariot élévateur, fourches, mât.
- [ ] D-105 Tracteur, chargeuse.
- [ ] D-106 Excavatrice, bras articulé, godet.
- [ ] D-107 Bulldozer, chenilles, lame, rouleau compresseur.
- [ ] D-108 Grue, flèche, crochet, contrepoids.
- [ ] D-109 Embarcation, coque, moteur, commandes.
- [ ] D-110 Hélicoptère, rotor, commandes, cabine.
- [ ] D-111 Véhicule blindé et équipements de protection propres à la fiction OASIS.

## P09 — Extérieur et ressources

- [ ] D-112 Arbres, branches, souches, bois mort et états de dommages ; préserver le patrimoine MG2 végétal.
- [ ] D-113 Arbustes, plantes, herbes, fougères, couverture du sol ; réutiliser les acquis.
- [ ] D-114 Rochers, berges, surfaces de terrain et matériaux de remblai.
- [ ] D-115 Équipements de pêche et de loisirs extérieurs.
- [ ] D-116 Chien et accessoires (collier, laisse) ; animation/comportement à part.

## Ajouts libres

Ajouter sous une famille existante ou ici : `- [ ] D-117 Nom : parties / variantes utiles.`
