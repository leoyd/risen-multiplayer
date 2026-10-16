---
title: 'Story 1.3 — Faux invité de session (outil de test en Go)'
type: 'feature'
created: '2026-10-17'
status: 'done'
baseline_commit: 'c202cbbafd59790977e4b0777a080e88de506853'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Deux instances de Risen ne tournent pas sur la même machine, donc l'hôte (story 1.4) ne peut pas être testé contre un vrai invité. De plus, `PROTOCOL.md` ne décrit que les octets : les règles de session (délais, retransmission, comparaison) ne vivent que dans la spine.

**Approach:** On complète `PROTOCOL.md` avec les règles de session, puis on écrit `tools/fake-guest/`. C'est un invité scriptable en Go, jamais livré, qui suit ces règles. Son codec est vérifié contre `protocol/vectors/`, son KCP est le même `ikcp.c` compilé via cgo, et des options injectent des fautes. Il est testé contre un hôte simulé en Go.

## Boundaries & Constraints

**Always:**
- **`PROTOCOL.md` §8 « Règles de session » (normatif, v1 inchangée) :**
  - **Invité :**
    - `Hello` toutes les 500 ms jusqu'à `Welcome` ou `Reject` ;
    - abandon après 10 s sans réponse (« hôte injoignable ») ;
    - sur `Reject`, la raison est journalisée et l'invité arrête.
  - **Hôte :**
    - vérifie dans l'ordre déjà écrit au §6 ;
    - comparaisons exactes : `version_mod` octet par octet, les 4 empreintes ;
    - tire un `conv` aléatoire non nul ;
    - un `Hello` de la même `ip:port` reçoit le même `Welcome` (même `conv`) tant qu'aucun segment KCP portant ce `conv` n'est arrivé ; ensuite il est ignoré ;
    - un `Hello` d'une autre adresse pendant une session reçoit `Reject{4}`.
  - **Les deux pairs :**
    - le KCP n'existe qu'après `Welcome` ;
    - `Heartbeat` à 1 Hz sur le canal fiable ;
    - 5 s sans aucun datagramme du pair → fin « timeout », sans envoi ;
    - fin volontaire = `SessionEnd{raison}`, puis fermeture du KCP.
  - **Délais** mesurés à l'horloge monotone.
- **Module Go `tools/fake-guest/`.** `go.mod` (Go ≥ 1.22), aucune dépendance externe. Trois paquets :
  - `protocol` : codec v1 écrit à partir de `PROTOCOL.md`. Mêmes erreurs que le C++ (`Truncated`, `UnknownChannel`, `UnknownType`, `UnsupportedVersion`, `BadLength`, `TrailingBytes`), plus `PeekHelloVersion`.
  - `kcp` : enveloppe cgo d'`ikcp.c`, inclus depuis `client/third_party/kcp/` sans copie ni modification. Réglages `nodelay(1,10,2,1)`, MTU 1199, segments préfixés de l'octet de canal 0.
  - `main` : la ligne de commande.
- **CLI :** `fake-guest --host <ipv4> [--port 27960] [--nickname Invite] [--mod-version 0.1.0] [--duration 0]`, plus les fautes :
  - `--drop <0..100>` : pourcentage de datagrammes sortants jetés, avec graine `--seed` ;
  - `--quit` : à la fin de `--duration`, envoie `SessionEnd{1}` puis sort ;
  - `--kill` : à la fin de `--duration`, sort sans rien envoyer ;
  - `--dup-hello` : chaque `Hello` part deux fois, et un `Hello` supplémentaire part après le `Welcome` ;
  - `--bad-version` : envoie le `Hello` de `hello_v2.bin` (version 2).

  `--duration 0` = jusqu'à Ctrl-C, qui vaut `--quit`. Empreintes par défaut : celles de `hello.bin` (Steam x64 2023).
- **Journal sur stdout :** une ligne horodatée par transition (`Connecting`, `InSession conv=…`, `Ended raison=…`) et par message reçu ou envoyé (type et champs), sauf les heartbeats, qui ne sont comptés qu'une fois par seconde.
- **Codes de sortie :** 0 = fin normale (`--quit`, `--kill`, ou `SessionEnd` reçu) ; 2 = `Reject` ; 3 = hôte injoignable ; 4 = timeout ; 1 = erreur d'usage ou de socket.
- **Hôte simulé** (paquet de test, pas un binaire) : il applique le §8 côté hôte, avec un `conv` et des empreintes attendues configurables, et enregistre ce qu'il reçoit.

**Never:**
- Pas de code Go dans la DLL, pas de binaire `server/`, pas de faux hôte livrable.
- Pas de modification de `client/third_party/kcp/` ni du codec C++.
- Pas de snapshot, `Spawn` ni `AniName`/`Motion` (Epic 2). Pas de nouvelle version du protocole.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu | Erreur |
|---|---|---|---|
| Vecteurs | chaque `.bin` de `protocol/vectors/` | les 5 valides décodent vers les valeurs de `PROTOCOL.md` et se réencodent à l'octet près ; les 4 invalides donnent l'erreur annoncée | — |
| Nominal | hôte simulé qui accepte | `Hello` jusqu'au `Welcome`, `InSession`, heartbeats à 1 Hz dans les deux sens | — |
| Hôte lent | hôte qui ne répond qu'au 3e `Hello` | 3 `Hello` espacés d'environ 500 ms, puis `InSession` | — |
| Injoignable | aucun hôte | `Hello` pendant 10 s, puis sortie 3 | — |
| Version mod | hôte qui attend une autre version du mod | `Reject{2}` journalisé, sortie 2 | — |
| `--bad-version` | — | l'hôte lit la version 2 (peek) et répond `Reject{1}`, sortie 2 | — |
| `--dup-hello` | — | l'hôte reçoit des `Hello` en double ; chaque `Welcome` porte le même `conv` | — |
| `--drop 20` | pertes à 20 % | la session s'établit et les heartbeats passent par KCP | — |
| `--quit` | `--duration 2s` | l'hôte reçoit `SessionEnd{1}`, sortie 0 | — |
| `--kill` | `--duration 2s` | aucun `SessionEnd` ; l'hôte simulé constate le timeout 5 s après le dernier datagramme | — |
| Hôte silencieux | l'hôte se tait après le `Welcome` | `Ended raison=timeout` environ 5 s plus tard, sortie 4 | — |
| `SessionEnd` reçu | l'hôte envoie `SessionEnd{3}` | `Ended raison=mort de l'hôte`, sortie 0 | — |

</frozen-after-approval>

## Code Map

- `protocol/PROTOCOL.md` -- ajouter le §8 ; §6 (ordre de vérification de l'hôte) et §7 (vecteurs) à garder tels quels.
- `protocol/vectors/*.bin` + `README.md` -- référence des tests Go. Lire le dossier par un chemin relatif au module (`../../protocol/vectors`).
- `client/src/core/protocol.{h,cpp}` -- modèle de comportement du codec (bornes, erreurs, raisons inconnues conservées). Ne pas modifier.
- `client/src/net/transport.cpp` -- modèle KCP : octet de canal 0 + segment ≤ 1199, `ikcp_getconv`, contrôle du `conv` et de l'adresse.
- `client/third_party/kcp/{ikcp.c,ikcp.h}` -- à inclure via cgo (par exemple un `kcp.c` qui fait `#include "../../../client/third_party/kcp/ikcp.c"`). Pas de copie.
- `client/CMakeLists.txt` (branche Linux) -- enregistrer un test ctest `fake_guest` (`go test ./...` dans `tools/fake-guest`) si `go` est trouvé, sur le modèle de `protocol_vectors`.
- `client/README.md`, `tools/fake-guest/README.md` -- usage, fautes, codes de sortie.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md` -- §8 « Règles de session », tel que décrit ci-dessus -- source de vérité pour le faux invité et pour la story 1.4.
- [x] `tools/fake-guest/go.mod`, `protocol/` -- codec v1 et tests sur les vecteurs -- parité avec le C++.
- [x] `tools/fake-guest/kcp/` -- enveloppe cgo (create, nodelay, mtu, send, input, update, check, recv, release ; sortie des segments par un tampon C ou un rappel exporté) et test d'échange en mémoire avec 20 % de pertes -- même KCP que la DLL.
- [x] `tools/fake-guest/main.go` (+ fichiers de session) -- machine à états de l'invité, options, journal, codes de sortie -- l'outil.
- [x] `tools/fake-guest/*_test.go` -- hôte simulé et toute la matrice I/O, avec des délais réduits dans les tests (paramètres internes, sans changer les valeurs par défaut) -- preuve.
- [x] `client/CMakeLists.txt`, `client/README.md`, `tools/fake-guest/README.md` -- test ctest `fake_guest` et documentation.

**Acceptance Criteria:**
- Given `tools/fake-guest`, when on lance `go vet ./... && go test ./...`, then tout passe.
- Given le build Linux, when `ctest` tourne, then `client_tests`, `protocol_vectors`, `wine_scenarios`, `wine_net` et `fake_guest` passent.
- Given `go build`, when on lance `fake-guest --help`, then l'usage liste toutes les options et les codes de sortie.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind | Le cache de `go test` ne suit pas `ikcp.c`, inclus hors du paquet : ctest peut passer `(cached)` sur l'ancien KCP | medium | Vérifié par le relecteur sur un module témoin → patch (`-count=1`, `CGO_ENABLED=1`, `CC`) |
| 2 | blind | cgo non vérifié par CMake : test enregistré même sans compilateur C | low | Même correctif que #1 (environnement du test) → patch |
| 3 | verification-gap | Câblage options → `sessionConfig` et délais par défaut du §8 jamais testés | medium | `TestCLI` s'arrête à la validation ; les tests de session remplacent tous les délais → patch (`parseConfig` + test) |
| 4 | edge-case, blind | `Welcome{conv=0}` accepté alors que le §8 impose un `conv` non nul | low | L'outil doit justement détecter un hôte fautif (story 1.4) ; correction directe → patch |
| 5 | edge-case, blind | `linger()` passe le canal 0 à KCP sans le filtre `conv`/adresse de `handle` | low | Deux chemins incohérents ; correction directe → patch |
| 6 | edge-case, blind | `TestSecondGuestRejected` : `t.Fatal`/`t.Logf` hors de la goroutine du test | low | Panique possible du binaire de test ; correction directe → patch |
| 7 | verification-gap, blind | `TestDrop20` ne prouve pas la retransmission KCP et ignore l'arrivée du `SessionEnd` | medium | Le seuil (`>= 10` sur ~15) tient sans retransmission → patch |
| 8 | blind, verification-gap | Règle §8 « Hello ignoré après un segment KCP » jamais exercée | medium | Le Hello en plus part avant tout segment → patch (Hello envoyé après le premier heartbeat) |
| 9 | verification-gap, edge-case, blind | Bornes hautes de durée serrées dans des tests parallèles : instables sous charge | low | Correction directe (élargir) → patch |
| 10 | blind | `.gitignore` : binaire rangé près de l'entrée obsolète `/server/bin/` | low | Correction directe → patch |
| 11 | verification-gap | Filtres adresse et `conv` de l'invité jamais exercés | medium | Ne compte que contre la DLL → defer (story 1.4) |
| 12 | blind | §8 muet sur : `Welcome` ou `Reject` reçu en session, durée du maintien après `SessionEnd`, Hello invalide du pair, `conv` neuf après un timeout | medium | À trancher avec l'hôte réel → defer (story 1.4) |
| 13 | edge-case | `--host 0.0.0.0` ou `255.255.255.255` accepté | low | Erreur d'usage sans effet grave (sortie 3) → rejeté |
| 14 | edge-case | `readLoop` tourne sans pause sur une erreur persistante | low | Improbable (socket local) → rejeté |
| 15 | edge-case | Hôte simulé : enregistre des Welcome non envoyés en mode silencieux ; Hello accepté pendant la fermeture | low | Code de test, sans effet sur les assertions actuelles → rejeté |
| 16 | edge-case | `TestUnreachable` : port libéré puis réattribué à un test parallèle | low | Improbable → rejeté |
| 17 | edge-case | `EncodeReliable` Go sans borne de 1200 octets, contrairement au C++ | low | Sans effet en v1 (2 octets au plus) → rejeté |
| 18 | blind | `-w` masque aussi les avertissements du code cgo du projet | low | Code d'enveloppe minimal → rejeté |
| 19 | blind | `steamFingerprint` défini deux fois | low | Copie de `main` vérifiée contre `hello.bin` → rejeté |
| 20 | blind | `TestCLI` : branches de validation manquantes (`--drop -1`, `--port 0`, version de 256 octets) | low | En partie couvert par #3 → rejeté |
| 21 | blind | `--quit` est sans effet (c'est le défaut) et `--kill` ne s'applique pas à Ctrl-C | low | Choix d'ergonomie documenté dans le README → rejeté |
| 22 | blind | `Recv`, `Update`, `Flush`, `WaitSnd`, `Check` non testés après `Release` | low | Garde commune à toutes les fonctions → rejeté |

## Design Notes

L'hôte simulé et le faux invité partagent le codec Go. Ce qui prouve qu'ils suivent bien le C++, ce sont les vecteurs, pas leur dialogue. Pour la matrice, les délais (500 ms, 10 s, 5 s, 1 Hz) sont injectables dans une struct de config de la session. Les valeurs par défaut de la CLI restent celles du §8, et les tests passent des valeurs plus courtes pour rester sous quelques secondes.

## Verification

**Commands:**
- `cd tools/fake-guest && go vet ./... && go test ./...` -- expected: tout passe
- `cmake -S client -B client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: 5 tests sur 5
- `protocol/vectors/regen.sh --check` -- expected: vecteurs inchangés
