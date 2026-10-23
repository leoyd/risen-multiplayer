---
title: 'Story 3.6 — Dégâts subis par l’invité'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_revision: 'eff5a8085a887b498df2d454fa60fc788cfb85ca'
route: 'auto'
review_loop_iteration: 0
followup_review_recommended: false
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-3-5-degats-invite.md'
  - '{project-root}/docs/re/spike-j2.md'
warnings: ['oversized']
deferred:
  - summary: >-
      La couture DLL des coups subis (lecture des dégâts bruts dans le détour, file {source, dégâts}, drain vers LocalHitOnGuest, guest_health::apply_hit et son SetHitPoints) n'a aucun test automatique.
    evidence: |-
      client_tests ne compile que sync/hit_rules.cpp ; le banc injecte LocalHitOnGuest directement et test_session part de LocalHitOnGuest/ApplyHitOnGuest. Une régression (file non remplie, source perdue, écriture sans r.write) passerait tous les tests. Couvert seulement par les étapes 2, 6 et 7 du « Test en jeu à faire » ; même manque de couture game:: que les stories 3.3 à 3.5.
    location: >-
      client/src/sync/hits.cpp decide/drain ; client/src/sync/guest_health.cpp apply_hit
    severity: medium (unverified)
  - summary: >-
      Les dégâts bruts (Amount + Bonus) × Multiplier du PS pourraient être très éloignés des dégâts natifs (force, protection), rendant les monstres trop faibles ou trop forts pour l'invité.
    evidence: |-
      maybe-false : choix C1 consigné (D1, hypothèse H2) ; tranché par les étapes 2 et 3 du « Test en jeu à faire » (comparer D à la perte native du héros de l'hôte face au même monstre).
    location: >-
      client/src/sync/hit_rules.cpp raw_damage ; client/src/sync/hits.cpp decide
    severity: medium (unverified)
---

<intent-contract>

## Intent

**Problem:** Les coups des monstres sur l’invité ne suivent aucune règle. Chez l’hôte, l’avatar invité encaisse nativement et peut mourir (`deferred-work.md`). Chez l’invité, une réplique qui rejoue une attaque peut blesser nativement le héros. Les PV du héros invité ne reflètent donc pas les combats de l’hôte, qui fait autorité (FR-11).

**Approach:** On étend les règles du détour `gCDamage_PS::DoDamage` de la 3.5 (`sync/hit_rules`, `sync/hits`), sans nouveau hook.
- Chez l’hôte, tout coup natif sur l’avatar invité est bloqué (invulnérabilité). Un coup d’un PNJ devient `HitOnGuest{NetId source, dégâts bruts}`.
- Chez l’invité, tout coup natif sur son héros est bloqué. Un `HitOnGuest` reçu baisse ses PV par un seul écrivain, `sync/guest_health`, qui ne descend jamais sous 1 PV en attendant la 3.7.
- Protocole **v13**.

## Boundaries & Constraints

**Always:**
- **Règles du détour** (`sync/hit_rules::decide`, pures, testées sous Linux). Les règles 1 à 4 de la 3.5 restent inchangées. On ajoute :
  - **3b — invité, cible = son héros** (`target == hero`, ou rôle `LocalHero` au registre) : **bloqué**, quel que soit l’agresseur, rien en file. Raisons : `GuestHeroByReplica` si l’agresseur est une réplique du registre, `GuestHeroOther` sinon.
  - **4b — hôte, cible = avatar invité** (registre, rôle `Avatar`) : **bloqué** (invulnérable), évalué **après** la règle 4. Selon l’agresseur :
    - le héros de l’hôte → `AvatarFriendly`, aucune émission (pas de tir ami, symétrique de D5 en 3.5) ;
    - un agresseur nul → `AvatarNoAggressor`, aucune émission ;
    - sinon → `AvatarHit`, `HitOnGuest` à émettre. Source = NetId de l’agresseur s’il est au registre en rôle `NpcCapture`, sinon **0** (source inconnue).
  - Les autres cas sont inchangés : un PNJ qui frappe le héros de l’hôte est **laissé**. Garde de réentrance active → laissé (règle 1, inchangée).
- **Dégâts bruts** (hôte, lus dans le détour sous SEH, seulement pour `AvatarHit`) :
  - `(GetDamageAmount + GetDamageBonus) × GetDamageMultiplier` du `gCDamage_PS` reçu. Getters exportés : `lea +0x44`, `+0x40`, `+0x48` (float), vérifié au désassemblage.
  - Calcul pur `hit_rules::raw_damage(long, long, float) → uint32` : arrondi au plus proche ; un résultat non fini ou ≤ 0 donne 0 ; plafond `kMaxRawDamage = 1000000`.
  - Lecture impossible : le coup reste bloqué, sans émission, avec un Warn une fois par session `[sync] dégâts : dégâts bruts illisibles (SEH) : coup sur l’avatar bloqué sans HitOnGuest`.
- **File hôte** : `{source, dégâts}`, 64 au plus, sous le verrou existant. `drain` produit les `LocalHitOnGuest{source, damage}`.
- **Lignes du détour** :
  - Info par coup bloqué, au format 3.5 : `[sync] coup natif bloqué : <agresseur> -> avatar invité (HitOnGuest, dégâts bruts D)`, `… -> héros (seuls les HitOnGuest blessent)`, `(pas de tir ami)`, `(agresseur nul, sans HitOnGuest)` ;
  - la ligne Debug `dégâts : agresseur … cible …` existante est conservée.
- **Core, hôte** : `LocalHitOnGuest`, accepté seulement en `InSession` et rôle hôte. On envoie `HitOnGuest` en fiable et on journalise en Info `[sync] coup sur l’avatar invité (source PNJ N|inconnue) : HitOnGuest envoyé, dégâts bruts D`. Sinon : ignoré, ligne Debug, compteur.
- **Core, invité** : un `HitOnGuest` reçu donne la commande `ApplyHitOnGuest{source, damage}`. Un hôte qui en reçoit un l’ignore : Warn `[sync] HitOnGuest ignoré : l’hôte ne subit pas de coups`, compteur.
- **Bilan** `[sync] snapshots (5 s)` : on ajoute **à la fin** `; coups subis : HitOnGuest envoyés %u, reçus %u, ignorés %u`. Le format existant ne change pas.
- **DLL, invité** (`sync/guest_health::apply_hit`, **seul écrivain** des PV du héros invité, AD-11) :
  - lit les PV, calcule avec la fonction pure `hit_rules::guest_hp_after(hp, damage)`, puis écrit par `set_hit_points(hero, nouveau, kSkillModifierSet)` ;
  - `damage = 0` → pas d’écriture ;
  - `hp − damage ≤ 0` → PV **fixés à 1** (`held`) ;
  - `hp ≤ 1` au départ → pas d’écriture, `held`.
  - Lignes : Info `[sync] HitOnGuest (source PNJ N|inconnue) appliqué : dégâts bruts D, PV a -> b`. Si `held` : Warn `[sync] HitOnGuest mortel (source …) : héros maintenu à 1 PV (à terre : story 3.7)`. Héros absent ou PV illisibles : Warn, aucune écriture. SEH : Error, aucune écriture.
- **Protocole v13** : `HitOnGuest` = type **23**, `{source_net_id u32, damage u32}`, 9 octets, fiable, de l’hôte vers l’invité. Source 0 = inconnue.
  - Mises à jour : `PROTOCOL.md` (table, section, §8, historique, versions citées) ; vecteur `hit_on_guest.bin` = `17 e8 03 00 00 12 00 00 00` (source 1000, 18 dégâts) ; `hello*.bin` en v13 ; codecs C++ et Go ; tests.
  - `unknown_type.bin` (type 0x63) est inchangé.
- **Faux invité** : décode `HitOnGuest`, journalise `<- HitOnGuest source=N dégâts=D` et ajoute `HitOnGuest reçus=` à son bilan. Il n’applique rien.
- **Banc** : `session_host --hit-guest-ms N` injecte toutes les N ms `LocalHitOnGuest{1000 si --npcs ≥ 1, sinon 0, 18}`.

**Never:**
- Pas de nouveau hook. Pas de détour `DownOrDeadBy*` en session, pas d’émission de `Death{2}`, pas de mise à terre ni de `Respawn` : c’est la 3.7.
- Ne jamais lire les PV de l’avatar chez l’hôte pour détecter un coup. Ne jamais les écrire depuis le détour.
- Chez l’invité, ne jamais écrire les PV du héros hors de `sync/guest_health`. Ne jamais les mettre à 0. Pas de `Kill` ni de `DoDamage` sur le héros.
- Pas de protection ni d’armure appliquée aux dégâts bruts en J2.
- Aucune attente, aucun réseau dans le détour. Pas d’appel au jeu autre que les accesseurs (agresseur, dégâts bruts).

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Hôte, PNJ capturé frappe l’avatar | agresseur `NpcCapture` 1000, cible `Avatar` | bloqué, `HitOnGuest{1000, D}` en file | N/A |
| Hôte, agresseur hors registre frappe l’avatar | agresseur non nul inconnu | bloqué, `HitOnGuest{0, D}` | N/A |
| Hôte, son héros frappe l’avatar | agresseur = héros | bloqué, rien | N/A |
| Hôte, agresseur nul sur l’avatar | agresseur nul | bloqué, rien | N/A |
| Hôte, PNJ frappe le héros de l’hôte | cible = héros | laissé | N/A |
| Hôte, garde active, cible avatar | garde | laissé (règle 1) | N/A |
| Invité, réplique frappe son héros | agresseur `NpcReplica` | bloqué, rien en file | N/A |
| Invité, agresseur inconnu frappe son héros | hors registre | bloqué, rien | N/A |
| Dégâts bruts | (10 + 2) × 1,5 ; négatif ; NaN ; 1e12 | 18 ; 0 ; 0 ; 1000000 | N/A |
| PV invité | (100, 18) ; (10, 18) ; (1, 5) ; (50, 0) | 82 ; 1 held ; pas d’écriture, held ; pas d’écriture | N/A |
| Hôte, `LocalHitOnGuest` en session | `{1000, 18}` | `HitOnGuest` envoyé, « envoyés » +1 | N/A |
| Hôte hors session / invité qui produit `LocalHitOnGuest` | | rien envoyé, « ignorés » +1 | Debug |
| Invité reçoit `HitOnGuest` | `{1000, 18}` en session | commande `ApplyHitOnGuest{1000, 18}`, « reçus » +1 | N/A |
| Hôte reçoit `HitOnGuest` | | aucune commande, « ignorés » +1 | Warn |
| Vecteur | `hit_on_guest.bin` | décodé et réencodé à l’identique (C++ et Go) | N/A |

</intent-contract>

## Code Map

- `client/src/sync/hit_rules.{h,cpp}` : enum `Reason` (h:21), `Decision` (h:41) et `decide` (cpp:9).
  - Ajouter à `Decision` : `hit_on_guest` (bool) et `source_net_id`.
  - Ajouter les raisons, les règles 3b et 4b, `raw_damage`, `guest_hp_after` (struct `{new_hp, write, held}`) et `kMaxRawDamage`.
  - Mettre à jour le commentaire d’en-tête.
- `client/tests/test_hit_rules.cpp` : un test par ligne « règles », « dégâts bruts » et « PV invité » de la matrice.
- `client/src/sync/hits.{h,cpp}`, `decide` (cpp:107-171) :
  - après `hit_rules::decide`, si `d.hit_on_guest` : `game::damage_raw(ps, amount, bonus, mult)`, puis `raw_damage`, puis mise en file `g_guest_hits` ;
  - nouveaux libellés de raison (switch cpp:150) ;
  - `drain` (cpp:203) émet `LocalHitOnGuest` ;
  - `publish` (cpp:184) vide la nouvelle file et remet à zéro le Warn des dégâts bruts, comme pour `g_queue` ;
  - exposer `hero()` (lecture de `g_hero`) si `guest_health` en a besoin.
- `client/src/game/combat.{h,cpp}` :
  - `DamageAbi` (cpp:226) et `resolve_damage` (cpp:363) : ajouter `?GetDamageAmount@gCDamage_PS@@QEBAAEBJXZ`, `?GetDamageBonus@gCDamage_PS@@QEBAAEBJXZ`, `?GetDamageMultiplier@gCDamage_PS@@QEBAAEBMXZ` (Game.dll ; ils rendent une référence, donc un pointeur, sans sret) ;
  - nouvelle fonction `damage_raw(const void* ps, long&, long&, float&)` sous SEH, sur le modèle de `damage_aggressor` (cpp:385).
- `client/src/sync/guest_health.{h,cpp}` (nouveau, à ajouter au CMake du client à côté de `hits.cpp`) : `apply_hit(source, damage)`. Utilise `game::get_hit_points` / `set_hit_points` (combat.h:149,159) et `kSkillModifierSet`, comme `sync/npc.cpp:365`.
- `client/src/core/session.{h,cpp}` :
  - `Input` (h:226) + `LocalHitOnGuest{source, damage}` ; branche dans `tick` **avant** le `else` final (cpp:267-273) ;
  - `Command` (h:347) + `ApplyHitOnGuest{source, damage}` ;
  - `on_reliable` : branche `HitOnGuest` **avant** le `std::get<SessionEnd>` final (cpp:654-658) ;
  - compteurs `GuestHitCounters` à côté de `hits_` (h:611), remis à zéro aux mêmes endroits (cpp:704, :1635) ; bilan à la fin du format (cpp:1613), tampon de 1400 octets ;
  - modèles : `on_local_hit` (cpp:973), `on_hit_request` (cpp:999).
- `client/src/sync/session.cpp` : `execute()` (cpp:255), `ApplyHitOnGuest` → `guest_health::apply_hit`.
- `client/src/core/protocol.{h,cpp}` :
  - `kProtocolVersion` (h:25) → 13 ;
  - `MsgType::HitOnGuest = 23` ; mettre à jour le commentaire des réservés (h:40 : seul 25 reste réservé) ;
  - struct, `ReliableMessage` (h:228), `write` et `read` sur le modèle de `HitRequest` (cpp:161, :346), `decode_reliable` (cpp:439).
- `client/tests/test_protocol.cpp` :
  - vecteur sur le modèle de `hit_request.bin` (:724) ;
  - `kValidReliable`, boucle des réservés (retirer 23), valeurs figées (:581), version.
- `protocol/PROTOCOL.md`, puis `protocol/vectors/regen.sh` : l’hexdump de `PROTOCOL.md` fait foi. Suivre le modèle de `HitRequest` en 3.5. **Piège 3.5** : ne pas insérer la section du vecteur au milieu d’un tableau existant.
- `tools/fake-guest/protocol/{protocol.go,protocol_test.go}` : `TypeHitOnGuest = 23` (protocol.go:49), struct et codec (:183, :349, :415, :630), v13, réservés.
- `tools/fake-guest/session.go` : log et bilan à la réception. `main.go` : texte d’aide v13.
- `tools/fake-guest/fakehost_test.go`, `session_test.go` : l’hôte factice envoie des `HitOnGuest` ; on vérifie le log `<- HitOnGuest source=1000 dégâts=18`.
- `client/tests/session_host_main.cpp` : option `--hit-guest-ms` (argv :99-111, usage :111).
- `client/tests/session_fake_guest.sh` : nouveau scénario au port `base_port + 20`, sur le modèle de :471-495. Hôte `--npcs 1 --hit-guest-ms 500`, invité `--duration 3s --quit`. Attentes :
  - hôte `HitOnGuest envoyé, dégâts bruts 18` ;
  - invité `<- HitOnGuest source=1000 dégâts=18`.
- `client/tests/wine/run_scenarios.sh` (:145) : ajouter les 3 getters.
- `client/tests/test_session.cpp` : un test par ligne « session » de la matrice (patrons des tests `HitRequest` de 3.5).
- Documentation :
  - spine : AD-9 (pas d’armure en J2, source 0, tir ami) et AD-11 (`sync/guest_health`, maintien à 1 PV jusqu’à la 3.7) ;
  - `epics.md` : note 3.6 ;
  - `client/README.md` : procédure et diagnostic ;
  - `README.md` : v13 ;
  - `epic-3-context.md` : 3.6 faite ;
  - `deferred-work.md` : retirer l’entrée « avatar peut mourir chez l’hôte » (:130) ou la marquer résolue, et ajouter les reports.

## Tasks & Acceptance

**Execution:**
- [x] `protocol/PROTOCOL.md`, `protocol/vectors/`, `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp` -- v13, `HitOnGuest` (23), `hit_on_guest.bin`, hello régénérés -- AD-10.
- [x] `client/src/sync/hit_rules.{h,cpp}`, `client/tests/test_hit_rules.cpp` -- règles 3b et 4b, `raw_damage`, `guest_hp_after`, tests de la matrice -- logique pure testée sous Linux.
- [x] `client/src/core/session.{h,cpp}`, `client/tests/test_session.cpp` -- `LocalHitOnGuest`, `HitOnGuest` reçu, `ApplyHitOnGuest`, compteurs et bilan, tests -- AD-11.
- [x] `client/src/game/combat.{h,cpp}` -- getters des dégâts bruts dans `resolve_damage`, `damage_raw` sous SEH -- AD-3.
- [x] `client/src/sync/hits.{h,cpp}`, `client/src/sync/guest_health.{h,cpp}`, `client/src/sync/session.cpp`, CMake -- file `HitOnGuest`, libellés, application chez l’invité -- AD-9, AD-11.
- [x] `tools/fake-guest/` -- v13, réception journalisée, tests -- banc.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh`, `client/tests/wine/run_scenarios.sh` -- `--hit-guest-ms`, scénario, symboles -- bout en bout.
- [x] Spine (AD-9, AD-11), `epics.md`, `client/README.md`, `README.md`, `epic-3-context.md`, `deferred-work.md` -- décisions consignées.

**Acceptance Criteria:**
- Given les tests Linux, Go et le banc, when on construit, then tout passe, `regen.sh --check` réussit, et `hit_on_guest.bin` concorde en C++ et en Go.
- Given le banc (`session_host --npcs 1 --hit-guest-ms 500`) et le faux invité, when l’hôte injecte des coups sur l’avatar, then le faux invité journalise un `<- HitOnGuest source=1000 dégâts=18` par coup injecté, et l’hôte journalise `HitOnGuest envoyé`.
- Given la spine, when on lit AD-11, then `sync/guest_health` y est nommé comme seul écrivain des PV du héros invité, avec le maintien à 1 PV jusqu’à la 3.7.
- Given Risen en hôte et le faux invité près d’un monstre hostile, when le monstre frappe l’avatar, then l’avatar ne perd aucun PV et ne meurt pas, le log de l’hôte contient `coup natif bloqué : PNJ N -> avatar invité (HitOnGuest, dégâts bruts D)`, et le faux invité reçoit un `HitOnGuest` par coup. *(Manuel.)*
- Given deux machines, when une réplique attaque le héros invité, then le coup natif est bloqué chez l’invité, les PV de l’invité baissent seulement par `HitOnGuest appliqué`, et un coup mortel le laisse à 1 PV, avec le Warn 3.7. *(Manuel.)*

## Spec Change Log

## Review Triage Log

### 2026-10-23 — Review pass
- verdicts: 33 findings — high 0, medium 7, low 20, false 5, maybe-false 1
- findings:
  - `[medium]` `[defer]` (verif-gap) Couture DLL (détour → file → drain, guest_health::apply_hit) sans test automatique — entrée `deferred` (medium) ; déjà notée dans deferred-work.md ; étapes 2, 6, 7 du test en jeu.
  - `[medium]` `[patch]` (verif-gap) Agresseur illisible : le coup passe avant les règles 3b/4b (héros invité et avatar non protégés) — hits.cpp : agresseur tenu pour nul, décision quand même ; les raisons GuestHeroByReplica/GuestHeroOther/AvatarNoAggressor gardent le blocage, le reste laisse passer comme en 3.5 ; commentaire de hits.h mis à jour.
  - `[false]` `[reject]` (verif-gap) on_hit_on_guest ne vérifie pas InSession — on_reliable (core/session.cpp:547) retourne déjà hors InSession.
  - `[medium]` `[defer]` (intent) AC1/AC4 : la chaîne réelle monstre → détour → HitOnGuest n'est exercée que par morceaux — même entrée `deferred` que la couture DLL.
  - `[medium]` `[defer]` (intent) AC2/AC3 : guest_health et l'aiguillage ApplyHitOnGuest sans test — même entrée `deferred`.
  - `[medium]` `[patch]` (intent) Repli « agresseur illisible » contraire à l'invulnérabilité — même correctif que la ligne verif-gap.
  - `[false]` `[reject]` (intent) Lecture A2 (tout coup sur le héros bloqué) plus large que « une réplique » — conforme au texte de l'AC (« les dégâts ne viennent que de HitOnGuest ») et consigné en D2.
  - `[false]` `[reject]` (intent) AD-11 amendé (maintien à 1 PV au lieu de « à terre ») — demandé par l'intention (« n'implémente pas la mort de l'invité ici »).
  - `[low]` `[reject]` (intent) Pas d'affichage des PV de l'avatar chez l'hôte — la clause de l'AC est une interdiction (ne jamais détecter un coup par ces PV), respectée ; l'affichage est une fonction de plus (D8), rarement visible.
  - `[maybe-false]` `[defer]` (intent) Dégâts bruts C1 contre dégâts réels C2 — entrée `deferred` (medium, non vérifié), hypothèse H2.
  - `[low]` `[patch]` (blind) epic-3-context.md : « à valider en jeu » retiré de 3.4b, 3.5, 3.6 — rétabli ; la dernière ligne précise que seule la mêlée du spike J2 a été testée en jeu.
  - `[low]` `[patch]` (blind) tools/fake-guest/README.md resté en v12 — passé en v13, ligne `<- HitOnGuest` et bilan documentés.
  - `[low]` `[patch]` (blind) PROTOCOL.md annonce des bornes que les codecs ne vérifient pas — bornes présentées comme indicatives, non vérifiées au décodage ; regen --check 31/31.
  - `[low]` `[reject]` (blind) HitOnGuest à 0 dégât envoyé — conforme à « un HitOnGuest par coup » ; un 0 est justement le diagnostic de H1/H2.
  - `[low]` `[reject]` (blind) Warn répété à chaque coup tant que le héros est à 1 PV — cadence ~1/s, ligne voulue par l'étape 7 ; correctif = état de plus.
  - `[low]` `[reject]` (blind) GetDamageAmount résolu deux fois (sonde et bloc dégâts) — blocs indépendants par conception (tout ou rien du bloc dégâts).
  - `[low]` `[reject]` (blind) Compteur « bloqués » commun 3.5/3.6 — les lignes Info par coup bloqué tranchent H4 ; correctif = compteur de plus.
  - `[low]` `[reject]` (blind) Spec ≠ code (port base+21, ligne « dégâts bruts illisibles ») — le correctif est d'éditer la spec ; écarts consignés dans le résultat.
  - `[low]` `[reject]` (blind) Trous de tests (hors session, échec d'encodage, ordre de drain) — hors session déjà gardé par on_reliable ; échec d'encodage impossible pour 9 octets (même patron que 3.5) ; ajout de tests seulement.
  - `[low]` `[patch]` (blind) Entrée 3.1 « l'avatar peut mourir chez l'hôte » retirée avant confirmation — rétablie, marquée « résolue par la règle 4b, à confirmer en jeu ».
  - `[low]` `[reject]` (blind) Source non vérifiée chez l'invité, pas de cadence — coop de confiance (D9 de la 3.5) ; la source figure déjà dans la ligne Info.
  - `[false]` `[reject]` (blind) L'hôte factice compte un HitOnGuest même si l'envoi échoue — simHost.sendReliable panique sur erreur (fakehost_test.go:230) : aucun comptage après un échec.
  - `[low]` `[patch]` (blind) Phrase AD-2/AD-12 ambiguë sur la limite de 64 — phrase scindée : chaque file a sa propre limite de 64.
  - `[medium]` `[patch]` (edge) Agresseur illisible avant 3b/4b — même correctif.
  - `[low]` `[reject]` (edge) Exception d'allocation après la décision → coup laissé — improbable ; correctif = gestion d'exception de plus.
  - `[low]` `[reject]` (edge) File de 64 pleine sans compteur — plus de 64 coups sur l'avatar en une frame est irréaliste (même rejet qu'en 3.5).
  - `[low]` `[reject]` (edge) HitOnGuest à 0 envoyé — même motif que la ligne blind.
  - `[low]` `[reject]` (edge) (Amount + Bonus) < 0 et Multiplier < 0 → dégâts positifs — données de jeu irréalistes ; correctif = garde de plus.
  - `[false]` `[reject]` (edge) Héros nul en session chez l'invité → 3b contournée — en session le monde est chargé et publish publie world.player à chaque frame ; une fin de monde termine la session.
  - `[low]` `[patch]` (edge) Bornes du protocole non vérifiées — même correctif documentaire que la ligne blind.
  - `[low]` `[reject]` (edge) `--hit-guest-ms` non numérique dans le banc — outil de test, échec visible.
  - `[medium]` `[patch]` (edge, claim) « Tout coup sur l'avatar est bloqué » faux si l'agresseur est illisible — même correctif.
  - `[low]` `[reject]` (edge, claim) « Un HitOnGuest par coup » faux en rafale (> 64) ou si les getters échouent — Warn une fois par session dans le second cas ; le premier est irréaliste.

## Design Notes

**Décisions prises sans l’utilisateur (option la plus prudente, à relire) :**
- D1 — **Dégâts bruts = `(Amount + Bonus) × Multiplier` du PS de l’arme**, calculés avant toute protection, comme le veulent AD-9 et la revue adversariale B4. **Pas d’armure en J2** : l’invité retire ces dégâts tels quels. Ils peuvent s’écarter des dégâts natifs, qui tiennent compte de la force et de la protection. C’est l’hypothèse H2, à mesurer en jeu.
- D2 — **Chez l’invité, tout coup natif sur le héros est bloqué**, pas seulement ceux des répliques. C’est la lecture stricte de « les dégâts ne viennent que de `HitOnGuest` ». C’est aussi ce qui garantit qu’aucun coup natif ne tue le héros invité avant la 3.7. Perte assumée : un agresseur que l’hôte ne voit pas (PNJ non apparié) ne blesse plus l’invité. Les chutes et la noyade ne passent pas par `DoDamage` (Q9, 3.7).
- D3 — **Pas de tir ami** : le héros de l’hôte ne blesse pas l’avatar, comme l’avatar ne blesse pas le héros de l’hôte (D5 de la 3.5).
- D4 — **Source inconnue = NetId 0.** Un PNJ non animé (hors registre) ou un projectile blesse quand même l’invité. Le NetId ne sert qu’au log chez l’invité.
- D5 — **En attendant la 3.7, le héros invité ne descend jamais sous 1 PV.** Un coup mortel le maintient à 1 PV avec un Warn explicite. Pas de `Kill`, pas de `Death{2}`. La 3.7 branchera la mise à terre sur `held`.
- D6 — **Application par écriture directe** (`SetHitPoints` fixé) plutôt que `PSDamage::DoDamage` sur le héros. Cet appel pourrait mener au `Kill` natif (`DownOrDeadBy*` → menu de chargement). Perte assumée : pas de réaction de recul chez l’invité ; l’animation vient de l’hôte par l’avatar.
- D7 — **Garde de réentrance inchangée** : un coup imbriqué sur l’avatar pendant l’application d’un `HitRequest` passerait. Aucun cas connu ; c’est l’hypothèse H5.
- D8 — **Affichage des PV de l’avatar chez l’hôte non ajouté** : hors critère. Les PV de l’avatar ne sont ni lus ni écrits par cette story.

**Hypothèses à confirmer en jeu :** H1, le PS du détour d’un monstre porte bien des getters de dégâts lisibles. H2, l’ordre de grandeur des dégâts bruts face aux dégâts natifs. H3, l’agresseur d’un monstre qui frappe l’avatar est l’entité du PNJ capturé (NetId ≠ 0). H4, les répliques chez l’invité déclenchent ou non des coups natifs. H5, voir D7. H6, l’écriture `SetHitPoints` sur le héros invité met bien à jour sa jauge.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: tout passe, `session_fake_guest` et le nouveau scénario compris.
- `cd tools/fake-guest && go test ./... && go build` -- expected: tout passe.
- `protocol/vectors/regen.sh --check` -- expected: tous `[ OK ]`, `hit_on_guest.bin` compris.

## Test en jeu à faire

Préparation :
- recompiler, puis copier `client/build/Script_Multiplayer.dll` dans `bin/scripts/` ;
- `log_level = debug`, `damage_hook = 1`, overlay Steam désactivé ;
- `cd tools/fake-guest && go build`.

Log : `bin/scripts/Script_Multiplayer.log`.

1. **Installation.** Attendu : `[hooks] point d’accroche des dégâts posé (gCDamage_PS::DoDamage)`. Si on voit `dégâts inactifs` avec un des trois getters `GetDamage*` : rapporter la ligne.
2. **Une machine, hôte + faux invité** : `fake-guest --host 127.0.0.1 --duration 180s --echo`. Attirer un monstre sur l’avatar (se placer pour que l’avatar soit près d’un camp hostile).
   - Attendu chez l’hôte : `coup natif bloqué : PNJ N -> avatar invité (HitOnGuest, dégâts bruts D)`, puis `coup sur l’avatar invité (source PNJ N) : HitOnGuest envoyé, dégâts bruts D`.
   - L’avatar ne meurt jamais et ne tombe pas.
   - Chez le faux invité : `<- HitOnGuest source=N dégâts=D`, une ligne par coup.
   - Noter D (H2). Une source `inconnue` contredit H3 : rapporter la ligne Debug `dégâts : agresseur …`.
3. **Une machine, comparaison H2** : se faire frapper par le même type de monstre avec le héros de l’hôte, et noter la perte de PV native sur la jauge. Comparer avec D.
4. **Une machine, tir ami** : frapper l’avatar avec le héros de l’hôte. Attendu : `(pas de tir ami)`, aucun `HitOnGuest`.
5. **Une machine, monstre sur le héros de l’hôte** : dégâts natifs normaux, aucune ligne `bloqué`.
6. **Deux machines** : une réplique attaque le héros invité.
   - Chez l’invité, s’il y a un coup natif : `coup natif bloqué : PNJ N -> héros (seuls les HitOnGuest blessent)`. Son absence tranche H4.
   - Chez l’invité, à chaque coup : `HitOnGuest (source PNJ N) appliqué : dégâts bruts D, PV a -> b`, et la jauge du héros baisse (H6).
   - Chez l’hôte : `HitOnGuest envoyé`.
   - Bilan : `coups subis : HitOnGuest envoyés X` côté hôte, `reçus X` côté invité.
7. **Deux machines, coup mortel** : laisser les monstres frapper l’invité jusqu’au bout. Attendu : `HitOnGuest mortel (…) : héros maintenu à 1 PV (à terre : story 3.7)`. Le héros reste debout à 1 PV, sans menu de chargement ni fin de session.

## Auto Run Result

Status: done

**Résumé :**
- Le détour de la 3.5 (`gCDamage_PS::DoDamage`) reçoit deux nouvelles règles, sans nouveau hook.
  - Règle 3b, chez l'invité : tout coup natif sur son héros est bloqué.
  - Règle 4b, chez l'hôte : tout coup natif sur l'avatar invité est bloqué, ce qui le rend invulnérable. Un coup de PNJ devient `HitOnGuest{source, dégâts bruts}`. Il n'y a pas de tir ami.
- Chez l'invité, `sync/guest_health` est le seul écrivain des PV du héros (AD-11). Un coup mortel le maintient à 1 PV, avec un Warn « à terre : story 3.7 ».
- Le protocole passe en **v13** : `HitOnGuest` = type 23, 9 octets.

**Fichiers :**
- Protocole : `protocol/PROTOCOL.md`, `protocol/vectors/` (`hit_on_guest.bin`, `hello*.bin` en v13, README), `client/src/core/protocol.{h,cpp}`.
- Session : `client/src/core/session.{h,cpp}` (`LocalHitOnGuest`, `ApplyHitOnGuest`, compteurs « coups subis »).
- Règles pures : `client/src/sync/hit_rules.{h,cpp}` (3b, 4b, `raw_damage`, `guest_hp_after`).
- Détour : `client/src/sync/hits.{h,cpp}` (file `{source, dégâts}`, repli si l'agresseur est illisible).
- PV du héros invité : `client/src/sync/guest_health.{h,cpp}` (nouveau).
- Branchement : `client/src/sync/session.cpp`, `client/CMakeLists.txt`.
- Accès au jeu : `client/src/game/combat.{h,cpp}` (3 getters `GetDamage*`, `damage_raw`).
- Tests : `test_hit_rules.cpp`, `test_session.cpp`, `test_protocol.cpp`, `session_host_main.cpp` (`--hit-guest-ms`), `session_fake_guest.sh` (scénario au port base+21), `wine/run_scenarios.sh`.
- `tools/fake-guest/` : v13, réception journalisée, `TestHitOnGuest`, README.
- Documentation : spine (AD-2, AD-9, AD-11), `epics.md`, `client/README.md`, `README.md`, `epic-3-context.md`, `deferred-work.md`.

**Revue :** 33 constats (high 0, medium 7, low 20, false 5, maybe-false 1).
- **Correctifs.**
  - Un groupe medium : un agresseur illisible laissait passer un coup sur le héros invité ou sur l'avatar. Ces coups restent maintenant bloqués.
  - Six low, tous dans la documentation :
    - `epic-3-context` ;
    - le README du faux invité ;
    - les bornes de `PROTOCOL.md` ;
    - l'entrée 3.1 de `deferred-work.md` ;
    - la phrase de la spine sur les files.
- **Reports** : 2 entrées `deferred`, la couture DLL sans test et l'hypothèse H2 sur les dégâts bruts.
- **Rejets** : motivés dans le journal (5 false, 13 low).

**Écarts avec la spec** (spec non modifiée) :
- le scénario du banc est au port `base_port + 21`, car le port +20 est pris par la 3.5 ;
- le banc tolère la perte d'un `HitOnGuest` au départ du faux invité ;
- le détour ajoute la ligne `(dégâts bruts illisibles, sans HitOnGuest)`.

**Revue de suivi recommandée :** non. Correctifs : high 0, medium 1, low 6, donc sous le seuil. Le corps modifié du détour (`hits.cpp`, repli agresseur illisible) n'a toujours pas de test automatique. Seul le test en jeu le vérifiera.

**Vérification :**
- `cmake --build client/build` et `cmake --build client/build-tests` : sans avertissement.
- `ctest` : 6/6, dont `client_tests` 366/366, `session_fake_guest`, `wine_scenarios` et `wine_net`.
- `go test ./...` (79 tests) et `go build` : OK.
- `regen.sh --check` : 31/31.

**Risques résiduels :**
- Aucun test en jeu.
- Hypothèses H1 à H6 non vérifiées.
- La garde de réentrance laisse passer un coup imbriqué sur l'avatar (D7).
- Un `HitOnGuest` arrivé à la toute première frame après un chargement est ignoré, avec un Warn « pas de héros ».
