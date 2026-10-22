# Reprise du travail — état au 2026-10-22

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
| 3.4 → 3.7 | Animations et mort des répliques, dégâts dans les deux sens, à terre et réapparition | à faire |

- **Branches** :
  - `main` contient tout jusqu’à la 3.3 (fusion en avance rapide le 2026-10-22) ; il a des commits d’avance sur `origin`, **non poussés**.

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

## Reprendre demain

1. **Story suivante** : `/bmad-build story 3.4` (animations et mort des répliques). À trancher : le budget `Motion` des PNJ (Q12), la mort d’une réplique (Q11), et le PNJ vivant à 0 PV (reporté dans `deferred-work.md`). Le contexte compilé `epic-3-context.md` est à jour.
2. **Recompiler et installer** :
   ```bash
   cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests
   cp client/build/Script_Multiplayer.dll ~/.local/share/Steam/steamapps/common/Risen/bin/scripts/
   (cd tools/fake-guest && go build)
   ```
   `multiplayer.ini` installé : `role = host`, `probe = 0`, `log_level = debug`. **Overlay Steam désactivé.**
3. **Tester en jeu** : Risen en hôte, puis `tools/fake-guest/fake-guest --host 127.0.0.1 --duration 120s --echo`. Les procédures par story sont dans `client/README.md`. Le log est dans `bin/scripts/Script_Multiplayer.log`.

## Points ouverts à vérifier en jeu

- **Pillage de l’avatar mort** : l’hôte peut-il fouiller le corps de l’avatar et récupérer les armes créées dans son inventaire ? Si oui, c’est une duplication d’objets, à corriger (retirer les objets au `Death` ou au `Despawn`).
- **Validation à deux machines** de J1, de l’arme, de l’équipement (héros d’un invité réel), de l’armure et du côté invité de la 3.3 (PNJ retrouvés par GUID, IA figée, positions et PV suivis).
- 3.3 : coût de la lecture des PNJ à 20 Hz, et `MoveTo` sur un PNJ figé sans lutte avec son contrôleur de déplacement.
- Sans `--echo`, l’avatar reste en T-pose : c’est **normal**, car le faux invité n’envoie alors aucune animation.

## Repères

- Règles : `CLAUDE.md`, spine d’architecture (AD-1 à AD-12) dans `_bmad-output/planning-artifacts/architecture/…/ARCHITECTURE-SPINE.md`, questions ouvertes 8 à 14 (la 14 est tranchée par la 3.2 et la 3.2b).
- Reports et dette connue : `_bmad-output/implementation-artifacts/deferred-work.md`.
- Rétro-ingénierie : `docs/re/` (spikes J1 et J2, fin de monde, `equipement.md` : objets par défaut du héros). Pour le désassemblage, utiliser `objdump -p` ; l’outil REA ne répond toujours pas.
