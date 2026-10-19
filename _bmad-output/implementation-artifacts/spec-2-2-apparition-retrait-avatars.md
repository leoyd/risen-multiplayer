---
title: 'Story 2.2 — Apparition et retrait des avatars'
type: 'feature'
created: '2026-10-19'
status: 'done'
baseline_commit: 'd9cb98c385813fcde248cf4e825058b6ff82080e'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** En session, aucun des deux joueurs ne voit l'autre : la session de l'Epic 1 n'a ni avatar ni placement de l'invité (FR-6, FR-7, FR-8).

**Approach:** Protocole v3 avec `Spawn` et `Despawn`. À l'entrée en `InSession`, l'invité est téléporté près de l'hôte d'après le `Welcome`. Chaque pair envoie le `Spawn` de son héros. À réception, l'autre crée une copie temporaire du héros local, qui sert d'avatar, enregistrée dans la table `NetId` et dans l'`AnimFilter`. La transition `Ended` retire l'avatar, puis le détruit.

## Boundaries & Constraints

**Always:**
- **Faits RE (objdump, 2026-10-19)** : `?Kill@Entity@@QEAAXXZ` (Script.dll) saute vers `eCEntity::Kill` (Engine.dll, `0x1806a5dd0`). Celle-ci sort si le bit `0x8` de `+0x28` est déjà posé. Sinon, elle ajoute l'entité à une liste du moteur (destruction différée), pose ce bit, puis applique `KillIntern` aux enfants. `Despawn` utilisera `Entity::Kill`.
- **Messages (AD-10)**, version de protocole 3 : `Spawn` (20) = `NetId u32, type u8, x y z f32, lacet f32` (22 o de charge) ; `Despawn` (21) = `NetId u32`. `type` est gardé brut par le codec, et `0` = héros d'un joueur. Disposition dans `PROTOCOL.md`, vecteurs `spawn.bin` et `despawn.bin`, codec C++ et Go.
- **`NetId` (AD-6)** : 1 = hôte, 2 = invité. Chaque pair n'accepte que le `Spawn`/`Despawn` du `NetId` de l'autre, de `type` 0. Tout autre message est ignoré avec une ligne de log. Un `Spawn` reçu alors que l'avatar existe déjà est ignoré : il n'y a jamais deux avatars.
- **`core/session` décide, `sync/` exécute** : nouvelles commandes `Teleport{pose}` (invité), `SpawnAvatar{NetId, pose}` et `DespawnAvatar{NetId}`. `core/` sait si un avatar est présent.
- **Envoi** : l'hôte envoie `Spawn{1, sa pose du Welcome}` à l'acceptation. L'invité, à réception du `Welcome`, envoie `Teleport`, puis `Spawn{2, pose de téléportation}`. Si la fin est locale et qu'on est en session, on envoie `Despawn{son NetId}` juste avant `SessionEnd`.
- **Fin (AD-11)** : à chaque passage en `Ended` avec un avatar présent, `DespawnAvatar`. `Despawn` reçu : `DespawnAvatar`, la session continue.
- **`sync/avatar`** : unique propriétaire de l'avatar. Création par `Entity::Spawn(template du héros local, pose, temporary = true)`, puis `add_replica`. Retrait dans cet ordre : `remove_replica`, effacement de la table, puis `Entity::Kill`. `Kill` n'est appelé que si le joueur et `running` sont les mêmes qu'à la création. Sinon le monde est détruit, et l'avatar avec lui : on l'oublie sans y toucher. Tout appel est fait sous SEH. Un échec laisse le jeu en solo pour l'avatar, avec la raison au log.
- **Hooks en session** : `PlayMotion` et `SetRagDollEnabled` sont installés avec `OnProcess`, en tout ou rien, et passent par `AnimFilter::classify`. Ils bloquent les appels du moteur sur l'avatar et laissent passer tout le reste. Il n'y a pas encore de capture du héros local, qui arrive en 2.4.
- **Décision (2026-10-19, option recommandée retenue) — placement** : l'invité est téléporté 2 m devant le héros de l'hôte (axe avant = ligne 2 de la matrice), tourné vers lui (lacet + π). Le faux invité calcule la même pose pour son `Spawn`.
- **Décision (2026-10-19, option recommandée retenue) — allure** : le moteur est bloqué sur l'avatar dès la 2.2 (AD-8). Il peut rester figé, voire en T-pose, jusqu'à la 2.4.

**Never:**
- Pas de snapshot, pas de placement par frame, pas d'interpolation (2.3), pas de rejeu de `Motion` (2.4).
- Pas d'équipement ni d'arme sur l'avatar (J2).
- Aucun pointeur sur le réseau, ni gardé hors de la table `NetId`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu |
|---|---|---|
| Hôte accepte | `Hello` valide | `Welcome`, `OpenReliable`, puis `Spawn{1, pose}` |
| Invité reçoit `Welcome` | `Connecting` | `Teleport`, puis `Spawn{2}` |
| `Spawn` du pair | aucun avatar | `SpawnAvatar` |
| `Spawn` en double | avatar présent | ignoré, avec log |
| `Spawn` d'un mauvais `NetId` ou `type` | — | ignoré, avec log |
| Fin locale | `InSession`, avatar présent | `Despawn`, `SessionEnd`, `DespawnAvatar` |
| Timeout ou `SessionEnd` reçu | avatar présent | `DespawnAvatar` |
| Fin de monde | joueur changé | avatar oublié, sans `Kill` |
| `Despawn` reçu | avatar présent | `DespawnAvatar`, la session continue |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` -- `MsgType::Spawn = 20`, `Despawn = 21`, et leurs structs dans `ReliableMessage`. `kProtocolVersion = 3`. Mettre à jour le commentaire des types réservés.
- `client/src/core/session.{h,cpp}` -- `host_on_hello` (acceptation), `guest_on_control` (`Welcome`), `on_reliable` : nouvelle branche avant le `std::get<SessionEnd>`, faute de quoi c'est une exception. `end()` gère `Despawn` et `DespawnAvatar`. Nouvel état `avatar_present_` et commandes. Le décalage de 2 m et le lacet sont calculés ici (logique pure).
- `client/src/game/entity.{h,cpp}` -- résoudre `?Kill@Entity@@QEAAXXZ` dans `resolve_entities`, `kill(const Entity&)` sous SEH. Réutiliser `spawn`, `get_template`, `move_to`, `get_animation_ps`.
- `client/src/sync/avatar.{h,cpp}` (nouveau, Windows) -- table `NetId → {Entity, PS, joueur à la création}`, `spawn`, `despawn`, `teleport_player`. Matrice depuis la pose : ligne 0 = (cos ψ, 0, −sin ψ), ligne 1 = (0, 1, 0), ligne 2 = (sin ψ, 0, cos ψ), translation `m[12..14]`.
- `client/src/sync/session.cpp` -- `execute()` traite les 3 commandes ; `read_world` sert pour le contrôle du `Kill`.
- `client/src/win/hooks.cpp` -- `install_session` crée aussi les deux détours, qui appellent `sync::avatar::on_play_motion` et `on_set_ragdoll`. Log : `hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled)`.
- `client/tests/wine/run_scenarios.sh` -- nouveaux libellés de log (lignes 63, 74 et 83).
- `client/tests/session_host_main.cpp` -- journalise les commandes d'avatar et compte les avatars présents. Ce nombre ne dépasse jamais 1, et le log dit « avatars présents=N ».
- `client/tests/session_fake_guest.sh` -- scénario « rejoindre, `--kill`, rejoindre, quitter » : `SpawnAvatar` et `DespawnAvatar` appariés, jamais deux avatars.
- `tools/fake-guest/protocol/*`, `session.go`, `*_test.go` -- codec v3 `Spawn`/`Despawn`. À l'entrée en session : `-> Spawn netid=2`. Journaliser `<- Spawn`/`<- Despawn`. Sur `--quit` : `Despawn{2}`, puis `SessionEnd{1}`.
- **Ne pas toucher** : `sync/probe.*`, `AnimFilter`, sauf si besoin d'un appel.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/*` -- v3, `Spawn`/`Despawn`, vecteurs régénérés ; les vecteurs `hello*` passent en version 3.
- [x] `client/src/core/protocol.*`, `client/tests/test_protocol.cpp` -- codec et vecteurs.
- [x] `client/src/core/session.*`, `client/tests/test_session.cpp` -- commandes, envois, règles `NetId`, matrice ci-dessus.
- [x] `tools/fake-guest/*` -- codec, envoi et journal ; `TestAllVectorsCovered` couvre `spawn.bin` et `despawn.bin`.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- scénario sans double avatar.
- [x] `client/src/game/entity.*`, `client/src/sync/avatar.*`, `client/src/sync/session.cpp`, `client/src/win/hooks.cpp`, `client/CMakeLists.txt`, `client/tests/wine/run_scenarios.sh` -- côté jeu.
- [x] `client/README.md` -- procédure du test en jeu de la 2.2.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then tout passe, nouveaux tests compris.
- Given `go test ./...` dans `tools/fake-guest`, when il tourne, then il passe en v3.
- Given le build Windows, when on construit la DLL, then aucun avertissement, imports inchangés (KERNEL32, USER32, WS2_32).
- Given Risen en hôte (`probe = 0`) et le faux invité, when on enchaîne rejoindre, `--kill`, rejoindre, puis quitter, then l'avatar de l'invité apparaît à chaque session, disparaît à chaque fin, n'existe jamais en double, et rien ne crashe. Vérification manuelle par l'utilisateur, log à l'appui.

## Implementation Notes

- Implémenté directement, sans sous-agent, comme les stories précédentes. **REA n'est toujours pas utilisable** : Hopper démarre, mais le pont n'est jamais prêt (timeout de 120 s). `Entity::Kill` a été lu à l'objdump.
- **Protocole v3** : `hello*.bin` passent en version 3. `Spawn` fait 22 octets, `Despawn` 5. Dans le Go, le champ `type` de `Spawn` s'appelle `Kind`, parce qu'il entre en conflit avec la méthode `Type()` de l'interface `Message`.
- **Pose de l'invité** : `core::session::guest_pose_near`, et `guestPoseNear` dans le faux invité. Si le `Welcome` porte une pose nulle (l'hôte n'a pas pu lire la sienne), l'invité n'est pas téléporté à l'origine : il envoie un `Spawn` avec sa propre pose, que `read_world` lit désormais aussi en `Connecting`.
- **Ordre en fin locale** : `Despawn`, `SessionEnd`, transition `Ended`, puis `DespawnAvatar`. L'ordre est testé.
- **`sync/avatar`** (Windows) : sa table garde le héros local de la création. `despawn` appelle `Kill` seulement si `GetPlayer` rend ce même héros et que `IsGameRunning` est vrai ; sinon l'avatar est oublié sans y toucher. Le nombre d'appels moteur bloqués pendant la vie de l'avatar est journalisé au retrait.
- **Matrice, ligne « fin de monde → oublié sans `Kill` »** : `core/` est testé (un `DespawnAvatar` est émis à la fin de monde). Le choix entre `Kill` et l'oubli se fait dans `sync/avatar`, qui appelle le jeu. Il n'est vérifié qu'en jeu (README, étape 4 du test des avatars).
- **Hooks** : le libellé du log devient `hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled)`, pour rester distinct de celui de la sonde. Le banc Wine a été adapté, et il confirme que `?Kill@Entity@@QEAAXXZ` est résolu dans les vraies DLL.
- **Tests** : `ctest` passe 6/6, avec le nouveau scénario `avatars` de `session_fake_guest`. `go test ./...` est OK, la DLL compile sans avertissement, et ses imports sont KERNEL32, USER32 et WS2_32.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind, edge-case | `same_world` se fie à l'adresse du héros : si elle est réutilisée après un rechargement, `Kill` s'applique à une entité libérée, et un PS périmé reste dans l'`AnimFilter` | maybe-false (medium si vrai) | `docs/re/fin-de-monde.md` : l'adresse a changé à chaque rechargement observé. La réutilisation n'a jamais été vue, et `core/` ne détecterait pas non plus ce monde → defer, avec le signal `eCEntity::GetID` déjà reporté |
| 2 | edge-case | Avatar déjà détruit par le moteur avant le `Despawn` (hors fin de monde) : `Kill` sur de la mémoire libérée | maybe-false | Inconnu, déjà reporté en 2.1 (« copie détruite sans changement de joueur ») → defer, entrée existante complétée |
| 3 | blind, edge-case | `Spawn` à pose NaN ou infinie transmis à `Entity::Spawn` | low | Correction directe → patch : ignoré dans `core/session`, avec log et test |
| 4 | blind, edge-case | `avatar_present_` reste vrai si `avatar::spawn` échoue : les `Spawn` suivants sont ignorés | low | Le pair n'envoie qu'un seul `Spawn` par session, donc un nouvel essai n'arriverait jamais. La spec prévoit « solo pour l'avatar, raison au log » → rejeté |
| 5 | blind, edge-case | Le `Spawn` de l'invité porte la pose demandée, même si `MoveTo` a échoué | low | L'échec est journalisé (SEH, rare). La pose sera recalée par les snapshots en 2.3 → rejeté |
| 6 | blind, edge-case | Pose nulle de l'hôte, ou héros de l'invité illisible : `Spawn` à l'origine | low | Seulement si la lecture de la pose échoue (SEH). Journalisé et recalé en 2.3 → rejeté |
| 7 | edge-case | Lacet hors de ]−π, π] si le `Welcome` en porte un | false | Le lacet vient d'`atan2`, qui rend une valeur dans [−π, π] : après + π, une seule correction suffit |
| 8 | edge-case | Téléportation dans un mur, ou au-dessus d'un vide | low | Inhérent au placement retenu (décision 1 A). Le test en jeu le montrera → rejeté |
| 9 | edge-case | File du transport pleine : `Despawn` perdu | low | Comportement préexistant de toute commande. Le `SessionEnd`, ou le timeout, retire quand même l'avatar → rejeté |
| 10 | edge-case | `Entity::Spawn` lève une SEH après avoir créé l'entité : copie orpheline | low | Jamais observé, et la correction demanderait une recherche d'entité → rejeté |
| 11 | blind, edge-case, verification-gap | Le contrôle « Despawn avant SessionEnd » du script ne vérifie pas l'ordre | low | → patch : comparaison des numéros de ligne |
| 12 | verification-gap | `TestNominal` (Go) calcule l'attendu avec `guestPoseNear` lui-même | medium | → patch : `TestGuestPoseNear` avec des valeurs littérales, alignées sur le test C++ |
| 13 | verification-gap | `sync/avatar` (`matrix_of`, prédicats des hooks, ordre AD-11) n'a pas de test automatique | medium | Ce code appelle le jeu ; seul le test en jeu le couvre → defer (rendre `matrix_of` et les prédicats portables en 2.4) |
| 14 | verification-gap | Un ragdoll coupé et laissé passer était quand même compté comme bloqué | low | → patch : `!on` testé avant `classify` |
| 15 | blind | Pas de test côté invité (`Despawn{2}` avant `SessionEnd`, `Despawn{1}` reçu, `SessionEnd` reçu) | low | → patch : test ajouté |
| 16 | blind | `PROTOCOL.md` : « `NetId` de son pair » dans §4, « `Despawn` de `type` 0 » dans §8 | low | → patch : formulation corrigée |
| 17 | blind | §6 ne dit pas que les pairs v2 sont refusés | false | §6 et §8 l'imposent déjà en général : même version exigée, sinon `Reject{1}` |
| 18 | blind | Le log « Kill a levé une exception » est faux si l'ABI n'est pas résolue | false | En session, l'ABI est forcément résolue (hooks posés), et `spawn` refuse un pointeur nul |
| 19 | blind | `PlayMotion` lancé par le moteur pendant `Entity::Spawn`, avant `add_replica` | maybe-false | Sans gravité au pire : une animation d'attente. Le test en jeu le dira → rejeté |
| 20 | blind | Avatar gardé sans PS : le moteur peut l'animer | low | Rare, et journalisé en Warn → rejeté |
| 21 | blind | La session dépend désormais de trois hooks | false | Choix de la spec (« en tout ou rien »). Le banc Wine confirme leur résolution |
| 22 | blind | `classify` prend le verrou à chaque `PlayMotion` de chaque PNJ | maybe-false | Coût déjà présent dans la sonde, sans effet mesuré sur les images/s → rejeté, à mesurer si besoin |

## Verification

**Commands:**
- `cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: tout passe
- `cd tools/fake-guest && go test ./...` -- expected: ok
- `protocol/vectors/regen.sh --check` -- expected: tous OK
- `cmake --build client/build` -- expected: sans avertissement

**Manual checks:**
- En jeu, en hôte, avec le faux invité (`--duration 20s --kill`, puis `--duration 20s`) : l'avatar est visible 2 m devant, puis disparaît. Fournir `Script_Multiplayer.log`.
