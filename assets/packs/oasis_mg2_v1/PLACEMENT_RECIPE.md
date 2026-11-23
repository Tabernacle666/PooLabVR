# OASIS MG2 — recette de placement et auto-correction

## Contrat réutilisable pour tous les bâtiments

La source d'autorité est `placement_recipe_v2.json`, référencée par
`runtime.json`. Chaque modèle hero décrit son axe avant **dans le GLB d'origine**;
l'import MG2 convertit le Z glTF vers le Z moteur avant de calculer le yaw.
Les objets ne partagent pas nécessairement le même avant physique. Un nouveau
logo OASIS réutilise cette règle sans intervention pièce par pièce.

- Bureau : la position réelle du moniteur est extraite de la recette
  `recipes/office_single01.json` après conversion des axes. La chaise est
  placée de l'autre côté du bureau et tournée vers le moniteur, avec un
  décalage et une rotation déterministes liés à l'identité de la salle.
- Rack : la face maintenance est orientée vers l'allée accessible (+Z
  relatif à la géométrie de la salle serveur), jamais contre le mur.
- Écran sysadmin : le moniteur se trouve du côté écran du plateau; sa face
  lisible regarde la chaise, dont l'assise fait face au moniteur.
- Extincteur : l'étiquette et le manomètre font face à la circulation
  ou vers l'intérieur de la pièce, pas vers le mur.
- Portes : le socket charnière et la direction de l'ouverture structurale
  conservent la référence; poignées des deux côtés.

## Correction automatique et retour de recette

Tout placement erroné est **rectifié avant l'instanciation**. Une correction
notable (translation ou angle) est inscrite dans le diagnostic et dans un
fichier `mg2_placement_fixes.tsv` local au dossier des sauvegardes du bâtiment,
ou dans `.oasis-local/` pour les sessions de diagnostic. Il n'y a aucun coût de
modification de meshes, aucun téléchargement et aucune mise à jour Git à
chaque correction. Ces retours sont destinés à améliorer la recette.

## FIX-ROOM à la demande dans le Tool

Dans le Tool, ouvrir la page **Bâtiment**, sélectionner l'opération
**FIX-ROOM**, puis appliquer avec le contrôle **S**. Pour une salle :
le mobilier MG2 résident est supprimé puis régénéré avec la recette actuelle.
Dans le corridor, l'escalier ou la salle de bain, l'opération corrige le décor
technique de l'étage. Les dégâts du bâtiment ne sont pas changés par FIX-ROOM.

## Régénération complète autorisée pour cette livraison

`Buildings::PlacementRecipeRevision=2` provoque **une seule fois**, au prochain
lancement, la génération des cinq bâtiments OASIS d'après leurs profils.
Les anciens journaux de dommages, percements et variantes de test sont
abandonnés, puis la nouvelle sauvegarde est écrite immédiatement.
Aux lancements suivants, les nouveaux dommages sont conservés normalement.
Dans les sessions sans chemin de sauvegarde, la génération est déjà nouvelle.

## Contrôles statiques de livraison

La recette définit un avant pour chacun des 15 emplacements hero. Le pack
runtime doit inclure `placement_recipe_v2.json`, même lorsqu'il utilise un
overlay PBR local. La validation visuelle et la compilation MSVC restent à
effectuer par une boucle `commit_and_loop.bat mg2`. Aucun changement de
politique mémoire, qualité des GLB ou seuil de LOD.

## Correctifs de visite — recette commune à tous les étages

- Les panneaux blancs provisoires `whiteboard01` et `whiteboard_mobile02`
  restent disponibles dans le catalogue, mais sont exclus du placement
  automatique. Les anciens prismes de bureau/écran ne sont jamais réactivés
  à la dépublication des accessoires MG2.
- Les accessoires reposent sur la borne basse de leur GLB et le dégagement
  `tabletop_clearance_m`; les empreintes de meubles sont calculées après
  inversion de Z et rotation, avant vérification d'intersection.
- Le cadre, le seuil et la charnière de porte restent immobiles dans le mur;
  seuls les nœuds du battant pivotent. Aux bureaux qui longent un escalier,
  l'ouverture est renvoyée vers l'intérieur de la pièce pour libérer l'accès.
  Les marches et les identifiants de dommages restent intacts.
- Chaque faisceau réseau suit un riser DATA vertical et se place sur la face
  extérieure de sa gaine, jamais au milieu du corridor. Les déplacements
  appliqués sont enregistrés dans le diagnostic.

Ces placements s'appliquent à chaque occurrence régénérée ou streamée,
sans réinitialiser les dégâts des bâtiments. Compilation MSVC et examen VR
restent à confirmer.
