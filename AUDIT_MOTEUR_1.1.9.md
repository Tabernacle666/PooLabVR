# Audit initial du moteur CooLabVR — 1.1.9.fix2.captureXY

Référence examinée : main à `6873362d5b600cbccd7beb4ffa6e255c5e31e96a`.
Cet audit porte sur les chemins indiqués ; il ne certifie pas l'ensemble du moteur,
ne remplace pas une validation VR et ne prétend pas que le moteur passe déjà à
l'échelle d'un univers. Les mesures se rapportent au diagnostic du même commit.

## Boucle et contrat réel

- `labs/labovr/main.cpp` : OpenXR commence la frame ; le monde et Home
  émettent leurs commandes ; `PhysicsRuntime::Advance` avance chaque scène ;
  la préparation et le rendu suivent. La physique actuelle est sur le fil de
  rendu et termine ses sous-pas avant que ce fil prépare les commandes graphiques.
- `HomePhysics.cpp` crée une scène PhysX à 120 Hz, deux sous-pas maximum. En
  surcharge, `PhysicsRuntime.cpp` élimine des pas entiers au-delà de ce budget.
  Ce choix borne le rattrapage mais ne préserve pas tout le temps simulé. Le
  dernier diagnostic signale des pertes ; leur cause et leur emplacement dans
  la session ne sont pas établis.
- Chaque sous-pas dynamique prépare les forces CUDA, attend leur événement,
  lance `simulate`, puis demande les résultats via `fetchResults`. La pose,
  la vitesse et la vitesse angulaire sont lues en lot par Direct GPU API ;
  les publications de rendu utilisent des buffers partagés et des fences.
- Un appel `fetchResults` bloquant n'est pas en soi un défaut de PhysX : la
  simulation est asynchrone et la prochaine étape dépend du résultat. Il faut
  examiner où placer du travail indépendant sans modifier illégalement la
  scène pendant la simulation, puis préserver les contraintes entre sous-pas.
- Le dernier diagnostic : 27 corps dynamiques, 27 joints D6, 500 mouches.
  La scène est petite ; la documentation NVIDIA indique que le GPU convient
  particulièrement aux lots importants. Cela ne démontre pas que le CPU serait
  meilleur pour cette scène, car il faudrait aussi intégrer les échanges avec
  les mouches et les ressources de rendu GPU.

## PhysX 5.9 / CUDA SM120

`CMakeLists.txt` épingle PhysX 5.9 et CUDA 12.8 minimum ; le correctif CMake
cible SM120. Le runtime vérifie l'identité LUID de l'adaptateur D3D12/CUDA et
refuse un GPU qui n'est pas SM120. C'est une cible explicite, pas une preuve
que tous les kernels exploitent au mieux l'architecture Blackwell.

Le mode PhysX choisi impose GPU dynamics, broad phase GPU, Direct GPU API et
`eDISABLE_SLEEPING`. Depuis PhysX 5.7, désactiver le sommeil est une exigence
de Direct GPU API ; ne pas supprimer ce drapeau comme optimisation isolée.
D6 est un chemin pris en charge sur GPU. Pour faire évoluer des milliers
d'objets, il faut une politique d'activité au-dessus de l'API ou une autre
répartition des scènes, en garantissant la conservation et le réveil de l'état.

Les corps cinématiques, contraintes, modifications de topologie, contacts et
lectures CUDA ont des règles de synchronisation différentes. Toute refonte
doit définir leur ordre, la durée de vie des buffers, les événements CUDA et
les fences D3D12 avant de déplacer un appel. Ne pas substituer une interpolation
visuelle à un état physique ou retirer un rendez-vous sans preuve.

## Rendu et limites de passage à l'échelle

- `Graphics.cpp` vérifie D3D12 feature level 12_2, Shader Model 6.6, mesh
  shaders, view instancing, DXR 1.1 et enhanced barriers. Trois files Direct,
  Compute et Copy, un frame graph, culling GPU et `ExecuteIndirect` existent.
- `shaders/compute.hlsl` sélectionne les **instances** par frustum et Hi-Z ;
  `scene.hlsl` traite leurs meshlets. Les meshlets ne constituent pas, à eux
  seuls, une hiérarchie de LOD et de streaming comparable à Nanite. Les
  maillages de scène sont envoyés dans des buffers au chargement.
- `GpuMemory.cpp` possède une gestion de résidence/éviction des ressources
  D3D12. Elle ne démontre pas à elle seule un streaming spatial de cellules,
  une hiérarchie de géométrie ou une sauvegarde du monde.
- Le GPU utilise le format de shader DXIL SM 6.6 ; « SM120 » est ici une
  architecture CUDA et ne doit pas être confondu avec Shader Model 6.6.
- `ConsoleSpec.h` fixe notamment 8 192 objets de scène et 4 096 meshes.
  La population accepte au plus 128 colliders par scène dans son accélérateur
  actuel ; l'augmenter exige de modifier la structure spatiale et le contrat
  de collision, pas seulement la constante. La grille couvre 16 m par axe ;
  hors grille, elle retombe sur une recherche de tous les colliders pris en
  charge, pas sur un partitionnement d'univers.

## Monde et causalité

Le registre `World.h` gère identités et composants en mémoire. `HomeProfile.cpp`
sauvegarde des poses d'édition, joints et paramètres dans un profil versionné ;
`ProfileStorage.cpp` écrit ce profil de façon atomique. Cela ne constitue pas
encore une sauvegarde permanente des états vivants, des corps en mouvement,
de l'eau, de la chaleur ou des modifications arbitraires d'un grand monde.

Dans la révision examinée, masquer Home coupe `populationInput.enabled` et
`StepPopulation` refuse en plus de simuler une couche cachée. Le labo peut
donc garder ses corps PhysX actifs tout en figeant ses insectes. La correction
de ce chantier sépare le pas de population de la visibilité pour une scène
encore chargée, et exclut les interactions du joueur distant. Cela ne fournit
ni persistance après fermeture, ni simulation de régions déchargées. Simuler
intégralement des populations lointaines a un coût : une représentation
agrégée et un scheduler d'activité restent à concevoir.

## Deux travaux structurants à choisir ensuite

1. **Simulation et temps du monde** : contrat d'horloge, zones actives ou
   agrégées, persistance des modifications et stratégie de réveil ; prototype
   avec eau/chaleur/plante et tests de conservation causale. Examiner
   l'ordonnancement PhysX/CUDA sans changer le résultat physique.
2. **Géométrie et monde spatial** : format par clusters hiérarchiques,
   sélection stéréo avec erreur en pixels, résidence et streaming des
   cellules, puis éclairage et collisions compatibles. Préparer un banc
   synthétique de complexité contrôlée et vérifier aussi les pires cas.

Chaque direction exige une validation des deux captures de référence et des
phénomènes existants. Les tests de charge servent à invalider les hypothèses,
pas à remplacer les contrats de conception.

## Références techniques

- PhysX 5.9 utilisé : https://github.com/NVIDIA-Omniverse/PhysX/tree/110.1-omni-and-physx-5.9.0
- Direct GPU et sommeil : https://nvidia-omniverse.github.io/PhysX/physx/5.8.0/docs/MigrationTo57.html
- Simulation asynchrone : https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/Threading.html
- PhysX GPU et contraintes : https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/GPURigidBodies.html
- Culling indirect D3D12 : https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing-and-gpu-culling-
- Mesh shaders et LOD : https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-mesh-shader-samples-win32/
- Nanite, SIGGRAPH 2021 : https://advances.realtimerendering.com/s2021/index.html
