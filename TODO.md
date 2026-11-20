# OASIS · TODO de jour de pluie

À reprendre quand il reste du temps entre deux chantiers. Cocher une tâche après sa validation en jeu.

## Lune

Retours VR et captures dans `e5df1d523fc4d63ab2d8f2aedfe697d1b5050709`. Le journal de la loop indique `83f6921` et le journal de jeu libre indique `fe3685d`. Pour comparer les essais, relever la révision effectivement lancée. [Loop lunaire](diagnostics/lune/OASIS.log) · [Jeu libre](diagnostics/OASIS.log)

- [ ] **Astres invisibles depuis la Lune.** Depuis l’intérieur, orienter le regard vers OASIS ou un autre astre. Le corps présent dans le champ doit rester visible dans les deux yeux. La sélection force actuellement `Hidden` pour les autres corps quand le joueur est dans la Lune. [GraphicsSphereLod.cpp](src/core/GraphicsSphereLod.cpp)
- [ ] **OASIS réduite à une bulle après la sortie de la Lune.** Regarder OASIS à portée locale avant d’entrer, puis se replacer à une distance et à un angle comparables après la sortie. Le logo et le terrain doivent retrouver leur rendu habituel. Vérifier la sélection et le relais de représentation au franchissement. [GraphicsSphereLod.cpp](src/core/GraphicsSphereLod.cpp)
- [ ] **Rafales des fantassins inaudibles.** En jeu libre, s’approcher d’un groupe qui tire et tourner la tête pour vérifier que ses rafales se localisent. Depuis le cosmos, vérifier que ces mêmes rafales contribuent au mix étouffé qui baisse avec la distance. Suivre un événement de tir jusqu’à sa sortie audio. [LunarBattle.cpp](src/core/LunarBattle.cpp) · [LunarBattleAudio.cpp](src/app/LunarBattleAudio.cpp)
- [ ] **Mur central indestructible.** Vérifier les impacts du joueur et ceux de l’escarmouche sur le mur. Un impact destructeur doit ouvrir une brèche visible que les tirs suivants peuvent traverser. Les tirs de l’escarmouche rencontrent actuellement une paroi définie dans `TraceShot`. [LunarBattle.cpp](src/core/LunarBattle.cpp) · [LunarBattlefield.cpp](src/core/LunarBattlefield.cpp)
- [ ] **Bombardement de la Lune sans effet apparent.** Depuis le cosmos, viser le terrain lunaire. Un projectile qui atteint le sol doit laisser des dégâts visibles après l’entrée dans la Lune. Comparer avec un tir depuis l’intérieur. Vérifier l’exclusion des objets lunaires du chemin de découpe, puis conserver l’arrêt et l’ondulation des projectiles lunaires qui atteignent le dôme depuis l’intérieur. [GraphicsCut.cpp](src/core/GraphicsCut.cpp) · [LunarBattlefield.cpp](src/core/LunarBattlefield.cpp)
