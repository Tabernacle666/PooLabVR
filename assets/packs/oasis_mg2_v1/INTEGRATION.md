# Mise en jeu des assets MG2

Le manifeste [runtime.json](runtime.json) relie les assets du pack aux surfaces et aux cellules que le moteur prépare. Une modification de ce fichier ou d’une recette qu’il référence suffit pour intégrer un objet à une cible disponible. Regroupez les objets, leurs métadonnées et leurs placements dans le même lot de contenu.

| Cible | Repère et comportement |
| --- | --- |
| `office_desktop` | Origine au centre du plateau existant. La liaison actuelle charge les placements `socket_desktop` de la recette `office_single01.json`. Les périphériques informatiques sont placés en premier. |
| `meeting_table` | Origine au centre de la table de réunion existante. Le moteur peut déplacer un objet sur le plateau pour le faire tenir à côté des autres. |
| `forest` | Une entrée par rangée de cellules. La recette fournit les arbres à position fixe et la taille de cellule. Les entrées `additional` ajoutent des placements de sous-bois par cellule. Le moteur projette le collet sur le sol de l’îlot et garde les fondations, le cours d’eau et la membrane dégagés. |

## Livrer un lot

1. Déposez le GLB dans `geometry/` et son fichier de métadonnées dans `meta/`. Référencez-les sous la même `asset_key` dans `pack.json`. Utilisez les mètres, Y vers le haut et le repère glTF. Placez le pivot sur le plan d’appui ou au collet.
2. Ajoutez une entrée au tableau `placements` de la cible, à la recette reliée à son socket ou au tableau `additional` d’une rangée de forêt. Pour un plateau, `offset_m` est relatif à son centre. Pour la forêt, `at_m` est relatif au centre de la cellule et `jitter_m` est une variation horizontale optionnelle en mètres.
3. Depuis la racine du dépôt, utilisez `node tools/sync_oasis_mg2_catalog.mjs` pour contrôler les métadonnées des GLB. Si un GLB a changé, utilisez `--write --revision=N` avec la révision du lot. Relisez les modifications du manifeste. Puis lancez `commit_and_loop.bat "mg2"` et vérifiez le placement. Le lancement normal utilise également le pack installé.

Exemple d’entrée dans les `placements` de `meeting_table`.

```json
{"asset": "oasis.props.notebook01", "offset_m": [-0.41, 0, 0.18]}
```

`mg2_liaisons` inventorie les placements déclarés. `pieces_residentes` compte les intérieurs préparés autour du joueur et `postes_residents` inclut les bureaux et les tables de réunion équipés. `claviers_residents` permet de contrôler le poste informatique. `mg2_foret` compte les arbres, le sous-bois et les instances à part.

Les cibles décrites ici accueillent du décor statique. Une porte articulée, un meuble qui bloque la circulation, un appareil de plomberie ou un nouveau point d’ancrage demandent un raccord moteur. Les recettes complètes peuvent rester dans le catalogue jusqu’à la disponibilité de leur cible.
