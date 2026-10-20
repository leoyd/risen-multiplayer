# Epic 3 Context: Combattre ensemble

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Les deux joueurs combattent les mêmes monstres en mêlée : le jeu de l’hôte simule les PNJ hostiles, l’invité en voit des répliques fidèles, ses coups blessent les monstres chez l’hôte, les monstres le blessent dans son propre jeu, et sa mort se règle par une réapparition près de l’hôte au lieu d’un rechargement. C’est J2, la ligne d’arrivée du MVP : nettoyer à deux un camp de monstres de la zone de départ, sans désynchronisation visible ni crash. L’epic n’est qu’esquissé : il commence par un spike (story 3.0) qui doit trancher les questions ouvertes 3, 5, 6 et 7 de la spine ; les stories suivantes seront détaillées d’après ses résultats.

## Stories

- Story 3.0 : Spike J2 (questions ouvertes 3, 5, 6, 7)
- Story 3.1 : Équipement de l’invité depuis la config
- Story 3.2 : Arme en main et mode combat de l’avatar
- Story 3.3 : Répliques des PNJ chez l’invité
- Story 3.4 : Dégâts de l’invité sur les monstres
- Story 3.5 : Dégâts subis par l’invité
- Story 3.6 : Mort et réapparition

## Requirements & Constraints

- **Répliques de PNJ** : chez l’invité, position, orientation, animation, PV et mort des PNJ proches suivent l’hôte, sans aucune décision d’IA locale. Un monstre tué d’un côté est mort des deux. Un monstre peut cibler et attaquer l’invité. « Proche » = zone chargée autour de l’hôte (hypothèse) ; la spine fixe 50 m d’un des deux joueurs (hypothèse).
- **Dégâts de l’invité** : un coup de mêlée de l’invité blesse le monstre chez l’hôte ; la jauge de PV baisse chez les deux.
- **Dégâts subis** : les attaques des monstres sur l’invité réduisent ses PV dans son propre jeu.
- **Mort** : l’invité mort réapparaît automatiquement près de l’hôte, PV pleins, et la session continue. La mort de l’hôte met fin à la session (les deux rechargent la sauvegarde de départ).
- **Équipement de l’invité** (reporté de J1) : défini dans sa config (`guest_equipment`), il voyage dans `Hello` (champ présent depuis la v1) et est porté aussi par son avatar chez l’hôte ; format et application à choisir dans la story.
- **Périmètre** : mêlée uniquement ; ne pas optimiser le nombre de PNJ ni de systèmes synchronisés — fiabilité du strict nécessaire.
- **Robustesse** : aucun échec du mod ne fait crasher le jeu ; toute erreur ramène au solo avec la raison au log. Session de 30 min d’exploration et de combat sans crash.
- **Tests** : une seule instance de Risen par machine ; tests quotidiens hôte + faux invité Go (`tools/fake-guest/`), validation finale à deux machines.

## Technical Decisions

- **Questions ouvertes à trancher par le spike 3.0** (avant toute story concernée ; les réponses s’inscrivent dans la spine avant toute story `sync/npc`) :
  - **Q3 — PNJ natifs de l’invité** : ils existent déjà (même sauvegarde). Hypothèse : appariement aux `NetId` de l’hôte par GUID d’entité ; candidats `eCEntity::GetID` (`bCPropertyID`) ou `Entity::GetNetIndex`. À fixer aussi : neutralisation de leur IA, fonction de dégâts côté hôte, invulnérabilité de l’avatar invité chez l’hôte, budget de `Motion` pour les PNJ.
  - **Q5 — Arme et mode combat de l’avatar** : le rejeu d’animation joue l’attaque sans arme en main. Pistes : `gECombatMode`, `PSCombatSystem`. À résoudre avant la story combat.
  - **Q6 — Collisions** : l’avatar placé par `MoveTo` traverse décor, héros et PNJ (pas de physique). Il doit devenir « solide » : corps de collision cinématique qui suit la pose imposée, sans gravité, et repousse héros et PNJ, pour qu’un PNJ puisse toucher l’avatar invité. Hypothèse : le recalage de `MoveTo` (`OnUpdatedWorldMatrix`) neutralise son corps physique.
  - **Q7 — Mort native de l’invité** : elle recharge vraisemblablement une sauvegarde, ce qui finirait la session ; l’interception doit précéder `Respawn`.
- **Autorité des PNJ (AD-9)** : seul l’hôte décide. Chez l’invité, l’IA des PNJ répliqués est neutralisée ; leur état ne vient que des snapshots et événements. L’invité ne modifie **jamais** une réplique : un coup produit `HitRequest{NetId cible, dégâts}`, appliqué par l’hôte avec la fonction de dégâts du jeu. Chez l’hôte, l’avatar invité est **invulnérable** ; quand un PNJ le touche, l’hôte envoie `HitOnGuest{NetId source, dégâts}`, que l’invité applique à son propre héros.
- **Identité (AD-6)** : `NetId` u32 attribué par l’hôte seul ; 1 = avatar hôte, 2 = avatar invité, PNJ à partir de 1000. Une table `NetId ↔ entité` par pair ; aucun pointeur sur le réseau ni gardé hors de la table ou après `Ended`.
- **Snapshots (AD-7)** : l’hôte y ajoute les PNJ (état complet, 22 o par entité, PV inclus, 20 Hz). Si un datagramme (≤ 1200 o, `nombre` ≤ 54) ne suffit pas, les entités les plus proches d’un joueur passent d’abord, les autres au tick suivant. Même interpolation (150 ms, extrapolation ≤ 250 ms).
- **Animation (AD-8)** : le rejeu par nom (`AniName`/`Motion`/`MotionParam`) s’étend aux PNJ, émis par l’hôte. Le moteur ne doit jamais animer une réplique lui-même (hook `PlayMotion` bloquant via `sync::AnimFilter`) ; une entité sort du filtre avant d’être oubliée ou détruite. Les leçons de la locomotion de l’avatar (vitesse issue de `gCCharacterMovement_PS`, trafic `MotionParam` élevé) valent aussi pour les PNJ.
- **Mort et réapparition (AD-11)** : à sa mort, l’invité émet `Death{2}` ; l’hôte répond `Respawn{NetId, pose}` près de son avatar, PV pleins ; `Respawn` réinitialise l’interpolation. La mort de l’hôte passe par l’unique transition vers `Ended`, qui supprime avatars **et répliques** puis rend la main au solo.
- **Protocole (AD-10)** : liste fermée, actuellement en v6. `HitRequest`, `HitOnGuest`, `Respawn` sont réservés (22, 23, 25) mais non définis : les définir exige de mettre à jour `protocol/PROTOCOL.md`, d’incrémenter la version et d’ajouter les vecteurs `protocol/vectors/*.bin`, vérifiés par les tests C++ et Go (faux invité inclus).
- **Rappels transverses** : jeu appelé uniquement depuis `OnProcess` (AD-2), cadences à l’horloge QPC ; seul `game/` appelle le jeu (`game/ai`, `game/combat`), résolution par nom décoré en tout ou rien, sret explicite, SEH autour de chaque appel, offsets en dur documentés dans `docs/re/` (AD-3). PNJ dans `sync/npc`. Pas d’exception entre couches ; préfixes de log `[net]`/`[game]`/`[sync]`, rien par frame hors `log_level = debug` ; nouvelle clé de config → défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- 3.0 conditionne tout l’epic : Q5 → 3.2 ; Q3 et Q6 → 3.3, 3.4, 3.5 ; Q7 → 3.6. Ses conclusions sont reportées dans la spine (AD-9, AD-11) avant les stories `sync/npc`.
- 3.1 s’appuie sur `Hello` (Epic 1) et sur l’avatar créé par `Spawn` (Epic 2) ; 3.2 complète l’avatar et ses animations d’attaque (Epic 2).
- 3.3 fournit les `NetId` de PNJ et la neutralisation d’IA dont dépendent 3.4 (`HitRequest`) et 3.5 (`HitOnGuest`, qui suppose aussi l’avatar solide de Q6).
- 3.6 dépend de 3.5 (l’invité peut mourir) et de la transition `Ended` de l’Epic 1.
