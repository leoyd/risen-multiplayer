# Revue — versions et vérification des faits (lens « versions / reality-check »)

- **Cible :** `ARCHITECTURE-SPINE.md` (J1–J2), statut draft, 2026-10-15
- **Méthode :** API GitHub (releases, tags, compare), sources téléchargées (MinHook v1.3.4, KCP 2.1.1) et **compilées localement** avec clang-cl + xwin, versions des outils locaux, `objdump -p` + `llvm-undname` sur `risen_ori/bin/{Script,Engine,Game}.dll`, lecture du code J0 (`client/`).

## Verdict

**Les versions de la stack sont exactes et à jour. Un engagement de conception (AD-8) repose sur une lecture erronée des exports. Il manque aussi deux précisions vérifiables (MTU de KCP, ABI des arguments passés par valeur).** Ces points sont à corriger avant les stories J1. Le reste a été confronté au réel et tient.

## Tableau de vérification

| Élément de la spine | Constat | Source | Statut |
| --- | --- | --- | --- |
| MinHook 1.3.4 | Dernière release (2025-03-28). Le `master` a **17 commits d’avance**, dont des correctifs de justesse : opérandes JMP/JCC relatifs signés (trampoline à offset négatif, nov. 2025), HDE64 réécrit et borné (sept. 2026), contrôle `VirtualQuery` manquant | API GitHub `compare/v1.3.4...master` | OK, voir F4 |
| MinHook x64 + clang-cl | `buffer.c`, `hook.c`, `trampoline.c`, `hde64.c` compilent avec `clang-cl --target=x86_64-pc-windows-msvc /MT /W3` et xwin : **0 erreur, 0 warning** ; `llvm-lib` OK. Le `CMakeLists.txt` de MinHook déclare `3.0...3.5` (accepté par CMake 4.x) et affiche encore `1.3.3` comme version interne | build local | Vérifié |
| KCP skywind3000 2.1.1 | Dernière release (2026-05-15). **2.0.0 (2026-05-14) a introduit un contrôle de congestion enfichable** (`ikcp_setcc`). La 2.1.1 ajoute un pacing optionnel et des callbacks `on_ack`/`on_pkt_sent`. `ikcp.c` compile sans warning avec clang-cl | API GitHub + build local | OK, voir F3 |
| `ikcp_nodelay(1,10,2,1)` = mode rapide | Confirmé : le README.en indique « Turbo Mode: `ikcp_nodelay(kcp, 1, 10, 2, 1)` ». Le commentaire de `ikcp.h` dit `fastest: (1, 20, 2, 1)` ; l’écart est sans importance. L’intervalle est borné à [10, 5000] et le RTO min passe à 30 ms en nodelay | README.en.md, ikcp.h/ikcp.c 2.1.1 | Vérifié |
| xwin 0.10.0 | Dernière release (2026-08-12). `~/.local/bin/xwin --version` → `xwin 0.10.0`. `~/.xwin/{crt,sdk}` présents | API GitHub + local | Vérifié |
| clang-cl / lld-link 23.1.3 | Dernière release LLVM (`llvmorg-23.1.3`, 2026-10-06). Local : `clang version 23.1.3 (Fedora 23.1.3-1.fc45)`, `LLD 23.1.3` | API GitHub + local | Vérifié |
| CMake ≥ 3.21 (4.3.0 local) | `client/CMakeLists.txt` déclare bien `3.21`. Local : `cmake 4.3.0`, ninja 1.13.2. **Attention :** le `CMakeLists.txt` de KCP exige `VERSION 4.0` depuis la 1.7.1 | local + sources KCP | OK, voir F5 |
| C++17 | Aucun conflit (MinHook et KCP sont en C) | — | OK |
| Cible « Steam x64, empreinte janvier 2023 » | `Game.dll` 2023-01-19 / `0x1417000`, `Engine.dll` 2023-01-20 / `0xd3a000` : conformes à `core/version.cpp`. **`Script.dll` (2023-01-19, `0x4d1000`) n’est pas dans la liste blanche**, alors que `game/` appellera surtout cette DLL | `objdump -p`, `client/src/core/version.cpp` | Écart, voir F6 |
| Exports `Script.dll` : `Entity::Spawn/GetPlayer/GetPose/SetPose/MoveTo/StartPlayAni/GetAni/Kill` | **Tous présents** (décorés, voir annexe). Mais la sémantique de `GetAni` ne correspond pas à l’usage prévu (F1), et `Spawn`/`StartPlayAni` prennent des classes **par valeur** (F2) | `objdump -p` + `llvm-undname` | Présents, sémantique à revoir |
| `Engine.dll` : `eCApplication::OnProcess` | Présent : `?OnProcess@eCApplication@@UEAAXXZ` = `public: virtual void eCApplication::OnProcess(void)`. Rien ne prouve encore qu’il est appelé **une fois par frame sur le thread principal** : c’est déduit du nom | `objdump -p` | Présent, comportement non vérifié (F7) |

## Constats

### F1 — [Haute] AD-8 : `Entity::GetAni` ne lit pas l’animation en cours

Signature démanglée :

```
public: class bCString Entity::GetAni(bCString const&, bCString const&, gEDirection, bool, gECombatPose, gEAniState) const
```

Il s’agit d’un **constructeur de nom** d’animation à partir de paramètres (mode, direction, posture, état). Ce n’est pas un accesseur de l’animation qui joue. Les surcharges `GetAniEx`, `GetHoldAni` et `GetOverlayAni` ont la même forme. Aucun export `Script.dll`, `Game.dll` ou `Engine.dll` ne répond à `GetCurrentAni`, `PlayingAni`, `ActiveAni` ou `GetAniName` sur une entité. On trouve seulement `GetCurrentAniDirection`, `GetMovementMode`, `RevertPlayingAni`, et `gCScriptProcessingUnit::GetAniName` (protégé). La phrase d’AD-8, « l’émetteur lit le nom de l’animation en cours via l’API `Entity` », est donc une hypothèse présentée comme un fait. Le memlog (« GetAni ») la consigne comme un *event* de reconnaissance, sans vérification de la sémantique.

**Action :** marquer AD-8 `[ASSUMPTION]` et ajouter un spike RE au début de J1, sur le modèle des deux spikes déjà dans *Deferred*. Deux pistes :
- lire la motion active côté Engine (`eCWrapper_emfx3SkeletalMotion` / `eSMotionDesc` / `eCMotionResource2`) ;
- ou promouvoir le repli « synchro par état » en solution primaire. Les ingrédients sont exportés : `GetMovementMode`, `GetCurrentAniDirection`, et `GetAni(...)` pour reconstruire le nom chez le récepteur.

### F2 — [Haute] AD-3 : la forme d’appel ne couvre pas les classes passées par valeur

```
public: static class Entity Entity::Spawn(class Template, class bCMatrix const&, bool)
public: void Entity::StartPlayAni(class bCString, int, bool, float, bool, eAnimShared::eEMotionType)
```

`Template` et `bCString` sont passés **par valeur**. Dans l’ABI MSVC x64, un objet non trivial ou de plus de 8 octets est passé par pointeur vers une **copie temporaire construite par l’appelant**, et c’est **l’appelé qui la détruit**. AD-3 traite les retours (`sret`), mais pas ces arguments. Un `bCString` construit puis détruit côté mod après l’appel provoque un double free.

**Action :** compléter AD-3 :
- argument de classe par valeur = l’appelant construit la copie avec le constructeur de copie exporté du jeu, passe son adresse, et **ne la détruit pas** ;
- vérifier `sizeof(Template)` et `sizeof(bCString)` au spike J1.

### F3 — [Moyenne] AD-4 : le MTU par défaut de KCP (1400) dépasse le plafond de 1200 octets

`IKCP_MTU_DEF = 1400` (`ikcp.c` 2.1.1). Avec l’octet de canal en tête (AD-4), un segment KCP plein fait 1401 octets, au-delà de la règle « 1200 octets au plus ».

**Action :** écrire dans AD-4 `ikcp_setmtu(kcp, 1199)`, ou 1200 − 1, et mentionner l’overhead KCP de 24 octets. Préciser aussi `ikcp_wndsize` si on le change (défauts : snd 32 / rcv 128).

Point annexe : avec KCP ≥ 2.0, `nc=1` désactive toujours la fenêtre de congestion (`kcp->nocwnd`). Ne pas installer de `IKCPOPS` via `ikcp_setcc`. Le pacing de la 2.1.1 est optionnel et inactif par défaut. Rien à changer, mais à noter dans `third_party/kcp/VERSION`.

### F4 — [Moyenne] MinHook : figer un commit plutôt que le tag v1.3.4

v1.3.4 est bien la dernière release. Mais depuis, `master` corrige des bugs qui touchent directement le hook d’un prologue x64 :
- opérandes de saut relatifs stockés en non signé (mauvais trampoline si l’offset est négatif) ;
- décodeur HDE64 qui lisait au-delà de 15 octets et décodait mal certains SIB/préfixes.

**Action :** vendoriser un commit de `master` daté et noté (≥ 2026-09-07), ou garder 1.3.4 et inscrire ce risque. Dans les deux cas, inscrire le hash dans `third_party/minhook/`.

### F5 — [Basse] « CMake ≥ 3.21 » n’est vrai que si KCP n’est pas intégré par `add_subdirectory`

Le `CMakeLists.txt` de KCP impose `CMAKE_MINIMUM_REQUIRED(VERSION 4.0)` (release 1.7.1).

**Action :** préciser dans *Consistency Conventions → Tiers* que les deux bibliothèques sont compilées **source par source** dans la cible du client. Leurs propres `CMakeLists.txt` ne sont pas utilisés. Ainsi, le minimum 3.21 reste vrai.

### F6 — [Basse] `Script.dll` absent de la liste blanche d’empreintes

L’adaptateur `game/` résoudra presque tout dans `Script.dll`, mais J0 ne vérifie que `Game.dll` et `Engine.dll`.

**Action :** ajouter `Script.dll` (`TimeDateStamp 0x63c9b014`, `SizeOfImage 0x4d1000`, relus dans l’en-tête PE) à la liste blanche, ou justifier que la mise à jour est atomique. La vérification « tout ou rien » d’AD-3 compense en partie, car un symbole absent désactive le mod. Mais un symbole présent avec un autre layout ne serait pas détecté.

### F7 — [Basse] `eCApplication::OnProcess` « une fois par frame » n’est pas vérifié

L’export existe et il est virtuel. MinHook sur son adresse exportée intercepte bien les appels virtuels, puisque le corps est patché. En revanche, la cadence (par frame ? plusieurs fois ? aussi pendant les menus et les chargements ?) et le thread appelant sont **déduits du nom**. AD-2 en dépend entièrement.

**Action :** premier critère d’acceptation de J1 : journaliser `GetCurrentThreadId()` et le nombre d’appels par seconde du hook, en jeu, en menu et en chargement.

## Ce qui est confirmé sans réserve

- Les versions MinHook 1.3.4, KCP 2.1.1, xwin 0.10.0 et LLVM 23.1.3 sont **les dernières publiées au 2026-10-15**, et ce sont celles installées ou visées.
- La chaîne clang-cl 23.1.3 + lld-link + xwin 0.10.0 compile MinHook et KCP sans modification.
- `ikcp_nodelay(1,10,2,1)` est bien le « Turbo Mode » documenté.
- Les huit méthodes `Entity` nommées et `eCApplication::OnProcess` sont exportées sous des noms décorés stables.

## Annexe — noms décorés relevés

```
Script.dll
?Spawn@Entity@@SA?AV1@VTemplate@@AEBVbCMatrix@@_N@Z
?Spawn@Entity@@SA?AV1@VTemplate@@V1@_NW4gEDirection@@@Z
?GetPlayer@Entity@@SA?AV1@XZ
?GetPose@Entity@@QEBA?AVbCMatrix@@XZ
?SetPose@Entity@@QEAAXAEBVbCMatrix@@@Z
?MoveTo@Entity@@QEAAXAEBV1@@Z
?MoveTo@Entity@@QEAAXAEBVbCMatrix@@@Z
?StartPlayAni@Entity@@QEAAXVbCString@@H_NM1W4eEMotionType@eAnimShared@@@Z
?StartPlayAniEx@Entity@@QEAAXAEBVbCString@@H_N1M1@Z
?StopPlayAni@Entity@@QEAAXXZ
?GetAni@Entity@@QEBA?AVbCString@@AEBV2@0W4gEDirection@@_NW4gECombatPose@@W4gEAniState@@@Z
?GetAni@Entity@@QEBA?AVbCString@@AEBV2@W4gEItemUseType@@10W4gEDirection@@_NW4gECombatPose@@W4gEAniState@@3@Z
?GetMovementMode@Entity@@QEBA?AW4gECharMovementMode@@XZ
?GetCurrentAniDirection@Entity@@QEBA?AW4gEDirection@@XZ
?Kill@Entity@@QEAAXXZ
?DoDamage@gCDamage_PS@@QEAA_NPEAVeCEntity@@@Z        (piste pour la fonction de dégâts J2)
Engine.dll
?OnProcess@eCApplication@@UEAAXXZ
```
