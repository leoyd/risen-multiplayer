---
title: 'Story 3.8 : attaques natives des répliques (retour visuel des coups chez l’invité)'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_commit: '4f8fea2499c9805a0aa0e12accd72436507b011a'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/docs/re/degats-hote.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Chez l’invité, une réplique de monstre ne fait que rejouer les animations de l’hôte : son système de combat ne lance aucun mouvement, `HitTargets` ne s’exécute jamais (0 coup natif d’un PNJ sur le héros invité au test du 2026-10-23). Les coups reçus n’ont donc ni sang, ni son d’impact, ni recul adapté, ni flash `OverlayHit` : seul un recul générique (`StartStumble`, 3.6) est joué.

**Approach:** L’hôte détecte le début de chaque attaque d’un PNJ capturé et l’envoie (`NpcAttack{NetId, mouvement, NetId cible}`, protocole v15). L’invité fixe la cible de la réplique et lance ce mouvement par `PSCombatSystem::StartMove` : la chaîne native du jeu produit le coup. Les dégâts restent ceux de l’hôte (`HitOnGuest`) : le coup local sur le héros est appliqué pour son rendu seulement (PV rendus, mise à terre annulée).

Décisions (prises sans l’utilisateur, modifiables) :
- D1. Détection chez l’hôte, à chaque frame, pour chaque PNJ capturé : `PSCombatSystem::GetLastAttackMove` a changé depuis la frame précédente, ou `GetCurrentMove` vaut `GetLastAttackMove` après avoir valu autre chose → une attaque commence. Cible = `PSCombatSystem::GetCurrentTarget` traduite en NetId : héros de l’hôte → 1, avatar invité → 2, PNJ capturé → son NetId, sinon 0.
- D2. Chez l’invité : cible 1 → avatar de l’hôte, 2 → son héros, ≥ 1000 → la réplique, 0 → aucune cible fixée. `PSCombatSystem::SetCurrentTarget` puis `PSCombatSystem::StartMove(mouvement)`, hors garde de rejeu : l’animation locale du mouvement reste bloquée par le registre, l’animation visible est toujours celle rejouée depuis l’hôte.
- D3. Détour des dégâts chez l’invité, cible = son héros, agresseur = réplique : coup **cosmétique** — l’original s’exécute (effet d’impact, script de dégâts : recul adapté, `OverlayHit`), puis les PV d’avant sont rendus ; mise à terre ou mort annulée dans les détours `DownOrDeadBy*` (même mécanisme que la mesure de l’hôte). Autres agresseurs : inchangé (bloqué, effet d’impact).
- D4. Le recul générique de la 3.6 ne reste qu’en repli, **différé** (décision de l’utilisateur à la revue, 2026-10-23 : le `HitOnGuest` arrive en général avant le coup local). À l’application d’un `HitOnGuest` non mortel, un `StartStumble` est mis en attente 500 ms ; il est annulé si un coup cosmétique touche le héros dans les 500 ms avant ou après le `HitOnGuest`, sinon joué à l’échéance.
- D5. Nouvelle clé `native_attacks` (`1` défaut, `0` = ni envoi ni application de `NpcAttack`, comportement 3.6), interrupteur de diagnostic comme `damage_hook`.
- D6. Désaccords visuels acceptés : sang sans perte de PV (coup local touché, hôte raté), ou perte de PV avec le recul de repli (l’inverse).

## Boundaries & Constraints

**Always:** l’hôte reste seul maître des dégâts ; un coup cosmétique ne change jamais les PV du héros invité au bilan. Tout appel au jeu passe par `game/` (nom décoré, SEH, résolution en tout ou rien d’un bloc facultatif : échec → Warn et comportement 3.6). Un `NpcAttack` pour un NetId inconnu, mort ou hors session est ignoré (compté). Protocole : `PROTOCOL.md`, vecteur `npc_attack.bin`, version 15, tests C++ et Go, faux invité qui l’affiche.

**Never:** pas de réveil de l’IA de routine des répliques (hook de routine inchangé) ; pas de levée du blocage `PlayMotion` des répliques ; pas d’`EnableClassProcessing` ; pas de dégâts natifs gardés sur le héros invité.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Attaque détectée | hôte, PNJ capturé 1005, LastAttackMove 0→M, cible avatar | `NpcAttack{1005, M, 2}` envoyé une fois | N/A |
| Même attaque | LastAttackMove inchangé, CurrentMove inchangé | rien | N/A |
| Cible inconnue | cible hors registre | `NpcAttack{…, 0}` | N/A |
| Application | invité, réplique 1005 vivante, cible 2 | SetCurrentTarget(héros), StartMove(M) | SEH → Warn une fois, ignoré |
| NetId inconnu ou mort | invité | ignoré, compteur `ignorés` | N/A |
| Coup cosmétique | invité, réplique → héros, PV 53 | original appelé, PV remis à 53, détour rend son résultat | PV illisibles → bloqué + effet (3.6) |
| Coup cosmétique mortel | PV tomberaient à 0 | DownOrDeadBy* annulé, PV rendus | N/A |
| Repli de recul | HitOnGuest non mortel, aucun coup cosmétique dans les 500 ms avant ni après | StartStumble (3.6), joué 500 ms après le HitOnGuest | N/A |
| `native_attacks = 0` | config | ni envoi ni application ; comportement 3.6 | N/A |
| Protocole | `NpcAttack` reçu par l’hôte | ignoré, Warn | N/A |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` -- types et codec ; modèle exact : `Respawn` (type 25, v14). Nouveau type 26 `NpcAttack{u32 net_id, u32 move, u32 target_net_id}`, `kProtocolVersion` 15.
- `client/src/core/session.{h,cpp}` -- entrées `Local*`/commandes ; ajouter `LocalNpcAttack` (hôte → envoi fiable si InSession) et commande `ApplyNpcAttack` (invité), compteurs au bilan `[sync] snapshots (5 s)`.
- `client/src/core/config.{h,cpp}`, `client/multiplayer.example.ini` -- clé `native_attacks` sur le modèle de `damage_hook`.
- `client/src/game/combat.{h,cpp}` -- bloc facultatif `resolve_native_attacks` : `PSCombatSystem::GetLastAttackMove`, `GetCurrentMove`, `GetCurrentTarget` (sret), `SetCurrentTarget(const Entity&)`, `StartMove(gECombatMove)` (Script.dll).
- `client/src/sync/npc.{h,cpp}` -- hôte : suivi par PNJ capturé du dernier mouvement (D1), production de `LocalNpcAttack` ; invité : `attack(net_id, move, target)` (D2), `replica_entity` existe déjà.
- `client/src/sync/hits.{h,cpp}`, `client/src/sync/hit_rules.{h,cpp}` -- règle 3b : action cosmétique quand l’agresseur est une réplique (D3), réutiliser `measure`/`t_measured`/`ApplyGuard` ; horodatage QPC du dernier coup cosmétique pour D4.
- `client/src/sync/guest_health.cpp` -- `on_down_or_dead` gère déjà `measured_target()` ; `apply_hit` : StartStumble seulement en repli (D4).
- `client/src/sync/avatar.{h,cpp}` -- entité de l’avatar de l’hôte chez l’invité (cible 1).
- `client/src/sync/session.cpp` -- branchement des entrées/commandes, `native_attacks`.
- `protocol/PROTOCOL.md`, `protocol/vectors/` -- message 26, vecteur, vecteurs `hello*` régénérés (version).
- `tools/fake-guest/` -- codec Go, affichage `<- NpcAttack netid=… move=… cible=…`, tests.
- `client/tests/test_protocol.cpp`, `test_session.cpp`, `test_hit_rules.cpp`, `test_config.cpp`, `tests/wine/run_scenarios.sh`, `tests/session_fake_guest.sh` -- couverture de la matrice et des symboles.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/core/protocol.*`, `protocol/PROTOCOL.md`, `protocol/vectors/*` -- message 26 et v15 -- contrat.
- [x] `tools/fake-guest/*` -- codec, affichage, tests -- banc de l’hôte.
- [x] `client/src/core/config.*`, `multiplayer.example.ini` -- `native_attacks` -- interrupteur.
- [x] `client/src/core/session.*` -- `LocalNpcAttack`, `ApplyNpcAttack`, compteurs, refus hors rôle -- logique testable.
- [x] `client/src/game/combat.*` -- bloc `resolve_native_attacks` -- ABI.
- [x] `client/src/sync/npc.*`, `sync/avatar.*`, `sync/session.cpp` -- détection (D1) et application (D2).
- [x] `client/src/sync/hit_rules.*`, `sync/hits.*`, `sync/guest_health.cpp` -- coup cosmétique (D3), repli (D4).
- [ ] tests listés au Code Map -- chaque ligne de la matrice.
- [x] `client/README.md`, `docs/re/degats-hote.md`, spine (AD-9, AD-10, liste des messages), `epics.md` (story 3.8) -- documentation.

**Acceptance Criteria:**
- Given deux instances en session, when un monstre frappe l’invité, then l’invité voit sang, recul adapté et flash comme l’hôte, et ses PV ne baissent que du `HitOnGuest`.
- Given `native_attacks = 0`, when un monstre frappe l’invité, then comportement identique à la 3.6 (recul générique).

## Implementation Notes

- `NpcAttack` = **type 29**, pas 26 : le 26 est déjà `MotionParam` (v6). 13 octets, aucune valeur
  vérifiée au décodage.
- Invité : un `NpcAttack` n'est pas appliqué à l'arrivée mais au même instant d'affichage que le
  dernier snapshot reçu de la réplique (règle du `Death`), après les gestes et la pose dus, pour que
  le coup tombe avec l'animation rejouée (150 ms de retard). File de 8 par réplique ; une mort reçue
  jette les attaques en attente (comptées ignorées).
- Attaques natives effectives = `native_attacks = 1` **et** détour des dégâts posé **et** bloc
  `resolve_native_attacks` résolu (`win/hooks`) : sans détour, le coup d'une réplique blesserait
  vraiment le héros invité. Sinon : comportement 3.6, `NpcAttack` reçus ignorés et comptés.
- D1 : la première lecture d'un PNJ capturé ne fait que l'initialiser ; un mouvement 0 n'est jamais
  émis ; l'état repart à zéro quand le PNJ sort du groupe des 8 ou est perdu.
- Coup cosmétique : `t_measured` = héros invité pendant l'original, donc le détour `DownOrDeadBy*`
  existant l'annule (aucun changement de `guest_health::on_down_or_dead`). PV rendus par `sync/hits`
  (exception au « seul écrivain » `guest_health`, sans effet au bilan).
- Correctifs de revue : D4 différé (`hit_rules::stumble_step`, attente dans `guest_health`, vidée à
  la sortie de session, au `RespawnHero` et à la mise à terre ; immédiat si attaques natives
  inactives) ; `attack_started` sans second début quand LastAttackMove précède CurrentMove ; cible
  effacée avant `StartMove` si nulle ou introuvable ; règle 3c seulement avec attaques natives, tir
  ami par registre ; pas de coup cosmétique héros à terre ; Warn si PV non rendus ; attaques natives
  exigées aussi les détours `DownOrDeadBy*` (banc Wine : Warn, `Script_Game.dll` absente) ; Info une
  fois par session chez l'invité qui ignore des `NpcAttack` (native_attacks = 0).
- Banc : `session_host --hit-guest-ms` injecte aussi un `LocalNpcAttack{1000, 11, 2}` avant chaque
  coup (scénario `coups_subis` de `session_fake_guest.sh`).

## Spec Change Log

- 2026-10-23 (dev) : Code Map, type du message 26 → 29 (26 = `MotionParam`).

## Review Triage Log

| # | Source | Constat | Verdict | Suite |
|---|---|---|---|---|
| 1 | blind, edge | Recul de repli D4 : `HitOnGuest` avant le coup local → double recul | high | intent_gap tranché par l'utilisateur (repli différé), patch |
| 2 | edge | `attack_started` déclenche deux fois si LastAttackMove change une frame avant CurrentMove | medium | patch + test |
| 3 | blind, edge | Cible 0 ou introuvable : StartMove garde l'ancienne cible de la réplique | medium | patch (cible effacée) |
| 4 | blind, edge, verif | Règle 3c non soumise à `native_attacks` : `native_attacks = 0` ≠ 3.6 | medium | patch + test |
| 5 | verif | 3c : héros reconnu par le registre seul non traité comme tir ami | low | patch (correction directe) |
| 6 | blind, edge | Coups cosmétiques sur le héros à terre | medium | patch |
| 7 | blind, edge | Échec de restauration des PV journalisé en Info | medium | patch (Warn) |
| 8 | edge | Attaques natives actives sans détours DownOrDeadBy* : coup cosmétique mortel non annulé | medium | patch |
| 9 | blind | Commentaire de npc.h : fonction inexistante | low | patch |
| 10 | blind | Warn SEH de detect_attacks trompeur | low | patch (libellé) |
| 11 | blind | Désaccord `native_attacks` hôte/invité invisible | low | patch (Info une fois) |
| 12 | verif | Chemin cosmétique (restauration, annulation) sans test automatique | medium | defer : code DLL seul, test en jeu |
| 13 | verif | Repli D4 côté appelant sans test | medium | couvert en partie par la règle pure du patch 1 ; reste defer |
| 14 | blind | Effets du script de dégâts autres que les PV (statuts, poison) gardés sur le héros | maybe-false | defer (medium non vérifié) : à observer en jeu |
| 15 | blind, edge | Compteur « appliqués » compte les commandes, pas les StartMove réussis | low | rejeté : le journal debug `StartMove -> X` sert au diagnostic H1 |
| 16 | blind | D1 rate un enchaînement du même mouvement sans retour | maybe-false | rejeté (low si vrai) : journal debug des transitions, à observer |
| 17 | blind | `epic-3-context.md` et AD-11 non à jour pour 3.8 | low | defer : contexte régénéré à la clôture de l'epic |
| 18 | blind | Spec : type 26 dans Code Map, « 6/6 » | rejeté | correctif = éditer la spec |

## Design Notes

Hypothèses à trancher au premier test en jeu (journal debug) : H1, `StartMove` sur une réplique dont l’IA de routine est figée fait progresser le mouvement jusqu’à `HitTargets` ; H2, la détection de coup ne dépend pas de l’animation locale (bloquée par le registre) ; H3, `GetLastAttackMove` change à chaque attaque (sinon : transition de `GetCurrentMove` seule). Journaliser en debug, chez l’hôte, chaque transition `(CurrentMove, LastAttackMove)` d’un PNJ capturé, et chez l’invité le retour de `StartMove` et chaque coup cosmétique.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: build sans nouvel avertissement, 6/6.
- `(cd tools/fake-guest && go test ./...)` -- expected: ok.

**Manual checks (if no CLI):**
- `tools/installer-local.sh`, puis `~/Games/lancer-risen-coop.sh` : un monstre frappe l’invité → sang, recul, flash ; log invité `StartMove -> 1` et `coup cosmétique`, hôte `NpcAttack envoyé`.
