# Epic 3 Context: Combattre ensemble

<!-- Generated from planning artifacts. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Les deux joueurs combattent les mêmes monstres en mêlée (jalon J2). C’est la ligne d’arrivée du MVP : nettoyer à deux un camp de monstres de la zone de départ, sans désynchronisation visible ni crash. L’invité porte l’équipement de sa config et son avatar tient son arme. Chez l’invité, les PNJ proches suivent l’hôte, sans IA locale. Ses coups blessent les monstres chez l’hôte, ceux des monstres le blessent dans son propre jeu, et aucun coup ne compte deux fois. À zéro PV, il tombe à terre puis réapparaît près de l’hôte, PV pleins, sans quitter la session. Le spike J2 (3.0) est fait. Ses conclusions sont intégrées aux décisions ci-dessous, et les détails de rétro-ingénierie sont dans `docs/re/spike-j2.md`.

## Stories

- Story 3.0 : Spike J2 *(fait)*
- Story 3.1 : Registre des entités (avatar solide abandonné) *(fait)*
- Story 3.2 : Arme de l’avatar *(fait ; découpée en 3.2, 3.2b et 3.2c)*
- Story 3.2b : Équipement de l’invité *(fait)*
- Story 3.2c : Armure portée, transmise à l’avatar *(fait)*
- Story 3.3 : Répliques des PNJ : apparition, position et PV *(fait)*
- Story 3.4 : Répliques des PNJ : animations *(fait)*
- Story 3.4a : Robustesse de session *(fait)*
- Story 3.4b : Mort des répliques de PNJ *(fait, protocole v11, à valider en jeu)*
- Story 3.5 : Dégâts de l’invité sur les monstres *(fait, protocole v12, à valider en jeu)*
- Story 3.6 : Dégâts subis par l’invité *(fait, protocole v13, à valider en jeu)*
- Story 3.7 : À terre et réapparition de l’invité *(fait, protocole v14, à valider en jeu)*

## Requirements & Constraints

- **Répliques.** Chez l’invité, la position, l’orientation, l’animation, les PV et la mort des PNJ hostiles proches suivent l’hôte, sans IA locale. « Proches » veut dire à 50 m au plus d’un des joueurs (hypothèse). Un monstre peut cibler et frapper l’avatar de l’invité.
- **Dégâts.** Un coup de l’invité blesse le monstre chez l’hôte, et la jauge baisse chez les deux. Les coups des monstres réduisent les PV de l’invité dans son propre jeu. Chaque coup a une seule source.
- **Mort.** L’invité ne meurt jamais nativement : il tombe à terre, puis réapparaît près de l’hôte, PV pleins, sans quitter la session. Ni `Kill`, ni ragdoll, ni menu de chargement. Limite (D5, Q9) : une mort native (chute, eau profonde) n’est pas interceptée ; la session se termine au rechargement de l’invité. La mort de l’hôte reste native et termine la session. Le relèvement d’un joueur à terre par l’autre est hors périmètre (J5).
- **Périmètre.** Mêlée uniquement, strict nécessaire : la fiabilité passe avant l’étendue. La locomotion de l’avatar n’est pas un objectif. Pas d’interface en jeu : le seul retour est le log.
- **Robustesse.** Aucun échec du mod ne fait crasher le jeu. Toute erreur ramène au solo, avec la raison au log. Une session de 30 min se déroule sans crash.
- **Tests.** Une seule instance de Risen par machine. Les tests courants se font avec Risen en hôte et le faux invité Go (`tools/fake-guest/`, étendu à chaque story). La validation finale se fait à deux machines, en particulier pour le côté invité.

## Technical Decisions

- **Avatar traversable.** Il est placé à chaque frame par `Entity::MoveTo(bCMatrix)`. La recette « solide » est abandonnée, car elle décroche le modèle animé : ne pas la réintroduire. Les PNJ de l’hôte ciblent et frappent l’avatar quand même.
- **Registre unique des entités.** C’est une extension de `sync::AnimFilter`, sous le même verrou. Chaque entrée vaut `{entité, PS d’animation, PS de routine, rôle}`, avec trois rôles : héros local, avatar, réplique de PNJ.
  - Seul le module propriétaire l’écrit : `sync/avatar` ou `sync/npc`. Les détours ne font que le lire.
  - Une entité en sort avant d’être oubliée ou libérée.
- **Liste fermée des hooks**, tous posés par `win/hooks` :
  - `OnProcess`, `PlayMotion`, `SetRagDollEnabled` et les quatre réglages de locomotion ;
  - `gCScriptRoutine_PS::OnProcess` et les trois `DownOrDeadBy*` ;
  - le point d’accroche des dégâts, `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll, version à 1 argument).

  Il y a un seul détour par fonction, qui choisit son comportement selon le rôle. Tout ajout met la liste à jour dans la spine.
- **Point d’accroche des dégâts.**
  - L’agresseur vaut `eCEntityProxy::GetEntity(GetAgressor())` (proxy à `+0x30`), et la cible est l’argument.
  - Bloquer un coup = rendre `false` sans appeler l’original.
  - Le détour est facultatif (clé `damage_hook`) : s’il est absent, la session tourne sans dégâts synchronisés.
  - Hors frame, il ne fait que lire deux accesseurs, comparer au registre et mettre en file (64 au plus), sous un verrou court.
  - Une garde de réentrance par thread couvre l’application des `HitRequest` par l’hôte. Elle est à revoir si des `DoDamage` imbriqués apparaissent, par exemple une riposte.
- **PNJ de l’invité = ses PNJ natifs**, issus de la même sauvegarde.
  - Ils sont appariés par le GUID de 16 octets de `GetID`, suivi de 4 octets nuls, via `gCRuntimeInfo::GetEntity`.
  - L’hôte attribue un `NetId` ≥ 1000, lié au GUID pendant toute la session.
  - Leur IA est figée PNJ par PNJ par le hook de routine. `EnableClassProcessing` est interdit.
  - À `Ended`, une réplique est libérée, jamais détruite. Chez l’invité, seul `sync/npc` écrit leur état.
- **Une source par coup.** Un coup natif qui implique une entité non locale est bloqué au point d’accroche, puis converti en événement :
  - **chez l’invité**, son héros frappe une réplique → `HitRequest{NetId cible}` (fait en 3.5) ;
  - **chez l’invité**, une réplique (ou tout autre agresseur) frappe son héros → bloqué ; les dégâts n’arrivent que par `HitOnGuest` (fait en 3.6) ;
  - **chez l’hôte**, l’avatar invité frappe un PNJ → bloqué ; seuls les `HitRequest` blessent ;
  - **chez l’hôte**, un PNJ frappe l’avatar invité → bloqué, et l’hôte émet `HitOnGuest{NetId source, dégâts bruts}` (fait en 3.6). Dégâts bruts = `(Amount + Bonus) × Multiplier` du `gCDamage_PS`, sans armure en J2 ; source 0 si l’agresseur n’est pas un PNJ capturé ; pas de tir ami du héros de l’hôte. L’avatar est invulnérable. Ses PV ne servent qu’à l’affichage et ne détectent jamais un coup.
- **Application chez l’hôte.** `PSDamage::DoDamage(arme en main de l’avatar, cible, agresseur = avatar)`. Sans arme en main, la requête est ignorée et journalisée. L’hôte n’écrit jamais directement les PV d’un PNJ.
- **PV du héros invité.** Ils n’ont qu’un chemin d’écriture, dans `sync/` :
  - `HitOnGuest` retire les dégâts, par `sync/guest_health`, seul écrivain (3.6). Un résultat ≤ 0 laisse le héros à 1 PV et le met à terre (3.7).
  - Les détours `DownOrDeadBy*` traitent les coups restants. `self` y est l’arme qui frappe et `other` la victime. Si la victime est le héros invité et que ses PV sont ≤ 0, le détour le remet à 1 PV, le met à terre et rend 0 sans appeler l’original.
  - À terre = 1 PV, et `Death{2}` est émis une seule fois (verrou jusqu’au `Respawn`). L’hôte répond `Respawn{2, pose}` près de son avatar : l’invité y place son héros, PV pleins, et réinitialise l’interpolation (comme `Spawn`).
  - Jamais 0 PV, jamais `Kill` ni `DoDamage` sur le héros invité.
  - Pistes écartées : `Kill → Defeat` (menu de chargement), `ZS_Unconscious` forcé, relève d’un mort.
- **Mort d’une réplique.** Elle passe par `Death{NetId}`, appliqué une fois et sans `Kill` natif. Un `NetId` mort n’est jamais réannoncé pendant la session.
- **Protocole.**
  - Les snapshots sont complets, à 20 Hz, avec 22 o par entité (PV inclus). Si un datagramme ne suffit pas, les entités les plus proches passent en premier.
  - La liste des événements fiables est fermée. `Spawn` dépend de son type : 0 = avatar, 1 = PNJ avec GUID.
  - Tout message ajouté ou toute struct modifiée exige de mettre à jour `protocol/PROTOCOL.md` et les vecteurs `protocol/vectors/`, d’incrémenter la version du protocole, et de passer les tests C++ et Go.
- **Règles transverses.**
  - Le jeu n’est appelé que depuis `OnProcess`. Exceptions : les hooks `PlayMotion` et de routine, les détours `DownOrDeadBy*` (même thread, qui peuvent appeler `SetHitPoints` et `GetOtherEntity`) et le détour des dégâts.
  - Toute cadence se mesure à l’horloge QPC.
  - Seul `game/` appelle le jeu : par nom décoré, en tout ou rien, avec sret explicite et SEH. `Script_Game.dll` est dans l’empreinte.
  - Aucun pointeur d’entité n’est gardé hors de la table `NetId`, ni après `Ended`.
  - Toute fin de session (mort de l’hôte, timeout, quitter, fin de monde, `Reject`) passe par la même transition vers `Ended`, qui retire les avatars et libère les répliques.
  - Aucune exception ne franchit une couche.
  - Logs préfixés `[net]`, `[game]` ou `[sync]`, rien par frame hors `log_level = debug`.
  - Toute nouvelle clé de config a un défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- Les stories 3.1 à 3.7 sont faites. Sont disponibles : le registre, l’arme et l’équipement de l’avatar, les `NetId` des PNJ, le gel d’IA, les animations et la mort des répliques, `HitRequest`, `HitOnGuest` et le point d’accroche des dégâts.
- **3.6** (faite, protocole v13) réutilise le point d’accroche de 3.5 : les coups reçus par le héros invité ou par l’avatar sont bloqués et convertis en `HitOnGuest`, l’avatar est invulnérable chez l’hôte, et `sync/guest_health` applique les dégâts chez l’invité (maintien à 1 PV, flag `held`). La garde de réentrance est inchangée (aucun `DoDamage` imbriqué connu).
- **3.7** dépend de 3.6, puisque l’invité doit pouvoir tomber à 0 PV, et de l’unique transition vers `Ended`. Elle branche la mise à terre sur le `held` de `sync/guest_health` (`hit_rules::guest_hp_after`). Elle règle deux questions ouvertes :
  - les morts hors `DownOrDeadBy*` : chute, noyade, message de perte d’XP ;
  - les snapshots périmés après `Respawn`, à traiter par une époque de vie dans le snapshot ou par un accusé de `Respawn`.

  **Faite** (protocole v14) : Q13 tranchée par un accusé de `Respawn` (format du snapshot inchangé) ; Q9 laissée en limite documentée (mort native par chute ou eau profonde non interceptée : `Death{2}`, `Respawn` refusé, session finie au rechargement de l’invité). Replis : 10 s sans `Respawn` chez l’invité, 5 s sans accusé chez l’hôte. Plus de ragdoll de l’avatar sur `Death{2}`.

  Seule la mêlée du spike J2 (sonde, 3.0) a été testée en jeu ; les chemins de dégâts des stories 3.5 et 3.6 ne sont validés qu’au banc. `Projectile` et `Inferno` ne sont vérifiés qu’au désassemblage.
