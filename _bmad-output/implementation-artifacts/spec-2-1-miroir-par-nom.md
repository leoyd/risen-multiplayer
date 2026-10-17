---
title: 'Story 2.1 — Miroir local par nom (lever le risque d’AD-8)'
type: 'feature'
created: '2026-10-17'
status: 'done'
baseline_commit: '4cdd4e37c0c5007ba6321746a2ce9ad0c73fc717'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/docs/re/spike-j1.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Le miroir v5 de la sonde rejoue les animations avec le pointeur de ressource et le descripteur brut du joueur. Or l’avatar distant n’aura que le nom de l’animation et quelques scalaires. Le chemin par nom (`GetMotion`) et le descripteur reconstruit ne sont pas validés (risque d’AD-8).

**Approach:** Le miroir rejoue chaque capture en passant par exactement ce que l’avatar recevra : le nom, plus un message `Motion` encodé puis décodé par `core/`, puis `GetMotion(nom)` → `SetMotion` → `PlayMotion(descripteur reconstruit)`. L’état partagé avec le hook passe dans un `sync::AnimFilter` réutilisable par l’avatar.

## Boundaries & Constraints

**Always:**
- **Faits RE (désassemblage, 2026-10-17)** :
  - `eCResource2::GetMotion(bCString const&)` est statique et ne passe pas de sret. Elle cherche dans le cache des ressources (et y crée l’entrée si besoin), puis appelle `QueryMotion`. Le pointeur rendu est **emprunté** : on ne le libère jamais, et c’est `SetMotion` qui prend la référence.
  - `eCWrapper_emfx3ActorInstance::PlayMotion` ne lit que les octets `0x00` à `0x1B` du descripteur : `f32 +0x00`, `f32 +0x04`, `u32 +0x08` (seuls 0 et 1 sont pris en compte), `f32 +0x0C`, `u32 +0x10`, `f32 +0x14`, `u32 +0x18` (comparé à 2). Le reste n’est pas lu. Un descripteur nul donne la lecture par défaut.
- **Descripteur reconstruit** : 0x60 octets à zéro, avec les 7 scalaires à leurs offsets. Il est construit et lu par `game/motion_desc`, sans aucune dépendance Windows.
- **`Motion` (MsgType 19, fiable)** : `net_id u32`, `ms u32`, `type u8` (0 à 7, sinon `BadValue`, nouvelle erreur de décodage), `ani_id u16`, `flags u8` (bit 0 = argument `flag`, bit 1 = descripteur présent, autres bits à 0, sinon `BadValue`), puis les 7 scalaires dans l’ordre des offsets. Total : 41 octets avec le `MsgType`. Si le bit 1 vaut 0, les scalaires sont à zéro, et le récepteur passe un descripteur nul.
- **Version du protocole → 2** (AD-10 : on ajoute un type). Le vecteur « version future » devient `hello_future.bin` (`version_protocole = 0xFFFF`), pour ne plus avoir à le renommer à chaque incrémentation. Le Go passe à la v2, sans codec `Motion` (celui-ci arrive en story 2.4).
- **`AnimFilter`** (AD-2) : une seule instance, verrou `std::mutex`. Il contient le PS local (capturé), l’ensemble des PS des répliques (bloqués), un anneau de 64 captures avec un compteur de pertes, et un compteur de blocages. Le garde de rejeu RAII est `thread_local` : il ne laisse passer que les répliques, et seulement sur son propre thread. Le PS local n’est **jamais** bloqué. Le hook range son thread dans le filtre ; la frame le journalise une fois et indique si c’est le thread d’`OnProcess`.
- **Miroir** : la copie est une réplique du filtre, et elle en est retirée avant d’être oubliée. Chaque capture est convertie en `Motion` (le nom est à part, `ani_id = 0`), encodée, décodée, puis rejouée. La première fois qu’on voit un nom, le log indique si `GetMotion(nom)` rend le pointeur capturé. Le tick compte les rejeux, les échecs et les écarts de pointeur, et journalise les scalaires décodés.

**Never:**
- Pas de pointeur de ressource capturé ni d’octets bruts du descripteur dans le rejeu (on ne s’en sert que pour comparer et journaliser).
- Pas de réseau, d’`AniName`, de codec Go pour `Motion`, ni de changement de la session.
- Le hook n’appelle jamais le jeu, hormis les lectures mémoire protégées de `game/`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu |
|---|---|---|
| Aller-retour `Motion` | `motion.bin` | décodé, réencodé à l’octet près |
| `Motion` invalide | type 8, bit de `flags` inconnu, tampon tronqué | `BadValue` ou `Truncated`, sans lecture hors limites |
| Descripteur | scalaires → 0x60 octets → scalaires | identiques, octets `0x1C`–`0x5F` à zéro |
| Filtre, PS local | aussi déclaré réplique | `Capture`, jamais `Block` |
| Filtre, réplique | sans garde / garde sur ce thread / garde sur un autre thread | `Block` / `Pass` / `Block` |
| Anneau plein | 65 captures | 64 restituées, 1 perte comptée |
| `GetMotion` échoue ou SEH | nom inconnu | rejeu sauté, échec compté, pas de crash |

</frozen-after-approval>

## Code Map

- `client/src/sync/probe.{h,cpp}` -- miroir v5 : `g_lock`/anneau, `g_replaying`, `replay_capture`, `on_play_motion`. À migrer vers `AnimFilter` et vers le rejeu par nom. Garder F6 et F4 (F4 passe aussi par le nom).
- `client/src/game/entity.{h,cpp}` -- ajouter `get_motion(nom, void*&)` : on construit un `bCString` avec `??0bCString@@QEAA@PEBD@Z`, on le détruit après l’appel, sous SEH. Ajouter `?GetMotion@eCResource2@@SAPEAVeCMotionResource2@@AEBVbCString@@@Z` (Engine.dll) à `resolve_entities`, en tout ou rien.
- `client/src/game/motion_desc.{h,cpp}` -- nouveau, pur : `MotionParams` ↔ descripteur de 0x60 octets.
- `client/src/sync/anim_filter.{h,cpp}` -- nouveau, portable : compilé dans la DLL et dans `client_tests`.
- `client/src/core/protocol.{h,cpp}` -- `MsgType::Motion = 19`, `struct Motion`, encodage et décodage fiables, `kProtocolVersion = 2`.
- `protocol/PROTOCOL.md` + `protocol/vectors/` -- v2, §3 et §4 pour `Motion`, hexdumps de `motion.bin` et `hello_future.bin`, `hello.bin` en v2, puis `regen.sh`. Mettre à jour `vectors/README.md`.
- `client/tests/test_protocol.cpp` + nouveaux `test_anim_filter.cpp` et `test_motion_desc.cpp` ; `client/CMakeLists.txt`.
- `tools/fake-guest/` (`protocol/protocol.go`, `session.go`, `session_test.go`, `main.go`, `README.md`) -- v2, `hello_future.bin`.
- `client/src/win/hooks.cpp` -- détour `PlayMotion` inchangé (il délègue à la sonde).
- `docs/re/spike-j1.md` -- section « Story 2.1 » : faits RE, scalaires observés en jeu, champs retenus.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/core/protocol.*`, `client/tests/test_protocol.cpp` -- `Motion`, v2 -- contrat.
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/*` -- disposition, vecteurs régénérés et vérifiés.
- [x] `tools/fake-guest/*` -- v2, `hello_future.bin`.
- [x] `client/src/game/motion_desc.*`, `client/tests/test_motion_desc.cpp` -- descripteur reconstruit.
- [x] `client/src/sync/anim_filter.*`, `client/tests/test_anim_filter.cpp`, `client/CMakeLists.txt` -- filtre et garde.
- [x] `client/src/game/entity.*` -- `get_motion`.
- [x] `client/src/sync/probe.*` -- miroir par nom via le filtre, diagnostics, thread du hook.
- [x] `docs/re/spike-j1.md`, `client/README.md` -- faits RE et procédure du test en jeu.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then tous les tests passent, nouveaux compris.
- Given les tests Go du faux invité, when `go test ./...` tourne, then ils passent en v2.
- Given le build Windows, when on construit la DLL, then elle compile sans avertissement, avec les imports inchangés (KERNEL32, USER32, WS2_32).
- Given `probe = 1`, F6 puis F7, when l’utilisateur marche, court, saute, frappe et meurt, then la copie reproduit les gestes comme en v5, et le log montre des rejeux par nom avec 0 échec et 0 écart de pointeur, ainsi que le thread du hook. Vérification manuelle par l’utilisateur.
- Given ce log, when on compare les scalaires, then les champs à transmettre sont consignés dans `docs/re/spike-j1.md`. Si un champ est toujours constant, il est noté, et sa suppression du `Motion` est reportée à la story 2.4.

## Implementation Notes

- Implémenté directement, sans sous-agent (pas de réponse de l'utilisateur sur ce point).
- **Protocole v2** : `hello.bin`, `hello_truncated.bin` et `hello_long_nickname.bin` passent en version 2. `hello_v2.bin` devient `hello_future.bin` (`0xFFFF`), et les tests de session C++ et Go qui codaient un `Hello` v2 « futur » en dur utilisent maintenant `0xFFFF`. `core/session` ignore un `Motion` reçu (log debug) : avant, `std::get<SessionEnd>` aurait levé une exception. Un test le couvre.
- **Go** : version 2 et `--bad-version` sur `hello_future.bin`. `motion*.bin` sont listés comme connus, sans être décodés : leur codec arrive en 2.4.
- **Sonde** : l'état partagé (verrou SRW, anneau, atomiques) est remplacé par `sync::anim_filter()`. Le compteur « PlayMotion du moteur sur la copie hors miroir » (diagnostic v3) est supprimé. La ligne `PlayMotion` journalise les 7 scalaires au lieu des 0x60 octets en hexadécimal. F4 passe aussi par le nom. Le cache nom → ressource est vidé au changement de joueur et à chaque activation de F7.
- **Matrice, ligne « `GetMotion` échoue ou SEH »** : pas de test automatique possible (il faut le moteur). Le chemin est relu dans le code (`motion_by_name` met en cache le pointeur nul, l'échec est compté, SEH dans `game::get_motion`). Il sera vérifié en jeu : le compteur d'échecs doit rester à 0.
- **Banc Wine** (`wine_scenarios`, vraies DLL du jeu, `probe = 1`) : hooks installés. Les nouveaux symboles (`GetMotion`, `bCString(char const*)`) sont donc bien résolus.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | verification-gap | Le passage Capture → `Motion` → arguments de rejeu (flags, descripteur nul ou reconstruit, `bool`) n'est vérifié qu'en jeu | medium | Inverser deux constantes passerait tous les tests → patch : `motion_from_call`, `replay_desc` et `replay_flag` vont dans `game/motion_desc` (code pur), avec un test des 3 combinaisons |
| 2 | verification-gap, blind, edge-case | Go en v2 sans codec `Motion` ; `motion*.bin` ajoutés à la liste blanche de `TestAllVectorsCovered` sans être testés | low | L'intent reporte le codec Go à la story 2.4 → defer (la 2.4 doit retirer ces deux entrées de la liste blanche) |
| 3 | verification-gap | Aucun résultat en jeu dans `spike-j1.md` (« à observer ») | low | Critère d'acceptation manuel, encore à faire par l'utilisateur → rien à corriger dans le code, vérification en jeu requise avant de clore |
| 4 | verification-gap, blind, edge-case | Sans le bit 1, des scalaires non nuls sont acceptés, alors que l'intent les veut à zéro | low | Écart à l'intent, correction d'une condition → patch : `valid()` → `BadValue`, avec un test et PROTOCOL.md §5 |
| 5 | verification-gap | Le nom lisible de `BadValue` n'est pas testé | low | → patch : un CHECK |
| 6 | blind | Commentaire Go « 18 à 25 réservés » | false | Le commentaire précise que 19 n'est pas encore décodé en Go, ce qui est exact (`ErrUnknownType`) |
| 7 | blind | Le faux invité ne peut pas envoyer de `Motion` pour tester de bout en bout la règle « ignoré » | low | Le codec Go est exclu par l'intent ; la règle est couverte par un test unitaire de `core/session` → rejeté |
| 8 | blind, edge-case | Flottants non finis (NaN, ±Inf) acceptés dans `Motion` | maybe-false | Seul le miroir local s'en sert ici ; l'effet d'un NaN dans `PlayMotion` est inconnu. À trancher quand les valeurs viendront du réseau → defer (medium non vérifié, story 2.4) |
| 9 | blind | `ani_id` renvoie à `AniName`, qui n'est pas encore défini | false | Voulu : `AniName` arrive en 2.4, annoncé dans PROTOCOL.md (« à venir ») ; une nouvelle version par story qui ajoute un type a été décidée |
| 10 | blind, edge-case | Un SEH de `GetMotion` est mis en cache (pointeur nul) et compté comme écart de pointeur | medium | Le compteur « 0 écarts » exigé par le test en jeu mélangeait échecs et écarts → patch : SEH ni mis en cache ni compté comme écart |
| 11 | blind, edge-case | Compteur diagnostique « PlayMotion du moteur sur la copie » supprimé | low | Diagnostic de la v3, retrait voulu (noté) ; le remettre ajoute de l'état au filtre → rejeté |
| 12 | blind | `bCString(char const*)` résolu à la fois dans `abi.cpp` et dans `entity.cpp` ; taille du `bCString` | low | Le destructeur était déjà résolu deux fois (cela existait déjà) ; la taille de 8 octets est vérifiée par `static_assert` dans `abi.h` et validée en J0 → rejeté |
| 13 | blind | Tests manquants : `Motion` tronqué ou avec octets en trop, sans descripteur | false | `motion.bin` fait partie de `kValidReliable`, donc coupé à chaque longueur et suivi d'un octet en trop ; le cas sans descripteur est ajouté (patch 4) |
| 14 | blind | `motion_desc` est dans `game/` alors que c'est du code pur | false | C'est la disposition ABI du jeu, et ses offsets relèvent de `game/` (AD-3). Rien n'y appelle le jeu ; la règle « seul `game/` appelle le jeu » est respectée |
| 15 | blind | Un mutex est pris à chaque `PlayMotion` | low | Mutex non disputé, quelques centaines d'appels par seconde, aucun coût mesurable → rejeté |
| 16 | blind | README : paragraphe collé, « v5 » sans référence | low | Correction directe → patch |
| 17 | blind | La base de `ms` diffère entre la sonde et la spec | false | La sonde n'a pas de session : `ms` n'y sert pas au rejeu ; la base de session vaut pour l'émetteur réseau (2.4) |
| 18 | blind | Validation propre à `Motion` dans `encode_reliable` (ad hoc) | low | Style, sans défaut concret → rejeté |
| 19 | edge-case | Des captures de l'ancien monde sont vidées après un changement de joueur, avec `GetFileTitleA` sur une ressource peut-être libérée | medium | `flush_captures` tourne dans la même frame que la remise à zéro → patch : on vide et on jette l'anneau au changement de joueur |
| 20 | edge-case | Copie détruite par le moteur sans changement de joueur : PS réplique pendant | maybe-false | Existait déjà en v5 ; il faudrait voir en jeu une copie temporaire détruite hors chargement → defer |
| 21 | edge-case | Type de motion hors 0..7 tronqué par `static_cast<u8>` | false | `read_motion_resource` rend une ressource nulle pour un type hors 0..7, le nom « (aucune) » est refusé, et `replay_by_name` sort avant toute conversion |
| 22 | edge-case | F4 hors miroir fausse le compteur d'écarts « depuis F7 » | false | F7 remet le compteur à zéro et vide le cache des noms |
| 23 | edge-case | Si l'anneau déborde, F4 se rabat sur une capture plus ancienne | low | Touche de débogage, il faut plus de 64 `PlayMotion` dans une frame → rejeté |

## Design Notes

L’aller-retour `Motion` dans le miroir ne coûte rien et prouve toute la chaîne de l’avatar, réseau excepté. Si le rejeu par nom marche avec ce message, la story 2.4 n’a plus qu’à le transporter. On garde les 7 scalaires plutôt que d’en choisir certains à l’aveugle : 28 octets suffisent, et les dumps du test en jeu diront lesquels sont constants.

## Verification

**Commands:**
- `protocol/vectors/regen.sh --check` -- expected: tous OK
- `cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: tout passe
- `cd tools/fake-guest && go test ./...` -- expected: ok
- `cmake --build client/build` -- expected: sans avertissement

**Manual checks:**
- En jeu avec `probe = 1` : F6, F7, puis marche, course, saut, attaque et mort. Fournir `Script_Multiplayer.log`.
