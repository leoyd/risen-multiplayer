# Epic 3 Context: Combattre ensemble

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Les deux joueurs combattent les mêmes monstres en mêlée (jalon J2, ligne d’arrivée du MVP) : nettoyer à deux un camp de monstres de la zone de départ, sans désynchronisation visible ni crash. L’invité porte l’équipement de sa config et son avatar tient son arme. Chez l’invité, les PNJ proches suivent l’hôte, sans IA locale. Ses coups blessent les monstres chez l’hôte, ceux des monstres le blessent dans son propre jeu, et aucun coup ne compte deux fois. Les coups se voient des deux côtés comme en solo (sang, recul, flash). À zéro PV, l’invité tombe à terre puis réapparaît près de l’hôte, PV pleins, sans quitter la session. **Epic close le 2026-10-24** : stories 3.0 à 3.8b validées en jeu avec deux instances locales (protocole v15). Reste à valider à deux machines (réseau réel, côté invité de la 3.3). Rétro-ingénierie : `docs/re/spike-j2.md`, `docs/re/degats-hote.md`.

## Stories

- Story 3.0 : Spike J2
- Story 3.1 : Registre des entités (avatar solide abandonné)
- Story 3.2 : Arme de l’avatar (v7)
- Story 3.2b : Équipement de l’invité
- Story 3.2c : Armure portée, transmise à l’avatar (v8)
- Story 3.3 : Répliques des PNJ : apparition, position et PV (v9)
- Story 3.4 : Répliques des PNJ : animations (v10)
- Story 3.4a : Robustesse de session
- Story 3.4b : Mort des répliques de PNJ (v11)
- Story 3.5 : Dégâts de l’invité sur les monstres (v12)
- Story 3.6 : Dégâts subis par l’invité (v13, dégâts mesurés)
- Story 3.7 : À terre et réapparition de l’invité (v14)
- Story 3.8 : Attaques natives des répliques chez l’invité (v15)
- Story 3.8b : Attaques natives de l’avatar invité chez l’hôte (v15, `NpcAttack` bidirectionnel)

## Requirements & Constraints

- **Répliques.** Chez l’invité, position, orientation, animation, PV et mort des PNJ vivants à 50 m au plus d’un joueur suivent l’hôte, sans IA locale. Un monstre peut cibler et frapper l’avatar de l’invité.
- **Dégâts.** Un coup de l’invité blesse le monstre chez l’hôte, la jauge baisse chez les deux. Les coups des monstres réduisent les PV de l’invité dans son propre jeu. Chaque coup a une seule source de dégâts ; le rendu natif (sang, recul, flash) peut exister sans dégâts.
- **Mort.** L’invité ne meurt jamais nativement : à terre, puis réapparition près de l’hôte, PV pleins, sans `Kill`, ragdoll ni menu de chargement. Limite documentée : une mort native (chute, eau profonde) n’est pas interceptée ; la session finit au rechargement de l’invité. La mort de l’hôte reste native et termine la session. Relèvement par l’autre joueur : hors périmètre (J5).
- **Périmètre.** Mêlée uniquement ; `Projectile` et `Inferno` ne sont vérifiés qu’au désassemblage. Pas d’interface en jeu hors overlay d’état ; le log est le retour principal.
- **Robustesse.** Aucun échec du mod ne fait crasher le jeu ; toute erreur ramène au solo, raison au log. Session de 30 min sans crash.
- **Tests.** Banc local à deux instances (`~/Games/lancer-risen-coop.sh`, `tools/installer-local.sh`) ou Risen en hôte + faux invité Go (`tools/fake-guest/`, ex. `--attack-move`) ; validation finale à deux machines.

## Technical Decisions

- **Avatar traversable**, placé à chaque frame par `Entity::MoveTo(bCMatrix)`. Ne pas réintroduire la recette « solide » (elle décroche le modèle animé).
- **Registre unique** (extension de `sync::AnimFilter`, même verrou) : `{entité, PS d’animation, PS de routine, rôle}` ; rôles héros local, avatar, réplique. Écrit seulement par `sync/avatar` ou `sync/npc` ; les détours ne font que lire.
- **Liste fermée des hooks** (`win/hooks`, un détour par fonction, comportement selon le rôle) : `OnProcess`, `PlayMotion`, `SetRagDollEnabled`, quatre réglages de locomotion, `gCScriptRoutine_PS::OnProcess`, trois `DownOrDeadBy*` (facultatifs), point d’accroche des dégâts `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll, 1 argument ; facultatif, clé `damage_hook`). Tout ajout met la spine à jour.
- **Point d’accroche des dégâts.** Agresseur = `eCEntityProxy::GetEntity(GetAgressor())` (proxy `+0x30`), cible = l’argument. **Bloquer = rendre `true` sans appeler l’original** : `HitTargets` joue l’effet d’impact sans dégâts (sauf tir ami et agresseur nul). Garde de réentrance par thread pour les `HitRequest` appliqués par l’hôte.
- **PNJ de l’invité = ses PNJ natifs**, appariés par GUID (16 o de `GetID` + 4 nuls, `gCRuntimeInfo::GetEntity`) ; `NetId` ≥ 1000 lié au GUID toute la session ; GUID introuvable ignoré, journalisé une fois. IA figée par le hook de routine (`EnableClassProcessing` interdit). À `Ended`, réplique libérée, jamais détruite. Mort par `Death{NetId}`, une fois, sans `Kill`.
- **Une source par coup** :
  - invité, son héros frappe une réplique → bloqué, `HitRequest{NetId}` ;
  - invité, coup d’une réplique sur son héros → **cosmétique** (exception AD-11) : original appliqué pour le rendu, PV d’avant rendus, mise à terre/mort annulée dans `DownOrDeadBy*` ; tout autre coup natif sur son héros est bloqué ; dégâts seulement par `HitOnGuest` ;
  - hôte, l’avatar frappe un PNJ → bloqué (effet d’impact gardé) ; seuls les `HitRequest` blessent ;
  - hôte, un PNJ frappe l’avatar → **mesuré** : avatar remis à ses PV max, coup natif appliqué (armure comprise, mise à terre annulée), perte émise dans `HitOnGuest{source, dégâts}`, PV d’avant rendus. Perte **doublée** si l’avatar est du groupe « Player » (le script de dégâts la divise par deux). Formule `(Amount + Bonus) × Multiplier` abandonnée (nulle pour les monstres). PV de l’avatar : affichage seulement.
- **`HitRequest` chez l’hôte** : `PSDamage::DoDamage(arme en main de l’avatar, cible, avatar)`, avec `PSNpc::SetCurrentTarget` de l’avatar posé sur la cible le temps de l’appel (sinon `CanDoMeleeDamage` refuse). L’avatar retient l’arme du dernier `Weapon` et la reprend (`HoldRightStack`) si le jeu vide sa main. Jamais d’écriture directe des PV d’un PNJ.
- **Attaques natives** (`NpcAttack{NetId, mouvement, cible}`, type 29, clé `native_attacks`, bloc facultatif) : début d’attaque détecté par `PSCombatSystem::GetLastAttackMove`/`GetCurrentMove`, rejoué à l’instant d’affichage de l’entité. Trois conditions pour que `StartMove` frappe : arme en main (`It_Fist_Monster` créé et tenu pour une réplique ; arme du dernier `Weapon` pour l’avatar), cible `PSCombatSystem` **et** cible `PSNpc`. Hôte → invité pour les répliques ; invité → hôte avec `NetId` 2 seulement (autre NetId : Warn). Le mouvement local est **arrêté au coup porté ou après 1,5 s** (sinon il prend la locomotion). Chez l’hôte, l’avatar est **tourné vers sa cible** (lacet seul) pendant le mouvement. Repli : recul générique `StartStumble` si aucun coup cosmétique dans les 500 ms. Sans point d’accroche ni `DownOrDeadBy*`, attaques natives inactives.
- **PV du héros invité** : seul écrivain `sync/guest_health` (`SetHitPoints`), hors exception cosmétique. ≤ 0 → 1 PV, à terre, `Death{2}` une fois ; `HitOnGuest` ignorés à terre. L’hôte répond `Respawn{2, pose}` (2 m devant son héros) ; l’invité s’y place, PV pleins, renvoie l’accusé `Respawn{2, pose, tick}` ; l’hôte ignore les snapshots du `NetId` 2 jusqu’à l’accusé puis ceux de tick inférieur. Replis : 10 s (invité), 5 s (hôte). Jamais 0 PV, `Kill` ni `DoDamage` sur le héros invité.
- **Protocole.** Snapshots complets 20 Hz, 22 o par entité (PV inclus), plus proches d’abord. Liste fermée d’événements ; `Spawn` typé (0 avatar, 1 PNJ + GUID). Tout message ou struct modifié : `PROTOCOL.md`, vecteurs, version incrémentée, tests C++ et Go au vert.
- **Règles transverses.** Jeu appelé seulement depuis `OnProcess` ou les détours sur ce même thread. Seul `game/` appelle le jeu (nom décoré, sret explicite, SEH, `Script_Game.dll` dans l’empreinte). Aucun pointeur d’entité hors table `NetId` ni après `Ended` ; une seule transition vers `Ended`. Logs `[net]`/`[game]`/`[sync]`, rien par frame hors `log_level = debug`. Toute nouvelle clé : défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- La mesure des coups sur l’avatar (3.6) et les coups cosmétiques (3.8) reposent sur les détours `DownOrDeadBy*` de 3.7 : tout changement doit préserver l’annulation de la mise à terre/mort pendant ces coups.
- 3.8b dépend de l’arme de l’avatar (3.2) et du blocage règle 4 du point d’accroche (3.5).
- Finitions reportées (`deferred-work.md`) : locomotion de l’avatar (marche figée par moments, vitesse non transmise) ; sang chez l’hôte sur ~40 % seulement des coups de l’invité (décalage de position dû à l’interpolation).
- Questions ouvertes : PNJ créés en cours de partie (même GUID chez les deux pairs ?), GUID identique d’un PC à l’autre, difficulté de l’invité non appliquée aux coups mesurés (sans effet au réglage par défaut).
- Spine en retard sur 3.8b : AD-10 décrit encore `NpcAttack` hôte → invité seul, et AD-12 « bloquer = rendre `false` » (le code rend `true`).
