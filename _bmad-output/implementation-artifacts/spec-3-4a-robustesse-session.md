---
title: 'Story 3.4a — Robustesse de session constatée au premier test à deux machines'
type: 'bugfix'
created: '2026-10-23'
status: 'done'
route: 'auto'
review_loop_iteration: 0
baseline_revision: 'f21a4079c420b357003a3ee157e376d73831fc88'
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-4-animations-pnj.md'
warnings: ['multiple-goals', 'oversized']
deferred:
  - summary: >-
      Messages fiables du pair accumulés pendant une pause longue (jusqu'à 60 s) et rejoués d'un coup à la reprise.
    evidence: |-
      maybe-false : file d'événements (4096) puis fenêtre KCP ; à observer en jeu (bilan après une cinématique longue).
    location: >-
      client/src/net/transport.cpp drain_reliable
    severity: medium (unverified)
  - summary: >-
      Rechargement qui redonne au héros la même adresse sans frame « monde non chargé » : fin de monde non vue (core et sync/npc).
    evidence: |-
      maybe-false, critère préexistant (core/session.cpp:242) ; docs/re/fin-de-monde.md : adresse changée à chaque rechargement observé.
    location: >-
      client/src/sync/session.cpp frame
    severity: medium (unverified)
  - summary: >-
      Oubli des PNJ à la fin de monde dans la DLL (release_all avant validate) sans test automatique.
    evidence: |-
      sync/ n'est pas compilé dans client_tests et le banc Wine n'appelle pas OnProcess ; couvert par les étapes manuelles 1 et 5.
    location: >-
      client/src/sync/session.cpp frame
    severity: medium
  - summary: >-
      PS des répliques détruites encore au registre pendant un chargement (frame non appelée).
    evidence: |-
      Une entité du nouveau monde à la même adresse verrait PlayMotion ou OnProcess bloqués jusqu'à la première frame ; il faudrait un hook de fin de monde.
    location: >-
      client/src/sync/anim_filter.cpp
    severity: medium (unverified)
---

<intent-contract>

## Intent

**Problem:** Au premier test à deux machines (2026-10-23), quatre défauts de robustesse sont apparus :
1. l'invité est passé par une cinématique scriptée (capture par l'Inquisition) ; son `eCApplication::OnProcess` n'a plus été appelé pendant plus de 5 s. L'hôte a conclu « timeout (5 s sans nouvelles du pair) » à 00:32:54, alors que le jeu de l'invité n'avait pas planté (report de `deferred-work.md`, specs 1.4 et 1.5) ;
2. l'invité ignore les entrées de snapshot d'un PNJ dont le `Spawn` (canal fiable) n'est pas encore arrivé (« entités ignorées 12/18/32 », « entités inconnues ») : le PNJ ne bouge qu'une seconde plus tard ;
3. à la fin de session, l'hôte journalise « PNJ … : plus animé » deux fois par PNJ ;
4. les répliques gardent des pointeurs bruts vers des entités du jeu ; un changement de monde ne doit jamais laisser une réplique utilisée (ni revérifiée par `GetEntity` pendant la destruction du monde) après que le moteur l'a détruite.

**Approach:** Le thread réseau envoie lui-même un `Heartbeat` de secours quand la frame ne vient plus lire le transport, et le fait au plus 60 s. L'invité garde 500 ms la dernière entrée de snapshot d'un PNJ pas encore annoncé et l'applique à l'arrivée du `Spawn`. Le doublon de journal est supprimé. Toutes les répliques et tous les PNJ capturés sont oubliés dès que la frame voit le monde changer, avant tout appel au jeu.

## Boundaries & Constraints

**Always:**
- AD-2 : aucun appel au jeu hors du thread principal. Le thread réseau ne fait que du réseau, sur des octets fournis par la frame.
- Le `Heartbeat` de secours est fourni par la session (commande `Start`) : octets du message, intervalle de 1000 ms, silence de la frame de 1500 ms avant le premier envoi, pause de 60 s au plus. Au-delà de 60 s, le thread réseau se tait : le pair conclut au timeout 5 s plus tard. Un jeu figé pour de bon finit donc toujours par être détecté.
- « Frame silencieuse » = aucun appel à `Transport::poll` depuis 1500 ms. Les deux pilotes (DLL et `session_host`) appellent `poll` à chaque frame.
- Le `Heartbeat` de secours ne part que si le KCP est ouvert, y compris pendant le maintien de 1 s après `SessionEnd`, où il est sans effet.
- La détection de timeout reste dans `core/session` et ne change pas : 5 s sans message, compté par la frame à la reprise. Les messages en file sont traités avant les délais : c'est déjà l'ordre de `Session::tick`.
- La fin de monde reste détectée par la frame à la reprise (`SessionEnd{2}`), comme aujourd'hui.
- À la reprise de la frame, le pilote journalise une ligne si des `Heartbeat` de secours sont partis, et une autre si la borne de 60 s a été atteinte.
- PNJ en avance (invité) :
  - une entrée de snapshot d'un NetId ≥ 1000 non annoncé est gardée, la plus récente (plus grand `tick`) par NetId, 64 NetId au plus, pendant 500 ms (horloge de la frame à la réception) ;
  - à l'arrivée du `Spawn` de ce NetId, elle est poussée dans l'interpolation et dans les PV de la nouvelle réplique, puis comptée « reçus avant leur Spawn » ;
  - une entrée qui expire sans `Spawn` compte une fois dans « entités inconnues » et dans « entités ignorées » ;
  - tout est vidé à l'entrée en session et à `Ended`.
- Le nouveau compteur s'ajoute **en fin** de la ligne du bilan `[sync] snapshots (5 s)`. L'ordre et le format des compteurs existants ne changent pas.
- Une seule ligne « plus animé » par PNJ et par sortie du groupe : celle de `core/session`. Celle de `sync/npc::uncapture` change de texte (« retiré du registre des captures »).
- `sync/npc` : quand la frame voit le monde changer (non chargé, ou héros d'une autre adresse que la frame précédente), `npc::release_all` s'exécute **avant** `npc::validate` et avant la session. C'est un oubli sans appel au jeu, journalisé avec la cause « fin de monde ». Les `ReleaseNpc` et `UncaptureNpc` émis ensuite par `end()` sont alors sans effet et sans log.
- `PROTOCOL.md` §8 : ajouter la règle du `Heartbeat` pendant une pause du jeu. Pas de changement de format : le protocole reste en **v10**, et les vecteurs ne changent pas.

**Never:**
- Pas de timeout allongé, pas de changement de `kPeerTimeoutMs` ni de `kHeartbeatIntervalMs`.
- Pas d'appel au jeu, pas de log et pas d'exception depuis `net/`.
- Pas de nouveau message ni de nouveau hook. Pas de reconnexion automatique.
- Le comportement de l'avatar ne change pas : son retrait à la fin de monde est déjà protégé par `same_world`.
- Pas de mort de réplique (3.4b).

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Frame en pause, KCP ouvert | aucun `poll` pendant 7 s | `Heartbeat` reçus par le pair à partir d'environ 1,5 s, puis toutes les ~1 s ; compteur `keepalives_sent` > 0 | N/A |
| Frame vivante | `poll` à chaque boucle | aucun `Heartbeat` de secours | N/A |
| Pause au-delà de la borne | borne réglée bas dans le test | plus d'envoi après la borne ; compteur `keepalive_capped` = 1 | N/A |
| Secours non configuré ou KCP fermé | `Options.keepalive` vide, ou pas d'`OpenReliable` | rien n'est envoyé | N/A |
| Banc : hôte en pause 7 s | `session_host --pause-at 1000 --pause-ms 7000`, faux invité `--strict-timeout` | pas de timeout des deux côtés ; l'hôte journalise la pause | N/A |
| Snapshot avant `Spawn` | entrée NetId 1000 puis `Spawn{1000}` 100 ms plus tard | la réplique est placée à la pose du snapshot (`MoveNpc`) dès le tick suivant ; compteur « reçus avant leur Spawn » = 1 ; « entités inconnues » = 0 | N/A |
| Plusieurs snapshots avant `Spawn` | ticks 10 puis 12 puis 11 | seul le tick 12 est appliqué | N/A |
| `Spawn` trop tard | `Spawn` 600 ms après l'entrée | entrée expirée : « entités inconnues » = 1, réplique à la pose du `Spawn` | N/A |
| Trop de NetId en avance | 65ᵉ NetId inconnu | ignoré et compté inconnu tout de suite | N/A |
| Fin de session hôte | 3 PNJ animés, `Ended` | exactement un `UncaptureNpc` et une ligne « plus animé » par PNJ | N/A |

</intent-contract>

## Code Map

- `client/src/net/transport.{h,cpp}` : `Options` (h:79-83) reçoit `Bytes keepalive` (vide = désactivé), `keepalive_interval_ms`, `keepalive_after_ms` et `keepalive_max_pause_ms`. `Stats` (h:85-109) et la macro `MP_NET_COUNTERS` (cpp:38-44) reçoivent `keepalives_sent` et `keepalive_capped`. `poll()` (cpp:154) note l'instant (`now_ms()`, atomique). `loop_once()` (cpp:177-201) envoie après `execute` par `ikcp_send` si : KCP ouvert, secours configuré, silence ≥ `after`, et prochain envoi dû. Au-delà de `max_pause`, plus d'envoi : `keepalive_capped` est compté une fois par pause. Une frame qui revient remet l'état à neuf. `start()` initialise l'instant du dernier `poll` à 0.
- `client/src/core/session.{h,cpp}` : `Start` (commande, cherchez `struct Start`) porte `Bytes keepalive` plus les trois durées. Elle est remplie avec `Heartbeat{}` encodé, 1000, `kKeepaliveAfterMs = 1500` et `kKeepaliveMaxPauseMs = 60000` (nouvelles constantes à côté de `kPeerTimeoutMs`, h:90-94). Pour les PNJ en avance : `on_snapshot` (cpp:696-717) et `on_npc_entity` (cpp:901-917) gardent l'entrée ; `on_npc_spawn` (cpp:871-886) l'applique ; l'expiration se fait dans `place_npcs` (cpp:919) ou au début du traitement des snapshots ; on vide dans `release_npcs` (cpp:938-953) et à l'entrée en session (cpp:660-675). Les compteurs sont dans `NpcCounters` (h) ; le bilan est dans `snapshot_stats` (cpp:1411-1445). `set_animated_npcs` (cpp:844-868) journalise déjà « plus animé » : on la garde.
- `client/src/sync/session.cpp` : `execute` passe les options de `Start` à `g_transport->start(port, options, error)` (:141-151). Dans `frame()` (:361-387) : après la boucle `poll`, ligne de log si `keepalives_sent` ou `keepalive_capped` ont augmenté. Détection du changement de monde : un `g_last_world_player` mémorisé ; si `!world.loaded()` ou si le héros change, appeler `npc::release_all("fin de monde")` avant `npc::validate`.
- `client/src/sync/npc.{h,cpp}` : `release_all` (cpp:405-428) prend une cause pour son log (« hors session » ou « fin de monde ») ; `uncapture` (cpp:325-332) change de texte de log.
- `client/tests/session_host_main.cpp` : même passage d'options à `transport.start` (:264). Il reçoit `--pause-at MS --pause-ms MS` : une seule pause, `pause-at` ms après l'entrée en `InSession`, sans `poll` ni tick (`sleep`). Il porte aussi la même ligne de log à la reprise que la DLL.
- `client/tests/session_fake_guest.sh` : nouveau scénario « pause de l'hôte ». `start_host` (:35) accepte des arguments en plus.
- `tools/fake-guest/` : `--strict-timeout` (`main.go:78-95`, `Config`) ; `lastPeer` (`session.go:531`, :651) n'est alors mis à jour que par les messages (contrôle, fiables rendus par KCP, snapshots), pas par chaque datagramme. Le mode par défaut ne change pas. Ajouter un test Go du drapeau.
- `client/tests/test_net.cpp` : tests du secours (vrai socket local, comme les tests existants). `client/tests/test_session.cpp` : `Start` porte un Heartbeat décodable ; tests des PNJ en avance et du décapturage unique.
- `protocol/PROTOCOL.md` §8 « Les deux pairs » (:800-807) : règle de la pause. `client/README.md` : procédure de test en jeu. `_bmad-output/implementation-artifacts/deferred-work.md` : marquer faites les entrées « OnProcess s'arrête plus de 5 s » (:49) et « marge faible » (:70).

## Tasks & Acceptance

**Execution:**
- [x] `client/src/net/transport.{h,cpp}`, `client/tests/test_net.cpp` -- Heartbeat de secours depuis le thread réseau, compteurs, tests des 4 premières lignes de la matrice -- défaut 1 sans appel au jeu (AD-2).
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` -- `Start` avec secours ; PNJ en avance ; compteur en fin de bilan ; test du décapturage unique à `Ended` -- défauts 1, 2, 3 (logique testée sous Linux).
- [x] `client/src/sync/session.cpp`, `client/src/sync/npc.{h,cpp}` -- options de démarrage, log de reprise, oubli des PNJ à la fin de monde avant `validate`, texte de `uncapture` -- défauts 1, 3, 4.
- [x] `tools/fake-guest/` -- `--strict-timeout` et son test -- banc discriminant.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- `--pause-at`/`--pause-ms`, scénario « pause de l'hôte 7 s » -- défaut 1 de bout en bout.
- [x] `protocol/PROTOCOL.md`, `client/README.md`, `_bmad-output/implementation-artifacts/deferred-work.md` -- règle de la pause, procédure en jeu, reports soldés -- AD-10.

**Acceptance Criteria:**
- Given les tests Linux, Go et le banc, when on construit, then tout passe et les vecteurs v10 sont inchangés.
- Given le banc, hôte en pause 7 s et faux invité `--strict-timeout`, when la session dure 10 s, then aucun « timeout » n'apparaît dans les deux logs, et le log de l'hôte contient la ligne de reprise avec au moins 4 `Heartbeat` de secours.
- Given deux machines, when l'invité traverse une cinématique ou un chargement de plus de 5 s sans changer de monde, then la session continue et l'hôte ne journalise pas de timeout. *(Manuel.)*
- Given deux machines, when l'hôte recharge une partie, then l'invité reçoit `SessionEnd{2}`, et l'hôte journalise l'oubli des répliques et des PNJ capturés « fin de monde » sans aucun « réplique perdue » ni « PNJ animé perdu ». *(Manuel.)*

## Spec Change Log

## Review Triage Log

### 2026-10-23 — Review pass
- verdicts: 29 findings — high 0, medium 4, low 15, false 4, maybe-false 6
- findings:
  - `[medium]` `[patch]` (verif-gap) Conversion Start → net::Options recopiée dans la DLL et le banc : le banc ne teste pas le chemin de la DLL — `Start` porte désormais un `net::Options` rempli par `make_start`, les deux pilotes l'utilisent tel quel, le test unitaire vérifie `Start::options`.
  - `[medium]` `[defer]` (verif-gap) Oubli « fin de monde » de la DLL sans test automatique — sync/ non compilé sous Linux ; reporté (frontmatter `deferred`, `deferred-work.md`), étapes manuelles 1 et 5.
  - `[low]` `[reject]` (verif-gap) Entrée gardée d'un NetId retiré puis réannoncé dans les 500 ms appliquée à la nouvelle réplique — rare (réordonnancement + réannonce en 500 ms), effet : une pose de moins de 500 ms aussitôt remplacée par les snapshots suivants ; correctif = garde supplémentaire.
  - `[low]` `[reject]` (intent) Borne de 60 s : un jeu figé est détecté en ~65 s au lieu de 5 s — choix consigné (D2), lecture « détection réelle conservée ».
  - `[low]` `[reject]` (intent) Défaut 3 traité comme un doublon de journal, le test core passait déjà avant — diagnostic consigné (D6) : `UncaptureNpc` n'est exécuté qu'une fois, `uncapture` déjà idempotent.
  - `[medium]` `[defer]` (intent) Avatar inchangé et fenêtre du registre pendant le chargement non fermée — avatar protégé par `same_world` (D7) ; fenêtre du registre reportée (hook interdit ici).
  - `[low]` `[reject]` (intent) Sens « invité en pause » couvert seulement par le test unitaire de `Start` côté invité — le transport est indépendant du rôle ; `Start` de l'invité testé ; étape manuelle 2.
  - `[false]` `[reject]` (edge) Spawn refusé (pose non finie / NetId déjà annoncé) recyclé dans le tampon — l'hôte filtre les poses non finies (`select_npcs`), et un NetId déjà annoncé a sa réplique : `on_npc_entity` ne le garde pas.
  - `[low]` `[reject]` (edge) Entrée antérieure à un Despawn appliquée à une réannonce — même constat que la ligne verif-gap ci-dessus.
  - `[maybe-false]` `[defer]` (edge) Héros réalloué à la même adresse : `release_all` sauté — critère préexistant partagé avec core ; reporté.
  - `[false]` `[reject]` (edge) `keepalive_interval_ms == 0` ou borne < seuil : inondation — valeurs fixées par les constantes de `make_start`, jamais nulles.
  - `[low]` `[patch]` (edge) Compteurs de reprise sous-comptés après un redémarrage du transport — remis à 0 au démarrage réussi du transport (DLL et banc), détection par baisse retirée.
  - `[low]` `[reject]` (edge) `clock_origin_` lu par `poll` sur le thread de la frame alors que commenté « thread réseau » — écrit par `start()` sur le même thread avant le lancement du thread réseau, lecture seule ensuite : pas de course ; commentaire seulement.
  - `[false]` `[reject]` (edge, claim) `read_world` appelle le jeu avant `release_all` — appels au héros local, pas aux répliques ; l'affirmation porte sur les répliques.
  - `[maybe-false]` `[defer]` (edge, claim) Réplique revalidée contre une entité libérée après rechargement à même adresse — même entrée reportée que ci-dessus.
  - `[low]` `[patch]` (blind) Heartbeats de secours après `SessionEnd` si la frame se fige pendant le maintien — sans effet chez le pair (maintien ou KCP fermé) ; PROTOCOL.md §8 le dit désormais.
  - `[maybe-false]` `[defer]` (blind) Messages fiables accumulés pendant une pause longue puis rejoués d'un coup — reporté, à mesurer en jeu.
  - `[false]` `[reject]` (blind) Spawn refusé pour GUID introuvable occupe le tampon — le GUID introuvable est traité dans sync/npc ; core a créé la réplique, l'entrée n'est donc plus « en avance ».
  - `[low]` `[reject]` (blind) `on_npc_despawn` ne retire pas l'entrée gardée — l'entrée arrive après le Despawn, l'effacer au Despawn ne change rien ; voir la ligne verif-gap.
  - `[low]` `[patch]` (blind) Limites connues sans entrée dans `deferred-work.md` — 5 entrées 3.4a ajoutées.
  - `[maybe-false]` `[defer]` (blind) Héros à la même adresse : fin de monde non vue — même entrée reportée.
  - `[low]` `[patch]` (blind) `epic-3-context.md` omet la 3.4a — ligne ajoutée.
  - `[medium]` `[patch]` (blind) Logique dupliquée DLL/banc — même correctif que la première ligne (groupe).
  - `[low]` `[reject]` (blind) Secours côté invité sans test de bout en bout, borne de 60 s sans test de bout en bout — mécanisme indépendant du rôle, borne testée au niveau du transport.
  - `[maybe-false]` `[reject]` (blind) Tests du transport sensibles au minutage — deux passages complets de ctest réussis ; au pire low (test intermittent), correctif = élargir des marges.
  - `[low]` `[reject]` (blind) `session_host` accepte des options de pause incohérentes — outil de banc, usage interne au script.
  - `[maybe-false]` `[reject]` (blind) `make_start` vide le secours sans log si l'encodage échoue — l'encodage d'un Heartbeat (1 octet) ne peut pas échouer ; au pire low.
  - `[low]` `[patch]` (blind) Aide du faux invité « protocole v8 » — corrigée en v10.
  - `[low]` `[patch]` (blind) Étape « menu Échap » du README sans résultat attendu — précisé (pas de ligne si la pause dure moins de 1,5 s).

## Design Notes

**Décisions prises sans l'utilisateur (option la plus prudente, à relire) :**
- D1 — Le secours part du thread réseau, avec les octets fournis par la session. On ne compte pas les datagrammes (ACK KCP) pour le timeout : ce choix ne dépend pas du trafic de l'autre pair. Le thread réseau reste un simple tuyau d'octets.
- D2 — Une pause de plus de 60 s fait taire le secours. Un jeu bloqué pour de bon est ainsi détecté en 65 s environ. Un chargement normal dure moins de 10 s (`docs/re/fin-de-monde.md` : 3,5 s mesurées).
- D3 — Le secours démarre après 1500 ms sans `poll`. Une frame vivante, même à 1 image/s, envoie ses propres heartbeats : il n'y a donc jamais de double envoi régulier, et un doublon éventuel est inoffensif.
- D4 — Pas de nouvelle version du protocole : le format ne change pas, et la règle ajoutée ne concerne que l'émetteur.
- D5 — PNJ en avance : on garde 500 ms (10 snapshots) et 64 NetId au plus (54 entités par snapshot), sans nouveau message.
- D6 — Défaut 3 : l'analyse montre qu'`UncaptureNpc` n'est exécuté qu'une fois. `core/session` et `sync/npc::uncapture` écrivent le même texte, « [sync] PNJ N : plus animé ». Le correctif ne touche donc que le texte. `uncapture` est déjà idempotent (retour anticipé si le NetId est absent).
- D7 — Défaut 4 : `npc::validate` (GetEntity par GUID à chaque frame) protège déjà des téléportations et des destructions en cours de monde. On ajoute seulement l'oubli global au changement de monde, avant tout appel au jeu. Le registre garde des adresses mortes pendant le chargement (la frame n'est pas appelée) : le reste du risque est reporté (voir ci-dessous).
- D8 — Le fichier `epic-3-context.md`, recompilé par ce workflow, est commité avec la story.

**Reporté :** pendant un chargement, la frame n'est pas appelée, donc le registre (`AnimFilter`) peut encore contenir les PS des répliques détruites. Une entité du nouveau monde placée à la même adresse pourrait voir ses `PlayMotion` ou son `OnProcess` de routine bloqués jusqu'à la première frame. Il faudrait un signal de fin de monde hors frame (un hook), ce qui est interdit ici.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: tout passe, banc `session_fake_guest` compris.
- `cd tools/fake-guest && go test ./... && go build` -- expected: tout passe, vecteurs v10 concordants.

## Test en jeu à faire

Préparation : recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/`, `log_level = debug`, overlay Steam désactivé. Log : `bin/scripts/Script_Multiplayer.log`.
1. **Une machine, hôte + faux invité `--strict-timeout --duration 120s --echo`** :
   - ouvrir le menu Échap, puis faire un chargement rapide de moins de 60 s ;
   - attendu : pas de « timeout » chez le faux invité pendant les pauses du jeu ;
   - au chargement, l'hôte journalise `PNJ : N réplique(s)/PNJ animé(s) … fin de monde`, puis `InSession → Ended : fin de monde`, et aucun « réplique perdue » ni « PNJ animé perdu » ;
   - si la frame a été suspendue plus de 1,5 s : la ligne `[sync] jeu en pause : N heartbeat(s) envoyé(s) par le thread réseau`.
2. **Deux machines, cinématique** : l'invité déclenche une cinématique scriptée ou un long dialogue avec transition (par exemple la capture par l'Inquisition).
   - attendu chez l'invité : `[sync] jeu en pause : N heartbeat(s)…` à la reprise ;
   - attendu chez l'hôte : pas de `timeout (5 s sans nouvelles du pair)`, et la session continue (snapshots reçus dans le bilan suivant).
3. **Deux machines, PNJ à l'arrivée** : s'approcher d'un camp.
   - attendu chez l'invité : dans `[sync] snapshots (5 s)`, « entités inconnues » proche de 0 et le dernier compteur « reçus avant leur Spawn » > 0 ;
   - les PNJ bougent dès leur apparition.
4. **Fin de session hôte** (l'invité quitte) : une seule ligne `PNJ N : plus animé` par PNJ animé, plus `PNJ N : retiré du registre des captures`.
5. **Deux machines, rechargement de l'hôte** : l'invité reçoit `SessionEnd{2}`. L'hôte oublie ses PNJ « fin de monde » et ne plante pas.

## Auto Run Result

Status: done

**Résumé :** heartbeat de secours envoyé par le thread réseau quand la frame ne lit plus le transport (1,5 s de silence, puis toutes les secondes, 60 s au plus), octets et délais fournis par la session via `Start::options` ; invité : entrées de snapshot d'un PNJ pas encore annoncé gardées 500 ms et appliquées à son `Spawn` ; doublon « plus animé » supprimé (même texte écrit par core et sync/npc) ; répliques et PNJ capturés oubliés dès que la frame voit le monde changer, avant `validate`.

**Fichiers :**
- `client/src/net/transport.{h,cpp}` : secours (`Options.keepalive`, compteurs `keepalives_sent`, `keepalive_capped`).
- `client/src/core/session.{h,cpp}` : `Start::options`, PNJ en avance, compteur en fin de bilan.
- `client/src/sync/session.cpp`, `client/src/sync/npc.{h,cpp}` : démarrage avec secours, ligne de reprise, oubli « fin de monde », texte de `uncapture`.
- `client/tests/test_net.cpp`, `client/tests/test_session.cpp` : 11 tests nouveaux.
- `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` : pause simulée, scénario « pause de l'hôte 7 s ».
- `tools/fake-guest/` : `--strict-timeout`, `TestStrictTimeout`, aide v10.
- `protocol/PROTOCOL.md` (§8 « Pause du jeu »), `client/README.md` (procédure 3.4a), `deferred-work.md` (2 reports soldés, 5 ouverts), `epic-3-context.md` (recompilé, ligne 3.4a).

**Revue :** 29 constats ; 8 lignes corrigées (7 correctifs : 1 medium, 6 low), 6 lignes reportées (4 entrées `deferred`), 15 rejetées (motifs dans le journal de triage).

**Revue de suivi recommandée :** non (un seul correctif medium, aucun high).

**Vérification :** `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : 6/6 (client_tests, protocol_vectors, fake_guest, session_fake_guest, wine_scenarios, wine_net), avant et après les correctifs ; `go test ./... && go build` dans `tools/fake-guest` : OK ; `protocol/vectors/` inchangé (v10).

**Risques résiduels :** aucun test en jeu (jeu non lançable) ; code `sync/` de la DLL couvert par la compilation seulement ; voir `deferred`.
