# Epic 3 Context: Combattre ensemble

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Les deux joueurs combattent les mêmes monstres en mêlée (jalon J2, ligne d’arrivée du MVP : nettoyer à deux un camp de monstres sans désynchronisation visible ni crash). L’invité porte l’équipement de sa config et son avatar tient son arme. Chez l’invité, les PNJ proches suivent l’hôte, sans IA locale. Ses coups blessent les monstres chez l’hôte, ceux des monstres le blessent dans son propre jeu, et aucun coup ne compte deux fois. À zéro PV, il tombe à terre puis réapparaît près de l’hôte, PV pleins, sans quitter la session. Le spike J2 (3.0) est fait ; ses conclusions sont intégrées aux décisions ci-dessous (détails de rétro-ingénierie dans `docs/re/spike-j2.md`).

## Stories

- Story 3.0 : Spike J2 *(fait)*
- Story 3.1 : Registre des entités (avatar solide abandonné) *(fait)*
- Story 3.2 : Arme de l’avatar *(faite ; découpée en 3.2, 3.2b, 3.2c)*
- Story 3.2b : Équipement de l’invité *(fait)*
- Story 3.2c : Armure portée, transmise à l’avatar *(fait)*
- Story 3.3 : Répliques des PNJ : apparition, position et PV *(fait)*
- Story 3.4 : Répliques des PNJ : animations *(fait ; la mort part en 3.4b)*
- Story 3.4a : Robustesse de session (heartbeat de secours, PNJ en avance, fin de monde) *(en cours)*
- Story 3.4b : Mort des répliques de PNJ
- Story 3.5 : Dégâts de l’invité sur les monstres
- Story 3.6 : Dégâts subis par l’invité
- Story 3.7 : À terre et réapparition de l’invité

## Requirements & Constraints

- **Répliques** : chez l’invité, position, orientation, animation, PV et mort des PNJ hostiles proches (≤ 50 m d’un des joueurs, hypothèse) suivent l’hôte, sans IA locale. Un monstre tué chez l’hôte est mort chez l’invité et ne réapparaît pas de la session. Un monstre peut cibler et frapper l’avatar de l’invité.
- **Dégâts** : un coup de l’invité blesse le monstre chez l’hôte, la jauge baisse chez les deux. Les coups des monstres réduisent les PV de l’invité dans son propre jeu. Une seule source par coup.
- **Mort** : l’invité ne meurt jamais nativement : à terre, puis réapparition près de l’hôte, PV pleins. Ni `Kill`, ni ragdoll, ni menu de chargement. La mort de l’hôte reste native et termine la session.
- **Périmètre** : mêlée uniquement, strict nécessaire (fiabilité plutôt qu’étendue). La locomotion de l’avatar n’est pas un objectif. Pas d’interface en jeu : le seul retour est le log.
- **Robustesse** : aucun échec du mod ne fait crasher le jeu ; toute erreur ramène au solo avec la raison au log. Session de 30 min sans crash.
- **Tests** : une seule instance de Risen par machine. Tests courants avec Risen en hôte et le faux invité Go (`tools/fake-guest/`, étendu par story), validation finale à deux machines.

## Technical Decisions

- **Base réutilisée** : enveloppes ABI de combat dans `game/combat`, détours `Kill`, `DownOrDeadBy*` et `gCScriptRoutine_PS::OnProcess` issus de la sonde. Sonde (`probe = 1`) et session sont exclusives pour tous les détours.
- **Avatar traversable**, placé à chaque frame par `Entity::MoveTo(bCMatrix)`. La recette « solide » (groupe 4, collision, DCC, `AddBodyFlag(0x81)`, gravité coupée) est abandonnée : elle décroche le modèle animé. Ne pas la réintroduire.
- **Registre unique des entités** (extension de `sync::AnimFilter`, même verrou) : `{entité, PS d’animation, PS de routine, rôle}`, rôles héros local / avatar / réplique de PNJ. Écrit seulement par le module propriétaire (`sync/avatar`, `sync/npc`), lu par les détours. Une entité en sort avant d’être oubliée ou libérée.
- **Liste fermée des hooks**, tous posés par `win/hooks` : `OnProcess`, `PlayMotion`, `SetRagDollEnabled`, les quatre réglages de locomotion, `gCScriptRoutine_PS::OnProcess`, les trois `DownOrDeadBy*`, et le point d’accroche des dégâts (à trancher en 3.5 : `gCDamage_PS::DoDamage` ou le virtuel `OnDamage` de la cible ; il doit voir agresseur et cible et pouvoir bloquer). Un détour par fonction, qui choisit selon le rôle. Tout ajout met la liste à jour dans la spine.
- **PNJ de l’invité = ses PNJ natifs** (même sauvegarde), appariés par le GUID de 16 octets de `GetID` (+ 4 octets nuls) via `gCRuntimeInfo::GetEntity`. `NetId` ≥ 1000 attribué par l’hôte, lié au GUID toute la session. IA figée PNJ par PNJ par le hook de routine ; `EnableClassProcessing` interdit. À `Ended`, une réplique est libérée (IA rendue), jamais détruite. Chez l’invité, seul `sync/npc` écrit l’état des répliques (PV compris). GUID introuvable : PNJ ignoré, journalisé une fois.
- **Animation des PNJ** : rejeu par nom (`AniName`/`Motion`/`MotionParam`) émis par l’hôte ; le moteur de l’invité n’anime jamais une réplique. Budget : seules les 8 répliques les plus proches d’un joueur sont animées, ≤ 32 Ko/s, mesuré au bilan debug.
- **Mort d’une réplique (3.4b)** : l’hôte émet `Death{NetId}` au lieu du `Despawn` de mort ; l’invité la tue une fois, mort « visuelle » sans `Kill` natif (`AIMode = 12` puis ragdoll) : ni XP, ni quête chez l’invité, cadavre pillable avec son inventaire natif. Trancher aussi le PNJ vivant à 0 PV (humain à terre).
- **Coups** : un coup natif impliquant une entité non locale est bloqué au point d’accroche puis converti. Invité frappant une réplique → `HitRequest{NetId cible}` ; PNJ frappant l’avatar invité chez l’hôte → `HitOnGuest{NetId source, dégâts bruts}` (avatar invulnérable, ses PV ne servent qu’à l’affichage). L’hôte applique `HitRequest` par `PSDamage::DoDamage(arme en main de l’avatar, cible, agresseur = avatar)` et n’écrit jamais directement les PV d’un PNJ.
- **PV du héros invité** : un seul chemin d’écriture, dans `sync/` : `HitOnGuest`, ou les détours `DownOrDeadBy*` (victime = héros invité et PV ≤ 0 → 1 PV, à terre, retour 0 sans l’original). À terre = 1 PV et `Death{2}` émis une seule fois, puis l’hôte répond `Respawn{2, pose}` (PV pleins, interpolation réinitialisée). Écartés : `Kill → Defeat`, `ZS_Unconscious` forcé.
- **Protocole** : snapshots complets à 20 Hz, 22 o par entité (PV inclus), les plus proches d’abord si un datagramme ne suffit pas. `Spawn` typé (0 avatar, 1 PNJ avec GUID). Tout message ajouté ou struct modifiée → `protocol/PROTOCOL.md`, vecteurs `protocol/vectors/`, version du protocole incrémentée, tests C++ et Go.
- **Transverse** : le jeu n’est appelé que depuis `OnProcess`, sauf le hook `PlayMotion` (copie ou blocage), le hook de routine (comparaison au registre) et les détours `DownOrDeadBy*` (même thread, peuvent appeler `SetHitPoints`/`GetOtherEntity`). Cadences à l’horloge QPC. Seul `game/` appelle le jeu (nom décoré, tout ou rien, sret explicite, SEH) ; `Script_Game.dll` est dans l’empreinte. Aucun pointeur d’entité hors de la table `NetId` ni après `Ended`. Pas d’exception entre couches ; logs `[net]`/`[game]`/`[sync]`, rien par frame hors `log_level = debug` ; toute nouvelle clé de config a un défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- 3.1 à 3.4 sont faites : registre, arme et équipement de l’avatar, `NetId` des PNJ, gel d’IA et animations des répliques sont disponibles.
- **3.4b** s’appuie sur 3.3/3.4 (répliques, ragdoll) ; 3.5 en dépend pour qu’un monstre tué par l’invité meure chez les deux.
- **3.5** a besoin de l’arme en main de l’avatar (3.2) pour `DoDamage`, tranche le point d’accroche des dégâts (Q8) et l’ajoute à la liste des hooks ; **3.6** réutilise ce même point d’accroche.
- **3.7** dépend de 3.6 (l’invité peut tomber à 0 PV) et de l’unique transition vers `Ended`. Elle règle les morts hors `DownOrDeadBy*` (Q9 : chute, noyade, message d’XP) et les snapshots périmés après `Respawn` (Q13). Le retour à la vie de l’avatar (arme et armure perdues pendant sa mort) relève aussi de `Respawn`.
