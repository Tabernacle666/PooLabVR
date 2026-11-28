# PATCH-009 · Finitions et ossature de maison de banlieue

Kit résidentiel en mètres. Dans les glTF, +Y est vertical, +X suit le mur et +Z pointe vers la pièce. Garder le dossier kit entier, avec les BIN et les textures partagées. Le catalogue JSON expose les fichiers, cotes et connecteurs. Chaque pièce a ses propres noms et matériaux.

| Famille | Cotes nominales et usage |
| --- | --- |
| Gypse peint | Panneau de 1 219,2 × 2 438,4 × 12,7 mm, peint lin, sauge ou coquille. Face peinte, âme de gypse et papier arrière ont leurs propres matériaux. Pour une réservation ou une découpe, conserver les UV en mètres. |
| Ossature | Montants de 38 × 89 mm, entraxe nominal de 406,4 mm, double lisse haute, hauteur de référence 2 438,4 mm. Module nu ou avec la baie brute de la fenêtre 914 × 914 mm. Les rives ont des montants d'extrémité. Les partager dans une production continue selon le plan. |
| Poutre apparente | 140 × 235 mm, longueur 2 438,4 mm. Profil chanfreiné, connecteurs aux extrémités. Dimensions visuelles de la pièce. |
| Portes intérieures | Vantaux de 762, 813 ou 914 mm sur 2 032 mm. Jambages, butées, chambranles d'onglet, poignées et charnières. Baie brute = largeur du vantail + 50 mm et hauteur + 55 mm. |
| Fenêtres thermos | 914 × 914, 1 219 × 914 ou 1 524 × 1 067 mm. Baie brute = dimensions hors dormant + 20 mm dans chaque sens. Deux verres de 4 mm avec 12 mm d'air et un intercalaire par vantail. L'ouvrant coulisse sur un plan distinct du fixe. Tablette et tablier séparés. |
| Boiseries | Colonial ivoire satiné avec galbes et gorges en volume. Droit chêne miel avec arêtes adoucies. Plinthe de 108 mm, quart-de-rond de 16 mm, cimaise de 61 mm dont le haut s'aligne à **762 mm du sol fini** et chambranle de 66 mm. |
| Bois franc | Lattes de 57,15, 82,55 ou 127 mm par 19,05 mm. Érable clair et chêne miel. Lattes seules de 914,4 mm ou modules de parquet de 2 438,4 mm avec joints décalés et microchanfreins. Le catalogue donne la largeur exacte de chaque module. |
| Escalier | Marche de 283 mm de profondeur avec nez de 23 mm, giron de 260 mm, contremarche de 180 mm, largeur de 914 mm. Pas répétable et volées à 14 ou 15 hauteurs, soit 2 520 ou 2 700 mm de plancher à plancher. La volée se termine au connecteur du palier haut. |
| Cadre photo | Deux finitions avec feuillure, passe-partout, image originale, verre de 2 mm et point d'accroche. |

Les volées incluent les limons. La pièce d'étage fixe la trémie, les paliers, l'échappée, les mains courantes et les garde-corps. Les cotes du giron, de la contremarche et de la profondeur des marches suivent les plages du CNB 2020, division B, articles 9.8.4.1 et 9.8.4.2 pour un escalier privé à marches rectangulaires. Le tableau 9.5.5.1 différencie les dimensions des portes selon le local. Les tailles du catalogue sont celles des vantaux et des baies brutes. Ce kit sert à l'assemblage 3D. Les exigences de construction dépendent du bâtiment et du projet.

- [CNB Canada 2020, volume 2 et partie 9](https://publications.gc.ca/collections/collection_2022/cnrc-nrc/NR24-28-2020-fra.pdf)
- [Cahier explicatif du CNB 2020 modifié Québec, RBQ](https://www.rbq.gouv.qc.ca/fileadmin/medias/pdf/Publications/francais/cahier-explicatif-changements-cnb-2020.pdf)

## Pose et variation

1. Choisir un style de boiserie stable par pièce ou maison. Trier les portes et fenêtres par **baie brute**. Autoriser un choix explicite ou un tirage déterministe à partir de la graine du bâtiment et d'un identifiant de placement stable. Conserver les dimensions de la baie et du plancher au rechargement.
2. Poser portes et fenêtres au centre en X, dans le plan médian du mur. La porte s'aligne au sol fini, la fenêtre au bas du dormant. Poser les profils linéaires entre les repères début et fin, l'origine sur la face de gypse. Tronçonner selon le plan en gardant la section et les UV en mètres. Le champ anchor_height_m donne l'altitude de pose. Garder les onglets à leur échelle.
3. Garder une largeur de latte par pièce. Pour un plancher sur mesure, décaler les longueurs et la phase du fil entre rangées. Les modules ont une séquence fixe par largeur. Les lattes sources permettent d'autres séquences.
4. Le vantail de porte pivote autour de +Y. La fenêtre possède un ouvrant qui coulisse vers +X et un fixe. Les deux vitres d'une unité isolante suivent leur vantail. Les verres sont en alpha blend. Les autres pièces exposent des matériaux réutilisables, par exemple wood et plaster pour la découpe. Les extras sont des données de branchement.
5. Un pas d'escalier avance de 260 mm vers −Z et monte de 180 mm. Pour une autre hauteur d'étage, choisir un nombre de hauteurs et répartir exactement la différence de niveau, puis régénérer la volée. Garder le nez, la largeur et le repère du palier cohérents.
6. Vérifier en lumière rasante les moulures, onglets, marches et joints du parquet. Ouvrir la porte et faire coulisser la fenêtre dans une baie témoin après raccordement. Les actions, la collision et la qualification MG2 restent à intégrer dans OASIS.

Les sources Blender et les cartes sont incluses. Depuis source, lancer « blender -b --python kit.py -- export », puis « python tangentes.py » avec NumPy pour corriger les tangentes nulles émises par l'exporteur à certaines arêtes. Les cartes du bois sont dérivées de [Wood Table Worn, Poly Haven](https://polyhaven.com/a/wood_table_worn), [CC0](https://polyhaven.com/license). Les autres cartes et l'illustration sont originales.
