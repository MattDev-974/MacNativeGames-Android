# 04 — Feuille de route

Découpage en étapes livrables. Les durées supposent **une personne à temps
plein**, familière du C et de Vulkan, sans connaissance préalable du code
d'origine. Elles sont indicatives : le risque principal (§ Risques) peut les
doubler.

---

## Étape 0 — Socle (fait dans ce dépôt)

**Livrable** : un APK qui s'installe, initialise Vulkan, traverse la gate HLE et
affiche un triangle tournant.

| Élément | État |
|---|---|
| Build NDK → `.so` → APK signé, sans Gradle | ✅ |
| Alignement 16 Kio, arm64-v8a, minSdk 26 | ✅ |
| `ANativeActivity_onCreate` exporté, `hasCode="false"` | ✅ |
| Device Vulkan 1.1 + swapchain + `suspend`/`resume` | ✅ |
| Gate `svc` ARM64 + changement de pile invité | ✅ |
| Pools mémoire + flux de commandes NVN + rejeu | ✅ |
| Shaders GLSL → SPIR-V embarqué | ✅ |
| Chargeur NSO, services Horizon, backend NVN complet | ❌ |

**Critère de sortie** : `adb logcat -s mnge-vk` affiche `Vulkan ready` et le
triangle tourne. *(Non vérifié ici : aucun appareil Android n'était disponible
pendant la rédaction. Le `.so` et l'APK sont compilés, liés, signés et
structurellement valides.)*

---

## Étape 1 — Prouver la prémisse (2–3 semaines)

Objectif : **du vrai code ARM64 du jeu s'exécute sur Android**. C'est le point
de non-retour : si ça ne marche pas, le reste n'a pas de sens.

1. Porter `runtime/loader.c` : `mmap`/ELF, flush du cache d'instruction
   (`__builtin___clear_cache` remplace `sys_icache_invalidate`), SHA-256
   (BoringSSL ou implémentation locale à la place de CommonCrypto).
2. Vérifier l'inversion `tpidrro_el0` / `tpidr_el0` (§1.1 du doc 02).
3. Porter `runtime/threads.c` sur pthread, avec piles de 8 Kio côté invité.
4. Charger **un seul** module NSO réécrit, résoudre ses imports, appeler son
   point d'entrée, et faire en sorte qu'il exécute une boucle et revienne.
5. `runtime/fs_host.c` + `romfs.c` : ouvrir un fichier du RomFS depuis le code
   invité.

**Critère de sortie** : un log émis *par le code invité* apparaît dans
`logcat`, et le module retourne proprement.

> C'est ici qu'on saura si `execmod`/SELinux bloque. Charger depuis
> `nativeLibraryDir` (voir doc 02 §3.3) dès le départ.

---

## Étape 2 — Backend NVN minimal (6–10 semaines)

Objectif : **un décor du jeu s'affiche**.

1. `mtl_pool_add`/`mtl_addr` sur mémoire réelle (copie explicite d'abord,
   alias ensuite).
2. Buffers, pipelines, render pass mono-cible, depth, draws indexés.
3. Textures 2D + samplers + formats BC (décodage CPU d'abord, ASTC ensuite).
4. `hle_nvn.c` branché sur le nouveau backend, sans changement.
5. Traducteur de shaders : SPIR-V direct, validé sur les sondes existantes.

**Critère de sortie** : l'écran-titre ou une scène statique s'affiche, image
correcte, sur **au moins deux GPU différents** (Adreno et Mali).

C'est l'étape où il faut instrumenter : reprendre les `nvn_probe_*` du projet
(`probe_vertex_streams`, `probe_depth16_clear`, `probe_copy_alias`, …) et les
faire tourner sur l'appareil. Ils isolent chaque chemin et évitent de déboguer
une scène entière.

---

## Étape 3 — Jeu jouable (8–12 semaines)

1. Cibles de rendu multiples, MRT, résolution dynamique.
2. Compute, storage buffers, barriers d'alias.
3. Audio : `AAudio`, mixage, effets, réverbération.
4. Entrées : manette (`AInputEvent`), tactile, gyroscope, vibration.
5. Clavier tactile pour le `swkbd`.
6. Cinématiques : `AMediaCodec` VP9.
7. Sauvegardes, mods, cache de shaders.

**Critère de sortie** : déplacement, menus, carte, sauvegarde/rechargement,
mort et réessai. C'est-à-dire la liste que le projet d'origine valide sur Mac.

---

## Étape 4 — Performance (continu)

1. Trancher la question de l'alias mémoire (doc 02 §3.4) par une mesure.
2. Transcoder BC→ASTC au build.
3. Cache de pipelines persistant, précompilation au premier lancement.
4. Objectif réaliste : **30 fps stables en 720p** sur un appareil haut de gamme
   de 2023+. Le Mac de référence fait 90–120 fps sur M4 ; un téléphone n'a ni
   la bande passante ni le budget thermique.

---

## Risques, par ordre de gravité

| # | Risque | Impact | Atténuation |
|---|---|---|---|
| 1 | **Alias de pools impossible** sur Mali → copies massives | 🔴 performance | mesurer tôt (étape 2) ; `VK_EXT_external_memory_host` sur Adreno, plan B copies ciblées |
| 2 | **Pilotes Vulkan fragiles** sur les motifs NVN | 🔴 stabilité | tester sur 2 GPU ; contournements par pilote |
| 3 | **BC non supporté sur Mali** | 🟠 mémoire/bande passante | transcodage ASTC au build |
| 4 | **`execmod`/SELinux** bloque le code invité | 🔴 bloquant | charger depuis `nativeLibraryDir`, patchs au build uniquement |
| 5 | **Pages 16 Kio** cassent la logique RELRO 4 Kio | 🟠 instabilité | refaire l'union des permissions avec la bonne taille |
| 6 | **Traducteur de shaders sur l'appareil** (NativeAOT bionic) | 🟠 taille/complexité | pré-traduction au build comme la graine iOS |
| 7 | **Budget thermique** | 🟠 jouabilité | résolution dynamique, limite de fps |
| 8 | **Maintenance** : le projet amont bouge vite (1.0.0 → 1.4.3 en 3 jours) | 🟠 | suivre la couture `mtl_*`, pas les internes |

---

## Ce qu'il ne faut pas faire

* Réécrire `hle_nvn.c`. Il est portable et c'est 2 727 lignes déjà validées.
* Toucher au format des commandes NVN. Le rejeu doit s'adapter au flux, pas
  l'inverse.
* Chercher la parité 1:1 avec `nvn_mtl.m` ligne par ligne. Les deux API
  diffèrent sur la synchronisation et les layouts ; une traduction fidèle des
  *intentions* vaut mieux qu'une traduction littérale.
