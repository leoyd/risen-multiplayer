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
