---
title: 'Story 3.3 — Répliques des PNJ : apparition, position et PV'
type: 'feature'
created: '2026-10-21'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'cbcbfda2cff46f41ba0ad538fa3f98a70b753e82'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/docs/re/spike-j2.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Chaque jeu fait vivre ses propres PNJ. L’invité voit donc les monstres ailleurs que chez l’hôte, avec d’autres PV, et les deux joueurs ne peuvent pas combattre les mêmes monstres (FR-9). Les animations et la mort des répliques sont l’objet de la story 3.4.

**Décisions (utilisateur, 2026-10-21) :**
- **Tous les PNJ vivants du rayon** sont répliqués, pas seulement les hostiles : les villageois proches suivent aussi l’hôte.
- **GUID introuvable chez l’invité** (Q10) : l’entité est ignorée et journalisée une fois. Le PNJ éventuel de l’invité garde son IA, et le protocole ne change pas pour ce cas.

**Approach:** L’hôte fait autorité. Il attribue un `NetId` ≥ 1000 à chaque PNJ retenu et annonce `Spawn{type 1, NetId, pose, GUID}` (protocole **v9**). L’invité retrouve son PNJ natif par le GUID, l’inscrit au registre comme réplique et fige son IA. Ensuite, la position, l’orientation et les PV de la réplique suivent les snapshots de l’hôte. À la sortie du rayon ou à la fin de session, la réplique est libérée : elle retrouve son IA et n’est jamais détruite.

## Boundaries & Constraints

**Always :**
- Hôte, à chaque tick de snapshot (20 Hz) : les PNJ de `Entity::ms_arrNPCs` (sans le héros ni l’avatar), vivants, à `kNpcRadius` = 50 m au plus du héros de l’hôte ou de l’avatar de l’invité, triés du plus proche au plus loin. Le snapshot porte au plus 54 entités par datagramme, héros de l’hôte compris : les plus proches passent d’abord (AD-7).
- `NetId` : attribué par l’hôte à partir de 1000, lié au GUID pour toute la session, jamais réutilisé pendant celle-ci. `Spawn{1}` part à l’entrée dans le rayon, `Despawn` à la sortie ou à la mort. Les PV (`PSSkills::GetHitPoints`) sont dans chaque entrée du snapshot, bornés à 65535.
- Invité, sur `Spawn{1}` :
  - `gCRuntimeInfo::GetEntity(GUID + 4 octets nuls)` retrouve le PNJ natif ;
  - la réplique est inscrite au registre avec le rôle `NpcReplica`, son PS de routine compris, et son IA est figée par le détour `gCScriptRoutine_PS::OnProcess`, qui lit le registre ;
  - elle est ensuite placée par `MoveTo` à la pose interpolée, avec 150 ms de retard et une horloge distante commune à toutes les entités ;
  - ses PV sont écrits par `SetHitPoints(pv, 2)` quand ils changent ;
  - `sync/npc` est le seul à écrire l’état d’une réplique.
- Libération, sur `Despawn`, à `Ended` et à la fin de monde : la réplique sort du registre (IA rendue) et la table l’oublie. Pas de `Kill`, pas de destruction. Aucun pointeur ne reste hors de la table.
- AD-3 : un groupe `resolve_npc` (Script, Game, Engine), propre à la session et en tout ou rien, reprend les symboles de la sonde dont la 3.3 a besoin. La sonde et la session restent exclusives pour le détour de routine.
- AD-10 : `PROTOCOL.md` v9, vecteurs, codecs C++ et Go, tests des deux côtés. Le jeu n’est appelé que depuis la frame (AD-2), sauf la comparaison au registre dans le détour. Aucun log par frame hors `debug`.

**Never :**
- Pas d’animation ni de mort des répliques (story 3.4), ni de dégâts (3.5, 3.6).
- Pas de `EnableClassProcessing`, qui figerait tous les PNJ.
- Pas de `Entity::GetNPCs` en session : il fait fuir son tampon à chaque appel.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Entrée dans le rayon | PNJ à 40 m de l’hôte | `NetId` ≥ 1000, `Spawn{1}`, puis l’entité dans les snapshots | N/A |
| Réplique suivie | snapshots | pose interpolée et PV de l’hôte ; aucune IA locale ne la déplace | N/A |
| Sortie du rayon ou mort chez l’hôte | > 50 m, ou `IsDead` | `Despawn` ; l’invité libère la réplique | N/A |
| Retour dans le rayon | même PNJ | même `NetId`, nouveau `Spawn{1}` | N/A |
| Trop de PNJ | > 53 PNJ retenus | les 53 plus proches ; les autres attendent | N/A |
| Fin de session ou fin de monde | `Ended` | toutes les répliques libérées, IA rendue | N/A |
| GUID introuvable chez l’invité | `GetEntity` rend nul | ignoré, log une fois par `NetId` ; snapshots de ce `NetId` ignorés | N/A |
| `Spawn{1}` en double | `NetId` déjà présent | ignoré, journalisé | N/A |
| Snapshot d’un `NetId` inconnu | pas de `Spawn` reçu | entité ignorée, comptée | N/A |
| Décodage | `Spawn{1}` tronqué, type inconnu | `Truncated` / refus | message ignoré, log |

</frozen-after-approval>

## Code Map

- `protocol/PROTOCOL.md`, `client/src/core/protocol.{h,cpp}`, `tools/fake-guest/protocol/` :
  - `Spawn` devient de taille variable : 22 o pour le type 0, 38 o pour le type 1, avec `guid` sur 16 o après le lacet. Le décodeur branche sur `type`.
  - `kSpawnTypeNpc = 1`, `kFirstNpcNetId = 1000`, `kProtocolVersion = 9`.
  - Les PV du snapshot changent de sens (§4 bis), pas de format.
  - Règle §8 étendue : pour le type 1, l’invité n’accepte que les `NetId` ≥ 1000 venus de l’hôte.
  - Vecteurs `spawn_npc.bin`, `snapshot_multi.bin` ; ceux qui portent la version sont régénérés.
  - Repères : PROTOCOL.md:168-188, 242-261, 804-806 ; protocol.h:124-158 ; protocol.go:137, 552-557.
- `client/src/core/session.{h,cpp}` :
  - `World` reçoit une liste `npcs{guid, pose, hp, alive}` remplie par l’hôte.
  - Côté hôte, la logique pure tient la table GUID → NetId, le rayon (héros de l’hôte et pose de l’avatar), le tri par distance, le plafond, `Spawn{1}`/`Despawn` et les PV (session.cpp:666-685 envoie aujourd’hui une seule entité, PV à 0).
  - Côté invité :
    - une map NetId → {Interpolator, PV, présent} ;
    - `RemoteClock` unique ;
    - `on_snapshot` (session.cpp:646-664) accepte les NetId annoncés ;
    - nouvelles commandes `SpawnNpc{net_id, guid, pose}`, `MoveNpc`, `SetNpcHp`, `ReleaseNpc` ;
    - tout est libéré à `Ended`.
  - Tests dans `client/tests/test_session.cpp`.
- `client/src/game/` :
  - groupe `resolve_npc`, avec `?ms_arrNPCs@Entity@@2V?$bTObjArray@VEntity@@@@A` (donnée de Script.dll, lue comme `{Entity* data; u32 count}`), `IsPlayer`, `IsDead`, `GetPose`, `MoveTo`, `GetID` (`get_entity_id`), `gCRuntimeInfo::GetEntity`, `GetHitPoints` et `SetHitPoints` ;
  - les enveloppes existantes de combat.cpp:75-87 et 227-246 sont réutilisées, comme `get_property_set(kRoutinePropertySet)` ;
  - `resolve_combat` (sonde) reste inchangé.
- `client/src/sync/npc.{h,cpp}` (nouveau) : énumération côté hôte (lecture, rayon) et répliques côté invité (`spawn`, `move`, `set_hp`, `release`, `release_all`). C’est le seul module qui inscrit des `NpcReplica` au registre.
- `client/src/sync/anim_filter.{h,cpp}` : `blocks_routine(routine_ps)` sous verrou. Le rôle `NpcReplica` existe déjà (:66), et `routine_ps` est déjà renseigné (:69-75).
- `client/src/win/hooks.cpp:58-60` et `sync/probe_j2.cpp:732-739` : en session, le détour de routine appelle `blocks_routine` (rôle `NpcReplica`) ; avec la sonde, il garde son comportement.
- `client/src/sync/session.cpp` : `World.npcs` est rempli par `sync/npc` (hôte), et les nouvelles commandes passent par `execute()`. `resolve_npc` est appelé dans `install_session` de win/hooks.
- `tools/fake-guest/` :
  - `Spawn{1}` avec GUID ;
  - une table des PNJ et un bilan par NetId (poses reçues, PV) ;
  - les entités inconnues sont comptées ;
  - tests contre les nouveaux vecteurs.
  - Repères : session.go:465-468, 496-516.
- `client/tests/session_host_main.cpp` et `client/tests/session_fake_guest.sh` : l’hôte Linux simule des PNJ (`--npcs N` : trajectoires, PV, entrées et sorties du rayon), et le faux invité les journalise. `client/tests/wine/run_scenarios.sh` vérifie les exports de `resolve_npc`.
- `client/README.md` et `README.md` : procédure de test en jeu, protocole v9.

## Tasks & Acceptance

**Execution :**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/`, `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp` : `Spawn{1}` avec GUID, v9. Motif : AD-10.
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` : sélection et NetId côté hôte, répliques côté invité, cas de la matrice. Motif : logique testée sous Linux.
- [x] `client/src/game/` : `resolve_npc` et ses enveloppes. Motif : AD-3.
- [x] `client/src/sync/npc.{h,cpp}`, `client/src/sync/anim_filter.{h,cpp}`, `client/src/sync/session.cpp`, `client/src/win/hooks.cpp` : énumération, répliques, gel par le registre. Motif : AD-9, AD-12.
- [x] `tools/fake-guest/` : `Spawn{1}`, snapshots multi-entités, bilan, tests. Motif : AC de l’epic.
- [x] `client/tests/session_host_main.cpp`, `session_fake_guest.sh`, `wine/run_scenarios.sh` : PNJ simulés et exports. Motif : banc.
- [x] `client/README.md`, `README.md` : test en jeu, protocole v9. Motif : utilisabilité.

**Acceptance Criteria :**
- Given les tests Linux, Go et le banc Wine, when on construit, then tout passe au vert et les vecteurs v9 concordent en C++ et en Go.
- Given Risen en hôte et le faux invité, when des monstres entrent et sortent du rayon, then le faux invité journalise les `Spawn{1}`, les `Despawn` et les snapshots des PNJ (PV compris), à moins de 54 entités par datagramme. *(Manuel.)*
- Given deux machines sur la même sauvegarde, when l’hôte approche un camp de monstres, then chez l’invité ils suivent les positions et les PV de l’hôte sans IA locale, puis retrouvent leur IA à la fin de session. *(Manuel, validation finale.)*

## Implementation Notes

- **Lecture de `ms_arrNPCs`** : `Entity::GetNPCs` appelle `Entity::UpdateEntityLists` (export
  `?UpdateEntityLists@Entity@@KAXXZ`, reconstruit les listes au plus une fois par frame, sans
  allocation pour l'appelant) avant de copier le tableau. `resolve_npc` résout donc aussi
  `UpdateEntityLists`, appelé juste avant la lecture directe : sans lui, le tableau peut être
  périmé (pointeurs d'entités détruites).
- **`IsDead`, `GetPose`, `MoveTo`** viennent de `resolve_entities`, déjà exigé en tout ou rien
  avant `resolve_npc` ; le banc Wine vérifie quand même leurs exports avec ceux du groupe.
- **Spawn de type inconnu** : la taille dépendant du type, il est refusé (`BadValue`) au
  décodage et à l'encodage (C++ et Go). Vecteur invalide ajouté : `spawn_bad_type.bin`.
- **Entrée dans le rayon** : le `Spawn{1}` passe par KCP (jusqu'à 10 ms), le snapshot part
  aussitôt. Un PNJ n'est porté par les snapshots qu'à partir du tick qui suit son annonce, sinon
  l'invité ignorerait systématiquement sa première entité (constaté au banc : autant d'entités
  inconnues que d'annonces). Les `Despawn` partent avant le snapshot du même tick.
- **Plafond** : un PNJ annoncé qui sort des 53 plus proches reçoit `Despawn` (« évincé par plus
  proche ») ; il est réannoncé sous le même NetId dès qu'il y revient.
- **Rayon** autour de l'avatar : pose affichée de l'avatar chez l'hôte (dernier `MoveAvatar`).
  `sync/npc` pré-filtre à 100 m (le double du rayon) pour que la sortie soit journalisée « hors du
  rayon » plutôt que « absent de la liste ».
- **Registre** : la réplique est inscrite avec son PS de routine seulement, sans PS d'animation
  ni enveloppe : le moteur de l'invité l'anime encore (story 3.4).
- **Sûreté des pointeurs** : une fois par frame (`npc::validate`, avant les commandes),
  `GetEntity(GUID)` doit rendre la même entité pour chaque réplique ; sinon elle sort du registre
  et de la table (journalisée une fois). Un `MoveTo` en SEH libère aussi la réplique (IA rendue),
  et un PNJ sans `gCScriptRoutine_PS` est traité comme un GUID introuvable. Côté hôte, un PNJ aux
  PV illisibles est omis du tick, et une liste de plus de 4096 entités est signalée une fois.
  Le bilan debug compte les `OnProcess` de routine bloqués. Hors session, `npc::release_all()` sert de filet (aucun appel au
  jeu). Le journal « GUID introuvable » est émis une fois par NetId et par session.
- **PV du héros** : `World::hp` (`GetHitPoints` du joueur), dans l'entrée du héros de chaque
  snapshot, des deux côtés.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge | `MoveTo` en SEH : réplique jamais déplacée, IA figée jusqu’au `Despawn` | medium | patch | `move_error_logged` gardait l’entrée au registre ; réplique désormais libérée (`drop`). |
| 2 | blind, edge | Revalidation par GUID seulement sur commande : adresse de PS réutilisée → mauvais PNJ figé ; un `GetEntity` par commande | medium | patch | Réplique sans `MoveNpc` jamais revérifiée ; `npc::validate()` une fois par frame. |
| 3 | edge | `GetHitPoints` en échec → PV 0 répliqués sur un PNJ vivant | medium | patch | Repli silencieux à 0 dans `read_host` ; PNJ sauté ce tick, log une fois. |
| 4 | edge | Réplique sans `gCScriptRoutine_PS` : déplacée mais IA active | low | patch | Traitée comme un GUID introuvable. |
| 5 | blind, edge | `ms_arrNPCs` coupé à 4096 entrées sans log | low | patch | Avertissement unique quand le total dépasse le plafond. |
| 6 | edge | Compteur d’`OnProcess` bloqués jamais journalisé | low | patch | Ajouté au bilan debug ; seul moyen d’observer le gel en jeu. |
| 7 | verif-gap, blind | Rayon après retrait de l’avatar non testé | low | patch | Test étendu (`Despawn{2}` → PNJ « hors du rayon »). |
| 8 | blind | Coups locaux de l’invité sur une réplique non documentés ; commentaire `NpcUnknownRx` incomplet | low | patch | Limitation ajoutée au README ; libellés du faux invité alignés. |
| 9 | verif-gap | Détour de routine et `sync/npc` jamais exécutés par un test | medium | defer | Même couture `game::` que 3.1 à 3.2c ; entrée dans `deferred-work.md`. |
| 10 | blind, edge | PNJ vivant à 0 PV (humain à terre) : `SetHitPoints(0)` chez l’invité | maybe-false | defer | Medium si vrai ; à trancher avec la 3.4 (mort des répliques). |
| 11 | blind, edge | GUID introuvable : la session garde la réplique et émet `MoveNpc`, rejetés par `sync/npc` | low | rejet | Effet limité aux compteurs debug ; un retour vers le cœur ajouterait un chemin. Comportement attendu tenu. |
| 12 | blind | `MoveNpc` à chaque frame même pose figée | low | rejet | Coût à mesurer en jeu ; le `GetEntity` par commande est déjà retiré (n° 2). |
| 13 | blind | Octets 16-19 du `bCPropertyID` non vérifiés nuls | low | rejet | Aller-retour GUID + 4 octets nuls validé au spike sur 3 PNJ. |
| 14 | blind | Snapshot tardif appliqué à une réplique réannoncée | low | rejet | Saut bref, corrigé au snapshot suivant. |
| 15 | blind | PV d’un snapshot hors d’ordre réappliqués | low | rejet | Corrigé au snapshot suivant. |
| 16 | blind, edge | Faux invité plus strict que la DLL (NetId relié à un autre GUID) | low | rejet | L’hôte ne réattribue jamais un NetId (règle §8) ; le faux invité vérifie plus, sans conséquence. |
| 17 | blind | L’invité ne plafonne pas ses répliques | low | rejet | `Despawn` passe par KCP (fiable) ; hôte de confiance. |
| 18 | edge | `GetEntity` rendant le héros ou l’avatar de l’invité | false | rejet | L’hôte exclut son héros (même GUID que celui de l’invité) ; l’avatar est une création temporaire, jamais dans la liste de l’hôte. |
| 19 | edge | Même NetId deux fois dans un snapshot | low | rejet | L’hôte n’en émet jamais ; hôte de confiance. |
| 20 | blind | `npc_guids_[id]` par `operator[]` | false | rejet | Les deux tables sont toujours remplies ensemble ; aucune insertion fantôme possible. |
| 21 | blind | Tests manquants (débordement `kMaxNpcHpChanges`, PNJ dans le snapshot de l’invité) | low | rejet | Chemins défensifs de la même logique testée. |
| 22 | blind | Code Map cite `probe_j2.cpp`, le diff ajoute un détour séparé dans `hooks.cpp` | low | rejet | Correction de la spec elle-même ; écart consigné dans les Implementation Notes. |

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : les 6 tests passent.
- `cd tools/fake-guest && go test ./...` : tout passe, vecteurs v9 compris.

**Manual checks :**
- En jeu, Risen en hôte et faux invité : relever les lignes `[sync] PNJ …` et le bilan du faux invité.
