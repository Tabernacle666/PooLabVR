# Profils OASIS

Les profils sont dans `.oasis-local` à la racine du dépôt. Pour une installation autonome, le dossier est à côté de l’exécutable. Le jeu charge les profils au démarrage. Il écrit les changements du cockpit à la fermeture normale, via un remplacement atomique de chaque fichier. Une recompilation conserve le dossier. Supprimer un profil rétablit ses valeurs intégrées au prochain lancement.

## Cockpit

À bord, **Y sur la manette gauche** fait défiler Mixage, Audio, Haptique, Matériaux et Usine. Le stick droit sélectionne une ligne avec haut/bas et change la valeur avec gauche/droite. Sur la ligne Événement, gauche/droite change l’événement. Le clic du stick droit rappelle la valeur d’usine du champ sélectionné. Les réglages de vol restent sur l’écran Systèmes et le stick gauche. Le clic gauche rappelle le réglage sélectionné ou lance le recalage sur la ligne Recalibrer.

**Overwrite Factory Default** demande deux clics du stick droit en quatre secondes. Il prend les valeurs courantes de la navette, du mixage, des effets et des matériaux comme références personnalisées. Ces références sont enregistrées sous les clés `factory.*` à la fermeture. Le clic d’un champ de vol, de mixage, d’effet ou de matériau rappelle ensuite cette référence.

## Fichiers

| Fichier | Paramètres |
| --- | --- |
| `controls.ini` | Association entre action, main et commande physique. Décommenter les lignes voulues dans le modèle créé à la première fermeture ou les reprendre depuis `src/app/input.ini`. Les modifications s’appliquent au prochain lancement. Un paramétrage de boutons invalide rétablit les associations intégrées. |
| `shuttle.ini` | Angles neutres, zone morte, couleur du CRT, poussée, régime et montée des hélices, assise et niveaux du mixage. Les angles sont en degrés, `helices_rpm` est en pas de 50 tr/min, `helices_montee` en dixièmes de seconde. |
| `materials.ini` | Réglages des matériaux par groupe, avec valeurs personnalisées sous `factory.*`. Voir la nomenclature ci-dessous. |
| `audio.ini` | Pour chaque événement, `.file` désigne le nom du WAV, `.gain` le volume en pourcentage (0–200) et `.pitch` la tonalité en pourcentage (25–200). |
| `haptics.ini` | Pour chaque événement, `.mode` accepte `-`, `T` (toc), `S` (secousse en deux impulsions) ou `D` (vibration modulée). `.force` et `.echo_force` sont en pourcentage. `.duree_ms`, `.delai_ms`, `.echo_duree_ms` et `.intervalle_ms` sont en millisecondes. L’intervalle limite la fréquence de répétition. Le CRT affiche les durées arrondies à 10 ms, le fichier accepte les millisecondes entières. En mode S, le délai règle le départ de la deuxième impulsion. En T/D, il décale la première impulsion. |

Placer les WAV dans `.oasis-local/audio/`. Utiliser un WAV PCM mono 16 bits entre 8 et 96 kHz, avec des points de bouclage raccordés pour les moteurs. Le jeu reprend le son généré existant quand un WAV personnalisé manque ou est illisible. Les événements auparavant silencieux peuvent recevoir un WAV.

| Événement | Nom du WAV par défaut |
| --- | --- |
| `capture` | `capture.wav` |
| `surface_enter` | `surface-enter.wav` |
| `surface_slide` | `surface-slide.wav` |
| `surface_exit` | `surface-exit.wav` |
| `shuttle_boot` | `shuttle-boot.wav` |
| `shuttle_ignition` | `shuttle-ignition.wav` |
| `rotor_reverse` | `rotor-reverse.wav` |
| `shuttle_jet` | `shuttle-jet.wav` |
| `shuttle_fan` | `shuttle-fan.wav` |
| `dust_glass` | `dust-glass.wav` |
| `dust_hull` | `dust-hull.wav` |
| `menu_detent` | `menu-detent.wav` |
| `lunar_rifle` | `lunar-rifle.wav` |
| `lunar_cannon` | `lunar-cannon.wav` |
| `lunar_surface` | `lunar-surface.wav` |
| `lunar_membrane` | `lunar-membrane.wav` |
| `lunar_motor` | `lunar-motor.wav` |

Exemples pour régler la force et le son de l’ignition et changer le bouton de page de la navette.

```ini
# haptics.ini
shuttle_ignition.mode=S
shuttle_ignition.force=100
shuttle_ignition.duree_ms=140
shuttle_ignition.delai_ms=220
shuttle_ignition.echo_duree_ms=320

# audio.ini
shuttle_boot.file=mon-demarrage.wav
shuttle_boot.gain=85

# controls.ini
bind.shuttle.options_page.left=primary_click
```


## Matériaux

Le jeu charge les images de `mods/textures/` avant le rendu. Dans la page Matériaux, la première ligne sélectionne le groupe avec gauche/droite. Haut/bas sélectionne ensuite le paramètre. Les réglages du CRT s’appliquent dès la prochaine image rendue. Les nouvelles images et les modifications faites à la main dans le profil sont prises en compte au prochain lancement. Le vol automatique de la navette charge aussi les matériaux locaux.

Nommer chaque image `<groupe>.<canal>.png`. Les extensions `.jpg` et `.jpeg` sont aussi acceptées. Taille maximale par image de 4 096 × 4 096 pixels. Un canal manquant ou illisible reprend l’image ou la couleur intégrée à l’asset.

| Canal | Convention |
| --- | --- |
| `basecolor` | Couleur sRGB. Préserver l’alpha du PNG sur les surfaces translucides ou découpées. |
| `normal` | Normale tangentielle RGB en espace linéaire, orientation +Y. |
| `roughness` | Rugosité linéaire en niveaux de gris, lue sur le canal rouge. Noir lisse, blanc rugueux. Le métal de l’asset reste indépendant. |
| `emissive` | Couleur sRGB émise par la surface. |

| Groupe | Surfaces |
| --- | --- |
| `shuttle.hull` | Blindage, flancs et bordures |
| `shuttle.metal` | Titane, alliage et chrome |
| `shuttle.trim` | Avertisseurs, voyants et marquages |
| `shuttle.seat` | Assise, fibre et joints |
| `shuttle.controls` | Manches de pilotage |
| `shuttle.canopy` | Verrière |
| `shuttle.crt_glass` | Vitres des CRT |
| `oasis.logo` | Structure des lettres |
| `oasis.windows` | Fenêtres et balises |
| `oasis.soil` | Terre de l’île |
| `oasis.stone` | Roche de l’île |
| `oasis.vegetation` | Végétation basse |
| `oasis.river` | Berges et eau |
| `moon.ground` | Régolithe, cratères et sous-face |
| `moon.wall` | Mur et parement |
| `moon.equipment` | Métal, caoutchouc, combinaisons et visières |
| `moon.markings` | Marquages des compagnies |

Exemples de fichiers à déposer.

- `mods/textures/shuttle.hull.basecolor.png`
- `mods/textures/oasis.logo.normal.png`
- `mods/textures/moon.ground.roughness.png`

Dans `.oasis-local/materials.ini`, `red`, `green`, `blue`, `gamma`, `uv`, `normal`, `emission` et `opacity` sont des pourcentages, avec 100 comme valeur neutre. `roughness` et `metallic` sont des écarts signés en points de pourcentage, avec 0 comme valeur neutre. L’opacité agit sur les surfaces déjà translucides ou découpées. Les réglages de matière préservent les catégories de rendu et les collisions. En vol, les vues de relief capturées sont rafraîchies lorsque le moteur dispose d’une vue 3D exploitable du corps modifié.

```ini
shuttle.hull.gamma=110
shuttle.hull.metallic=15
oasis.logo.normal=120
moon.ground.uv=160
moon.ground.roughness=10
shuttle.canopy.opacity=80
```

### Contrat GPU

`GpuInstance` conserve un pas de 64 octets. Le vecteur d’échelle utilise trois flottants et les données visuelles utilisent un entier au décalage 44. Les producteurs CPU nomment séparément le rôle lunaire, le CRT et la poussière de vitesse. Les drapeaux communs au C++ et au HLSL désignent le rôle de l’entier côté GPU. Le format `CutInstance` de la découpe conserve sa disposition.

En mode solaire, les vues OpenXR servent d’abord à choisir le corps à rendre. La vue et les visuels venant du cosmos passent alors au repère de ce corps avant la publication du tampon d’instances, la visibilité, le tri et l’enregistrement des passes. Les modifications de matériaux empruntent une passe de copie qui attend la lecture éventuelle du tampon par la file compute de l’image précédente.
