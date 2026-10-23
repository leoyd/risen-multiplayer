---
title: 'Story 3.5 — Dégâts de l’invité sur les monstres'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: '46fbb3fe239e845aeba7eed981bd8dc6d0ec3b2c'
route: 'auto'
review_loop_iteration: 0
followup_review_recommended: true
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-4b-mort-repliques-pnj.md'
  - '{project-root}/docs/re/spike-j2.md'
warnings: ['oversized']
deferred:
  - summary: >-
      Le héros publié par la frame (Entity::GetPlayer) et l'agresseur lu au proxy (eCEntityProxy::GetEntity) pourraient ne pas être le même pointeur (eCDynamicEntity* contre eCEntity*) ; aucun coup de l'invité ne deviendrait alors HitRequest.
    evidence: |-
      maybe-false : les deux sont censés être l'eCEntity* brut (Entity = {eCEntity*}, héritage simple), et les entités du registre viennent du même Entity.ptr. Étapes 6 et 9 du « Test en jeu à faire » : la ligne Debug « dégâts : agresseur P (rôle) cible Q (rôle) » tranche.
    location: >-
      client/src/sync/hits.cpp decide ; client/src/sync/session.cpp frame (publish)
    severity: high (unverified)
  - summary: >-
      Un HitRequest arrivé entre la mort native d'un PNJ et la mise à jour de npc_dead_ (tick de snapshot) appelle DoDamage sur un cadavre.
    evidence: |-
      maybe-false : le script DoDamage passe par CanDoDamage, qui refuse sans doute une cible morte ; à confirmer en jeu (log « HitRequest PNJ N appliqué : DoDamage -> 0, PV 0 -> 0 » sans second message de mort ni d'XP). Correctif possible : game::is_dead(cible) avant do_damage dans hits::apply.
    location: >-
      client/src/sync/hits.cpp apply
    severity: medium (unverified)
  - summary: >-
      Le corps du détour et hits::apply (lecture de l'agresseur, DoDamage réel, publication du héros et du rôle) ne sont vérifiés qu'à l'installation ; seules les règles (hit_rules) sont testées sous Linux.
    evidence: |-
      Le banc Wine n'appelle jamais OnProcess ni gCDamage_PS::DoDamage ; jeu non lançable ici. Couvert par les étapes 1 à 9 du « Test en jeu à faire ».
    location: >-
      client/src/sync/hits.cpp ; client/src/win/hooks.cpp detour_damage
    severity: medium (unverified)
---

<intent-contract>

## Intent

**Problem:** Les coups du héros de l’invité ne blessent que sa réplique locale, sans effet chez l’hôte, qui fait autorité : on ne peut pas tuer un monstre à deux (FR-10). La question ouverte 8 (point d’accroche des dégâts) n’est pas tranchée, et aucun détour ne voit agresseur et cible d’un coup.

**Approach:** Q8 tranchée : on détourne `?DoDamage@gCDamage_PS@@QEAA_NPEAVeCEntity@@@Z` (Game.dll, version à 1 argument), seul passage commun de tout coup natif avant la baisse des PV. Chez l’invité, le coup de son héros sur une réplique est bloqué et devient `HitRequest{NetId}`. Chez l’hôte, tout coup natif de l’avatar invité est bloqué, et un `HitRequest` s’applique par `PSDamage::DoDamage(arme en main de l’avatar, cible, avatar)`, sous un garde de réentrance. La mort passe ensuite par le chemin 3.4b. Protocole **v12**.

## Boundaries & Constraints

**Always:**
- **Point d’accroche** (`win/hooks`, un seul détour, logique dans `sync/hits`) : `bool detour(void* damage_ps, void* target)`.
  - Cible = `target` (`eCEntity*` brut, comparable aux entités du registre). Agresseur = `eCEntityProxy::GetEntity(gCDamage_PS::GetAgressor(damage_ps))`, lu par une enveloppe `game/` sous SEH.
  - Bloquer = rendre `false` sans appeler l’original. Dans tous les autres cas (garde actif, lecture impossible, entités non concernées, erreur), on appelle l’original : **en cas de doute, on laisse passer**.
- **Règles du détour**, dans l’ordre :
  1. Garde `thread_local` « application d’un HitRequest » actif → original.
  2. Session hors `InSession`, ou rôle inconnu → original.
  3. Invité, cible = réplique de PNJ (registre, rôle `NpcReplica`) :
     - agresseur = son héros (pointeur publié par la frame, atomique) et réplique vivante → **bloqué**, NetId mis en file pour la frame (`HitRequest`) ;
     - réplique morte ou autre agresseur → bloqué sans émission.
  4. Hôte, agresseur = avatar de l’invité (registre, rôle `Avatar`) → **bloqué**, sans émission : seuls les `HitRequest` blessent.
  5. Sinon → original. Les coups qui touchent le héros invité ou l’avatar relèvent de la 3.6 : on les laisse passer, avec une ligne Debug.
- **Diagnostic** (lignes Info, hors frame) :
  - à la première exécution du détour par session : `[sync] dégâts : 1er passage au point d’accroche (thread T, thread OnProcess T0)` ;
  - à chaque coup bloqué : `[sync] coup natif bloqué : <agresseur> -> PNJ N (HitRequest)` ou `… avatar invité -> <cible> (seuls les HitRequest blessent)` ;
  - agresseur illisible → Warn une fois par session, puis on laisse passer ;
  - en `log_level = debug`, chaque passage dont la cible ou l’agresseur est au registre (ou est le héros) : `[sync] dégâts : agresseur P (rôle) cible Q (rôle) -> bloqué|laissé`.
- **Invité (core)** : la nouvelle entrée `LocalHit{net_id}` est acceptée seulement en `InSession` ; le rôle invité et une réplique annoncée, vivante, sans mort en attente sont requis. Dans ce cas : `HitRequest` fiable + Info `[sync] coup sur PNJ N : HitRequest envoyé`. Sinon on ignore, Debug, compteur « ignorés ».
- **Hôte (core)** :
  - `HitRequest{id}` accepté si `id ≥ 1000`, annoncé (`npc_active_`) et pas mort (`npc_dead_`). Dans ce cas, commande `ApplyHit{id, guid}`. Sinon : Warn `[sync] HitRequest PNJ N ignoré : <raison>` (NetId inconnu, mort, < 1000) et compteur.
  - Un invité qui reçoit `HitRequest` l’ignore (Warn).
- **Hôte (DLL, `sync/hits::apply`)**, dans l’ordre :
  1. cible par `find_entity(guid)` ;
  2. avatar invité (`avatar::entity()`) ;
  3. arme = `get_right_hold_item(avatar)` ;
  4. `get_hit_points` avant ;
  5. `do_damage(arme, cible, avatar)` sous le garde ;
  6. `get_hit_points` après.

  Ligne Info : `[sync] HitRequest PNJ N appliqué : DoDamage -> 0|1, PV a -> b`. Cible, avatar ou arme absents : Warn qui nomme la cause (`PNJ introuvable`, `pas d’avatar`, `avatar sans arme en main`), aucun dégât, aucun `SetHitPoints` direct.
- **Bilan** `[sync] snapshots (5 s)` : ajouter **en fin** `; coups : envoyés %u, appliqués %u, ignorés %u, bloqués %u` (bloqués = compteur atomique du détour, relevé par la frame). Format existant inchangé.
- **Configuration** : clé `damage_hook` (défaut `1`, `multiplayer.example.ini` documenté). À `0`, le détour n’est pas posé, Info `[hooks] point d’accroche des dégâts désactivé (damage_hook = 0)`, et les `HitRequest` reçus restent appliqués.
- **Résolution** : un bloc `resolve_damage` facultatif dans `game/combat`, avec `gCDamage_PS::DoDamage` (1 arg), `GetAgressor`, `eCEntityProxy::GetEntity` et `PSDamage::DoDamage`, en tout ou rien. Un échec donne un Warn `dégâts inactifs`, sans détour, et les `HitRequest` sont ignorés (Warn). Le reste de la session continue.
- **Protocole v12** : `HitRequest` = type **22**, `{net_id u32}`, 5 octets, sur le canal fiable, de l’invité vers l’hôte. Mises à jour :
  - `PROTOCOL.md` : table §3, section du message, §8 « Coups de l’invité », historique ;
  - vecteurs : `hit_request.bin` (`16 e8 03 00 00`), `hello*.bin` en version 12 ;
  - codecs, tests C++ et Go.
- **Faux invité** : options `--hit-interval DUR` (défaut 0 = aucun) et `--hit-target N` (défaut 0 = premier PNJ présent et vivant). Il envoie `HitRequest` à cette cadence et journalise `-> HitRequest PNJ netid=N`. Il ne vérifie pas que le NetId est annoncé : c’est ce qui permet de tester le cas « inconnu ».

**Never:**
- Ne jamais écrire directement les PV d’un PNJ chez l’hôte. Pas de `Kill`, pas de détour `HitTargets`/`OnDamage`/`DoMeleeDamage`, un seul nouveau hook.
- Ne pas traiter les coups reçus par le héros invité ou l’avatar (3.6), ni la mise à terre (3.7). Pas de modification de `DamageAmount`.
- Aucune attente, allocation lourde ni appel réseau dans le détour : seulement lecture du registre, file sous verrou court, compteurs.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Invité frappe une réplique | `LocalHit{1000}`, 1000 vivante | `HitRequest{1000}` envoyé, compteur « envoyés » | N/A |
| Invité, réplique morte ou en attente de mort | `LocalHit{1000}` | rien envoyé, « ignorés » | Debug |
| Invité, NetId non annoncé | `LocalHit{1500}` | rien envoyé, « ignorés » | Debug |
| Hors session | `LocalHit` avant `InSession` | ignoré | N/A |
| Hôte reçoit HitRequest valide | 1000 annoncé et vivant | `ApplyHit{1000, guid}`, « appliqués » | N/A |
| Hôte, NetId inconnu | `HitRequest{1500}` | pas de commande, « ignorés » | Warn « NetId inconnu » |
| Hôte, PNJ mort | 1000 dans `npc_dead_` | pas de commande | Warn « PNJ mort » |
| Hôte, NetId < 1000 | `HitRequest{2}` | pas de commande | Warn |
| Invité reçoit HitRequest | | ignoré | Warn |

</intent-contract>

## Code Map

**Point d’accroche (rétro-ingénierie, 2026-10-23, Game.dll x64) :**
- `gCCombatMoveMelee::HitTargets` (0x1807d5650) : pour chaque cible touchée, `SetEntity(ps+0x30, agresseur)`, puis `call gCDamage_PS::DoDamage(ps, cible)` (0x1807d5c5a et 0x1807d5ea3). Si le retour est faux, l’effet d’impact est sauté.
- `DoDamage` 1 arg (0x1808081b0) : appelle `cible->OnDamage`, puis le `DamageScript` (`DoSwordDamage`/`DoFistDamage` → `DoMeleeDamage` → script `DoDamage` → `SetHitPoints(-dmg, ajout)` → `DownOrDeadByMeleeWeapon` → `DoMeleeStumble`). Les PV n’ont pas encore baissé à l’entrée.
- `PSDamage::DoDamage` (Script.dll) fait `jmp` vers `gCDamage_PS::DoDamage` 2 args (0x1808091c0), qui fait `SetEntity(+0x30, agresseur)`, puis `jmp` vers la version 1 arg : **le chemin du mod traverse le détour**, d’où le garde.
- Le prologue (`push rbp; push rsi; push r14; sub rsp,30h`) n’a pas d’adressage RIP : MinHook peut s’y poser.
- Correction du spike : dans `DownOrDeadBy*`, `self` = **l’arme**, pas le personnage. À corriger dans `docs/re/spike-j2.md`, `combat.cpp:103` et la spine (AD-11).

**Fichiers :**
- `client/src/core/protocol.{h,cpp}` :
  - `kProtocolVersion` (h:25) → 12 ; `MsgType::HitRequest = 22` (h:40-56, commentaire des réservés) ; `struct HitRequest{net_id}` ;
  - `ReliableMessage` (h:221) ;
  - `write`/`read` sur le modèle de `Death` (cpp:156, :336), `decode_reliable` (cpp:417).
- `client/tests/test_protocol.cpp` :
  - vecteur `hit_request.bin` (modèle `death_npc.bin`, :712) ;
  - `kValidReliable` (:139) ;
  - boucle des réservés (:498, retirer 22) ; valeurs figées (:562-590), version (:432).
- `protocol/PROTOCOL.md` : titre et historique (:1-13), table (:57), section `HitRequest` après `Death` (:197), §8 nouvelle puce après « Mort des répliques » (:999-1020), version citée (:69, :283, :289, :321, :337), bloc `hello.bin` et vecteurs. Ensuite `protocol/vectors/regen.sh` (l’hexdump de PROTOCOL.md fait foi).
- `client/src/core/session.{h,cpp}` :
  - `Input` (h:212) + `LocalHit{net_id}` ; branche explicite **avant** le `else` final de `tick` (cpp:267, piège) ;
  - `on_reliable` : branche `HitRequest` **avant** le `std::get<SessionEnd>` final (cpp:650, piège) ;
  - commande `ApplyHit{net_id, guid}` dans `Command` (h:323), modèle `CaptureNpc` ;
  - compteurs dans `NpcCounters` (h:571) ou une struct `HitCounters`, ligne de `snapshot_stats` (cpp:1536, tampon de 1400 octets) ;
  - émission sur le modèle de `send_death` (cpp:1175) ; refus côté rôle sur le modèle de `on_npc_death` (cpp:940) ;
  - entrée `blocked` : nouvel `Input` `LocalHitsBlocked{count}` ou champ passé par la frame : au choix de l’implémenteur, du moment que le bilan l’affiche.
- `client/src/game/combat.{h,cpp}` : `resolve_damage()` facultatif (table `DamageAbi`, `lookup` :220), `damage_hook_address()`, `damage_aggressor(ps, void*& out)` (SEH). `do_damage` (:780-818) doit marcher en session : passer par une table de session comme `pick`/`pick_npc` (:189-213), aujourd’hui gardée par `g_resolved`. Les symboles `GetAgressor@gCDamage_PS` (Game) et `GetEntity@eCEntityProxy@@QEAAPEAVeCEntity@@XZ` (Engine) sont exportés (vérifié).
- `client/src/sync/hits.{h,cpp}` (nouveau, à ajouter au CMake du client) :
  - `on_damage(ps, target) → bool block` (détour) : file `std::vector<uint32_t>` sous mutex, compteur atomique des bloqués, journal du 1ᵉʳ passage ;
  - `drain(std::vector<Input>&)` (frame) ; `apply(net_id, guid)` (hôte) ; `set_hero(void*)` (atomique) ; garde `thread_local`.
- `client/src/sync/anim_filter.{h,cpp}` : `find_by_entity(const void* entity, TrackedEntity& out) const`, sur le modèle de `find_by_animation_ps` (h:93, cpp:59).
- `client/src/sync/session.cpp` :
  - `frame()` (:366-400) : publier le héros (`world.player`, :107) dans `hits` ; drainer les `LocalHit` après `local_captures` ;
  - `execute()` (:215-252) : `ApplyHit` → `hits::apply` ;
  - exposer le rôle et l’état « en session » au détour par atomiques : remis à « hors session » à `Ended` et à la fin de monde.
- `client/src/sync/avatar.{h,cpp}` : `avatar::entity()` (:663) donne l’avatar invité chez l’hôte.
- `client/src/sync/npc.{h,cpp}` : `find_entity(guid, …)` (:100), à exposer si besoin.
- `client/src/win/hooks.cpp` :
  - `install_session` (:224-278) : après les détours obligatoires, si `damage_hook` vaut 1 et `resolve_damage` réussit, `create(damage_hook_address(), detour_damage, &g_original_damage, …)` ; sinon Warn et on continue ;
  - log séparé `[hooks] point d’accroche des dégâts posé (gCDamage_PS::DoDamage)`. Le log « hooks de session installés (…) » est vérifié mot pour mot par `run_scenarios.sh` :63/:80 : ne pas le changer.
- `client/src/core/config.{h,cpp}`, `client/tests/test_config.cpp`, `client/multiplayer.example.ini` : clé `damage_hook` (0/1, défaut 1), transmise par `win/script_init.cpp` (:212-239) puis `sync::session::configure`.
- `client/tests/session_host_main.cpp` :
  - `ApplyHit` journalisé `[test] ApplyHit NetId N` ;
  - les PV simulés du PNJ visé baissent de 10 (état de dégâts par index dans `fill_npcs`, :149-171).
- `client/tests/session_fake_guest.sh` : scénario port `base+19`, `--npcs` + `--hit-interval 500ms`, puis un second passage `--hit-target 1500`. Attentes :
  - hôte `HitRequest PNJ 1000 appliqué` (ou `[test] ApplyHit NetId 1000`) ;
  - invité `-> HitRequest PNJ netid=1000` ;
  - hôte `HitRequest PNJ 1500 ignoré : NetId inconnu`.
- `client/tests/wine/run_scenarios.sh` (:125-141) : ajouter les 4 symboles de `resolve_damage`.
- `client/tests/test_session.cpp` : un test par ligne de la matrice (patron `guest_with_npcs()`).
- `tools/fake-guest/` :
  - `protocol/protocol.go` : v12, `TypeHitRequest = 22`, struct, encodage et décodage, réservés (`protocol_test.go` :50/:61/:129/:156/:331) ;
  - `main.go` : options, aide v12 ;
  - `session.go` : envoi périodique sur le modèle du heartbeat (:244-283), `sendReliable` (:498), bilan `HitRequest envoyés` ;
  - `fakehost_test.go` : `hostRecord.hits` (:365) ; `session_test.go` : `TestParseConfig`, test d’envoi.
- Documentation :
  - `ARCHITECTURE-SPINE.md` : AD-12, liste des hooks : « le point d’accroche des dégâts : `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll, Q8 tranchée en 3.5) » ; AD-9, garde de réentrance ; AD-11, `self` = arme ;
  - `epics.md` : Q8 tranchée ;
  - `docs/re/spike-j2.md` : section « Q8 — point d’accroche des dégâts » (chaîne ci-dessus) ;
  - `client/README.md` : procédure ; `README.md` : v12 ;
  - `epic-3-context.md` : 3.5 faite, hook nommé ;
  - `deferred-work.md` si besoin.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/`, `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp` -- v12, `HitRequest` (22), `hit_request.bin`, hello régénérés -- AD-10.
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` -- `LocalHit`, émission, réception hôte, `ApplyHit`, refus, compteurs et bilan, un test par ligne de la matrice -- logique testée sous Linux.
- [x] `client/src/core/config.{h,cpp}`, `client/tests/test_config.cpp`, `client/multiplayer.example.ini` -- `damage_hook` -- repli sûr pour le test en jeu.
- [x] `client/src/game/combat.{h,cpp}` -- `resolve_damage`, `damage_aggressor`, `do_damage` utilisable en session -- AD-3.
- [x] `client/src/sync/anim_filter.{h,cpp}`, `client/src/sync/hits.{h,cpp}`, `client/src/sync/session.cpp`, CMake -- règles du détour, file, garde, `apply`, publication du héros, du rôle et de l’état -- AD-9, AD-12.
- [x] `client/src/win/hooks.cpp`, `client/src/win/script_init.cpp` -- détour facultatif `gCDamage_PS::DoDamage`, configuration -- AD-12.
- [x] `tools/fake-guest/` -- v12, `--hit-interval`, `--hit-target`, tests -- banc.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh`, `client/tests/wine/run_scenarios.sh` -- `ApplyHit` simulé, scénario coups, symboles -- bout en bout.
- [x] Spine (AD-9, AD-11, AD-12), `epics.md`, `docs/re/spike-j2.md`, `client/README.md`, `README.md`, `epic-3-context.md` -- Q8 tranchée et consignée.

**Acceptance Criteria:**
- Given les tests Linux, Go et le banc, when on construit, then tout passe, `regen.sh --check` réussit et `hit_request.bin` concorde en C++ et en Go.
- Given le banc (`session_host --npcs`) et le faux invité `--hit-interval`, when il envoie des `HitRequest` sur 1000, then l’hôte émet `ApplyHit` pour 1000, les PV simulés baissent dans les snapshots reçus par le faux invité, et `--hit-target 1500` donne un Warn « NetId inconnu » sans commande.
- Given la spine, when on lit AD-12, then le point d’accroche des dégâts y est nommé (`gCDamage_PS::DoDamage`, Game.dll).
- Given Risen en hôte et le faux invité `--hit-interval 1s`, when un monstre est répliqué, then sa jauge baisse chez l’hôte et le log contient `HitRequest PNJ N appliqué : DoDamage -> 1, PV a -> b` avec b < a. *(Manuel.)*
- Given deux machines, when le héros de l’invité frappe une réplique, then son coup natif est bloqué (`coup natif bloqué`), la jauge baisse chez les deux, et le monstre meurt chez les deux. *(Manuel.)*

## Spec Change Log

## Review Triage Log

### 2026-10-23 — Review pass
- verdicts: 29 findings — high 0, medium 4, low 16, false 3, maybe-false 6
- findings:
  - `[low]` `[reject]` (edge) HitRequest appliqué alors que l'avatar invité est mort chez l'hôte — l'arme est perdue à la mort (spike J2), donc « avatar sans arme » ; l'invité mort ne frappe pas ; le correctif ajoute une garde.
  - `[low]` `[reject]` (edge) Compteur « appliqués » compté à l'émission d'ApplyHit, même si hits::apply refuse ensuite — chaque refus a sa ligne Warn ; le correctif demande un retour DLL → session (nouvelle entrée).
  - `[low]` `[reject]` (edge) File de 64 HitRequest pleine : coups perdus sans compteur — plus de 64 coups du héros en une frame est irréaliste ; correctif = compteur et bilan en plus.
  - `[maybe-false]` `[defer]` (edge) Héros (GetPlayer) et agresseur (proxy) de types différents → aucun HitRequest — entrée `deferred` (high, non vérifié), tranché par la ligne Debug en jeu.
  - `[low]` `[reject]` (edge) Réplique sortie du registre (drop) frappée nativement — une réplique libérée redevient un PNJ natif de l'invité, comme hors rayon (comportement 3.3).
  - `[maybe-false]` `[reject]` (edge) Détour sur un autre thread que OnProcess pendant publish → LocalHit périmé — hypothèse « même thread » journalisée au 1er passage ; au pire low : le core ignore un NetId non annoncé.
  - `[maybe-false]` `[defer]` (edge) DoDamage sur un PNJ mort natif avant la mise à jour de npc_dead_ — entrée `deferred` (medium, non vérifié).
  - `[medium]` `[patch]` (edge) PROTOCOL.md : section hit_request.bin insérée dans le tableau de motion_npc.bin — tableau rétabli, section déplacée après ; regen --check 30/30.
  - `[medium]` `[patch]` (blind) Même défaut PROTOCOL.md — même correctif.
  - `[low]` `[patch]` (blind) `damage_hook = 0` présenté comme bénin alors que l'invité tuerait nativement ses répliques — example.ini et client/README : interrupteur de diagnostic, à éviter chez l'invité.
  - `[maybe-false]` `[defer]` (blind) apply sans vérifier que la cible est vivante — groupé avec l'entrée `deferred` du cadavre.
  - `[low]` `[reject]` (blind) « appliqués » faux — même motif que la ligne edge.
  - `[low]` `[reject]` (blind) Log « appliqué » quand DoDamage rend 0 — la ligne affiche déjà `DoDamage -> 0` et les PV ; correctif = branche de plus.
  - `[low]` `[reject]` (blind) Aucun contrôle de distance ou de cadence chez l'hôte — coop à deux joueurs de confiance (D9) ; correctif = nouvelles gardes.
  - `[low]` `[patch]` (blind) Le détour journalise alors que l'AD-2 de la spine dit « compare et met en file » — spine complétée (lignes Info hors frame, Debug). La spec impose ces lignes.
  - `[low]` `[reject]` (blind) File et liste des morts : débordements silencieux — même motif que la ligne edge.
  - `[low]` `[reject]` (blind) Règle 2 « rôle inconnu » jamais atteinte après configure — branche défensive sans effet nuisible ; hors session, le détour laisse déjà passer.
  - `[maybe-false]` `[reject]` (blind) Garde de réentrance trop large pour des DoDamage imbriqués (riposte, 3.6) — rien d'imbriqué n'est montré ; à revoir en 3.6 ; au pire low aujourd'hui.
  - `[low]` `[patch]` (blind) Projectiles et magie : l'agresseur au proxy pourrait être le projectile (dégâts à distance perdus) — entrée deferred-work.md et étape facultative 8 (arc) dans client/README.md.
  - `[medium]` `[patch]` (blind) Règles du détour non testées alors qu'elles sont pures — groupé avec la ligne verif-gap.
  - `[low]` `[reject]` (blind) Trous de test_session (LocalHit chez l'hôte, hors session, remise à zéro) — issue identique (ignoré) ; on_reliable a déjà la garde InSession.
  - `[false]` `[reject]` (blind) TestHitRequest instable (dernier HitRequest non livré) — 8 exécutions de suite OK ; KCP est ordonné et le SessionEnd suit les HitRequest sur le même flux.
  - `[medium]` `[patch]` (verif-gap) Règles 1 à 5 du détour, garde et file sans test automatique — décision extraite dans `sync/hit_rules.{h,cpp}` (sans Windows, compilée dans la DLL et client_tests), `client/tests/test_hit_rules.cpp` : 8 tests, dont « garde active, avatar agresseur → laissé ». Le reste du chemin DLL est reporté (`deferred`).
  - `[low]` `[reject]` (verif-gap) « appliqués » compté avant l'application — même motif que la ligne edge.
  - `[low]` `[reject]` (verif-gap) Deux tests de session ne figent pas la branche de rôle — l'issue visible est la même (ignoré) ; correctif de test seulement.
  - `[false]` `[reject]` (intent) Commit local pas encore fait au moment du diff — il est fait à la finalisation de ce workflow.
  - `[maybe-false]` `[defer]` (intent) Parties au contact du jeu validées seulement à l'installation — entrée `deferred` du chemin DLL ; « Test en jeu à faire ».
  - `[low]` `[reject]` (intent) D7 (compétences de l'avatar) et D8 (mains nues ignorées) en retrait du critère « PV baissent chez les deux » — choix prudents consignés en Design Notes, à relire par l'utilisateur.
  - `[false]` `[reject]` (intent) Correction de `self` = arme dans DownOrDeadBy* hors périmètre — documentation issue de la rétro-ingénierie de Q8, sans changement de comportement.

## Design Notes

**Décisions prises sans l’utilisateur (option la plus prudente, à relire) :**
- D1 — **Q8 : `gCDamage_PS::DoDamage(eCEntity*)` (1 arg), Game.dll.**
  - C’est le seul maillon qui voit cible et agresseur (proxy `+0x30`, renseigné juste avant), avant la baisse des PV, et qui sait rendre « rien fait » (`false`, déjà géré par `HitTargets`).
  - Il couvre mêlée, projectiles et magie.
  - Écartés :
    - la version à 2 arguments, jamais appelée par la mêlée native ;
    - `OnDamage`, qui ne fait que diffuser aux PS et ne bloque rien ;
    - `DoMeleeDamage`/`DoSwordDamage` : plusieurs détours, Script_Game.dll absente du banc. C’est le repli documenté si l’hypothèse tombe.
- D2 — **Garde de réentrance `thread_local`** : l’application d’un `HitRequest` traverse le détour (chaîne `jmp`). Sans garde, l’hôte bloquerait son propre `DoDamage` (agresseur = avatar).
- D3 — **Repli sûr** :
  - tout doute fait laisser passer le coup ;
  - le détour est facultatif (résolution en échec → session sans dégâts synchronisés) ;
  - `damage_hook = 0` le coupe si le test en jeu crashe.
  - Cela déroge à AD-3 (tout ou rien) pour ce seul bloc, comme `Script_Game.dll` dans la sonde.
- D4 — **Blocage chez l’invité de tout coup sur une réplique**, quel que soit l’agresseur (AD-9 : la cible n’est pas sous autorité locale). Seul le héros émet. Les coups reçus par le héros ou l’avatar passent encore (3.6).
- D5 — **Hôte : tout coup natif de l’avatar est bloqué**, quelle que soit la cible (héros de l’hôte compris : pas de tir ami). On ne sait pas si l’avatar, animé par rejeu, déclenche `HitTargets` : le log le dira.
- D6 — **Perte assumée chez l’invité** : le coup bloqué n’a ni effet d’impact ni réaction (`DoMeleeStumble`) sur la réplique. La réaction jouée chez l’hôte revient par le rejeu d’animations (3.4).
- D7 — **Dégâts calculés par le script de l’hôte** avec l’arme de l’avatar et les compétences de l’avatar (pas celles du héros invité). Pas de `DamageAmount` modifié. Écart de force accepté pour le MVP.
- D8 — **Pas d’arme en main de l’avatar** (poings, arme rangée) → `HitRequest` ignoré, Warn. Les poings ne sont pas des objets d’inventaire de l’avatar.
- D9 — **Pas de limite de cadence** des `HitRequest` : un coup natif = un appel par cible ; le log et le bilan permettront de juger.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: tout passe, banc `session_fake_guest` compris.
- `cd tools/fake-guest && go test ./... && go build` -- expected: tout passe.
- `protocol/vectors/regen.sh --check` -- expected: tous `[ OK ]`, `hit_request.bin` compris.

## Test en jeu à faire

Préparation :
- recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/` ;
- `log_level = debug`, `damage_hook = 1`, overlay Steam désactivé ;
- `cd tools/fake-guest && go build`.

Log : `bin/scripts/Script_Multiplayer.log`.

1. **Installation (hôte).** Attendu : `[hooks] point d’accroche des dégâts posé (gCDamage_PS::DoDamage)`. Si on voit à la place `dégâts inactifs` ou un crash au chargement : remettre `damage_hook = 0` et rapporter la ligne.
2. **Une machine : Risen en hôte + faux invité** (`fake-guest --host 127.0.0.1 --duration 180s --echo --weapon It_1H_Sword --hit-interval 1s`), près d’un camp de monstres.
   - Attendu chez l’hôte : `HitRequest PNJ N appliqué : DoDamage -> 1, PV a -> b`, avec b < a ; jauge du monstre qui baisse ; monstre qui devient hostile envers l’avatar.
   - Jusqu’à la mort : `PNJ N mort (Death)`, puis plus de `HitRequest` appliqué pour N (`ignoré : PNJ mort`).
   - Le faux invité voit les PV de N baisser (`PNJ netid=N PV a -> b`).
   - Ligne `1er passage au point d’accroche (thread T, thread OnProcess T0)` : T doit égaler T0.
3. **Une machine, NetId inconnu** : `--hit-target 1500 --hit-interval 1s`. Attendu : `HitRequest PNJ 1500 ignoré : NetId inconnu` (Warn), aucun crash.
4. **Une machine, coups du héros de l’hôte** : frapper un monstre avec le héros de l’hôte. Attendu : dégâts normaux, aucune ligne `coup natif bloqué` (le détour laisse passer).
5. **Une machine, avatar qui attaque** : avec `--echo`, faire attaquer le héros de l’hôte près d’un monstre (l’avatar rejoue l’attaque). Si une ligne `coup natif bloqué : avatar invité -> …` apparaît, l’avatar déclenche bien des coups natifs, bloqués. Son absence n’est pas une erreur : noter le résultat (hypothèse D5).
6. **Deux machines** : l’invité frappe une réplique.
   - Attendu chez l’invité : `coup natif bloqué : héros -> PNJ N (HitRequest)` et `coup sur PNJ N : HitRequest envoyé` ; la jauge locale ne baisse pas d’elle-même, puis suit l’hôte.
   - Attendu chez l’hôte : `HitRequest PNJ N appliqué`.
   - Tuer le monstre à deux : il meurt chez les deux (`Death`, 3.4b).
   - Bilan : `coups : envoyés X, … bloqués Y` côté invité, `appliqués X` côté hôte.
7. **Deux machines, arme rangée** : frapper à mains nues. Attendu chez l’hôte : `HitRequest PNJ N ignoré : avatar sans arme en main` (D8).
8. Si l’agresseur n’est jamais reconnu (aucune ligne `bloqué` alors que l’invité frappe), rapporter les lignes Debug `dégâts : agresseur … cible …` : l’hypothèse « proxy `+0x30` = personnage » est alors fausse. Repli prévu : `DoMeleeDamage` (Script_Game).

## Auto Run Result

Status: done

**Résumé :**
- Q8 est tranchée : le point d'accroche des dégâts est `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll, version à 1 argument), ajouté à AD-12.
- Chez l'invité, le coup natif de son héros sur une réplique est bloqué et devient `HitRequest{NetId}`.
- Chez l'hôte, tout coup natif de l'avatar invité est bloqué. Un `HitRequest` s'applique par `PSDamage::DoDamage(arme en main de l'avatar, PNJ, avatar)`, sous une garde de réentrance.
- Le détour est facultatif (`damage_hook`, résolution en tout ou rien de son bloc). En cas de doute, il laisse passer le coup.
- Les lignes de diagnostic sont prévues pour le test en jeu.
- Le protocole passe en v12.

**Fichiers :**
- `protocol/PROTOCOL.md` et `protocol/vectors/` (`hit_request.bin`, `hello*.bin` en v12) ; `client/src/core/protocol.{h,cpp}` : message `HitRequest` (22).
- `client/src/core/session.{h,cpp}` : `LocalHit`, `LocalHitsBlocked`, `ApplyHit`, refus, compteurs « coups » en fin de bilan.
- `client/src/core/config.{h,cpp}`, `client/multiplayer.example.ini` : clé `damage_hook`.
- `client/src/game/combat.{h,cpp}` : `resolve_damage`, `damage_aggressor`, `do_damage` utilisable en session.
- `client/src/sync/hits.{h,cpp}` (détour, file, `apply`), `client/src/sync/hit_rules.{h,cpp}` (règles pures), `anim_filter` (`find_by_entity`), `npc` (`find_entity` public, `mark_dead`), `session.cpp` (publication, routage).
- `client/src/win/hooks.{h,cpp}`, `client/src/win/script_init.cpp` : détour facultatif.
- Tests :
  - `test_session.cpp` (11 tests), `test_hit_rules.cpp` (8), `test_protocol.cpp`, `test_config.cpp`, `test_anim_filter.cpp` ;
  - `session_host_main.cpp`, `session_fake_guest.sh`, `wine/run_scenarios.sh`.
- `tools/fake-guest/` : v12, `--hit-interval`, `--hit-target`, tests.
- Documentation :
  - spine : AD-2, AD-9, AD-11 (`self` = arme), AD-12, Q8 ;
  - `epics.md`, `docs/re/spike-j2.md` (section Q8) ;
  - `client/README.md`, `README.md`, `epic-3-context.md`, `deferred-work.md`.

**Revue :** 29 constats. 4 groupes corrigés : PROTOCOL.md (medium), règles du détour testées (medium), `damage_hook = 0` (low), AD-2 et projectiles (low). Les reports tiennent en 3 entrées `deferred`. Les rejets sont motivés dans le journal : 3 false, 12 low, 2 maybe-false.

**Revue de suivi recommandée :** oui. Deux entrées medium ont été corrigées. La décision du détour a été refactorisée après la première vérification (`hit_rules`), sans test du corps du détour. Risque nommé : une régression du chemin `hits.cpp::decide` vers `hit_rules`, par exemple la file ou la lecture de « réplique morte », qui ne se verrait qu'en jeu.

**Vérification :**
- `cmake --build client/build` : sans avertissement. `cmake --build client/build-tests`, puis `ctest` : 6/6 (client_tests 347/347, `session_fake_guest`, `wine_scenarios`, `wine_net`).
- `go test ./...` et `go build` dans `tools/fake-guest` : OK. `TestHitRequest` passe 8 fois de suite.
- `protocol/vectors/regen.sh --check` : 30/30.

**Risques résiduels :**
- Aucun test en jeu.
- Hypothèses non vérifiées :
  - D1 : le proxy `+0x30` est le personnage, et les pointeurs sont comparables ;
  - D5 : l'avatar déclenche-t-il des coups natifs ?
  - D7 : dégâts calculés avec les compétences de l'avatar ;
  - le thread du détour est celui d'OnProcess ;
  - les projectiles et la magie.
