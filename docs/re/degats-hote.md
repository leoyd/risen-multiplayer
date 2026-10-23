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
