---
title: 'Spike J1 — DLL sonde : boucle par frame, capture des animations, apparition d’une copie du joueur'
type: 'feature'
created: '2026-10-15'
baseline_commit: '3760cf08052205e8ebc2ca425aaa31565fbdae55'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/planning-artifacts/architecture/architecture-risen-multiplayer-2026-10-15/ARCHITECTURE-SPINE.md'
  - '{project-root}/docs/re/spike-j1.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Quatre inconnues bloquent J1 et ne se tranchent qu’en jeu (`docs/re/spike-j1.md`) : `eCApplication::OnProcess` est-il un tick par frame sur le thread principal ? Comment détecter qu’un monde est chargé ? Quelle est la disposition de `eSMotionPlayBackDesc` (lecture de l’animation courante, AD-8) ? Une copie du joueur créée par `Entity::Spawn` se comporte-t-elle comme un avatar ?

**Approach:** Ajouter à `Script_Multiplayer.dll` un mode sonde, activé par `probe = 1` dans `multiplayer.ini` et sans aucun effet quand il est à 0. Il installe deux hooks MinHook (`OnProcess`, `PlayMotion`) qui journalisent ce qu’il faut pour répondre. F9 fait apparaître une copie du joueur, F10 lui fait rejouer la dernière animation capturée. On en profite pour migrer la structure J0 vers celle de la spine.

## Boundaries & Constraints

**Always :**
- Respecter la spine :
  - AD-2 : appels au jeu uniquement depuis le hook `OnProcess`. Le hook `PlayMotion` se contente de copier des octets.
  - AD-3 : sret explicite ; une classe passée par valeur est passée par pointeur vers une copie, et c’est l’appelé qui la détruit ; SEH autour de chaque appel.
  - Le code est rangé selon les couches de la spine.
- `probe = 0`, valeur par défaut ou clé absente : aucun hook installé, comportement identique à J0.
- Les journaux de la sonde sont limités en débit, pour ne jamais ralentir le jeu de façon visible.
- MinHook est vendorisé et figé sur un commit dont le hash est noté.

**Never :**
- Pas de réseau ni de session : c’est la suite de J1.
- Aucun appel au jeu ailleurs que dans le hook `OnProcess` (hors `ScriptInit`, qui installe les hooks).
- Ne pas laisser la copie du joueur prendre le contrôle du joueur. Si cela arrive, c’est un **résultat à journaliser**, pas un bug à masquer.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Gestion d’erreur |
|---|---|---|---|
| Sonde désactivée | `probe` absent ou `0` | Aucun hook, log identique à J0 + « sonde désactivée » | N/A |
| Tick | `probe = 1`, menu puis partie chargée | Chaque seconde : thread (identique au 1er appel ?), images/s, joueur valide oui/non, pose du joueur | Si la lecture de la pose échoue (SEH) : « pose illisible » |
| Animation | Le joueur marche, court, saute, frappe | Une ligne par `PlayMotion` du joueur : type et dump hexa des 0x60 octets du descripteur. Au plus 20 lignes/s | Descripteur nul : journalisé tel quel |
| F9 | Partie chargée | Copie du joueur à environ 2 m devant lui ; log du résultat et de la pose | Pas de joueur, ou échec de `Spawn` : log, rien d’autre |
| F10 | Une copie existe et une animation a été capturée | `PlayMotion` rejoué sur l’`eCAnimation_PS` de la copie | Pas de copie ou pas d’animation : log |
| Hook impossible | Symbole absent ou échec de MinHook | Log, sonde inactive, jeu intact | Silencieux en jeu |

</frozen-after-approval>

## Code Map

Faits établis par désassemblage (Steam x64) :
- `Engine.dll` `?OnProcess@eCApplication@@UEAAXXZ` @ `0x1804c0730` : `void(eCApplication* this)`.
- `Engine.dll` `?PlayMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAUeSMotionPlayBackDesc@3@_N@Z` @ `0x1804d4f10` : `void(eCAnimation_PS* this, int type /*0..7*/, eSMotionPlayBackDesc* desc, bool)`.
- `Engine.dll` `?GetPropertySet@eCEntity@@UEAAPEAVeCEntityPropertySet@@W4eEPropertySetType@@@Z` : `void*(eCEntity*, int type)`. Animation = **`0x52`** (`eCAnimation_PS::GetPropertySetType` retourne `0x52`).
- `Script.dll` `Entity` = **8 octets**, `{ eCEntity* }`. Constructeur `{nullptr}` et destructeur vides.
  - `?GetPlayer@Entity@@SA?AV1@XZ` : `Entity*(Entity* sret)`.
  - `?GetPose@Entity@@QEBA?AVbCMatrix@@XZ` : `bCMatrix*(const Entity*, bCMatrix* sret)`.
  - `?GetTemplate@Entity@@QEBA?AVTemplate@@XZ` : `Template*(const Entity*, Template* sret)`.
  - `?Spawn@Entity@@SA?AV1@VTemplate@@AEBVbCMatrix@@_N@Z` : `Entity*(Entity* sret, Template* tmp_passée_et_détruite_par_l_appelé, const bCMatrix*, bool)`.
- `Template` = 8 octets (enveloppe de `eCTemplateEntityProxy`, à compteur de références). Le destructeur `??1Template@@QEAA@XZ` est à appeler sur la copie **retournée** par `GetTemplate` si elle n’est pas passée à `Spawn`.
- `bCMatrix` : 4×4 `float` (64 octets). `[ASSUMPTION]` La translation est aux indices 12..14 et l’axe avant sur la ligne 2 ; les unités sont des centimètres (2 m = 200). Journaliser la matrice brute pour le vérifier.
- Code J0 : `client/src/win/{script_init,game_abi,log}.cpp`. Il faut migrer `game_abi` vers `client/src/game/`, `log` vers `client/src/common/`, et `mp::abi` vers `mp::game` (convention de la spine).
- Config : `client/src/core/config.{h,cpp}` reçoit la nouvelle clé `probe` (0/1, défaut 0) ; ajout dans `multiplayer.example.ini`.

## Tasks & Acceptance

**Execution :**
- [x] `client/third_party/minhook/` -- sources MinHook à un commit figé (≥ 2026-09-07) + `VERSION.txt` (hash, date, licence) -- AD-3, convention « Tiers »
- [x] `client/src/common/log.*`, `client/src/game/abi.*` -- migration du code J0 (aucun changement de comportement) -- conventions de la spine
- [x] `client/src/core/config.*` + tests -- clé `probe` (0/1, défaut 0, valeur invalide = erreur nommant le champ) -- matrice
- [x] `client/src/game/entity.*` -- enveloppes `GetPlayer`, `GetPose`, `GetTemplate`, `Spawn`, `GetPropertySet(0x52)`, `PlayMotion`, toutes protégées par SEH -- AD-3
- [x] `client/src/win/hooks.*` -- installation et retrait des hooks MinHook `OnProcess` et `PlayMotion` -- AD-2
- [x] `client/src/sync/probe.*` -- tick par seconde, capture des animations (copie d’octets dans un tampon, filtrée sur l’`eCAnimation_PS` du joueur, mise à jour à chaque tick), F9/F10 en lisant `GetAsyncKeyState` depuis le tick -- matrice
- [x] `client/src/win/script_init.cpp` -- si mod actif et `probe = 1`, installer les hooks après `build_descriptor` -- matrice
- [x] `client/tests/wine/run_scenarios.sh` -- scénario `probe = 0` (log « sonde désactivée ») et `probe = 1` (log « hooks installés » ou erreur explicite) -- non-régression
- [x] `client/README.md` + `docs/re/spike-j1.md` -- comment activer la sonde et quoi rapporter -- utilisabilité

**Acceptance Criteria :**
- Given `probe = 0`, when on joue, then le comportement et le log sont ceux de J0, plus la ligne « sonde désactivée ».
- Given `probe = 1` sous Proton, when on charge une partie et qu’on joue 2 minutes, then le jeu reste fluide, sans crash, et le log contient les lignes de tick, d’animation, et les résultats de F9/F10.

## Implementation Notes

- ABI revérifiée au désassembleur le 2026-10-16 (REA/Hopper : bridge timeout, repli sur `objdump -d`) ; détails dans `docs/re/spike-j1.md`.
- `GetPropertySet` : la sonde appelle la variante const non virtuelle `?GetPropertySet@eCEntity@@QEBA…W4eEPropertySetType@@@Z`, qui saute elle-même dans la vtable (`+0x198`) : bon dispatch même si une sous-classe redéfinit la méthode.
- `Spawn` : le `bool` appelle `eCEntity::SetTemporary` ; la sonde passe `true` (copie non sauvegardée).
- Capture `PlayMotion` : en plus du descripteur, on lit la ressource de motion du PS (`this + 0x120 + type*0x10`), car `PlayMotion` la prend là et non dans le descripteur. Simple lecture mémoire protégée, aucun appel au jeu dans le hook.
- Journalisation des animations faite depuis le tick (anneau de 64 captures sous SRWLOCK) : le hook ne fait que copier. Surplus au-delà de 20 lignes/s compté et signalé.
- F9/F10 : actifs seulement si la fenêtre au premier plan appartient au processus du jeu (évite les déclenchements depuis une autre application).
- SEH sous clang-cl : `__try` ne protège que les appels ; la copie mémoire passe par une fonction `noinline`.
- Touches : **F6/F7** au lieu de F9/F10 (demande de l’utilisateur le 2026-10-16 : F9/F10 servent déjà dans Risen, ex. chargement rapide). Partout ailleurs dans cette spec, lire F9 = F6 et F10 = F7.
- `Script.dll` : `GetModuleHandle`, sinon `LoadLibrary` (cas du banc Wine, qui ne la charge pas).
- Banc Wine : 3 scénarios ajoutés (probe absent/0, probe = 1 avec hooks réellement posés sur les DLL du jeu, jeu sans `Script.dll` → « sonde inactive : symbole introuvable »). Les lignes Tick / Animation / F9 / F10 de la matrice ne se vérifient qu’en jeu (2ᵉ critère d’acceptation, vérification manuelle).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge, gap | `g_copy` pendant après chargement/menu → F10 sur mémoire libérée | medium | patch | Rien ne remettait `g_copy` à zéro ; corrigé : oubli de la copie dès que le pointeur du joueur change. |
| 2 | blind, edge | F9 répété : copies orphelines | low | patch | Chaque F9 appelait `Spawn` ; corrigé : refus tant qu’une copie est suivie. |
| 3 | edge | Échec SEH de `GetPlayer` après `Spawn` journalisé comme « CHANGÉ » | low | patch | Retour de `get_player` ignoré ; corrigé. |
| 4 | blind | `probe = 1` ignoré sans message si mod inactif | low | patch | `return` silencieux ; ajout de « sonde ignorée : mod inactif ». |
| 5 | blind | Débordement d’anneau et limite de débit confondus | low | patch | `dropped` ajouté à `g_anim_skipped` ; compteurs séparés. |
| 6 | edge | `remove()` remettait les originaux à nul | low | patch | Un détour encore actif après retrait partiel aurait appelé nul ; pointeurs conservés. |
| 7 | edge | Touche tenue au retour du focus → déclenchement | low | patch | État forcé à `false` hors focus ; on suit désormais l’état réel. |
| 8 | gap | Logique pure de la sonde (anneau, débit, hexa) non testée | medium | defer | Code compilé seulement dans la DLL ; à déplacer dans `core/` si conservé après le spike. |
| 9 | gap | Détours et enveloppes ABI vérifiés seulement en jeu | medium | defer | Le banc n’appelle jamais `OnProcess` ; nécessite un monde chargé. Couvert par le 2ᵉ critère (manuel). |
| 10 | gap | Chemin de retrait après échec MinHook non testé | low | defer | Échec MinHook non provocable dans le banc ; chemin court. |
| 11 | blind, edge, gap | Rejeu F10 d’un descripteur de 0x60 octets de taille/contenu inconnus | maybe-false | defer | Non vérifiable sans connaître `eSMotionPlayBackDesc` (c’est Q1) ; à reconsidérer après lecture du log. |
| 12 | blind | SEH à chaque frame dans les menus si `GetPlayer` faute | maybe-false | defer | `GetPlayer` passe par `gCSession::GetPlayer` (rend probablement nul) ; le tick affichera « erreur (SEH…) » si c’est le cas. |
| 13 | blind | Code Map ≠ code (`GetPropertySet`, `eCDynamicEntity*`) | low | rejet | Correctif = éditer la spec ; écart déjà documenté dans Implementation Notes. |
| 14 | blind | MinHook absent du diff | false | rejet | `third_party/` exclu du diff volontairement ; `VERSION.txt` (hash 8af6b4a) et `LICENSE.txt` présents. |
| 15 | blind, edge | `LoadLibraryW(Script.dll)` : DLL non initialisée / ordre de chargement | false | rejet | `Script_Game.dll` importe `Script.dll` ; `LoadLibrary` exécute la même initialisation que le chargeur. |
| 16 | blind | Hooks jamais retirés à l’arrêt | maybe-false | rejet | Appels protégés par SEH ; aucun déchargement des scripts par Risen ; impact faible. |
| 17 | blind | Axe avant non normalisé pour F9 | low | rejet | La matrice brute (demandée et obtenue) est journalisée : lecture non ambiguë. |
| 18 | edge | `OnProcess` sur un autre thread | maybe-false | rejet | C’est précisément ce que mesure Q4 ; le tick le signale (« DIFFÉRENT »). |
| 19 | edge | Adresse d’`anim_ps` réutilisée avant la frame suivante | low | rejet | Fenêtre d’une frame, impact = une ligne mal attribuée. |
| 20 | edge | `log::info` lève après `MH_EnableHook` | low | rejet | `bad_alloc` sur une chaîne courte : improbable. |
| 21 | edge | Sous-classe de `eCApplication` redéfinissant `OnProcess` | maybe-false | rejet | Aucun export de ce type ; si le tick n’apparaît pas, la sonde le montre. |

## Verification

**Commands :**
- `cmake --build client/build && cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- attendu : `client_tests` et `wine_scenarios` passent
- `objdump -p client/build/Script_Multiplayer.dll | grep -E 'ScriptInit|DLL Name'` -- attendu : export présent, seul import `KERNEL32.dll` (+ `USER32.dll` pour `GetAsyncKeyState`)

**Manual checks :**
- En jeu avec `probe = 1` : coller le log à l’humain pour analyse.
