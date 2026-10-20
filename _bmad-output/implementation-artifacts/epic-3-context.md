# Epic 3 Context: Combattre ensemble

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Les deux joueurs combattent les mêmes monstres en mêlée (jalon J2, ligne d’arrivée du MVP : nettoyer à deux un camp de monstres sans désynchronisation visible ni crash). L’avatar de l’autre joueur est armé ; il reste traversable, mais les monstres le ciblent et le frappent. L’invité porte l’équipement de sa config. Chez l’invité, les PNJ proches suivent l’hôte sans IA locale. Ses coups blessent les monstres chez l’hôte, ceux des monstres le blessent dans son propre jeu, et aucun coup ne compte deux fois. À zéro PV, il tombe à terre puis réapparaît près de l’hôte, PV pleins, sans quitter la session. Le spike J2 (3.0) est fait : ses conclusions sont intégrées aux invariants ci-dessous, et les détails de rétro-ingénierie sont dans `docs/re/spike-j2.md`.

## Stories

- Story 3.0 : Spike J2 *(fait)*
- Story 3.1 : Registre des entités (avatar solide abandonné) *(fait)*
- Story 3.2 : Équipement et arme de l’avatar
- Story 3.3 : Répliques des PNJ : apparition, position et PV
- Story 3.4 : Répliques des PNJ : animations et mort
- Story 3.5 : Dégâts de l’invité sur les monstres
- Story 3.6 : Dégâts subis par l’invité
- Story 3.7 : À terre et réapparition de l’invité

## Requirements & Constraints

- **Répliques** : chez l’invité, la position, l’orientation, l’animation, les PV et la mort des PNJ hostiles proches (≤ 50 m d’un des joueurs, hypothèse) suivent l’hôte, sans aucune IA locale. Un monstre peut cibler et attaquer l’invité.
- **Dégâts** : un coup de l’invité blesse le monstre chez l’hôte, et la jauge baisse chez les deux. Les coups des monstres réduisent les PV de l’invité dans son propre jeu. Chaque coup a une seule source.
- **Mort** : l’invité ne meurt jamais nativement. Il passe « à terre », puis réapparaît près de l’hôte, PV pleins. La mort de l’hôte reste native et met fin à la session.
- **Équipement** (reporté de J1) : `guest_equipment` vient de la config de l’invité et voyage dans `Hello`. Une valeur invalide est refusée au chargement, avec le nom du champ au log.
- **Périmètre** : mêlée uniquement, strict nécessaire (fiabilité plutôt qu’étendue). La locomotion de l’avatar (reliquat de la 2.4b) n’est pas un objectif.
- **Robustesse** : aucun échec du mod ne fait crasher le jeu ; toute erreur ramène au solo, avec la raison au log. Session de 30 min sans crash.
- **Tests** : une seule instance de Risen par machine. Tests quotidiens avec Risen en hôte et le faux invité Go (`tools/fake-guest/`, à étendre selon les stories), validation finale à deux machines.

## Technical Decisions

- **Base réutilisée** : les enveloppes ABI du combat (`game/combat`) et les détours `Kill`, `DownOrDeadBy*` et `gCScriptRoutine_PS::OnProcess` existent déjà, réservés à la sonde (`probe = 1`). Les stories les reprennent pour la session. Sonde et session sont exclusives pour tous les détours.
- **Avatar traversable** : il est placé à chaque frame par `Entity::MoveTo(bCMatrix)` (`SetPose` seul laisse le contrôleur de personnage et l’équipement sur place). La recette « solide » du spike (groupe 4, collision, DCC, `AddBodyFlag(0x81)`, gravité coupée) est **abandonnée** : elle décroche le modèle animé, seule l’arme suivant la pose réseau, quel que soit le placement. Les PNJ de l’hôte ciblent et frappent l’avatar sans elle. Ne pas la réintroduire dans l’Epic 3.
- **Registre unique des entités** : extension de `sync::AnimFilter`, même verrou. Il stocke `{entité, PS d’animation, PS de routine, rôle}`, avec les rôles héros local / avatar / réplique de PNJ. Seul le module propriétaire l’écrit (`sync/avatar`, `sync/npc`) ; les détours ne font que le lire. Une entité en sort avant d’être oubliée ou libérée.
- **Liste fermée des hooks**, tous posés par `win/hooks` : `OnProcess`, `PlayMotion`, `SetRagDollEnabled`, les quatre réglages de locomotion, `gCScriptRoutine_PS::OnProcess`, les trois `DownOrDeadBy*` et le point d’accroche des dégâts (à trancher). Un seul détour par fonction, qui délègue à `sync/` selon le rôle. Ajouter un hook exige de mettre la liste à jour dans la spine.
- **PNJ de l’invité = ses PNJ natifs** (même sauvegarde). Ils sont appariés par le GUID de 16 octets de `GetID` (+ 4 octets nuls), retrouvé par `gCRuntimeInfo::GetEntity`. Le `NetId` ≥ 1000 est attribué par l’hôte et reste lié à son GUID toute la session. L’IA est figée PNJ par PNJ par le hook de routine ; `EnableClassProcessing` est **interdit** (il fige tous les PNJ). À `Ended`, une réplique est libérée (IA rendue), jamais détruite. Chez l’invité, `sync/npc` est le seul à écrire l’état des répliques, PV compris.
- **Coups** : un coup natif impliquant une entité non locale est bloqué au point d’accroche, puis converti en événement. Côté invité, son héros qui frappe une réplique émet `HitRequest{NetId cible}`. Côté hôte, un PNJ qui frappe l’avatar invité émet `HitOnGuest{NetId source, dégâts bruts}` ; l’avatar est invulnérable et ses PV ne servent qu’à l’affichage. L’hôte applique `HitRequest` par `PSDamage::DoDamage(arme en main de l’avatar, cible, agresseur = avatar)` et n’écrit jamais directement les PV d’un PNJ.
- **Arme de l’avatar** : créée par `PSInventory::CreateItems(template)`, tenue par `HoldRightStack`, rangée par `EquipStack`. Il n’y a pas de mode de combat à transmettre : la posture est dans le nom des animations rejouées.
- **PV du héros invité** : un seul chemin d’écriture, dans `sync/` : `HitOnGuest`, ou les détours `DownOrDeadBy*` (victime = héros invité, PV ≤ 0 → 1 PV, à terre, retour 0 sans appeler l’original). « À terre » = 1 PV et `Death{2}` émis une seule fois, puis l’hôte répond `Respawn{2, pose}` (PV pleins, interpolation réinitialisée). Écartés : `Kill → Defeat` (menu de chargement), `ZS_Unconscious` forcé.
- **Réseau** : snapshots complets à 20 Hz (22 o par entité, PV inclus). Si un datagramme ne suffit pas, les entités les plus proches passent d’abord. Le rejeu d’animation par nom (`AniName`/`Motion`/`MotionParam`) s’étend aux PNJ, émis par l’hôte ; le moteur ne doit jamais animer une réplique. `Spawn` dépend de son type : 0 = avatar, 1 = PNJ avec GUID. Tout message ajouté ou struct modifiée → `protocol/PROTOCOL.md`, vecteurs `protocol/vectors/`, version du protocole incrémentée, tests C++ et Go.
- **Rappels transverses** : le jeu n’est appelé que depuis `OnProcess`, sauf trois cas : le hook `PlayMotion` (copie d’octets ou blocage), le hook de routine (simple comparaison au registre) et les détours `DownOrDeadBy*`, qui tournent sur ce même thread et peuvent appeler `SetHitPoints` ou `GetOtherEntity`. Cadences à l’horloge QPC. Seul `game/` appelle le jeu : résolution par nom décoré en tout ou rien, sret explicite, SEH. `Script_Game.dll` entre dans les modules résolus et dans l’empreinte. Aucun pointeur d’entité hors de la table `NetId` ni après `Ended`. Pas d’exception entre couches ; préfixes de log `[net]`/`[game]`/`[sync]`, rien par frame hors `log_level = debug`. Toute nouvelle clé de config a un défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- **3.1** (faite) fournit le registre des entités, dont dépendent toutes les stories suivantes ; l’avatar y est resté traversable. Les détours existants (`PlayMotion`, ragdoll, locomotion) passent sur le registre sans changer de comportement (banc Wine et tests 2.4b au vert).
- **3.2** fixe le format de `guest_equipment` et la transmission de l’arme sortie ou rangée (Q14). `DoDamage` côté hôte (3.5) a besoin de l’arme en main de l’avatar.
- **3.3** fixe le repli pour un GUID introuvable (Q10, PNJ créés en cours de partie) et doit vérifier que les GUID sont identiques d’un PC à l’autre. Elle fournit les `NetId` de PNJ et le gel d’IA dont dépendent 3.4 à 3.6.
- **3.4** fixe le budget de `Motion`/`MotionParam` des PNJ (Q12) et la mort d’une réplique (Q11 : sans `Kill` natif ? butin et XP de l’invité ?).
- **3.5** tranche le point d’accroche des dégâts (Q8 : `gCDamage_PS::DoDamage` ou le virtuel `OnDamage` de la cible) et l’ajoute à la liste des hooks. 3.6 réutilise ce même point d’accroche.
- **3.7** dépend de 3.6 (l’invité peut tomber à 0 PV) et de l’unique transition vers `Ended` (Epic 1). Elle règle les morts hors `DownOrDeadBy*` (Q9 : chute, noyade, message d’XP) et les snapshots périmés après `Respawn` (Q13).
