# Dégâts appliqués par l'hôte pour l'invité (`HitRequest`) : `CanDoMeleeDamage`

Constat au premier test à deux instances (2026-10-23) : chaque `HitRequest` reçu par l'hôte donnait
`[sync] HitRequest PNJ N appliqué : DoDamage -> 0, PV 30 -> 30`. Le détour des dégâts laissait bien
passer l'appel (garde de réentrance), donc le refus vient du jeu.

## Chaîne d'appel (désassemblage, Steam x64 de janvier 2023)

- `gCDamage_PS::DoDamage(eCEntity*)` (Game.dll, `0x1808081b0`) rend `false` sans dégâts si la cible
  est nulle ou si `eCScriptProxy::IsScriptValid` est faux sur le `DamageScript` de l'arme (`PS+0x50`).
  Sinon elle appelle ce script (`eCScriptProxyScript::operator()(arme, cible, dégâts)`) et rend
  `résultat != 0`.
- `DoSwordDamage` (Script_Game.dll, `0x1800d90c0`) appelle d'abord le script `CanDoMeleeDamage(arme,
  cible)`. S'il rend 0, le coup s'arrête là (résultat 0, PV inchangés). Sinon il calcule les dégâts
  et appelle `DoMeleeDamage`.
- `CanDoMeleeDamage` (Script_Game.dll, `0x1800d8870`) :
  - agresseur = `PSDamage::GetAgressor()` de l'arme ; agresseur nul → 0 ;
  - agresseur == `Entity::GetPlayer()` → 1 ;
  - sinon : 1 seulement si `PSNpc::GetCurrentTarget()` de l'agresseur est la cible, sinon 0.

L'avatar de l'invité chez l'hôte est un PNJ dont l'IA est figée : il n'a pas de cible actuelle, donc
tous ses coups sont refusés. Au spike J2 (Num5), l'IA de la copie avait été libérée (Num4) : elle
avait une cible, d'où le succès observé alors.

## Correctif (`sync/hits`, `apply`)

Autour de `PSDamage::DoDamage`, l'hôte lit la cible actuelle de l'avatar (`PSNpc::GetCurrentTarget`),
la remplace par le PNJ frappé (`PSNpc::SetCurrentTarget`), puis la rétablit. `SetCurrentTarget`
(Script.dll, `0x18022b0a0`) ne fait que construire un `eCEntityProxy` et appeler
`gCNPC_PS::SetCurrentTarget` : aucun autre effet observé dans le désassemblage.

## Effet d'impact et dégâts des monstres (même jour)

- Après `gCDamage_PS::DoDamage` → vrai, `gCCombatMoveMelee::HitTargets` (`0x1807d5c61`) appelle
  l'effet d'impact (`0x1807d63a0`, avec le PS `0x60` de la cible) et ajuste le minutage du coup ;
  rien d'autre. Un détour qui rend vrai sans appeler l'original garde donc l'effet sans dégâts.
- Dégâts des monstres : leur `DamageScript` n'est pas `DoSwordDamage`. `DoSpecialMonsterDamage`
  (`0x1800d98c0`) multiplie `PropertyDamageAmount` par `PropertyDamageMultiplier`, d'autres scripts
  utilisent les compétences de l'agresseur (`PSSkills::GetTotal`). `(Amount + Bonus) × Multiplier`
  du PS reçu par le détour valait 0 pour tous les coups observés (Nautilus, Vautours). D'où la
  mesure sur l'avatar plutôt qu'un calcul par script.

## Dégâts divisés par deux sur l'avatar (groupe « Player »)

Script générique `DoDamage` (Script_Game.dll, `0x1800d8060`), sur la cible :
`dégâts = max(brut − GetProtection(type), DamageMultiplier × 10)`, puis :
- cible = joueur (`IsPlayer`) : × facteur de difficulté (`/Game/Difficulty` : 0 → 0,5, 1 → 1,0,
  2 → 1,5 ; défaut 1) ;
- sinon : divisés par deux si le script `IsInPlayerParty` rend 1 (`PSParty::GetPartyName() ==
  "Player"`) ou si `PartyMemberType == 6`.

L'avatar est créé depuis le template du héros : il est du groupe « Player », donc ses dégâts étaient
divisés par deux (5 au lieu de 10 par coup de Vautour, constaté en jeu). L'hôte double la perte
mesurée quand l'avatar est de ce groupe (±1 PV sur un nombre impair). La difficulté de l'invité
n'est pas appliquée (limite : sans effet au réglage par défaut).

## Arme de l'avatar et recul de l'invité (test du 2026-10-23, 22:49)

- Chez l'hôte, la main de l'avatar s'est vidée sans `Weapon` de l'invité (entre 22:49:34 et
  22:49:39, peu après la mort d'un PNJ) : 11 `HitRequest` ignorés (« avatar sans arme en main »).
  Cause probable (test de 23:02) : le recul natif de l'avatar pendant un coup mesuré (règle 4b)
  range son arme, la main se vide 60 ms après le coup. L'avatar retient
  l'arme du dernier `Weapon` et la reprend (`HoldRightStack`) deux fois par seconde et avant chaque
  `HitRequest`.
- Recul : les coups natifs sur le héros invité étant bloqués, le jeu ne joue pas sa réaction.
  L'invité appelle `PSCombatSystem::StartStumble(gECombatMove_Stumble = 0x19, Entity attaquant)`
  (Script.dll) sur son héros à chaque `HitOnGuest` non mortel, attaquant = réplique du PNJ source.
  La variante sans mouvement rendait faux : elle interroge le mouvement de combat en cours de
  l'attaquant (`gCCombatSystem_PS+0x90`), qu'une réplique (IA figée) n'a pas.

## Attaques natives des répliques (story 3.8)

Constat (test du 2026-10-23) : chez l'invité, une réplique ne fait que rejouer les animations de
l'hôte ; son système de combat ne lance aucun mouvement, `HitTargets` n'est jamais appelé : aucun
coup natif d'un PNJ sur le héros invité, donc ni sang, ni son, ni recul adapté, ni flash.

Fonctions de `PSCombatSystem` utilisées (Script.dll, Steam x64 de janvier 2023). Toutes refont
`GetPropertySet(0x60)` sur l'entité et relaient au `gCCombatSystem_PS` par un saut indirect ; sans
ce PS, elles rendent 0, faux ou `Entity::None` :
- `GetLastAttackMove` (`0x18021e370`, non const) et `GetCurrentMove` (`0x18021f700`) : rendent un
  `gECombatMove` (enum, EAX) ;
- `GetCurrentTarget` (`0x18021f810`) : `Entity` par valeur (sret en RDX) ;
- `SetCurrentTarget(const Entity&)` (`0x18021f8b0`) : passe `*(eCEntity**)` au PS, rend un booléen ;
- `StartMove(gECombatMove)` (`0x18021f520`) : passe le mouvement (EDX) au PS, rend un booléen.

Mécanisme : l'hôte détecte le début d'une attaque d'un PNJ capturé (dernier mouvement d'attaque
changé, ou mouvement en cours redevenu égal à lui) et l'envoie (`NpcAttack`, protocole v15) ;
l'invité appelle `SetCurrentTarget` puis `StartMove` sur la réplique. Le coup natif qui en résulte
sur son héros passe par le point d'accroche des dégâts : il est appliqué pour son rendu (effet
d'impact, script de dégâts : recul, `OverlayHit`), puis les PV d'avant sont rendus ; la mise à terre
ou la mort est annulée dans `DownOrDeadBy*` comme pour la mesure de l'hôte.

Hypothèses à trancher en jeu (journal debug) : H1, `StartMove` sur une réplique à l'IA de routine
figée fait progresser le mouvement jusqu'à `HitTargets` ; H2, la détection de coup ne dépend pas de
l'animation locale (bloquée par le registre) ; H3, `GetLastAttackMove` change à chaque attaque
(sinon seule la transition de `GetCurrentMove` compte).

## Attaques natives des répliques (story 3.8, tests du 2026-10-24)

Trois conditions, trouvées en jeu une à une, pour qu'une réplique (IA figée) frappe nativement :
1. **Arme naturelle en main.** `gCCombatSystem_PS::StartMove(move)` (Game.dll, `0x1807f6f00`) cherche
   l'objet mouvement dans le style de l'arme tenue (`0x1807f8b30`), puis dans l'espèce
   (`0x1807ec610`) ; introuvable → faux. Chez l'hôte, un monstre en combat tient `It_Fist_Monster`
   (pile 0) ; la réplique ne l'a pas même dans son inventaire (créé par l'IA à l'entrée en combat).
   L'invité le crée (`PSCombatSystem::GetRightFistItem` → `FindStackIndex`, sinon `CreateItems`) et
   le tient (`HoldRightStack`), puis relance `StartMove`.
2. **Cible du système de combat** : `PSCombatSystem::SetCurrentTarget`.
3. **Cible PNJ** : `PSNpc::SetCurrentTarget`, distincte de la précédente. Sans elle,
   `CanDoMeleeDamage` refuse le coup local (`DoDamage -> 0`) et `HitTargets` saute l'effet d'impact.

Résultat : 20 coups locaux sur 24 attaques ont touché le héros invité, avec sang, recul et flash ; le
coup local fait les mêmes dégâts que la mesure de l'hôte (10), PV rendus aussitôt.

## Attaques natives de l'avatar invité chez l'hôte (story 3.8b, à valider en jeu)

Constat (test du 2026-10-24) : chez l'hôte, l'avatar de l'invité ne fait que rejouer les animations
de l'invité ; ses coups n'existent qu'en `HitRequest`, appliqués par `PSDamage::DoDamage` direct, qui
ne passe pas par `HitTargets` : ni sang ni effet d'impact sur le monstre.

Mécanisme, symétrique de la 3.8 : l'invité détecte le début d'une attaque de son héros (mêmes
fonctions `PSCombatSystem`) et l'envoie (`NpcAttack{2, mouvement, cible}`) ; l'hôte, à l'instant
d'affichage de l'avatar, applique les trois conditions trouvées pour les répliques : arme en main
(l'arme du dernier `Weapon`, reprise par `HoldRightStack` si le jeu a vidé la main ; sans arme,
`StartMove` ne trouve pas le mouvement), cible `PSCombatSystem` et cible `PSNpc` (PNJ capturé visé),
puis `StartMove`. Le coup local de l'avatar sur le PNJ passe par le point d'accroche des dégâts et
y est bloqué (règle 4) en rendant vrai : `HitTargets` joue l'effet d'impact sans dégâts. Les PV du
monstre ne changent que par `HitRequest`.

Hypothèses à vérifier en jeu : `StartMove` rend vrai sur l'avatar (gabarit du héros, IA figée) avec
son arme humaine ; le coup local atteint `HitTargets` malgré l'animation locale bloquée (comme H2
pour les répliques).
