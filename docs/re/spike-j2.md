# Spike J2 — combat (story 3.0)

Questions ouvertes de la spine à trancher : **Q5** (arme en main et mode combat de l’avatar), **Q6** (avatar « solide »), **Q3** (identifier, figer et blesser les PNJ natifs) et **Q7** (empêcher la mort native de finir la session).

Méthode : exports décorés et désassemblage (`llvm-objdump -d --disassemble-symbols=…`), build Steam x64 de janvier 2023. Code : `client/src/game/combat.{h,cpp}` (enveloppes sous SEH), `client/src/sync/probe_j2.{h,cpp}` (expériences), `client/src/win/hooks.cpp` (hooks facultatifs posés par `install_probe`).

## ABI établie statiquement (2026-10-20)

### Vues PS de Script.dll
- `PSInventory`, `PSSkills`, `PSRoutine`, `PSRigidBody`, `PSCharacterMovement`, `PSNavigation`, `PSCombatSystem` et `PSDamage` sont de simples vues sur l’`Entity` : **`this` = `Entity*`** (8 octets, `{ eCDynamicEntity* }`). Chaque méthode refait `GetPropertySet(type)` (vtable `+0x198`) et ne fait rien si le PS est absent. Dans ce cas, les accesseurs entiers rendent −1 ou 0.
- Les propriétés imbriquées (`PSRoutine::PropertyAIMode`, `PropertyLastAIMode`) prennent elles aussi `this` = `Entity*`.
- Types de PS lus dans le désassemblage :

  | Vue | Type | Classe moteur |
  |---|---|---|
  | `PSInventory` | `0x1f` | `gCInventory_PS` |
  | `PSCombatSystem` | `0x60` | `gCCombatSystem_PS` |
  | `PSRoutine` | `0x24` | `gCScriptRoutine_PS` (`AIMode` à `+0xC0`, `LastAIMode` à `+0xD0`) |
  | `PSDamage` | `0x29` | `gCDamage_PS` (sur l’arme) |
  | `PSRigidBody` | `0xd` | `eCRigidBody_PS` |
  | `PSCharacterMovement` | `0x15` | `gCCharacterMovement_PS` |
  | `PSNavigation` | `0x5` | `gCNavigation_PS` |

  La spec mentionnait aussi `0xe`. La sonde journalise sa présence sans l’utiliser.

### Q5 — arme
- `PSInventory::GetRightHoldStack() → int` (−1 si pas d’inventaire). `GetRightHoldItem()` → `Entity` (sret en RDX). `GetTemplate(int pile)` → `Template` (sret en RDX, pile en R8, à détruire par `??1Template`).
- `CreateItems(const Template&, int nombre, gEStackType)` → `int` (index de pile, −1 en cas d’échec). Le `Template` est passé par référence : il reste à l’appelant. Script_Game.dll passe toujours `gEStackType` = 0.
- `HoldRightStack(int) → bool` et `EquipStack(int) → bool`.
- `PSCombatSystem::GetCombatMode() → gECombatMode`, rendu en EAX. Il n’y a pas de setter : le mode découle de l’objet tenu.
- Nom d’une entité : `Entity::GetName()` → `bCString` (sret, détruit par l’appelant).

### Q6 — collisions
- `Entity::GetCollisionGroup / SetCollisionGroup(eECollisionGroup)` et `Entity::EnableCollision(bool)` (variante à un argument).
- `PSRigidBody::AddBodyFlag(flag)` et `RemoveBodyFlag` : `eCRigidBody_PS::AddBodyFlag` fait `or [ps+0x90], flag`, puis recopie dans le tampon d’état physique. **0x81 en un seul appel** équivaut donc à 0x80 puis 0x1.
- `PSRigidBody::SetPose(bCMatrix)` : la matrice passée par valeur est un pointeur vers la copie de l’appelant. L’appel passe par la vtable du PS (`+0x2d0`).
- `PSCharacterMovement::EnableGravitation(bool)`, `IsGravitationEnabled()` et `SetGoalPose(const bCMatrix&)`.
- `PSNavigation::SetDCCEnabled(bool)` et `GetDCCRadius() → float`. Il n’existe pas d’accesseur `IsDCCEnabled`.
- Rappel : `Entity::MoveTo` = `SetToWorldMatrix`, puis `PSRigidBody::SetPose`, puis `PSNavigation::ResetAll` et `StopMovement`.

### Q3 — PNJ natifs
- `eCDynamicEntity::GetID() → const bCPropertyID*` (Engine.dll, `this` = `eCDynamicEntity*`). La sonde copie les 0x14 octets : les 16 premiers forment le GUID.
- `gCRuntimeInfo::GetEntity(const bCPropertyID&) → eCEntity*` (Game.dll) : `this` n’est pas lu, la sonde passe nul.
- `PSSkills::GetHitPoints`, `GetHitPointsMax → int`. `SetHitPoints(int, gESkillModifier)` saute vers `SetBase(compétence 0, valeur, mod)`. Valeurs du modificateur : 0 ajout, 1 max, 2 fixer.
- `PSDamage::DoDamage(Entity& cible, Entity& agresseur) → bool`, avec `this` = l’arme (PS `0x29`). Elle appelle `gCDamage_PS::DoDamage(eCEntity*, eCEntity*)`. Les dégâts valent `(PS+0x40 + PS+0x44) × PS+0x48`. `gCDamage_PS::SetDamageAmount(const long&)` et `GetDamageAmount() → const long&` agissent sur le PS de l’arme. La sonde rétablit la valeur d’origine après le coup.
- Hook `?OnProcess@gCScriptRoutine_PS@@UEAAXXZ` (Game.dll) : `this` = le PS `0x24`.

### Q7 — mort
- Script_Game.dll : `?Kill@@YA_JPEAVgCScriptProcessingUnit@@PEAX1_J@Z` et `?Defeat@@…`, de même signature `(gCScriptProcessingUnit*, Entity* self, Entity* other, __int64)`.
  - Les deux `void*` sont des **`Entity*`** (recopiés par `Entity::operator=`). Un pointeur nul est remplacé par `GetSelfEntity` ou `GetOtherEntity` de l’unité.
  - **La victime est `other`** (3ᵉ argument) : `Kill` lui applique `SetHitPoints(0, fixer)`, puis `AIMode = 12`. `Defeat` met `AIMode = 11`.
  - Le détour lit `*(void**)other` et le compare au héros publié par la frame. Un `Kill` avec `other` nul (cas observé au 1ᵉʳ test) : la victime est lue par `gCScriptProcessingUnit::GetOtherEntity(spu)` (Game.dll, accesseur : proxy à `+0x150` → `eCEntityProxy::GetEntity`), appelé depuis le détour sur le même thread.
- Script_Game.dll est résolue avec `GetModuleHandle` seulement : la sonde ne la charge jamais elle-même (son initialisation reste à l’initiative du jeu). Si elle n’est pas encore chargée quand `ScriptInit` s’exécute, l’expérience Q7 est inactive et le log le dit. Sur le banc Wine, elle est absente : l’expérience Q7 est inactive (testé par `run_scenarios.sh`).
- Relève : `PSRoutine::PropertyLastAIMode` (lecture `??B`), `PropertyAIMode::operator=(const gEAIMode&)`, puis `PSRoutine::SetState(bCString)`. Le `bCString` est passé par valeur : l’appelé le détruit (il saute vers `~bCString` en fin de fonction). Viennent ensuite `PSNavigation::SetDCCEnabled` et `eCAnimation_PS::SetRagDollEnabled(false)`.
- Sans interception, le menu de chargement s’ouvre au bout de 10 s environ (`ZS_Dead_LoopPlayer`).

### AD-2
Tous les appels au jeu se font depuis `OnProcess`, avec une seule exception : le détour de `Kill` appelle `Defeat` avec les mêmes arguments, sur le même thread. Le log indique le thread du détour. Le détour de `gCScriptRoutine_PS::OnProcess` ne fait que comparer un pointeur atomique.

## Protocole de test en jeu

Prérequis : `probe = 1` dans `multiplayer.ini`, overlay Steam désactivé, **Verr. Num actif**. Charger une partie près de quelques PNJ, de préférence un camp de monstres de la zone de départ. Les touches ne réagissent que si le jeu a le focus.

1. **Vérifier l’installation.** Le log doit contenir `[hooks] J2 : hooks Kill/Routine posés`. Sinon, il contient la cause : expérience Q7 ou Num4 inactive.
2. **F6**, puis **F7** : copie du héros en mode miroir (comme dans le spike J1).
3. **Q5, arme.** Sortir l’épée (héros en mode combat), puis **Num1**.
   - Observer : la copie a-t-elle l’épée en main ? Prend-elle une posture de combat ?
   - Lignes à rapporter : `J2 Num1 : héros …`, `copie PS 0x1f=… PS 0x60=…`, `CreateItems … -> index`, `HoldRightStack … mode combat avant/après`, `1 s plus tard …`.
   - Appuyer de nouveau sur **Num1** (`EquipStack`, arme rangée) et observer si l’arme passe dans le dos. Rapporter les mêmes lignes.
   - Frapper quelques coups : le miroir rejoue-t-il l’attaque avec l’arme en main ?
4. **Q6, collisions.** Appuyer sur **Num2** pour passer au mode suivant. À chaque mode :
   - marcher dans la copie avec le héros : la copie le repousse-t-elle ou est-elle traversée ?
   - observer si la copie flotte, tombe, tremble ou suit bien le héros (saut compris) ;
   - si possible, attirer un PNJ hostile vers la copie : la touche-t-il ?
   - rapporter les lignes `J2 Num2 avant/après` (groupe, PS, gravité, rayon DCC) et le résumé par seconde `J2 Num2 : mode …`.

   Modes : 1 = groupe NPC + collision + DCC (placement `MoveTo`) ; 2 = 1 + corps cinématique sans gravité (`SetPose` + `PSRigidBody::SetPose`) ; 3 = 1 + `SetGoalPose`, avec `MoveTo` si l’écart dépasse 150 cm ; retour à 0.
5. **Q3, PNJ.** S’approcher à moins de 15 m d’un PNJ et appuyer sur **Num3**.
   - Rapporter la ligne `J2 Num3 : PNJ …` : nom, GUID, PV et aller-retour `GetEntity`.
   - Si possible, refaire Num3 après un rechargement de la même sauvegarde : le GUID est-il identique ?
6. **Num4** (après Num3) : IA figée. Le PNJ s’arrête-t-il, et reste-t-il animé ou figé en pose ?
   - Rapporter les lignes `J2 Num4 t+…`, qui donnent la pose, l’AIMode et le nombre d’`OnProcess` bloqués.
   - Attaquer ce PNJ pendant qu’il est figé : réagit-il ?
   - **Num4** de nouveau pour le libérer.
7. **Num5** (après Num3, **IA libérée** par Num4 : un PNJ figé ne réagit pas et ne meurt pas normalement) : dégâts. Le repli `SetHitPoints` ne s'applique que si `DoDamage` n'a pas porté. Avec une arme en main (celle du héros, ou celle de la copie après Num1), rapporter la ligne `DoDamage …`, puis celle du `repli SetHitPoints`.
   - Observer la jauge de PV du PNJ.
   - Le PNJ devient-il hostile ? Envers qui ?
   - Répéter jusqu’à la mort du PNJ : meurt-il normalement ?
8. **Q7, mort.**
   - **Num6** (appui court) : `interception … ACTIVÉE`. Se faire tuer (par un monstre, ou par une chute).
     - Attendu : le héros tombe à terre sans écran de chargement, et le log contient `J2 Num6 : Kill vus=… morts du héros interceptées=…`.
     - Observer : le héros se relève-t-il seul ? Les ennemis s’en vont-ils ? Le menu de chargement apparaît-il quand même ?
   - Appui court pour désactiver. Mourir de nouveau, et **avant** le menu de chargement (10 s), **tenir Num6 1 s**.
     - Rapporter les lignes `J2 Num6 relève 1/6 … 6/6` et `relève : après`.
     - Observer : le héros se relève-t-il ? Le contrôle revient-il ? Le menu de chargement s’ouvre-t-il quand même ?
   - Essayer aussi la relève après un `Defeat` (mort interceptée).

Coller ensuite tout `Script_Multiplayer.log` (lignes `[probe] J2` et `[hooks] J2`) pour analyse.

## Réponses (à remplir après le test)

| Question | Expérience | Résultat observé | Décision pour l’Epic 3 |
|---|---|---|---|
| Q5 — arme en main | Num1 (`CreateItems` + `HoldRightStack`) | **OK** (1ᵉʳ test, 2026-10-20). Il faut que le héros ait l’arme sortie, sinon : « main du héros vide ». `CreateItems` → pile 2, `HoldRightStack` → true, `It_1H_Sword` en main de la copie 1 s plus tard. | Retenu pour 3.1/3.2 : créer l’arme de l’invité dans l’inventaire de l’avatar et la tenir en main. |
| Q5 — mode combat | Num1 (`GetCombatMode` avant/après, +1 s) | `GetCombatMode` vaut **0 partout**, y compris pour le héros arme sortie. Les animations portent la posture dans leur nom (`Hero_Stand_None_1H_…`) et le miroir par nom les rejoue avec l’arme en main. | Pas de « mode combat » à piloter : arme en main + rejeu par nom suffisent. `GetCombatMode` n’est pas un indicateur fiable. |
| Q5 — arme rangée | Num1 bis (`EquipStack`) | 1ᵉʳ `EquipStack` → true, pile en main = −1. Après la mort de la copie, `EquipStack` → false à chaque appui (l’arme a été perdue à la mort). | À revoir si besoin en 3.2 (arme rangée transmise par le rejeu ?). |
| Q6 — mode 1 (groupe NPC, DCC) | Num2 | Le groupe de la copie vaut **déjà 4** avant tout réglage ; les PNJ (Nautilus, Vulture) rendent **0**. La correspondance « NPC = 4 » n’est donc pas confirmée. Héros bloqué par la copie : non observé. | À compléter (2ᵉ test). |
| Q6 — mode 2 (cinématique, sans gravité) | Num2 | 0 échec de placement. Peu observé en mouvement. | À compléter (2ᵉ test). |
| Q6 — mode 3 (`SetGoalPose`) | Num2 | La copie monte sur un rocher (vraie capsule de déplacement), mais **glisse** au lieu de marcher : jusqu’à 700 animations moteur bloquées/s (son contrôleur veut animer la marche, le miroir bloque). En course, l’écart atteint 150 cm et déclenche des recalages `MoveTo` (1 à 3/s). | Prometteur pour la solidité, mais l’animation de locomotion doit venir du contrôleur ou être réconciliée avec le rejeu. À trancher. |
| Q6 — PNJ qui frappent la copie | observation | **Oui** : la copie est attaquée, reçoit des coups et joue l’animation de coup ; elle a fini par mourir. | La copie est déjà une cible valide pour les PNJ. Il faut la rendre **invulnérable** chez l’hôte (AD-9) et convertir les coups en `HitOnGuest`. |
| Q3 — identifiant stable (GUID) | Num3, avant/après rechargement | **Stable** (2ᵉ et 3ᵉ tests) : les Vultures `c3624668…` et `f0ac4ff5…` gardent leur GUID d’une session et d’un rechargement à l’autre ; seul le pointeur change. | Retenu : GUID de 16 octets comme clé d’appariement des PNJ placés (3.3). Les PNJ créés en cours de partie restent à vérifier. |
| Q3 — aller-retour `GetEntity(GUID)` | Num3 | **OK** à chaque fois (« == entité »), sur 3 PNJ. | Retenu pour l’appariement des PNJ (3.3). |
| Q3 — IA figée par PNJ | Num4 | **OK** : position fixe, ~200 `OnProcess` bloqués par 0,5 s ; le PNJ repart dès la libération. AIMode reste 10 (attaque). | Retenu pour neutraliser l’IA des répliques (3.3). |
| Q3 — dégâts côté hôte | Num5 (`DoDamage`, puis `SetHitPoints`) | **OK** : `DoDamage(épée, Vulture, copie)` → true, 10 PV en moins à chaque appui (150 → 60), y compris IA figée. Agresseur = copie ou héros. | Retenu pour `HitRequest` (3.4). |
| Q7 — interception `Kill → Defeat` | Num6 court | 4ᵉ test : `Defeat` remplace bien `Kill` (aucun `Kill` imbriqué), mais le héros finit quand même dans la boucle de mort et le menu. Cause trouvée au désassemblage : sur un coup mortel, `DownOrDeadByMeleeWeapon` (Script_Game) appelle `Kill`, puis met l’état `ZS_RagDollDead` (sinon `ZS_Unconscious` pour un coup qui assomme). Le menu (pages 13 à 15) n’est ouvert que par `ZS_Dead_LoopPlayer`, et `ContinueRoutine_Base` envoie en `ZS_Dead` tout personnage dont l’AIMode vaut 12. Correctif : à la frame qui suit l’interception, la sonde remet le héros en `ZS_Unconscious`. Message de perte d’XP observé, origine non identifiée. 5ᵉ et 6ᵉ tests : forcer `ZS_Unconscious` après coup donne un héros en T-pose, qui passe sous la carte tant que le ragdoll est actif, et reste figé au-dessus du sol une fois le ragdoll coupé (l’animation « à terre » redémarre ~350 fois/s). Nouvelle approche : intercepter **avant** la décision, dans `DownOrDeadByMeleeWeapon`, `…Projectile` et `…Inferno` (self = ce qui frappe, other = victime). La fonction rend 0 sans rien faire si la victime a des PV : le détour remet le héros à 1 PV et rend 0. 7ᵉ test : **OK**, le héros reste debout ; 10 coups mortels de mêlée sur 10 annulés, aucun `Kill`. | `Kill → Defeat` écarté. **Retenu** : annuler le coup mortel de l’invité ; le mod gère ensuite « à terre » puis la réapparition (AD-11, J5). |
| Q7 — relève après mort native | Num6 tenu 1 s | 3ᵉ test : PV au max, « mort=non », mais le personnage est **invisible**, le corps reste au sol en ragdoll alors qu’il peut équiper ses armes. Relever un mort à la main n’est pas fiable. | Écarté : l’invité ne doit jamais mourir nativement, il faut l’intercepter avant (`Kill → Defeat`). |
| AD-2 — thread du détour `Kill` | log `thread du détour` | Thread 328 = celui d’`OnProcess`. | L’exception AD-2 (Defeat depuis le détour) est sans risque de thread. |
| Q7 — la mort du héros passe-t-elle par `Kill` ? | `Kill vus` après une mort (monstre, chute) | 1 seul `Kill` vu, au moment de la **mort de la copie** : la mort d’un personnage passe bien par `Kill`. Pas de ragdoll sur la copie (bloqué par le miroir), arme perdue, son de mort. | Confirmé pour un personnage ; à confirmer pour le héros. |
| Q7 — `Kill` sans victime explicite | `sans victime explicite=` | **1 sur 1** : ce `Kill` avait `other` nul. La victime se lit alors par `gCScriptProcessingUnit::GetOtherEntity` (Game.dll, accesseur trivial : proxy à `+0x150` → `eCEntityProxy::GetEntity`). | La sonde doit résoudre la victime par ce getter, sinon la mort du héros risque de ne jamais être interceptée. |
| Q6 — groupe de collision d’un vrai PNJ (vérifie « NPC = 4 ») | Num3 `groupe de collision=` | 0 pour Nautilus et Vulture. | Hypothèse « NPC = 4 » non vérifiée. |

Indicateur à l’écran (depuis le 3ᵉ test) : la sonde écrit dans le journal à l’écran du jeu (`GUI2::PrintGameLogF`, donnée `gui2` de Script.dll). Num0 affiche l’aide et l’état des modes.

Divers (1ᵉʳ test) : aucun crash, 150 à 400 images/s, aucun échec SEH. La course de la copie reste lente : c’est attendu, le miroir de la sonde ne rejoue pas les réglages de locomotion de la story 2.4b (seule la session les rejoue).

## Conclusions pour l’Epic 3 (2026-10-20, après 7 tests en jeu)

- **Q5, arme et mode combat** : on crée l’arme de l’invité dans l’inventaire de son avatar (`CreateItems`), puis on la tient en main (`HoldRightStack`). Il n’y a pas de mode combat à piloter : la posture est dans le nom des animations, que le rejeu par nom reproduit déjà. Sert aux stories 3.1 et 3.2.
- **Q6, avatar solide** : le mode 2 (corps cinématique `AddBodyFlag(0x81)`, sans gravité, placé par `SetPose` + `PSRigidBody::SetPose`) bloque le héros local. Le mode 3 (`SetGoalPose`) donne une vraie capsule de déplacement, mais l’avatar glisse : à écarter tant que la locomotion vient du rejeu. Les PNJ frappent déjà l’avatar, il faudra donc le rendre invulnérable chez l’hôte et convertir ses coups reçus en `HitOnGuest` (AD-9). Le chemin « coup mortel annulé » ci-dessous le permet.
- **Q3, PNJ natifs** :
  - Appariement : le GUID de 16 octets est stable d’un rechargement à l’autre, et `gCRuntimeInfo::GetEntity` retrouve l’entité à partir de lui.
  - IA : elle se fige PNJ par PNJ, par hook de `gCScriptRoutine_PS::OnProcess`.
  - Dégâts : `PSDamage::DoDamage(arme, cible, agresseur)` fonctionne côté hôte.
  - Sert aux stories 3.3 et 3.4.
- **Q7, mort de l’invité** :
  - Intercepter avant la décision, dans `DownOrDeadByMeleeWeapon`, `…Projectile` et `…Inferno` (victime = `other`) : l’invité reste à 1 PV, sans `Kill`, ragdoll ni menu.
  - Le mod gère ensuite « à terre » et la réapparition (AD-11, J5).
  - À écarter : `Kill → Defeat`, l’état `ZS_Unconscious` forcé, la relève d’un mort.
  - Les morts par chute ou noyade restent à couvrir.
- **AD-2** : les détours `Kill` et `DownOrDeadBy*` tournent sur le thread d’`OnProcess`. Pour cette raison, la remise à 1 PV depuis le détour est une exception sûre, à inscrire dans la spine.
