# PATCH-010 · Mobilier de banlieue montréalaise

Quatre modèles originaux et trois états par modèle. Les états neuf et patiné partagent le même dessin et les mêmes ancrages. L'état endommagé ajoute une modification réelle de géométrie. Le catalogue donne les fichiers, les connecteurs, et les budgets de triangles. Conserver le dossier kit, avec ses buffers et textures partagées, lors de l'import.

## Cotes et pièces

| Objet | Cotes de référence | Pièces et variantes |
| --- | --- | --- |
| Abribus montréalais | Empreinte de toiture 3,820 × 1,600 m, hauteur du modèle 2,410 m | Aluminium extrudé, joints, ancrages, panneaux vitrés de 10 mm, assise, panneau publicitaire autonome sur le petit côté droit. Dans l'état endommagé, la vitre arrière centrale et son marquage disparaissent. |
| Boîte postale communautaire | 0,780 m de large, 0,518 m de profondeur et 1,663 m de haut | Module sur socle, deux colonnes, 17 cases individuelles et deux cases à colis. Portillons et cavités séparés. La variante endommagée cabosse en volume la tôle de la porte à colis gauche. |
| Banc de parc | Enveloppe de référence 1,930 × 0,665 × 0,820 m, assise autour de 0,44 m | Pieds à volutes et accoudoirs en métal noir, lattes de bois indépendantes, vis et ancrages. La variante endommagée fend une latte d’assise en deux parties avec tranches irrégulières. |
| Corbeille verte | Ø 0,464 m aux cerceaux, 0,471 m aux rivets, hauteur 0,737 m | Deux cerceaux d'acier, 24 lattes de bois peint, collerette ouverte, bac galvanisé amovible. La variante endommagée enlève deux lattes et laisse voir le bac. |

Le cahier de Design Montréal et de la STM donne l'empreinte universelle 1,60 × 3,82 m. La hauteur et l'écran sont propres à l'asset OASIS. La boîte postale reprend les cotes du module communautaire de Postes Canada. Le banc et la corbeille adoptent le gabarit de mobilier municipal. Les formes en fer forgé et les lattes vertes sont les variantes demandées. La fiche du banc de référence décrit de l'aluminium coulé, et la fiche Secteur 5 consultée décrit la variante en lattes recyclées. Les dimensions servent au placement de décors 3D.

- [Cahier des charges du concours d'abribus, Design Montréal et STM, section 5.1.3](https://designmontreal.com/sites/default/files/abribus_cahier_des_charges.pdf)
- [Postes Canada, Manuel des normes de planification de la livraison, annexe D](https://www.canadapost-postescanada.ca/cpo/mc/assets/pdf/business/standardsmanual_fr.pdf)
- [Ville de Montréal, fiches de mobilier du parvis de Biencourt](https://designmontreal.com/sites/default/files/annexes_programme_biencourt_lr.pdf)

## Écran et actions

Les fichiers sont en mètres, +Y vertical, +Z vers la façade. Les deux triangles de surface_video_9x16 sont sur le petit côté droit de l'abribus, normale +X. Surface utile 0,810 × 1,440 m en UV0 plein cadre, matériau noir_ecran au repos. Un vitrage mince indépendant sert de protection. L'entrée socket_power_in est à la base arrière du poteau droit, légèrement sous le sol. Le raccord porte le signal de jeu powered et le port electric_input.

Action primaire de l'abribus · lecture ou pause du MP4 sur la surface. Tant que powered vaut faux, le matériau reste noir. Au retour de l'alimentation, reprendre la lecture selon l'état sauvegardé. Publier les images décodées sur une texture vidéo séparée et la lire en émission. La vidéo est un état de matériau. Un changement de frame ne déclenche aucune reconstruction MG2. L'émission visuelle peut être testée sans la coupler au calcul de lumière dynamique. Le clip original de vérification pub_oasis_9x16.mp4 dure 6 s, 720 × 1280, H.264 sans audio. Le rendu d'aperçu montre sa première image. Le décodage, les contrôles et l'alimentation sont à raccorder dans le moteur.

Pour la boîte postale, action primaire sur le portillon assigné. Les nœuds casier_G_NN et casier_D_NN portent le pivot +Y. Les serrures sont des détails visuels. Pour le banc, les repères place_G et place_D donnent les assises. Pour la corbeille, centre_depot marque l'ouverture de 308 mm. L'action secondaire extrait bac_interieur par +Y.

## Usure et profils de dommage

| Profil | Matériaux et support à intégrer | État visible |
| --- | --- | --- |
| wood | Bois teint et bois peint vert, âme bois exposée | Usure du fini sous les mains et les intempéries, peinture écaillée. Un impact creuse ou fend la latte. La section fraîche garde son propre matériau. Une latte cassée reste gérée par son identité. |
| metal | Aluminium, acier peint, métal noir, acier galvanisé et acier nu | Patine propre à chaque métal. L'acier peint peut s'écailler en révélant son substrat. Une tôle déformée a une vraie concavité. Le profil métal de PATCH-005 pilote les réactions de l'outil en jeu. |
| glass | Verre trempé et vitrage de protection, panneau par panneau | Salissure en surface sur le pack patiné. En cas de bris, retirer la vitre et son marquage des consommateurs, conserver ses rails et les autres vitres, regrouper les fragments résiduels au sol. |
| display | Écran et entrée d'alimentation | Noir hors tension. Un défaut d'alimentation ou d'écran coupe la vidéo. Le vitrage protecteur reste une pièce séparée. |
| rubber | Joints EPDM | Vieillissement par rugosité et teinte. Joints conservés autour du verre et des profilés. |

Associer la patine à une valeur de finition fixée ou tirée par graine. Sauvegarder séparément les événements de dommage et les positions des éléments mobiles. La patine seule réutilise la géométrie. Pour les dégâts, employer l'identité du composant concerné et réviser sa géométrie, sa collision et ses contributions RTX de manière coordonnée. Les variantes endommagées fournissent des géométries de référence pour les essais. Les sources et événements de coupe suivent la convention du PATCH-005.

## Admission MG2 et vérification

- Garder le châssis, la toiture, les pieds, les lattes et les corps métalliques opaques dans les sources MG2. Prendre séparément vitres BLEND, écran vidéo, portillons, bac intérieur et composants révisables. Les détails statiques par objet peuvent partager un ensemble de clusters ou CLAS si leurs limites et identités le permettent.
- Conserver les ajours, les cavités, les bords d'appui et le volume de casse dans les LOD. Les rayons et la collision doivent lire l'état réellement publié. La microtexture d'usure reste dans les cartes. Aucune hiérarchie MG2 n'est précalculée dans ce pack glTF.
- Vérifier une séquence alimenter, lire, couper l'alimentation, reprendre. Déplacer le point de vue derrière la vitre brisée. Contrôler le creux de la tôle, les deux états de la latte et la corbeille sous un éclairage de biais.
- Textures du bois adaptées de [Wood Table Worn, Poly Haven](https://polyhaven.com/a/wood_table_worn), sous [CC0](https://polyhaven.com/license). Les textures d'usure, la publicité, les marquages et la géométrie du pack ont été créés pour OASIS.

Les sources Blender et leurs cartes sont dans source. Depuis ce dossier, lancer « blender -b --python kit.py -- export » pour régénérer les glTF. Le script maps_build.py génère les cartes d’usure et l’affiche à partir des cartes de référence livrées.
