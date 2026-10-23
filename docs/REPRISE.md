# Reprise du travail — état au 2026-10-23 (soir)

Note de passation : où en est le projet et quoi faire à la prochaine session. Le détail de chaque livraison est dans sa spec, sous `_bmad-output/implementation-artifacts/spec-*.md`.

## Où on en est

- **Epics 1 et 2 (J0, J1)** : finies, fusionnées dans `main`. La validation à deux machines de J1 reste à faire (« je testerai plus tard »).
- **Epic 3 « Combattre ensemble » (J2)**, stories dans `_bmad-output/planning-artifacts/epics.md` :

| Story | Sujet | État |
|---|---|---|
| 3.0 | Spike J2 (sonde `probe = 1`, pavé numérique) | fait, conclusions dans `docs/re/spike-j2.md` |
| 3.1 | Registre des entités (AD-12) ; avatar solide **abandonné** | fait |
| 3.2 | Arme de l’avatar (message `Weapon`, protocole **v7**) | fait, **validé en jeu sur une machine** le 2026-10-21 (7 armes, écho) |
| 3.2b | Équipement de l’invité : `guest_equipment`, héros invité, avatar équipé chez l’hôte (protocole toujours v7) | fait, **avatar validé en jeu sur une machine** le 2026-10-21 ; équipement de l’avatar différé de 15 frames (`docs/re/equipement.md`) ; héros d’un invité réel non testé |
| 3.2c | Armure portée, transmise à l’avatar dans les deux sens (message `Armor`, protocole **v8**) | fait, **validé en jeu sur une machine** le 2026-10-21 (changements d’armure suivis, tunique de retour quand l’armure est retirée) |
| 3.3 | Répliques des PNJ : apparition, position, PV (`Spawn{1}` avec GUID, protocole **v9**) | fait ; **côté hôte testé en jeu** le 2026-10-22 (annonces, GUID, PV) ; correctif : délai de grâce de 2 s pour les PNJ absents de la liste du moteur (`8887fe1`), **revérifié en jeu** le 2026-10-22 (retraits espacés, tous « absent de la liste depuis 2 s », 73 annonces pour 55 retraits en 2 min de va-et-vient) ; côté invité (GUID, IA figée) seulement testable à deux machines |
| 3.4 | Répliques des PNJ : animations (protocole **v10**) | fait ; côté invité vu en jeu au premier test à deux machines le 2026-10-23 (session coupée par timeout pendant une cinématique, corrigé en 3.4a) ; branche `auto/epic-3` |
| 3.4a | Robustesse de session (heartbeat de secours, PNJ en avance, fin de monde) | fait, non testé en jeu, branche `auto/epic-3` |
| 3.4b | Mort des répliques de PNJ sans `Kill` natif (protocole **v11**) | fait, non testé en jeu, branche `auto/epic-3` |
| 3.5 | Dégâts de l’invité sur les monstres (`HitRequest`, protocole **v12**) | fait, non testé en jeu, branche `auto/epic-3` |
| 3.6 | Dégâts subis par l’invité (`HitOnGuest`, protocole **v13**) | fait, non testé en jeu, branche `auto/epic-3` |
| 3.7 | À terre et réapparition de l’invité (`Respawn` avec accusé, protocole **v14**) ; mort native (chute, eau profonde) = limite documentée | fait, non testé en jeu, branche `auto/epic-3` (procédure : `client/README.md`, « À terre et réapparition ») |

- **Branches** :
  - `main` est à `be50231` (story 3.3 revérifiée en jeu), identique à `origin/main`.
  - tout le travail des stories 3.4 à 3.7 est fusionné dans `auto/epic-3` (8 commits d’avance sur `main`, dont `f21a407` : PRD), **rien de poussé**. Les branches `story/3-4-*` à `story/3-7-*` sont des étapes intermédiaires, déjà contenues dans `auto/epic-3`.

## Décisions de l’utilisateur à ne pas rediscuter

- L’avatar reste **traversable** et placé par `MoveTo`. La recette « solide » du spike décroche son modèle animé. Les PNJ le ciblent quand même.
- La story 3.2 a été découpée. La 3.2b reprend :
  - le format de `guest_equipment` : noms de templates séparés par des virgules, `[A-Za-z0-9_]`, 1 à 64 octets par nom, 16 noms et 255 octets au plus ;
  - le héros de l’invité, équipé à l’entrée en session : l’objet est créé **seulement s’il manque** (`FindStackIndex`, sinon `CreateItems`), puis `EquipStack` ;
  - l’avatar de l’invité, équipé chez l’hôte depuis `Hello.equipment` ;
- L’armure portée (3.2c) circule **dans les deux sens** (message `Armor`), emplacement d’équipement 8.
- 3.3 :
  - **tous les PNJ vivants du rayon** sont répliqués, pas seulement les hostiles ;
  - un **GUID introuvable** chez l’invité est ignoré et journalisé une fois.
- Le protocole change de version à chaque story qui ajoute un message.

## Reprendre

1. **Dérouler [`docs/test-epic-3.md`](test-epic-3.md)** sur la branche `auto/epic-3` : préparation (compilation, DLL identiques des deux côtés, `multiplayer.ini`, overlay Steam désactivé), partie A seul (Risen en hôte + faux invité), partie B à deux avec Stone, puis la grille de résultats et les extraits de logs du §8.
2. **Corriger** ce que le test met en défaut, story par story (specs `_bmad-output/implementation-artifacts/spec-3-4*.md` à `spec-3-7-a-terre-reapparition.md`).
3. **Fusionner `auto/epic-3` dans `main`** une fois le test passé, puis pousser si l’utilisateur le demande.

## Points ouverts à vérifier en jeu

- ~~**Pillage de l’avatar mort**~~ : sans objet depuis la story 3.7 (protocole v14) : `Death{2}` ne met plus l’avatar en ragdoll, il est replacé devant l’hôte ; l’avatar ne meurt plus chez l’hôte (invulnérable depuis la 3.6).
- **Validation à deux machines** de J1, de l’arme, de l’équipement (héros d’un invité réel), de l’armure et du côté invité de la 3.3 (PNJ retrouvés par GUID, IA figée, positions et PV suivis).
- 3.3 : coût de la lecture des PNJ à 20 Hz, et `MoveTo` sur un PNJ figé sans lutte avec son contrôleur de déplacement.
- Sans `--echo`, l’avatar reste en T-pose : c’est **normal**, car le faux invité n’envoie alors aucune animation.

## Repères

- Règles : `CLAUDE.md`, spine d’architecture (AD-1 à AD-12) dans `_bmad-output/planning-artifacts/architecture/…/ARCHITECTURE-SPINE.md`, questions ouvertes 8 à 14 (la 14 est tranchée par la 3.2 et la 3.2b).
- Reports et dette connue : `_bmad-output/implementation-artifacts/deferred-work.md`.
- Rétro-ingénierie : `docs/re/` (spikes J1 et J2, fin de monde, `equipement.md` : objets par défaut du héros). Pour le désassemblage, utiliser `objdump -p` ; l’outil REA ne répond toujours pas.
