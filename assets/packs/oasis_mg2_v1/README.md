# OASIS — kit visuel MG2 et production PBR (22 novembre 2026)

Catalogue source actuel : **132 GLB**, **27 profils residents**, **28 recettes auteurs**. Le moteur ne charge pas toutes les recettes du manifeste automatiquement : `src/app/Mg2Catalog.cpp` lit `runtime.json`, et `src/app/BuildingSession.cpp` construit les decorations MG2 residentes autour du joueur. Le moteur de construction est `src/core/building/BuildingPlan.cpp` / `BuildingGeometry.cpp`.

## Source, importation et gros fichiers locaux

- `tools/generate_oasis_mg2_assets.mjs` est le generateur original du catalogue de bureau/foret; son execution reecrit ses anciens GLB. Ne pas le lancer lors d'un travail PBR sans plan de synchronisation.
- `tools/generate_oasis_hospitality.mjs` fournit **13 modeles supplementaires originaux**, chacun avec GLB, metadonnees et provenance. Les sources sont integrees en Git, avec normales, UV, metallic-roughness et pieces non fusionnees. Les textures PBR locales 2K ne font **pas** partie du commit.
- `pbr_lot1.bat` enrichit le cache non versionne `.oasis-local/mg2_runtime` et reconnait maintenant la famille `hospitality`; le staging `tools/mg2_overlay_reconcile.py` preserve les GLB dont les scans CC0 ont ete acquis et ajoute les GLB sources plus recents.
- `run.bat` et le build via CMake reclament `tools/stage_oasis_mg2.cmake`, puis `tools/mg2_hero_repair.py` verifie les GLB avant lancement. Trois anciens GLB lourds (rack de serveur, agrafeuse, cables reseau) sont volontairement distribues avec leurs sources ASCII `hero_sources/*.glb.b64`; la synchronisation du pack SOURCE avec `tools/sync_oasis_mg2_catalog.mjs` peut les signaler absents si ces binaires n'ont pas ete materalises. Ne pas committer l'overlay prive par erreur.

## Refonte visuelle installee

- **RDC** : hall commun en facade, grand vitrage commercial, passage entre vantaux representes ouverts, canapes, comptoir, plantes, chariot a bagages, deux silhouettes de portiques d'ascenseur, cabines telephoniques. L'ascenseur original reste pilote par sa mecanique; les portiques ajoutes n'animent pas encore les portes.
- **Etages** : conciergeries en cadence, depanneur avec refrigerateur vitrine, salon, bureaux enrichis, espaces de maintenance et salles d'eau deja presentes; les bureaux gardent des choix de mobilier par piece et accessoires probabilistes. **400 compositions possibles ne signifient pas 400 meshes uniques**.
- **Haut** : suites avec literie, cuisine avec cuisiniere a gaz, gym avec tapis roulant et poids, spa avec bain a remous, sauna avec poele et banc; bois et materiaux de penthouse sur les deux derniers niveaux.
- **Toits** : plateforme H, eclairage ambre en poursuite temporelle sans revision geometrique, deux reseaux AIR raccroches aux trames et modeles de ventilation MG2 residents a proximite. Les moteurs de ventilation et la simulation de debit d'air ne sont pas actives.
- **Evacuation** : escalier metallique exterieur en zigzag sur les lettres O et I seulement; ouverture depuis le corridor commun a chaque niveau. Le programme de la facade A/S impose des tranches discontinues et ne possede donc pas ici une fausse echelle suspendue. Les protections et normes de batiment ne sont pas certifiees.
- **Micro-detail** : tasse originale LIBERTE55, selectionnable parmi les accessoires des bureaux. Les affiches du dressing OASIS restent reutilisees par catalogue et atlas.

## Ce que le present lot ne garantit pas

Les 13 nouveaux GLB sont des **maillages parametriques** a detail proche, pas des meshes photogrammetriques AAA. L'utilisateur doit executer `pbr_lot1.bat` pour les textures CC0 reelles disponibles; les ressources non acquises conservent leurs materiaux d'auteur. Les portes coulissantes du hall sont statiques, les produits du depanneur ne sont pas tous uniques, les appareils ne sont pas utilisables et la compatibilite physique/dynamique MG2 est encore dependante de l'integration moteur. Aucune compilation Windows DX12, telemetrie GPU 72 Hz, ni validation visuelle Quest n'a ete executee dans la session GitHub de livraison. Un nouveau lot asset peut affiner les formes au lieu de substituer des boites lorsque MG2 sera entierement branche.
