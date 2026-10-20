# 05 — Construire, installer, déboguer

## 1. Outils

| Outil | Version testée | Rôle |
|---|---|---|
| NDK | r27c (clang 18) | compilation croisée aarch64 |
| build-tools | 34.0.0 | `aapt2`, `zipalign`, `apksigner` |
| platform | android-34 (`android.jar`) | liaison du manifeste |
| JDK | 21 | `apksigner` / `keytool` |
| cmake + ninja | 3.22+ | build natif |
| glslangValidator | optionnel | GLSL → SPIR-V (sinon `.spv` fournis) |

Rien d'autre : **pas de Gradle, pas de dépendance tierce, pas de téléchargement
au build** (hors les outils ci-dessus, à installer une fois).

## 2. Build

```sh
export ANDROID_NDK=$HOME/Android/Sdk/ndk/27.0.12077973
export ANDROID_HOME=$HOME/Android/Sdk
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk

python3 tools/build_apk.py            # → out/TOTK-debug.apk
```

Le script :

1. compile les shaders en SPIR-V et régénère `app/cpp/shaders_spv.h` ;
2. configure CMake avec le toolchain NDK et produit `build/<abi>/libtotk.so` ;
3. `aapt2 compile` puis `aapt2 link` (manifeste + ressources, **sans dex**) ;
4. ajoute `lib/<abi>/libtotk.so` **non compressé** dans l'APK ;
5. `zipalign -p 4` puis `apksigner` (v2 + v3, clé de debug générée au premier
   build).

Options : `--abi x86_64` (émulateur), `--api 26`, `--release`, `-o`.

## 3. Installer et regarder

```sh
adb install -r out/TOTK-debug.apk
adb logcat -s mnge-main mnge-vk mnge-nvn
```

Sortie attendue :

```
mnge-main  MacNativeGames / Android starting
mnge-vk    GPU: Adreno (TM) 740 (Vulkan 1.3.x)
mnge-vk    swapchain 2400x1080 fmt=37 images=3
mnge-vk    Vulkan ready: 2400x1080
mnge-nvn   guest pool at 0x7... , 64 KiB; guest stack 0x7...
```

Un triangle coloré tourne sur fond bleu nuit.

## 4. Ce qui a été vérifié ici, et ce qui ne l'a pas été

Vérifié sur la machine de rédaction (Linux x86-64, sans appareil Android) :

* compilation croisée `aarch64-linux-android26` sans erreur ;
* `libtotk.so` : ELF64 AArch64, `DT_NEEDED` = `libandroid`, `liblog`,
  `libvulkan`, `libm`, `libdl`, `libc` ;
* `ANativeActivity_onCreate` bien exporté (piège : le linker supprime le
  membre d'archive si rien ne le référence → `-Wl,-u,ANativeActivity_onCreate`) ;
* segments `LOAD` alignés sur `0x4000` (obligatoire pour les noyaux 16 Kio) ;
* désassemblage de `hle_gate` : trame de 800 octets, offsets conformes, `ret`
  correct ;
* APK : `aapt2 dump badging` OK, `arm64-v8a`, `minSdk 26`, activité
  `android.app.NativeActivity` déclarée ;
* signature : `apksigner verify` → v2 ✅, v3 ✅, 1 signataire ;
* **test hôte en aarch64 réel** : `tests/host/run.sh` compile `nvn.c` + `gate.S`
  pour `aarch64-linux-musl` et l'exécute sous `qemu-aarch64`. Il vérifie que
  `hle_gate` préserve x19–x28 et q8/q9/q15 (la propriété dont tout le portage
  dépend) et que la trame invitée produit bien clear + uniforme + vertex buffer
  + `draw(0,3)`. **PASSED**.

**Validé sur appareil** : Redmi 23090RA98G (Mali-G610 MC4, Android 16) —
Vulkan 1.3, ~120 fps, triangle correct, arrière-plan/premier plan OK. Cinq
défauts ont été corrigés pour y arriver : voir
[06-journal-de-mise-au-point.md](06-journal-de-mise-au-point.md).
Le rendu, la gate et le cycle de vie de la fenêtre doivent être validés sur un
téléphone avant d'aller plus loin — c'est l'objet de l'étape 0/1 de la feuille
de route.

## 5. Déboguer sur l'appareil

* `adb shell dumpsys SurfaceFlinger` — vérifier que la surface est bien à la
  bonne taille.
* Couche de validation Vulkan : poser le fichier
  `/data/local/debug/vulkan/` (voir la doc Android « Validation layer ») et
  relancer ; `logcat -s vulkan`.
* `adb shell am force-stop org.macnativegames.totk` avant chaque
  réinstallation pour éviter les surfaces fantômes.
* Si l'écran reste noir mais que `Vulkan ready` s'affiche : c'est presque
  toujours un `VkRenderPass` incompatible avec la `VkFramebuffer` (formats), ou
  une transition de layout manquante. Activer la couche de validation.

## 6. Ajouter le vrai chargeur

Quand l'étape 1 de la feuille de route attaque le chargeur :

```cmake
# app/cpp/CMakeLists.txt
target_sources(totk PRIVATE
    ../../../MacNativeGames/games/totk/runtime/loader.c
    ../../../MacNativeGames/games/totk/runtime/threads.c
    ../../../MacNativeGames/games/totk/runtime/fs_host.c
    ../../../MacNativeGames/games/totk/runtime/romfs.c)
```

Il faut d'abord remplacer les en-têtes Mach par un `plat/linux_compat.h`
équivalent à `mach_vm_compat.h` :

| Mach | Linux/Android |
|---|---|
| `mach_vm_allocate` | `mmap(PROT_NONE)` puis `mprotect` |
| `mach_vm_deallocate` | `munmap` |
| `mach_vm_remap` | `mmap(..., MAP_FIXED_NOREPLACE)` de la même source |
| `mach_vm_region` | `/proc/self/maps` |
| `mach_vm_read_overwrite` | `process_vm_readv` ou `memcpy` |
| `mach_absolute_time` | `clock_gettime(CLOCK_MONOTONIC)` |
| `pthread_threadid_np` | `gettid()` |
| `sys_icache_invalidate` | `__builtin___clear_cache` |
| `CC_SHA256` | BoringSSL (`<openssl/sha.h>`) ou implementation locale |
| `vm_protect` | `mprotect` |
| `os_unfair_lock` | `pthread_mutex` |

Le reste de `loader.c` — table de modules, hachage de symboles, relocations,
vérification des empreintes — ne change pas.
