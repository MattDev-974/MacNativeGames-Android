# 02 — Ce qui se transpose, ce qui se réécrit

Analyse du dépôt [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
(commit du 19 octobre, ~39 000 lignes de C/Objective-C) couche par couche, et
conclusion pour Android.

---

## 1. Ce qui se transpose **tel quel**

| Élément | Pourquoi |
|---|---|
| Le code du jeu | ARM64 → ARM64. Aucune récompilation du jeu n'est nécessaire : c'est déjà le principe du projet. |
| `patch_nso.py` (réécriture `svc`, `x18`, `tpidrro_el0`, `cntpct`) | Mêmes contraintes : Android réserve aussi `x18` (registre plateforme) et n'autorise pas `svc` depuis l'espace utilisateur. |
| `runtime/gate.S` + `runtime/hle_stack.S` | Le changement de pile hôte et la sauvegarde intégrale des registres sont identiques. Seul le pointeur de thread (`tpidrro_el0`) change de rôle — voir §1.1. |
| Table de hachage de symboles, résolution de relocations (`loader.c`) | Logique pure, indépendante du format de fichier. |
| `romfs.c`, `fs_host.c`, `shared_font.c`, `account.c`, `applet_host.c` | POSIX + parsing BFTTF. |
| `audio_mixer.c`, `audio_reverb.c`, `audio_effects.c`, `motion_fusion.c` | Calcul flottant pur. |
| `nvn_tex.c` (décodage GOB / block-linear), `nvn_dirty.c` (suivi des pages sales) | C portable ; seul `mprotect` change de granularité (§3.2). |
| `hle_nvn.c` (2 727 lignes, 575 points d'entrée NVN) | Il ne parle pas à Metal : il appelle la couture `mtl_*`. |

### 1.1 `tpidrro_el0`, le point à vérifier en premier

`gate.S` retrouve le TSD du runtime en lisant `tpidrro_el0`, et `hle_stack.S`
s'en sert pour basculer de pile.

* Sous Apple, `tpidrro_el0` est le pointeur de thread utilisateur, et
  `tpidr_el0` est réservé au noyau.
* Sous Linux/Android, c'est **l'inverse** : `tpidr_el0` est la TLS de Bionic,
  `tpidrro_el0` est libre côté EL0 (le noyau ne l'utilise que pour KVM/VHE).

Donc le mécanisme fonctionne, mais **il faut inverser les deux registres** dans
la réécriture : les sites `mrs xN, tpidrro_el0` du jeu doivent lire
`tpidr_el0`, et la gate doit stocker son TSD dans `tpidrro_el0`. À vérifier sur
un appareil réel avant d'aller plus loin — c'est le prérequis de tout le reste.

---

## 2. Ce qui doit être réécrit, fichier par fichier

| Fichier d'origine | Lignes | Android | Effort |
|---|---:|---|---|
| `gfx/nvn/nvn_mtl.m` | 13 696 | **`nvn_vk.c`** — tout le backend | 🔴 3–4 mois |
| `gfx/nvn/totk_shader_rt.m` | 279 | traducteur → SPIR-V direct, plus de SPIRV-Cross | 🟠 2–3 sem. |
| `gfx/nvn/nvn_refl.m` | — | réflexion depuis le SPIR-V au lieu de MSL | 🟠 |
| `gfx/nvn/totk_mfx.m`, `totk_mfx_up.m` | 662 | MetalFX → FSR1 / upscale simple | 🟠 |
| `runtime/loader.c` | 505 | **plein de Mach** : `OSCacheControl`, `CommonCrypto`, `mach_vm_*`, `ucontext` Mach | 🟠 2–3 sem. |
| `runtime/threads.c` | 723 | Mach threads → pthread ; priorités | 🟠 |
| `runtime/audio_renderer.c` | 598 | Core Audio → **AAudio** | 🟢 1 sem. |
| `runtime/gamepad.m`, `gamepad_feedback.m` | 512 | `AInputEvent` + `ASensor` + `AVibrator` | 🟢 1 sem. |
| `runtime/frontend.m` | 360 | AppKit → overlay sur `ANativeWindow` | 🟢 |
| `runtime/launcher.m` | 200 | supprimé (l'Activity fait office de lanceur) | 🟢 |
| `runtime/swkbd_host.c` | 271 | clavier Mac → clavier tactile (JNI) | 🟢 |
| `runtime/movie_host.c` | 459 | libvpx → **`AMediaCodec`** (VP9 matériel) | 🟢 |
| `runtime/time_host.c` | — | `mach_absolute_time` → `clock_gettime` | 🟢 |
| `gfx/nvn/totk_vpool.c` | — | `mach_vm_remap` (alias) → à repenser (§3.3) | 🟠 |
| `gfx_bridge.c`, `memory_probe.c`, `world_survey.c` | — | en-têtes Mach, watchpoints | 🟢 |
| `tools/totk_xlate` (binaire macOS) | — | rebuilt en `linux-bionic-arm64` ou exécuté au build | 🟠 |

Le reste (~15 000 lignes) compile sans modification.

---

## 3. Les six problèmes propres à Android

### 3.1 `svc` : identique, mais la gate doit changer de pile

Rien à faire de plus que sur Apple. En revanche les piles de thread du jeu font
8 Kio alors qu'Android en donne 1 Mio par défaut : le changement de pile de
`hle_stack.S` n'est plus une optimisation, c'est **obligatoire**. Il est déjà
porté dans `app/cpp/gate.S` de ce dépôt.

### 3.2 Pages de 16 Kio (Android 15+)

Le chargeur raisonne en pages invité de 4 Kio et compense la page Darwin de
16 Kio en prenant l'**union des quatre permissions** (`runtime_module_permission`,
`module_page_permission`). Sur un noyau Android à pages de 16 Kio, cette
compensation doit être refaite avec la bonne taille — sinon les `mprotect` de
RELRO échouent en `EINVAL` et le jeu s'arrête.

Notre propre `.so` est déjà aligné 16 Kio (`-Wl,-z,max-page-size=16384`),
sinon il ne se charge même pas sur ces appareils.

### 3.3 W^X et SELinux : le vrai piège

Android refuse `execmod` (écrire dans une page puis l'exécuter) pour les
fichiers du répertoire applicatif. Or :

1. `game_patches.c` applique des patchs **en mémoire** sur le module chargé
   (cadence de trame en temps réel, intervalle de présentation 60 Hz, résolution
   de sortie). Écrire dans le segment exécutable est refusé.
   **Solution :** appliquer ces patchs *au build*, dans `patch_nso.py`, puisque
   ce sont des patchs fixes et que le projet vérifie déjà chaque site par
   empreinte. Le fichier livré est alors définitif et se charge en lecture seule.
2. Le module réécrit doit être exécutable. Le placer dans `lib/<abi>/` de l'APK
   (`nativeLibraryDir`, lecture seule, `exec` autorisé) est la seule voie
   robuste. Le RomFS, lui, reste dans le stockage applicatif : ce sont des
   données, pas du code.

C'est faisable, mais cela impose que **tout le code soit figé avant
l'installation** — fini les patchs à chaud.

### 3.4 Les pools mémoire zéro-copie

Sur Apple, un `MTLBuffer` peut pointer directement sur la mémoire du jeu
(`newBufferWithBytesNoCopy`) : CPU et GPU partagent la même adresse physique.
Vulkan n'a pas d'équivalent portable :

| Option | Réalité |
|---|---|
| `VK_EXT_external_memory_host` | importe un pointeur hôte en `VkDeviceMemory`. Supporté par Adreno (partiellement), **absent sur Mali**. À tester par pilote. |
| `VK_EXT_external_memory_android_hardware_buffer` | passe par `AHardwareBuffer`, alloue côté système → il faut *remplacer* les allocations du jeu, pas les aliaser. |
| Copie explicite | marche partout, coûte une copie CPU→GPU par ressource modifiée. |

Le jeu écrit dans ses pools depuis le CPU (vertices, uniformes) : sans alias,
il faut un suivi des pages sales (`nvn_dirty.c`, déjà là) et des transferts
`vkCmdUpdateBuffer`/staging ciblés. **C'est le premier risque de performance du
portage.** À trancher tôt, par une mesure sur l'appareil cible.

### 3.5 Les textures BC

TOTK utilise massivement BC1/BC3/BC7. Metal les accepte nativement. Sur Android :

* Adreno (Qualcomm) : BC supporté à partir des séries récentes ;
* **Mali (ARM) : aucun format BC**, uniquement ASTC/ETC2.

Sur Mali il faut soit décoder en RGBA8 (×4 à ×8 de bande passante, rédhibitoire
en 1080p), soit **transcoder BC→ASTC au build** (`astcenc`), ce qui ajoute
~1,5 Go de données préparées mais reste la bonne réponse. À intégrer dans
`prepare_romfs_backend.py`.

### 3.6 Le traducteur de shaders

Le projet embarque `totk_xlate`, un binaire .NET NativeAOT qui traduit le
Maxwell en SPIR-V puis, via SPIRV-Cross, en MSL. Sur Android :

* la sortie **SPIR-V est directement consommable** par `vkCreateShaderModule` :
  SPIRV-Cross et MSL disparaissent du chemin, c'est **plus simple** ;
* il reste à faire tourner le traducteur *sur l'appareil* pour les shaders
  manquants. NativeAOT cible `linux-bionic-arm64`, donc c'est possible, mais
  c'est un binaire de plusieurs dizaines de Mo à côté du jeu. Alternative :
  pré-traduire tout le RomFS au build comme le fait déjà la graine iOS
  (`prepare_shader_seed.py`, 15 266 shaders préparés pour 1.4.3).

Bonne nouvelle : contrairement à iOS, Android **autorise la génération de code
à l'exécution**. Pas besoin de précompiler 26 832 variantes dans l'APK.

---

## 4. Ce qui est *plus facile* sur Android

1. **Pas de contrainte de paquet signé** : la RAM n'est pas plafonnée à 4 Go
   comme sur iPhone (où le jeu se fait tuer au bout de ~8 minutes). Un appareil
   à 12 Go laisse de la marge.
2. **JIT autorisé** : traduction de shaders à la volée, pas de graine à figer.
3. **SPIR-V direct** : un étage entier (SPIRV-Cross) disparaît.
4. **Décodage vidéo matériel** : `AMediaCodec` décode le VP9 des cinématiques
   sans libvpx et sans coût CPU.
5. **Mémoire unifiée** comme sur Apple silicon — le principe des pools partagés
   reste valable, c'est juste l'API qui manque.

## 5. Ce qui est *plus difficile*

1. **Fragmenté** : Adreno, Mali, Xclipse, PowerVR. Un backend Vulkan correct
   sur l'un peut planter sur l'autre. Le projet d'origine n'a qu'un seul GPU
   cible (Apple).
2. **Pilotes plus jeunes** sur les usages NVN extrêmes (beaucoup de
   descripteurs, de cibles de rendu, de barrières). Prévoir du temps de
   contournement.
3. **Pas de MetalFX** : l'upscale et l'interpolation de frames sont à
   réimplémenter (FSR1 pour l'upscale).
4. **Thermique** : un téléphone tient 15–25 minutes à pleine charge avant de
   brider. Le jeu desktop tourne à 90–120 fps sur M4 ; viser 30 fps stables en
   720p est déjà un bon objectif.

---

## 6. Verdict honnête

Le cœur de l'idée — « le code ARM64 du jeu tourne tel quel » — **tient sur
Android**. Ce n'est pas la partie difficile.

La partie difficile est le backend graphique : 13 700 lignes de Metal à
réécrire en Vulkan, avec des contraintes mémoire (alias de pools) et de format
de texture (BC) qui n'ont pas de solution portable unique. C'est un projet de
**plusieurs mois à temps plein**, pas un week-end. La feuille de route
[docs/04-feuille-de-route.md](04-feuille-de-route.md) découpe ça en étapes
livrables.
