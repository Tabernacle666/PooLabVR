# Profils OASIS

Les profils sont dans `.oasis-local` à la racine du dépôt. Pour une installation autonome, le dossier est à côté de l’exécutable. Le jeu charge les profils au démarrage. Il écrit les changements du cockpit à la fermeture normale, via un remplacement atomique de chaque fichier. Une recompilation conserve le dossier. Supprimer un profil rétablit ses valeurs intégrées au prochain lancement.

## Cockpit

À bord, **Y sur la manette gauche** fait défiler Mixage, Audio, Haptique et Usine. Le stick droit sélectionne une ligne avec haut/bas et change la valeur avec gauche/droite. Sur la ligne Événement, gauche/droite change l’événement. Le clic du stick droit rappelle la valeur d’usine du champ sélectionné. Les réglages de vol restent sur l’écran Systèmes et le stick gauche. Le clic gauche rappelle le réglage sélectionné ou lance le recalage sur la ligne Recalibrer.

**Overwrite Factory Default** demande deux clics du stick droit en quatre secondes. Il prend les valeurs courantes de la navette, du mixage et des effets comme références personnalisées. Ces références sont enregistrées sous les clés `factory.*` à la fermeture. Le clic d’un champ de vol, de mixage ou d’effet rappelle ensuite cette référence.

## Fichiers

| Fichier | Paramètres |
| --- | --- |
| `controls.ini` | Association entre action, main et commande physique. Décommenter les lignes voulues dans le modèle créé à la première fermeture ou les reprendre depuis `src/app/input.ini`. Les modifications s’appliquent au prochain lancement. Un paramétrage de boutons invalide rétablit les associations intégrées. |
| `shuttle.ini` | Angles neutres, zone morte, couleur du CRT, poussée, régime et montée des hélices, assise et niveaux du mixage. Les angles sont en degrés, `helices_rpm` est en pas de 50 tr/min, `helices_montee` en dixièmes de seconde. |
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
