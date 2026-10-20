---
title: 'Spike J2 — sonde de combat : arme de la copie, avatar solide, PNJ natifs, mort interceptée'
type: 'feature'
created: '2026-10-20'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: '19a98339520bd407238afbf0a2ec5ce162701041'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/docs/re/spike-j1.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Quatre questions ouvertes de la spine bloquent l'Epic 3 et ne se tranchent qu'en jeu : Q5 (arme en main et mode combat de l'avatar), Q6 (avatar « solide »), Q3 (identifier, figer et blesser les PNJ natifs), Q7 (empêcher la mort native de finir la session).

**Approach:** On étend la sonde (`probe = 1`) avec six expériences sur le pavé numérique. Elles portent sur la copie du joueur (F6/F7) ou sur le PNJ le plus proche, et chacune journalise ce qu'il faut pour trancher. `docs/re/spike-j2.md` décrit l'ABI et le protocole de test en jeu, que l'utilisateur fera plus tard. Le code est livré sans attendre ce test.

## Boundaries & Constraints

**Always :**
- AD-2 : les appels au jeu se font depuis `OnProcess`. Il y a une seule exception, documentée : le détour de `Kill` peut appeler l'original de `Defeat` avec les mêmes arguments, sur le même thread. Le détour de `gCScriptRoutine_PS::OnProcess` se contente de comparer un pointeur.
- AD-3 : symboles résolus par nom décoré, sret explicite, SEH autour de chaque appel, offsets notés dans `docs/re/spike-j2.md`. Les nouvelles enveloppes vont dans `client/src/game/combat.*`.
- Les nouveaux hooks sont facultatifs : si l'un échoue, son expérience est inactive et le log dit pourquoi, mais la sonde J1 continue de marcher.
- Avec `probe = 0` (mode session), rien ne change : aucun nouveau hook n'est posé.
- Touches actives seulement quand le jeu a le focus, sur front montant. Log limité en débit.

**Never :**
- Pas de protocole ni de réseau. `HitRequest`, `HitOnGuest` et `Respawn` attendent les stories 3.4 à 3.6.
- Ne pas couper l'IA de tous les PNJ (`EnableClassProcessing` écrit un booléen global).
- Pas d'`Entity::Kill` pour tuer un PNJ (c'est le retrait du monde).

## I/O & Edge-Case Matrix

| Touche | Entrée / état | Résultat attendu (log `[probe] J2`) | Erreur |
|---|---|---|---|
| Num1 arme (Q5) | Copie présente | Héros : index et nom de l'arme en main, mode combat. Copie : PS 0x1f/0x60 présents ?, `CreateItems` → index, `HoldRightStack` → bool, mode combat avant/après. Un 2ᵉ appui fait `EquipStack` (arme rangée) | Pas de copie, main vide ou PS absent : log seul |
| Num2 collision (Q6) | Copie présente | Passe au mode suivant (0 → 1 → 2 → 3 → 0) et journalise groupe de collision, PS 0xd/0xe/0x15, gravité. Mode 0 = `MoveTo` actuel ; 1 = groupe NPC (4) + `EnableCollision` + DCC ; 2 = 1 + corps cinématique sans gravité (drapeaux 0x80\|0x1), placement `SetPose` du corps sans `StopMovement` ; 3 = `SetGoalPose` chaque frame, `MoveTo` si l'écart dépasse 150 cm | PS absent : étape sautée, log |
| Num3 PNJ (Q3) | PNJ ≤ 15 m | Nom, GUID (16 o en hexa), PV/max, aller-retour `gCRuntimeInfo::GetEntity(GUID)` == entité ? | Aucun PNJ : log |
| Num4 IA (Q3) | Après Num3 | Fige ou libère l'IA de ce PNJ (hook `gCScriptRoutine_PS::OnProcess` filtré sur son PS 0x24). Pose journalisée sur 3 s | Hook absent : log |
| Num5 dégâts (Q3) | Après Num3 | `DoDamage(arme, PNJ, copie ou héros)` avec 10 dégâts, puis repli `SetHitPoints(-10, AddValue)`. PV avant/après de chaque méthode | Pas d'arme : seulement le repli |
| Num6 mort (Q7) | Partie chargée | Active ou désactive l'interception : un `Kill` du héros devient `Defeat`, en mode inconscient. Le log indique chaque mort interceptée. Désactivée, la mort native se déroule normalement et Num6 tenu 1 s relève le héros (PV max, AIMode = LastAIMode, collision, DCC, ragdoll coupé, `ZS_StandUp`), étape par étape | Hook `Kill` absent : relève seule |

</frozen-after-approval>

## Code Map

- `client/src/sync/probe.cpp` -- F6 (copie), F7 (miroir, `mirror_pose` → `move_to`), F4 (PNJ le plus proche ≤ 15 m, à réutiliser), `pressed`/`game_has_focus`, remise à zéro quand le joueur change. Le mode de collision de Num2 se branche dans `mirror_pose`.
- `client/src/game/entity.{h,cpp}` -- `resolve_entities` (`GetProcAddress`, tout ou rien) ; même modèle pour une `resolve_combat` séparée, qui peut échouer seule.
- `client/src/win/hooks.cpp` -- `install_probe` : ajouter les hooks facultatifs `Kill` (Script_Game.dll) et `gCScriptRoutine_PS::OnProcess` (Game.dll).
- ABI (statique, 2026-10-20) — les vues PS de Script.dll prennent `this = Entity*` :
  - Q5 : `PSInventory` (PS 0x1f) `GetRightHoldStack`→int, `GetRightHoldItem(sret Entity)`, `GetTemplate(sret Template, int)`, `CreateItems(const Template*, int, int stackType)`→int, `HoldRightStack(int)`→bool, `EquipStack(int)`→bool. `PSCombatSystem` (0x60) `GetCombatMode`→int. Il n'existe pas de setter du mode de combat : il découle de l'objet tenu.
  - Q6 : `Entity::{Get,Set}CollisionGroup`, `EnableCollision(bool)`, `PSRigidBody::{AddBodyFlag,SetPose}`, `PSCharacterMovement::{EnableGravitation,SetGoalPose(const bCMatrix*)}`, `PSNavigation::{SetDCCEnabled,SetDCCRadius}`. `MoveTo` = `SetToWorldMatrix`, puis `PSRigidBody::SetPose`, puis `PSNavigation::ResetAll` et `StopMovement`.
  - Q3 : `eCDynamicEntity::GetID`→`const bCPropertyID*` (0x14 o, dont 16 o de GUID ; pas la version `eCEntity`). `gCRuntimeInfo::GetEntity(nullptr, id)`. `PSSkills::{GetHitPoints,GetHitPointsMax,SetHitPoints(int,int mod)}` (mod : 0 ajout, 1 max, 2 fixer). `PSDamage::DoDamage(Entity* arme, Entity* cible, Entity* agresseur)`, `gCDamage_PS::SetDamageAmount`. Hook `?OnProcess@gCScriptRoutine_PS@@UEAAXXZ`.
  - Q7 : Script_Game.dll `?Kill@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z` et `?Defeat@@…` (mêmes signatures). Relève : `PSRoutine` `LastAIMode`/`AIMode` (opérateurs), `SetState(bCString par valeur)`, `PSNavigation::SetDCCEnabled`, `set_ragdoll(false)`. Sans interception, le menu de chargement s'ouvre au bout de 10 s environ (`ZS_Dead_LoopPlayer`).

## Tasks & Acceptance

**Execution :**
- [x] `client/src/game/combat.{h,cpp}` -- enveloppes Q5, Q6, Q3 et Q7 sous SEH, `resolve_combat` séparée, adresses des deux hooks. Pour `bCString`, utiliser le constructeur et le destructeur exportés de SharedBase -- AD-3
- [x] `client/src/win/hooks.cpp` -- hooks facultatifs `Kill` et `RoutineOnProcess` dans `install_probe` uniquement, avec leurs détours -- AD-2, matrice
- [x] `client/src/sync/probe_j2.{h,cpp}` -- les six expériences, l'état du mode de collision, le PNJ suivi (remis à zéro au changement de joueur), le PS figé (atomique) et le drapeau d'interception ; appelé depuis `probe::frame` et `mirror_pose` -- matrice
- [x] `client/CMakeLists.txt` -- nouvelles sources
- [x] `client/tests/wine/run_scenarios.sh` -- `probe = 1` : log « J2 : hooks Kill/Routine posés » ou cause explicite -- non-régression
- [x] `docs/re/spike-j2.md` -- ABI ci-dessus, protocole de test en jeu pas à pas (touches, quoi observer, quelles lignes rapporter), tableau des réponses à remplir -- utilisabilité

**Acceptance Criteria :**
- Given `probe = 0`, when on lance une session, then le log et le comportement sont ceux de la v6 (aucun hook J2).
- Given `probe = 1` et `Script_Game.dll` absente (banc Wine), when la sonde s'installe, then la sonde J1 est active et le log nomme l'expérience Q7 inactive.
- Given `probe = 1` sous Proton (vérification manuelle, plus tard), when on suit `docs/re/spike-j2.md`, then chaque expérience produit ses lignes sans crash.

## Implementation Notes

- ABI revérifiée au désassembleur : la victime de `Kill`/`Defeat` est `other` (3ᵉ argument, un `Entity*`) ; `AIMode` à `+0xC0`, `LastAIMode` à `+0xD0` du PS 0x24 ; `AddBodyFlag` fait un OU, d'où 0x81 en un appel ; `PSDamage::DoDamage` a pour `this` l'arme, et la sonde rétablit `DamageAmount` après le coup.
- Num6 : un appui court bascule l'interception **au relâchement** (et non sur front montant), pour qu'un appui tenu 1 s (relève) ne la bascule pas aussi.
- Un `Kill` dont `other` est nul n'est pas intercepté : identifier la victime demanderait d'appeler `GetOtherEntity` hors de l'exception AD-2. Ces appels sont comptés dans le log.
- `Script_Game.dll` n'est jamais chargée par le mod (`GetModuleHandle` seul). Si elle est absente au moment de `ScriptInit`, Q7 est inactive (correctif d'orchestration : la 1ʳᵉ version la chargeait par avance).
- Num2 : le mode 3 = mode 1 + `SetGoalPose` (drapeaux cinématiques et gravité du mode 2 retirés). Retour au mode 0 : seul le groupe d'origine est rétabli, collision et DCC restent actifs (pas d'accesseur pour leur état d'origine).
- Num1 : `CreateItems` avec le type de pile 0 (comme Script_Game.dll) ; les appuis suivants alternent `HoldRightStack` et `EquipStack`.
- Les hooks J2 sont créés avant l'activation groupée `MH_EnableHook(MH_ALL_HOOKS)` : un échec à ce stade (improbable) coupe toute la sonde.
- Banc Wine : sans `Script_Game.dll`, le hook Routine est posé et Q7 est nommée inactive ; `probe = 0` et le jeu sans Script.dll n'ont aucune ligne `[hooks] J2`. Les lignes des six touches ne se vérifient qu'en jeu (3ᵉ critère, manuel, plus tard).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge | Num5 : le repli `SetHitPoints(-10)` s'ajoutait toujours à `DoDamage` | medium | patch | Deux sources de dégâts par appui, réactions non attribuables ; repli seulement si `DoDamage` n'a pas porté. |
| 2 | blind, edge | PNJ suivi et PS figé gardés après la mort ou le retrait du PNJ | medium | patch | Rien ne les oubliait hors Num3/Num4/changement de joueur ; vérification dans `GetNPCs` avant Num4/Num5, oubli et IA libérée sinon. |
| 3 | blind, edge | Relève : `AIMode = LastAIMode` peut remettre 11 ou 12 | medium | patch | `Kill`/`Defeat` écrivent `LastAIMode` ; étape sautée si mort ou à terre (constantes déjà déclarées). |
| 4 | blind, edge | `detour_kill` sans garde de réentrance | low | patch | Garde `thread_local` : un Kill imbriqué va à l'original. |
| 5 | blind, gap | Defeat en échec compté comme mort interceptée | low | patch | Le détour retombe sur Kill ; compté seulement dans les échecs. |
| 6 | gap, edge | Num1 : bascule tenue/rangée même en cas d'échec | low | patch | Bascule seulement si l'appel réussit et rend true. |
| 7 | blind | Groupe NPC = 4 jamais vérifié | low | patch | Num3 journalise le groupe de collision d'un vrai PNJ. |
| 8 | blind, gap | Code mort `previous == 2`, littéraux 0xd/0x15 | low | patch | Le cycle n'atteint 0 que depuis 3 ; constantes utilisées. |
| 9 | blind | `GetPropertySet` résolu et enveloppé deux fois | low | patch | `combat.cpp` utilise `game::get_property_set`. |
| 10 | gap | Commentaire de `probe_j2.h` contredit l'exception AD-2 | low | patch | Commentaire corrigé. |
| 11 | gap | Noms de `Kill`/`Defeat` jamais vérifiés | medium | patch | Faute de frappe invisible au banc ; `run_scenarios.sh` cherche les deux noms dans la Script_Game.dll du jeu. |
| 12 | blind | Branche « hooks Kill/Routine posés » du banc morte, `[ OK ]` écrit à la main ; échec de `resolve_combat` non vérifié | low | patch | Le mod ne charge plus Script_Game.dll ; assertions directes, plus `expect_no_log` « inactives ». |
| 13 | blind | Ordre alphabétique de chargement affirmé sans preuve | low | patch | Commentaire marqué `[ASSUMPTION]` ; le log d'installation tranche. |
| 14 | blind | Le commentaire `probe` de l'ini ne cite pas Num1 à Num6 | low | patch | `multiplayer.example.ini` complété (le `multiplayer.ini` local n'est pas versionné). |
| 15 | blind | Q7 suppose que la mort passe par `Kill` ; part des `other` nuls | low | patch | Lignes ajoutées au tableau des réponses de `spike-j2.md`. |
| 16 | gap | Repli « ABI du combat introuvable, sonde J1 active » sans scénario | medium | defer | Il faudrait une Script.dll aux exports trafiqués ; disproportionné pour un spike. |
| 17 | edge | Relève sur un héros vivant | false | rejet | Voulu : la relève doit aussi marcher après `Defeat` (AIMode 11, `IsDead` faux). |
| 18 | blind, edge | Relève capturée par le hook ragdoll J1 et rejouée sur la copie | false | rejet | C'est le miroir 2.1b voulu : le ragdoll du héros est rejoué sur la copie. |
| 19 | gap | `reset()` ne remet pas l'interception à zéro | false | rejet | Réglage de session voulu : armé avant un rechargement, il doit survivre. |
| 20 | blind | `resolve_combat` en tout ou rien | false | rejet | Les 35 symboles sont résolus sur les vraies DLL (banc Wine, assertion « ABI du combat résolue »). |
| 21 | blind | `RemoveBodyFlag(0x81)` efface d'éventuels bits d'origine ; mode 0 ≠ état initial au 2ᵉ cycle | low | rejet | Spike : le log « avant/après » de chaque mode suffit à l'interpréter. |
| 22 | blind | Exception AD-2 absente de la spine | false | rejet | Limitée à la sonde ; la spine reçoit les conclusions après le test en jeu (epic-3-context). |
| 23 | edge | `find_nearest_npc` : pose illisible journalisée comme « GetNPCs a échoué » | low | rejet | Rare (le joueur a une pose dès que `GetPlayer` le rend) ; message seulement. |
| 24 | edge | Mode 1 : groupe d'origine illisible non signalé au retour | low | rejet | Le log « avant » affiche déjà `groupe=SEH`. |
| 25 | edge | Mode 3 : `get_pose(copie)` en échec, pas de recalage | low | rejet | Les échecs de placement sont comptés par seconde. |
| 26 | edge | `GetTemplate` : fuite du proxy si SEH en cours de construction | low | rejet | Cas théorique, une référence au plus, sonde seulement. |
| 27 | edge | Num5 sur un PNJ à 0 PV | low | rejet | Observation utile au spike ; PV journalisés avant/après. |
| 28 | blind | Log « -> 10 » ≠ dégâts réels | low | rejet | Les PV avant/après sont journalisés. |

## Design Notes

- Num6 : on intercepte `Kill` plutôt que le menu de mort, parce que `Kill` déclenche aussi « GO_GameOver », la caméra libre et le renvoi des compagnons. `Defeat` est l'état « à terre » natif (et une piste pour J5).
- Num4 : le détour compare le PS reçu à un `std::atomic<void*>` écrit depuis `frame()`, sans appeler le jeu.

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- attendu : `client_tests` et `wine_scenarios` passent

**Manual checks :**
- En jeu (plus tard), avec `probe = 1` : suivre `docs/re/spike-j2.md` et coller le log pour analyse.
