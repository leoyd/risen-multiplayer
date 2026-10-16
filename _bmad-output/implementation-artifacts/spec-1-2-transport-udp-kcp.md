---
title: 'Story 1.2 — Transport UDP + KCP en boucle locale'
type: 'feature'
created: '2026-10-16'
status: 'done'
baseline_commit: '8ab87ded712feccd2bf90ec7887efb568a1b7c76'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Le contrat v1 existe (story 1.1), mais il n'y a pas encore de transport pour le faire circuler. La session (story 1.4) a besoin d'un réseau qui tourne dans son propre thread, sans jamais toucher au jeu.

**Approach:** Une couche `net/` : un socket UDP, le démultiplexage par octet de canal, KCP 2.1.1 vendorisé, et un thread réseau. Ce thread n'échange avec la frame que par deux files sous verrou. Elle est testée en boucle locale sous Linux, puis sous Wine avec le même test compilé pour Windows.

## Boundaries & Constraints

**Always:**
- **KCP 2.1.1** (tag `2.1.1`, commit `fbd716b257628036375b94061a38a188ea4ed0c8` de `github.com/skywind3000/kcp`), copié sans modification dans `client/third_party/kcp/` (`ikcp.c`, `ikcp.h`, `LICENSE`, `VERSION.txt` sur le modèle de MinHook). Il est compilé dans la bibliothèque `mp_net`, avec `/W0` sous clang-cl.
- **Réglages KCP :** `ikcp_nodelay(1, 10, 2, 1)` et `ikcp_setmtu(1199)` ; fenêtres par défaut.
  - Callback de sortie : `[u8 canal=0][segment]`, donc ≤ 1200 octets.
  - Horloge : millisecondes de `steady_clock`.
- **Le thread réseau possède seul le socket et le KCP**, qui n'est pas thread-safe. La frame ne partage avec lui que :
  - la file sortante (commandes) et la file entrante (événements), sous `std::mutex` ;
  - un drapeau d'arrêt et des compteurs atomiques.

  Chaque file a un plafond de 4096 éléments. Au-delà, l'élément est jeté et compté : la frame n'attend jamais.
- **Commandes :**
  - `SendControl{destination, octets}` : datagramme envoyé tel quel ; le codec 1.1 y a déjà mis l'octet de canal 2.
  - `OpenReliable{conv, pair}` : crée le KCP. Il n'existe qu'après le Welcome (AD-5).
  - `SendReliable{octets}` : un message `[MsgType][charge]` confié à `ikcp_send`.
  - `CloseReliable{}`.
- **Événements :**
  - `ControlReceived{source, octets}` : datagrammes de canal 2, octets complets.
  - `ReliableReceived{octets}` : messages rendus par `ikcp_recv`.
- **Datagrammes ignorés et comptés :** canal 1 (snapshots, Epic 2) ; canal inconnu ; canal 0 sans KCP ouvert, venant d'une autre adresse que le pair ou portant un autre `conv` ; tout envoi de plus de 1200 octets (refusé).
- **Adresses :** IPv4 seulement en 1.2 (`Endpoint{u32 ipv4, u16 port}`, fonction `parse_ipv4`). La résolution de nom et l'IPv6 sont pour la story 1.4.
- **Windows :** `WSAStartup`/`WSACleanup` comptés, et `SIO_UDP_CONNRESET` désactivé. Sinon, un ICMP « port injoignable » fait échouer `recvfrom` (`WSAECONNRESET`).
- **Arrêt :** drapeau, `join`, puis socket fermé. Le destructeur appelle `stop()`. Après `stop()`, le même port peut être relié aussitôt.
- **Erreurs :** `start()` retourne `false` avec un message (port occupé, échec de socket). Aucune exception ne sort de `net/`, création du thread comprise.
- **Simulation de pertes :** option de test `drop_rate` (graine fixe), appliquée à l'envoi de chaque datagramme.

**Never:**
- Pas d'appel au jeu ni au log depuis `net/`. Le log est réservé à Windows et au thread principal ; `sync/` (story 1.4) journalisera à partir des compteurs et des erreurs.
- Pas de machine à états de session, de Hello renvoyé, de heartbeat ni de timeout (story 1.4).
- Pas de snapshot (Epic 2), de chiffrement, de FEC ni d'IPv6.
- Pas de modification des sources KCP.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu | Erreur |
|---|---|---|---|
| Contrôle | A envoie `hello.bin` à B en 127.0.0.1 | B reçoit `ControlReceived` avec la source de A et les octets identiques | — |
| Fiable sans perte | KCP ouvert des deux côtés (même `conv`), 200 messages de 1 à 3000 octets dans chaque sens | tous reçus, dans l'ordre, octets identiques | — |
| Fiable, 20 % de pertes | idem, `drop_rate=0.2` des deux côtés | tous reçus dans l'ordre en moins de 15 s | — |
| Taille | tout le trafic ci-dessus | plus grand datagramme envoyé ≤ 1200 | — |
| Trop gros | `SendControl` de 1201 octets | rien n'est envoyé | compteur « trop gros » +1 |
| Canal ignoré | datagramme canal 1, canal 9, ou canal 0 sans KCP / mauvais `conv` / autre adresse | aucun événement | compteur correspondant +1 |
| Port occupé | `start` sur un port déjà lié | `false` + message | pas de thread lancé |
| Arrêt | `start` puis `stop`, 20 fois de suite, sur le même port | chaque `stop` revient (join), le port est relié aussitôt | — |
| File pleine | 5000 commandes postées, thread arrêté | 4096 gardées | compteur « file pleine » +904 |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.h` -- `kMaxDatagram` (1200), `Channel` (0 fiable, 1 snapshot, 2 contrôle) : à réutiliser, ne pas redéfinir.
- `protocol/vectors/hello.bin` -- charge réelle pour le test de contrôle (`MP_VECTORS_DIR`).
- `client/tests/test.h`, `client/tests/test_main.cpp` -- mini-framework, compilable aussi sous clang-cl.
- `client/CMakeLists.txt` -- `mp_core` est compilé partout. Il faut :
  - ajouter `mp_net`, avec `udp_socket_posix.cpp` ou `udp_socket_win.cpp` selon la plateforme, `pthread` sous Linux et `ws2_32` sous Windows ;
  - le lier à `Script_Multiplayer`, même s'il n'est pas encore appelé, pour prouver qu'il compile dans la DLL ;
  - côté Windows, construire `net_tests.exe` (`test_main` + `test_net`) ;
  - côté Linux, ajouter `test_net.cpp` à `client_tests` et enregistrer le test `wine_net` (`wine <MP_WIN_BUILD_DIR>/net_tests.exe`) si `wine` et l'exécutable existent, sur le modèle de `wine_scenarios`.
- `client/third_party/minhook/VERSION.txt` -- modèle de `VERSION.txt` pour KCP.
- `client/README.md` -- section Organisation : ajouter `src/net/` et `third_party/kcp/`.

## Tasks & Acceptance

**Execution:**
- [x] `client/third_party/kcp/` -- vendoriser KCP 2.1.1 (commit ci-dessus) avec `LICENSE` et `VERSION.txt` -- dépendance figée.
- [x] `client/src/net/udp_socket.h`, `udp_socket_posix.cpp`, `udp_socket_win.cpp` -- socket UDP non bloquant minimal : bind, sendto, recvfrom, attente bornée de lisibilité (≤ 5 ms), fermeture, `parse_ipv4` -- seul code dépendant de la plateforme.
- [x] `client/src/net/transport.{h,cpp}` -- `mp::net::Transport` : `start(port, options)` (0 = port éphémère), `stop()`, `local_port()`, `post(commande)`, `poll(événement)`, `stats()`. Boucle : réception, commandes, `ikcp_update`/`ikcp_check`, `ikcp_recv` vers la file entrante -- cœur de la story.
- [x] `client/tests/test_net.cpp` -- toute la matrice I/O en boucle locale, avec des délais par horloge et des attentes actives bornées (pas de sleep fixe pour conclure) -- preuve Linux et Wine.
- [x] `client/CMakeLists.txt`, `client/README.md` -- intégration, cibles et tests ctest, documentation.

**Acceptance Criteria:**
- Given le build Windows, when on construit la DLL et `net_tests.exe`, then ils compilent sans nouvel avertissement, et la DLL n'importe que KERNEL32, USER32 et éventuellement WS2_32.
- Given le build Linux, when `ctest` tourne, then `client_tests`, `protocol_vectors`, `wine_scenarios` et `wine_net` passent.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind, edge-case, verification-gap | File d'événements pleine : un message déjà sorti de KCP par `ikcp_recv` est jeté, ce qui fait perdre un message au canal fiable | medium | `drain_reliable` → `push_event` jette au-delà de 4096 → patch : ne plus vider KCP tant que la file est pleine, et tester ce cas |
| 2 | blind, edge-case | `post()` est dit « accepté transport arrêté », mais `start()` vide la file sans le compter | low | Commentaire faux, correction directe → patch (documentation seule) |
| 3 | verification-gap, blind | La limite de 1200 octets à la réception (`rx_too_big`, `MSG_TRUNC` / `WSAEMSGSIZE`) et le datagramme vide ne sont pas testés | medium | Seul l'envoi est testé → patch (test avec un socket brut) |
| 4 | verification-gap, blind | Le plafond de la file d'événements n'est pas testé | medium | `event_queue_full` absent des tests → patch |
| 5 | verification-gap, blind | `CloseReliable`, la réouverture, `SendReliable` sans KCP ou vide ne sont pas testés | medium | Aucun test ne poste `CloseReliable` → patch |
| 6 | blind, edge-case | Lien KCP mort (`state = -1`) non signalé ; file d'envoi KCP sans limite | medium | Le timeout de 5 s de la story 1.4 met fin à la session ; à exposer quand l'Epic 2 enverra beaucoup → defer |
| 7 | edge-case | Destructeur de `Transport` (join) appelé au déchargement de la DLL sous le verrou du chargeur : risque de blocage | medium | Sans objet tant que la DLL n'instancie pas de transport → defer (story 1.4) |
| 8 | verification-gap, blind | `wine_net` n'est enregistré que si `net_tests.exe` existe à la configuration : ctest peut passer à 3/3 en silence | medium | Même schéma que `wine_scenarios` (antérieur) → defer |
| 9 | blind | `CLAUDE.md` dit « plus de Go », alors que la spine (AD-1 amendé) autorise le faux invité Go | low | Le correctif touche un fichier de contexte agent → defer |
| 10 | blind | Sources KCP vendorisées absentes du diff : la copie sans modification n'est pas vérifiable | false | Comparées à `cmp` avec `raw.githubusercontent.com/skywind3000/kcp/fbd716b…` : identiques (ikcp.c, ikcp.h, LICENSE) |
| 11 | blind | Pas de `ikcp_flush` après `SendReliable` : jusqu'à 10 ms de latence en plus | low | Négligeable pour des messages de session → rejeté |
| 12 | blind, edge-case | Retour de `WSAIoctl(SIO_UDP_CONNRESET)` ignoré | low | Un échec ne produit que des `recv_errors` comptés, et la boucle continue → rejeté |
| 13 | blind | Tampons `SO_RCVBUF` / `SO_SNDBUF` laissés par défaut, saturation confondue avec `send_errors` | low | Trafic de session minime → rejeté |
| 14 | blind | Pas de `SO_EXCLUSIVEADDRUSE` sous Windows | low | Pas d'adversaire local visé (NFR-9) → rejeté |
| 15 | blind | Assertion `max_datagram_sent <= 1200` toujours vraie dans le test « 1201 » ; même graine de pertes des deux côtés | low | Sans effet sur ce que le test prouve (`too_big == 1`) → rejeté |
| 16 | blind | `on_kcp_output` public ; commandes restantes du lot perdues après une exception ; `recv_errors` en boucle sur une erreur persistante | low | Cas improbables, corrections non directes → rejeté |
| 17 | edge-case | Inondation du canal 2 par un tiers : remplit la file d'événements | low | Pas d'adversaire visé (NFR-9) → rejeté |
| 18 | edge-case | `SendControl` n'impose ni charge non vide ni octet de canal 2 | low | La frame passe par le codec 1.1 → rejeté |
| 19 | edge-case | `open_reliable` détruit l'ancien KCP avant de savoir si `ikcp_create` réussit | low | Échec d'allocation seulement → rejeté |
| 20 | edge-case | `drop_rate` non validé (NaN, ≥ 1) | low | Option de test seulement → rejeté |
| 21 | edge-case | `wait_readable` Windows : `tv_usec` ≥ 1 s si `timeout_ms` ≥ 1000 | false | L'unique appelant passe au plus 5 ms (`kMaxWaitMs`) |
| 22 | edge-case | `errno` lu après des allocations dans `errno_text` ; `poll` interrompu par `EINTR` | low | Message ou attente seulement, sans effet fonctionnel → rejeté |
| 23 | edge-case | Exceptions possibles hors de `net/` (`bad_alloc` dans `start`, `system_error` d'un `lock_guard`) | low | Cas d'épuisement mémoire seulement → rejeté |

## Design Notes

Envoyer un message fiable depuis la frame se fait en deux temps : `encode_reliable` (1.1) produit les octets, puis `post(SendReliable{...})`. Le thread réseau seul appelle `ikcp_send`. Le chemin inverse est symétrique : `poll()` rend `ReliableReceived`, et la frame appelle `decode_reliable`. Le transport ne connaît que les octets de canal, jamais les messages : le codec et la session restent hors de `net/`.

## Verification

**Commands:**
- `cmake --build client/build && llvm-objdump -p client/build/Script_Multiplayer.dll | grep 'DLL Name'` -- expected: build sans avertissement ; KERNEL32, USER32, éventuellement WS2_32
- `cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: 4 tests sur 4 (`client_tests`, `protocol_vectors`, `wine_scenarios`, `wine_net`)
