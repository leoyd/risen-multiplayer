---
title: 'Story 2.3 — Déplacements fluides de l''avatar'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_commit: 'e2b7266a8094f6d235c7c4b6ed808becbd4b171b'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** L'avatar de la 2.2 reste figé à sa pose de `Spawn` : il ne suit pas les déplacements de l'autre joueur (FR-7, NFR-2).

**Approach:** Ajouter le snapshot d'AD-7 sur le canal 1, en protocole v4. Chaque pair envoie à 20 Hz la pose de son héros, avec la mesure du temps prise à l'horloge. À la réception, `core/` tient une horloge distante et un interpolateur : affichage avec 150 ms de retard, Hermite pour la position, chemin le plus court pour le lacet, extrapolation de 250 ms au plus. L'avatar est placé à chaque frame par `MoveTo` sur la pose interpolée. Le faux invité gagne `--scenario` et une latence simulée.

## Boundaries & Constraints

**Always:**
- **Snapshot (AD-7)** : datagramme `u8 canal=1, u32 tick, u8 nombre`, suivi de 22 o par entité : `NetId u32`, `x y z f32`, `lacet f32`, `PV u16`. Au plus 54 entités par datagramme (1200 o). Le codec refuse une troncature, des octets en trop, un mauvais canal, ou plus de 54 entités. Disposition dans `PROTOCOL.md`, vecteurs `snapshot.bin` et `snapshot_truncated.bin`, codec C++ et Go.
- **Version 4** : les vecteurs `hello*.bin` passent en 4.
- **Envoi** : en `InSession`, `tick = floor((now − t0) / 50 ms)`, où `t0` est l'entrée en session. Un snapshot part à la première frame où le `tick` change, avec la pose de son héros lue à cette frame, sous son propre `NetId`. Sans pose lisible, rien ne part. **Décision : `PV = 0` en 2.3** ; la lecture passe par `PSSkills`, dont la disposition n'est pas établie, et ne sert qu'à partir de J2.
- **Transport** : commande `SendSnapshot{octets}`, envoyée au pair du KCP. Événement `SnapshotReceived{octets}`, accepté seulement depuis ce pair une fois le KCP ouvert ; sinon il est compté, et ignoré.
- **Horloge distante (`core/`)**, une par session : échantillon `s = local_ms − tick × 50`. Le décalage suit `min(s)`, et remonte lentement (1 % de l'écart par échantillon) quand `s` grandit. Elle est remise à zéro à chaque entrée en session.
- **Interpolateur (`core/`)** : jusqu'à 16 échantillons triés par `tick`, doublons ignorés. Il rend la pose au temps distant `t = horloge(now) − 150 ms` :
  - Hermite, avec des tangentes Catmull-Rom (dérivées des échantillons voisins) ;
  - lacet interpolé par le chemin le plus court ;
  - après le dernier échantillon, extrapolation à la dernière vitesse, au plus 250 ms, puis la pose reste figée ;
  - avant le premier, le premier échantillon.
  Il est remis à zéro à chaque `SpawnAvatar`. Une entité dont un champ n'est pas fini est ignorée.
- **Placement** : à chaque tick où un avatar existe et où l'interpolateur a au moins un échantillon, `core/session` émet `MoveAvatar{NetId, pose}`, que `sync/avatar` applique par `Entity::MoveTo`. Rien n'est journalisé par frame. Un bilan `debug` toutes les 5 s donne les snapshots reçus, les frames extrapolées et l'avance de l'horloge.
- **Faux invité** :
  - il envoie ses snapshots à 20 Hz (pose fixe, ou celle d'un `--scenario`) et compte ceux de l'hôte (« snapshots reçus » dans la ligne de bilan) ;
  - `--scenario <fichier>` : une directive par ligne, jouées en boucle à partir de la pose du `Spawn` : `cercle <rayon cm> <durée d'un tour s>`, `aller-retour <longueur cm> <vitesse cm/s>` (demi-tour sec à chaque bout), `pause <s>`, et `#` pour un commentaire. Exemples dans `tools/fake-guest/scenarios/` ;
  - **décision** : `--delay <durée>` et `--jitter <durée>` retardent chaque datagramme sortant. On simule ainsi les 100 ms sans `tc netem`, qui demande les droits root.

**Never:**
- Pas de `Motion` (2.4), pas de PNJ, pas de priorité entre entités : une seule par snapshot en J1.
- Pas d'attente de retransmission, pas de delta.
- Pas de socket ni d'horloge système dans `core/`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu |
|---|---|---|
| Mouvement linéaire | 20 Hz, sans perte | pose exacte au retard de 150 ms |
| Perte isolée | un snapshot sur 10 perdu | Hermite sur l'intervalle élargi, sans saut |
| Gigue | ±20 ms | le retard l'absorbe ; ordre rétabli par `tick` |
| Flux coupé | plus rien pendant 1 s | extrapolation de 250 ms, puis pose figée |
| Lacet | de +3,1 à −3,1 rad | passe par π, pas par 0 |
| Snapshot illisible ou NaN | — | ignoré, compté |
| Snapshot d'un autre `NetId` | — | ignoré |
| Nouveau `Spawn` | — | interpolateur remis à zéro |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` -- `SnapshotEntity`, `Snapshot`, `encode_snapshot`, `decode_snapshot`, `kMaxSnapshotEntities = 54`, `kProtocolVersion = 4`. Réutiliser `Writer`/`Reader`.
- `client/src/core/interpolation.{h,cpp}` (nouveau, pur) -- `RemoteClock` et `Interpolator` (`push(tick, Pose)`, `sample(t_ms) → {Pose, mode : interpolé / extrapolé / figé}`, `reset`). `Pose` vient de `core/session.h` : la déplacer dans un en-tête commun (`core/pose.h`) pour éviter une inclusion circulaire.
- `client/src/core/session.{h,cpp}` -- entrée `SnapshotIn`, commandes `SendSnapshot` et `MoveAvatar`. `t0_`, dernier tick envoyé, `RemoteClock`, `Interpolator`, compteurs du bilan. `World` porte la pose à chaque tick.
- `client/src/net/transport.{h,cpp}` -- `SendSnapshot`, `SnapshotReceived` ; filtre du pair (comme le canal 0) ; compteurs. Tests dans `client/tests/test_net.cpp`.
- `client/src/sync/session.cpp` -- `read_world` lit la pose dans tout monde chargé. Traduction des nouvelles commandes et du nouvel événement.
- `client/src/sync/avatar.{h,cpp}` -- `move(net_id, pose)` par `game::move_to` (réutiliser `matrix_of`), en silence ; erreur SEH journalisée une fois.
- `client/tests/test_interpolation.cpp` (nouveau), `client/tests/test_protocol.cpp`, `client/tests/test_session.cpp`, `client/CMakeLists.txt`.
- `client/tests/session_host_main.cpp` -- mesure le plus grand saut entre deux `MoveAvatar` successifs et l'écrit à la fin (« saut max N cm »). `client/tests/session_fake_guest.sh` -- scénario cercle avec `--delay 100ms --jitter 20ms --drop 10` : snapshots reçus des deux côtés, et saut max < 25 cm.
- `tools/fake-guest/` -- `protocol/` (codec du snapshot), `scenario.go` (nouveau : lecture du fichier, `PoseAt(t)`), `session.go` (envoi, réception, retard), `main.go` (options), tests Go, `scenarios/*.txt`, README.
- `client/README.md` -- procédure du test en jeu de la 2.3.
- **Ne pas toucher** : `sync/probe.*`, `AnimFilter`.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/*` -- v4, snapshot, vecteurs.
- [x] `client/src/core/protocol.*`, `client/tests/test_protocol.cpp` -- codec.
- [x] `client/src/core/pose.h`, `client/src/core/interpolation.*`, `client/tests/test_interpolation.cpp` -- horloge et interpolateur, avec la matrice ci-dessus et un test de fluidité : cercle à 300 cm/s, latence de 100 ms ±20 ms, 10 % de pertes, frames de 6 ms. Le plus grand pas entre frames reste sous 3 fois le pas nominal plus 2 cm, et l'écart à la trajectoire vraie reste sous 15 cm.
- [x] `client/src/core/session.*`, `client/tests/test_session.cpp` -- envoi à 20 Hz, réception, `MoveAvatar`, remise à zéro.
- [x] `client/src/net/transport.*`, `client/tests/test_net.cpp` -- canal 1.
- [x] `client/src/sync/session.cpp`, `client/src/sync/avatar.*` -- côté jeu.
- [x] `tools/fake-guest/*` -- codec, scénario, retard, envoi et réception.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- scénario sans téléportation.
- [x] `client/README.md` -- procédure du test en jeu.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then tout passe, nouveaux tests compris.
- Given `go test ./...`, when il tourne, then il passe en v4.
- Given le build Windows, when on construit la DLL, then aucun avertissement, imports inchangés.
- Given Risen en hôte et `fake-guest --scenario scenarios/mixte.txt --delay 100ms --jitter 20ms`, when l'avatar suit le cercle, les allers-retours et les demi-tours, then on ne voit aucune téléportation. Vérification manuelle par l'utilisateur.

## Implementation Notes

- Implémenté directement, sans sous-agent.
- **Protocole v4** : `hello*.bin` passent en 4 ; `snapshot.bin` fait 28 octets et `snapshot_truncated.bin` annonce 2 entités. Les tests Go et C++ qui figeaient la version utilisent désormais la constante.
- **`core/pose.h`** : `Pose` sort de `session.h`, que `session.h` réexpose par `using core::Pose`.
- **Mesures** (programme de mesure à part, mêmes simulations que `test_interpolation.cpp`) :

  | Cas | Plus grand pas | Plus grand écart | Frames extrapolées |
  |---|---|---|---|
  | Cercle, 100 ± 20 ms, 10 % de pertes | 3,1 cm (limite 7,4) | 0,7 cm | 4 sur 3 167 |
  | Allers-retours | 6,1 cm | 12,3 cm (au demi-tour) | — |
  | Cercle, 100 ± 60 ms, 30 % de pertes | 7,8 cm | — | 257 |

  Les allers-retours restent dans [−0,1 ; 600,1] cm. Le dernier cas dépassait la limite : elle est donc discriminante.
  - **Après la relecture**, avec la baisse de l'horloge limitée à 2 ms par échantillon : le cercle passe à 2,4 cm et le cas dur à 3,4 cm (150 frames extrapolées). De bout en bout, le saut maximal tombe à 1,3 cm, premier pas `Spawn` → `MoveAvatar` compris.
- **De bout en bout** (`session_fake_guest`, scénario « fluidité ») : 2 986 `MoveAvatar`, 1 891 cm parcourus, saut maximal de 2,1 cm.
- **Lacet** : il n'est pas extrapolé (seulement la position), sans quoi un demi-tour sec ferait tourner l'avatar sur lui-même pendant l'extrapolation.
- **Tests existants ajustés** : le test de l'invité « Welcome ou Reject ignorés » ne vérifie plus que l'absence de `Send` et de `SendReliable`, car un snapshot part désormais au même tick. Côté Go, `TestDupHello` est inchangé.
- **Faux invité** : `--delay` et `--jitter` utilisent `time.AfterFunc` avec une copie du datagramme. Le hasard est tiré dans la boucle principale (pas d'accès concurrent à `rng`).
- `PV = 0` (décision) ; `read_world` lit la pose dans tout monde chargé (un `GetPose` par frame).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | edge-case | Le snapshot du tick 0 de l'invité porte la pose d'avant la `Teleport` émise au même tick : chez l'hôte, l'avatar saute à l'ancienne position, puis glisse | medium | `send_snapshot` lisait `world` dans le tick du `Welcome`, et la `Teleport` ne s'exécute qu'après → patch : pas de snapshot au tick 0, avec un test côté invité |
| 2 | blind, edge-case | Nouveau minimum de l'horloge : le décalage retombait d'un coup, le temps affiché bondissait | low | Mesure : pas maximal de 3,1 cm, contre 2,4 une fois corrigé → patch : baisse limitée à 2 ms par échantillon, plus un test de 20 000 échantillons |
| 3 | blind, edge-case | Retour en arrière quand les snapshots reprennent après une extrapolation (jusqu'à 75 cm à 3 m/s) | maybe-false (medium si vrai) | Il faut une coupure de plus de 150 ms (trois pertes de suite). Mesuré : 4 frames extrapolées sur 3 167 avec 10 % de pertes. Corriger demande un fondu → defer |
| 4 | blind, edge-case | `MoveTo` rappelé à chaque frame après une SEH | low | Correction directe → patch : l'avatar n'est plus déplacé, et c'est journalisé |
| 5 | edge-case | La mesure de bout en bout ignorait le premier pas (`Spawn` → premier `MoveAvatar`) | low | → patch, et le seuil passe de 25 à 10 cm (mesuré : 1,3 cm) |
| 6 | blind | Le décodeur acceptait plus de 54 entités | low | → patch : `BadValue` en C++ et en Go (`ErrBadValue` ajouté), avec tests |
| 7 | blind | La constante de 50 ms existe en double | low | → patch : `static_assert` qui lie `kTickMs` à `kSnapshotTickMs` |
| 8 | blind | §5 de `PROTOCOL.md` ne couvre pas le snapshot | low | → patch : `Truncated`, `UnknownChannel` et `BadValue` complétés, plus l'activité du canal 1 |
| 9 | blind | Le faux invité comptait tout snapshot décodable | low | L'adresse est déjà vérifiée → patch : seule compte une entité de `NetId` 1 à champs finis |
| 10 | edge-case | README : « Ctrl-C → extrapolation » est faux, car Ctrl-C envoie aussitôt `Despawn` | low | → patch : la variante utilise `--kill` |
| 11 | blind, verification-gap | Paragraphe des tests du README dans le désordre | low | → patch |
| 12 | verification-gap | Le chemin de l'invité (snapshot envoyé sous `NetId` 2, `MoveAvatar{1}`) n'est pas testé | medium | → patch : test côté invité |
| 13 | verification-gap | L'assertion « `Reject` en session ignoré » était affaiblie | low | → patch : exactement un `SendSnapshot` |
| 14 | blind | Pas de test pour une pose infinie | low | → patch |
| 15 | verification-gap | La glu `sync/session.cpp` (pose en session, routage des snapshots) n'a pas de test automatique | medium | Ce code appelle le jeu, et `session_host` en est une copie → defer, avec le report du même type en 2.2 |
| 16 | blind | `MoveAvatar` à chaque frame, même quand la pose est figée | false | Voulu (contexte de l'epic : « placement à chaque frame par `MoveTo` », `sync/avatar` seul maître de la pose) |
| 17 | blind | Pose lue à chaque frame, même hors session | low | Un `GetPose` par frame, coût négligeable → rejeté |
| 18 | blind | Horloge observée à l'heure de la frame, pas à la réception | low | `OnProcess` tourne de 160 à 250 fois par seconde (4 à 6 ms) → rejeté |
| 19 | edge-case | Snapshots arrivés avant le `Spawn` effacés par la remise à zéro | low | Ils reviennent 50 ms plus tard ; l'avatar reste un instant à sa pose de `Spawn` → rejeté |
| 20 | edge-case | Faux invité : les envois retardés partent après la fermeture du socket | low | Seuls des acquittements après la fin sont perdus, ce qui est sans effet → rejeté |
| 21 | blind | `TestDelay` ne borne que le bas de l'intervalle | low | Le scénario de bout en bout (100 ± 20 ms, 10 % de pertes) couvre la survie du KCP → rejeté |
| 22 | verification-gap | Un datagramme de canal 1 illisible venu du pair compte comme activité | false | Conforme au §8 (« tout datagramme ») ; désormais écrit en §4 bis |

## Verification

**Commands:**
- `cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: tout passe
- `cd tools/fake-guest && go vet ./... && go test ./...` -- expected: ok
- `protocol/vectors/regen.sh --check` -- expected: tous OK
- `cmake --build client/build` -- expected: sans avertissement ; imports KERNEL32, USER32, WS2_32

**Manual checks:**
- En jeu (overlay Steam désactivé), en hôte : `fake-guest --duration 60s --scenario scenarios/mixte.txt --delay 100ms --jitter 20ms`. L'avatar suit la trajectoire sans à-coups. Fournir `Script_Multiplayer.log` avec `log_level = debug`.
