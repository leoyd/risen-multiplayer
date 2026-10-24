---
title: 'Story 4.1 : dialogue de l’hôte vu par l’invité'
type: 'feature'
created: '2026-10-24'
status: 'done'
baseline_commit: '18f956d66f186d48ced98944e3d207510790dfe4'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/docs/re/spike-j3.md'
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Quand l’hôte parle à un PNJ, l’invité ne voit ni n’entend rien : chez lui, la réplique de ce PNJ et l’avatar de l’hôte restent muets.

**Approach:** Selon AD-13 et le spike 4.0 (validé en jeu) : l’hôte détecte chaque réplique de son dialogue par le détour de `gCScriptProcessingUnit::StartOutput` et l’envoie dans un nouveau message fiable `DialogLine` (protocole v16). L’invité la rejoue sur l’entité du locuteur (avatar de l’hôte ou réplique du PNJ) par `sStartOutput` puis `StartFacialAni`, s’il est à 20 m au plus et pas lui-même en dialogue.

Décisions (prises sans l’utilisateur, modifiables) :
- D1. `DialogLine` = type **30**, fiable, hôte → invité dans cette story : `u32 speaker_net_id` (1 = héros de l’hôte, ≥ 1000 = PNJ annoncé), `u8 output_type` (0 à 3), `u8 sample_len` puis l’échantillon (1 à 128 octets, `[A-Za-z0-9_%.]`, suffixe `.wav`). Tout autre contenu → `BadValue` au décodage.
- D2. Émission chez l’hôte : seulement si `InfoSystem::IsRunning` et si le locuteur est son héros ou le partenaire de dialogue (`InfoSystem::GetNPC`), et seulement si ce PNJ a un NetId annoncé (sinon ignoré, compté). Pas de commentaires ambiants.
- D3. Rejeu chez l’invité : locuteur 1 → avatar de l’hôte ; ≥ 1000 → réplique de ce NetId. Ignoré (compté) si l’entité manque, est morte, est à plus de 20 m de son héros, ou si l’invité est lui-même en dialogue (`InfoSystem::IsRunning`). Pas de caméra, pas de menu, pas de `ZS_Talk`, jamais `StartSay`.
- D4. Nouvelle clé `dialogues` (défaut `1`, `0` = ni émission ni rejeu), interrupteur de diagnostic.
- D5. Sous-titres hors périmètre (H5 du spike non résolue).

## Boundaries & Constraints

**Always:** le détour de `StartOutput` ne fait que copier (entité locuteur, échantillon, type) dans une file bornée (32) sous verrou court et appelle toujours l’original ; la frame décide et émet. Appels au jeu par `game/dialogue` (bloc facultatif en tout ou rien : échec → Warn, session sans dialogues). Échantillon validé à l’émission et à la réception. Protocole : `PROTOCOL.md`, vecteur `dialog_line.bin`, version 16, `hello*.bin` régénérés, tests C++ et Go, faux invité qui affiche `<- DialogLine …`.

**Never:** pas de dialogue côté invité dans cette story (story 4.2) ; pas de synchronisation des effets du dialogue ; pas de sous-titres ; pas d’appel au jeu depuis le détour hors accesseurs (`GetSelfEntity`, lecture du `bCString`).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Réplique du PNJ | hôte en dialogue avec PNJ annoncé 1004, réplique de ce PNJ | `DialogLine{1004, type, « INFO_…wav »}` | N/A |
| Réplique du héros | hôte en dialogue, locuteur = son héros | `DialogLine{1, 3, …}` | N/A |
| Commentaire ambiant | pas de dialogue en cours | rien | N/A |
| PNJ non annoncé | partenaire sans NetId | rien, compteur `ignorés` | N/A |
| Rejeu proche | invité à 8 m de la réplique 1004 | `sStartOutput` + `StartFacialAni` sur la réplique | SEH → Warn une fois |
| Trop loin | invité à 25 m | ignoré, compteur | N/A |
| Invité en dialogue | `InfoSystem::IsRunning` chez l’invité | ignoré, compteur | N/A |
| Échantillon invalide | `../x.wav`, sans `.wav`, > 128 o | `BadValue` au décodage | journalisé |
| `dialogues = 0` | l’un ou l’autre | ni émission ni rejeu | N/A |
| Hôte reçoit `DialogLine` | | ignoré, Warn | N/A |

</frozen-after-approval>

## Code Map

- `client/src/game/dialogue.{h,cpp}` -- ABI du spike (`resolve_dialogue`, `spu_self_entity`, `read_bcstring`, `info_is_running`, `info_dialog_npc`, `start_output`, `start_facial_ani`) : à utiliser aussi en session (commentaire d’en-tête à mettre à jour).
- `client/src/sync/probe_j3.cpp` -- modèle de la file du détour et du rejeu (`sStartOutput` + `StartFacialAni`).
- `client/src/win/hooks.cpp` -- `install_session` : poser le détour de `StartOutput` (session) si `dialogues` et le bloc résolu ; `detour_start_output` existe pour la sonde (exclusivité sonde/session).
- `client/src/core/protocol.{h,cpp}`, `protocol/PROTOCOL.md`, `protocol/vectors/` -- modèle : `NpcAttack` (type 29, v15).
- `client/src/core/session.{h,cpp}` -- entrée `LocalDialogLine` (hôte → envoi), commande `ApplyDialogLine` (invité), compteurs au bilan `[sync] snapshots (5 s)`.
- `client/src/core/config.{h,cpp}`, `multiplayer.example.ini` -- clé `dialogues` (modèle `native_attacks`).
- `client/src/sync/npc.{h,cpp}` -- `replica_entity(net_id)` (invité), NetId d’un PNJ par entité chez l’hôte (registre `anim_filter().find_by_entity`, ou table GUID → NetId de la session).
- `client/src/sync/avatar.{h,cpp}` -- `entity()` : avatar de l’hôte chez l’invité.
- `client/src/sync/session.cpp` -- branchement (drain de la file, distance au héros, exécution).
- `tools/fake-guest/` -- codec Go, affichage, faux hôte qui peut émettre un `DialogLine` (test).
- Tests : `test_protocol.cpp`, `test_session.cpp`, `test_config.cpp`, Go, `tests/wine/run_scenarios.sh` (symboles), `tests/session_fake_guest.sh`.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/core/protocol.*`, `protocol/PROTOCOL.md`, `protocol/vectors/*`, `tools/fake-guest/*` -- message 30, v16, validation de l’échantillon.
- [x] `client/src/core/config.*`, `multiplayer.example.ini` -- clé `dialogues`.
- [x] `client/src/core/session.*` + tests -- `LocalDialogLine`, `ApplyDialogLine`, refus hors rôle, compteurs.
- [x] `client/src/win/hooks.cpp`, `client/src/sync/` (nouveau `dialogue.{h,cpp}` ou dans `session.cpp`) -- détour en session, file, filtre D2, rejeu D3.
- [x] `client/README.md`, `docs/re/spike-j3.md`, `epics.md` -- documentation et procédure de test en jeu.

**Acceptance Criteria:**
- Given deux instances en session, l’invité près de l’hôte, when l’hôte parle à un PNJ, then chez l’invité la réplique du PNJ et l’avatar de l’hôte disent chaque réplique, voix et lèvres, sans menu ni blocage.

## Implementation Notes

- Vecteur `dialog_line.bin` : locuteur 1004, type 0, `INFO_Sara_2_Sara_DE_%_Sara.wav` (37 octets).
- Échantillon : **5 à 128 octets** (nom de base non vide avant `.wav`, `.wav` seul refusé), `..` refusé
  partout (précision de D1 après revue : « 1 à 128 » ne pouvait être atteint avec le suffixe).
- État d'`InfoSystem` (`IsRunning`, `GetNPC`) lu dans le détour, au moment de la réplique (accesseurs
  sans effet de bord sous SEH, comme `GetSelfEntity`), pas à la frame. Décisions pures dans
  `sync/dialogue_rules` (testées sous Linux). Lignes par réplique en Debug ; le bilan de 5 s reste Info.
  Locuteur 2 refusé au décodage en v16 (D1 littéral) : la story 4.2 devra l'ouvrir.
- Hôte : le PNJ est traduit en NetId par son GUID (`LocalDialogLine{guid}`, table GUID → NetId de
  `core/session`), pas par le registre (qui ne tient que les 8 PNJ animés). Hors session : file vidée.
- Invité : rejeu aussitôt reçu (pas de retard d'affichage) ; `sync/dialogue::replay` vérifie entité,
  mort, distance (20 m) et `InfoSystem::IsRunning`, puis `sStartOutput` + `StartFacialAni` **sous
  `ReplayGuard`** (le registre bloque les animations natives de l'avatar et des répliques ; non vérifié
  par la sonde). Bilan remonté à la session par `LocalDialogReplayed{rejoués, ignorés}`.
- Détour de `StartOutput` posé dans les deux rôles (l'invité vide sa file sans rien émettre).
- Spine : AD-10 (`DialogLine`) et AD-12 (hook `StartOutput` en session) mis à jour.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Suite |
|---|---|---|---|---|
| 1 | edge, blind | IsRunning/GetNPC lus à la frame, pas à la capture : 1re ou dernière réplique perdue | medium | patch (lus dans le détour : accesseurs sans effet de bord sous SEH, écart assumé à la règle « Never » qui ne citait que GetSelfEntity et le bCString) |
| 2 | edge, blind | Répliques écartées sans compteur ni log | low | patch |
| 3 | verif | Filtre hôte et règles de rejeu sans test automatique | medium | patch (règles pures + tests Linux) |
| 4 | edge, blind | Warn de débordement de file sans limite | low | patch |
| 5 | edge | Warn à chaque DialogLine reçu par l'hôte | low | patch |
| 6 | blind, verif | Branche « PNJ mort » inatteignable ; avatar mort non testé | low | patch + tests |
| 7 | edge | dialogue::active reste vrai après hooks::remove | low | patch |
| 8 | blind | Journal Info à chaque réplique | low | patch (Debug) |
| 9 | blind | Échantillon : « 1 à 128 » faux (minimum 4), `.wav` seul accepté | low | patch |
| 10 | blind | Faux hôte n'émet jamais le locuteur 1 | low | patch |
| 11 | blind | Type 3 rejoué sur l'avatar non vérifié | low | patch (procédure de test) |
| 12 | edge, blind | Répliques qui se chevauchent / rafale | false | `StartOutput` arrête d'abord la sortie en cours (désassemblage du spike) |
| 13 | blind | Rien n'arrête une réplique rejouée en fin de session | low | rejeté (une réplique dure quelques secondes) |
| 14 | blind | Détour installé chez l'invité sans usage | low | rejeté : prépare la 4.2 (dialogue de l'invité) |
| 15 | blind | Réglage `dialogues` non échangé entre pairs | low | rejeté (même choix que `native_attacks`) |

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: 6/6, sans nouvel avertissement.
- `(cd tools/fake-guest && go test ./...)` et `protocol/vectors/regen.sh --check` -- expected: ok.

**Manual checks (if no CLI):**
- `tools/installer-local.sh`, `~/Games/lancer-risen-coop.sh` : l’hôte parle à un PNJ près de l’invité ; log hôte `DialogLine envoyé`, log invité `DialogLine … rejouée`.
