# Story 4.5 — Champ de force et téléportation commune (protocole v19)

Demande de l'utilisateur (2026-11-22), après le test de la 3.4f :
« est-il possible de rajouter un champ de force qui empêche le client d'aller trop loin par rapport à
l'hôte ? en cas de téléportation d'un des joueurs, les 2 joueurs sont téléportés ensemble ».
Choix de l'utilisateur : revenir à la portée du moteur (40 m) + champ de force ; rayon 35 m ;
téléportation dans les deux sens, même monde.

## Constat du test du 2026-11-22 (3.4f)

- `portée de traitement réglée : 40 m → 250 m` des deux côtés, mais 32 PNJ suivis, 44 m au plus.
- PNJ de l'hôte en T-pose en marchant après un aller-retour lointain.
- Cause (Engine.dll, `eCPrefetchAdmin`) : `OnTestProcessingRange` teste la sphère de l'entité contre la
  sphère ROI, mais seulement pour les entités de l'arbre de scène (+0x2a8), où elles n'entrent qu'une
  fois « chargées » ; le chargement suit leur sphère de préchargement, calculée par entité à l'ajout
  (`GetPrefetchSphere` : rayon propre +0xd0 / +0xcc, sinon portée de cache par défaut +0x28). Relever la
  ROI en cours de partie ne recalcule que les entités qui bougent.
- Décision : `processing_range = 0` par défaut (code gardé, expérimental).

## Comportement

- **Champ de force** (invité, `leash_radius`, mètres, 35 par défaut, 0 = aucun) : distance horizontale
  au héros de l'hôte affiché ; au-delà, `Teleport{Leash}` sur le cercle de rayon `leash_radius − 0,5 m`,
  message à l'écran (`ShowMessage`, `GUI2::PrintGameLog`) toutes les 3 s au plus. Au-delà de
  `leash_radius + 20 m` : Regroup attendu ; après 3 s, `Teleport{Rejoin}` devant l'hôte.
  Suspendu : sans avatar de l'hôte, avatar mort, héros mort, invité à terre, en dialogue, 3 s après un
  déplacement imposé ou un saut (de soi ou de la pose affichée de l'hôte).
- **Alerte à l'écran** (demande de l'utilisateur, même jour) : overlay de l'invité, dès 80 % du rayon,
  bordure rouge en dégradé d'opacité croissante (15 % → 60 %), pulsée à la limite, texte centré
  « Distance de l’hôte : 32 m / 35 m » (`core/overlay_text` : `leash_warning_level`, `leash_warning_text` ;
  `win/overlay` : `set_warning`, `draw_edges`).
- **Téléportation commune** : saut > 15 m d'un tick à l'autre → `Regroup{pose}` (fiable, 17 octets,
  type 32). Réception : `Teleport{Regroup}` à 2 m devant la pose (`guest_pose_near`), sauf pose non
  finie, héros mort, invité à terre, en dialogue (Warn). Sauts non signalés pendant 3 s après un
  déplacement imposé (Welcome, Respawn, Regroup reçu, Rejoin).
- Hors périmètre : changement de monde (fin de session), arbitrage de deux téléportations croisées.

## Diagnostic du moonwalk

L'utilisateur a vu l'avatar de l'invité faire du moonwalk chez l'hôte alors qu'il courait droit devant.
Les journaux ne suffisent pas : Debug `[sync] avatar : déplacement à rebours …` / `glisse sans animation
de déplacement …` (angle, vitesse, animations de type 0 et 1 en cours), une ligne par seconde au plus.

## Fichiers

`core/protocol.*` (Regroup, v19), `core/config.*` (`leash_radius`, `processing_range` 0),
`core/session.*` (regroup, on_regroup, champ de force, diagnostic), `sync/session.cpp` (raisons de
Teleport, ShowMessage, réglage), `sync/avatar.*` (téléportation sans journal), `protocol/PROTOCOL.md`,
`protocol/vectors/regroup.bin`, `tools/fake-guest` (codec v19), tests.
