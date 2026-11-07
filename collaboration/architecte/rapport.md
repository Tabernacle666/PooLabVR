# Décision architecturale — Caca AAA / matiere-pbd-001
## Tentative 3 — refus, correction imposée
**Relais figé** : `8f6d33964f75e63baccdadb7d11371a4d42b4deb`  
**Code diagnostiqué** : `7a16b64087624530b666b8e7df1120be66e88f51`  
**QA statique conforme** : `8447f3771991f707c0603d1624820a52d46fb203`  
**Diagnostic** : `diagnostics/runs/20261101_230009_617_38740/manifest.json`  
**Décision** : `corriger`, tentative 4, même billet, sans rollback.

### Constats vérifiés
- Les six JPG ont été examinés. Le plateau est occupé par le support du ventilateur et le vase; aucun tas de matière fécale identifiable n'apparaît. Les vues intitulées « matière » cadrent le support du ventilateur; la vue « dessous » montre la face inférieure d'une table encore à l'endroit. Ni adhérence, ni détachement, ni fissure ne sont démontrés.
- Avant PBD, dernier diagnostic complet disponible `20261031_172601_143_5992`: physique **6,327 ms/frame**, **3,725 ms/sous-pas**, CPU total **7,908 ms/frame**, file GPU rendu la plus chargée **4,430 ms**, VRAM **1 264,4 Mo**.
- Après PBD, diagnostic complet `20261101_230009_617_38740`: physique **50,517 ms/frame**, **25,266 ms/sous-pas**, CPU total **54,205 ms/frame**, file GPU rendu la plus chargée **4,467 ms**, VRAM **3 155,9 Mo**. **611 frames de simulation en retard; 27,15 s abandonnées**.
- Régression concentrée dans la physique, pas dans le rendu. **1 225 fetchResults bloquants sur 1 225 sous-pas**, total **29 953,426 ms** d'attente. La boucle à deux sous-pas fait deux attentes et chaque sous-pas PBD coûte ~25 ms. Cette mesure localise le coût dans le travail PhysX synchronisé; elle ne prouve pas à elle seule que supprimer fetchResults est légal ou optimal.
- `HomePhysics.cpp` crée **262 144 particules actives** par défaut et place le volume au centre de la table, dans la zone du support du ventilateur. `MatterPbdCuda.cu` publie des instances de sphères de **2 × (.245/128) ≈ 3,83 mm** sans reconstruction de surface continue. Ces choix sont compatibles avec un matériau visuellement absent ou occulté dans les captures. Leur contribution exacte doit être mesurée, non devinée.
- La compilation et le lancement réussis et la QA conforme sont confirmés, mais ne prouvent aucun des comportements matériels demandés.

### Ordre de correction au programmeur
1. **Rendre vérifiable la matière PhysX réelle** : dégager un emplacement visible sur la table, conserver les collisions, matérialiser un volume cohérent à partir des positions GPU PBD (pas de primitive qui ment sur l'état physique), assurer les vues près/de côté, et journaliser quantité active/visible, AABB et contact avec la table.
2. **Éliminer la régression structurelle sans tricher** : instrumenter séparément solveur PhysX, self-collision/voisinage PBD, itérations, phases, attente fetchResults, synchronisations CUDA, publications GPU et temps CPU des deux sous-pas. Comparer la scène identique avec/sans matière et les quatre paliers de particules prescrits. Réduire les blocages inutiles avec preuve de correction, sans supprimer arbitrairement la physique ou les particules. La cible 72 Hz reste une exigence non démontrée; si elle ne peut pas être atteinte, publier les mesures et le diagnostic au lieu de déclarer une victoire.
3. **Créer des essais physiques véritables** : table retournée par action/couple PhysX, partie qui adhère et partie qui tombe, impact, séchage et fissure par contact mécanique, transport thermique/hydrique et effet sur les mouches. Captures avant/après de chaque phénomène; ne pas confondre déplacement de caméra et démonstration d'interaction.
4. **Conserver le reste du labo** et ne pas déplacer/masquer les phénomènes existants. Le QA vérifie le code statiquement avant le prochain diagnostic utilisateur, puis l'architecte arbitre seul.

### Conditions d'acceptation
La prochaine livraison n'est recevable que si le rendu perceptible, les phénomènes physiques réels, leur preuve instrumentée et les performances sur la RTX 5060 Ti sont présents ensemble. Le programmeur ne peut ni relâcher le masterticket, ni décaler discrètement la validation sur un comportement factice. Pas de limite de tentatives, pas de recul autorisé au programmeur.

## Retour demande au moteur precedant Caca AAA

Decision du proprietaire : restaurer le moteur du commit `8272389e3c0b6b4cf4f29783d9f7f5217b818e99` et interrompre le cycle `caca-aaa-001`. Code restaure par le commit `9d9413b8d4d8232742167a475d1a62973d59925d` : les huit fichiers modifies du moteur et les deux nouveaux fichiers MatterPbdCuda sont ramenes exactement a la base; cameras de diagnostic revenues aux deux vues initiales. Les outils de relais, les diagnostics et l'historique restent dans le depot. Aucun nouvel essai ne doit etre automatiquement lance. Compilation et rendu Windows apres retour non verifies.
