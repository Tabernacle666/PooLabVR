# PATCH-005 · Vaisselier en chêne et recette d'endommagement OASIS

Accessoire indépendant à intégrer. Dimensions nominales 1,58 × 0,55 × 2,16 m. GLB autonome avec l'origine au sol, +Y vers le haut et +Z en façade. Le fichier s'ouvre fermé. Les rendus ouverts montrent le débattement prévu.

Le corps est assemblé en planches d'épaisseur réelle, avec dos, tablettes, montants et traverses. Les panneaux bas ont une surface usinée continue. Contre-courbes, cavets, sillons et feuilles en relief font partie de la géométrie. Les quatre vitres sont distinctes, la quincaillerie est séparée du bois et chaque pièce de vaisselle peut être prise individuellement. Les tasses, assiettes et bols ont un fond et une paroi interne.

Le bois utilise les cartes PBR du matériau [Wood Table Worn de Poly Haven](https://polyhaven.com/a/wood_table_worn), adaptées en couleur et rugosité. Les cartes source sont [CC0](https://polyhaven.com/license). Le fichier `bois-tranche.jpg` est une carte plus claire de départ pour les coupes fraîches. Les textures peuvent détailler le grain. Les déformations et les cassures sont portées par le mesh.

## Interaction

| Nœud ou groupe | Action |
| --- | --- |
| `porte_basse_G/D`, `porte_vitree_G/D` | Action primaire sur la poignée. Basculer ouverte ou fermée autour du +Y local au pivot de charnière. Le sens et l'angle sont stockés dans `extras`. |
| `tiroir_G/D` | Action primaire sur l'anse. Translation de 0,298 m suivant le +Z local. Le tiroir a un fond, deux côtés et un dos qui se déplacent ensemble. |
| `assiette_*`, `soucoupe_*`, `tasse_*`, `bol_*` | Action secondaire. Prendre et reposer la pièce. Les nœuds `socket_assiette_*` et `socket_tasse_*` donnent des positions de rangement. Valider l'accès et la place libre avant de reposer. |

Les nœuds portent `component_id`, `role` et, pour les surfaces, `material_profile`. Ces champs sont des données d'intégration à lire dans le catalogue. La granularité de dommage est la pièce physique. Deux primitives d'une assiette peuvent représenter l'ivoire et le bleu du même tesson. Les fenêtres translucides ont un `fracture_group` par vitre. Une porte reste l'assemblage de ses traverses, montants, prises et vitres.

## Recette commune par matériau

| Profil physique | Projectile | Laser | Surface intérieure et état |
| --- | --- | --- | --- |
| `wood` · Bois massif | Perforer ou creuser selon la traversée. Ouvrir une lèvre irrégulière avec quelques fibres en volume sur le bord. | Retirer de la matière à la première surface de bois atteinte. Émettre un panache de fumée marqué pendant l'ablation. | Tranche fraîche et rugueuse, fil du bois lisible, zone noircie limitée au passage du laser. |
| `plaster` · Gypse | Cratère friable, bord écaillé et poussière. | Couper et produire un panache blanc abondant, distinct de la fumée sombre du bois. | Noyau clair et mat, lèvres irrégulières. Ce profil sert aussi aux murs. |
| `metal` · Métal | Déplacer localement la surface vers l'intérieur, relever une petite lèvre et marquer la finition exposée. La pièce garde son volume général. | Produire un trajet réfléchi depuis la première surface, à partir de la normale de la pièce touchée. | Verrou et laiton gardent leur matière, le métal mis à nu change de rugosité. |
| `ceramic` · Grès et céramique émaillée | Écailler un bord ou faire sauter un fragment. Le fond de l'éclat est géométrique, brut et mat. | Arrêter le rayon sur la surface. Laisser la géométrie et la finition intactes. | Glacure brillante à l'extérieur, pâte mate sur le nouveau bord. Les bandes bleues font partie du matériau de la paroi. |
| `glass` · Vitrage | Retirer la vitre touchée du rendu, du RT et des collisions. Émettre des fragments transitoires. Au sol, les tessons peuvent rejoindre un maillage groupé par tuile, sans corps individuel persistant. | Traverser la vitre. La surface traitable suivante reçoit le rayon. | État brisé par vitre, persistent au rechargement. Les montants et les autres vitres gardent leur propre état. |

Ces réponses sont des choix de simulation pour OASIS. Le type de matériau, les dimensions de la pièce et la surface touchée sélectionnent la réponse. La couleur, le nom du matériau visuel et l'alpha de rendu restent des paramètres de présentation.

## Publication d'une modification

1. Identifier la pièce et l'intersection dans son espace local. Sur le trajet du laser, traverser le verre. Le premier autre matériau termine le trajet incident ou produit sa réflexion, selon le tableau. Pour un projectile, le vitrage doit encore participer à la requête même s'il ne bloque pas la découpe laser.
2. Recoller les sommets géométriquement communs pour calculer l'adjacence avant la coupe. Conserver les coutures UV et les frontières de matériaux dans le mesh de rendu. Retrianguler le creux ou la tranche, y compris le fond concave, avec normales et matériau intérieur. Une coupe traversant une paroi creuse doit respecter la cavité. Pour une fente partielle, la pièce reste connectée. Si le trajet sépare effectivement la pièce, fermer les tranches et publier un objet par composante détachée.
3. Sur les copies modifiées, publier ensemble géométrie visible, ombres, rayons et collisions. La révision de la pièce permet d'invalider les seuls clusters, volumes et partitions touchés. Les pièces modifiées restent soumises à la politique de LOD du moteur. Leur source de dommage reste la géométrie en mètres avec son épaisseur, indépendante de la coupe LOD de la caméra.
4. Sauvegarder l'identité stable de la pièce, la succession de modifications en espace local et l'état des vitres. Une semence stable par impact rend les ébréchures reproductibles. Réserver des entrées distinctes pour les objets détachés. La fumée et les fragments en vol ont une durée de vie de simulation.

## Points d'entrée à vérifier dans OASIS

Dans [`GraphicsCut::InitializeCutSource`](https://github.com/Tabernacle666/PooLabVR/blob/main/src/core/GraphicsCut.cpp#L43), la source courante parcourt les objets statiques et exclut entre autres les surfaces alpha blend ainsi que les objets dynamiques. La classification de solides est liée à certaines surfaces historiques. Dans [`BuildingCampus::Laser`](https://github.com/Tabernacle666/PooLabVR/blob/main/src/app/BuildingCampus.h), le chemin bâtiment abrège le rayon à la pièce admissible la plus proche. Les accessoires mobiles demandent donc une source de dommage enregistrée par pièce et un routage explicite pour verre, métal et céramique. Ces chemins existants ne démontrent pas, à eux seuls, une règle générale « première surface opaque » pour les meubles.

## Essai du vaisselier

- Ouvrir une porte et un tiroir. Sortir une tasse, la poser et la reprendre. Vérifier les axes et que la vaisselle repose sur les tablettes.
- Pointer à travers une vitre intacte. Le bois du fond porte le trait et la fumée, puis les faces nouvellement exposées répondent à la lumière. Pointer ensuite une pièce en métal ou en céramique et comparer leurs réponses.
- Comparer trois impacts proches sous le même éclairage rasant. Voir la pâte mate de la tasse, le creux du laiton et les fibres dans le bois quand la tête change d'angle. Le creux doit également modifier les ombres et les reflets.
- Briser une vitre puis regarder les tessons au sol. Former une coupe partielle dans un panneau, puis la compléter. Vérifier le passage d'une pièce connectée à deux objets, ainsi que l'accord entre rendu, ombres et collisions.
- Recharger la scène et refaire le tour du meuble. Les dommages persistants doivent rester en place.
