# OASIS — passage Hotel/PBR/MG2 : contrat de livraison

Derniere livraison: 132 GLB, 27 profils actifs et 28 recettes, sans acquisition CC0 embarquee.

## Ordre de generation

1. Charger `main` avant un nouveau chantier (aucun branch ni PR). Conserver les fichiers locaux `.oasis-local/mg2_runtime` et les textures hors Git.
2. Pour la premiere visite apres changement de `PlacementRecipeRevision=3`, les plans des cinq lettres sont regeneres; les anciens dommages de test sont reset. La migration de revision n'est pas incrementee pour une simple modification de mesh.
3. `pbr_lot1.bat` : enrichissement CC0 de l'overlay local. Ce script peut retelecharger les maps et n'est pas execute par ce commit.
4. Recompiler le moteur pour les changements C++; `run.bat` stage la source et l'overlay local, puis se lance avec diagnostic. Un script de simple execution ne peut pas compiler les modifications C++ tout seul.

## Contenus effectivement raccordes

- Hall ouvert en facade, vitres commercial et ventaux statiques representes ouverts; portiques d'ascenseurs MG2 associes aux fonds du hall, deux cages ascenseurs motorisees deja existantes.
- Conciergerie, depanneur avec produits, salon, cuisine, gym, spa, sauna, chambres, maintenance et bureau sur 27 profils residents. Selection stable par programme, hote de piece et ordre de meubles; collisions locales controlent les chevauchements, detail arrive avec la distance du visiteur.
- Toiture H, signal lumineux ambre a quatre positions, groupes AIR vers HVAC reel charge via MG2 en toiture et a proximite; stair ext O/I a deux voles par niveau avec paliers dans les corridors.
- Le bureau propose une tasse ceramique originale LIBERTE55 parmi les choix optionnels.

## Differences entre annonces et rendu AAA final

- **Pas d'objets photogrammetriques dans ces 13 GLB** : la geometrie est originalement parametrique, meme si les maps 2K peuvent etre ajoutees localement.
- Les portes d'ascenseur ajoutees sont visuelles (pas encore synchronisees au cycle d'ouverture); portes commerciales ouvertes non motorisees, spa/poele/sauna/tapis roulant non interactifs. Les objets n'ont pas de simulation complexe de fluides, chaleur ou poids.
- Escaliers exterieurs O/I uniquement; A/S restent sur les voies internes. Aucun certificat d'accessibilite/evacuation.
- Aucun profil de performances Quest 3S ni test de rendu n'a ete execute dans ce changement distant. Aucun grand fichier local prive n'est lu ou modifie.

## Diagnostic bref attendu lors de la prochaine visite

Chercher `mg2_room_profiles loaded`, `mg2_rooftop_hvac=ready`, `mg2_room_asset_failed` et `mg2_worktop_prepare` dans les journaux. Les deux autres controles qualitatifs restent le visuel du hall / patio et l'absence de coups de lumiere provoquant des recaptures de sphere (on anime uniquement la puissance des points lumineux ambre).

## Contrat de placement MG2 v3 : quatre ancrages (pas seulement un seed)

`room_profiles_v1.json` specifie maintenant un `mount` pour les 70 placements problematiques qui etaient auparavant au plancher:

- `floor` (implicite) : empreinte de collision locale, reservee avant tout autre contenu.
- `wall` avec `at_height_m` : recherche d'une vraie cloison opaque de la piece, ancrage a la face interieure, yaw vers la piece. Exemples : horloges, tableaux, lecteurs d'acces.
- `countertop` : surface existante (`sector.mounts` pour le bureau) ou meuble porteur deja place (reception, kitchenette, petite table). Ajuste la hauteur sur la face superieure du support et evite l'intersection entre accessoires.
- `service` : requiert un `ServiceNode` intersectant la piece, et ne cree pas de conduit sans precedent physique. Les risers communs sont toujours geres par la couche de service.

Le chargeur `src/app/Mg2Catalog.cpp`, le streamer `src/app/BuildingSession.cpp` et la synchronisation `tools/sync_oasis_mg2_catalog.mjs` traitent ce contrat. La selection se fait en deux passes : meubles du sol, puis accessoires dependants. Si un support manque, seul l'accessoire sans support n'est pas installe; le GLB reste dans le catalogue et la recette. Le nouveau sol du hall/conciergerie/cuisine/spa est en ceramique, le depanneur et maintenance en beton, les suites/salons/gym/saunas en bois; tout reste compatible avec les vrais PBR architecturaux acquis dans l'overlay local.

Les supports restent limites aux types declares et aux surfaces deja exposees par l'architecture. Le prochain enrichissement du catalogue peut ajouter d'autres meubles porteurs et sockets authoring au lieu de supprimer des objets ou de relacher la collision.
