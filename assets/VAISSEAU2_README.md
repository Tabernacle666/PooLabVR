# Vaisseau2 – Nissan 300ZX Z31 (1983)

Raccordement sur le circuit existant de `vaisseau1`, sans remplacer les commandes,
la physique GPU ni les organes et sans modifier le vaisseau par défaut.

## Asset source

Source: `1983+Nissan+300ZX+Z31(1).glb` fourni par l'utilisateur.
L'asset converti est `assets/vaisseau2_z31.glb` (4,425 m de long,
1,913 m de large mesuré dans le maillage, 1,268 m de haut).
Le GLB est livré à part, non stocké dans ce premier commit source.
Le placer exactement sous `assets/vaisseau2_z31.glb`, reconfigurer CMake,
puis compiler : le post-build le copie à côté de `OASIS.exe`.

## Sélection

Ajouter `vaisseau=2` dans le profil local `.oasis-local/shuttle.ini`
(à côté du dossier d'audio/config du profil). `vaisseau=1` garde
l'engin original. Si le GLB n'est pas installé, le rendu demeure sur
le vaisseau1 sans exception de chargement.

## Architecture

- `ShuttlePilot` reste le pilote unique avec le frein, les deux manches,
  les axes, les ratios commandés, les réglages et le modèle de forces inchangés.
- `ShuttleVisuals` échange les trois modules de coque, pas les organes animés.
- Les matériaux `Z31.Headlamp` et `Z31.Taillamp` deviennent émissifs dès
  l'allumage de l'avionique; à l'arrêt ils conservent leur matière non émissive.
- La verrière Z31 contribue aux interactions de poussière/impact.
- La première conversion conserve les deux commandes, les panneaux CRT et
  les propulseurs de vaisseau1 : la refonte Nissan du cluster reste à faire.

## Suites nécessaires

Ajuster les ancrages hélices/réacteurs aux passages de roues, le poste VR et
la lisibilité des instruments dans le casque; valider par un lancement réel.
Le GLB converti doit être installé avant toute validation visuelle.
