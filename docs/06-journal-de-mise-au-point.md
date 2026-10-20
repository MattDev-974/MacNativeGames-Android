# 06 — Journal de mise au point sur appareil

Cinq défauts ont été trouvés et corrigés en faisant tourner le squelette sur un
téléphone réel. Ils sont instructifs : les quatre premiers sont invisibles à la
compilation, et le cinquième est un défaut du pilote qu'il faut contourner.

**Appareil de test** : Redmi 23090RA98G (« zircon »), SoC MediaTek MT6886,
**Mali-G610 MC4**, Vulkan 1.3.219, Android 16. Écran 2712×1220, surface
applicative 2590×1220, `currentTransform = ROTATE_90`.

Symptôme initial : *l'application s'ouvre puis se ferme immédiatement.*

---

## 1. `vkGetSwapchainImagesKHR` : compteur d'entrée/sortie

```c
uint32_t n = 0;
vkGetSwapchainImagesKHR(dev, swap, &n, NULL);      // n = 8 (sortie)
vkGetSwapchainImagesKHR(dev, swap, &g.nimg, g.img); // ❌ g.nimg valait 0
```

Le deuxième appel attend la **capacité** du tableau dans `*count`. Avec 0, le
pilote n'écrit rien et renvoie `VK_INCOMPLETE`.

**Symptôme** : `swapchain 2590x1220 fmt=37 images=0` dans le log, aucun
framebuffer créé, puis `vkCmdBeginRenderPass` avec `VK_NULL_HANDLE` → SIGSEGV
dans le pilote.

**Correction** : `g.nimg = n` avant le second appel, plus un garde-fou
(`if (!g.img_fb[g.image_index]) return -1;`).

## 2. `VkShaderModuleCreateInfo.codeSize` est en **octets**

Mon générateur de SPIR-V embarquait le nombre de **mots de 32 bits**. Le pilote
lisait un module tronqué au quart.

**Symptôme** : `SIGSEGV` dans `libGLES_mali.so`, pile
`cmpbe_v2_compile_multiple_shaders` ← `vkCreateGraphicsPipelines` ←
`create_pipeline`. Cause : « null pointer dereference » à `0xb8`.

**Correction** : `embed_spirv.py` émet `_len` en octets (et `_words` pour les
contrôles), plus une vérification d'alignement dans `vk.c`.

## 3. La surface fait 1×1 à `APP_CMD_INIT_WINDOW`

Au moment où `INIT_WINDOW` arrive, Android n'a pas encore posé la surface :
`ANativeWindow_getWidth()` renvoie 1. Créer le swapchain à cette taille donne un
écran noir définitif.

**Symptôme** : `window 1x1`, `swapchain 1x1`, puis plus rien.

**Correction** : l'initialisation graphique est déplacée dans la boucle
principale ; elle attend une taille ≥ 2 px, et `create_swapchain()` refuse les
extents dégénérés. Le swapchain est en outre recréé quand la taille de la
surface change réellement.

## 4. Un swapchain jetable casse le device sur Mali

Pour connaître le format avant de créer la passe de rendu, le code créait un
premier swapchain puis le détruisait. Sur ce pilote, cela laisse le device dans
un état inutilisable.

**Correction** : le format est une propriété de la **surface**, pas du
swapchain. `pick_format()` l'obtient par `vkGetPhysicalDeviceSurfaceFormatsKHR`
avant toute création. Un seul swapchain est créé.

## 5. `VK_ERROR_DEVICE_LOST` en attendant le sémaphore d'acquisition

C'est le défaut le plus déroutant, et c'est un bug du pilote.

```c
vkAcquireNextImageKHR(dev, swap, t, sem_acquire[slot], VK_NULL_HANDLE, &idx);
// puis
submit.waitSemaphoreCount = 1;
submit.pWaitSemaphores = &sem_acquire[slot];   // → VK_ERROR_DEVICE_LOST (-4)
```

Ce qui a permis de l'isoler :

| Expérience | Résultat |
|---|---|
| submit vide, sans sémaphore ni fence, juste après `vkCreateDevice` | ✅ 0 |
| `vkDeviceWaitIdle` juste avant la première frame | ✅ 0 (device vivant) |
| submit sans passe de rendu mais **avec** le sémaphore | ❌ -4 |
| submit sans le sémaphore (`waitSemaphoreCount = 0`) | ✅ 0 |
| `vkAcquireNextImageKHR` → `result 0, image 0 of 8` | ✅ l'acquisition réussit |

**Correction** : acquérir avec une **fence** et attendre côté CPU, puis
soumettre sans sémaphore d'attente. C'est équivalent au regard de la spec, au
prix d'un aller-retour CPU par frame — négligeable ici, et `vk.c` garde le
chemin par sémaphore derrière `debug.mnge.acqfence=0` pour les pilotes sains.

**Leçon** : c'est exactement le risque « pilotes Vulkan fragiles » identifié
dans la feuille de route. Il faudra s'attendre au même genre de contournement
sur le vrai backend NVN.

## 6. `TPIDRRO_EL0` est en **lecture seule** depuis EL0

C'est le blocage architectural du portage, découvert en entrant dans le code du
jeu.

Le réécrivain transforme chaque `svc` en un bloc qui commence par :

```
mrs x30, tpidrro_el0     ; le bloc de thread, base de tous les slots
add x30, x30, #KGATE*8
ldr x30, [x30]           ; adresse de la porte
blr  x30
```

Sur macOS, `tpidrro_el0` est renseigné par l'OS et pointe sur le bloc de
thread : le mécanisme fonctionne. Sur AArch64, ce registre est **Read-Only à
EL0** — `msr tpidrro_el0, xN` est une instruction illégale.

Mesuré sur l'appareil :

```
Fatal signal 4 (SIGILL), code 1 (ILL_ILLOPC)
  #00 libtotk.so (guest_main+28)      ← l'écriture du registre
```

Et Linux ne le renseigne pas pour les threads utilisateur (il ne sert qu'à KVM).

**Solution retenue** (à implémenter) : faire porter le pointeur de thread par
`TPIDR_EL0`, qui *est* accessible en écriture.

1. `patch_nso.py` émet `mrs xN, tpidr_el0` au lieu de `tpidrro_el0` — un seul
   mot d'instruction à substituer sur place, même famille d'encodage.
2. La porte bascule `TPIDR_EL0` vers la TLS de Bionic avant d'appeler le C, et
   revient au bloc invité au retour. La valeur sauvée est rangée **dans le bloc
   invité lui-même**, pour que la bascule n'ait jamais besoin de TLS — sinon
   `__tls_get_addr` casserait pendant qu'on a changé son registre.

Le bloc invité devient donc : 8 slots de clés, puis la TLS de Bionic sauvée,
puis la pile hôte de la porte.

---

## Résultat

```
mnge-main  window attached (2590x1220)
mnge-vk    chosen format 37, colour space 0
mnge-vk    swapchain 2590x1220 fmt=37 images=8
mnge-vk    Vulkan ready: 2590x1220
mnge-nvn   guest pool at 0x750a182000, 64 KiB; guest stack 0x75069e7000
mnge-main  rendering 2590x1220
mnge-main  frame 121 / 241 / 361 …        (~120 fps)
```

Le triangle tourne, à la bonne orientation, et le passage arrière-plan →
premier plan (`cmd 2` puis `cmd 1`) reconstruit correctement la surface.

Toute la chaîne est donc validée sur du matériel réel : **code ARM64 invité →
gate `svc` → HLE → rejeu NVN → Vulkan → écran**, avec `x19`–`x28` et `q8`–`q15`
préservés à travers la gate (vérifié aussi hors appareil sous `qemu-aarch64`).

## Interrupteurs de diagnostic

Lisibles par `adb shell setprop` (l'app doit être `debuggable`) :

| Propriété | Effet |
|---|---|
| `debug.mnge.acqfence` | 1 (défaut) = acquisition par fence ; 0 = par sémaphore |
| `debug.mnge.clearonly` | saute les liaisons et le draw : ne garde que le clear |
| `debug.mnge.emptysubmit` | ne commence aucune passe de rendu |
| `debug.mnge.nopipe` | ne lie pas le pipeline |
| `debug.mnge.fmtidx` | force un des formats listés dans le log |
| `debug.mnge.idtransform` | force `preTransform = IDENTITY` |
| `debug.mnge.probe` | soumet un command buffer vide avant tout le reste |
