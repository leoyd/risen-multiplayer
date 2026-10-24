---
title: 'Story 4.2 : dialogue de l’invité vu par l’hôte'
type: 'feature'
created: '2026-10-24'
status: 'done'
baseline_commit: 'ce55140c64e8e8e272f6c859aa58525c73431388'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/spec-4-1-dialogue-hote-vu-invite.md'
  - '{project-root}/docs/re/spike-j3.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** L’invité ne peut parler qu’à des répliques de PNJ dont l’IA est figée (registre) : leur dialogue natif risque de ne pas démarrer ou de rester muet et immobile. Et l’hôte ne voit ni n’entend rien du dialogue de l’invité ; son PNJ peut même repartir pendant que l’invité lui parle.

**Approach:** Symétrique de la story 4.1 (validée en jeu), plus la « tenue » du PNJ (G3 Together). Chez l’invité, le PNJ du dialogue est **libéré** du registre le temps du dialogue (routine et animations natives, pose de l’hôte ignorée) pour que le dialogue natif tourne ; l’invité envoie `DialogState{PNJ, en cours}` puis chaque réplique (`DialogLine`, locuteur 2 ou PNJ). Chez l’hôte, le PNJ est **tenu** (routine figée par le registre) pendant ce dialogue et les répliques sont rejouées sur l’avatar de l’invité ou sur ce PNJ (`sStartOutput` + `StartFacialAni`, voix pompée comme en 4.1).

Décisions (prises sans l’utilisateur, modifiables) :
- D1. Protocole **v17** : `DialogLine` accepte le locuteur 2 (invité → hôte seulement) ; nouveau message fiable `DialogState` type **31**, invité → hôte : `u32 npc_net_id` (≥ 1000), `u8 active` (0/1). `DialogLine` de l’invité : locuteur 2 ou le PNJ de son dialogue en cours.
- D2. Détection chez l’invité : transition de `InfoSystem::IsRunning` avec un partenaire qui est une réplique (NetId ≥ 1000) → `DialogState{id, 1}` ; fin (ou changement de partenaire, fin de session) → `DialogState{id, 0}`. Ses répliques : même filtre que l’hôte en 4.1 (héros ou partenaire, état lu à la capture).
- D3. Libération chez l’invité, pendant son dialogue : la réplique du partenaire sort du blocage de routine et d’animation, ses poses, `Motion`, PV et `NpcAttack` de l’hôte sont ignorés ; à la fin elle revient au registre (IA figée, rejeu repris). Si le dialogue natif ne démarre pas avec une réplique figée (hypothèse H1 ci-dessous), la libération doit se faire **dès la demande de dialogue** : le spec d’implémentation journalise en debug `PSDialog::IsBeingTalkedTo` / l’état du partenaire pour le trancher en jeu.
- D4. Tenue chez l’hôte : à `DialogState{id, 1}`, le PNJ natif de ce NetId (capturé ou retrouvé par GUID) a sa routine figée (registre, rôle tenu), à `{id, 0}`, à la fin de session, au retrait du PNJ ou après **60 s** sans fin, il est rendu. Un `DialogState` pour un PNJ mort, inconnu ou déjà en dialogue avec l’hôte : ignoré, compté.
- D5. Rejeu chez l’hôte : comme 4.1 (20 m, hôte pas lui-même en dialogue) ; locuteur 2 → avatar de l’invité ; PNJ → son entité native. Clé `dialogues` inchangée (couvre les deux sens).

## Boundaries & Constraints

**Always:** une seule source de vérité du dialogue : le jeu de celui qui parle ; ses effets y restent (FR-16). Le PNJ tenu ou libéré retrouve son état d’avant à la fin, quoi qu’il arrive (fin de session, mort, retrait, délai). Validation de l’échantillon et des NetId comme en 4.1. Protocole : `PROTOCOL.md`, vecteurs `dialog_state.bin` et `dialog_line_guest.bin`, `hello*.bin` régénérés, tests C++ et Go, faux invité capable d’envoyer `DialogState` et `DialogLine{2,…}`.

**Never:** pas de dialogue natif démarré par le mod ; pas de caméra ni de menu chez le spectateur ; pas de synchronisation des effets ; jamais `EnableClassProcessing`.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Début | invité : IsRunning 0→1, partenaire = réplique 1033 | réplique 1033 libérée, `DialogState{1033,1}` | N/A |
| Réplique invité | locuteur héros invité | `DialogLine{2, 3, …}` | N/A |
| Réplique PNJ | locuteur = réplique 1033 | `DialogLine{1033, t, …}` | N/A |
| Fin | IsRunning 1→0 | réplique re-gelée, `DialogState{1033,0}` | N/A |
| Hôte : tenue | `DialogState{1033,1}` | PNJ 1033 tenu (routine figée) | inconnu/mort → ignoré |
| Hôte : rejeu | `DialogLine{2,…}`, avatar à 5 m | voix + lèvres sur l’avatar | SEH → Warn une fois |
| Hôte : délai | 60 s sans `{1033,0}` | PNJ rendu, Warn | N/A |
| Fin de session pendant le dialogue | | hôte : PNJ rendu ; invité : réplique libérée puis rendue au jeu (comme `Ended`) | N/A |
| Hôte en dialogue avec 1033 | `DialogState{1033,1}` reçu | ignoré, compté | N/A |
| Hôte reçoit `DialogLine{1,…}` ou invité reçoit `DialogState` | | ignoré, Warn une fois | N/A |

</frozen-after-approval>

## Code Map

- `client/src/sync/dialogue.{h,cpp}`, `client/src/sync/dialogue_rules.{h,cpp}` -- 4.1 : file du détour (état capturé), `host_decide` / `guest_decide`, rejeu, `pump_routine_output` (`game/dialogue`) ; à étendre aux deux rôles.
- `client/src/sync/anim_filter.{h,cpp}` -- registre : `blocks_routine`, blocage de `PlayMotion` des répliques ; ajouter la libération temporaire d’une réplique et la tenue d’un PNJ capturé (routine bloquée sans rôle de réplique).
- `client/src/sync/npc.{h,cpp}` -- répliques (invité), PNJ capturés et GUID → entité (hôte) ; ignorer pose/Motion/PV/NpcAttack d’une réplique libérée.
- `client/src/core/session.{h,cpp}` -- `DialogLine` dans les deux sens, `DialogState` (invité → hôte), délai de 60 s (horloge QPC de la session), compteurs, nettoyage à `Ended`.
- `client/src/core/protocol.{h,cpp}`, `protocol/PROTOCOL.md`, `protocol/vectors/`, `tools/fake-guest/` -- type 31, v17.
- `client/src/win/hooks.cpp` -- détour de routine : `blocks_routine` doit refléter libération/tenue.
- Tests : `test_protocol.cpp`, `test_session.cpp`, `test_dialogue_rules.cpp`, `test_anim_filter.cpp`, Go, `tests/session_fake_guest.sh`.

## Tasks & Acceptance

**Execution:**
- [x] Protocole v17, `DialogState`, `DialogLine{2}` (C++, Go, vecteurs, PROTOCOL.md).
- [x] `core/session` + tests : deux sens, tenue côté hôte (délai, fin de session, mort, retrait), compteurs.
- [x] Registre + `sync/npc` + `sync/dialogue` : libération chez l’invité, tenue chez l’hôte, rejeu chez l’hôte, journal debug pour H1.
- [x] Documentation : README (procédure de test), `docs/re/spike-j3.md`, spine AD-13, `epics.md`.

**Acceptance Criteria:**
- Given deux instances, l’hôte près de l’invité, when l’invité parle à un PNJ, then le dialogue se déroule normalement chez l’invité, et l’hôte entend et voit (lèvres) l’avatar de l’invité et ce PNJ, qui ne repart pas avant la fin.
- Given la fin du dialogue, then chez l’invité la réplique reprend le rejeu de l’hôte et chez l’hôte le PNJ reprend sa routine.

## Implementation Notes

- Vecteurs : `dialog_state.bin` = `DialogState{1033, 1}` (6 octets) ; `dialog_line_guest.bin` =
  `DialogLine{2, 3, « INFO_Sara_1_PC_Hero_DE_%_PC_Hero.wav »}` (43 octets). Le décodeur accepte les
  locuteurs 1, 2 et ≥ 1000 dans les deux sens ; le sens est vérifié par `core/session` (hôte : locuteur 1
  ignoré ; invité : locuteur 2 et `DialogState` ignorés ; Warn une fois par session, un seul drapeau par
  rôle).
- Invité : `sync/dialogue::guest_frame` lit `IsRunning`/`GetNPC` à chaque frame et émet
  `LocalDialogState{NetId, 1}` **avant** les répliques de la frame, `{NetId, 0}` **après** (première et
  dernière répliques dans le dialogue). `core/session` émet `FreeReplica{NetId, true}` + `DialogState`,
  ignore pour cette réplique PV, gestes (comptés `npc_anim_.ignored`), pose et attaques (comptés
  `attacks_.ignored`) ; à la fin `FreeReplica{…, false}`, PV de l'hôte réinjectés (réécrits au prochain
  instant d'affichage). Death : re-figée avant `NpcDeath` ; Despawn et `Ended` : `ReleaseNpc` seul.
  Nouveau partenaire : l'ancien est re-figé et fermé avant le nouveau.
- Libération dans le registre : drapeau `TrackedEntity::freed` (`AnimFilter::set_freed`, réplique
  seulement) ; l'entrée reste (recherches, NetId). Tenue : liste séparée de PS de routine
  (`AnimFilter::hold_routine`), vidée par `clear()` ; `blocks_routine` les deux. `sync/npc` seul écrit.
- Hôte : `World::dialog_running/dialog_partner_ok/dialog_partner` (lus par `sync/dialogue::read_host_state`)
  pour refuser de tenir le partenaire de l'hôte. Un seul PNJ tenu ; délai `kDialogHoldTimeoutMs` (60 s) à
  l'horloge de la session ; rendu aussi à la sortie de sélection (Death ou Despawn), à `Ended`, et par
  `sync/npc::validate` (PNJ perdu) ou `release_all`. `ApplyDialogLine` porte le GUID chez l'hôte (PNJ
  tenu retrouvé par `GetEntity`). `sync/dialogue::replay` reçoit l'entité résolue par `sync/session`.
- H1 : `PSDialog::IsBeingTalkedTo` (Script.dll, facultatif, hors du bloc en tout ou rien) ; transitions
  par réplique en debug (`log_talk_states`). Libération à la demande non implémentée (en attente du test).
- Règle de rejeu commune : raison « joueur local en dialogue » (au lieu de « invité en dialogue »).
- Faux invité : `--dialog-every`, `--dialog-npc` ; `session_host` journalise `HoldNpc`, `UnholdNpc`,
  `ApplyDialogLine` ; scénarios `dialogues_invite` et `dialogues_netid`.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Suite |
|---|---|---|---|---|
| 1 | blind, edge | Tenue de 60 s jamais réarmée : dialogue long coupé | high | patch (délai d'inactivité) |
| 2 | blind, edge | Hôte qui parle ensuite au PNJ tenu ; partenaire illisible accepté | medium | patch |
| 3 | blind, edge | Échecs de tenue/libération côté sync jamais remontés à la session | medium | patch (entrée Local) |
| 4 | blind, edge | Réplique regelée dans son état natif jusqu'au prochain Motion | medium | patch (dernier Motion gardé puis rejoué) |
| 5 | blind | Fin tardive après mort/Despawn comptée « ignorée » | low | patch (Debug, non compté) |
| 6 | blind, edge, verif | GetNPC en échec étiqueté SEH ; monde déchargé sans fin ; transition non testée | medium | patch (règle pure testée) |
| 7 | edge | Journal H1 coupé pour tous si une réplique échoue | low | patch |
| 8 | blind | Pas de garde d'écho sur les rejeux capturés | low | patch (drapeau de rejeu) |
| 9 | verif | Les autres répliques suivent-elles l'hôte pendant le dialogue : non vérifié | medium | patch (test) |
| 10 | blind, edge | PROTOCOL §8 faux sur la fin de session ; refus de l'hôte muet pour l'invité | low | patch (doc) |
| 11 | blind | Effets du dialogue sur le PNJ (attaque, suite, commerce) divergents | maybe-false | defer (doc) |
| 12 | verif | Résolution du locuteur chez l'hôte non testée | medium | defer (code DLL) |
| 13 | edge, verif | Changement de partenaire dans une même frame : dernières répliques de A perdues | low | rejeté (cas rare) |
| 14 | blind | H1 sans repli (libération à la demande) | maybe-false | à trancher par le test en jeu en cours |

## Design Notes

Hypothèses à trancher en jeu : **H1**, le dialogue natif démarre-t-il avec une réplique encore figée (la demande de dialogue passe-t-elle par la routine du PNJ) ? Sinon, libérer à la demande (`PSDialog::IsBeingTalkedTo` côté PNJ, ou détour de la commande de dialogue). **H2**, la réplique libérée, en dialogue, se tourne et gesticule sans que l’hôte la déplace. **H3**, la tenue chez l’hôte (routine figée) laisse sa voix jouer (pompe de 4.1) et ses lèvres bouger.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: 6/6, sans nouvel avertissement.
- `(cd tools/fake-guest && go test ./...)` et `protocol/vectors/regen.sh --check` -- expected: ok.

**Manual checks (if no CLI):**
- `tools/installer-local.sh`, `~/Games/lancer-risen-coop.sh` : l’invité parle à un PNJ près de l’hôte ; log invité `DialogState … 1` puis `DialogLine envoyé`, log hôte `PNJ N tenu` puis `rejouée`.
