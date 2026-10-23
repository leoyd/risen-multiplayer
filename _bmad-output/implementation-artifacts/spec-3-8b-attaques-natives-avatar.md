---
title: 'Story 3.8b : attaques natives de l’avatar invité chez l’hôte'
type: 'feature'
created: '2026-10-24'
status: 'done'
baseline_commit: 'a9ded08dc3505b920159beec5437053f34e5e232'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-8-attaques-natives-repliques.md'
  - '{project-root}/docs/re/degats-hote.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Chez l’hôte, l’avatar de l’invité ne fait que rejouer les animations de l’invité ; ses coups n’existent qu’en `HitRequest`, appliqués par `PSDamage::DoDamage` direct, qui ne passe pas par `HitTargets`. L’hôte ne voit donc ni sang ni effet d’impact quand l’invité frappe un monstre (constaté en jeu le 2026-10-24).

**Approach:** Symétrique de la story 3.8 (validée en jeu). L’invité détecte le début de chaque attaque de son héros et l’envoie dans le même message `NpcAttack`, avec le NetId 2 (avatar invité), dans le sens invité → hôte. L’hôte, à l’instant d’affichage de l’avatar, fixe ses deux cibles (`PSCombatSystem` et `PSNpc`) et lance le mouvement (`StartMove`). Le coup local de l’avatar sur un PNJ reste bloqué par la règle 4 en gardant l’effet d’impact ; seuls les `HitRequest` blessent (inchangé).

Décisions (prises sans l’utilisateur, modifiables) :
- D1. Protocole inchangé en octets et en version (v15 non publiée) : `NpcAttack` devient bidirectionnel. Hôte → invité : `net_id` ≥ 1000 (3.8). Invité → hôte : `net_id` = 2 seulement ; tout autre `net_id` reçu par l’hôte est ignoré (Warn), comme un `NpcAttack` ≥ 1000 reçu de l’invité.
- D2. Détection chez l’invité : même règle pure `hit_rules::attack_started` sur son héros (`GetLastAttackMove`, `GetCurrentMove`), chaque frame en session. Cible traduite : réplique → son NetId, avatar de l’hôte → 1, sinon 0.
- D3. Chez l’hôte : file d’attaques de l’avatar appliquées à l’instant d’affichage de ses snapshots (comme les répliques en 3.8, au plus 8 en attente, vidée au retrait de l’avatar). Application : arme du dernier `Weapon` reprise si besoin (`avatar::rearm`), cibles fixées (PNJ capturé de ce NetId ; 1 ou 0 → cibles effacées), `StartMove`. Pas d’arme en main : attaque ignorée (comptée).
- D4. Couvert par `native_attacks` des deux côtés (0 = ni envoi ni application).

## Boundaries & Constraints

**Always:** l’hôte reste seul maître des dégâts (règle 4 inchangée : bloqué avec effet, pas d’effet sur le héros de l’hôte). Appels au jeu par `game/` (fonctions de la 3.8 déjà résolues). Compteurs `attaques natives` du bilan étendus aux deux sens. `PROTOCOL.md` (§ `NpcAttack`, §8), faux invité (il peut envoyer `NpcAttack{2,…}` : option de test), tests C++ et Go.

**Never:** pas de nouveau type de message ni de changement de version ; pas de levée du blocage `PlayMotion` de l’avatar ; pas de dégâts natifs gardés sur un PNJ de l’hôte venant de l’avatar.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Attaque de l’invité | invité, héros LastAttackMove 0→M, cible réplique 1004 | `NpcAttack{2, M, 1004}` envoyé une fois | N/A |
| Application | hôte, avatar présent, arme en main | à l’affichage : cibles = PNJ 1004, StartMove(M) | SEH → Warn une fois |
| Cible 1 ou 0 | hôte | cibles effacées, StartMove | N/A |
| Sans arme | hôte, rearm échoue | ignorée, compteur | N/A |
| NetId ≠ 2 reçu par l’hôte | `NpcAttack{1004,…}` | ignoré, Warn | N/A |
| NetId 2 reçu par l’invité | `NpcAttack{2,…}` | ignoré (NetId non annoncé) | N/A |
| `native_attacks = 0` | l’un ou l’autre | ni envoi ni application | N/A |
| Hors session, avatar absent | | ignoré | N/A |

</frozen-after-approval>

## Code Map

- `client/src/core/session.{h,cpp}` -- `on_local_npc_attack` (accepter l’invité avec `net_id` 2), `on_npc_attack` (hôte : `net_id` 2 → file de l’avatar), file et application à l’affichage dans la boucle de l’avatar (modèle : `Replica::attacks`, `place_npcs`), nouvelle commande `ApplyAvatarAttack{move, target}`, compteurs.
- `client/src/sync/hit_rules.{h,cpp}` -- traduction de cible côté invité (pure, testée).
- `client/src/sync/npc.cpp` -- `detect_attacks` (hôte, 3.8) : modèle de la détection ; ajouter celle du héros invité (ou dans `sync/session.cpp`).
- `client/src/sync/avatar.{h,cpp}` -- `attack(move, target_entity)` : `rearm`, cibles (`game::set_npc_current_target`, `game::combat_set_current_target`), `combat_start_move`.
- `client/src/sync/session.cpp` -- branchement de la détection et de `ApplyAvatarAttack`.
- `protocol/PROTOCOL.md`, `tools/fake-guest/` -- sens invité → hôte, option d’envoi du faux invité.
- Tests : `test_session.cpp`, `test_hit_rules.cpp`, `tools/fake-guest/*_test.go`, `tests/session_fake_guest.sh`.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/core/session.*` + tests -- sens invité → hôte, file de l’avatar, `ApplyAvatarAttack`, compteurs.
- [x] `client/src/sync/hit_rules.*` + tests -- cible côté invité.
- [x] `client/src/sync/npc.cpp` ou `sync/session.cpp`, `sync/avatar.*` -- détection (D2) et application (D3).
- [x] `protocol/PROTOCOL.md`, `tools/fake-guest/*`, `tests/session_fake_guest.sh` -- contrat et banc.
- [x] `client/README.md`, `docs/re/degats-hote.md`, `epics.md` (3.8b) -- documentation.

**Acceptance Criteria:**
- Given deux instances en session, when l’invité frappe un monstre, then l’hôte voit le sang sur le monstre et ses PV ne baissent que du `HitRequest`.

## Implementation Notes

- Core : `on_npc_attack` chez l'hôte route `NetId` 2 vers `on_avatar_attack` (avatar présent, vivant,
  file `avatar_attacks_` de `kMaxPendingAttacks`) ; tout autre NetId garde le Warn de la 3.8.
  Application dans `place_avatar` après `MoveAvatar` (aussitôt si l'horloge n'est pas prête), sautée
  si une mort attend ; file vidée et comptée ignorée au retrait de l'avatar (`clear_peer_avatar_state`)
  et à la mort appliquée. Invité : `LocalNpcAttack{2}` accepté (refusé héros à terre) ; un
  `LocalNpcAttack` d'un autre NetId chez l'invité reste « pas hôte ». Compteurs du bilan inchangés en
  format, ils comptent les deux sens. Info une fois par session chez l'hôte à `native_attacks = 0`.
- `ApplyAvatarAttack{move, target}` : `sync/session` traduit la cible (≥ 1000 → `npc::captured_entity`,
  PNJ non capturé → cibles effacées, Debug), `avatar::attack` reprend l'arme (`reassert_weapon`),
  ignore sans arme en main (compteur par avatar, Info la première fois), fixe `PSNpc` et
  `PSCombatSystem`, puis `StartMove` (Debug `StartMove -> X, main P`). Pas de repli « arme
  naturelle » : l'avatar est un humain armé.
- Détection invité : `npc::detect_hero_attack` (état oublié par `release_all`), cible par
  `hit_rules::guest_attack_target_net_id`.
- Banc : faux invité `--attack-move M` (avec `--hit-interval`) et `--attack-netid N` ;
  `session_host` journalise `[test] ApplyAvatarAttack` ; scénarios `attaques_avatar` et
  `attaques_netid` de `session_fake_guest.sh`.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Suite |
|---|---|---|---|---|
| 1 | edge, verif | Hôte : l'avatar d'un invité à terre attaque encore ; file non vidée à la mise à terre ni au Respawn | medium | patch + tests |
| 2 | blind | Attaque différée sur un PNJ déjà tué par le HitRequest | low | patch (cible 0) + test |
| 3 | verif | Ordre AvatarWeapon avant ApplyAvatarAttack au même instant non testé | medium | patch (test) |
| 4 | edge | Bloc non résolu : abandon silencieux | low | patch (Debug) |
| 5 | blind | Info à chaque coup de l'invité | low | patch (Debug) |
| 6 | blind | Constantes 1 et 1000 en dur | low | patch |
| 7 | edge, blind | Options du faux invité ignorées sans message | low | patch + tests Go |
| 8 | edge, blind | « appliqués » compte les commandes, pas les StartMove réussis | low | rejeté (comme 3.8 #15, journal debug) |
| 9 | blind | Cible du héros invité (GetCurrentTarget) peut être vide | maybe-false | rejeté : chez l'hôte le coup de l'avatar est bloqué avant l'original, l'effet d'impact ne dépend pas de CanDoMeleeDamage |
| 10 | blind | Cibles de l'avatar jamais effacées | low | rejeté |
| 11 | blind | Effets de bord d'un StartMove (aggro, crime) | maybe-false | defer : à observer en jeu |
| 12 | verif, blind | Traduction de cible côté hôte non testée | medium | defer (code DLL, comme 3.8) |
| 13 | blind | Sens de NpcAttack changé sans version | rejeté | v15 jamais publiée, branches locales |
| 14 | edge | Warn SEH « une fois » par avatar, pas par session | low | rejeté |
| 15 | blind | Recette du README incomplète, formulations | low | rejeté |

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: 6/6, sans nouvel avertissement.
- `(cd tools/fake-guest && go test ./...)` -- expected: ok.

**Manual checks (if no CLI):**
- `tools/installer-local.sh`, `~/Games/lancer-risen-coop.sh` : l’invité frappe un monstre → sang chez l’hôte ; log hôte `avatar … StartMove -> 1` et `coup natif bloqué : avatar invité -> PNJ N (seuls les HitRequest blessent)`.
