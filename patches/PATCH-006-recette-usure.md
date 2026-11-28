# PATCH-006 · Causeuse en velours côtelé, fin des années 1980

Deux GLB autonomes dans la même échelle et avec les mêmes ancrages. +Y vers le haut, +Z en façade, origine au sol. Les cartes PBR et toute la géométrie sont originales et générées pour ce lot. Les textures portent la trame serrée et les pores. Les côtes du tissu ont un pas de 6 mm. Sur les surfaces exposées des coussins, des dossiers et des accoudoirs, les crêtes ont un relief maillé d'environ 0,84 mm à neuf, réduit dans les zones polies.

| État | Contenu |
| --- | --- |
| `causeuse_neuf.glb` | Couture intacte, deux coussins bombés, teinte et côtes régulières. |
| `causeuse_use.glb` | Assises affaissées, textile plus irrégulier et plages de frottement. Le coussin de droite a une déchirure qui s'étend de l'assise au bord avant, un passepoil interrompu et un volume de mousse blanche évidé. Les fibres libres sont des meshes séparés, sans collision propre. |

Les états `neuf` et `use` contiennent tous deux `coussin_G`, `coussin_D`, `logement_G` et `logement_D`, avec les mêmes pivots et noms. Le slot mobile porte l'assise, le passepoil et les détails de son état. Avec l'action secondaire, détacher le coussin du logement en conservant sa pose. Au rangement, recaler son pivot dans le repère du logement libre. La déchirure, le tassement et les taches voyagent avec le coussin.

## Usure réutilisable

1. Conserver un état local par composant stable. Le temps et les contacts peuvent mettre à jour séparément la **compression**, l'**encrassement** et le **polissage**. L'ouverture de l'enveloppe et la perte de mousse forment des événements persistants. Une assise peut être tassée avec une housse intacte.
2. Mélanger les cartes de la housse avec les masques linéaires fournis. R porte la compression, G l'encrassement et B le polissage. Le polissage réduit la hauteur des côtes, atténue le microrelief et fait évoluer la rugosité. Les taches changent surtout la couleur. Un simple changement de teinte ne reproduit pas l'affaissement, qui demande une géométrie de référence adaptée à l'état. Le GLB usé livre un exemple d'état final par composant.
3. Lorsqu'une déchirure expose la mousse, publier une surface de mousse et un bord de toile avec leurs propres matériaux. Mettre à jour la collision et les surfaces visibles des rayons pour la pièce touchée. Sélectionner les détails de la mousse et des fibres selon leur contribution à l'image. La silhouette du coussin, la cavité et la couverture blanche doivent subsister dans les représentations simplifiées.
4. Sauvegarder l'état et la pose du coussin sous le même identifiant lorsqu'il est pris, reposé ou rechargé. Partager les variantes entre instances lorsqu'elles ont le même état. Sur une modification locale, réviser seulement les représentations dépendantes du composant.

Les masques d'auteur sont dans le pack usé. Les coordonnées suivantes utilisent le repère local glTF et sont bornées à [0, 1]. Pour `masque_assise.png`, u = 0,5 + X/0,562 et v = 0,5 + Z/0,604, avec v mesuré depuis le haut de l'image. Pour `masque_accoudoir.png`, u = 0,5 − Z/0,810 et v = 0,5 − Y/0,406. Appliquer le masque au tissu externe concerné. Le champ `COLOR_0` du GLB usé contient la modulation de couleur du rendu de référence. Les cartes de base, normales et de rugosité sont des PBR glTF standard. Le reflet textile diffus utilise en plus `KHR_materials_sheen`.

## Branchement OASIS et essai VR

Le chemin `Primitive` de [Assets.cpp](https://github.com/Tabernacle666/PooLabVR/blob/main/src/core/Assets.cpp) charge positions, normales et UV. Le rendu localisé des taches demande de transmettre `COLOR_0` ou de relire les masques dans le matériau. Le GLB usé conserve sa forme, sa mousse et son tissu de base même si l'import n'exploite pas cette modulation. Le sheen est une finition facultative.

Comparer neuf et usé à quelques mètres, puis à portée de main, puis près du coussin. À distance, vérifier la silhouette, le creux et la surface blanche. De près, faire glisser un éclairage rasant sur les côtes, le passepoil, la toile et la mousse. Retirer le coussin abîmé, le retourner, le reposer et recharger la scène. Le détail et l'état doivent suivre le coussin. Les GLB fournissent les sources à MG2. Les seuils et le coût sont à mesurer dans OASIS sur cet essai.
