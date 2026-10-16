---
title: 'Story 1.4 — Héberger et rejoindre une session'
type: 'feature'
created: '2026-10-17'
status: 'in-review'
baseline_commit: '3f2809a9eaa132d8207fafaa94a7de06fa2c245d'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Le contrat (1.1), le transport (1.2) et le faux invité (1.3) existent, mais le mod ne fait encore rien en réseau. Charger une partie ne démarre ni l'hôte ni la connexion.

**Approach:**
- **Logique de session pure** (`core/session`), testée sous Linux. Elle reçoit le temps, l'état du monde et les messages décodés, et rend des commandes et des lignes de log.
- **Pilote Windows** (`sync/session`), appelé chaque frame depuis le hook `OnProcess`. Il relie cette logique au transport et au jeu.
- **Vérification de l'hôte** contre le faux invité, sous Linux, sans le jeu. Puis en jeu, à la main.

## Boundaries & Constraints

**Always:**
- **§8 de `PROTOCOL.md` appliqué tel quel** : ordre de vérification du §6, comparaisons exactes, `conv` aléatoire non nul, même `Welcome` pour une retransmission depuis la même adresse, `Reject{4}` pour une autre adresse, heartbeat à 1 Hz, timeout à 5 s, Hello toutes les 500 ms, abandon à 10 s.
  - **Activité du pair :** tout événement reçu de lui (contrôle ou fiable). Les heartbeats à 1 Hz suffisent à la maintenir.
- **États :**
  - `Inactive → Hosting | Connecting → InSession → Ended`, au premier `OnProcess` où `GetPlayer` est non nul.
  - Dans cette story, `Ended` rend le solo et reste final jusqu'au redémarrage du jeu. Le réarmement au monde suivant et la fin de monde viennent en story 1.5.
  - L'hôte en `InSession` qui perd son invité (timeout ou `SessionEnd` reçu) passe aussi en `Ended`.
- **`core/session` est pur :** pas d'horloge système, de socket ni de jeu. L'horloge est injectée en ms. La logique produit des commandes du type `Send{dest, octets}`, `OpenReliable`, `SendReliable`, `CloseReliable` et `Stop`, ainsi que des transitions journalisables.
- **Empreintes :**
  - le `Hello` porte les 4 empreintes, lues dans les en-têtes PE de Game, Engine, Script et SharedBase chargées en mémoire ;
  - l'hôte compare celles de l'invité aux **siennes**, sans liste blanche supplémentaire (J0 a déjà validé Game et Engine des deux côtés) ;
  - `version_mod` = `MP_MOD_VERSION`.
- **`Welcome`** porte la pose du joueur hôte : `m[12..14]` et le lacet `atan2(m[8], m[10])`. L'invité la journalise seulement (le placement vient avec l'Epic 2).
- **Adresse de l'hôte (invité) :** IPv4 littérale seulement. Un nom d'hôte ou une IPv6 donne une erreur claire au log (« IPv4 attendue »), et le mod reste en solo.
- **Exclusivité :** avec `probe = 1`, aucune session. Avec `probe = 0`, seul `OnProcess` est hooké, pas `PlayMotion`.
- **Log :** préfixe `[sync]` pour les transitions, `Hello` reçus et `Reject` envoyés ou reçus, avec leur raison lisible (code brut si inconnu). Rien par frame hors `log_level = debug`.
- **Fin du processus :** le `Transport` est alloué une fois et volontairement jamais détruit. Aucun `join` n'est fait sous le verrou du chargeur (report de la story 1.2). L'arrêt explicite (`Ended`) fait `stop()` depuis le thread principal.
- **Robustesse :** aucune exception ne sort du détour `OnProcess`. Un échec (port occupé, ABI) journalise la cause et laisse le jeu en solo.

**Never:**
- Pas de fin de monde, de réarmement ni de mort de l'hôte (story 1.5). Pas d'avatar, de téléportation ni de snapshot (Epic 2).
- Pas de résolution DNS ni d'IPv6. Pas de nouvelle version du protocole. Pas de changement de `net/` hors correction de bug.
- Ne jamais appeler le jeu hors du thread principal.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu (core/session, tests Linux) |
|---|---|---|
| Hôte armé | rôle hôte, monde chargé | `Hosting`, écoute ; log « en attente d'un invité sur le port N » |
| Hello OK | Hello v1 compatible depuis A | `Welcome{conv≠0, pose}`, `OpenReliable(conv, A)`, `InSession` |
| Retransmission | même Hello depuis A, aucun segment KCP reçu | même `Welcome` (même `conv`), pas de nouvel `OpenReliable` |
| Après KCP | Hello depuis A après un message fiable de A | ignoré |
| Autre adresse | Hello depuis B pendant la session avec A | `Reject{4}` vers B ; session A intacte |
| Version | v2 (peek) / mod différent / empreinte différente / illisible | `Reject{1}` / `{2}` / `{3}` / `{5}`, raison au log |
| Invité armé | rôle invité, IPv4 valide | `Connecting`, Hello à t = 0, 500, 1000… ms |
| Welcome reçu | `conv≠0` | `OpenReliable`, `InSession`, pose journalisée |
| Welcome conv nul | `conv = 0` | ignoré, Hello continue |
| Reject reçu | `Reject{r}` | `Ended`, raison lisible, `Stop` |
| Injoignable | 10 s sans réponse | `Ended` « hôte injoignable », `Stop` |
| Heartbeat | `InSession` | `SendReliable(Heartbeat)` toutes les 1000 ms |
| Timeout | 5 s sans événement du pair | `Ended` « timeout », sans envoi |
| SessionEnd reçu | `SessionEnd{r}` | `Ended` avec la raison |
| Nom d'hôte | `host_ip = mon.serveur` | erreur « IPv4 attendue », reste `Inactive` |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}`, `client/src/net/transport.{h,cpp}`, `net/udp_socket.h` (`parse_ipv4`, `Endpoint`) -- briques à réutiliser telles quelles.
- `client/src/win/script_init.cpp` -- `fingerprint_of()` (`:181`) à réutiliser pour Script.dll et SharedBase.dll. `initialize()` (`:236`) : avec `probe = 0`, appeler l'installation du hook de session au lieu de « sonde désactivée » seul (garder cette ligne, utilisée par `run_scenarios.sh`).
- `client/src/win/hooks.{h,cpp}` -- ajouter `install_session()` (OnProcess seul, détour → `sync::session::on_frame()`), sur le modèle de `install_probe()`.
- `client/src/game/entity.{h,cpp}` -- `resolve_entities`, `get_player`, `get_pose` : seuls appels au jeu nécessaires.
- `client/src/sync/probe.cpp` -- modèle du tick par frame (horloge QPC, détection du joueur).
- `tools/fake-guest/` -- client de test des scénarios hôte (`--dup-hello`, `--bad-version`, `--drop 20`, deux instances).
- `client/tests/wine/run_scenarios.sh` -- le banc n'appelle jamais `OnProcess` : il peut seulement vérifier que le hook de session s'installe.
- `_bmad-output/implementation-artifacts/deferred-work.md` -- reports de 1.2 et 1.3 traités ici : join sous verrou du chargeur ; règles implicites (Welcome ou Reject reçu en session, Hello du pair rejeté) à ajouter au §8.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/core/session.{h,cpp}` + `client/tests/test_session.cpp` -- machine à états hôte et invité, pure, et toute la matrice -- cœur testable.
- [x] `client/tests/session_host_main.cpp` + `client/tests/session_fake_guest.sh` + ctest `session_fake_guest` -- hôte Linux (`core/session` + `net::Transport`, monde simulé toujours chargé), puis scénarios du faux invité : nominal, `--dup-hello`, `--bad-version`, deux invités, `--drop 20`. On vérifie les codes de sortie et le log de l'hôte -- preuve hôte sans le jeu.
- [x] `client/src/sync/session.{h,cpp}` -- pilote Windows : état du monde (`GetPlayer`), pose, empreintes, `Transport`, exécution des commandes, log `[sync]` -- intégration.
- [x] `client/src/win/hooks.*`, `client/src/win/script_init.cpp` -- `install_session()` quand `probe = 0` -- branchement.
- [x] `client/tests/wine/run_scenarios.sh` -- `probe = 0` : « hooks installés (OnProcess) » ; `probe = 1` : hooks de la sonde seuls -- exclusivité.
- [x] `protocol/PROTOCOL.md` §8 -- rendre normatifs : `Welcome` ou `Reject` reçu en session (ignoré), Hello invalide venant du pair accepté (ignoré), `conv` neuf à chaque session -- reports de 1.3.
- [x] `client/CMakeLists.txt`, `client/README.md` -- cibles, tests, section « Tester une session » (Risen hôte + faux invité ; invité injoignable).

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then les 6 tests passent (`client_tests`, `protocol_vectors`, `fake_guest`, `session_fake_guest`, `wine_scenarios`, `wine_net`).
- Given le build Windows, when on construit la DLL, then elle compile sans avertissement et importe KERNEL32, USER32 et WS2_32.
- Given Risen en hôte (`probe = 0`) et `fake-guest --host 127.0.0.1 --duration 10s`, when on charge une partie, then le log du jeu montre `Hosting`, puis `InSession`, puis la fin de session de l'invité. Vérification manuelle par l'utilisateur.
- Given Risen en invité vers une IPv4 où personne n'écoute, when on charge une partie, then le log montre `Connecting`, puis « hôte injoignable » environ 10 s plus tard, et le jeu reste jouable. Vérification manuelle. Le reste du côté invité n'est validé qu'à deux machines, et le log de la story le dit.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind | §8 dit « segment KCP » pour l'arrêt du renvoi du Welcome ; le code s'arrête au premier message fiable | low | Renvoyer le même Welcome est sans effet ; texte corrigé → patch |
| 2 | blind | §8 promet un `conv` « jamais réutilisé », alors qu'il est seulement tiré au hasard | low | Texte corrigé (« tiré à neuf ») → patch |
| 3 | blind | `deferred-work.md` : le point de 1.3 est marqué fait alors qu'une partie part en 1.5 | low | Correction directe → patch |
| 4 | verification-gap, blind | Rattrapage des minuteries après une frame tardive non testé | medium | Supprimer ces lignes ne fait échouer aucun test → patch |
| 5 | blind | Chemins de `core/session` non testés (`fail` en Connecting, message fiable illisible, §6 avant `Reject{4}`, Hello trop court du pair) | medium | Aucun test ne les exerce → patch |
| 6 | verification-gap | Échec de l'installation de la session avec `probe = 0` non testé sous Wine | medium | Seul le scénario sans Script.dll avec `probe = 1` existe → patch |
| 7 | blind | Invité : une réponse venant d'une autre adresse n'est journalisée qu'en debug | low | L'utilisateur ne voit qu'« hôte injoignable » → patch (avertissement unique) |
| 8 | blind | Le log de l'exception ne donne pas l'état de la session | low | Correction directe → patch |
| 9 | blind, edge-case | `session_fake_guest.sh` : diagnostics vides ; échec de `start_host` en cascade ; attente de la mauvaise ligne | low | Script de test, corrections directes → patch |
| 10 | blind | Doc : « Port UDP du serveur » ; port à ouvrir chez l'hôte ; `--duration` non expliqué | low | Correction directe → patch |
| 11 | blind | Activité du pair comptée sur les messages et non sur les datagrammes : faux timeout possible sous fortes pertes (blocage en tête de file KCP) | medium | Choix figé dans l'intent ; risque non mesuré → defer (à observer à deux machines) |
| 12 | edge-case | `OnProcess` muet plus de 5 s (chargement, saccade) : plus de heartbeat, donc timeout chez le pair | medium | Non vérifié en jeu → defer (mesure en jeu, heartbeat depuis le thread réseau à envisager) |
| 13 | verification-gap, blind | L'exécuteur de commandes de la DLL est une copie de celui de `session_host` : il n'est couvert que par le test en jeu | medium | Le test manuel est prévu par la spec → defer (exécuteur partagé) |
| 14 | edge-case | Joueur nul (menu, rechargement) pendant Hosting ou InSession : la session survit ; Welcome avec une pose nulle | low | Fin de monde = story 1.5 (exclue par l'intent) → rejeté ici, traité en 1.5 |
| 15 | blind | Pose lue à chaque frame en Hosting | low | Un appel ABI par frame, sans effet mesurable → rejeté |
| 16 | blind, edge-case | Log et Reject sans limite face à une inondation de Hello | low | Pas d'adversaire visé (NFR-9) → rejeté |
| 17 | edge-case | Datagramme de contrôle illisible venant du pair : il rafraîchit l'activité | low | Usurpation non visée → rejeté |
| 18 | edge-case | `host_ip` 0.0.0.0, diffusion ou multicast accepté | low | Erreur de config, menant à « injoignable » → rejeté |
| 19 | blind | `session_host` : `post()` ignoré, options inconnues tolérées | low | Outil de test → rejeté |

## Design Notes

`core/session` rend des commandes au lieu d'agir : le pilote Windows et l'hôte Linux de test exécutent exactement la même logique. C'est ce qui permet de vérifier le comportement de l'hôte face au faux invité sans le jeu. Le pilote ne fait qu'une chose par frame : lire le monde, vider `poll()`, appeler `session.tick(now, world, events)`, exécuter les commandes et écrire les logs.

## Verification

**Commands:**
- `cmake --build client/build && llvm-objdump -p client/build/Script_Multiplayer.dll | grep 'DLL Name'` -- expected: sans avertissement ; KERNEL32, USER32, WS2_32
- `cmake -S client -B client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: 6 tests sur 6

**Manual checks:**
- En jeu, Risen hôte et faux invité : `Hosting` → `InSession` → fin, dans `Script_Multiplayer.log`.
- En jeu, invité vers une IPv4 sans hôte : « hôte injoignable » après environ 10 s.
