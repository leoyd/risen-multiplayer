---
title: 'Story 3.4 — Répliques des PNJ : animations'
type: 'feature'
created: '2026-10-22'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'be50231d903902c8bd023db23b31d2cbc58babcf'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-3-repliques-pnj.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Chez l’invité, les répliques de PNJ (story 3.3) suivent la position et les PV de l’hôte, mais c’est le moteur de l’invité qui les anime. Elles glissent sans marcher ni attaquer comme chez l’hôte (FR-9).

**Décisions (utilisateur, 2026-10-22) :**
- La story est découpée : la mort des répliques (Q11, option A) part en **story 3.4b** (`deferred-work.md`). Ici, un PNJ mort reçoit toujours `Despawn`.
- **Budget (Q12, option A)** : seules les **8 répliques les plus proches** d’un des deux joueurs sont animées. Les réglages `MotionParam` d’un PNJ sont regroupés, au plus un par frame et par (type, nature). Le budget visé est de **32 Ko/s au plus** pour l’ensemble des PNJ.

**Approach:** L’hôte capture les `AniName`, `Motion` et `MotionParam` de ses 8 PNJ répliqués les plus proches, comme ceux de son héros, mais avec leur `NetId`. L’invité les rejoue sur la réplique et bloque toute animation de son propre moteur sur elle. Le protocole passe en **v10**.

## Boundaries & Constraints

**Always :**
- Hôte :
  - Les PNJ animés sont inscrits au registre (AD-12) avec leur PS d’animation et leur enveloppe, dans un rôle de capture qui porte leur `NetId`.
  - Les détours `PlayMotion` et de locomotion existants les reconnaissent sous le même verrou.
  - L’inscription suit chaque tick de sélection : un PNJ entre au registre quand il passe dans les 8 plus proches. Il en sort quand il quitte ce groupe, au `Despawn` et à `Ended`.
- Invité :
  - Toute réplique est inscrite avec son PS d’animation et son enveloppe.
  - Son moteur ne l’anime plus (`blocks_locked`), sauf sous `ReplayGuard`. Une réplique hors des 8 reste figée dans sa dernière animation.
  - Le rejeu reprend les règles validées sur l’avatar, appliquées **par entité** : 150 ms de retard, un seul `Motion` des types 0 ou 1 (le plus récent) par frame, filtre des réglages identiques, valeurs finales 0 ou 1 toujours envoyées.
  - Une réplique libérée sort du registre, et son moteur l’anime de nouveau.
- Protocole v10 :
  - `Motion` et `MotionParam` sont acceptés pour tout `NetId` de PNJ annoncé par l’hôte et pas encore retiré.
  - L’espace des `ani_id` reste unique par émetteur.
  - Mettre à jour `PROTOCOL.md`, les vecteurs, les codecs C++ et Go, et les tests des deux côtés (AD-10).
- Le bilan debug de 5 s compte les `Motion` et `MotionParam` des PNJ envoyés, rejoués et ignorés, ainsi que les octets envoyés par seconde. Le dépassement du budget est journalisé une fois par bilan.
- Rien de ce qui a été validé pour l’avatar en 2.4 et 2.4b ne change :
  - capture du héros et ordre de l’anneau ;
  - un `AniName` part avant le premier `Motion` qui l’utilise ;
  - ordre arme → armure → gestes ;
  - format des compteurs existants.

**Never :**
- Pas de mort de réplique (3.4b), pas de dégâts (3.5, 3.6). Le comportement de l’avatar ne change pas.
- Aucun nouveau hook. Pas d’`EnableClassProcessing`, pas de `Entity::GetNPCs` en session.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| PNJ proche qui marche ou attaque | `PlayMotion` et réglages chez l’hôte | `AniName`/`Motion`/`MotionParam` avec son NetId, rejoués 150 ms plus tard | N/A |
| Moteur de l’invité | `PlayMotion` natif sur une réplique | bloqué | N/A |
| 9e PNJ ou plus loin | hors des 8 plus proches | rien n’est capturé, la réplique est figée | N/A |
| Entrée dans les 8 | un PNJ s’approche | capture à partir du `PlayMotion` suivant | N/A |
| Rafale de réglages | plusieurs réglages dans une frame | un seul par (type, nature) et par PNJ, le dernier | N/A |
| `Motion` d’un NetId inconnu ou retiré | | ignoré, compté | N/A |
| `Despawn` ou `Ended` | | file d’animations vidée, réplique rendue au moteur | N/A |

</frozen-after-approval>

## Code Map

- `protocol/PROTOCOL.md` : `Motion` (:133-157), `AniName` (:160-167), `MotionParam` (:203-219) ; règle « `Motion` seulement du NetId de l’avatar » (:799) à élargir ; version 10.
- `client/src/core/session.{h,cpp}` (logique pure ; tests `client/tests/test_session.cpp`, animations :1590-1804, locomotion :1839-1959) :
  - Hôte : `on_local_motion` (:974) et `on_local_param` (:1094) posent `own_net_id()`. Ils doivent prendre le NetId de la capture. Le filtre `last_params_[type][kind]` (`session.h:421`) passe par entité. La sélection des PNJ (:700-792) fournit le rang (les 8 plus proches).
  - Invité : la file `pending_` est liée à l’avatar (`session.h:408-422`, `play_due_motions` :1159, `accepts_peer_animation` :1002). Il faut une file par `Replica` (`session.h:463-471`) et les commandes `PlayNpcMotion` et `PlayNpcParam`.
  - Bilan : `snapshot_stats` (:1227-1248), avec de nouveaux compteurs ajoutés en fin de ligne.
- `client/src/sync/anim_filter.{h,cpp}` :
  - `classify` (:103-116) et `blocks_locked` (:77-83) ;
  - `Capture` (`anim_filter.h:38-55`) ne porte pas d’identité : y ajouter le NetId ;
  - l’anneau `kCaptureRing = 256` reste partagé, avec la perte comptée.
- `client/src/sync/npc.{h,cpp}` :
  - `read_host` (:85-143) inscrit ou retire les PS des 8 PNJ animés ;
  - `spawn` (:176-181) ajoute `get_animation_ps` et `get_actor_wrapper`, comme `avatar.cpp:316-334` ;
  - nouveaux `play` et `play_param` ;
  - `release` (:224).
- `client/src/sync/avatar.cpp` : modèle sans modification de comportement. `play` (:407-442), `play_param` (:444-472), le filtre d’enveloppe `g_last_capture` (:76-84, :718-733), qui doit devenir par entité pour les PNJ, et `track_local` (:699-708). Le code de rejeu commun peut être extrait si l’avatar garde exactement le même comportement.
- `client/src/sync/session.cpp` : `local_captures` (:261-283), `motion_name` (:249-257), routage des commandes (:187).
- `client/src/win/hooks.cpp` : `PlayMotion` (:143) et locomotion (:154-176), à ne pas modifier sauf pour suivre la délégation.
- `tools/fake-guest/session.go` : compteurs (:94-98), bilan (:249), écho limité à `HostNetId` (:698-704). Ajouter le comptage des animations par NetId de PNJ et les vecteurs v10.
- `client/tests/session_host_main.cpp`, `session_fake_guest.sh` : les PNJ simulés émettent des animations.

## Tasks & Acceptance

**Execution :**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/`, `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp` : v10, règle d’acceptation élargie, vecteur `motion_npc.bin`. Motif : AD-10.
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` : NetId des captures, filtres et files par entité, 8 plus proches, regroupement par frame, compteurs, cas de la matrice. Motif : logique testée sous Linux.
- [x] `client/src/sync/anim_filter.{h,cpp}`, `client/tests/test_anim_filter.cpp` : NetId dans `Capture`, rôle de capture des PNJ de l’hôte, blocage des répliques. Motif : AD-12.
- [x] `client/src/sync/npc.{h,cpp}`, `client/src/sync/avatar.cpp`, `client/src/sync/session.cpp` : inscription des PS (hôte et invité), rejeu. Motif : AD-8, AD-9.
- [x] `tools/fake-guest/` : codec v10, bilan d’animations par NetId, tests. Motif : banc.
- [x] `client/tests/session_host_main.cpp`, `session_fake_guest.sh`, `client/README.md`, `README.md` : PNJ animés au banc, procédure de test en jeu, v10.

**Acceptance Criteria :**
- Given les tests Linux, Go et le banc Wine, when on construit, then tout passe et les vecteurs v10 concordent en C++ et en Go.
- Given Risen en hôte et le faux invité près d’un camp, when des monstres marchent et attaquent, then le faux invité journalise des `Motion`/`MotionParam` par NetId de PNJ (8 au plus), et le bilan de l’hôte reste sous 32 Ko/s pour les PNJ. *(Manuel.)*
- Given deux machines, when l’hôte combat un monstre, then chez l’invité la réplique marche et attaque comme chez l’hôte, sans animation propre. *(Manuel, validation finale.)*

## Implementation Notes

- **Commandes** (`core/session`) : hôte `CaptureNpc{net_id, guid}` / `UncaptureNpc{net_id}` à
  chaque changement du groupe des 8 (`kMaxAnimatedNpcs`), au `Despawn` et à `Ended` ; invité
  `PlayNpcMotion` / `PlayNpcParam`. `LocalMotion` et `LocalParam` portent un `net_id` (0 = héros).
- **Hôte, PNJ capturé** : `sync/npc::capture` retrouve l'entité par `GetEntity(GUID)` (comme
  l'invité), inscrit son PS d'animation et son enveloppe au rôle `NpcCapture` (avec `net_id`) ;
  `npc::validate` revérifie aussi ces PNJ chaque frame (entité perdue → retirée du registre,
  journalisée une fois). `AnimFilter::classify(ptr, thread, net_id&)` rend `Capture` avec le NetId ;
  le filtre des réglages identiques côté hook est par PNJ (`npc_param_changed`,
  `npc_motion_started`), remis à neuf à l'inscription et au retrait. Le filtre du héros
  (`g_last_capture`) est inchangé.
- **Regroupement** : les `MotionParam` d'un PNJ attendent la fin des entrées du tick (un par
  (PNJ, type, nature), le dernier) ; un `Motion` du PNJ fait d'abord partir ceux de son type
  (l'ordre réglage → Motion est gardé), puis invalide le filtre de ce type. Ordre d'envoi en fin
  de frame : celui de la dernière mise à jour. Le filtre des réglages identiques
  (`Session::keep_param`) est celui du héros, extrait sans changement de comportement.
- **Rejeu** : `Session::play_due` est la logique de `play_due_motions` extraite, appliquée à la file
  de l'avatar (inchangée, mêmes logs et compteurs) ou à celle de chaque `Replica`. Pas de ligne de
  debug par geste rejoué pour les PNJ (compteurs seulement). `sync/npc::play`/`play_param` copient
  le rejeu de l'avatar, avec leur propre cache de ressources (vidé hors session).
- **Budget** : octets des `Motion`, `MotionParam` et `AniName` des PNJ (messages, sans KCP),
  ramenés à la seconde sur 5 s ; au-delà de 32768 o/s, un `Warn` par bilan, rien n'est retenu.
- **Bilan** : compteurs ajoutés en fin de ligne (`animations des PNJ : animés …, Motion envoyés …,
  rejoués …, écrasés …, ignorés …, file pleine …, MotionParam envoyés …, regroupés …, rejoués …,
  ignorés …, X o/s`).
- **Diagnostics partagés** : les réglages bloqués sur une réplique s'ajoutent au compteur debug
  « bloqués sur l'avatar » de `avatar::log_wrapper_stats`, et `AnimFilter::take_blocked` compte
  aussi les `PlayMotion` bloqués sur les répliques (journal du retrait de l'avatar).
- **Banc** : `session_host --npcs` anime les PNJ capturés (un `PlayMotion` par 200 ms, trois
  réglages par frame) ; le faux invité compte `Motion`/`MotionParam` par NetId et le nombre de
  PNJ distincts animés par seconde (relèves comprises : 13 au banc, 8 à la fois côté hôte).
- **Non fait** : tests en jeu (une machine avec le faux invité, puis deux machines).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge, verif-gap | Capture échouée ou PNJ capturé perdu : la session le croit animé, aucune recapture tant qu’il reste dans les 8 ; journal « échec » et « perdu » confondus | medium | patch | `set_animated_npcs` n’émet `CaptureNpc` qu’à l’entrée ; `npc::validate` retire sans retour ; recapture interne à `sync/npc`, réessai ≤ 1/s. |
| 2 | verif-gap | Rejeu des animations d’une réplique avant tout snapshot non testé | low | patch | Tous les tests invité passent par `guest_with_npcs()` (horloge prête). |
| 3 | edge | Faux invité : compteurs du héros (`MotionsRx`, `ParamsRx`) gonflés par les PNJ | low | patch | Incrémentés avant la branche PNJ (session.go:711-724). |
| 4 | blind | Commentaire de `g_resources` faux (vidé seulement par `release_all`) | low | patch | Correction directe. |
| 5 | blind, verif-gap | Réplique tuée localement par le héros invité (avant 3.5) : reste debout, animations et ragdoll bloqués ; README la dit fonctionnelle sans test en jeu | low | patch | `blocks_locked` vrai pour le PS de la réplique ; README mis à jour (limitation, « à valider en jeu »). |
| 6 | blind | PNJ entrant dans les 8 (et réplique à l’apparition) sans état d’animation initial : figé jusqu’au `PlayMotion` suivant | medium | defer | Comportement voulu par la matrice (« à partir du `PlayMotion` suivant ») ; à observer en jeu, piste : renvoyer l’animation courante à la capture. |
| 7 | blind, edge | Anneau de 256 captures partagé héros/PNJ : saturation possible, et filtre `npc_last_` mis à jour même si la capture est perdue | maybe-false | defer | Medium si vrai ; le compteur « captures perdues » en jeu tranchera. |
| 8 | blind | Charge des PNJ sur la file KCP non bornée (heartbeat retardé) ; octets sans en-têtes KCP | maybe-false | defer | Report existant (`ikcp_send` sans limite) ; mesurer `ikcp_waitsnd` en jeu. |
| 9 | verif-gap | Code DLL (`execute`, `npc::capture/play`, branche PNJ de `on_wrapper_call`) sans test automatique | medium | defer | Même couture `game::` que 3.1 à 3.3 ; procédure en jeu du README. |
| 10 | blind | 8e PNJ et répliques hors des 8 figées (recul par rapport à 3.3) | false | rejet | Décision Q12 de l’utilisateur. |
| 11 | blind | `MotionParam` envoyés dès `CaptureNpc`, avant le premier `Motion` | low | rejet | Réglages sans effet visible notable ; la matrice ne vise que les `Motion`. |
| 12 | blind | Compteurs « bloqués » de l’avatar mêlés aux répliques | low | rejet | Diagnostic debug seulement ; format des compteurs à garder. |
| 13 | blind | Faux invité n’applique pas la règle AniId inconnu / non fini | low | rejet | Outil de test ; l’hôte n’émet jamais ce cas. |
| 14 | blind | Correspondance `WrapperCall` → nature avec repli implicite sur 3 | low | rejet | L’énumération a exactement 4 valeurs. |
| 15 | blind | Deux filtres « identiques » (hook exact, session à epsilon) jamais testés de bout en bout | low | rejet | Chacun testé ; le second ne fait que filtrer davantage. |
| 16 | blind | Lignes citées dans `deferred-work.md` déjà décalées | low | rejet | Les fonctions sont nommées à côté. |
| 17 | edge | Lot de réglages : ordre de dernière mise à jour, pas de capture (poids, arrêt, poids) | low | rejet | L’état final rejoué est celui de l’hôte. |
| 18 | edge | Réglages d’autres types envoyés après le `Motion` | low | rejet | Types (couches) indépendants. |
| 19 | edge | Débit du budget divisé par 5 000 ms fixes | low | rejet | Bilan cadencé à 5 s ; écart négligeable. |
| 20 | edge | Réplique sans enveloppe : réglages perdus en silence | low | rejet | Avertissement à l’apparition déjà émis. |
| 21 | edge | `session_host_main` : `t` du premier tour calculé avant `session_start` | low | rejet | Banc seulement, valeur sinus sans effet. |
| 22 | edge (claim) | Spec : « l’invité bloque toute animation » faux sans PS ou enveloppe | low | rejet | Cas journalisé à l’apparition. |

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : tout passe.
- `cd tools/fake-guest && go test ./...` : tout passe, vecteurs v10 compris.

**Manual checks :**
- En jeu, Risen en hôte et faux invité : bilan `[sync] snapshots (5 s)` (compteurs des PNJ, octets par seconde), bilan du faux invité par NetId.
