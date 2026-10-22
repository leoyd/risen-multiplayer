---
title: 'Story 3.4b — Mort des répliques de PNJ'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: '7d01443c86163349ba4dcf2b653811c0e8959996'
route: 'auto'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-4-animations-pnj.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-4a-robustesse-session.md'
warnings: ['oversized']
deferred:
  - summary: >-
      Chemin DLL de la mort côté invité (routage NpcDeath, npc::die, gardes des répliques mortes, PropertyAIMode) sans test automatique.
    evidence: |-
      sync/ n'est pas compilé dans client_tests et le banc Wine n'appelle pas OnProcess ; couvert par la compilation et les étapes 3 à 6 du « Test en jeu à faire ». Symboles PropertyAIMode ajoutés à la vérification de run_scenarios.sh.
    location: >-
      client/src/sync/npc.cpp die ; client/src/sync/session.cpp execute
    severity: medium
  - summary: >-
      Un PNJ dont le cadavre quitte Entity::ms_arrNPCs (ou dont IsDead échoue) reçoit Despawn après 2 s au lieu de Death : sa copie chez l'invité garde son IA.
    evidence: |-
      maybe-false : dépend du moteur ; l'étape 1 du test en jeu (ligne « PNJ N mort (Death) » contre « absent de la liste depuis 2 s ») tranchera. Piste : revérifier par GetEntity(GUID) + IsDead les PNJ annoncés absents de la liste.
    location: >-
      client/src/core/session.cpp select_npcs ; client/src/sync/npc.cpp read_host
    severity: medium (unverified)
  - summary: >-
      Le PNJ natif de l'invité reste mort (AIMode 12, 0 PV) sans ses scripts de mort, y compris dans sa sauvegarde : une quête de l'invité liée à ce PNJ pourrait rester bloquée.
    evidence: |-
      maybe-false : conséquence de la décision Q11 (pas de Kill natif) ; à évaluer avec la persistance du profil invité (J4) ; zone de départ neutre hors trame principale.
    location: >-
      client/src/sync/npc.cpp die
    severity: medium (unverified)
---

<intent-contract>

## Intent

**Problem:** Aujourd’hui, un PNJ qui meurt chez l’hôte reçoit `Despawn` : chez l’invité, la réplique est libérée, son IA repart et le monstre reste vivant chez lui (FR-9, epics.md story 3.4, critère « mort »). De plus, un PNJ vivant à 0 PV chez l’hôte (humain à terre) fait écrire 0 PV sur la réplique, ce qui pourrait déclencher une mort ou un « à terre » local (report de la 3.3).

**Décision utilisateur (Q11, option A, 2026-10-22) :** l’hôte envoie `Death{NetId}` à la place du `Despawn` de mort. La réplique meurt chez l’invité **une seule fois**, mort « visuelle » **sans `Kill` natif** : ni XP, ni script, ni quête chez l’invité. Le butin reste propre à chaque joueur (chacun pille son propre PNJ natif). La réplique ne réapparaît pas, n’est plus déplacée ni animée de la session.

**Approach:** L’hôte remplace le `Despawn` « mort » par `Death{NetId}` et n’annonce plus jamais ce NetId de la session. L’invité applique la mort au même instant d’affichage que le dernier snapshot reçu : `SetHitPoints(0, fixer)`, `AIMode = 12`, puis ragdoll sous `ReplayGuard`, la réplique restant au registre (IA et animations natives bloquées) jusqu’à `Ended`. Une réplique vivante n’a jamais moins de 1 PV écrit chez l’invité. Protocole **v11**.

## Boundaries & Constraints

**Always:**
- Hôte (`core/session`, `select_npcs`) :
  - un PNJ annoncé qui sort de la sélection parce qu’il est mort (`NpcState.alive` faux) reçoit `Death{NetId}` au lieu de `Despawn`, envoyé avant le snapshot du même tick, une fois par NetId ;
  - ce NetId est mémorisé comme mort jusqu’à la fin de session : jamais réannoncé, même si le PNJ redevient vivant ou repasse dans le rayon ;
  - il sort du groupe des 8 animés au même tick (`UncaptureNpc`, comme aujourd’hui).
- Invité (`core/session`) :
  - `Death{NetId ≥ 1000}` accepté seulement d’une réplique annoncée, ni morte ni déjà en attente de mort ; sinon ignoré, journalisé (Warn si NetId non annoncé, Debug si déjà morte) ;
  - la mort s’applique quand le temps affiché atteint le temps du dernier snapshot reçu de cette réplique (tout de suite si aucun, ou si l’horloge n’est pas prête), après les gestes et PV dus et le dernier `MoveNpc` de cette frame, par la commande `NpcDeath{NetId}` ; les gestes encore en file sont jetés et comptés ignorés ;
  - ensuite, plus aucun `MoveNpc`, `SetNpcHp`, `PlayNpcMotion` ni `PlayNpcParam` pour elle ; ses `Motion`/`MotionParam` reçus après le `Death` sont ignorés et comptés ; ses entrées de snapshot sont ignorées sans être comptées « inconnues » ;
  - la réplique morte reste dans la table jusqu’à `Ended` : un `Spawn{1}` de ce NetId est refusé (« NetId déjà annoncé »), un `Despawn` de ce NetId la libère normalement ;
  - PV d’une réplique vivante : une valeur affichée de 0 est écrite **1** (`SetNpcHp{id, 1}`) ; seul `NpcDeath` met 0 PV.
- DLL (`sync/npc::die`) : sur la réplique trouvée, dans l’ordre `SetHitPoints(0, fixer)`, `AIMode = 12` (`PSRoutine::PropertyAIMode::operator=`), puis `SetRagDollEnabled(1, 0, nul)` sous `AnimFilter::ReplayGuard`. Chaque échec est journalisé et n’empêche pas les étapes suivantes ; la réplique est marquée morte quoi qu’il arrive (plus de pose, de PV, de rejeu). Elle **reste au registre** (rôle `NpcReplica`) : son `OnProcess` de routine, ses `PlayMotion`, réglages et ragdoll natifs restent bloqués. Une ligne Info par mort.
- Fin de session ou de monde : une réplique morte est libérée comme les autres (sortie du registre, oubliée, aucun appel au jeu) ; elle reste morte chez l’invité (AIMode 12, 0 PV). `release` journalise « reste morte ».
- Lignes de log (Info) : hôte `[sync] PNJ N mort (Death)` ; invité `[sync] PNJ N : Death reçu : mort à l’instant d’affichage` (core) et `[sync] PNJ N mort : PV 0, AIMode 12, ragdoll` (DLL, étapes en échec nommées en Warn/Error) ; `release` d’une morte : `[sync] PNJ N libéré : reste morte`.
- Bilan `[sync] snapshots (5 s)` : compteur ajouté **en fin** de ligne : `morts de PNJ : envoyées %u, appliquées %u, ignorées %u`. Format et ordre des compteurs existants inchangés.
- Protocole **v11** : `Death` inchangé en format ; règle §8 « Mort des répliques » ; `Despawn` n’a plus la cause « mort » ; vecteurs `hello*.bin` régénérés (version 11), nouveau vecteur `death_npc.bin` (`net_id = 1000`) ; codecs, tests C++ et Go, faux invité (AD-10).
- `game::resolve_npc` résout aussi `PropertyAIMode` (lecture et `operator=`), en tout ou rien ; `get_ai_mode`/`set_ai_mode` utilisent `pick_npc` comme `set_hit_points`.

**Never:**
- Pas de `Kill`, `Defeat`, `DownOrDeadBy*`, `PSRoutine::SetState`, `EnableClassProcessing` ni destruction d’entité sur une réplique. Aucun nouveau hook.
- Pas de dégâts (3.5, 3.6), pas de changement du comportement de l’avatar ni de la mort du héros (`Death{1|2}` inchangés).
- Pas de suppression ni de transfert d’inventaire : le butin natif de chaque joueur reste intact.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Mort chez l’hôte | PNJ annoncé, `alive` faux au tick suivant | `Death{id}` (pas de `Despawn`), avant le snapshot ; `UncaptureNpc` s’il était animé ; compteur « envoyées » | N/A |
| Mort non réannoncée | même GUID de nouveau vivant et dans le rayon | aucun `Spawn`, pas d’entrée de snapshot | N/A |
| Réception invité | réplique 1000 vivante, snapshot tick 10 reçu, puis `Death{1000}` | `NpcDeath{1000}` quand le temps affiché atteint le tick 10, après `MoveNpc` de cette frame ; une seule fois | N/A |
| Gestes en retard | `Motion`/`MotionParam` de 1000 après le `Death` | ignorés, comptés ; aucun `PlayNpc*` après `NpcDeath` | N/A |
| Snapshot après la mort | entrée NetId 1000 après `NpcDeath` | ni `MoveNpc` ni `SetNpcHp` ; pas compté « inconnue » | N/A |
| Doublon | second `Death{1000}` | ignoré (Debug), compteur « ignorées » | N/A |
| NetId inconnu | `Death{1500}` non annoncé | ignoré (Warn), compteur « ignorées » | N/A |
| Hôte reçoit `Death{≥1000}` | | ignoré (l’hôte fait autorité) | N/A |
| Réannonce | `Spawn{1}` 1000 après la mort | refusé « NetId déjà annoncé » | N/A |
| Fin de session | réplique morte, `Ended` | `ReleaseNpc{1000}` comme les autres | N/A |
| PNJ vivant à 0 PV | entrées hp 50 puis 0, réplique vivante | `SetNpcHp` 50 puis **1** | N/A |

</intent-contract>

## Code Map

- `client/src/core/session.h` :
  - `struct Replica` (≈ :541) : ajouter `death_pending`, `death_at`, `dead`.
  - Hôte : un `std::set<std::uint32_t> npc_dead_` (à vider dans `release_npcs`).
  - `NpcCounters` (≈ :550) : `deaths_sent`, `deaths_applied`, `deaths_ignored`.
  - Nouvelle commande `NpcDeath{net_id}` à côté de `ReleaseNpc` (≈ :290), ajoutée au `std::variant Command` (≈ :313).
  - Commentaire d’en-tête (≈ :48-66) : la mort.
- `client/src/core/session.cpp` :
  - `on_reliable` branche `Death` (≈ :582) : `net_id ≥ kFirstNpcNetId` → nouvelle `on_npc_death` (avant la logique de l’avatar).
  - `select_npcs` (≈ :738-856) : boucle des sorties (≈ :800-817). `outside` vaut « mort » → `Death` + `npc_dead_.insert` au lieu de `Despawn`. Les candidats dont le NetId (via `npc_ids_`) est dans `npc_dead_` sont écartés avant `kept`.
  - `on_npc_motion` (≈ :1340) et `on_npc_param` (≈ :1368) : réplique morte ou mort en attente → ignorés, comptés.
  - `on_npc_entity` / `push_npc_entity` (≈ :923-961) : réplique morte → retour `true` sans rien pousser.
  - `place_npcs` (≈ :975-996) : PV 0 → 1 pour une vivante ; application de la mort ; rien pour une morte.
  - `release_npcs` (≈ :998) ; `snapshot_stats` (≈ :1469-1504), compteur en fin.
- `client/src/sync/npc.{h,cpp}` :
  - `Replica` (cpp ≈ :30) reçoit `dead` ; nouvelle `die(net_id)`.
  - `move`, `set_hp`, `play`, `play_param` : sans effet sur une réplique morte (défense en profondeur).
  - `release` (≈ :392) : log « reste morte ».
  - `validate` : inchangé (une réplique morte perdue est oubliée comme les autres).
  - Modèle à suivre : `sync/avatar.cpp:474-494` (`die` : ragdoll sous `ReplayGuard`).
- `client/src/sync/session.cpp` : routage des commandes (≈ :216-247), ajouter `NpcDeath` → `npc::die`.
- `client/src/game/combat.{h,cpp}` :
  - `NpcAbi` (≈ :194) et `resolve_npc` (≈ :316) : `kGetAIMode`, `kSetAIMode` (déjà définis :79-81) ;
  - `get_ai_mode`/`set_ai_mode` (≈ :818-830) passent par `pick_npc`, comme `set_hit_points` (:718) ;
  - commentaires de `combat.h:65-71` et `:41`.
- `client/src/core/protocol.{h,cpp}` : `kProtocolVersion` 10 → 11. `client/tests/test_protocol.cpp` : vecteur `death_npc.bin`, version.
- `protocol/PROTOCOL.md` :
  - titre et historique (:1-13), table des types (:52), `Death` (:196-202) ;
  - §8 : « Répliques des PNJ », annonce (:937, plus de cause « mort ») et libération (:952) ; nouvelle puce « Mort des répliques (v11) » après « Animations des PNJ » ;
  - vecteurs : `hello*.bin` (version 11), `death_npc.bin` ; puis `protocol/vectors/regen.sh` et `regen.sh --check`.
- `tools/fake-guest/` :
  - `protocol/` : `ProtocolVersion` 11, test du vecteur `death_npc.bin` ;
  - `session.go` : branche `Death` (≈ :744), si NetId ≥ 1000 → `npcDeath`. Elle marque `npcStats.Dead`, `Present = false`, compte `NpcDeathsRx`, journalise `<- Death PNJ netid=%d (poses reçues=%d, PV=%d)`. Un NetId non annoncé ou mort est refusé (`NpcIgnored`). `npcSpawn` (≈ :814) refuse un NetId mort. Bilan final : nombre de morts ;
  - `session_test.go`/`fakehost_test.go` : test de la mort ; aide v11 (`main.go`).
- `client/tests/session_host_main.cpp` : le PNJ 0 meurt déjà à t > 2 s (:167). Il redevient vivant à t > 6 s, pour vérifier qu’il n’est pas réannoncé.
- `client/tests/session_fake_guest.sh` (scénario 3.3, ≈ :401-431) : l’attente « mort → Despawn » (:423) devient :
  - « mort → Death » : hôte `PNJ 1000 mort (Death)` ;
  - faux invité `<- Death PNJ netid=1000` ;
  - aucune ligne `annoncé (Spawn)` pour 1000 après la mort.
- `client/tests/test_session.cpp` : tests de la matrice. Patrons à suivre : `guest_with_npcs()` et les tests « mort : » de l’avatar (≈ :1702).
- Docs :
  - `client/README.md` : procédure « Mort des répliques » ;
  - `README.md` : protocole v11 ;
  - `_bmad-output/implementation-artifacts/deferred-work.md` : entrées « Story 3.4b » et « PNJ vivant à 0 PV » marquées faites (lignes :166, :169) ;
  - `epic-3-context.md` : 3.4a faite, 3.4b faite.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/`, `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp` -- v11, règle de mort des répliques, `death_npc.bin`, hello régénérés -- AD-10.
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` -- `Death` à la place du `Despawn` de mort, NetId morts jamais réannoncés, `on_npc_death`, `NpcDeath` à l’instant d’affichage, filtres des gestes, des snapshots et des PV (plancher 1), compteurs, un test par ligne de la matrice -- logique testée sous Linux.
- [x] `client/src/game/combat.{h,cpp}` -- `PropertyAIMode` dans `resolve_npc`, `pick_npc` -- AD-3, la session n’a pas la sonde.
- [x] `client/src/sync/npc.{h,cpp}`, `client/src/sync/session.cpp` -- `die`, garde des répliques mortes, log de `release`, routage de `NpcDeath` -- AD-8, AD-12.
- [x] `tools/fake-guest/` -- v11, `npcDeath`, refus de la réannonce, bilan, tests -- banc.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- PNJ 0 mort puis « revivant », attentes Death et non-réannonce -- bout en bout.
- [x] `client/README.md`, `README.md`, `deferred-work.md`, `epic-3-context.md` -- procédure en jeu, v11, reports soldés.

**Acceptance Criteria:**
- Given les tests Linux, Go et le banc, when on construit, then tout passe, `regen.sh --check` réussit et les vecteurs v11 concordent en C++ et en Go.
- Given le banc (`session_host --npcs`, faux invité), when le PNJ 1000 meurt, then le faux invité journalise `<- Death PNJ netid=1000`, aucun `Despawn` de 1000, et 1000 n’est jamais réannoncé.
- Given Risen en hôte et le faux invité, when l’hôte tue un monstre répliqué, then le log de l’hôte contient `PNJ N mort (Death)` et pas `retiré (Despawn) : mort`. *(Manuel.)*
- Given deux machines, when l’hôte tue un monstre, then chez l’invité la réplique tombe en ragdoll une fois, reste morte, ne se relève pas à la fin de session, et l’invité ne gagne pas d’XP. *(Manuel.)*

## Spec Change Log

## Review Triage Log

### 2026-10-23 — Review pass
- verdicts: 27 findings — high 0, medium 2, low 13, false 4, maybe-false 8
- findings:
  - `[maybe-false]` `[defer]` (intent) Lecture étroite de « un PNJ meurt chez l'hôte » : mort hors du rayon ou pendant le délai de grâce → Despawn (D8) — dépend de la présence des cadavres dans ms_arrNPCs ; étape 1 du test en jeu ; entrée `deferred`.
  - `[maybe-false]` `[defer]` (intent) PV plancher à 1 testé en logique pure ; l'effet moteur de SetHitPoints bas sur une réplique non vérifié — étape 6 du test en jeu ; groupé avec l'entrée `deferred` du chemin DLL.
  - `[medium]` `[defer]` (intent) Comportement en jeu (ragdoll unique, pas d'XP, butin, pas de relève) non démontré automatiquement — jeu non lançable ; étapes 3 à 5 ; entrée `deferred` du chemin DLL.
  - `[medium]` `[defer]` (verif-gap) Routage NpcDeath → npc::die, gardes `dead` et résolution PropertyAIMode sans test automatique — couche sync/ non testable sous Linux (comme 3.3/3.4) ; garde partielle appliquée : symboles PropertyAIMode ajoutés à `run_scenarios.sh` ; entrée `deferred`.
  - `[low]` `[patch]` (verif-gap) Faux invité : Despawn après Death refusé, contrairement au C++ — `npcDespawn` accepte un PNJ mort (`Released`), test Go étendu.
  - `[medium]` `[patch]` (edge) Mort en attente perdue si Ended/fin de monde arrive avant `death_at` : réplique libérée vivante — `apply_pending_death` émis avant chaque `ReleaseNpc` (`release_npcs`, `on_npc_despawn`), test C++ ajouté.
  - `[low]` `[patch]` (edge) Snapshot plus récent arrivé après le Death : `death_at` figé, dernière pose sautée — `death_at = max(death_at, last_time)` dans `push_npc_entity`, test C++ ajouté.
  - `[low]` `[reject]` (edge) GUID en double, l'un mort et l'autre vivant hors rayon : raison choisie par l'ordre de la liste — GUID dupliqué jamais observé (déjà dédoublonné en 3.3) ; correctif = règle de priorité supplémentaire.
  - `[low]` `[patch]` (edge) « appliquées » compté alors que la réplique manque côté DLL, sans trace — `die` journalise un Warn « Death sans réplique chez l'invité ».
  - `[false]` `[reject]` (edge) Un symbole PropertyAIMode absent couperait toute la réplication — les deux symboles sont exportés par `risen_ori/bin/Script.dll` (objdump : `??4…AEBW4gEAIMode@@@Z`, `??B…XZ`) et déjà résolus par la sonde.
  - `[low]` `[patch]` (edge) Faux invité : Despawn d'un PNJ mort ignoré — même correctif que la ligne verif-gap.
  - `[low]` `[patch]` (edge) Faux invité : entrées de snapshot d'un PNJ mort comptées « inattendues » — ignorées sans compter, comme en C++.
  - `[medium]` `[patch]` (edge, claim) Réplique tuée juste avant la fin de session libérée vivante — même correctif que `apply_pending_death`.
  - `[maybe-false]` `[defer]` (blind) Cadavre retiré de ms_arrNPCs ou IsDead en échec → Despawn au lieu de Death — même entrée `deferred` que la ligne intent.
  - `[false]` `[reject]` (blind) `SetHitPoints(0)` avant `AIMode = 12` contredirait D5 — `SetHitPoints` saute vers `SetBase` sans décision (spike J2) ; « à terre »/mort sont décidés par `DownOrDeadBy*` après un coup ; `Kill` fait le même ordre. D5 vise une réplique vivante dont l'AIMode n'est pas 12.
  - `[medium]` `[patch]` (blind) Mort en attente perdue à la fin de session — même correctif que `apply_pending_death`.
  - `[false]` `[reject]` (blind) D6 « symbole absent » impossible / réplication coupée — symboles présents (voir plus haut) ; libellé du log sans effet.
  - `[low]` `[patch]` (blind) Faux invité divergent (Despawn après Death, entrées comptées) — correctif ci-dessus.
  - `[low]` `[patch]` (blind) Death dépassé par le dernier snapshot — correctif `max(death_at, …)`.
  - `[low]` `[patch]` (blind) « appliquées » sans mort réelle — Warn ajouté dans `die`.
  - `[low]` `[patch]` (blind) Logs d'oubli (`drop`, `release_all`) muets sur les répliques mortes — « dont K morte(s), laissée(s) mortes » et « reste morte ».
  - `[maybe-false]` `[defer]` (blind) Sauvegarde de l'invité : PNJ de quête mort sans ses scripts — entrée `deferred` (J4).
  - `[low]` `[reject]` (blind) `SetNpcHp 1` puis `NpcDeath` (0) dans la même frame — deux écritures de PV sans effet visible, une ligne debug ; correctif = branche supplémentaire.
  - `[low]` `[reject]` (blind) Marqueur « mort » comparé par adresse (`kDead`) — constante locale à `select_npcs`, unique point d'émission ; risque seulement en cas de modification future.
  - `[maybe-false]` `[reject]` (blind) Invité : Death → Despawn → Spawn du même NetId accepterait une réplique — l'hôte n'envoie jamais cette suite (NetId mort jamais réannoncé, testé) ; au pire low.
  - `[low]` `[reject]` (blind) `TestNpcDeath` dépend d'un compte d'un autre scénario — outil de test, rupture visible immédiatement.
  - `[low]` `[patch]` (blind) `client/README.md` : phrase 3.3 fausse pour une réplique morte, phrase 3.4a maladroite — corrigées.

## Design Notes

**Décisions prises sans l’utilisateur (option la plus prudente, à relire) :**
- D1 — **Version 11.** Le format de `Death` ne change pas, mais sa sémantique s’étend aux PNJ et le `Despawn` de mort disparaît. Un invité v10 garderait sinon une réplique vivante figée. La règle « une version par story qui modifie un message » s’applique.
- D2 — **Ordre de la mort chez l’invité** : PV 0, puis `AIMode = 12`, puis ragdoll. Hypothèses :
  - `Entity::IsDead` lit `AIMode == 12` (spike J2) : le héros de l’invité voit donc un cadavre, pillable et non ciblé ;
  - `Kill` (Script_Game) fait ces deux écritures, puis ses propres effets (XP, scripts). En ne l’appelant pas, on évite ces effets ;
  - `PropertyAIMode::operator=` est une simple affectation de propriété, sans rappel de script. À vérifier en jeu : aucune ligne d’XP ni de quête chez l’invité.
- D3 — **La réplique morte reste au registre jusqu’à `Ended`.** Son `OnProcess` de routine reste bloqué : `ContinueRoutine_Base` ne l’envoie donc pas en `ZS_Dead` pendant la session, et aucun script de mort ne tourne. Les `PlayMotion`, réglages et `SetRagDollEnabled` natifs restent bloqués : les `Motion` en retard ne la réaniment pas, et le moteur ne coupe pas le ragdoll.
- D4 — **À la fin de session, la réplique morte est libérée, pas gardée** (AD-12 interdit tout pointeur d’entité après `Ended`). Elle reste morte chez l’invité (AIMode 12, 0 PV) et sa routine native reprend, sans doute en `ZS_Dead`, l’état normal d’un cadavre. Relever un mort n’est pas fiable (spike J2, Q7). Le cadavre persiste dans la sauvegarde de l’invité : c’est voulu, le monstre a été tué dans la session. Hypothèse à vérifier au log et à l’écran : pas de relève, pas de son ni d’XP à la libération.
- D5 — **PNJ vivant à 0 PV (humain à terre chez l’hôte)** : la réplique vivante reçoit au plus bas 1 PV. Aucune décision « à terre » ou mort locale ne peut donc partir chez l’invité. L’écart d’affichage (1 au lieu de 0) est négligeable. La posture « à terre » arrive par les animations rejouées (3.4).
- D6 — **Échec partiel de `die`** (SEH, ou symbole `PropertyAIMode` absent) : on journalise et on marque la réplique morte quand même. Une réplique figée debout vaut mieux qu’une réplique qui reprendrait vie. `resolve_npc` reste en tout ou rien : `PropertyAIMode` est exporté par `Script.dll` (déjà résolu par la sonde).
- D7 — **NetId mort jamais réannoncé, même revenu à la vie** (respawn scripté rare). Un PNJ ressuscité chez l’hôte n’est donc plus répliqué de la session : c’est l’option prudente, alignée sur « ne réapparaît pas de la session ».
- D8 — **Limite connue, non traitée** : un PNJ qui meurt hors du rayon, ou pendant son délai de grâce « absent de la liste », reçoit `Despawn` et non `Death`. Sa copie chez l’invité garde son IA. C’est le comportement de la 3.3 pour tout PNJ hors sélection.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: tout passe, banc `session_fake_guest` compris.
- `cd tools/fake-guest && go test ./... && go build` -- expected: tout passe, vecteurs v11 concordants.
- `protocol/vectors/regen.sh --check` -- expected: tous `[ OK ]`, `death_npc.bin` compris.

## Test en jeu à faire

Préparation : recompiler, copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/`, `log_level = debug`, overlay Steam désactivé ; reconstruire le faux invité (`cd tools/fake-guest && go build`). Log : `bin/scripts/Script_Multiplayer.log`.

1. **Une machine : Risen en hôte et faux invité** (`fake-guest --host 127.0.0.1 --duration 180s --echo`), près d’un camp de monstres. Tuer un monstre avec le héros de l’hôte.
   - Attendu chez l’hôte : `[sync] PNJ N mort (Death)`, sans `retiré (Despawn) : mort` pour ce NetId ; `PNJ N : plus animé` au même tick s’il était animé.
   - Bilan : `morts de PNJ : envoyées 1, …`.
   - Attendu chez le faux invité : `<- Death PNJ netid=N (poses reçues=…, PV=…)`, puis plus aucune pose de N.
   - S’éloigner à plus de 50 m, puis revenir : aucun nouveau `annoncé (Spawn)` pour ce GUID.
2. **Une machine, humain à terre** : mettre à terre un humain (combat à mains nues ou arme contondante, sans le tuer). Attendu : l’hôte n’envoie **pas** de `Death` pour lui (pas de `PNJ N mort (Death)`, l’humain n’est pas mort) ; le faux invité ne journalise pas de `<- Death PNJ` pour ce NetId.
3. **Deux machines, mort d’un monstre** : l’hôte tue un monstre proche de l’invité. Attendu chez l’invité :
   - au log : `[sync] PNJ N : Death reçu`, puis `[sync] PNJ N mort : PV 0, AIMode 12, ragdoll`, une seule fois ;
   - à l’écran : la réplique tombe en ragdoll et reste au sol ;
   - aucun message d’XP ni de quête ;
   - le bilan montre `morts de PNJ : … appliquées 1`.
   Si `AIMode` ou le ragdoll échouent, une ligne Warn ou Error donne l’étape.
4. **Deux machines, butin** : l’invité fouille le cadavre de sa réplique. Il doit trouver l’inventaire natif de son propre PNJ. L’hôte fouille le sien de son côté.
5. **Deux machines, fin de session** : l’invité quitte, ou l’hôte recharge. Attendu chez l’invité :
   - au log : `PNJ N libéré : reste morte` (ou l’oubli « fin de monde ») ;
   - à l’écran : le cadavre ne se relève pas, ne joue pas de son de mort et ne donne pas d’XP ;
   - si la routine native relève le PNJ, noter le fait pour la 3.5 (hypothèse D4 invalidée).
6. **Deux machines, humain à terre** : la réplique d’un humain à terre chez l’hôte garde au moins 1 PV chez l’invité (`[sync] PNJ N : PV = 1` en debug), sans mort locale.

## Auto Run Result

Status: done

**Résumé :** l'hôte envoie `Death{NetId}` (et non plus `Despawn`) pour un PNJ annoncé qui meurt, et ne le réannonce plus de la session. L'invité tue sa réplique une seule fois, à l'instant d'affichage du dernier snapshot reçu : PV 0, `AIMode = 12`, ragdoll sous `ReplayGuard`, sans `Kill` natif. La réplique morte reste au registre jusqu'à `Ended` (IA, animations et ragdoll natifs bloqués), puis elle est libérée et reste morte. Une réplique vivante n'a jamais moins de 1 PV. Le protocole passe en v11.

**Fichiers :**
- `protocol/PROTOCOL.md`, `protocol/vectors/` (hello*.bin v11, `death_npc.bin`), `client/src/core/protocol.h` : v11, règle « Mort des répliques ».
- `client/src/core/session.{h,cpp}` : `Death` à la place du `Despawn` de mort, `npc_dead_`, `on_npc_death`, `apply_pending_death`, `NpcDeath`, plancher de PV, compteurs en fin de bilan.
- `client/src/game/combat.{h,cpp}` : `PropertyAIMode` dans `resolve_npc`, `pick_npc`.
- `client/src/sync/npc.{h,cpp}`, `client/src/sync/session.cpp` : `die`, gardes des répliques mortes, logs de libération et d'oubli.
- `client/tests/test_session.cpp` (14 tests nouveaux), `test_protocol.cpp`, `session_host_main.cpp`, `session_fake_guest.sh`, `wine/run_scenarios.sh`.
- `tools/fake-guest/` : v11, `npcDeath`, Despawn d'un PNJ mort, tests, README.
- `client/README.md`, `README.md`, `deferred-work.md`, `epic-3-context.md`.

**Revue :** 27 constats, dont 10 lignes corrigées (2 medium, qui partagent un seul correctif, plus 8 low). 6 lignes sont reportées, groupées dans 3 entrées `deferred`. 11 sont rejetées : 4 false, 6 low, 1 maybe-false, avec leurs motifs dans le journal.

**Revue de suivi recommandée :** non. Un seul correctif medium (la mort en attente à la libération), aucun high.

**Vérification :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : 6/6, après les correctifs.
- `go test ./... && go build` dans `tools/fake-guest` : OK.
- `protocol/vectors/regen.sh --check` : 29/29.

**Risques résiduels :**
- Aucun test en jeu : le jeu ne peut pas être lancé ici.
- Hypothèses D2 (pas de script sur `PropertyAIMode`) et D4 (pas de relève à la libération) non vérifiées.
- Le chemin des cadavres dans `ms_arrNPCs` reste à voir (voir `deferred`).
