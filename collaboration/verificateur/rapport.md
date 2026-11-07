# Vérification QA — matiere-pbd-001 · caca-aaa-002 · tentative 2

- Code figé : `0da8d4cdfd0928af2fde0b1e92308a0c8ae7ff99`
- Relais de livraison : `d06d1d82a25517cda0da98f19526c2b82a44999a`
- Base de correction : `7be5552797c4531900f270078919e0e41c3a4846`
- Autorisation du programmeur : `120b90855db8c9b638751289e4748844718f20f1`
- Examen : inspection statique du diff de correction et des cinq fichiers de code concernés, des déclarations CUDA/PhysX, des appels d'intégration, des consommateurs HLSL et des exigences du masterticket. Aucun lancement, aucune compilation ni capture examinée.
- **Résultat : `conforme` — destination `arch` — 0 constat.**

## Vérification des corrections demandées

1. **QA-MATIERE-001 — liaison et rupture de la croûte** : `MatterPbdCuda.cu` maintient sur GPU les masques de liaisons et de fractures, forme des ressorts entre particules desséchées en contact, rompt les liaisons selon leur allongement et recalcule l'exposition et la vapeur des faces ouvertes. `PhysicsRuntime.cpp` alloue, initialise, transmet et libère ces états, puis signale au tampon PBD la mise à jour des vitesses ; `HomePhysics.cpp` crée un bâton capsule PhysX dynamique manipulable. L'inspection ne relève pas d'absence statique du mécanisme précédemment demandé.
2. **QA-MATIERE-002 — rendu de la sécheresse** : `MatterPack` écrit la sécheresse issue de l'humidité GPU dans `GpuInstance.scale.w` et identifie les particules par le préfixe `0x72`. `scene.hlsl` transmet cette donnée via les sorties du mesh shader, puis `PixelMain` applique aux seules particules de matière une variation de couleur et de rugosité. Le consommateur de l'état de sécheresse existe effectivement dans le rendu.

## Périmètre et limites

Fichiers de correction examinés : `labs/labovr/HomePhysics.cpp`, `shaders/scene.hlsl`, `src/core/MatterPbdCuda.cu`, `src/core/MatterPbdCuda.h`, `src/core/PhysicsRuntime.cpp`. Interfaces et configuration de compilation consultées pour vérifier leurs dépendances. Aucun changement au code de production par QA.

**Aucune divergence constatée.**

Cette conformité est strictement statique : elle ne prouve ni compilation réelle, ni stabilité GPU/PhysX, ni fissuration visible, ni performance à 72 Hz. Le propriétaire doit effectuer le diagnostic local sur le SHA livré ; l'architecte ne décide qu'après vérification de ses preuves, conformément à `gabarit.md`.
