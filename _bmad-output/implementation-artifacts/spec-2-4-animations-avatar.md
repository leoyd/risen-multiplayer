---
title: 'Story 2.4 — Animations de l''avatar'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_commit: 'd786601fc82291e690f150f240fd94077684f6b8'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
  - '{project-root}/docs/re/spike-j1.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** L'avatar se déplace (2.3) mais reste en T-pose : il ne refait aucun geste du joueur distant (marche, course, saut, dégainer, attaques, mort ; FR-7).

**Approach:** Protocole v5 : `AniName` et `Motion` sur le canal fiable. Les `PlayMotion` du héros local sont capturés en session par l'`AnimFilter`, internés par nom et datés dans la base du tick. Chez le pair, ils sont rejoués sur l'avatar au même retard d'interpolation que les snapshots, par le chemin validé en 2.1 : nom → `GetMotion` → `SetMotion` → `PlayMotion`. Le faux invité gagne des animations horodatées dans ses scénarios et un mode `--echo`, qui renvoie au joueur ses propres gestes et déplacements comme ceux d'un invité.

## Boundaries & Constraints

**Always:**
- **Messages (AD-10)**, version 5 :
  - `AniName` (18) = `ani_id u16, nom str8` (1 à 255 octets) ;
  - `Motion` (19) garde la disposition de la 2.1. Le codec Go est écrit, et `motion*.bin` sort de la liste blanche ;
  - vecteur `aniname.bin`, codecs C++ et Go.
- **Émetteur** :
  - en session, la frame rafraîchit le PS local de l'`AnimFilter` (`set_local`) ; les hooks de session capturent ses `PlayMotion` (et non plus seulement bloquer les répliques) ;
  - la frame vide l'anneau. Chaque capture `PlayMotion` dont le nom est lisible devient une entrée de `core/session` `LocalMotion{nom, Motion sans net_id, ani_id ni ms}` ;
  - `core/` interne le nom : nouvel `AniId` → `AniName`, puis `Motion{NetId propre, ms = now − t0, …}`. L'espace d'`AniId` est propre à l'émetteur et remis à zéro à chaque entrée en session. Au-delà de 65 535 noms, plus rien ne part (une ligne de log) ;
  - hors session, rien n'est envoyé.
- **Récepteur** (`core/`) :
  - table `AniId → nom` du pair, remise à zéro à l'entrée en session ;
  - un `Motion` n'est accepté que du `NetId` du pair, avec un avatar présent. Il est ignoré, avec une ligne de log par `AniId`, si son `AniId` est inconnu ou si un scalaire n'est pas fini ;
  - les `Motion` acceptés attendent dans une file (256 au plus, le plus ancien est jeté et compté). À chaque tick, ceux dont `ms ≤ horloge(now) − 150` partent, dans l'ordre ; un `Motion` en retard part donc tout de suite. Avant le premier snapshot (horloge pas prête), un `Motion` part tout de suite ;
  - parmi ceux qui partent dans un même tick, seul le plus récent de chacun des types 0 et 1 est gardé ;
  - commande `PlayAvatarMotion{NetId, nom, Motion}`. Le bilan `debug` des 5 s compte les `Motion` rejoués, ignorés et écrasés.
- **`sync/avatar`** : rejeu sous `ReplayGuard`, avec un cache `nom → ressource` vidé à chaque retrait d'avatar. Un échec est journalisé une fois par nom. Le moteur reste bloqué sur l'avatar (hooks, 2.2).
- **Décision (2026-10-19, option recommandée retenue) — mort** :
  - `Death{NetId u32}` (24) fait partie de la v5, avec le vecteur `death.bin` ;
  - côté émetteur : **décision (2026-10-19, après la relecture, option A)** : `core/` envoie `Death{NetId propre}` quand `Entity::IsDead` du héros (`World::dead`) passe à 1 en session, une fois par mort (à nouveau après un retour à la vie). Le ragdoll n'est plus un déclencheur, car un ragdoll sans mort (chute, renversement) aurait figé l'avatar ;
  - côté récepteur, un `Death` du pair met l'avatar présent en ragdoll (commande `AvatarDeath`, `SetRagDollEnabled(1, 0, nul)` sous `ReplayGuard`, comme en 2.1b) ; il n'est plus ni déplacé ni animé jusqu'au retrait, et ses `Motion` en attente sont jetés ;
  - un nouveau `Spawn` lève l'état de mort ;
  - le faux invité accepte la directive `mort <instant s>`.
- **Portable et testé sous Linux** :
  - la conversion `Capture` → entrée de `core/` (nature `Motion` ou ragdoll), qui solde le report de la 2.1b ;
  - la matrice d'une `Pose` (`matrix_of`, AD-7), qui passe dans `core/pose.h`.
- **Faux invité** :
  - codecs `AniName`, `Motion` et `Death` ; il journalise ce qu'il reçoit (`<- AniName id=… nom=…`, et le nombre de `Motion` par seconde dans la ligne de bilan) ;
  - directive de scénario `anim <instant s> <type> <nom> [d00 d04 d08 d0C d10 d14 d18]`, à l'instant donné dans la boucle. Scalaires par défaut : 0.2 0 0 1 1 1 1, `flags` = jouer + descripteur ;
  - `scenarios/gestes.txt` reprend les noms des logs de la sonde (course, saut, attaque) ;
  - `--echo` (exclusif avec `--scenario`) renvoie chaque snapshot de l'hôte comme le sien, décalé de son `Spawn` (`Spawn` invité − position du `Welcome`), et chaque `Motion` de l'hôte, réinterné, comme le sien. Le joueur voit ainsi l'avatar refaire ses gestes avec 150 ms de retard.

**Never:**
- Pas d'arme en main, pas d'équipement (J2) : le dégainage se limite à l'animation.
- Le descripteur n'est jamais transmis brut, seulement les 7 scalaires (2.1).
- Pas de `Motion` pour les PNJ (J2).

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu |
|---|---|---|
| Premier geste | nom jamais envoyé | `AniName{0, nom}`, puis `Motion{ani_id 0}` |
| Geste répété | nom déjà interné | `Motion` seul |
| Nouvelle session | — | `AniId` repart de 0, côté émetteur comme côté récepteur |
| `Motion` à l'heure | `ms` > temps affiché | attend, puis part quand le temps affiché l'atteint |
| `Motion` en retard | `ms` ≤ temps affiché | part dans le tick |
| Types 0/1 en rafale | trois `Motion` de type 0 dus au même tick | seul le plus récent part |
| `AniId` inconnu | aucun `AniName` reçu | ignoré, une ligne de log |
| Scalaire NaN | — | ignoré, une ligne de log |
| Sans avatar | — | ignoré |
| Capture d'un nom illisible | — | rien n'est envoyé |
| Ragdoll du héros sans mort | `IsDead` reste à 0 | pas de `Death` |
| Mort, retour à la vie, mort | `IsDead` 0 → 1 → 0 → 1 | deux `Death` |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` -- `AniName` (18) et `Death` (24) dans `ReliableMessage`, `kMaxAniName = 255`, `kProtocolVersion = 5`. `Motion` existe déjà avec sa validation `valid()`.
- `client/src/core/pose.h` -- `pose_matrix(const Pose&, float (&m)[16])`, reprise de `matrix_of` (`sync/avatar.cpp`).
- `client/src/core/session.{h,cpp}` -- entrée `LocalMotion{name, proto::Motion}` et `LocalRagdoll{on}` ; commandes `PlayAvatarMotion` et `AvatarDeath` ; table d'internement (émetteur), table des noms (récepteur) ; file d'attente des `Motion` ; application dans `place_avatar` (même instant que `MoveAvatar`) ; bilan.
- `client/src/sync/local_capture.{h,cpp}` (nouveau, portable) -- `Capture` + nom → `std::optional<core::session::Input>`. Compilé dans `client_tests` comme `anim_filter.cpp`.
- `client/src/sync/avatar.{h,cpp}` -- `play(net_id, name, motion)` : cache nom → ressource (`game::get_motion`), `game::set_motion`, `game::play_motion` sous `ReplayGuard`, avec `game::replay_desc` et `replay_flag`. Hooks de session : capture du PS local, comme `probe::on_play_motion` et `on_set_ragdoll` (copier la logique, sans toucher à `probe`).
- `client/src/sync/session.cpp` -- dans `frame()` : `anim_filter().set_local(PS du héros)` (`set_local(nullptr)` hors monde chargé) ; vidage de l'anneau → noms (cache `ressource → nom`, vidé au changement de joueur) → entrées ; routage de `PlayAvatarMotion`.
- Réutiliser `game::motion_from_call`, `replay_desc`, `replay_flag` (`game/motion_desc`), `game::get_resource_name` et `get_motion`.
- Tests : `test_protocol.cpp`, `test_session.cpp`, `test_local_capture.cpp` (nouveau), `test_pose.cpp` (nouveau, ou dans `test_interpolation.cpp`).
- `tools/fake-guest/` -- `protocol/` (codecs), `scenario.go` (`anim`), `session.go` (réception, envoi, `--echo`), `main.go`, tests, `scenarios/gestes.txt`, README.
- `client/README.md` -- procédure du test en jeu (`--echo` et `gestes.txt`).
- **Ne pas toucher** : `sync/probe.*`.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/*` -- v5, `AniName`, règles de rejeu au §8, vecteurs.
- [x] `client/src/core/protocol.*`, `client/tests/test_protocol.cpp` -- `AniName`.
- [x] `client/src/core/pose.h`, test -- `pose_matrix`.
- [x] `client/src/core/session.*`, `client/tests/test_session.cpp` -- émetteur, récepteur, matrice ci-dessus.
- [x] `client/src/sync/local_capture.*`, `client/tests/test_local_capture.cpp`, `client/CMakeLists.txt`.
- [x] `client/src/sync/avatar.*`, `client/src/sync/session.cpp`, `client/src/win/hooks.cpp` -- côté jeu.
- [x] `tools/fake-guest/*` -- codecs, scénario `anim`, `--echo`, journal, `gestes.txt`.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- un scénario `gestes.txt` : l'hôte reçoit les `AniName` et les `Motion`, et les rejoue dans l'ordre (`PlayAvatarMotion` journalisés) ; en mode `--echo`, le faux invité reçoit des snapshots et renvoie les siens.
- [x] `client/README.md`, `tools/fake-guest/README.md`.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then tout passe.
- Given `go test ./...`, when il tourne, then il passe en v5, sans liste blanche pour `motion*.bin`.
- Given le build Windows, when on construit la DLL, then aucun avertissement, imports inchangés.
- Given Risen en hôte et `fake-guest --echo`, when le joueur marche, court, saute, dégaine et frappe, then l'avatar refait les mêmes gestes, 150 ms plus tard et en accord avec son déplacement. Vérification manuelle par l'utilisateur ; la validation à deux machines reste à faire.
- Given `fake-guest --scenario scenarios/gestes.txt`, when il tourne, then l'avatar court sur le cercle, saute et frappe aux instants prévus.

## Implementation Notes

- Implémenté directement, sans sous-agent.
- **v5** : `hello*.bin` passent en 5 ; nouveaux vecteurs `aniname.bin` (nom de course relevé par la sonde), `death.bin` et `aniname_empty.bin` (`BadLength`). Côté Go, le codec `Motion` est écrit, `ErrBadValue` sert au `Motion` invalide, et la liste blanche de `TestAllVectorsCovered` est retirée.
- **`on_reliable`** se terminait par un `std::get<SessionEnd>` : sans les nouvelles branches, un `AniName` ou un `Death` reçu aurait levé une exception.
- **Hook ragdoll** : nouvelle méthode `AnimFilter::is_local`. Une désactivation sur un PS qui n'est pas celui du héros local passe sans `classify`, donc sans être comptée parmi les appels bloqués (correctif de la 2.2 conservé).
- **`pose_matrix`** (`core/pose.h`) remplace le corps de `matrix_of`, désormais testé sous Linux. `sync/local_capture` porte le routage des captures et le teste aussi. Ces deux points soldent les reports de la 2.1b et de la 2.2.
- **Noms des ressources** : le cache `ressource → nom` de `sync/session` est vidé quand le héros change d'adresse. Le cache `nom → ressource` de `sync/avatar` est vidé à chaque retrait d'avatar. Un nom illisible ne part pas.
- **Faux invité** :
  - un `Motion` reçu n'est pas journalisé un par un, il est compté par seconde ; les `AniName` reçus le sont ;
  - `--echo` renvoie les snapshots dès leur réception (avec le tick de l'invité) et les `Motion` avec le `ms` de l'invité, ce qui garde les écarts relatifs ;
  - l'hôte simulé des tests Go envoie `AniName` et `Motion` (`SendMotions`) ; sa pose avance de 1 cm par tick.
- **Mort, option A** : `LocalRagdoll` est supprimé. `send_death` lit `World::dead` à chaque tick en session. Le hook ragdoll ne capture plus le héros local : il ne fait que bloquer le moteur sur l'avatar, via `classify_ragdoll`. Le code map mentionne encore `LocalRagdoll` : il est périmé sur ce point.
- **De bout en bout** (`session_fake_guest`) : course, saut et attaque sont rejoués dans l'ordre (`PlayAvatarMotion`), puis la mort (`AvatarDeath`), sans aucun `Motion` ignoré. L'écho : avatar déplacé.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind, edge-case | Tout ragdoll du héros (chute, renversement) envoie `Death` une fois par session : l'avatar reste figé en ragdoll, et une vraie mort ensuite ne part plus | medium | `on_local_ragdoll` ne regarde que `on`. Le déclencheur (« capture ragdoll → `Death` ») est dans l'intention figée → intent_gap, soumis à l'utilisateur |
| 2 | blind | `Death` appliqué dès la réception : l'avatar tombe 150 ms trop tôt, et les gestes en attente (le coup fatal) sont jetés | medium | → patch : mort appliquée quand le temps affiché atteint le dernier snapshot reçu, après les `Motion` dus ; tests réécrits |
| 3 | blind, edge-case | Un `Motion` daté loin dans le futur bloque toute la file | low | → patch : au-delà d'1 s d'avance, il part aussitôt ; test |
| 4 | blind, edge-case | `avatar::die` fige l'avatar même si le ragdoll échoue, ou sans PS | low | → patch : marqué mort seulement en cas de succès, avec un message distinct sans PS |
| 5 | blind, edge-case | Cache `ressource → nom` : échec mis en cache, taille non bornée, captures de l'ancien monde | low | → patch : échecs non retenus, borne de 4 096, anneau vidé au changement de héros |
| 6 | edge-case | `Motion` local à scalaire non fini envoyé, puis refusé par le pair ; `AniName` sans `Motion` si l'encodage échoue | low | → patch : validation avant l'internement ; test |
| 7 | verification-gap | Le routage des hooks de session (passer, bloquer, capturer) n'est pas testé | medium | → patch : `AnimFilter::classify_ragdoll`, portable et testé (désactivation d'un avatar, `ReplayGuard`, héros local) ; `classify` couvrait déjà `PlayMotion` |
| 8 | blind | Le test `pose_matrix` annonçait un produit vectoriel qu'il ne calculait pas | low | → patch : droite × haut = avant vérifié |
| 9 | blind | Pas de test pour l'épuisement des `AniId`, un `AniName` redéfini, la file vidée au retrait | low | → patch : trois tests |
| 10 | blind, edge-case | Faux invité : gestes dans l'ordre du fichier, entiers non validés, doublons d'écho, erreur de lecture ignorée dans un test | low | → patch |
| 11 | blind | Docs et aide pas à jour pour la v5 ; retard de l'écho mal décrit ; pas de scénario de mort livré | low | → patch : commentaires, aide, README, `scenarios/mort.txt` |
| 12 | blind, edge-case | Mort de l'hôte : `Death` jamais envoyé | false | Voulu : la mort de l'hôte termine la session (1.5) avant tout envoi, et l'invité retire l'avatar. Désormais écrit au §8 |
| 13 | blind | `gestes.txt` met l'attaque en type 0, qui pourrait être écrasée par la course | false | La sonde relève les attaques en type 0 (`spike-j1.md`), et aucune course n'est due au même tick dans le scénario |
| 14 | blind | L'anneau capture aussi les désactivations du héros, que la session ignore ; log « captures perdues » | low | Coût négligeable, log en `debug` seulement → rejeté |
| 15 | edge-case | Nom de plus de 255 caractères tronqué par `get_resource_name` | low | Noms relevés : environ 55 caractères → rejeté |
| 16 | edge-case | Faux invité : gestes d'une boucle sautée si la boucle de l'outil gèle plus d'une période | low | Outil de test, boucle de 5 ms → rejeté |
| 17 | verification-gap | Les compteurs `Motion` du bilan `debug` ne sont pas testés | low | Ceux des snapshots ne l'étaient pas non plus ; diagnostic seulement → defer |

## Verification

**Commands:**
- `cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: tout passe
- `cd tools/fake-guest && go vet ./... && go test ./...` -- expected: ok
- `protocol/vectors/regen.sh --check` -- expected: tous OK
- `cmake --build client/build` -- expected: sans avertissement ; imports KERNEL32, USER32, WS2_32

**Manual checks:**
- En jeu (overlay Steam désactivé, `log_level = debug`) : `fake-guest --echo --duration 120s`, en marchant, courant, sautant, dégainant et frappant ; puis `--scenario scenarios/gestes.txt`. Fournir `Script_Multiplayer.log`.
