# Équipement d'une entité créée par le mod (story 3.2b)

Désassemblage statique de `Game.dll` (Steam x64, base `0x180000000`), puis confirmé en jeu le
2026-10-21 (Risen hôte, faux invité `--equipment It_Armor_Don_Fighter --echo`).

## Symptôme

L'avatar (`Entity::Spawn(template du héros, temporary = true)`) équipé juste après sa création
affichait l'armure **et** la tunique de départ du héros, ainsi que son bâton. Son inventaire était
pourtant vide à la création (`GetStackCount` = 0) : tunique et bâton ne sont pas des piles.
`PSInventory::UpdateVisibility()` n'y change rien (il ne fait qu'appeler `EnableRendering` objet par
objet).

## Mécanisme

- `gCInventory_PS` porte 15 **emplacements par défaut** (`gCInventorySlot`, 0x28 octets, à
  `this + 0x48 + 0x28 × slot`) : `+0x08` template de l'objet par défaut, `+0x10` entité créée,
  `+0x20` `gESlot` où il est lié (0 = non lié). Tunique et bâton viennent du template PC_Hero.
- Emplacement d'équipement 8 (armure) → `gESlot` 11 (`Slot_Body`, lien « morceau de corps »,
  `eCAnimation_PS::AddBodyPart`) ; 10 = tête, 1 = main droite, 3 à 5 = dos
  (`gCInventorySlot::GetSlot(gEEquipSlot)` @`1808cd140`, `GetBone` @`1808cc450`).
- Équiper : `EquipStackToSlot` @`1808c4bc0` → `LinkStackToSlot(i, 11)` @`1808c1ea0` →
  `UnlinkDefaultItemFromSlot(11)` @`1808c1780`, qui retire l'objet par défaut **sauf** si une pile
  est déjà marquée liée à cet emplacement (`pile + 0x20 == 11`).
- Les objets par défaut ne sont liés que dans `gCInventory_PS::OnCacheIn` @`1808bffa0` : d'abord
  tous les objets par défaut, puis `LinkStackToSlot` pour chaque pile équipée. `OnCacheOut`
  @`1808bfea0` délie tout.

## Cause et correctif

Équipée avant le premier `OnCacheIn` de l'entité, l'armure est déjà marquée liée (`LinkToSlot`
@`1808cce30`) ; `OnCacheIn` lie ensuite la tunique, et `UnlinkDefaultItemFromSlot` sort sans rien
faire. Le héros du joueur s'équipe toujours après son `OnCacheIn`.

Correctif retenu (`sync/avatar`) : équipement **et** arme de l'avatar différés de 15 frames après
la création (environ 250 ms). Validé en jeu : plus de tunique, le bâton disparaît.

## Plan B (non utilisé)

`gCInventory_PS` s'obtient par `??CPSInventory@@QEBAPEBVgCInventory_PS@@XZ` (Script.dll) ;
rééquiper par `?UnEquipStack@gCInventory_PS@@QEAA_NH_N@Z` puis `EquipStack`, ou délier
directement l'objet par défaut : `?GetDefaultSlot@gCInventory_PS@@QEAAAEAVgCInventorySlot@@W4gESlot@@@Z`
@`1808c1db0`, `?IsLinked@gCInventorySlot@@QEBA_NXZ`, `?UnLinkFromSlot@gCInventorySlot@@QEAA_NPEAVeCEntity@@@Z`.
Utile pour la story 3.2c (armure de l'hôte) si l'avatar doit changer d'armure en cours de session.
