# CooLabVR — diagnostic automatique (2 angles, casque sur la table)

Depuis Windows, casque allumé, session OpenXR active :

```bat
diagnostic_cycle.bat
```

Le programme charge le laboratoire **Home** avec les conditions de comparaison :
- intérieur sombre (« nuit » de la scène, sans horloge jour/nuit dans cette version) et lampe allumée ;
- terminal **ambre** (thème 1), face extérieure ;
- fog activé (densité au moins 0,065) ;
- exactement **500 mouches**, même si le profil utilisateur en avait un nombre différent.

Ces valeurs sont appliquées **seulement pendant le diagnostic**, après lecture du profil, sans sauvegarder ni remplacer les réglages normaux.

## Séquence

1. Si `build/bin/Release/LaboVR.exe` manque ou si un fichier source/CMake est plus récent, `Build.ps1` lance une compilation incrémentale Release. Sinon la compilation est **ignorée**.
2. Le jeu démarre en mode `--diagnostic-sweep`. Le casque reste sur la table; le suivi OpenXR doit être actif.
3. La caméra virtuelle #1 rend l'intérieur (ampoule, ventilateur, table, brouillard) pendant **au moins trois secondes**, puis demande une capture de l'œil gauche incluant l'interface.
4. Après confirmation de la première capture, la caméra virtuelle #2 vise le panneau ambre depuis l'extérieur, rend pendant **au moins trois secondes**, puis capture.
5. Le jeu quitte de lui-même après confirmation des **deux** images ou retourne une erreur en cas d'échec/expiration. Le lanceur convertit les PNG intermédiaires issus du service OpenXR/WIC en **JPEG qualité 92**, puis supprime les PNG intermédiaires si la conversion a réussi.

Les points de vue se règlent **sans recompiler** dans `tools/diagnostic_cameras.csv`. Ils correspondent à la pièce actuelle, construite autour de Y=0..3,8 m : l'ancien CSV Y≈12 visait trop haut.

Résultats locaux dans `diagnostics/runs/<date>_<pid>/` :

```text
manifest.json
cycle.txt
LaboVR_runtime.log
LaboVR_build.log         (seulement si compilation)
capture_index.txt
diagnostic_cameras.csv
screenshots/
    01_lamp_fan_interior.jpg
    02_original_option_wall.jpg
```

Les journaux existants `vr.frame` fournissent le détail par trame; le résumé `sweep.summary` contient nombre de frames, frames suivies, frames soumises, coût CPU moyen et maximal, durée, erreurs et avertissements GPU. Le manifeste distingue `complete: true` des captures manquantes ou d'un arrêt forcé.

Le lanceur **ne fait aucun fetch, pull, commit, push ou changement de branche**. Après avoir inspecté les résultats, utiliser GitHub Desktop pour committer et pousser `diagnostics/runs/`, puis partager le résultat. Si le dépôt est public, les captures et les logs publiés seront publics.

Options : `diagnostic_cycle.bat -Seconds 90 -Parallel 8` ou `-GpuValidation`. `Seconds` (15..600) est la durée maximale de la séquence de jeu, **pas** le temps affiché par caméra (3 secondes fixes).

## Frontière XR et limites

Le diagnostic ne change que les matrices de vue/projection utilisées par le rendu (deux yeux séparés de 64 mm); **les poses de tracking OpenXR, les manettes, la simulation PhysX et les poses soumises au compositeur restent inchangées**. Les captures sont obtenues par le mécanisme hôte GPU readback déjà existant. Les vues sont destinées à la vérification visuelle, pas à un port du casque pendant la procédure.

Un casque en veille, un runtime qui refuse le rendu, une défaillance GPU ou une compilation incomplète peuvent empêcher les captures. Dans ce cas, la commande retourne un code d'échec et conserve les preuves disponibles.

**État de validation :** changements publiés au niveau source seulement. La compilation Windows/MSVC, le rendu OpenXR et le cadrage exact par rapport aux deux images de référence doivent être confirmés sur le PC du créateur; les angles CSV pourront ensuite être ajustés sans recompilation.


## Profiling leger pour optimisation future

Le balayage utilise un log compact (pas de journalisation des frames une par une).
A la fin, `sweep.summary` donne les frames demandees/suivies/soumises, le temps CPU
actif moyen et maximal et les erreurs GPU. `sweep.cpu` donne uniquement les
moyennes CPU des etapes Home, PhysX, preparation graphique, acquisition XR et
soumission graphique, ainsi que les frames dont le CPU depasse le budget
d'affichage et la periode XR moyenne. `sweep.gpu` releve une seule fois
l'adaptateur, la resolution de rendu et la VRAM utilisee/disponible.

**Important :** `submit_avg_ms` mesure le temps CPU dans la fonction de
rendu et non le temps d'execution GPU. Aucun query GPU par frame n'est ajoute.
L'option `-GpuValidation` reste facultative et peut influencer les performances.


## Profil physique compact

Au moment de la fermeture, trois lignes `physics.profile.*` sont écrites une
seule fois et recopiées dans `manifest.json > performance_physics` :

- `physics.profile.cpu` : pas fixes, saturation des sous-pas, temps CPU moyen et
  maximal par pas, temps CPU par frame et temps d'acquisition du verrou CUDA.
- `physics.profile.wait` : appels `fetchResults` bloquants, sommes des temps
  d'attente PhysX, des commandes et des changements de topologie.
- `physics.profile.submit` : temps CPU par pas pour la soumission PhysX,
  la population CUDA et les lectures d'état après la simulation.

Ce ne sont pas des durées d'exécution GPU. Les valeurs sont agrégées en mémoire
et n'ajoutent aucune requête GPU ni synchronisation. La télémétrie capture le
coût CPU des opérations existantes, sans enregistrer les frames une par une.


## Politique PhysX anti-emballement

Le laboratoire utilise désormais 120 Hz avec au maximum deux sous-pas fixes
par frame XR (au lieu de 240 Hz / huit sous-pas). Le scheduler élimine le
temps de simulation excédentaire plutôt que de le reporter d'une frame à
l'autre. Il préserve uniquement la fraction de pas nécessaire pour aligner
la fréquence VR et la fréquence physique. `dropped_frames` et `dropped_s`
quantifient les surcharges et pertes de temps simulé.

Cela ne supprime pas l'attente `fetchResults` exigée pour réutiliser les
résultats PhysX : cela borne son nombre d'occurrences par frame.
Le passage à 120 Hz peut changer légèrement la stabilité et les mouvements
des joints/chaînes. Comparer les captures et vérifier le comportement de la
lampe, du ventilateur et des mouches au prochain diagnostic.


## Burst de profilage GPU/OpenXR automatique

Après les deux photos de référence, le diagnostic exécute 90 frames supplémentaires
avec les **timestamps D3D12 déjà implémentés**. Leurs résultats sont collectés
après l'arrêt des commandes GPU; aucune lecture de timestamp pendant la frame.

- `sweep.gpu_timing`: temps GPU du premier au dernier pass, moyennes, p95 et pic.
  Les files Copy/Compute/Direct peuvent se chevaucher; ne pas sommer leurs durées.
- `sweep.gpu_pass`: huit passes classées selon le coût GPU cumulé pendant le burst.
- `sweep.gpu_queue`: moyenne du temps actif et de l'étendue des passes pour chaque file.
- `sweep.xr`: durées de `xrWaitFrame`, `xrBeginFrame`, synchronisation des
  actions, localisation, acquisition/attente couleur et profondeur, `xrEndFrame`.
  **xrWaitFrame représente la cadence imposée par le runtime** et n'est pas du
  travail GPU. Les attentes swapchain peuvent, elles, exposer une contention.

Le profilage est séparé du relevé CPU habituel pour ne pas fausser les
comparaisons. Le manifeste contient les champs `performance_xr`,
`performance_gpu_timing`, `performance_gpu_passes` et
`performance_gpu_queues`. Cet outil ne mesure pas encore le débit hors
OpenXR; celui-ci requiert un banc de rendu à cible hors écran distinct.


### Comparaison A/B temporaire du brouillard

Après les deux captures de référence, un burst de 90 frames avec brouillard
est suivi de 90 frames avec son volume GPU temporairement désactivé. Le
brouillard est automatiquement restauré avant la fermeture; aucun profil
utilisateur n'est modifié. Les lignes `sweep.gpu_timing`,
`sweep.gpu_pass`, `sweep.gpu_queue` et `sweep.gpu_experiment`
permettent la comparaison des phases `baseline` et `fog_off`.

`avg_busiest_queue_ms` est la durée maximale mesurée sur une file de commandes
individuelle par frame, **pas** l'étendue GPU totale. Les horloges Copy,
Compute et Direct ne sont jamais soustraites entre elles. Un résultat de ce
test ne justifie pas de désactiver définitivement le brouillard : il renseigne
sur les étapes à optimiser sans perdre le phénomène visuel.


### Optimisation CPU des transferts de forces PhysX

Dans un sous-pas sans commande explicite de force ou de couple, les tableaux
`forces` et `torques` sont remis à zéro par deux `cudaMemsetAsync` dans le
flux CUDA déjà utilisé. Cela évite de remplir deux tableaux CPU épinglés et
de les recopier sur le bus PCIe. Les commandes cinématiques n'exigent pas
ce transfert. Dès qu'une force, impulsion ou un couple explicite existe,
le chemin d'accumulation CPU + deux transferts H2D est conservé. Les noyaux
de population, d'écoulement et d'aérodynamique fonctionnent ensuite sans
changement, dans le même ordre GPU.

La télémétrie `physics.profile.transfer` indique `device_zero_steps`,
`host_upload_steps` et `force_init_submit_avg_ms`, pour vérifier que le
chemin est effectivement emprunté et mesurer le temps de soumission CPU.
Les quatre lignes `physics.profile.*` doivent figurer au manifeste.
Les synchronisations PhysX `fetchResults` restent inchangées; un éventuel
gain global doit être confirmé par un autre diagnostic Windows.


### PhysX GPU chronométré sans synchronisation ajoutée

La ligne `physics.profile.gpu` regroupe les mesures d'événements CUDA déjà
placés autour des sous-pas et des frames. `step_samples` / `step_untimed`
indiquent le taux de couverture; `step_avg_span_ms` et `frame_avg_span_ms`
indiquent les temps d'attente observables sur le flux CUDA, en incluant
ses dépendances et creux de soumission. Ces durées ne mesurent pas
exclusivement les noyaux PhysX. Les échantillons sont relevés par
`cudaEventQuery` pendant l'exécution; les derniers sont récoltés après
le retrait normal de la simulation au moment de fermer la scène.
Aucun nouveau `cudaEventSynchronize` n'est introduit.
Le manifeste doit contenir cinq lignes `physics.profile.*`.


### Suppression des réécritures identiques des moteurs D6

Les cibles et vitesses des drives PhysX sont persistantes. Si la cible
et les vitesses sont exactement identiques à celles déjà soumises, la boucle
fixe ne rappelle plus `setDrivePosition` / `setDriveVelocity`. Une cible
animée (`frequencyHz > 0`) est toujours mise à jour à chaque sous-pas;
une modification de la configuration du joint invalide le cache. La nouvelle
ligne `physics.profile.motor` donne `writes` et `unchanged_skips`.
Les six lignes `physics.profile.*` sont requises dans le manifeste.
Cette optimisation évite des modifications de contraintes inutiles, mais
le gain GPU/CPU doit être évalué sur le prochain diagnostic Windows.


### Essai de coût des itérations du solveur PhysX GPU

À l'origine chaque corps dynamique utilise `12` itérations de position et
`4` de vitesse. Un essai conservateur passe à `10` et `3`, sans
modifier le pas de 120 Hz, les contraintes D6, les collisions ni le rendu.
Ce n'est **pas** un gain validé : comparer `physics.profile.gpu`,
`physics.profile.wait` et `sweep.cpu` aux diagnostics précédents.
`physics.profile.solver` enregistre le réglage effectif, le nombre de
corps dynamiques, les joints et l'état de mise en veille. Surveiller le
comportement de la suspension et du ventilateur ; rétablir 12/4 si
l'amélioration est absente ou si la stabilité physique diminue.
Sept lignes `physics.profile.*` sont requises dans le manifeste.


### Detail des attentes PhysX par sous-pas

`physics.profile.wait` inclut les temps d'appel CPU de `fetchResults` separes
entre le premier sous-pas de chaque frame (`first_*`) et les sous-pas suivants
(`later_*`). Les compteurs `fetch_lt_0_5ms` a `fetch_ge_4ms` forment une
distribution de tous les appels. `check_avg_ms` mesure l'appel CPU a
`checkResults(false)` ; une reponse non prete indique uniquement que le
resultat n'etait pas pret a cet instant. Le temps dans `fetchResults` peut
inclure du travail GPU et de la synchronisation et ne mesure pas un noyau
PhysX isole. Les sous-pas suivants sont regroupes, meme si la configuration
actuelle en autorise au plus deux par frame. Ces mesures sont agregees sans
synchronisation GPU supplementaire et n'ajoutent pas de ligne de profil.


## Verification de la population quand Home est masque

Après les deux captures et le profil GPU, le diagnostic masque temporairement
la couche Home. Il compare deux lectures GPU de la position du même insecte,
prises après le masquage et séparées d'au moins huit frames. Un avancement de
la scène seule ne suffit pas à réussir. La couche est restaurée à la fin.

La ligne `sweep.population_continuity` est recopiée dans `manifest.json` :
`result=pass` signifie qu'un déplacement a été observé ; `result=fail` indique
que des individus comparables sont restés immobiles ; `result=inconclusive`
signifie qu'aucun individu comparable n'a pu être obtenu dans le délai. Les
cas fail et inconclusive empêchent de qualifier le cycle de complet. Ce test
ne prouve ni la sauvegarde après fermeture ni l'absence de toute interaction
fantôme avec le joueur. La phase masquée est exclue des moyennes CPU de base.

Attention : les lignes `physics.profile.*` sont agrégées pour toute la vie
de la scène, y compris la phase de continuité masquée. Elles ne doivent pas
être comparées directement à celles d'un cycle antérieur qui ne comportait
pas cette phase ; les moyennes `sweep.cpu` et le burst GPU de référence
restent séparés.
