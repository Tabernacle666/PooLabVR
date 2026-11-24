# Étape 6 — intégration et billet post-compilation

Le billet unique `logs/OASIS-materials-post-compile.md` est généré après une compilation Windows réussie. Il inspecte les en-têtes JSON des GLB sources et stagés, incluant les ressources dormantes, sans décoder les textures ni exécuter le moteur. Il recense les primitives non classifiées, les références invalides, les fichiers absents et les dépendances externes manquantes. Après une session, les anomalies réelles du journal sont consolidées dans le même billet (si la révision correspond). Le billet n'est pas une condition de réussite de la compilation.

Les corrections matérielles restantes devront être faites dans les sources ou dans les déclarations explicites. La disponibilité d'un asset ne détermine pas la disponibilité générale du programme. Les erreurs fondamentales d'allocation ou du GPU ne sont pas des défauts de décoration.

État avant validation locale : 160 définitions matérielles, 611 références de slots/nœuds résolues dans le registre, 143 fichiers GLB recensés dans le dépôt. Les données binaires non accessibles par l'interface Git ne sont pas présentées comme vérifiées ; le billet local couvre cette vérification. La navette d'origine est conservée, son CRT est déclaré, et ses modules valides sont importés indépendamment des modules inconnus ou manquants. Aucun changement des commandes ni des étapes 4, 5, 8.

La compilation et l'essai ingame restent à constater sur la machine cible.
