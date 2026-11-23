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
