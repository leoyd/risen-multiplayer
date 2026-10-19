---
title: 'Story 2.4b — Locomotion de l''avatar'
type: 'feature'
created: '2026-10-20'
status: 'done'
baseline_commit: '4813987fe01913667df6913dd88ee7f92d31d635'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-2-4-animations-avatar.md'
  - '{project-root}/protocol/PROTOCOL.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Les gestes ponctuels de l'avatar fonctionnent (2.4), mais pas la locomotion :
- au démarrage d'une course, l'avatar reste un instant en T-pose et glisse ;
- la marche arrière ne s'anime pas ;
- après une marche arrière, la course avant se bloque de nouveau.

La raison : Risen démarre une animation de locomotion avec un poids presque nul (`Move_Begin` 0,004, `Run_N_Back` 0,003), puis la fait monter et règle sa vitesse de lecture à chaque frame par `eCWrapper_emfx3ActorInstance::SetTargetWeight` et `SetPlaySpeed`. Ces appels ne sont pas transmis. Depuis la piste A′, ceux que le propre contrôleur de l'avatar lance de lui-même sont bloqués, puisqu'il le croit immobile.

**Approach:** Protocole v6 avec un événement fiable `MotionParam`. Les `SetTargetWeight` et `SetPlaySpeed` du héros local sont capturés, et seuls les changements sont envoyés. Ils passent dans le même flux daté que les `Motion` et sont rejoués sur l'enveloppe EMotionFX de l'avatar au même instant d'affichage, dans l'ordre.

## Boundaries & Constraints

**Always:**
- **Faits RE** (2026-10-19 et 20, désassemblage et relevés en jeu) :
  - la locomotion est réglée chaque frame par `gCCharacterMovement_PS` (Game.dll) d'après sa vitesse d'animation (`+0x1B8`, `+0x1D0`) ;
  - sur l'avatar, cette vitesse reste à 0, parce que `MoveTo` le recale (`OnUpdatedWorldMatrix`) ; `SetCurrentVelocity` (`+0x1A0`) n'y change rien (piste A, abandonnée) ;
  - chez le héros, par seconde : 200 à 400 `SetPlaySpeed` (0,03 au démarrage, 1,0 en course), 400 à 600 `SetTargetWeight` (cible 1,0 sur le type 0) et 100 à 300 `SwitchFadeOut`/`StopMotion`.
- **Piste A′ conservée** : sur l'avatar, les quatre réglages lancés par son propre moteur restent bloqués (`AnimFilter`, enveloppe en réplique). Seul notre rejeu passe, sous `ReplayGuard`.
- **`MotionParam` (26)**, v6 (AD-10 : liste fermée, étendue par cette story) :
  - champs : `net_id u32, ms u32, type u8, nature u8, valeur f32, durée f32`, soit 19 octets ;
  - nature 0 = `SetTargetWeight(type, valeur, durée)`, nature 1 = `SetPlaySpeed(type, valeur)` (durée à 0) ;
  - `BadValue` si le type est au-delà de 7, la nature au-delà de 1, ou une valeur non finie ;
  - vecteurs `motion_param.bin` et `motion_param_bad.bin`, codecs C++ et Go.
- **Émetteur** :
  - le hook range les `SetTargetWeight`/`SetPlaySpeed` de l'enveloppe du héros local dans l'anneau de l'`AnimFilter`, comme une nouvelle nature de `Capture`. L'ordre avec les `PlayMotion` est ainsi conservé ;
  - la frame les convertit en entrées `LocalParam` (`sync/local_capture`, portable) ;
  - `core/` n'envoie que les changements, par (type, nature) : écart supérieur à 0,01 sur la valeur, durée différente, ou premier réglage après un `Motion` de ce type. Hors session, rien ne part. Remise à zéro à l'entrée en session.
- **Récepteur** :
  - un `MotionParam` est accepté aux mêmes conditions qu'un `Motion` (`NetId` du pair, avatar présent et vivant) ;
  - il entre dans la même file d'attente, dans l'ordre d'arrivée, et part au même instant d'affichage, sous forme de commande `PlayAvatarParam` ;
  - la règle « seul le plus récent des types 0/1 » ne vaut que pour les `Motion`. Un `MotionParam` qui suit un `Motion` écrasé s'applique quand même : il règle le même emplacement ;
  - `sync/avatar` appelle `SetTargetWeight`/`SetPlaySpeed` sur l'enveloppe de l'avatar, sous `ReplayGuard`.
- **Bilan `debug`** : `MotionParam` envoyés, rejoués et ignorés. La ligne `locomotion (1 s)` reste.
- **Faux invité** : codec ; `--echo` renvoie aussi les `MotionParam` de l'hôte, redatés.

**Never:**
- Pas de transmission de `SwitchFadeOut`/`StopMotion` dans cette story : un `PlayMotion` remplace l'animation du même type. Ce sera ajouté si une animation reste visible à l'arrêt.
- Pas de PNJ ; pas de changement du snapshot (AD-7 inchangé).

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu |
|---|---|---|
| Démarrage | `Motion(Move_Begin, poids 0,004)`, puis `SetTargetWeight(0, 1, t)` | `Motion`, puis `MotionParam{poids 1, t}`, rejoués dans cet ordre |
| Valeur répétée | 300 `SetTargetWeight(0, 1, t)` identiques | un seul `MotionParam` |
| Accélération | `SetPlaySpeed` de 0,03 à 1,0 | un `MotionParam` par pas de plus de 0,01 |
| Nouveau `Motion` du même type | même valeur qu'avant | renvoyée (le premier réglage après un `Motion` part toujours) |
| Valeur non finie | NaN | ni envoyée, ni acceptée (`BadValue`) |
| Avatar absent ou mort | — | ignoré |
| Moteur de l'avatar | `SetTargetWeight` hors rejeu | bloqué (A′) |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` -- `MotionParam` (26), `kProtocolVersion = 6`, `valid()`.
- `client/src/core/session.{h,cpp}` -- entrée `LocalParam{type, kind, value, time}`, commande `PlayAvatarParam`. Table « dernier envoyé » par (type, nature), remise à zéro à l'entrée en session et, pour un type, à chaque `Motion` local de ce type. `PendingMotion` devient une file d'événements (`Motion` ou `MotionParam`). Compteurs du bilan.
- `client/src/sync/anim_filter.h` -- `CaptureKind::Param` et ses champs : `param_kind`, `value`, `time`.
- `client/src/sync/local_capture.{h,cpp}` -- `Param` → `LocalParam`.
- `client/src/sync/avatar.{h,cpp}` -- `on_wrapper_call` pousse la capture pour l'enveloppe du héros ; nouvelle fonction `play_param(net_id, type, kind, value, time)`.
- `client/src/game/entity.{h,cpp}` -- `set_target_weight(wrapper, type, w, t)` et `set_play_speed(wrapper, type, s)` sous SEH, par les pointeurs déjà résolus. Elles passent par le hook, que `ReplayGuard` laisse passer.
- `client/src/sync/session.cpp` -- routage de `PlayAvatarParam`.
- Tests : `test_protocol.cpp`, `test_session.cpp`, `test_local_capture.cpp`.
- `tools/fake-guest/` -- codec, écho, journal (nombre de `MotionParam` par seconde), tests.
- `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- journalisent `PlayAvatarParam` ; un scénario `anim` suivi d'un réglage est rejoué dans l'ordre.
- `protocol/PROTOCOL.md`, `client/README.md`.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/*` -- v6, `MotionParam`, règles de rejeu.
- [x] `client/src/core/protocol.*`, tests.
- [x] `client/src/core/session.*`, tests (matrice).
- [x] `client/src/sync/anim_filter.h`, `client/src/sync/local_capture.*`, tests.
- [x] `client/src/game/entity.*`, `client/src/sync/avatar.*`, `client/src/sync/session.cpp`.
- [x] `tools/fake-guest/*` -- codec, écho, scénario `param <instant s> <type> <poids|vitesse> <valeur> [durée]`.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh`.
- [x] `client/README.md`.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then tout passe.
- Given `go test ./...`, when il tourne, then il passe en v6.
- Given le build Windows, when on construit la DLL, then aucun avertissement, imports inchangés.
- Given Risen en hôte et `fake-guest --echo`, when le joueur démarre, court en avant puis en arrière, s'arrête, marche et tourne, then l'avatar démarre sans T-pose ni glissade, et suit marche avant, marche arrière et course sans se bloquer. Vérification manuelle par l'utilisateur.

## Implementation Notes

- Implémenté directement, sans sous-agent, sur la branche qui porte déjà la piste A′ (`4813987`). La piste A (`52e0d07`) a été annulée (`03e4eac`).
- **v6** : `hello*.bin` passent en 6 ; nouveaux vecteurs `motion_param.bin` (19 octets) et `motion_param_bad.bin` (nature 2 → `BadValue`).
- **`PendingMotion`** porte un `Motion` ou un `MotionParam` (`is_param`, `ms()`). La règle des types 0/1 ignore les réglages.
- **Capture** : `on_wrapper_call` reçoit aussi la durée. Pour l'enveloppe du héros, `SetTargetWeight` et `SetPlaySpeed` sont rangés dans l'anneau de l'`AnimFilter` (`CaptureKind::Param`), à côté des `PlayMotion`.
- **Rejeu** : `game::set_target_weight` et `set_play_speed` appellent les fonctions accrochées ; sous `ReplayGuard`, le détour laisse passer l'appel vers l'original.
- **Faux invité** : directive `param <instant s> <type> <poids|vitesse> <valeur> [durée]`. `--echo` renvoie les `MotionParam`. L'hôte simulé en envoie un avec chaque `Motion`.
- **De bout en bout** : `anim`, puis `param`, puis `anim` sont rejoués dans cet ordre (`PlayAvatarMotion` / `PlayAvatarParam`).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind, edge-case, verification-gap | Fin de rampe jamais envoyée (comparée au dernier envoi) : l'avatar reste à 0,99 ou à 0,008 | medium | → patch : une valeur finale exacte de 0 ou 1 part toujours ; tests (rampe finissant à 1,0, fondu à 0) |
| 2 | blind | Durée comparée exactement : si elle varie d'une frame à l'autre, un `MotionParam` par frame | low | → patch : tolérance de 0,005 s ; test |
| 3 | blind, verification-gap | Anneau de 64 places inondé par les réglages, qui peuvent évincer un `PlayMotion` | medium | → patch : réglages strictement identiques filtrés dans le hook (cache invalidé par un `PlayMotion` du type et au changement d'enveloppe), anneau porté à 256 |
| 4 | blind, edge-case | Un réglage placé avant le `Motion` gardé de son type, dans un même lot, visait l'animation remplacée | low | → patch : écarté (compté « écrasés ») ; test d'ordre mis à jour, plus un cas « autre type » |
| 5 | blind, edge-case | Domaine non borné (valeur ou durée négative, énorme ; durée non nulle pour une vitesse) | low | → patch : valeur dans [0, 100], durée dans [0, 60], durée nulle pour une vitesse, en C++, en Go et dans `PROTOCOL.md` ; tests de décodage |
| 6 | blind, verification-gap | Test « sans avatar » : horloge qui recule, dernière assertion vide | low | → patch : remplacé par Despawn puis Spawn, rien en attente |
| 7 | verification-gap | Remise à zéro de la déduplication à la nouvelle session : pas de test | medium | → patch : test |
| 8 | blind | Lacunes des tests du codec (type 8, Inf en décodage, canal de contrôle) | low | → patch |
| 9 | blind | Compteurs : un `MotionParam` jeté (file pleine) compté avec les « Motion jetés » ; tampon du bilan presque plein | low | → patch : libellé « file pleine », tampon de 640 |
| 10 | blind | `SwitchFadeOut`/`StopMotion` bloqués sur l'avatar sans être transmis : une couche pourrait rester mélangée | maybe-false | Les poids cibles transmis (fondu à 0) couvrent le cas courant ; à voir en jeu → defer si une animation traîne à l'arrêt |
| 11 | edge-case, blind | Avatar recréé en cours de session : les réglages ne sont pas renvoyés | low | Aucun Despawn/Spawn en cours de session aujourd'hui (seulement à la fin) → rejeté |
| 12 | edge-case | Réglages appliqués alors que le `Motion` précédent a été ignoré (AniId inconnu) | low | Rare : l'`AniName` précède toujours le `Motion` → rejeté |
| 13 | blind | Arguments sans objet passés par les détours `FadeOut`/`StopMotion` | low | Diagnostic seulement → rejeté |
| 14 | blind | `PendingMotion` à drapeau plutôt qu'un `std::variant` | low | Pas de défaut ; `ms()` couvre la lecture → rejeté |
| 15 | edge-case | Faux invité : surcomptage de `ParamsTx` si l'envoi échoue ; dépassement de `float32` dans le scénario | low | Plage du scénario désormais bornée à [0, 100] et [0, 60] → patch partiel ; surcomptage rejeté |
| 16 | verification-gap | Le compteur `MotionParam ignorés` n'est pas testé | low | → defer (diagnostic, comme les autres compteurs du bilan) |

## Verification

**Commands:**
- `cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: tout passe
- `cd tools/fake-guest && go vet ./... && go test ./...` -- expected: ok
- `protocol/vectors/regen.sh --check` -- expected: tous OK
- `cmake --build client/build` -- expected: sans avertissement

**Manual checks:**
- En jeu (`log_level = debug`, overlay désactivé) : `fake-guest --echo --duration 60s`, démarrages, course avant et arrière, arrêts, marche, virages. Fournir le log.
