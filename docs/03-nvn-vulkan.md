# 03 — Le backend NVN : de Metal 4 à Vulkan

Le fichier `gfx/nvn/nvn_mtl.m` fait 13 696 lignes. C'est la totalité du travail
difficile du portage. Ce document décrit la couture, la correspondance des API
et l'ordre dans lequel réécrire.

---

## 1. Où se situe exactement la couture

```
  code du jeu (ARM64)
        │  nvnCommandBufferBeginRecording / BindVertexBuffer / DrawArrays / …
        ▼
  hle_nvn.c  (2 727 lignes, 575 points d'entrée)      ← portable tel quel
        │  mtl_sub_begin / mtl_pool_add / mtl_addr / mtl_sub_commit …
        ▼
  nvn_mtl.m  (13 696 lignes)                          ← à réécrire
        │  MTL4CommandBuffer, MTL4RenderCommandEncoder, MTLTexture …
        ▼
  Metal 4
```

`hle_nvn.c` ne connaît **que** les fonctions `mtl_*`. C'est la bonne nouvelle :
on peut réécrire le backend derrière une interface stable, et faire tourner les
deux implémentations côte à côte le temps de la transition.

Les fonctions de la couture, par importance :

| Fonction | Rôle |
|---|---|
| `mtl_pool_add` / `mtl_pool_del` / `mtl_addr` | enregistrer un pool mémoire et traduire une adresse GPU en ressource |
| `mtl_sub_begin` / `mtl_sub_commit` | ouvrir et clore une soumission, renvoyer la valeur de fence |
| `mtl_sub_copy` | copie buffer→buffer ordonnée GPU |
| `mtl_sub_event` / `mtl_sub_counter` | écrire une valeur **après** complétion GPU |
| `mtl_present` / `mtl_present_wait` | présenter, attendre le vsync |
| `mtl_prog_release`, `mtl_tex_release`, `mtl_dead_forget` | cycle de vie des ressources |
| `mtl_*_stats` (~30) | instrumentation, à garder |

La convention de ordonnancement est explicite dans l'en-tête d'origine : *tout
effet visible d'une complétion GPU n'a lieu qu'après la complétion réelle du
command buffer*. En Vulkan cela se traduit par : `VkFence` + sémaphores, jamais
par une écriture CPU optimiste.

---

## 2. Correspondance des API

### Objets

| Metal 4 | Vulkan | Remarque |
|---|---|---|
| `MTLDevice` | `VkPhysicalDevice` + `VkDevice` | |
| `MTLCommandQueue` | `VkQueue` | |
| `MTL4CommandAllocator` | `VkCommandPool` | |
| `MTL4CommandBuffer` | `VkCommandBuffer` | |
| `MTL4RenderCommandEncoder` | `VkRenderPass` + `VkFramebuffer` + `vkCmd*` | ou *dynamic rendering* (1.3) |
| `MTL4ComputeCommandEncoder` | `vkCmdDispatch` + `VkPipeline` compute | |
| `MTLBlitCommandEncoder` | `vkCmdCopyBuffer/Image`, `vkCmdBlitImage` | |
| `MTL4ArgumentTable` | `VkDescriptorSet` | `VK_EXT_descriptor_indexing` pour les tableaux non bornés |
| `MTLResidencySet` | **rien** | à remplacer par une gestion `VkDeviceMemory` + `VK_EXT_memory_budget` |
| `MTLSharedEvent` | `VkSemaphore` *timeline* (Vulkan 1.2) | |
| `MTLFence` | `vkCmdPipelineBarrier` | |
| `MTLBuffer` | `VkBuffer` + `VkDeviceMemory` | |
| `MTLTexture` | `VkImage` + `VkImageView` | |
| `MTLSamplerState` | `VkSampler` | |
| `MTLDepthStencilState` | `VkPipelineDepthStencilStateCreateInfo` | |
| `MTLRenderPipelineState` | `VkPipeline` | |
| `MTLComputePipelineState` | `VkPipeline` (compute) | |
| `MTLFunction` / `MTLLibrary` | `VkShaderModule` (SPIR-V) | |
| `MTL4CommitFeedback` | `VkQueryPool` + timestamps | |
| MetalFX (2K, interpolation) | FSR1 + upscale bilinéaire | pas d'équivalent |

### Formats de pixels

Correspondance directe, sauf :

* `MTLPixelFormatDepth32Float` → `VK_FORMAT_D32_SFLOAT` ;
* les formats `sRGB` doivent être déclarés dès la création de la vue
  (`VK_FORMAT_*_SRGB`) — Metal permet une vue sRGB sur une texture linéaire,
  Vulkan non. Le projet utilise déjà `nvn_probe_srgb_view` pour ce cas ; sur
  Vulkan il faut **deux `VkImageView`** sur la même `VkImage`
  (`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` + `VkImageViewUsageCreateInfo`) ou une
  image miroir.
* BC1/BC3/BC7 : voir `02-portage-android.md` §3.5 (transcodage ASTC sur Mali).

### Modes de stockage

| Metal | Vulkan |
|---|---|
| `MTLStorageModeShared` | mémoire `HOST_VISIBLE` (+ `HOST_COHERENT`) |
| `MTLStorageModePrivate` | mémoire `DEVICE_LOCAL` |
| `newBufferWithBytesNoCopy` | `VK_EXT_external_memory_host` (§3.4 du doc 02) |
| `MTL4VisibilityOptionResourceAlias` | `VkMemoryBarrier` avec `VK_ACCESS_*` — l'alias de deux buffers sur une allocation n'est pas rendu visible par une barrière de seule visibilité device |

---

## 3. Ce que Vulkan rend plus dur

### 3.1 Les layouts d'image

Metal n'a pas de layouts. Vulkan en exige, et une transition manquante est un
plantage ou une corruption silencieuse. C'est **la** source de bugs du portage.

Approche recommandée : ne pas gérer les layouts à la main. Dériver tout du
`VkRenderPass` (les dépendances de sous-passe font les transitions), et
n'utiliser des barrières explicites que pour les ressources échantillonnées
entre deux passes. Le projet a déjà `nvn_probe_*` pour chaque chemin isolé :
c'est exactement le bon outillage pour valider ces transitions une par une.

### 3.2 Les passes de rendu

`nvn_mtl.m` ouvre un encoder par groupe de draws partageant les mêmes cibles.
En Vulkan, chaque `VkFramebuffer`/`VkRenderPass` doit être compatible. Il faut
donc **regrouper les draws par ensemble de cibles**, ce qui est déjà la
structure du flux NVN (BeginRenderPass/EndRenderPass) — mais l'implémentation
Metal est plus permissive sur les changements de cibles en cours d'encoder.

### 3.3 La synchronisation CPU/GPU

Metal 4 avec `MTLSharedEvent` est très bon marché. Les sémaphores *timeline*
Vulkan (1.2) sont l'équivalent le plus proche et doivent être préférés aux
sémaphores binaires. Vérifier `VK_KHR_timeline_semaphore` (ou Vulkan ≥ 1.2,
disponible sur Android 12+ en pratique).

---

## 4. Structure proposée pour `nvn_vk.c`

```
nvn_vk.c
├── device        instance/device/queue, VK_KHR_android_surface, swapchain
├── pool          mtl_pool_add/del/addr  → VkBuffer aliasé + table de segments
├── resource      MTLBuffer/MTLTexture   → VkBuffer/VkImage + cache par handle NVN
├── format        MTLPixelFormat         → VkFormat (+ vues sRGB)
├── sampler       MTLSamplerState        → VkSampler (cache)
├── pipeline      MTLRenderPipelineState → VkPipeline (cache par clé NVN)
├── pass          render pass / framebuffer à la volée (cache LRU)
├── encode        le rejeu des commandes : draws, clears, copies, dispatch
├── submit        mtl_sub_begin/commit, fences, timeline semaphores
└── present       swapchain, vsync, resize
```

Ordre d'implémentation conseillé (chaque étape est testable seule grâce aux
sondes `nvn_probe_*` existantes) :

1. device + swapchain + `mtl_present` → écran noir qui ne plante pas ;
2. `mtl_pool_add` + `mtl_addr` → pools résolus ;
3. buffers + `mtl_sub_copy` → copie GPU correcte ;
4. pipeline + draws sans texture → premier triangle venu du flux NVN ;
5. textures 2D + samplers → premier décor ;
6. cibles de rendu multiples + MRT → ombres, réflexions ;
7. depth/stencil + clear exact (le projet a une sonde dédiée : `probe_depth16_clear`) ;
8. compute + storage buffers → effets ;
9. alias de pools + barrières d'alias → dernier gros morceau.

---

## 5. Le code de ce dépôt

`app/cpp/vk.c` implémente les étapes 1 et une partie de 4 : device, swapchain,
passe de rendu, pipeline depuis SPIR-V, buffers, descripteurs, soumission,
présentation, et la gestion `suspend`/`resume` du cycle de vie Android.

`app/cpp/nvn.c` montre la couture côté NVN avec cinq commandes
(`NVN_CLEAR_COLOR`, `NVN_SET_UBO`, `NVN_BIND_VB`, `NVN_DRAW`, `NVN_VIEWPORT`) et
le rejeu vers `vk_cmd_*`. C'est le gabarit dans lequel `hle_nvn.c` viendra se
brancher.

Ce qui n'est **pas** là : les textures, les formats, les render passes
multi-cibles, le depth, le compute, l'alias de pools, le cache de pipelines.
C'est-à-dire l'essentiel des 13 696 lignes.
