# Revue — confrontation au réel après le spike J1 (lens « versions / reality-check »)

- **Cible :** `ARCHITECTURE-SPINE.md` (J1–J2), statut final, mise à jour du 2026-10-16 (post-spike J1)
- **Date :** 2026-10-16
- **Méthode :**
  - exports de `risen_ori/bin/{Engine,Game,Script,SharedBase}.dll` via `objdump -p`, en séparant la **table d’exports** de la table d’imports (un nom présent dans plusieurs DLL n’est défini que dans une seule) ;
  - horodatages PE des DLL ;
  - API GitHub interrogée le jour même (`releases/latest`, `commits/master`) ;
  - versions des outils locaux (`clang-cl`, `xwin`, `cmake`) ;
  - preuves du dépôt : `docs/re/spike-j1.md`, `client/third_party/minhook/VERSION.txt`, `client/CMakeLists.txt`, `client/src/game/entity.{h,cpp}`, `client/src/sync/probe.cpp`, historique git (`d88d2e0`…`3760cf0`), `.memlog.md`, revue précédente `reviews/review-versions.md`.

## Verdict

**Tous les noms d’API cités dans AD-8 et AD-11 existent comme exports dans la DLL attendue, et toutes les versions de la Stack sont confirmées comme les dernières à ce jour. Deux affirmations « vérifiées / validées » vont toutefois au-delà des preuves : le chemin récepteur par nom (`GetMotion`) et le placement par `MoveTo` n’ont pas été exercés ou consignés en jeu.** Rien n’est périmé. Les corrections portent sur la formulation et la traçabilité, pas sur les choix.

## 1. Noms d’API du jeu (AD-2, AD-3, AD-8, AD-11)

Les statuts portent sur la **table d’exports** de la DLL.

| Nom dans la spine | Symbole décoré exporté | DLL | Statut |
| --- | --- | --- | --- |
| `eCApplication::OnProcess` | `?OnProcess@eCApplication@@UEAAXXZ` | Engine.dll | Existe. Une fois par frame, sur un seul thread : **vérifié en jeu** (spike, Q4) |
| `eCAnimation_PS::PlayMotion` | `?PlayMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAUeSMotionPlayBackDesc@3@_N@Z` | Engine.dll (importé par Game et Script) | Existe. Signature `(this, type, desc*, bool)` conforme à la spine et au désassemblage |
| `eCAnimation_PS::SetMotion` | `?SetMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAVeCMotionResource2@@@Z` | Engine.dll | Existe. Utilisé en jeu (sondes v2 à v5) |
| `eCResource2::GetFileTitleA` | `?GetFileTitleA@eCResource2@@QEBA?AVbCString@@XZ` | Engine.dll | Existe. **Retourne un `bCString` par valeur** (sret en RDX). Noms obtenus en jeu (sonde v2). Le suffixe `A` vient de la macro Windows `GetFileTitle` ; c’est bien le nom exporté |
| `eCResource2::GetMotion(nom)` | `?GetMotion@eCResource2@@SAPEAVeCMotionResource2@@AEBVbCString@@@Z` | Engine.dll (importé par Script) | Existe : statique, `const bCString&` en entrée, pointeur en sortie (pas de sret). **Jamais appelé par la sonde** (voir F1) |
| `Entity::Spawn(Template, pose, bool)` | `?Spawn@Entity@@SA?AV1@VTemplate@@AEBVbCMatrix@@_N@Z` | Script.dll | Existe. Il existe une seconde surcharge, `Spawn(Template, Entity, bool, gEDirection)`. En jeu, l’entité est créée et le joueur reste inchangé. Le `bool` appelle `SetTemporary` (désassemblage) |
| `Entity::MoveTo(bCMatrix)` | `?MoveTo@Entity@@QEAAXAEBVbCMatrix@@@Z` | Script.dll | Existe, avec la surcharge `MoveTo(Entity const&)`. **Effet en jeu non consigné** (voir F2) |
| `Entity::GetPlayer()` | `?GetPlayer@Entity@@SA?AV1@XZ` | Script.dll | Existe. Statique qui retourne un `Entity` **par valeur** (sret en RCX). Nul au menu principal : vérifié. Retour au menu : **non testé** (voir F3) |
| `Entity::SetPose` / `GetPose` / `Kill` / `GetTemplate` | `?SetPose@Entity@@QEAAXAEBVbCMatrix@@@Z`, `?GetPose@Entity@@QEBA?AVbCMatrix@@XZ`, `?Kill@Entity@@QEAAXXZ`, `?GetTemplate@Entity@@QEBA?AVTemplate@@XZ` | Script.dll | Existent |
| `gECombatMode`, `PSCombatSystem` (QO 5) | `?GetCombatMode@PSCombatSystem@@…`, `?GetCombatMode@gCCombatSystem_PS@@…` | Script.dll, Game.dll | Existent : la piste du spike J2 est réelle |
| « GUID d’entité » (AD-9, hypothèse) | `?GetID@eCEntity@@UEBAAEBVbCPropertyID@@XZ` (Engine), `?GetNetIndex@Entity@@QEBAKXZ` (Script) | Engine, Script | Il existe des candidats, mais aucun accesseur ne s’appelle « GUID ». C’est déjà marqué comme hypothèse : à nommer au spike J2 |
| `bCString` (construction / destruction) | `??0bCString@@QEAA@PEBD@Z`, `??1bCString@@QEAA@XZ` | SharedBase.dll | Existent. Nécessaires pour `GetMotion` et pour libérer le retour de `GetFileTitleA` |

**Cible « Steam x64, empreinte de janvier 2023 » : confirmée.** Horodatages PE : Engine 2023-01-20, Script 2023-01-19, Game 2023-01-19, SharedBase 2023-01-16.

## 2. Stack (vérifiée le 2026-10-16)

| Élément | Spine | Réel | Statut |
| --- | --- | --- | --- |
| C++ | 17 | `CMAKE_CXX_STANDARD 17`, `REQUIRED ON` (`client/CMakeLists.txt`) | Vérifié (projet) |
| clang-cl / lld-link | 23.1.3 | Dernière release GitHub : `llvmorg-23.1.3` (2026-10-06). Local : `clang version 23.1.3 (Fedora 23.1.3-1.fc45)` | Vérifié (web + local) |
| xwin | 0.10.0 | Dernière release : `0.10.0` (2026-08-12). Local : `xwin 0.10.0` | Vérifié (web + local) |
| CMake | ≥ 3.21 (4.3.0 local) | `cmake_minimum_required(VERSION 3.21)`. Local : 4.3.0. Dernière release : **v4.4.4** (2026-10-02) | Exact. Le local a une mineure de retard, sans effet |
| MinHook | 1.3.4 + correctifs de `master`, commit noté | Dernière release `v1.3.4` (2025-03-28). Tête de `master` = `8af6b4acae5a…` (2026-09-07), **identique** au commit de `VERSION.txt` | Vérifié (web + projet) : le commit figé est la tête actuelle |
| KCP (skywind3000) | 2.1.1 | Dernière release : `2.1.1` (2026-05-15). Pas encore vendorisé (`client/third_party/` ne contient que `minhook/`). Compilation clang-cl vérifiée dans `review-versions.md` | Vérifié (web). Vendorisation à faire en J1 |
| `ikcp_nodelay(1,10,2,1)`, `ikcp_setmtu(1199)` | AD-4 | « Turbo mode » du README de KCP. MTU par défaut de 1400, d’où le réglage explicite (revue précédente) | Vérifié (revue du 2026-10-15) |

## 3. Constats

### F1 — « Validé en local » couvre moins que la règle d’AD-8 [moyen]

AD-8 décrit le récepteur ainsi : `GetMotion(nom)` → `SetMotion` → `PlayMotion(type, **descripteur reconstruit**, flag)`, et conclut « Validé en local par le mode miroir ». Or le mode miroir (`client/src/sync/probe.cpp`, `replay_capture`) fait autre chose :

- il passe à `SetMotion` **le pointeur de ressource capturé chez le joueur** (`c.resource`) ;
- il ne résout aucun nom par `eCResource2::GetMotion`, qui n’est d’ailleurs résolu nulle part dans `client/src` ;
- il passe à `PlayMotion` une **copie brute des 0x60 octets** du descripteur, pointeurs compris.

Il en résulte que deux maillons, ceux qui comptent entre deux machines, n’ont jamais tourné :

- la recherche par nom, qui doit retourner une ressource valide même si elle n’est pas encore en cache chez l’invité ;
- le descripteur reconstruit à partir de scalaires (encore marqué `[ASSUMPTION]`).

On ignore aussi si `GetMotion` incrémente le compteur de références (`res + 0x24`) et qui doit le libérer.

**Correctif :** remplacer « Validé en local » par « Validé en local avec le même pointeur de ressource et le descripteur brut ; `GetMotion(nom)` et le descripteur reconstruit restent à valider ». Mieux encore, ajouter à la sonde (v6) un rejeu par nom (`GetFileTitleA` → `GetMotion` → `SetMotion`) avec un descripteur réduit aux scalaires, avant la story avatar.

### F2 — `Entity::MoveTo` adopté sans résultat en jeu consigné [moyen]

La spine écrit « À chaque frame, `Entity::MoveTo(bCMatrix)` place l’avatar ». Le message du commit `d88d2e0` (sonde v5) annonce des « résultats consignés dans docs/re/spike-j1.md ». Pourtant, les 11 lignes ajoutées par ce commit sont la section **v4**, qui conclut « la sonde v5 l’utilise ». Aucune section v5 n’existe : rien n’établit que `MoveTo` corrige le saut et l’équipement, ni qu’il ne pose pas d’autres problèmes (recalage sur le sol, contrôleur qui combat la pose imposée, coût par frame). Le seul fait connu est que `MoveTo` recale les property sets 0xd et 5 (désassemblage).

**Correctif :** consigner le résultat v5 dans `docs/re/spike-j1.md`. D’ici là, marquer la ligne « Placement » d’AD-8 `[ASSUMPTION : MoveTo corrige le saut, vérifié par la sonde v5]`, ou garder `SetPose` comme repli documenté.

### F3 — AD-11 s’appuie sur un « retour au menu » non testé [moyen]

AD-11 affirme que `GetPlayer` est nul au menu principal (vérifié) et qu’un changement du pointeur du joueur, en cas de chargement ou de retour au menu, met fin à la session. Le spike précise : « Le retour au menu n’a pas encore été testé. » Le chargement d’une sauvegarde en cours de partie n’a pas été testé non plus. Ces deux transitions conditionnent pourtant la règle « aucun pointeur d’entité réutilisé ». Si le moteur réutilisait la même adresse de `eCDynamicEntity` après un rechargement, la détection par changement de pointeur ne verrait rien.

**Correctif :** ajouter à la story session, ou à une sonde, un test explicite « retour au menu » et « chargement d’une sauvegarde en partie », en journalisant le pointeur. Prévoir aussi un second signal, par exemple `gCSession::AccessState()`, cité par le spike.

### F4 — L’ABI des objets retournés n’est pas couverte par AD-3 [faible]

`GetFileTitleA` retourne un `bCString` **par valeur**. L’appelant fournit le sret, puis doit le **détruire** (`??1bCString@@QEAA@XZ`, SharedBase.dll). `GetMotion` attend un `bCString` construit par l’appelant (`??0bCString@@QEAA@PEBD@Z`). Le code de la sonde le fait correctement (`entity.cpp` résout `bcstring_dtor`), mais AD-3 n’énonce que la règle inverse, pour les arguments (« détruite par l’appelé »). C’est une source probable de fuite, ou de double libération, dans le futur `game/animation`.

**Correctif :** ajouter à AD-3 une phrase : « Un objet **retourné** par valeur (sret) appartient à l’appelant, qui le détruit. » Ajouter aussi SharedBase.dll (ctor et dtor de `bCString`) à la liste des symboles résolus en bloc.

### F5 — Précisions mineures [info]

- `Entity::GetPlayer()` « non nul » : la fonction retourne un `Entity` par valeur, c’est-à-dire une enveloppe de 8 octets. C’est son `eCDynamicEntity*` interne qui est nul. Le code le traite bien ainsi (`get_player`, `out.ptr == nullptr`). Préciser la formulation éviterait une erreur de lecture.
- Exception d’AD-2 pour le hook `PlayMotion` : le thread d’appel de `PlayMotion` n’est pas mesuré explicitement dans le spike. Seul un commentaire de `probe.cpp` le suppose égal au thread principal. Ce n’est pas bloquant, puisque le hook se contente de copier sous verrou.
- Le spike laissait ouverte la question « `eCMotionResource2` hérite-t-il de `eCResource2` sans décalage ? ». Elle est tranchée dans `entity.h` (même compteur de références à `+0x24`) et par le succès de `GetFileTitleA` en jeu, mais `spike-j1.md` ne la marque pas comme résolue.
- CMake 4.4.4 est sorti ; le local est en 4.3.0. Sans effet (minimum 3.21).

## 4. Ce qui tient

- Toutes les fonctions du jeu nommées dans la spine existent, dans la bonne DLL, avec la signature annoncée.
- Les quatre versions d’outils et de bibliothèques sont les dernières publiées au 2026-10-16. Le commit MinHook figé est la tête de `master`.
- Sont vérifiés en jeu : cadence et thread de `OnProcess`, repère `bCMatrix` (lignes, Y vertical, cm, axe avant en ligne 2), `Spawn` temporaire sans prise de contrôle, noms d’animation stables, mode miroir fluide (avec le pointeur partagé).
