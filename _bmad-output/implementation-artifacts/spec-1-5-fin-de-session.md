---
title: 'Story 1.5 — Fin de session propre'
type: 'feature'
created: '2026-10-17'
status: 'done'
baseline_commit: '7025003b23751868d477d71ae8d5a713210cb1ac'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-1-context.md'
  - '{project-root}/protocol/PROTOCOL.md'
  - '{project-root}/_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** En story 1.4, `Ended` est final jusqu'au redémarrage du jeu. Un rechargement ou un retour au menu ne termine pas la session, et la mort de l'hôte n'est pas détectée.

**Approach:** On détecte la fin de monde et la mort du héros de l'hôte à partir de signaux lus chaque frame. Toute fin passe par la même transition `Ended → Inactive`, journalisée avec sa raison, puis la session est réarmée. Les signaux sont confirmés en jeu par un journal de diagnostic, et le résultat est consigné dans `docs/re/`.

## Boundaries & Constraints

**Always:**
- **Signaux lus chaque frame par le pilote** (thread principal, sous SEH, via `game/`) :
  - `player` : pointeur de `Entity::GetPlayer` ;
  - `running` : `gCSession::IsGameRunning`. C'est l'octet à `+0x110` de `gCSession::GetInstance()` (fonction statique qui retourne le pointeur global, éventuellement nul). Une instance nulle vaut `running = false` ;
  - `dead` : `Entity::IsDead` du joueur (Script.dll : le PS 0x24 à l'offset `+0xC0` vaut 12).
- **Monde chargé** = `player ≠ null` et `running`.
- **Fin de monde** = dans `Hosting`, `Connecting` ou `InSession`, le monde n'est plus chargé, ou `player` diffère du pointeur mémorisé à l'armement.
- **Mort de l'hôte** = rôle hôte, `InSession` ou `Hosting`, et `dead`.
- **Une seule transition de fin** pour toutes les causes (fin de monde, mort de l'hôte, timeout, `SessionEnd` reçu, `Reject`, injoignable, erreur) :
  1. si `InSession` et que la cause est locale, `SendReliable(SessionEnd{raison})` : 2 = fin de monde, 3 = mort de l'hôte ;
  2. `Ended`, avec la raison au log ;
  3. maintien du KCP pendant **1 s** pour laisser partir le `SessionEnd` (durée rendue normative au §8), puis `CloseReliable` et `Stop` ;
  4. `Ended → Inactive`, journalisé.
- **Réarmement** depuis `Inactive`, dès que le monde est chargé, avec un nouveau `conv` à chaque session. Après une fin de monde, c'est forcément un nouveau monde.
  - **Décision (2026-10-17, option recommandée retenue) :** quand l'invité part dans le même monde (timeout ou `SessionEnd` reçu), l'hôte se réarme aussitôt : il repasse en `Hosting` sans attendre de recharger.
  - L'invité, lui, attend le monde suivant après un `Reject`, « injoignable », un timeout ou un `SessionEnd` reçu, pour ne pas boucler sur des `Reject`.
  - **Décision (2026-10-17, option B) :** pas de reconnexion automatique de l'invité après un rechargement ou la mort de l'hôte. L'invité se reconnecte à son propre prochain chargement.
- **Quitter le jeu :** rien n'est envoyé à la fin du processus (pas de travail réseau dans `DllMain`). Le pair conclut au timeout de 5 s.
- **Diagnostic :** avec `log_level = debug`, une ligne `[sync] signaux` à chaque changement de `player`, `running` ou `dead` (jamais à chaque frame). Elle sert au test en jeu qui fixe la règle.

**Never:**
- Pas de mort ni de réapparition de l'invité (J2), pas d'avatar (Epic 2).
- Pas de nouvelle version du protocole : `SessionEnd` existe déjà.
- Pas d'envoi réseau ni de `join` dans `DllMain`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu (core/session) |
|---|---|---|
| Rechargement en session | `InSession`, `player` change | `SessionEnd{2}`, `Ended` « fin de monde », maintien 1 s, `Stop`, `Inactive`, puis réarmement au monde suivant |
| Retour au menu | `InSession`, `player` nul ou `running` faux | idem ; pas de réarmement tant que le monde n'est pas chargé |
| Fin de monde hors session | `Hosting` ou `Connecting` | `Ended` « fin de monde » sans envoi, `Stop`, `Inactive`, réarmement au monde suivant |
| Mort de l'hôte | hôte `InSession`, `dead` | `SessionEnd{3}`, `Ended` « mort de l'hôte », `Inactive` |
| Mort de l'hôte sans invité | hôte `Hosting`, `dead` | `Ended` sans envoi, `Inactive`, réarmement au monde suivant (rechargement par le jeu) |
| `SessionEnd` reçu | `InSession`, `SessionEnd{r}` | `Ended` avec r, pas d'envoi, `Inactive` |
| Pair parti sans prévenir | 5 s sans événement | `Ended` « timeout », `Inactive` |
| Trois rechargements | hôte + faux invité qui se reconnecte | 3 `conv` distincts, 3 couples `InSession`/`Ended`, aucun crash |
| `Reject` reçu (invité) | `Connecting`, `Reject{r}` | `Ended`, `Inactive`, pas de nouvel essai dans le même monde |

</frozen-after-approval>

## Code Map

- `client/src/core/session.{h,cpp}` -- étendre `World` (`player` en `uintptr_t` opaque, `running`, `dead`), la transition de fin unique, le maintien (`linger_until_`), `Ended → Inactive`, le réarmement, le `conv` neuf. Garder la pureté.
- `client/tests/test_session.cpp` -- la matrice I/O.
- `client/src/sync/session.cpp` -- `read_world()` : lire les 3 signaux et journaliser leurs changements en debug. Supprimer le retour anticipé « Ended final ».
- `client/src/game/entity.{h,cpp}` -- ajouter `is_dead(Entity, bool&)` (`?IsDead@Entity@@QEBA_NXZ`, Script.dll) et `game_running(bool&)` (`?GetInstance@gCSession@@SAAEAV1@XZ` + `?IsGameRunning@gCSession@@QEBA_NXZ`, Game.dll), sous SEH, résolus en bloc dans `resolve_entities`.
- `client/tests/session_host_main.cpp` + `session_fake_guest.sh` -- l'hôte simulé peut changer de monde sur signal (fichier témoin ou `SIGUSR1`). Scénarios : trois cycles « rejoindre, fin de monde, réarmer » avec `SessionEnd{2}` reçu par le faux invité, et réarmement après `--quit`.
- `protocol/PROTOCOL.md` §8 -- maintien de 1 s après `SessionEnd` ; réarmement.
- `docs/re/fin-de-monde.md` -- nouveau : signaux, règle retenue, résultats du test en jeu (à compléter après ce test).
- `_bmad-output/implementation-artifacts/deferred-work.md` -- report de la 1.4 (maintien du KCP) traité ici.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/game/entity.*` -- `is_dead`, `game_running` -- nouveaux signaux.
- [x] `client/src/core/session.*` + `client/tests/test_session.cpp` -- fin unique, maintien, réarmement, matrice -- cœur.
- [x] `client/src/sync/session.cpp` -- lecture des signaux, diagnostic debug -- pilote.
- [x] `client/tests/session_host_main.cpp`, `client/tests/session_fake_guest.sh` -- cycles de fin de monde et réarmement contre le faux invité -- preuve sans le jeu.
- [x] `protocol/PROTOCOL.md`, `docs/re/fin-de-monde.md`, `client/README.md` -- normes, journal de RE, procédure du test en jeu.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then les 6 tests passent.
- Given le build Windows, when on construit la DLL, then elle compile sans avertissement (imports inchangés).
- Given Risen en hôte (`log_level = debug`) et le faux invité qui se reconnecte, when on recharge rapidement 3 fois puis revient au menu, then le log montre 3 sessions distinctes, chacune terminée par « fin de monde » avec `SessionEnd{2}` reçu par le faux invité, sans crash. Les lignes `[sync] signaux` sont recopiées dans `docs/re/fin-de-monde.md`. Vérification manuelle par l'utilisateur.
- Given l'hôte en session avec le faux invité, when le héros de l'hôte meurt, then le faux invité reçoit `SessionEnd{3}`. Vérification manuelle.

## Implementation Notes

**Test en jeu (2026-10-17) :** 3 chargements rapides et la mort de l'hôte en session, avec le faux invité. Tous conformes (détail dans `docs/re/fin-de-monde.md`).
- **Fin de monde au chargement rapide :** détectée par le **changement d'adresse du joueur**. `running` reste à 1.
- **`OnProcess` muet pendant un chargement :** 3 à 3,5 s, sous le timeout de 5 s.
- **Retour au menu en cours de session :** **pas encore observé en jeu** (couvert par les tests unitaires).
- **Côté invité de la DLL :** à valider à deux machines.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | blind, edge-case | Un hôte au héros mort peut s'armer : cycle Start → « mort de l'hôte » → Stop | medium | `maybe_arm` ne lisait pas `dead` → patch (`can_arm`) + test |
| 2 | blind, edge-case | Après la mort, si le rechargement garde la même adresse et que `running` ne retombe pas, la session reste Inactive jusqu'au redémarrage | medium | `wait_new_world_` jamais levé → patch (le passage de `dead` à faux vaut nouveau monde) + test |
| 3 | blind | `is_dead` appelé pendant le démontage du monde (`running` faux) | medium | Appel virtuel sur une entité peut-être à moitié détruite → patch (lu seulement si le monde est chargé) |
| 4 | edge-case | Log « réarmement immédiat » sans Hosting derrière | low | Correction directe → patch |
| 5 | blind | Tout `Ended` est journalisé en Warn, même les rechargements normaux | low | Correction directe → patch (Info pour les fins normales) |
| 6 | blind | Tests manquants : `fail()` invité en session, `SessionEnd` et fin de monde dans le même tick, monde déchargé pendant le maintien | medium | Chemins non couverts → patch |
| 7 | blind, edge-case | `session_fake_guest.sh` reste bloqué au lieu d'échouer vite ; fins mal comptées dans `session_host` | low | Scripts de test → patch |
| 8 | blind | README : relance du faux invité oubliée ; `deferred-work.md` périmé ou trop affirmatif | low | Correction directe → patch |
| 9 | blind | L'invité ne se reconnecte pas tout seul après un rechargement de l'hôte | intent_gap | Question posée à l'utilisateur : option B retenue (pas de reconnexion automatique), inscrite dans l'intent et dans PROTOCOL.md |
| 10 | blind | `SessionEnd{4}` envoyé par `fail()` en session, non listé dans l'intent | low | Cause « erreur » déjà prévue par l'intent ; code 4 défini en v1 ; documenté et testé → accepté |
| 11 | verification-gap | Le pilote Windows (`frame`, `read_world`) n'est couvert que par le test en jeu | medium | Il faut le vrai jeu → defer : test en jeu requis avant de clore la story |
| 12 | blind | CTest réduit en silence le nombre de tests si go, wine ou les DLL du jeu manquent | low | Déjà reporté en story 1.2 → rejeté (doublon) |

## Design Notes

Le pointeur du joueur est mémorisé comme un entier opaque, jamais déréférencé hors de `game/`. Cela respecte AD-6, puisqu'aucun pointeur d'entité n'est utilisé au-delà de `Ended`. Le monde peut redonner la même adresse après un rechargement ; c'est pourquoi `running` sert de second signal. Si le test en jeu montre que `running` ne retombe pas pendant un chargement rapide, `docs/re/fin-de-monde.md` le dira, et le signal de remplacement (`eCEntity::GetID`) fera l'objet d'une story suivante.

## Verification

**Commands:**
- `cmake --build client/build` -- expected: sans avertissement
- `cmake -S client -B client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: 6 tests sur 6

**Manual checks:**
- En jeu : 3 rechargements rapides et un retour au menu avec le faux invité ; mort de l'hôte en session.
