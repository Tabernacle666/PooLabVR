# Carnet de compilation OASIS

À consulter avant de modifier les types CPU/GPU, les appels au SDK Windows ou les shaders. Pour chaque nouveau blocage, ajouter le premier diagnostic, sa cause, le correctif minimal et une vérification qui aurait détecté l’oubli.

| Diagnostic observé | Cause | Correctif et vérification préventive |
| --- | --- | --- |
| MSVC `C2664` sur `XMLoadFloat4`, suivi de `C2660` sur `XMStoreFloat4`, dans `GraphicsRender.cpp` | `GpuInstance::rotation` est un `GpuFloat4` du tampon GPU. Les fonctions DirectXMath prennent des pointeurs vers `DirectX::XMFLOAT4`. | Copier les quatre composantes dans un `XMFLOAT4`, appliquer DirectXMath, puis recopier le résultat. Garder la disposition de `GpuInstance` intacte. Compiler le passage avec les vrais en-têtes `GpuMeshTypes.h` et DirectXMath pour vérifier la conversion. [Révision du blocage](https://github.com/Tabernacle666/PooLabVR/commit/911f138872707082efaaf9c185d642e5f676b965) |
| MSVC `C3861` sur `XAudio2CutoffFrequencyToRadians` dans `ShuttleAudio.cpp` | Les fonctions auxiliaires XAudio2 sont déclarées quand `XAUDIO2_HELPER_FUNCTIONS` est défini avant la première inclusion de `xaudio2.h` dans l’unité de compilation. | Reprendre le motif de `LunarBattleAudio.cpp` dans l’unité qui appelle le helper. Un substitut local de l’en-tête qui expose le helper en permanence masque cet oubli. Vérifier avec le SDK Windows ou avec un substitut qui respecte la même condition. [Correctif](https://github.com/Tabernacle666/PooLabVR/commit/34adc9cd222581a6133e7a8c4773d2468d30d823) |
| DXC `no member named 'visualData' in 'CutInstance'` dans `impact_shared.hlsli` | Le constructeur d’impact manipule le format compact `CutInstance`. Le nouvel entier visuel appartient à `GpuInstance`. | Garder `CutInstance` et son initialisation intacts. Compiler `StepProjectiles` en plus de `MeshMain` et des passes de visibilité lors d’un changement d’instance. |

## Vérification ciblée

- Lire la première erreur de l’unité de compilation concernée et les notes de types qui la suivent. Corriger cette cause avant de traiter les diagnostics suivants.
- Pour du C++ Windows ou DX12, privilégier `build.bat` sur Windows. Si seul un contrôle local avec des en-têtes partiels est possible, indiquer précisément les unités contrôlées et réserver la confirmation du build complet à MSVC.
- Pour du HLSL, compiler les points d’entrée et les variantes touchés avec DXC. Indiquer séparément le résultat des shaders et celui des unités C++.
