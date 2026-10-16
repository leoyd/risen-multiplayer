---
title: 'Story 1.1 — Contrat binaire de session et codec'
type: 'feature'
created: '2026-10-16'
status: 'done'
baseline_commit: '4e1ac02b9811441ad690e3a8b70749f29aefc6c3'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** L'hôte, l'invité (DLL) et le futur faux invité Go (story 1.3) n'ont pas encore de contrat binaire commun. Sans lui, toute différence de format ou de version passerait en silence.

**Approach:** On écrit `protocol/PROTOCOL.md` v1 (messages de session seulement), un codec pur `core/protocol` testé sous Linux, et des vecteurs `.bin` de référence partagés C++/Go. On ajoute aussi la clé de config `log_level`.

## Boundaries & Constraints

**Always:**
- Little-endian, structs sérialisées champ par champ (jamais de `memcpy` de struct). Datagramme ≤ 1200 octets.
- Cadrage :
  - contrôle = `[u8 canal=2][u8 MsgType][charge]` ;
  - message fiable (unité confiée à KCP) = `[u8 MsgType][charge]`, sans octet de canal, que la story 1.2 ajoute autour des segments KCP.
- `MsgType` : `Hello=1`, `Welcome=2`, `Reject=3` (contrôle) ; `Heartbeat=16`, `SessionEnd=17` (fiable). `PROTOCOL.md` réserve 18 à 25 (AniName, Motion, Spawn, Despawn, HitRequest, HitOnGuest, Death, Respawn) sans les définir.
- Charges v1 :
  - `Hello` = `u16 version_protocole(=1)`, `str8 version_mod`, empreinte 4×(`u32 TimeDateStamp`, `u32 SizeOfImage`) dans l'ordre Game, Engine, Script, SharedBase, `str8 pseudo` (1 à 32 octets), `str8 équipement` (0 à 255).
  - `Welcome` = `u32 conv`, position 3×`f32`, lacet `f32`.
  - `Reject` = `u8 raison`.
  - `Heartbeat` = vide.
  - `SessionEnd` = `u8 raison`.
  - `str8` = `u8 longueur` + octets UTF-8.
- Codes de raison :
  - Reject : 1 protocole, 2 version du mod, 3 empreinte du jeu, 4 session pleine, 5 Hello invalide.
  - SessionEnd : 1 départ du joueur, 2 fin de monde, 3 mort de l'hôte, 4 erreur interne.
  - Un code inconnu se décode quand même : la valeur brute est conservée et journalisée par l'appelant.
- Compatibilité figée pour toujours : le début de `Hello` (`canal`, `MsgType`, `u16 version_protocole`) et la forme de `Reject`. Un hôte doit toujours pouvoir lire la version d'un `Hello` d'une autre version et lui répondre par un `Reject` compréhensible.
- Le décodage refuse les cas suivants avec une erreur typée, sans exception et sans lecture hors limites : tampon tronqué, canal ou type inconnu, longueur de texte hors bornes, octets en trop.
- `log_level` : `debug` | `info`, insensible à la casse, vide ou absent = `info`, toute autre valeur = erreur qui nomme le champ.

**Never:**
- Pas de snapshot, d'AniName, de Motion ni de Spawn (Epic 2).
- Pas de socket, de KCP ni de thread (story 1.2). Pas d'appel au jeu, pas de machine à états de session (story 1.4).
- Pas de code Go (story 1.3). Pas de dépendance externe ni de protobuf.
- Ne pas produire les `.bin` avec l'encodeur testé : ils viennent d'un hexdump annoté écrit à la main.

## I/O & Edge-Case Matrix

| Scénario | Entrée | Attendu | Erreur |
|---|---|---|---|
| Aller-retour | chaque message v1, textes de longueur maximale compris | encode → decode identique | — |
| Vecteur | `protocol/vectors/<msg>.bin` | décode vers les valeurs de `PROTOCOL.md`, réencode à l'octet près | — |
| Autre version | `Hello` avec `version_protocole=2` et une suite illisible | `peek_hello_version` → 2 | décodage complet refusé |
| Tronqué | chaque vecteur coupé à chaque longueur < n | erreur `Truncated` | aucun accès hors du tampon |
| Octets en trop | vecteur + 1 octet | erreur `TrailingBytes` | — |
| Inconnu | canal 9, `MsgType` 99 | `UnknownChannel` / `UnknownType` | — |
| Texte hors bornes | pseudo vide ou de 33 octets | `BadLength` au décodage, refus à l'encodage | — |
| Raison inconnue | `Reject` raison 200 | décodé, raison brute 200 | — |

</frozen-after-approval>

## Code Map

- `client/src/core/config.{h,cpp}` -- `parse_config` : y ajouter `log_level` sur le modèle de `probe` (vide = défaut, erreur via `fail`).
- `client/src/common/log.{h,cpp}` -- ajouter `set_debug(bool)` et `debug(msg)`, sans effet tant que `debug` n'est pas activé. Garder le format de ligne.
- `client/src/win/script_init.cpp:230-232` -- après `g_config = r.config`, appliquer `log_level` et l'ajouter à la ligne « config OK ».
- `client/src/core/version.h` -- `ModuleFingerprint` est réutilisé tel quel pour l'empreinte de `Hello`.
- `client/tests/test.h` -- mini-framework (`TEST`, `CHECK`, `CHECK_EQ`). Les tests sont enregistrés dans `client_tests`.
- `client/CMakeLists.txt` -- `mp_core` (+ `protocol.cpp`), `client_tests` (+ `test_protocol.cpp`, définition `MP_VECTORS_DIR`).
- `client/multiplayer.example.ini`, `client/README.md` -- documenter `log_level`.
- Le code existant est dans `namespace mp` : le codec va dans `mp::protocol`.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md` -- contrat v1 complet : canaux, cadrage, `MsgType`, charges, raisons, règles de compatibilité, limites ; un hexdump annoté par vecteur -- source de vérité commune C++/Go.
- [x] `protocol/vectors/*.bin` -- produits par `xxd -r` à partir des hexdumps de `PROTOCOL.md` :
  - valides : `hello`, `welcome`, `reject`, `heartbeat`, `session_end` ;
  - invalides : `hello_v2`, `hello_truncated`, `hello_long_nickname`, `unknown_type` ;
  plus `protocol/vectors/README.md` (rôle, valeurs attendues, procédure de régénération) -- référence indépendante de l'encodeur.
- [x] `client/src/core/protocol.{h,cpp}` -- structs des messages et enums `Channel`, `MsgType`, `RejectReason`, `EndReason`. Fonctions `encode_control`, `encode_reliable` (bool + `std::vector<uint8_t>`), `decode_control`, `decode_reliable` (résultat + `DecodeError`), `peek_hello_version`, plus noms lisibles des raisons pour le log -- codec pur.
- [x] `client/tests/test_protocol.cpp` -- toute la matrice I/O, plus la lecture et la comparaison des vecteurs à l'octet près -- preuve Linux.
- [x] `client/src/core/config.*`, `client/tests/test_config.cpp` -- `log_level` et ses tests (absent, debug, INFO, vide, invalide).
- [x] `client/src/common/log.*`, `client/src/win/script_init.cpp` -- niveau `debug` et son application.
- [x] `client/CMakeLists.txt`, `client/multiplayer.example.ini`, `client/README.md` -- intégration et documentation.

**Acceptance Criteria:**
- Given le build Linux des tests, when `ctest` tourne, then tous les tests passent, nouveaux et anciens.
- Given le build Windows, when on construit la DLL, then elle compile sans nouvel avertissement et sans nouvel import.
- Given le banc Wine (`wine_scenarios` de ctest), when il tourne, then les scénarios existants passent et la ligne « config OK » du log contient `log_level=info`.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | verification-gap, blind | `log_level = debug` n'a aucun effet observable : aucun appel à `log::debug`, l'assertion Wine ne prouve que la lecture de la clé | medium | Docs et titre de l'assertion corrigés (patch) ; le test de l'effet attend le premier appel `log::debug` (defer) |
| 2 | verification-gap, blind | `regen.sh --check` n'est pas lancé par ctest : `PROTOCOL.md` et les `.bin` peuvent diverger | medium | Aucun `add_test` dans `CMakeLists.txt` → patch (`protocol_vectors`) |
| 3 | edge-case, blind | `regen.sh` sans `nullglob` : le garde « aucun vecteur » ne se déclenche jamais | low | Glob littéral → `count=1` ; correction d'une ligne → patch |
| 4 | blind | `same(Welcome)` compare les flottants par `==` (perte du signe de -0.0 invisible) ; le vecteur Welcome ne réencode pas `*got` | low | Correction directe dans le test → patch |
| 5 | blind | `kMaxNickname` duplique `kMaxNicknameLength` sans lien | low | Une config pourrait accepter un pseudo refusé par l'encodeur ; `static_assert` → patch |
| 6 | edge-case, blind | `Welcome` accepte NaN/Inf | low | L'hôte est choisi par l'invité (NFR-9, pas d'adversaire) ; garde + nouvelle erreur → rejeté |
| 7 | edge-case, blind | Les `str8` acceptent UTF-8 invalide et caractères de contrôle (injection de lignes dans le log) | low | Pair de confiance ; garde → rejeté. À assainir au moment de journaliser un pseudo distant (story 1.4) |
| 8 | edge-case | Test « pseudo vide » : comportement indéfini si `hello.bin` manque | low | Seulement si le dépôt est cassé ; garde → rejeté |
| 9 | edge-case | Test `hello_v2` : `peek` avec taille 4 sur un tampon plus court si le vecteur manque | low | Même cas → rejeté |
| 10 | edge-case, blind | `regen.sh` : noms de bloc en double concaténés ; `.bin` orphelin non signalé par `--check` | low | Cas improbables ; nouvelles branches → rejeté |
| 11 | edge-case, blind | Matrice « chaque vecteur coupé » : seuls les vecteurs valides sont coupés | false | La ligne vise les messages valides ; couper un vecteur invalide n'a pas de résultat `Truncated` défini |
| 12 | blind | `Decoded::message` contient une valeur par défaut (Heartbeat) en cas d'erreur | low | Un appelant qui oublie `ok()` est hypothétique ; changer le type ajoute de la surface → rejeté |
| 13 | blind | Les parenthèses du motif `grep` de `run_scenarios.sh` ne servent à rien (BRE) | false | En BRE, `(` est littéral et la ligne « config OK (… ) » contient bien ces parenthèses : le motif est correct |
| 14 | blind | Règles de session absentes de `PROTOCOL.md` (délais, retransmission de Welcome, `conv=0`, comparaison des empreintes) | medium | Hors du contrat binaire de cette story ; nécessaires pour 1.3 et 1.4 → defer |
| 15 | blind | Empreintes Script/SharedBase de `hello.bin` « inventées » | false | `llvm-objdump -p` sur `risen_ori/bin` : Script `0x63c9b014`/`0x4d1000`, SharedBase `0x63c569b6`/`0x25a000` |
| 16 | blind | `SupportedVersion` ne connaît que Game et Engine : l'hôte ne peut pas vérifier Script/SharedBase pour `Reject{3}` | medium | Code antérieur (J0) ; à traiter avec la vérification du Hello → defer |
| 17 | blind | `encode_reliable` applique la limite de 1200 octets à une charge KCP | low | Sans effet en v1 (2 octets max) ; limite à fixer quand l'Epic 2 ajoutera de gros messages → rejeté |
| 18 | blind | `UnknownChannel` pour les canaux 0 et 1 passés à `decode_control` | low | Le transport trie par canal avant d'appeler le décodeur → rejeté |
| 19 | blind | Matrice : longueur valide mais octets absents non testés ; `version_mod`/équipement trop longs non testés au décodage ; priorité BadLength/Truncated | false | La coupure de `hello.bin` à chaque longueur couvre le cas longueur lue, octets absents ; 255 est le maximum d'un `u8` ; aucune priorité n'est spécifiée |

## Design Notes

L'encodeur valide les bornes avant d'écrire et retourne false plutôt que de tronquer. `PROTOCOL.md` explique pourquoi le début de `Hello` et `Reject` sont figés : un invité v1 face à un hôte v2 doit recevoir un `Reject{1}` qu'il sait lire, et non un silence. Le lacet suit la définition d'AD-7 (`atan2(m[8], m[10])`, radians).

## Verification

**Commands:**
- `cmake --build client/build` -- expected: DLL Windows construite, sans nouvel avertissement
- `cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: `client_tests` et `wine_scenarios` réussis
- `for f in protocol/vectors/*.bin; do xxd $f; done` -- expected: identique aux hexdumps de `PROTOCOL.md`
