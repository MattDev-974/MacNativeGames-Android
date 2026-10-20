# 07 — Exécuter le jeu : le chemin complet

Réponse courte : il reste **quatre blocs**, dans cet ordre. Le premier est fait
et livré ici ; les trois autres sont des mois de travail, pas des semaines.

| # | Bloc | Effort | Dépend de vos fichiers |
|---|---|---|---|
| A | Importer votre XCI/NSP et le packager pour Android | **fait** | oui, pour l'exécuter |
| B | Le chargeur de modules NSO | 2–3 sem. | oui |
| C | Les services Horizon (threads, mémoire, fs, temps, hid, audio) | 3–4 sem. | oui |
| D | Le backend NVN → Vulkan | 3–4 mois | oui |

---

## A. L'import — fait

`tools/import_totk.py` réutilise **tel quel** le pipeline de MacNativeGames :
lecture du conteneur, vérification de chaque empreinte, réécriture des NSO,
déchiffrement des 17 Go de RomFS. Rien de tout cela n'est spécifique à Apple —
c'est du Python plus un hactool compilé localement — et je l'ai vérifié en
exécutant les scripts amont ici, sous Linux.

```sh
# 1. importer (une fois ; ~20 Go d'espace disque)
python3 tools/import_totk.py ~/Games/totk.xci -k ~/keys/prod.keys \
        --upstream ~/MacNativeGames --work ~/TOTK_android

# 2. envoyer les données sur le téléphone (le RomFS ne tient pas dans un APK)
python3 tools/push_data.py --work ~/TOTK_android

# 3. construire l'APK avec les modules du jeu
python3 tools/build_apk.py --game-tree ~/TOTK_android
```

Ce que produit l'étape 1 :

```
~/TOTK_android/totk/rewritten/v0/main.patched.bin      le code du jeu, réécrit
~/TOTK_android/totk/rewritten/v0/*.gates.tsv           une ligne par site réécrit
~/TOTK_android/totk/rewritten/v0/*.fixups.tsv          la disposition à reproduire
~/TOTK_android/totk/extracted/v0/elf/                  inventaire des symboles
~/TOTK_android/totk/extracted/v0/romfs/romfs.bin       17 Go
```

**Découverte importante** : la porte HLE n'est pas à une adresse fixe. Le
réécrivain la fait lire dans un **slot de thread** (`KGATE`), que le chargeur
renseigne au démarrage. Conséquence : `main.patched.bin` est **le même binaire
pour macOS, iOS et Android**. Seul le chargeur change. C'est ce qui rend le
portage réaliste.

### Ce qu'il vous faut

* la version **1.0.0** (`0100F2C0115B6000`) en XCI ou NSP, plus `prod.keys` ;
  les mises à jour 1.1.2 / 1.2.1 / 1.4.3 viendront après (`--update` n'est pas
  encore câblé côté Android) ;
* **~20 Go libres** sur le PC et sur le téléphone ;
* pour un NSP : le dossier firmware décompressé (`--firmware`) pour les polices
  partagées, comme sur Mac.

### État de l'import (base 1.0.0, repack XCI)

Réalisé et vérifié :

| | |
|---|---|
| RomFS | 304 207 fichiers, 17,07 Go, SHA-256 vérifié sur les **6 niveaux IVFC** |
| `main.patched.bin` | 118,5 Mo — 45,6 Mio de code réécrit |
| `sdk.patched.bin` | 15,9 Mo — 27 747 exports, 60 sites `svc`, 40 `cntpct` |
| `subsdk0.patched.bin` | 7,4 Mo — 10 954 exports |
| `runtime_tables.h` | 22,9 Mo — 558 865 relocations, 3 modules |
| page de clés | 0x4000, 8 slots, `KGATE` = index 3 |
| polices partagées | 6, vérifiées |
| APK avec le jeu | 142 Mo (`libgame_*.so` + `assets/nvn/*.tsv`) |

Les overlays RomFS du projet (capture rapide, réessai après game over) et celui
de la sortie 2560×1440 sont générés et vérifiés.

---

## B. Le chargeur — la prochaine brique

C'est `runtime/loader.c` (505 lignes), à porter. Il fait, dans l'ordre :

1. **réserver** la région d'adresses invitée sous 2³⁹ (le SDK encode les
   adresses de tampon IPC sur 39 bits) ;
2. **mapper** les trois modules (`main`, `sdk`, `subsdk0`) à leur disposition
   d'origine, en appliquant `*.fixups.tsv` ;
3. **installer la porte** dans le slot de thread que `patch_nso.py` a laissé
   (`KGATE`) — c'est le seul point où Android diffère vraiment d'Apple, à cause
   de l'inversion `tpidr_el0` / `tpidrro_el0` (doc 02, §1.1) ;
4. **résoudre les imports** entre modules (la table de hachage de symboles est
   portable telle quelle) ;
5. **sauter** dans `nn::init::Start`.

Ce qui change par rapport à Apple :

| Apple | Android |
|---|---|
| `mach_vm_allocate` / `mach_vm_remap` | `mmap` / `mmap(MAP_FIXED_NOREPLACE)` |
| `sys_icache_invalidate` | `__builtin___clear_cache` |
| `CC_SHA256` | BoringSSL (`<openssl/sha.h>`) |
| `pthread_threadid_np` | `gettid()` |
| `mach_absolute_time` | `clock_gettime(CLOCK_MONOTONIC)` |
| ucontext Mach | `ucontext_t` Linux (identique dans les champs utiles) |

Et deux contraintes qui n'existent pas sur Mac :

* **W^X / SELinux** : impossible d'écrire dans un segment exécutable. Tous les
  patchs doivent être appliqués *au build* (ils le sont déjà : `patch_nso.py`
  produit un fichier définitif) et les modules doivent être chargés depuis
  `nativeLibraryDir` (d'où `lib/arm64-v8a/libgame_*.so`). Pas de patch à chaud.
* **pages de 16 Kio** : la logique de permissions 4 Kio de `loader.c` doit être
  refaite avec la bonne granularité (doc 02, §3.2).

### État du chargeur

Fait et validé sur les données réelles (`tests/host/run_loader.sh`, qemu-aarch64) :

| Étage | Résultat |
|---|---|
| mapping des 3 modules | `main` 118,5 Mo, `sdk` 15,9 Mo, `subsdk0` 7,4 Mo |
| `fixups.tsv` ↔ `runtime_tables.h` | cohérence vérifiée (patch, clé, text_end, data) |
| relocations | **558 865 appliquées, 0 non résolue** (514 885 relatives, 43 980 symboliques, 8 661 inter-modules) |
| portes | **100 installées** (60 `svc` + 40 `cntpct`), chacune dispatchée vers la bonne cible |
| permissions | `.text` R+X, écriture refusée (W^X), `.data` et page de clés R+W |

Découverte structurante : **les services Horizon ne passent pas par des
imports**. Les 0 import manquant le prouvent — tout se résout entre modules. Les
services sont atteints par les **60 appels `svc`** de la table des portes :
`1–29, 31, 33–35, 37–42, 44, 45, 47, 50–54, 64, 65, 67–69, 72, 73, 75, 80, 81,
82, 112, 114`. C'est la liste exacte, bornée, de ce que `services.c` doit
implémenter.

---

## C. Les services Horizon

`runtime/` contient ~4 500 lignes portables et ~2 000 lignes Apple à réécrire.
Ordre de priorité, par ce qui bloque le démarrage :

1. `threads.c` — threads et synchronisation (pthread, piles invité de 8 Kio) ;
2. `services.c` + `ipc.c` — le protocole IPC de Horizon ;
3. `fs_host.c` + `romfs.c` — lecture du RomFS et des sauvegardes ;
4. `time_host.c`, `account.c`, `applet_host.c` ;
5. `input.c` + `gamepad.m` → `AInputEvent` ;
6. `audio_*` → AAudio.

À ce stade le jeu devrait **démarrer et tourner sans image**. C'est un jalon
précieux : il sépare les bugs de services des bugs de rendu.

---

## D. Le backend NVN → Vulkan

Voir [03-nvn-vulkan.md](03-nvn-vulkan.md). 13 700 lignes de Metal à réécrire,
avec deux décisions structurantes à prendre tôt :

* **l'alias mémoire** (pools zéro-copie) : `VK_EXT_external_memory_host` marche
  sur Adreno, pas sur Mali. À mesurer avant de s'engager ;
* **les textures BC** : Mali ne les supporte pas ; il faut transcoder en ASTC au
  build.

Les shaders sont la bonne nouvelle : le traducteur Ryujinx produit déjà du
SPIR-V, que Vulkan consomme directement. `SPIRV-Cross` et le MSL disparaissent
du chemin — c'est **plus simple que sur Mac**. Et `prepare_shader_seed.py`
(pré-traduction au build) est réutilisable.

---

## Par où commencer concrètement

1. `git clone https://github.com/ly05010419/MacNativeGames` à côté de ce dépôt
   (ou `--upstream` vers votre copie).
2. Lancer `tools/import_totk.py` sur votre XCI. C'est long (17 Go) mais
   entièrement vérifié par empreintes.
3. M'envoyer `gates.tsv`, `fixups.tsv` et `main-inventory.json` (quelques
   centaines de Ko, **aucune donnée de jeu** : ce sont des tables d'adresses et
   de symboles). J'écris le chargeur contre ces données réelles.
4. Ensuite seulement, le backend Vulkan.

Le point 3 est le vrai déblocage : sans ces tables, le chargeur s'écrit à
l'aveugle.
