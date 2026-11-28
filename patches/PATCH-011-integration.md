# PATCH-011 · Bibliothèque de classiques et reliures anciennes

Module en noyer de **1,080 m** de pas, environ **0,37 m** de profondeur hors tout et **2,126 m** de haut. En glTF, +Y est le haut et +Z la façade. Les connecteurs alignement_gauche et alignement_droit permettent de composer un mur de rayonnages. Cinq tablettes, dos à planches, corniche, traverse cintrée, rosace, plinthe et moulures latérales en géométrie. Le meuble existe vide et dans un agencement clairsemé. La troisième tablette du milieu reste vide.

| Fichier ou ensemble | Contenu |
| --- | --- |
| kit/bibliotheque_vide.gltf | Corps, cinq tablettes et repères de pose. |
| kit/bibliotheque_clairsemee.gltf | Même corps, **23 livres** indépendants sur quatre tablettes, répartis en **six groupes** autour des vides. Les exemplaires d’un titre partagent leurs meshes. |
| kit/bibliotheque_clairsemee_agregee.gltf | Même implantation et même corps. Une seule primitive à matériau opaque par groupe, avec dos texturés, hauteurs en escalier, tranches et espaces préservés. |
| kit/livre_*.gltf | Six titres seuls. Le dos bombé, ses nerfs, les plats arrondis, les tranches et les tranchefiles sont des volumes séparés. Le cuir a des cartes PBR de grain, de craquelures, de patine et de dorure partiellement polie. |
| kit/livre_lafontaine_i_ouvert.gltf | Pose de lecture distincte. Pages courbées, page de titre, deux vers du Corbeau et le Renard et résumé de la fable. |
| catalogue.json | Titres, auteurs, notices, dimensions, fichiers et mesures de géométrie. |
| emplacements.json | Positions, nœuds et correspondance exacte entre chaque livre et le groupe qui peut le représenter. |

Garder le dossier kit entier avec ses buffers BIN et ses textures partagées. Les 23 livres représentent 154 376 triangles à leur détail source et 276 triangles dans les six groupes. Ces comptes excluent les 10 452 triangles du meuble et ne sont pas des mesures de coût par image. Les couvertures, les fers d’ornement et les notices sont des créations du pack. Le texte original du court extrait de La Fontaine et les œuvres utilisées sont dans le domaine public. Les six notices sont écrites pour OASIS. Les textures du noyer dérivent de [Wood Table Worn, Poly Haven](https://polyhaven.com/a/wood_table_worn), en [CC0](https://polyhaven.com/license). Les textures de reliure et de papier sont originales.

## Prise en main et lecture

Le nœud du livre porte book_id et slot_id dans la scène garnie. Le repère prise donne le point d’attache à la main. Le repère pose_livre correspond à l’assise du livre sur la tablette. La quatrième de couverture porte la notice lisible, également fournie en UTF-8 dans le catalogue pour une lecture VR sans agrandir le modèle. L’action primaire est prendre/reposer. Le moteur peut présenter la pose ouverte des Fables I en action de lecture. La pose ouverte a sa propre géométrie et des pages fixes.

Garder l’identité d’un livre par couple identifiant du meuble et slot_id, y compris lorsqu’il est tenu, rangé ou chargé d’une sauvegarde. Conserver sa pose canonique de rangement. Une rangée agrégée ne prend pas les décisions de collision ou d’interaction du livre. La prise résout le livre logique de l’emplacement et publie sa représentation de proximité pour la main.

## Essai MG2

1. Importer les deux scènes garnies comme coupes alternatives. Pour une instance, garder le corps de meuble une seule fois. La source détaillée conserve les vraies épaisseurs, les hauts et bas de tranches et la lecture à la main. L’agrégat garde les vides entre petits groupes et les légers écarts entre les livres. Il utilise le même UV de dos et le même atlas pour garder les couleurs.
2. Faire le test à portée de main et autour de 2 m sur la même bibliothèque et la même exposition. Le moteur sélectionne une coupe par erreur projetée et par œil. La distance de 2 m est un point de comparaison. Les représentations sont des sources d’essai. L’intégration du sélecteur, des clusters, de la résidence, du raster et des structures RTX reste à faire.
3. Pour substituer une rangée, publier l’agrégat ou précisément les membres listés dans emplacements.json. Éviter qu’ils apparaissent tous les deux dans un même consommateur. Le budget d’erreur est à qualifier en VR. Les textes de dos restent perceptibles dans l’atlas, tandis que la finesse des arêtes et des nerfs suit la coupe du moteur.
4. Quand un livre sort du rayonnage, invalider le groupe concerné et conserver les enfants restants jusqu’à production d’une représentation qui exclut le livre. Appliquer la même règle au rangement ou à une modification de l’agencement. Les groupes voisins et le corpus gardent leurs sources et leur résidence. Le tirage éventuel d’un agencement peut choisir des groupes permis ou laisser des zones libres, avec une graine stable.

## Sources des œuvres

- [Jean de La Fontaine, Fables, livre I, édition numérique française](https://www.gutenberg.org/files/17941/17941-h/17941-h.htm)
- [Jean de La Fontaine, Fables, édition originale, livres I–II](https://fr.wikisource.org/wiki/Fables_de_La_Fontaine_(%C3%A9dition_originale))
- [Charles Perrault, Histoires ou Contes du temps passé, 1697](https://fr.wikisource.org/wiki/Histoires_ou_Contes_du_temps_pass%C3%A9_(1697)/Original)
- [Jules Verne, Le Tour du monde en quatre-vingts jours](https://www.gutenberg.org/ebooks/46541)
- [Jules Verne, Vingt mille lieues sous les mers](https://www.gutenberg.org/ebooks/54873)
- [Victor Hugo, Notre-Dame de Paris](https://www.gutenberg.org/ebooks/19657)

Les sources Blender sont sous source. Avec Python, Pillow et SciPy, exécuter textures.py pour régénérer les cartes de papier et de cuir et leur atlas. Blender 4.3 exporte les modèles avec « blender -b --python kit.py -- export ». Les options meubles, cuirs, ouvert, comparaison génèrent les quatre vues de contrôle.
