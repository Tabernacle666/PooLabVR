# OASIS MG2 — hero assets intégrés

Commit unique : extincteur, luminaire à deux tubes, porte et bureau, avec remplissage résident des bureaux. Le modèle complet est chargé lors de la création de la scène, avant les variations de distance : l'existant peut choisir automatiquement le niveau de détail disponible. Aucune réduction de mémoire ou de LOD n'est introduite.

| Asset MG2 | Triangles source | Triangles hero | Fichiers source |
|---|---:|---:|---|
| `fire_extinguisher01` | 568 | 10632 | `geometry/fire_extinguisher01.glb` |
| `light_panel01` | 36 | 1764 | `geometry/light_panel01.glb` |
| `door_office_solid01` | 264 | 4012 | `geometry/door_office_solid01.glb` |
| `desk_office01` | 264 | 3740 | `geometry/desk_office01.glb` |

## Contrats

- L'extincteur possède cylindre profilé, base, valve, poignée, flexible, buse, goupille, anneau, câble, manomètre et aiguille géométrique, ainsi qu'une étiquette PNG à instructions intégrée dans la surface courbe. Les sockets `socket_floor` / `socket_wall` sont conservés et `socket_pin` / `socket_gauge` ajoutés.
- Le luminaire contient le caisson, les deux tubes longs, leurs embouts, le ballast noir, la vasque réfléchissante, les attaches, et le diffuseur en polycarbonate transparent. Le système de lumière de la pièce reste responsable de l'éclairement.
- La porte comporte trois charnières, serrure, rosaces, poignées de chaque côté, stiles, panneaux et moulures. Le moteur conserve son hôte de passage et ses données de collision. L'animation articulée de porte reste un chantier distinct.
- Le bureau comporte top épais, chant plaqué, structure métallique, deux tiroirs, poignées, passes-câbles, pieds réglables et goulotte grillagée. Les sockets du poste de travail sont conservés.

## Matériaux photographiques

**Statut factuel :** les quatre GLB source du commit embarquent leur géométrie et des matériaux de référence; le bois photographié Poly Haven n'est pas contenu dans le commit. Le script `pbr_lot1.bat` injecte **wood_table_001**, texture Poly Haven CC0, dans `wood_oiled` et `wood_light` du bureau et de la porte. Source : https://polyhaven.com/a/wood_table_001 . Il met à jour automatiquement les méta-données et les longueurs des GLB, nécessaires au chargement MG2. Les textures photographiques ne doivent pas être remplacées lors de la génération d'une nouvelle géométrie.

## Intégration et qualification

- `Mg2Catalog::AppendWorktop` prépare les cinq modèles hero (quatre assets plus chaise existante).
- `BuildingSession::StreamRoomDecor` place les bureaux/sièges aux hôtes de poste, les portes sur les passages, les luminaires sur leurs hôtes et les extincteurs à proximité des portes. Les pièces sont prêtes dans la zone proche étendue; toutes les pièces de type Office disposent du bureau hero lorsqu'elles sont résidentes.
- Les géométries simplifiées restent des hôtes techniques mais cessent d'être affichées là où les modèles hero ont pris leur place.
- `commit_and_loop.bat mg2` : compiler sur Windows puis vérifier le rendu proche de l'extincteur, des tubes et de leurs couvercles, de la porte, du bureau, et les bureaux voisins.
- Validation effectuée ici : structure glTF 2.0, dimensions, triangles, présence des matériaux/UV et des sources; compilation MSVC et validation VR non exécutées.

Aucune promesse de FPS ni de réduction de mémoire n'est associée à ce commit.


## Réparation binaire et surcouche PBR (2026-11-22)

**Ne pas réintroduire les GLB générés localement dans les fichiers suivis par Git.**

Les quatre premiers GLB hero ont été publiés avec un encodage binaire
incorrect (longueur déclarée différente des octets stockés). Les quatre
sources de mesh exactes sont maintenant sauvegardées comme texte base64
`hero_sources/*.glb.b64`, vérifiables sans transfert binaire. Ce sont les
meshes source, avec leurs textures de repli intégrées; **ce ne sont pas
les matériaux CC0 photographiques téléchargés**.

`mg2_hero_repair.py` décode les quatre modèles dans le répertoire de staging,
répare les métadonnées et empreintes Git localement, puis vérifie les 116
GLB et leurs manifests. Si un GLB est déjà valide, en particulier un GLB
enrichi PBR, il n'est pas reconstruit.

`stage_oasis_mg2.cmake` fait cette opération avec le Python 3.10+ déjà
nécessaire à `pbr_lot1.bat`. **Node.js n'est plus requis pour le lancement
ou les PBR.** Le générateur JS existe uniquement comme source d'authoring
pour l'équipe de développement.

Les cartes PBR téléchargées restent dans `.pbr_cache`, et les GLB
enrichis restent dans `.oasis-local/mg2_runtime`, dossier déjà ignoré par
Git. Le staging normal consomme ce pack enrichi local sans modifier
les modèles source du dépôt.
