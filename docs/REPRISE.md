# Reprise du travail — état au 2026-10-21 (soir)

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
| 3.3 → 3.7 | Répliques des PNJ, dégâts dans les deux sens, à terre et réapparition | à faire |

- **Branche** : tout est dans `main` (avance rapide depuis `story/3-2b-equipement-invite`).

## Décisions de l’utilisateur à ne pas rediscuter

- L’avatar reste **traversable** et placé par `MoveTo`. La recette « solide » du spike décroche son modèle animé. Les PNJ le ciblent quand même.
- La story 3.2 a été découpée. La 3.2b reprend :
  - le format de `guest_equipment` : noms de templates séparés par des virgules, `[A-Za-z0-9_]`, 1 à 64 octets par nom, 16 noms et 255 octets au plus ;
  - le héros de l’invité, équipé à l’entrée en session : l’objet est créé **seulement s’il manque** (`FindStackIndex`, sinon `CreateItems`), puis `EquipStack` ;
  - l’avatar de l’invité, équipé chez l’hôte depuis `Hello.equipment` ;
  - l’**armure portée par l’hôte**, transmise à son avatar chez l’invité. Les emplacements `gEEquipSlot` sont encore à rétro-concevoir.
- Le protocole change de version à chaque story qui ajoute un message.

## Reprendre demain

1. **Lancer la prochaine story** : `/bmad-build story 3.3` (répliques des PNJ : apparition, position, PV). Le contexte compilé `epic-3-context.md` est à jour.
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
- **Validation à deux machines** de J1 et de l’arme.
- Sans `--echo`, l’avatar reste en T-pose : c’est **normal**, car le faux invité n’envoie alors aucune animation.

## Repères

- Règles : `CLAUDE.md`, spine d’architecture (AD-1 à AD-12) dans `_bmad-output/planning-artifacts/architecture/…/ARCHITECTURE-SPINE.md`, questions ouvertes 8 à 14 (la 14 est tranchée par la 3.2 et la 3.2b).
- Reports et dette connue : `_bmad-output/implementation-artifacts/deferred-work.md`.
- Rétro-ingénierie : `docs/re/` (spikes J1 et J2, fin de monde). Pour le désassemblage, utiliser `objdump -p` ; l’outil REA ne répond toujours pas.
