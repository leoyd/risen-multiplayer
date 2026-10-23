---
title: 'Story 3.7 — À terre et réapparition de l’invité'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: 'f09d2ccffca4820ca9cd2749c55d4d4a80ee071e'
route: 'auto'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-6-degats-subis-invite.md'
  - '{project-root}/docs/re/spike-j2.md'
warnings: ['oversized']
deferred:
  - summary: >-
      La couture DLL de la mise à terre (apply_hit held → note_down → drain → LocalDown, détour DownOrDeadBy* de session, RespawnHero → guest_health::respawn) n'a aucun test automatique.
    evidence: |-
      Les tests de core/session injectent LocalDown à la main et s'arrêtent aux commandes ; le banc ne lance que l'hôte Linux face au faux invité Go ; le banc Wine ne charge pas Script_Game.dll et n'exécute aucune frame. Supprimer guest_health::drain de frame() ou la branche RespawnHero d'execute() passerait tous les tests. Couvert seulement par les étapes 4 et 5 du « Test en jeu à faire ».
    location: >-
      client/src/sync/guest_health.cpp ; client/src/sync/session.cpp frame/execute ; client/src/win/hooks.cpp install_session
    severity: medium
  - summary: >-
      Un échec de RespawnHero côté DLL (pas de héros, MoveTo en SEH, PV max illisibles) n'est pas signalé à la session, qui a déjà envoyé l'accusé et levé l'état à terre.
    evidence: |-
      maybe-false : on_respawn envoie l'accusé avant l'exécution de RespawnHero. Si MoveTo échoue, les snapshots de tick ≥ T portent l'ancienne pose (Q13 rouverte) ; si les PV ne sont pas écrits, le héros reste à 1 PV sans repli. Les PV sont désormais rendus même si la téléportation échoue. Tranché par l'étape 4 du « Test en jeu à faire » (H1, H2) : rapporter toute ligne Error/Warn « réapparition ».
    location: >-
      client/src/core/session.cpp on_respawn ; client/src/sync/guest_health.cpp respawn
    severity: medium (unverified)
---

<intent-contract>

## Intent

**Problem:** Depuis la 3.6, un coup mortel laisse le héros invité debout à 1 PV, sans fin : FR-12 (« l’invité mort réapparaît près de l’hôte, PV pleins ») n’est pas tenu. Le `Death{2}` de la v5 met encore l’avatar en ragdoll chez l’hôte, sans retour possible, et les snapshots de l’invité encore en route après une réapparition replaceraient l’avatar sur le lieu de la mort (question ouverte 13). Les morts hors coups (chute, eau profonde : question 9) ne sont pas traitées.

**Approach:** Chez l’invité, toute mise à terre (HitOnGuest mortel, coup natif mortel intercepté dans `DownOrDeadBy*`) émet `Death{2}` **une seule fois** (verrou jusqu’au `Respawn`). L’hôte répond `Respawn{2, pose}` (2 m devant son héros), place l’avatar et ignore les snapshots de l’invité jusqu’à un **accusé** `Respawn{2, pose, tick}` de l’invité, qui donne le premier tick de snapshot valide. L’invité se téléporte, PV pleins. Nouveau message `Respawn` (type 25), **protocole v14**. Une mort native (chute, eau profonde) reste une limite documentée, sans crash et sans fin de session forcée par le mod.

## Boundaries & Constraints

**Always:**
- **Protocole v14** : `Respawn` = type **25**, fiable, 25 octets : `net_id u32`, position `x, y, z f32`, `lacet f32`, `tick u32`.
  - Hôte → invité : **ordre** de réapparition, `net_id` = 2, `tick` = 0 (ignoré à la réception).
  - Invité → hôte : **accusé**, même pose, `tick` = premier tick de snapshot de l’invité qui porte la nouvelle pose.
  - Le décodeur ne vérifie aucune valeur (comme `HitOnGuest`) ; le récepteur ignore un `net_id` ≠ 2 ou une pose non finie (Warn).
  - Plus aucun type réservé. Vecteur `respawn.bin` = `Respawn{2, (100, 0, −50), lacet 1,5, tick 42}` = `19 02 00 00 00 00 00 c8 42 00 00 00 00 00 00 48 c2 00 00 c0 3f 2a 00 00 00`. `hello*.bin` régénérés en v14 ; `unknown_type.bin` inchangé.
  - Sens de `Death` en v14 : `Death{2}` venu de l’invité = **à terre** (plus de ragdoll de l’avatar chez l’hôte). `Death{1}` n’est jamais émis (la mort de l’hôte termine la session avant) ; sa réception par l’invité garde le comportement v5.
- **Core, invité** (`core/session`, pur, testé sous Linux) :
  - Entrée `LocalDown{cause}` (0 = HitOnGuest, 1 mêlée, 2 projectile, 3 inferno). En session, si le héros n’est pas déjà à terre : état **à terre**, `Death{2}` envoyé, Info `[sync] héros à terre (<cause>) : Death envoyé, attente du Respawn`. Déjà à terre : Debug, compteur « ignorés ».
  - **Mort native** (`World::dead` passe à 1, hors interception) : même verrou ; `Death{2}` envoyé s’il ne l’a pas déjà été, Warn `[sync] mort native du héros (chute, eau profonde ?) : Death envoyé ; réapparition impossible, la session finira au rechargement`.
  - À terre, un `HitOnGuest` reçu n’émet **pas** d’`ApplyHitOnGuest` (Debug, compteur « ignorés » des coups subis).
  - `Respawn` reçu : accepté seulement à terre, héros non mort nativement, `net_id` = 2 et pose finie ; sinon Warn et rien. Accepté : commande `RespawnHero{pose, teleport = true}`, accusé envoyé avec `tick` = tick de snapshot courant + 1, fin de l’état à terre, Info `[sync] Respawn reçu : héros replacé (pose), PV pleins ; accusé envoyé (tick T)`.
  - **Repli** : à terre depuis `kRespawnTimeoutMs` = 10 000 ms sans `Respawn` et sans mort native → `RespawnHero{teleport = false}` (PV pleins sur place), fin de l’état à terre, Warn `[sync] pas de Respawn de l’hôte depuis 10 s : PV rendus sur place`. Un `Respawn` arrivé ensuite est ignoré (pas à terre).
  - L’état à terre est remis à zéro à l’entrée en session et à `Ended`.
- **Core, hôte** :
  - `Death{2}` reçu, avatar présent, pas de réapparition en cours : réapparition **à envoyer**, Info `[sync] Death reçu : invité à terre, Respawn à envoyer`. Pas d’`AvatarDeath`. Sinon (pas d’avatar, réapparition en cours) : Warn, ignoré.
  - Au premier tick en session où la pose du héros de l’hôte est lisible : pose = `guest_pose_near(pose de l’hôte)`, envoi de `Respawn{2, pose, 0}`, `MoveAvatar{2, pose}` aussitôt, interpolation de l’avatar remise à zéro, **attente de l’accusé**. Info `[sync] Respawn envoyé : invité replacé (pose)`.
  - En attente de l’accusé, toute entrée de snapshot du NetId 2 est ignorée (compteur « snapshots périmés ») ; l’avatar reste à la pose du `Respawn` (aucun `MoveAvatar` sans échantillon).
  - Accusé reçu en attente : fin de l’attente, interpolation remise à zéro, tick minimal = `tick` de l’accusé ; ensuite, une entrée du NetId 2 de tick inférieur est ignorée (périmée). Info `[sync] réapparition confirmée par l’invité (tick T)`. Accusé hors attente : Warn, ignoré.
  - **Repli** : pas d’accusé depuis `kRespawnAckTimeoutMs` = 5 000 ms → fin de l’attente, tick minimal 0, Warn `[sync] pas d’accusé de Respawn depuis 5 s : snapshots de l’invité repris`.
  - États remis à zéro à l’entrée en session, au retrait de l’avatar et à `Ended`.
- **Bilan** `[sync] snapshots (5 s)` : on ajoute **à la fin** `; à terre : Death %u, Respawn envoyés %u, reçus %u, accusés %u, ignorés %u, snapshots périmés %u`.
- **DLL, invité** (`sync/guest_health`, seul écrivain des PV du héros invité, AD-11) :
  - `apply_hit` : un résultat `held` note une mise à terre (cause 0) au lieu du Warn « story 3.7 ». La frame la transmet en `LocalDown` (une par frame au plus).
  - `on_down_or_dead(which, spu, other)` (détour, même thread que la frame) : seulement chez l’invité en session ; victime = `other` lu par `read_entity_ptr`, ou `GetOtherEntity` si `other` est nul (comme `probe_j2::victim_of`). Victime = héros et PV ≤ 0 → `SetHitPoints(1, fixer)`, mise à terre notée (cause 1 + which), rend true (le détour rend 0 sans appeler l’original). Sinon, ou en cas d’échec : false (original appelé). Une ligne Warn par interception `[sync] coup natif mortel intercepté (DownOrDeadBy<type>) : héros à 1 PV, à terre`.
  - `respawn(pose, teleport)` : si `teleport`, `Entity::MoveTo` du héros (via `avatar::teleport_player`) ; puis PV = `GetHitPointsMax` par `SetHitPoints(max, fixer)`. Lignes Info ; héros absent ou SEH : Warn/Error, rien d’écrit.
- **Hooks** (`win/hooks`, liste fermée AD-12 inchangée) : en session, les trois détours `DownOrDeadBy*` sont posés s’ils se résolvent (`resolve_script_game`) ; sinon Warn `[hooks] DownOrDeadBy* inactifs : … (coup natif mortel non intercepté si le point d’accroche des dégâts manque)` et la session continue. Le détour appelle `guest_health::on_down_or_dead`.
- **Faux invité** : v14, codec `Respawn`, option `--die-every DUR` (Death{2} DUR après l’entrée en session puis DUR après chaque `Respawn` reçu ; 0 = jamais). À la réception : log `<- Respawn netid=2 pose=(x, y, z, lacet)`, sa pose simulée devient celle du `Respawn`, accusé envoyé (`-> Respawn accusé tick=T`), bilan `Respawn reçus=N`.
- **Banc** : scénario `session_fake_guest.sh` (port suivant libre) : hôte `session_host`, invité `--die-every 1s --duration 4s --quit`. Attendus : hôte `Respawn envoyé` et `réapparition confirmée`, invité `<- Respawn netid=2`.

**Never:**
- Pas de `Kill`, de `DoDamage`, de ragdoll, d’état `ZS_Unconscious` forcé ni de relève d’un mort sur le héros invité. Jamais 0 PV écrit.
- Pas de nouveau hook (pas de détour de `Kill`, `RagDollKill`, `OnMovementModeChanged`), pas de mode dieu (`gCSessionCheats`).
- Ne pas changer le format du snapshot. Ne pas changer la mort de l’hôte (native, fin de session raison 3).
- Pas d’écriture des PV du héros invité hors de `sync/guest_health`.
- Pas d’attente, de réseau ni d’allocation non bornée dans le détour `DownOrDeadBy*`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Invité, mise à terre | `LocalDown{0}` en session | `Death{2}` envoyé une fois, à terre | N/A |
| Invité, deuxième mise à terre | `LocalDown` déjà à terre | rien envoyé, « ignorés » +1 | Debug |
| Invité, mort native | `World::dead` = 1 | `Death{2}` une fois, à terre | Warn |
| Invité, HitOnGuest à terre | `HitOnGuest` reçu | pas d’`ApplyHitOnGuest` | Debug |
| Invité, Respawn valide | à terre, `Respawn{2, pose}` | `RespawnHero{pose, true}`, accusé `tick` = courant + 1, plus à terre | N/A |
| Invité, Respawn hors état | pas à terre, ou mort native, ou `net_id` ≠ 2, ou pose non finie | rien | Warn |
| Invité, repli 10 s | à terre sans `Respawn` | `RespawnHero{teleport = false}`, plus à terre | Warn |
| Hôte, Death{2} | avatar présent | pas d’`AvatarDeath` ; `Respawn{2, guest_pose_near(hôte), 0}` envoyé au tick suivant de pose lisible, `MoveAvatar` à cette pose | N/A |
| Hôte, pose de l’hôte illisible | réapparition à envoyer | rien tant que la pose manque | N/A |
| Hôte, Death{2} en attente d’accusé ou sans avatar | | ignoré | Warn |
| Hôte, snapshot pendant l’attente | entrée NetId 2 | ignorée, « périmés » +1 | N/A |
| Hôte, accusé | `Respawn{2, …, tick 42}` en attente | fin d’attente ; entrées de tick < 42 ignorées, ≥ 42 interpolées | N/A |
| Hôte, accusé hors attente | | ignoré | Warn |
| Hôte, repli 5 s | aucun accusé | snapshots repris | Warn |
| Vecteur | `respawn.bin` | décodé et réencodé à l’identique (C++ et Go) | N/A |

</intent-contract>

## Code Map

- `client/src/core/protocol.{h,cpp}` : `kProtocolVersion` (h:25) → 14 ; `MsgType::Respawn = 25` (commentaire des réservés h:39 à retirer) ; struct `Respawn{net_id, x, y, z, yaw, tick}`, `ReliableMessage`, `write`/`read`, `decode_reliable` sur le modèle de `HitOnGuest` (3.6) et de `Spawn` (pose).
- `client/tests/test_protocol.cpp` : vecteur `respawn.bin` (modèle `hit_on_guest.bin`), `kValidReliable`, boucle des réservés (plus rien), valeurs figées, version 14.
- `protocol/PROTOCOL.md` : en-tête v14 et historique, table §3 (ligne 25), section `### Respawn (25) — fiable — v14`, §7 vecteur (ne pas l’insérer dans un tableau existant : piège 3.5), §8 « Mort » réécrit (à terre, réapparition, accusé, replis, mort native) ; puis `protocol/vectors/regen.sh` (l’hexdump de `PROTOCOL.md` fait foi) et `protocol/vectors/README.md`.
- `client/src/core/session.h` :
  - `Input` (h:226) + `LocalDown{cause u8}` ; `Command` (h:361) + `RespawnHero{Pose pose; bool teleport}` ;
  - constantes `kRespawnTimeoutMs = 10000`, `kRespawnAckTimeoutMs = 5000` ;
  - membres : invité `guest_down_`, `down_since_`, `native_dead_` (ou lecture de `world_.dead`) ; hôte `respawn_pending_`, `respawn_wait_`, `respawn_sent_at_`, `respawn_min_tick_` ; compteurs `RespawnCounters` à côté de `guest_hits_` (h:625) ;
  - commentaire d’en-tête : paragraphe « À terre et réapparition (story 3.7, protocole v14) ».
- `client/src/core/session.cpp` :
  - `tick` (cpp:207) : branche `LocalDown` avant le `else` final (cpp:270-274) ; après `timers`, en session : replis (invité 10 s, hôte 5 s) et envoi du `Respawn` en attente (hôte) avant `send_snapshot`/`place_avatar` (cpp:284-290) ;
  - `send_death` (cpp:1285) : chez l’invité, devient l’émission unique de `Death{2}` pour une mise à terre ou une mort native (verrou `guest_down_`), log Warn pour la mort native ; `death_sent_` peut être absorbé ;
  - `on_reliable`, branche `Death` (cpp:588-607) : hôte + `net_id` = 2 → réapparition à envoyer (pas de `death_pending_`) ; invité : inchangé ;
  - `on_reliable` : branche `Respawn` avant le `std::get<SessionEnd>` final (cpp:659) : invité = ordre, hôte = accusé ;
  - `on_snapshot` (cpp:731) : filtre de l’entrée du NetId pair chez l’hôte (attente, tick minimal) ;
  - `on_hit_on_guest` (cpp:1062) : à terre → ignoré ;
  - `enter_session` (cpp:673), `remove_avatar` (cpp:156), `end` (cpp:131) : remises à zéro ;
  - `snapshot_stats` (cpp:1646) : suffixe du bilan (tampon de 1400 octets : vérifier la marge) ;
  - réutiliser `guest_pose_near` (h:232) et `finite_pose`.
- `client/tests/test_session.cpp` : un test par ligne « Invité »/« Hôte » de la matrice (patrons des tests `HitOnGuest` de la 3.6 et des tests `Death` d’avatar existants, à adapter : l’hôte ne met plus l’avatar en ragdoll sur `Death{2}`).
- `client/src/game/combat.{h,cpp}` : ajouter `kGetHitPointsMax` au bloc `resolve_npc` (`NpcAbi`, cpp:346) et faire passer `get_hit_points_max` (cpp:809) par `pick_npc` ; mettre à jour le commentaire de `resolve_npc` (h). `resolve_script_game`, `down_or_dead_address`, `read_entity_ptr`, `spu_other_entity` existent déjà.
- `client/src/sync/guest_health.{h,cpp}` : `apply_hit` (note la mise à terre), `on_down_or_dead`, `drain(std::vector<Input>&)` (LocalDown en attente, atomique), `respawn(pose, teleport)`. Héros = `hits::hero()` ; rôle/session : ajouter à `sync/hits` un accesseur `bool guest_in_session()` (lit `g_role` et `g_in_session`, hits.cpp:34-36).
- `client/src/sync/avatar.{h,cpp}` : `teleport_player` (cpp:665) journalise « placé devant l’hôte » : ajouter un paramètre de libellé ou une variante pour la réapparition.
- `client/src/sync/session.cpp` : `frame()` (cpp:358) appelle `guest_health::drain(g_inputs)` après `hits::drain` ; `execute()` (cpp:155) exécute `RespawnHero` → `guest_health::respawn`.
- `client/src/win/hooks.cpp` : `install_session` (cpp:239) : bloc facultatif après celui des dégâts : `game::resolve_script_game`, puis trois `create(down_or_dead_address(i), detour_down_or_dead_session<i>, …)` qui réutilisent `g_original_down_or_dead` (cpp:31 ; sonde et session sont exclusives) ; log du résultat.
- `tools/fake-guest/protocol/{protocol.go,protocol_test.go}` : `TypeRespawn = 25`, struct, codec, v14, réservés, vecteur.
- `tools/fake-guest/session.go`, `main.go`, `README.md` : `--die-every`, réception du `Respawn`, accusé, bilan, aide v14 ; tests `session_test.go` / `fakehost_test.go` (hôte factice qui répond `Respawn` à un `Death{2}` et vérifie l’accusé).
- `client/tests/session_host_main.cpp` (si besoin d’une option) et `client/tests/session_fake_guest.sh` : nouveau scénario (modèle du scénario `HitOnGuest` de la 3.6, port libre suivant).
- `client/tests/wine/run_scenarios.sh` : ajouter `?GetHitPointsMax@PSSkills@@QEBAHXZ` s’il liste les symboles du bloc PNJ.
- Documentation : spine (AD-10 : `Respawn{NetId, pose, tick}` et accusé ; AD-11 : à terre, verrou, replis, mort native ; AD-12 : `DownOrDeadBy*` en session via `sync/guest_health` ; questions 9 et 13 tranchées) ; `epics.md` (note 3.7) ; `client/README.md` (procédure et diagnostic) ; `README.md` (v14) ; `epic-3-context.md` (3.7 faite) ; `deferred-work.md` (pillage de l’avatar mort : résolu, plus de ragdoll ; reports de la story) ; `docs/REPRISE.md` (tableau 3.4a à 3.7 : faites, non testées en jeu, branche `auto/epic-3`).

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/`, `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp` -- v14, `Respawn` (25), `respawn.bin`, hello régénérés, §8 -- AD-10.
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` -- à terre, verrou de `Death{2}`, `Respawn` ordre/accusé, filtre des snapshots, replis, compteurs, tests de la matrice -- AD-11, Q13.
- [x] `client/src/game/combat.{h,cpp}` -- `GetHitPointsMax` dans le bloc PNJ -- AD-3.
- [x] `client/src/sync/hits.{h,cpp}`, `client/src/sync/guest_health.{h,cpp}`, `client/src/sync/avatar.{h,cpp}`, `client/src/sync/session.cpp` -- mise à terre notée, détour `DownOrDeadBy*`, `RespawnHero` -- AD-11.
- [x] `client/src/win/hooks.cpp` -- détours `DownOrDeadBy*` de session, facultatifs -- AD-12.
- [x] `tools/fake-guest/` -- v14, `--die-every`, accusé, tests -- banc.
- [x] `client/tests/session_fake_guest.sh`, `client/tests/session_host_main.cpp`, `client/tests/wine/run_scenarios.sh` -- scénario de réapparition, symboles -- bout en bout.
- [x] Spine, `epics.md`, `client/README.md`, `README.md`, `epic-3-context.md`, `deferred-work.md`, `docs/REPRISE.md` -- décisions, limite Q9, état de l’epic.

**Acceptance Criteria:**
- Given les tests Linux, Go et le banc, when on construit, then tout passe, `regen.sh --check` réussit et `respawn.bin` concorde en C++ et en Go.
- Given le banc (`session_host`) et le faux invité `--die-every 1s`, when l’invité envoie `Death{2}`, then l’hôte journalise `Respawn envoyé` puis `réapparition confirmée`, et le faux invité journalise `<- Respawn netid=2` et envoie son accusé, à chaque mort.
- Given la spine, when on lit AD-10, AD-11 et les questions 9 et 13, then `Respawn` (ordre et accusé), le verrou de `Death{2}`, les replis et la limite de la mort native y sont décrits.
- Given Risen en hôte et le faux invité `--die-every 10s`, when l’invité « meurt », then l’avatar ne passe jamais en ragdoll, saute 2 m devant le héros de l’hôte, puis suit de nouveau les snapshots, sans retour sur l’ancienne position. *(Manuel.)*
- Given deux machines, when les monstres tuent l’invité, then son héros reste à 1 PV, `Death{2}` part une fois, il réapparaît devant l’hôte PV pleins, sans `Kill`, ragdoll ni menu de chargement ; la session continue. *(Manuel.)*
- Given la mort du héros de l’hôte, when elle arrive, then la session se termine (raison 3), comme avant. *(Test existant, inchangé.)*

## Spec Change Log

## Review Triage Log

### 2026-10-23 — Review pass
- verdicts: 32 findings — high 0, medium 2, low 21, false 6, maybe-false 3
- findings:
  - `[low]` `[patch]` (intent a) CA3 contre la limite D5 ; `epic-3-context.md` affirmait qu'une chute ou une noyade ne termine pas la session — la consigne autorise une limite documentée ; la phrase contradictoire est remplacée par la limite D5/Q9.
  - `[low]` `[reject]` (intent b) Après une mort native, l'hôte envoie un Respawn refusé : l'avatar saute devant l'hôte, reste figé 5 s, puis revient — dans le périmètre de la limite documentée (la session finit au rechargement) ; un accusé négatif ajouterait du protocole.
  - `[false]` `[reject]` (intent c) Pas d'état « à terre » visible — le critère met « à terre » entre guillemets et D2 le consigne (le spike J2 a montré que ZS_Unconscious ou un ragdoll cassent le modèle).
  - `[medium]` `[defer]` (intent d) Les attentes vivent dans le jeu, les tests s'arrêtent au core et au banc — même entrée `deferred` que la couture DLL.
  - `[false]` `[reject]` (intent e) Le commit local n'est pas dans le diff relu — il est fait à la finalisation de cette étape.
  - `[false]` `[reject]` (intent f) REPRISE.md nomme la branche `auto/epic-3` — conforme à la consigne ; la section « Branches » précise maintenant la branche réelle.
  - `[low]` `[reject]` (intent g) Sémantique de Death{2} changée et repli invité de 10 s non demandés — décisions D3 et D6 consignées, nécessaires au Respawn sans relève d'un avatar en ragdoll.
  - `[low]` `[reject]` (edge 1) Repli invité à 10 s pendant que l'hôte attend encore une pose lisible : Respawn tardif ignoré — il faut une pose de l'hôte illisible pendant 10 s en monde chargé, irréaliste ; le correctif ajoute un délai et un état.
  - `[low]` `[reject]` (edge 2) respawn_pending_ sans expiration — même cause et même rejet que edge 1.
  - `[low]` `[reject]` (edge 3) Nouvelle mise à terre pendant l'attente d'accusé de l'hôte : Death{2} ignoré — n'arrive qu'après un Respawn refusé ou tardif (cas edge 1 ou mort native) ; l'invité retrouve ses PV au repli de 10 s ; le correctif ajoute une branche.
  - `[maybe-false]` `[defer]` (edge 4) RespawnHero en échec après accusé et levée de l'état à terre — entrée `deferred` (medium, non vérifié), tranchée par H1/H2 en jeu.
  - `[low]` `[patch]` (edge 5) Téléportation ratée : les PV ne sont pas rendus — guest_health::respawn rend maintenant les PV même si teleport_player échoue (ligne Error conservée, Info « sur place »).
  - `[low]` `[reject]` (edge 6) Accusé de tick 0 ou énorme non vérifié — l'accusé vient du code du mod chez l'invité (coop de confiance, D9 de la 3.5) ; le correctif ajoute un contrôle.
  - `[false]` `[reject]` (edge 7) Échec silencieux de l'encodage de Death — un Death de 5 octets ne peut pas échouer à l'encodage (même patron que HitRequest/HitOnGuest).
  - `[false]` `[reject]` (edge 8) g_down périmé transmis à la session suivante — guest_health::drain est appelé à chaque frame et vide g_down ; hors session, on_local_down l'ignore.
  - `[low]` `[reject]` (edge 9, claim) session_host_main.cpp coché sans changement — la spec disait « si besoin d'une option » ; corriger revient à éditer la spec.
  - `[maybe-false]` `[defer]` (edge 10, claim) Accusé envoyé avant RespawnHero : si la téléportation échoue, l'avatar revient sur le lieu de la mort — même entrée `deferred` que edge 4.
  - `[medium]` `[defer]` (verif-gap) Couture DLL de la mise à terre sans test automatique — entrée `deferred` (medium) ; déjà notée dans deferred-work.md ; étapes 4 et 5 du test en jeu.
  - `[low]` `[reject]` (blind 1) Respawn en attente sans expiration — même rejet que edge 1.
  - `[low]` `[reject]` (blind 2) L'invité qui refuse un Respawn ne prévient pas l'hôte — même rejet que intent b.
  - `[maybe-false]` `[defer]` (blind 3) Échec de réapparition non signalé au core — même entrée `deferred` que edge 4 ; la partie « PV non rendus si MoveTo échoue » est corrigée (edge 5).
  - `[low]` `[patch]` (blind 4) Scénario du banc instable (confirmed == sent strict) — accepte maintenant confirmed = sent ou sent − 1, toujours ≥ 2 morts et un Respawn par mort.
  - `[low]` `[reject]` (blind 5) Le faux invité accepte tout Respawn — outil de test ; les refus sont couverts par les tests unitaires du core ; correctif = branche de plus.
  - `[low]` `[reject]` (blind 6) Pas de test du tick d'accusé face au snapshot du même tick — l'arithmétique est testée et TestDieEveryRespawn vérifie que les snapshots suivants portent la nouvelle pose ; le reste dépend de H3 (test en jeu, étape 2).
  - `[low]` `[patch]` (blind 7) epic-3-context.md contredit D5 — même correctif que intent a.
  - `[low]` `[patch]` (blind 8) REPRISE.md mis à jour en partie — date, « Branches » et « Reprendre demain » mis à jour (tests en jeu des stories 3.4a à 3.7).
  - `[low]` `[reject]` (blind 9) on_down_or_dead peut écrire 1 PV hors session effective — fenêtre d'une frame au plus ; le héros reste vivant à 1 PV, sans dommage.
  - `[false]` `[reject]` (blind 10) GetHitPointsMax dans le bloc tout-ou-rien des PNJ pourrait couper la session — symbole exporté par Script.dll du build ciblé (vérifié par llvm-readobj), comme les autres du bloc.
  - `[low]` `[patch]` (blind 11) README.md trop affirmatif sur les snapshots périmés — phrase nuancée (« to be confirmed in game »).
  - `[low]` `[reject]` (blind 12) Frontmatter `deferred` vide et tâches de la spec — rempli par ce tri ; le reste revient à éditer la spec.
  - `[low]` `[reject]` (blind 13) Marge du tampon du bilan non testée — pire cas calculé ≈ 1430 octets sous 1600 ; un test ne ferait qu'ajouter de la surface.
  - `[low]` `[reject]` (blind 14) « À terre » sans effet de jeu (l'invité peut frapper) — fenêtre d'un aller-retour réseau, D2 « état logique court » consigné.

## Design Notes

**Décisions prises sans l’utilisateur (option la plus prudente, à relire) :**
- D1 — **Q13 par accusé**, pas par époque de vie dans le snapshot : un seul message ajouté, le format du snapshot (et ses vecteurs) ne change pas. L’accusé porte le premier tick de snapshot valide, ce qui écarte aussi les snapshots partis avant l’accusé mais arrivés après.
- D2 — **Réapparition immédiate** : l’hôte répond dès la réception de `Death{2}` (au premier tick de pose lisible). « À terre » est un état logique court (1 PV, verrou), sans animation : le spike J2 a montré que forcer `ZS_Unconscious` ou un ragdoll sur le héros casse son modèle.
- D3 — **Plus de ragdoll de l’avatar sur `Death{2}`** : un avatar en ragdoll ne se relève pas ; il est simplement déplacé au point de réapparition. Effet de bord : la duplication d’objets par pillage de l’avatar mort (deferred-work) disparaît.
- D4 — **Point de réapparition** = `guest_pose_near(pose du héros de l’hôte)` (2 m devant lui, tourné vers lui), la règle déjà utilisée à l’entrée en session.
- D5 — **Q9 : pas d’interception des morts hors coups.** Au désassemblage (`Script_Game.dll`, Steam x64) : la chute passe par le script `OnMovementModeChanged` (`SetHitPoints(−dégâts, ajout)`, puis `RagDollKill` et un état de mort à `AIMode` 12) ; l’eau profonde par `PlayerInDeepWater`. Aucun ne passe par `gCDamage_PS::DoDamage` ni par `DownOrDeadBy*`. Les intercepter demanderait un nouveau hook dont l’effet n’est pas vérifiable sans le jeu (le spike a montré que bloquer `Kill` ne suffit pas : l’état de mort posé ensuite mène au menu). Le mode dieu (`gCSessionCheats`, qui annule les dégâts de chute du joueur) est écarté : état de session sauvegardable, effet sur les succès inconnu. **Limite documentée** : une mort native de l’invité donne l’écran de mort de son jeu ; le mod envoie `Death{2}`, refuse le `Respawn` (pas de relève d’un mort), et la session se termine proprement au rechargement de l’invité (fin de monde, `SessionEnd{2}`) ; l’hôte reste en jeu et se réarme.
- D6 — **Replis temporisés** des deux côtés (invité 10 s : PV rendus sur place ; hôte 5 s : snapshots repris), pour qu’un message perdu ou une mort native ne fige ni le héros à 1 PV ni l’avatar.
- D7 — Les détours `DownOrDeadBy*` de session ne servent que de filet (point d’accroche des dégâts absent ou `damage_hook = 0`) : la règle 3b bloque déjà tout coup natif sur le héros invité en amont.

**Hypothèses à confirmer en jeu :** H1, `Entity::MoveTo` sur le héros en cours de partie (hors entrée en session) le replace sans effet de bord, même en combat. H2, `SetHitPoints(GetHitPointsMax, fixer)` remplit la jauge. H3, la pose lue à la frame suivant la téléportation est la nouvelle (sinon l’accusé daterait trop tôt : l’avatar repasserait un instant sur l’ancienne position). H4, `DownOrDeadBy*` voit bien le héros dans `other` (vérifié par la sonde J2 pour la mêlée seulement). H5, une mort native par chute ou eau profonde passe bien `Entity::IsDead` à 1 avant tout chargement.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: tout passe, nouveau scénario de réapparition compris.
- `cd tools/fake-guest && go test ./... && go build` -- expected: tout passe.
- `protocol/vectors/regen.sh --check` -- expected: tous `[ OK ]`, `respawn.bin` compris.

## Test en jeu à faire

Préparation : recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/`, `log_level = debug`, `damage_hook = 1`, overlay Steam désactivé, `cd tools/fake-guest && go build`. Log : `bin/scripts/Script_Multiplayer.log`.

1. **Installation.** Attendu : `[hooks] point d’accroche des dégâts posé` et la ligne des détours `DownOrDeadBy*` de session. Un Warn `DownOrDeadBy* inactifs` est à rapporter.
2. **Une machine, hôte + faux invité qui meurt** : `fake-guest --host 127.0.0.1 --duration 120s --echo --die-every 10s`.
   - Hôte : `Death reçu : invité à terre`, `Respawn envoyé : invité replacé (…)`, `réapparition confirmée par l’invité (tick T)`, une fois par mort.
   - L’avatar ne passe jamais en ragdoll, saute 2 m devant le héros de l’hôte, puis reprend l’écho sans revenir à l’ancienne position (Q13).
   - Faux invité : `<- Respawn netid=2 pose=…` et `-> Respawn accusé tick=T`.
   - Bilan : `à terre : Death N, Respawn envoyés N, …, accusés N`.
3. **Une machine, mort de l’hôte** : se faire tuer avec le héros de l’hôte. Attendu : `mort de l’hôte`, fin de session raison 3, comme avant.
4. **Deux machines, mort par monstres** : laisser les monstres tuer l’invité.
   - Invité : `HitOnGuest … appliqué`, puis `héros à terre (HitOnGuest) : Death envoyé`, puis `Respawn reçu : héros replacé …, PV pleins ; accusé envoyé`. Le héros est devant l’hôte, jauge pleine (H1, H2) ; ni menu de chargement, ni ragdoll ; la session continue.
   - Hôte : mêmes lignes qu’à l’étape 2.
5. **Deux machines, filet `DownOrDeadBy*`** : `damage_hook = 0` chez l’invité, se faire tuer par un monstre. Attendu : `coup natif mortel intercepté (DownOrDeadBy…)`, puis la réapparition (H4).
6. **Deux machines, chute ou eau profonde (limite Q9)** : faire mourir l’invité d’une chute. Attendu : Warn `mort native du héros …`, écran de mort natif chez l’invité, `Respawn` refusé ; au rechargement de l’invité, fin de session (`fin de monde`) et réarmement de l’hôte. Aucun crash. Rapporter si la session se termine autrement (H5).

## Auto Run Result

Status: done

**Résumé :**
- Chez l'invité, un coup mortel (HitOnGuest `held`, ou coup natif intercepté dans `DownOrDeadBy*`) met le héros **à terre** : 1 PV, `Death{2}` envoyé une seule fois (verrou jusqu'au `Respawn`).
- L'hôte répond `Respawn{2, pose}` (2 m devant son héros), sans ragdoll de l'avatar, puis ignore les snapshots de l'invité jusqu'à son **accusé** `Respawn{2, pose, tick}` (Q13). L'invité se téléporte, PV pleins.
- Replis : 10 s chez l'invité (PV rendus sur place), 5 s chez l'hôte (snapshots repris). Mort native (chute, eau profonde) : limite documentée (Q9, D5).
- Protocole **v14** : `Respawn` = type 25, 25 octets.

**Fichiers :**
- Protocole : `protocol/PROTOCOL.md`, `protocol/vectors/` (`respawn.bin`, `hello*.bin` v14, README), `client/src/core/protocol.{h,cpp}`.
- Session : `client/src/core/session.{h,cpp}` (à terre, Respawn ordre/accusé, filtre des snapshots, replis, bilan).
- DLL : `client/src/sync/guest_health.{h,cpp}` (mise à terre, détour `DownOrDeadBy*`, réapparition), `client/src/sync/hits.{h,cpp}` (`guest_in_session`), `client/src/sync/avatar.{h,cpp}` (`teleport_player` rend bool), `client/src/sync/session.cpp`, `client/src/win/hooks.cpp` (détours de session facultatifs), `client/src/game/combat.{h,cpp}` (`GetHitPointsMax`).
- Tests : `test_session.cpp` (14 nouveaux tests, tests de mort d'avatar passés côté invité), `test_protocol.cpp`, `session_fake_guest.sh` (scénario de réapparition), `wine/run_scenarios.sh`.
- `tools/fake-guest/` : v14, `--die-every`, accusé, `TestDieEveryRespawn`, README, scénario `mort.txt`.
- Documentation : spine (AD-10, AD-11, AD-12, Q9, Q13), `epics.md`, `client/README.md`, `README.md`, `epic-3-context.md`, `deferred-work.md`, `docs/REPRISE.md`.

**Revue :** 32 constats (high 0, medium 2, low 21, false 6, maybe-false 3).
- **Correctifs (5 entrées, toutes low)** : PV rendus même si la téléportation échoue ; banc tolérant une mort en vol au départ du faux invité ; `epic-3-context.md` aligné sur D5 ; phrase du `README.md` nuancée ; `docs/REPRISE.md` (date, branches, reprise).
- **Reports** : 2 entrées `deferred` (couture DLL sans test ; échec de RespawnHero non signalé, non vérifié).
- **Rejets** : motivés dans le journal (6 false, 16 low).

**Revue de suivi recommandée :** non. Correctifs : high 0, medium 0, low 5, sous le seuil. Le risque principal (chemin DLL de l'invité) n'est vérifiable qu'en jeu.

**Vérification (après correctifs) :**
- `cmake --build client/build` et `cmake --build client/build-tests` : OK, sans avertissement.
- `ctest` : 6/6 (`client_tests` 382/382, `session_fake_guest` avec le scénario de réapparition, `wine_scenarios`, `wine_net`).
- `go test ./...` et `go build` : OK.
- `regen.sh --check` : 32/32.

**Risques résiduels :**
- Aucun test en jeu ; hypothèses H1 à H5 non vérifiées.
- Mort native de l'invité (chute, eau profonde) : écran de mort natif, session terminée au rechargement (limite D5).
- Après une mort native, l'avatar saute devant l'hôte puis revient 5 s plus tard (Respawn refusé, sans accusé).
