---
title: 'Story 2.1b — Mort par ragdoll dans le miroir'
type: 'feature'
created: '2026-10-17'
status: 'done'
baseline_commit: '4d6c290fede9e442d8777096cb75ebae82abb630'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-2-context.md'
  - '{project-root}/docs/re/spike-j1.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Au test de la story 2.1, la copie n'a pas reproduit la mort du héros. Dans Risen, la mort ne passe pas par `PlayMotion` mais par le ragdoll (`eCAnimation_PS::SetRagDollEnabled`). L'avatar ne pourra donc pas montrer la mort de l'autre joueur (FR-7).

**Approach:** La sonde capture aussi les appels à `SetRagDollEnabled` sur le héros local et les rejoue sur la copie en mode miroir, par le même `AnimFilter` et dans l'ordre des `PlayMotion`. On valide ainsi en local le mécanisme que portera plus tard l'événement `Death` (AD-10).

## Boundaries & Constraints

**Always:**
- **Faits RE (désassemblage, 2026-10-17)** :
  - `?SetRagDollEnabled@eCAnimation_PS@@QEAAX_N0PEAVbCVector@@@Z` (Engine.dll, `0x1804d7bd0`) : `RCX=this`, `DL=actif`, `R8B=second bool` (non lu sur le chemin observé), `R9=bCVector*` (nul possible). Le vecteur est transmis à la création du ragdoll (impulsion ?).
  - L'état est rangé dans le bit 2 de `this+0x22C`. `?GetRagDollEnabled@eCAnimation_PS@@QEBA_NXZ` le relit, et désactiver appelle `StopRagDoll`.
- **Hook** de `SetRagDollEnabled`, installé avec ceux de la sonde, en tout ou rien. Il passe par `AnimFilter::classify`, avec les mêmes règles que `PlayMotion` :
  - PS local : capturé, jamais bloqué ;
  - réplique : bloquée, sauf sous le garde de rejeu. **Décision (2026-10-17, option recommandée retenue) :** une désactivation (`actif=0`) lancée par le moteur sur une réplique passe toujours. Le moteur peut nettoyer la copie, jamais la tuer. L'avatar suivra la même règle ;
  - autres PS : laissés passer.
- **Capture** : une `Capture` porte maintenant sa nature, `PlayMotion` ou ragdoll. Pour le ragdoll, elle garde les deux `bool` et le vecteur s'il n'est pas nul : 3 `f32` copiés sous SEH. `bCVector` est supposé faire 12 octets. Les deux natures partagent le même anneau, ce qui préserve l'ordre.
- **Miroir** : le ragdoll capturé est rejoué sur la copie avec les mêmes arguments, sous `ReplayGuard`. **Décision (2026-10-17, après le test en jeu) :** la fin du ragdoll du joueur (`actif=0`) n'est pas rejouée telle quelle. Elle laissait la copie en T-pose, puis debout, parce que chez le joueur c'est `CheckRagdollDeathPose` qui fige la pose du squelette avant de couper le ragdoll. La copie **reste en ragdoll**, ce qui revient à un corps au sol, et sa pose n'est plus imposée une fois qu'elle est morte. Faire exécuter à la copie sa propre `CheckRagdollDeathPose` a été essayé et donnait aussi une T-pose : la pose figée n'est réaffichée que pour un personnage mort au sens du jeu. Tant que la copie est en ragdoll (d'après `GetRagDollEnabled`), la sonde cesse de lui imposer la pose du joueur (`MoveTo`) : c'est la physique qui la pilote.
- **Log** : une ligne par appel capturé (actif, second `bool`, vecteur), le résultat du rejeu sur la copie, et l'état ragdoll de la copie dans la ligne `copie`.

**Never:**
- Pas de message réseau `Death` ni de version du protocole : l'événement réseau vient avec l'avatar.
- Pas de hook de `StopRagDoll` ni de `NeedsDeathAnimation` (on relève seulement ce que montre le log).
- Le hook n'appelle jamais le jeu, hormis les lectures protégées de `game/`.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Attendu |
|---|---|---|
| Ragdoll du héros | PS local, `actif=1` | capture, l'appel original passe |
| Ragdoll moteur sur la copie | réplique, sans garde, `actif=1` | bloqué |
| Nettoyage moteur de la copie | réplique, sans garde, `actif=0` | passe |
| Rejeu sur la copie | réplique, sous garde | passe |
| Ordre | `PlayMotion`, puis ragdoll, puis `PlayMotion` | restitués dans cet ordre |
| Vecteur nul | `R9 = 0` | capture sans vecteur, rejeu avec un vecteur nul |

</frozen-after-approval>

## Code Map

- `client/src/sync/anim_filter.{h,cpp}` -- `Capture` reçoit un `kind` et des champs ragdoll ; `classify` reste inchangé.
- `client/tests/test_anim_filter.cpp` -- ordre mixte et vecteur nul.
- `client/src/game/entity.{h,cpp}` -- résoudre `SetRagDollEnabled` et `GetRagDollEnabled` dans `resolve_entities`. Ajouter `set_ragdoll_address()`, `set_ragdoll(ps, bool, bool, const float* vec3)` (un `bCVector` local est construit si `vec3` n'est pas nul), `get_ragdoll(ps, bool&)` et `SetRagdollFn`, tous sous SEH.
- `client/src/win/hooks.cpp` -- détour `SetRagDollEnabled`, créé dans `install_probe` à côté de `PlayMotion`, qui délègue à `sync::probe::on_set_ragdoll`.
- `client/src/sync/probe.{h,cpp}` :
  - `on_set_ragdoll` côté hook ;
  - dans `flush_captures`, le rejeu selon la nature de la capture ;
  - `mirror_pose` suspendue tant que la copie est en ragdoll ;
  - la ligne `copie` ;
  - `describe` pour le ragdoll ;
  - le classement F4, qui ignore les captures ragdoll.
- `client/README.md` -- procédure du test : mort par chute et au combat.
- `docs/re/spike-j1.md` -- faits RE, puis résultats du test en jeu.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/sync/anim_filter.*`, `client/tests/test_anim_filter.cpp` -- capture ragdoll et matrice.
- [x] `client/src/game/entity.*` -- `set_ragdoll`, `get_ragdoll`, adresse du hook.
- [x] `client/src/win/hooks.cpp`, `client/src/sync/probe.*` -- hook, rejeu, pose suspendue, log.
- [x] `client/README.md`, `docs/re/spike-j1.md` -- procédure et faits RE.

**Acceptance Criteria:**
- Given le build Linux, when `ctest` tourne, then tout passe.
- Given le build Windows, when on construit la DLL, then elle compile sans avertissement, avec les imports inchangés ; le banc Wine (`probe = 1`) installe les trois hooks.
- Given `probe = 1`, F6 puis F7, when le héros meurt par chute, puis au combat, then la copie s'effondre en ragdoll au même moment, et le log montre la capture et le rejeu. Vérification manuelle par l'utilisateur, résultats dans `docs/re/spike-j1.md`.

## Spec Change Log

- **2026-10-17, retour du test en jeu** (utilisateur : « effondrée en ragdoll, puis T-pose, puis relevée »). Rejouer `SetRagDollEnabled(0)` ne fige pas la pose de mort, car c'est `CheckRagdollDeathPose` qui la fige (appel interne `0x1804dfd90`, `this+0x1D0`). Changements :
  - la fin de ragdoll déclenche `CheckRagdollDeathPose` sur la copie ;
  - la pose n'est plus imposée à une copie morte.

  Défaut évité : la copie se relève en T-pose.
- **2026-10-17, second test en jeu** (utilisateur : « encore une T-pose ») : même avec `CheckRagdollDeathPose OK` sur la copie, elle se met en T-pose. `0x1804dfd90` ne fait que recopier les matrices des os dans `PS+0x1D0`, et cette pose n'est réaffichée que pour une entité morte au sens du jeu. Changement : la copie garde son ragdoll ; `CheckRagdollDeathPose` est retirée de l'adaptateur. **KEEP** : l'activation du ragdoll rejouée avec le vecteur (vitesse) tel quel ; la désactivation lancée par le moteur sur une réplique passe toujours.

## Implementation Notes

- Implémenté directement, sans sous-agent. `bCVector` = 12 octets, vérifié sur le constructeur de copie (SharedBase.dll).
- Le banc Wine attend désormais `hooks installés (OnProcess, PlayMotion, SetRagDollEnabled)`.
- Les appels ragdoll répétés au même état ne sont ni rejoués ni journalisés : le moteur sort aussitôt quand l'état ne change pas.
- Une fois, `session_fake_guest` a échoué en même temps qu'une compilation de la DLL. Il passe seul, puis la suite complète passe (6/6) : sensible à la charge machine, sans lien avec ce diff.

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / route |
|---|---|---|---|---|
| 1 | verification-gap | Le routage des captures par nature dans la sonde (rejeu, `g_last`, classement F4, blocage) n'est pas testé automatiquement | medium | `probe.cpp` est réservé à Windows et appelle le jeu ; le test en jeu le couvre → defer (à tester quand l'avatar porte ce routage) |
| 2 | blind, edge-case | La copie reste bloquée en ragdoll si F7 est coupé puis rallumé | low | Correction directe → patch : F7 refuse une copie morte et le dit dans le log |
| 3 | blind, edge-case | Le moteur ne peut plus nettoyer la copie, car `actif=0` est bloqué | medium | Ragdoll orphelin possible au rechargement → intent_gap, tranché par l'utilisateur (option recommandée : la désactivation passe) → intent mis à jour, patch |
| 4 | blind, edge-case | Si le vecteur est une position du monde, le rejouer sur la copie décalée applique la force au mauvais endroit | maybe-false | Sens du vecteur inconnu → procédure du test enrichie (comparer à la position du héros) ; on corrigera après le test si besoin |
| 5 | blind | Le commentaire sur la copie locale du `bCVector` n'est pas vérifié | low | Correction directe → patch : copie `static`, commentaire honnête |
| 6 | blind | Commentaires d'`anim_filter.h` limités à `PlayMotion` | low | → patch |
| 7 | blind | Le test ne vérifie ni le vecteur, ni le `bool`, ni les champs neutres après l'anneau | low | → patch : test complété |
| 8 | blind | Rejeux ragdoll comptés avec les rejeux d'animations | low | Chaque rejeu ragdoll a sa propre ligne de log → rejeté |
| 9 | blind | Une frame de retard entre le héros et la copie | low | Inhérent au vidage par frame ; noté dans le README → patch (doc) |
| 10 | blind, verification-gap, edge-case | Si la lecture du ragdoll échoue, la copie se fige sans log | low | → patch : on impose la pose si la lecture échoue |
| 11 | blind, edge-case | Une mort survenue miroir coupé n'est pas rattrapée | low | Outil de test ; README : F7 avant la mort → patch (doc) |
| 12 | blind | La piste `NeedsDeathAnimation` manque dans la procédure | low | → patch (README) |
| 13 | blind | Statut `in-review` prématuré tant que le test en jeu n'est pas fait | false | Le critère manuel est attendu, comme en 2.1 |
| 14 | edge-case | Symboles ragdoll obligatoires aussi en mode session | false | Une seule version du jeu est prise en charge (empreinte vérifiée) ; le banc Wine résout tout en session comme en sonde (AD-3 : tout ou rien) |
| 15 | edge-case | Mort perdue si l'anneau est plein | low | Débordement compté et journalisé ; plus de 64 appels dans une frame, c'est rare → rejeté |
| 16 | edge-case | Vecteur non nul mais illisible, rejoué avec un vecteur nul | low | `copy_bytes` sur un pointeur du moteur : jamais observé → rejeté |
| 17 | edge-case | Flot de logs et de rejeux si le moteur rappelle `SetRagDollEnabled` à chaque frame | maybe-false | Peu coûteux à éviter → patch : on ne garde que les changements d'état |
| 18 | verification-gap, edge-case | Le suivi du thread est commun aux deux hooks, mais le log ne parle que de `PlayMotion` | low | → patch : libellé « hooks PlayMotion/SetRagDollEnabled » |
| 19 | blind, edge-case | Le compteur de blocages mélange les deux hooks | low | → patch : libellé « appels moteur bloqués/s » |
| 20 | verification-gap | Commentaire de `g_last` périmé | low | → patch |

## Verification

**Commands:**
- `cmake --build client/build-tests && ctest --test-dir client/build-tests --output-on-failure` -- expected: tout passe
- `cmake --build client/build` -- expected: sans avertissement ; `objdump -p` : KERNEL32, USER32, WS2_32

**Manual checks:**
- En jeu, avec `probe = 1` : F6, F7, mort par chute, puis rechargement, F6, F7 et mort au combat. Fournir `Script_Multiplayer.log`.
