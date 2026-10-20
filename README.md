# MacNativeGames — portage Android

Ce dépôt est le point de départ d'un portage **Android** de
[MacNativeGames](https://github.com/ly05010419/MacNativeGames) (TOTK recompilé
nativement pour Apple silicon, et non émulé).

> **État : squelette fonctionnel, pas encore un jeu jouable.**
> **Testé sur un appareil réel** (Redmi 23090RA98G, Mali-G610 MC4, Android 16) :
> ~120 fps, rendu correct, cycle arrière-plan/premier plan OK.
> Ce qui marche ici : la chaîne de build Android complète (NDK → `.so` →
> APK signé), l'initialisation Vulkan, la *gate* HLE en assembleur ARM64, la
> traversée invité → hôte → GPU d'une trame, et le rendu d'un triangle.
> Ce qui manque : le chargeur de modules NSO, les services Horizon, et les
> ~13 700 lignes du backend NVN. Voir [docs/04-feuille-de-route.md](docs/04-feuille-de-route.md)
> et le [journal de mise au point](docs/06-journal-de-mise-au-point.md).

![Architecture](docs/architecture.svg)

## Pourquoi ce n'est pas un simple « rebuild pour Android »

MacNativeGames n'émule pas la Switch : il exécute **le code ARM64 du jeu tel
quel** et fournit à sa place ce que fournirait la console — un chargeur, les
services de l'OS Horizon, et un pilote graphique NVN. La Switch et un
téléphone Android sont tous deux ARM64, donc l'idée de base tient. Mais tout ce
qui est spécifique à Apple doit être refait :

| Couche | macOS / iOS | Android |
|---|---|---|
| Code du jeu (ARM64) | inchangé | inchangé ✅ |
| Réécriture `svc` / `x18` / `tpidrro_el0` | `patch_nso.py` | identique ✅ |
| Chargeur, services Horizon | Mach-O, mach_vm, pthread | ELF, mmap, pthread (à porter) |
| Graphismes NVN | **Metal 4** (13 696 lignes) | **Vulkan** (à réécrire) |
| Shaders Maxwell | SPIR-V → SPIRV-Cross → MSL | **SPIR-V direct** (plus simple) |
| Audio | Core Audio | AAudio |
| Manettes / tactile | GameController.framework | `AInputEvent` + vue tactile |
| Décodage VP9 | libvpx | `AMediaCodec` (matériel) |

Le détail complet est dans [docs/02-portage-android.md](docs/02-portage-android.md).

## Exécuter le jeu : où en est le chemin

Le pipeline d'import est en place et fonctionne sous Linux (il réutilise celui
de MacNativeGames) :

```sh
python3 tools/import_totk.py ~/Games/totk.xci -k ~/keys/prod.keys --upstream ~/MacNativeGames
python3 tools/push_data.py --work ~/TOTK_android
python3 tools/build_apk.py --game-tree ~/TOTK_android
```

Il reste le chargeur NSO, les services Horizon et le backend NVN → Vulkan.
Le détail, l'ordre et les efforts sont dans
[docs/07-executer-le-jeu.md](docs/07-executer-le-jeu.md).

## Construire

Prérequis : NDK r26+ , SDK Android (build-tools + platform), JDK (pour
`apksigner`). Rien d'autre — pas de Gradle, pas de dépendance tierce.

```sh
export ANDROID_NDK=/chemin/vers/android-ndk-r27c
export ANDROID_HOME=/chemin/vers/android-sdk
export JAVA_HOME=/chemin/vers/jdk

python3 tools/build_apk.py                 # arm64-v8a, debug-signé
adb install -r out/TOTK-debug.apk
adb logcat -s mnge-vk mnge-nvn mnge-main
```

Options : `--abi x86_64` (émulateur), `--release`, `-o chemin.apk`.

## Règle de provenance — vérifiée automatiquement

**Ce dépôt ne contient aucun code de jeu, aucune clé, aucun firmware.** C'est
à l'utilisateur de fournir son XCI/NSP et son `prod.keys` au moment de la
compilation ; `tools/import_totk.py` en dérive les modules et le RomFS dans un
dossier **hors du dépôt**, sur sa machine.

Ce n'est pas une promesse, c'est contrôlé :

```sh
python3 tools/check_provenance.py     # échoue si quoi que ce soit dérive du jeu
```

Le script refuse les noms de fichiers de dump (`*.xci`, `*.nca`, `prod.keys`,
`*.patched.bin`, `runtime_tables.h`, `*.bfttf`…), les fichiers commençant par
une signature de conteneur (`HEAD`, `PFS0`, `NSO0`), les binaires anormalement
gros pour du source, et les lignes qui ressemblent à un dump de clés. Il est
utilisable tel quel en pre-commit ou en CI.

## Légal

Comme le projet d'origine : **vous devez posséder le jeu**. Ce dépôt ne
contient aucun contenu de jeu — ni programme, ni texte, ni texture, ni son, ni
shader, ni clé, ni firmware. Rien n'est téléchargé. L'APK que vous construisez
contiendra le jeu et est destiné à votre usage personnel uniquement ; ne le
partagez pas. Projet indépendant, non commercial, non affilié à Nintendo.

## Arborescence

```
app/cpp/            code natif (C + un fichier .S)
  main.c            NativeActivity : fenêtre, boucle, entrées
  vk.c / vk.h       device Vulkan, swapchain, passe de rendu, pipeline
  nvn.c / nvn.h     la couture NVN : flux de commandes, HLE, invité de démo
  gate.S            la gate HLE (svc) + changement de pile invité
  shaders/          GLSL compilé en SPIR-V, embarqué dans le .so
app/AndroidManifest.xml
tools/build_apk.py  cmake + aapt2 + zipalign + apksigner
tools/embed_spirv.py
docs/               analyse et plan de portage
```
