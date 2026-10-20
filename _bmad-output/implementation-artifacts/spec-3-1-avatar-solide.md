---
title: 'Story 3.1 — Avatar solide et registre des entités'
type: 'feature'
created: '2026-10-20'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'bf0fa72833e841a5b85d93ed54ff7e9b6c5011cc'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/_bmad-output/planning-artifacts/architecture/architecture-risen-multiplayer-2026-10-15/ARCHITECTURE-SPINE.md'
  - '{project-root}/docs/re/spike-j2.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Dans une session, l’avatar de l’autre joueur se laisse traverser, car `Entity::MoveTo` le téléporte à chaque frame. Ce n’est pas un vrai personnage du monde. Par ailleurs, les détours lisent chacun un filtre de pointeurs « réplique », sans notion de rôle. Or les stories suivantes de l’Epic 3 doivent partager un registre unique, comme le demande AD-12.

**Approach:**
- Appliquer à l’avatar de session la recette du mode 2 du spike (AD-8) :
  - réglages posés une fois à la création ;
  - placement à chaque frame par `SetPose` puis `PSRigidBody::SetPose`, au lieu de `MoveTo`.
- Transformer `sync::AnimFilter` en registre d’entités `{entité, PS d’animation, PS de routine, enveloppe, rôle}`, sans changer le comportement des détours existants.

## Boundaries & Constraints

**Always :**
- AD-2 / AD-12 :
  - Le registre reste le seul état partagé entre les hooks et la frame, sous un seul verrou.
  - Il n’est alimenté que par le module qui possède l’entité : `sync/avatar` en session, la sonde en mode sonde.
  - Une entité en sort **avant** d’être oubliée ou détruite.
- AD-3 :
  - Les enveloppes de `game/combat` utilisées par la session sont résolues à l’installation des hooks de session, en tout ou rien.
  - Un symbole manquant laisse le mod inactif, avec sa cause au log.
- Le comportement de J1 ne change pas, sauf le placement de l’avatar : rejeu `Motion` et `MotionParam`, blocage du moteur sur l’avatar, `Death` en ragdoll, `Despawn`.
- Le héros local reste placé par `MoveTo` lors de la téléportation de l’invité à l’entrée en session.

**Never :**
- Pas de hook nouveau en session : le hook de routine et les `DownOrDeadBy*` arrivent avec les stories 3.3 et 3.7.
- Pas de changement de protocole.
- Pas de `MoveTo` à chaque frame sur l’avatar, ni de `SetGoalPose`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Création | `Spawn` de l’avatar | Après `Entity::Spawn` : `SetCollisionGroup(4)`, `EnableCollision(true)`, DCC actif, `AddBodyFlag(0x81)`, `EnableGravitation(false)`. Une ligne de log donne le résultat de chaque réglage | Réglage en échec (SEH) : log, l’avatar est gardé |
| Placement | Snapshot interpolé | `Entity::SetPose` puis `PSRigidBody::SetPose` à chaque frame, sans `StopMovement` | Première SEH : log une fois, l’avatar n’est plus déplacé (comme aujourd’hui) |
| Mort | `Death` de l’avatar | `RemoveBodyFlag(0x81)` et gravité rendue **avant** le ragdoll, pour que le corps tombe | SEH : log, le ragdoll est tenté quand même |
| Registre | Création, retrait, fin de session | L’avatar y entre avec le rôle « avatar », son PS d’animation, son PS de routine (0x24) et son enveloppe ; il en sort avant `Kill` ou l’oubli | N/A |
| Détours | Appels du moteur | Mêmes verdicts qu’avant pour `PlayMotion`, `SetRagDollEnabled` et les réglages de locomotion (héros local capturé, avatar bloqué hors rejeu) | N/A |

</frozen-after-approval>

## Code Map

- `client/src/sync/anim_filter.{h,cpp}` : aujourd’hui `set_local`, `add_replica`/`remove_replica`/`clear_replicas` sur un `std::vector<void*>`, où les enveloppes EMotionFX sont mélangées aux PS. À remplacer par un registre :
  - `track(TrackedEntity)` ;
  - `untrack(entité)` ;
  - recherches par PS d’animation et par enveloppe, avec le rôle.

  `classify`, `classify_ragdoll` et `is_local` gardent leur sémantique. Testé sous Linux : `client/tests/test_anim_filter.cpp` (à adapter, avec de nouveaux cas pour le rôle et `untrack`).
- `client/src/sync/avatar.cpp` :
  - `spawn()`, l. 124 : appels à `add_replica` sur le PS et l’enveloppe, à remplacer par `track`, puis appliquer les réglages de la recette.
  - `despawn()`, l. 168 : `untrack` avant `Kill`.
  - `move()`, l. 195 : `move_to`, à remplacer par `set_pose` + `set_body_pose`.
  - `die()`, l. 275 : rendre corps et gravité avant le ragdoll.
- `client/src/sync/session.cpp`, l. 232 : `filter.set_local(ps)` reste, c’est le héros local.
- `client/src/sync/probe.cpp`, l. 437, 463 et 534 : la copie de la sonde passe par `track`/`untrack` avec le rôle « avatar ».
- `client/src/game/combat.{h,cpp}` : enveloppes existantes à réutiliser telles quelles :
  - `set_collision_group`, `enable_collision`, `set_dcc_enabled`, `add_body_flag`, `remove_body_flag`, `enable_gravitation`, `set_body_pose` ;
  - `kRoutinePropertySet`, `kBodyFlagsKinematic`, `kCollisionGroupNpc` ;
  - `game::get_property_set` (entity.h) pour le PS 0x24.
- `client/src/win/hooks.cpp` : `install_session()` (vers la l. 219) ne résout aujourd’hui que `resolve_entities`. Il doit aussi appeler `game::resolve_combat`, en tout ou rien.
- Recette et observations : `docs/re/spike-j2.md` (mode 2 de Num2), spine AD-8 (Placement) et AD-12.

## Tasks & Acceptance

**Execution :**
- [x] `client/src/sync/anim_filter.{h,cpp}` : registre `TrackedEntity{entity, animation_ps, routine_ps, wrapper, role}` avec les rôles héros local, avatar et réplique de PNJ ; `track`, `untrack`, `clear` ; recherches par PS et par enveloppe ; verdicts inchangés. Motif : AD-12.
- [x] `client/tests/test_anim_filter.cpp` : tests adaptés, plus rôle, `untrack` d’une entité qui retire PS et enveloppe, et pas de doublon. Motif : non-régression.
- [x] `client/src/sync/avatar.cpp` : recette de création, placement cinématique, mort qui rend corps et gravité, `track`/`untrack`. Motif : matrice.
- [x] `client/src/sync/probe.cpp` : la copie passe par `track`/`untrack`. Motif : un seul registre.
- [x] `client/src/win/hooks.cpp` : `resolve_combat` dans `install_session`, en tout ou rien. Motif : AD-3.
- [x] `client/tests/wine/run_scenarios.sh` : la session s’installe toujours (assertions existantes au vert). Motif : non-régression.
- [x] `client/README.md` : section « avatar solide » avec le protocole de test en jeu (critères ci-dessous). Motif : utilisabilité.

**Acceptance Criteria :**
- Given les tests Linux et le banc Wine, when on construit, then tout est au vert et les verdicts du filtre sont inchangés pour le héros local et l’avatar.
- Given une session en jeu (Risen hôte + faux invité, `--echo` pour un avatar qui bouge), when mon héros marche dans l’avatar, then il est bloqué en marche, en course et pendant un saut de l’avatar. *(Manuel.)*
- Given l’avatar en mode cinématique, when il saute ou court, then son équipement le suit. *(Manuel, vérifié en premier ; sinon la story s’arrête et le constat remonte dans la spine.)*
- Given un monstre hostile près de l’avatar, chez l’hôte, when l’avatar est à sa portée, then le monstre le cible et l’attaque. *(Manuel.)*
- Given un `Death` de l’avatar, when il est appliqué, then le corps tombe en ragdoll au lieu de rester figé en l’air. *(Manuel.)*

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge, gap | `move()` appelle `PSRigidBody::SetPose` sans vérifier le corps ; une SEH fige l’avatar pour la session | medium | patch | `Entry::kinematic` posé par `make_solid` ; sans corps ou après une SEH du corps, repli sur `MoveTo` (J1) au lieu de figer. |
| 2 | blind, edge | Toute la table du combat requise en session (37 symboles, dont ceux de la sonde) | medium | patch | Écart à la spec (« enveloppes utilisées par la session ») ; `resolve_combat_session` ne résout que les 7 de l’avatar solide, enveloppes gardées contre les pointeurs nuls. |
| 3 | blind | Réglage sauté (PS absent) journalisé comme succès | low | patch | Passe en `warn`, avec le repli `MoveTo` nommé. |
| 4 | blind, edge | Ordre des lignes du README inversé (« réglages » avant « créé ») | low | patch | `make_solid` après la ligne « créé ». |
| 5 | blind | Rôle `LocalHero` en contradiction avec `set_local` | low | patch | En-tête : le rôle décrit une entrée, `set_local` reste la seule capture. |
| 6 | blind | Groupe 4 présenté comme acquis | low | patch | README : valeur à confirmer (PNJ natifs lus à 0 au spike). |
| 7 | blind | L’hôte peut tuer l’avatar (PNJ) : mort figé debout, ragdoll bloqué | medium | defer | Déjà possible en J1 (copie tuée au spike) ; l’entité n’est pas détruite par `Kill`. L’invulnérabilité relève de la story 3.6. |
| 8 | gap | Placement cinématique, `release_body`, `make_solid` vérifiés seulement en jeu | medium | defer | Appels ABI sans banc qui appelle `OnProcess` ; il faudrait une couture `game::` simulée. |
| 9 | edge | `die()` sans PS d’animation : l’avatar reste solide et placé | low | rejet | Cas déjà dégradé en J1 (avatar sans PS non animé) ; jamais vu. |
| 10 | edge, blind | Échec du ragdoll : `make_solid` réapplique tout et relogue | low | rejet | Chemin d’erreur rare ; le résultat (avatar solide) est le bon. |
| 11 | edge | `track` d’une même entité par deux propriétaires | low | rejet | Sonde et session exclusives (AD-12) : un seul propriétaire par mode. |
| 12 | blind, gap | Assertion Wine « ABI du combat résolue » redondante ; dépendance à l’ordre `resolve_entities` → `LoadLibrary(Script.dll)` | low | rejet | Redondance sans risque ; l’ordre est fixé dans `install_session`. |
| 13 | blind | API `find_by_*` et `routine_ps` sans appelant | false | rejet | Voulu par AD-12 pour la story 3.3 (hook de routine). |
| 14 | blind | Copie de la sonde sans enveloppe EMotionFX | low | rejet | Comportement de la sonde inchangé (ne la bloquait pas avant). |
| 15 | blind | Trous de tests unitaires du registre (réplique de PNJ sous garde, ragdoll sur enveloppe, partage de PS, concurrence) | low | rejet | Cas couverts par la même logique `blocks_locked` déjà testée ; à étendre en 3.3 avec les répliques. |

## Implementation Notes

- Placement : corps cinématique si `AddBodyFlag(0x81)` a réussi, sinon repli `MoveTo` (J1), y compris après une SEH de `PSRigidBody::SetPose`.
- Session : `resolve_combat_session` (7 symboles) ; la sonde garde `resolve_combat` (table complète).
- Les critères manuels (équipement qui suit, héros bloqué, monstre qui cible, ragdoll qui tombe) restent à vérifier en jeu par l’utilisateur.

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : `client_tests`, `wine_scenarios` et `session_fake_guest` passent.

**Manual checks :**
- En jeu, `probe = 0`, Risen hôte et faux invité en `--echo` : suivre la section du README et rapporter au log les lignes `[sync] avatar … réglages` et le comportement observé.
