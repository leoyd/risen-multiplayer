# Epic 3 Context: Combattre ensemble

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

Les deux joueurs combattent les mêmes monstres en mêlée (jalon J2). C’est la ligne d’arrivée du MVP : nettoyer à deux un camp de monstres de la zone de départ, sans désynchronisation visible ni crash. L’invité porte l’équipement de sa config, et son avatar tient son arme. Chez l’invité, les PNJ proches suivent l’hôte, sans IA locale. Ses coups blessent les monstres chez l’hôte, ceux des monstres le blessent dans son propre jeu, et aucun coup ne compte deux fois. À zéro PV, il tombe à terre puis réapparaît près de l’hôte, PV pleins, sans quitter la session. Le spike J2 (3.0) est fait ; les détails de rétro-ingénierie sont dans `docs/re/spike-j2.md`.

## Stories

- Story 3.0 : Spike J2 *(fait)*
- Story 3.1 : Registre des entités (avatar solide abandonné) *(fait)*
- Story 3.2 : Arme de l’avatar *(fait ; découpée en 3.2, 3.2b, 3.2c)*
- Story 3.2b : Équipement de l’invité *(fait)*
- Story 3.2c : Armure portée, transmise à l’avatar *(fait)*
- Story 3.3 : Répliques des PNJ : apparition, position et PV *(fait)*
- Story 3.4 : Répliques des PNJ : animations *(fait)*
- Story 3.4a : Robustesse de session *(fait)*
- Story 3.4b : Mort des répliques de PNJ *(fait, protocole v11)*
- Story 3.5 : Dégâts de l’invité sur les monstres *(fait, protocole v12)*
- Story 3.6 : Dégâts subis par l’invité *(fait, protocole v13 ; corrigé le 2026-10-23 : dégâts mesurés)*
- Story 3.7 : À terre et réapparition de l’invité *(fait, protocole v14)*

## Requirements & Constraints

- **Répliques.** Chez l’invité, position, orientation, animation, PV et mort des PNJ hostiles proches (50 m au plus d’un joueur, hypothèse) suivent l’hôte, sans IA locale. Un monstre peut cibler et frapper l’avatar de l’invité.
- **Dégâts.** Un coup de l’invité blesse le monstre chez l’hôte, et la jauge baisse chez les deux. Les coups des monstres réduisent les PV de l’invité dans son propre jeu. Chaque coup a une seule source.
- **Mort.** L’invité ne meurt jamais nativement : à terre, puis réapparition près de l’hôte, PV pleins, sans `Kill`, ragdoll ni menu de chargement. Limite documentée : une mort native (chute, eau profonde) n’est pas interceptée ; la session finit au rechargement de l’invité. La mort de l’hôte reste native et termine la session. Le relèvement par l’autre joueur est hors périmètre (J5).
- **Périmètre.** Mêlée uniquement ; fiabilité avant étendue. Pas d’interface en jeu : le seul retour est le log.
- **Robustesse.** Aucun échec du mod ne fait crasher le jeu ; toute erreur ramène au solo, raison au log. Session de 30 min sans crash.
- **Tests.** Une instance de Risen par machine : tests courants avec Risen en hôte et le faux invité Go (`tools/fake-guest/`, étendu à chaque story) ; validation finale à deux machines, surtout côté invité. Les stories 3.4 à 3.7 restent à valider en jeu (`docs/test-epic-3.md`).

## Technical Decisions

- **Avatar traversable**, placé à chaque frame par `Entity::MoveTo(bCMatrix)`. Ne pas réintroduire la recette « solide » (elle décroche le modèle animé). Les PNJ de l’hôte ciblent et frappent l’avatar quand même.
- **Registre unique** (extension de `sync::AnimFilter`, même verrou) : `{entité, PS d’animation, PS de routine, rôle}`, rôles héros local / avatar / réplique de PNJ. Écrit seulement par `sync/avatar` ou `sync/npc` ; les détours ne font que lire. Une entité en sort avant d’être oubliée ou libérée.
- **Liste fermée des hooks**, tous posés par `win/hooks`, un détour par fonction qui choisit selon le rôle : `OnProcess`, `PlayMotion`, `SetRagDollEnabled`, les quatre réglages de locomotion, `gCScriptRoutine_PS::OnProcess`, les trois `DownOrDeadBy*` (filet facultatif en session), et le point d’accroche des dégâts `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll, 1 argument). Tout ajout met la liste à jour dans la spine.
- **Point d’accroche des dégâts.** Agresseur = `eCEntityProxy::GetEntity(GetAgressor())` (proxy à `+0x30`), cible = l’argument. Facultatif (clé `damage_hook`) : absent, la session tourne sans dégâts synchronisés. Hors frame, il ne lit que des accesseurs sans effet de bord, compare au registre et met en file (64 au plus par file), sous verrou court. **Bloquer un coup = rendre `true` sans appeler l’original** : le jeu joue l’effet d’impact sans dégâts. Exceptions : tir ami et agresseur nul, bloqués sans effet. Une garde de réentrance par thread couvre l’application des `HitRequest` par l’hôte.
- **PNJ de l’invité = ses PNJ natifs** (même sauvegarde), appariés par le GUID de 16 octets de `GetID` (+ 4 octets nuls) via `gCRuntimeInfo::GetEntity`. L’hôte attribue un `NetId` ≥ 1000, lié au GUID toute la session. IA figée PNJ par PNJ par le hook de routine ; `EnableClassProcessing` interdit. À `Ended`, une réplique est libérée, jamais détruite.
- **Une source par coup** :
  - invité, son héros frappe une réplique → bloqué, `HitRequest{NetId cible}` ;
  - invité, tout coup natif sur son héros → bloqué ; dégâts seulement par `HitOnGuest` ;
  - hôte, l’avatar invité frappe un PNJ → bloqué ; seuls les `HitRequest` blessent ;
  - hôte, un PNJ frappe l’avatar invité → **mesuré** : avatar remis à ses PV max, coup natif appliqué (scripts du jeu, armure de l’avatar comprise ; mise à terre ou mort annulée dans `DownOrDeadBy*`), perte de PV émise dans `HitOnGuest{NetId source, dégâts}`, PV d’avant rendus. La perte est **doublée** quand l’avatar est du groupe « Player » (le script de dégâts divise par deux pour ce groupe). L’avatar ne perd jamais de PV au bilan ; ses PV ne servent qu’à l’affichage, jamais à détecter un coup. Source 0 si l’agresseur n’est pas un PNJ capturé (journal seulement). La formule `(Amount + Bonus) × Multiplier` d’origine est abandonnée (nulle pour les monstres).
- **Application chez l’hôte.** `PSDamage::DoDamage(arme en main de l’avatar, cible, agresseur = avatar)` ; sans arme en main, requête ignorée et journalisée. Jamais d’écriture directe des PV d’un PNJ.
- **PV du héros invité** : un seul écrivain, `sync/guest_health` (`SetHitPoints`). `HitOnGuest` retire les dégâts ; ≤ 0 → 1 PV et « à terre ». Les détours `DownOrDeadBy*` (`self` = l’arme, `other` = la victime) servent de filet. À terre : `Death{2}` une fois (verrou jusqu’au `Respawn`), `HitOnGuest` ignorés. L’hôte répond `Respawn{2, pose}` (2 m devant son héros) ; l’invité s’y place, PV pleins, et renvoie l’accusé `Respawn{2, pose, tick}`. L’hôte ignore les snapshots du `NetId` 2 jusqu’à l’accusé, puis ceux de tick inférieur. Replis : 10 s sans `Respawn` chez l’invité, 5 s sans accusé chez l’hôte. Jamais 0 PV, jamais `Kill` ni `DoDamage` sur le héros invité.
- **Mort d’une réplique** : par `Death{NetId}`, appliqué une fois, sans `Kill` natif ; un `NetId` mort n’est jamais réannoncé.
- **Protocole.** Snapshots complets à 20 Hz, 22 o par entité (PV inclus), les plus proches d’abord. Liste fermée d’événements ; `Spawn` typé (0 avatar, 1 PNJ avec GUID). Tout message ou struct modifié : `protocol/PROTOCOL.md`, vecteurs `protocol/vectors/`, version incrémentée, tests C++ et Go au vert.
- **Règles transverses.** Jeu appelé seulement depuis `OnProcess`, sauf hooks `PlayMotion`, routine, `DownOrDeadBy*` et dégâts (même thread). Cadences à l’horloge QPC. Seul `game/` appelle le jeu (nom décoré, tout ou rien, sret explicite, SEH ; `Script_Game.dll` dans l’empreinte). Aucun pointeur d’entité hors de la table `NetId` ni après `Ended`. Une seule transition vers `Ended` pour toute fin de session. Pas d’exception entre couches. Logs `[net]`/`[game]`/`[sync]`, rien par frame hors `log_level = debug`. Toute nouvelle clé de config a un défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- Disponibles : registre, arme/équipement/armure de l’avatar, `NetId` des PNJ, gel d’IA, animations et mort des répliques, `HitRequest`, `HitOnGuest`, `Respawn` avec accusé, point d’accroche des dégâts.
- La mesure des dégâts sur l’avatar (correctif 3.6) s’appuie sur les détours `DownOrDeadBy*` de 3.7 pour annuler mise à terre et mort de l’avatar chez l’hôte : tout changement de ces détours doit préserver ce chemin.
- Questions encore ouvertes : PNJ créés en cours de partie (GUID identique chez les deux pairs ?), GUID identique d’un PC à l’autre (hypothèse). Seule la mêlée a été testée en jeu ; `Projectile` et `Inferno` ne sont vérifiés qu’au désassemblage.
