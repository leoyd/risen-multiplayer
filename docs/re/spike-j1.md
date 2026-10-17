# Spike J1 — rétro-ingénierie (build Steam x64, janvier 2023)

Méthode : exports décorés et désassemblage (`objdump -p`, `objdump -d`). REA/Hopper n’a pas démarré (bridge timeout, de nouveau le 2026-10-16).

## Q4 — Boucle par frame (AD-2)
- `?OnProcess@eCApplication@@UEAAXXZ` (Engine.dll), virtuelle, sans argument : c’est la candidate au hook par frame.
- **Reste à confirmer en jeu** : appelée une fois par frame, sur le thread principal.

## Q1 — Animation courante (AD-8)
- Aucun export « animation en cours ». `Entity::GetAni*` **construit** un nom à partir de (action, direction, posture, état).
- **Piste principale : intercepter la lecture.** `?PlayMotion@eCAnimation_PS@@QEAAXW4eEMotionType@eAnimShared@@PEAUeSMotionPlayBackDesc@3@_N@Z` (Engine.dll) est la fonction centrale de lecture d’une animation. On la hooke, on filtre sur l’`eCAnimation_PS` du joueur local, et on capture `(eEMotionType, eSMotionPlayBackDesc)`. Il reste à établir la disposition de `eSMotionPlayBackDesc`, qui contient la ressource de motion, donc son nom.
- Autres exports utiles :
  - `PSAnimation::PlayBodyAni(bCString, bool)` ;
  - `PSAnimation::GetAniPlayTime` et `GetAniMaxTime(eEMotionType)` ;
  - `eCWrapper_emfx3ActorInstance::PlayMotion` (niveau EMotionFX 3) ;
  - `eCAnimation_PS::GetLastAnimationVariation`.
- Repli (AD-8 b) : `Entity::GetMovementMode`, `GetCurrentAniDirection`, `IsRunning`, `IsSprinting`, `IsJumping`, etc. sont exportés.

## Q2 — Avatar (`Entity::Spawn`) et monde chargé (AD-11)
- `Entity::GetTemplate() → Template` et `Entity::Spawn(Template, bCMatrix const&, bool) → Entity` : piste « avatar = template du joueur ». Reste à tester en jeu : comportement d’une copie du héros, et contrôle du joueur pris ou non.
- `Entity::GetPlayer()` / `GetOriginalPlayer()` (statiques) ; `gCSession::AccessState()` → `gESession_State`. Pistes pour détecter qu’un monde est chargé.
- Trouvaille : `Entity::GetNetIndex() → u32` existe (reliquat réseau du moteur ?). À explorer comme identifiant stable d’entité pour le NetId et l’appariement des PNJ (Q3).

## Faits confirmés au désassembleur (2026-10-16)
- `OnProcess` n’est pas redéfini par `Risen.exe` (qui l’importe d’Engine.dll) : hooker le corps couvre tous les appelants.
- `eCAnimation_PS::PlayMotion` : `RCX=this, EDX=type (0..7, ignoré au-delà), R8=desc, R9B=bool`. La ressource de motion n’est **pas** dans le descripteur : elle est lue dans le PS, à `this + 0x120 + type * 0x10`, puis passée à `eCWrapper_emfx3ActorInstance::PlayMotion` (`this + 0x30`) avec le descripteur. Le nom de l’animation est donc à chercher du côté de cette ressource (`eCMotionResource2`).
- `eCEntity::GetPropertySet(type)` const (`QEBA…`) est un simple saut vers la vtable (`+0x198`) : c’est la variante appelée par la sonde.
- `Entity` (Script.dll) contient le `eCDynamicEntity*` rendu par `gCSession::GetPlayer`. `GetPose` rend l’identité si l’entité n’est pas géométrique.
- `Entity::Spawn(Template, bCMatrix const&, bool)` : `CreateDynamicEntity` + `CreateFromTemplate` + `SetToWorldMatrix`, détruit le `eCTemplateEntityProxy` reçu dans tous les cas ; le `bool` appelle `eCEntity::SetTemporary` (entité non sauvegardée).

## Sonde en jeu : quoi rapporter
Activer `probe = 1` (voir `client/README.md`), puis fournir `Script_Multiplayer.log`. Les lignes `[probe]` répondent à :
- **Q4** : `tick … thread=… (identique au 1er appel)` et `images/s` cohérent avec l’affichage ⇒ un appel par frame sur un seul thread.
- **Monde chargé** : passage de `joueur=non` à `joueur=oui` au chargement d’une partie (et retour au menu).
- **Pose** : la matrice brute du `tick` permet de vérifier où sont la translation (`m[12..14]` ?) et l’axe avant, et les unités (cm ?).
- **Q1** : lignes `PlayMotion` (type, ressource, 0x60 octets du descripteur) en marchant, courant, sautant, frappant.
- **Q2** : résultats de F6 (copie, pose, « joueur inchangé » ou non) et de F7 (la copie rejoue-t-elle l’animation ?).

## Prochaine étape : DLL sonde en jeu
Hook `OnProcess` (thread, fps, état de session, pose du joueur, une fois par seconde) + hook `PlayMotion` (dump du type et des premiers octets du descripteur pour le joueur) + touche de debug qui fait apparaître une copie du joueur et lui fait rejouer la dernière animation capturée.

## Résultats de la sonde en jeu (2026-10-16, Proton)
- **Q4 — boucle par frame : confirmé.** `OnProcess` est appelé sur un seul thread (le même du début à la fin). En partie : 160–250 appels/s, cohérent avec l’affichage. Au menu ou en pause : 2 000–2 800/s (boucle non bridée). `OnProcess` est donc un tick par itération de la boucle principale. Le pas de temps devra venir d’une horloge, pas d’un compte d’appels.
- **Monde chargé : confirmé.** `Entity::GetPlayer` rend nul au menu principal, puis un pointeur stable après le chargement. Le retour au menu n’a pas encore été testé.
- **Pose : confirmé.** `bCMatrix` est en lignes ; la translation est en `m[12..14]` ; Y est vertical (ligne 1 = 0 1 0) ; l’unité est le centimètre (course ≈ 3–4 m/s, saut ≈ +100 en Y). L’axe avant en ligne 2 (`m[8..10]`) reste à confirmer visuellement (position de la copie par rapport au héros).
- **Q2 — copie du joueur : partiel.** `Entity::Spawn(template du joueur, pose, true)` crée bien une entité visible, à la pose demandée, et le joueur ne change pas. La copie reste immobile.
- **Q1 — animation courante : le descripteur ne suffit pas.**
  - `PlayMotion(type)` joue la ressource déjà rangée dans l’emplacement `type` du PS (`this + 0x120 + type*0x10`).
  - Ce choix de ressource se fait avant l’appel, via `eCAnimation_PS::SetMotion(eEMotionType, eCMotionResource2*)`.
  - Le F7 du premier essai a rejoué `type 0` sur la copie avec **sa propre** ressource (`0x7817d70`, identique à celle du joueur : l’animation d’attente qui revient toutes les ≈ 2,6 s). D’où aucun effet visible.
  - Les pointeurs de ressource sont partagés et stables (cache) : un même mouvement donne toujours le même pointeur.
  - Types observés : 0 et 1 (locomotion, en paires, descripteur sur la pile) ; 4 (attaque, descripteur dans l’entité) ; 5 (une seule fois).
- **Piste de synchro par nom (AD-8 a), désormais concrète :**
  - émetteur : ressource de l’emplacement → `eCResource2::GetFileTitleA()` (ou `GetFileName`) → nom, interné une fois par pointeur ;
  - récepteur : `eCResource2::GetMotion(bCString)` → ressource → `eCAnimation_PS::SetMotion(type, res)` → `PlayMotion(type, desc, flag)`.
  - Alternative haut niveau : `Entity::StartPlayAni(bCString, int, bool, float, bool, eEMotionType)` (Script.dll).
  - À vérifier : `eCMotionResource2` hérite-t-il de `eCResource2` sans décalage de pointeur ?

## Sonde v2 en jeu (2026-10-16)
- **Noms d’animation : confirmé.** `eCResource2::GetFileTitleA()` sur la ressource de l’emplacement donne des noms stables et lisibles, par exemple `Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400`, `Hero_Stand_None_1H_S1_Attack_00_N_Fwd_00_%_00_P0_0`, `Hero_Stand_None_None_P0_Jump_Run_N_Fwd_00_%_00_P0_0`. La synchro par nom (AD-8 a) est viable côté émetteur.
- **Types de motion :** 0 et 1 (locomotion en paires avant/côté, avec poids) ; les attaques sont de **type 0** ; le type 4 est la parade au bouclier (`Hero_Stand_Shield_1H_P0_Hold_Begin_O_…`).
- **Axe avant : confirmé.** La copie apparaît devant le héros, de dos (même orientation que lui) : la ligne 2 (`m[8..10]`) est l’axe avant.
- **La copie reste figée,** même après `SetMotion` (emplacement 4 vide → ressource du joueur) puis `PlayMotion`. Hypothèses :
  - (a) la copie n’est pas dans la zone de traitement du moteur (`Entity::IsProcessingRangeEntered`) ;
  - (b) le héros copié n’a pas de logique d’animation (aucun `PlayMotion` du moteur sur son PS) ;
  - (c) le rejeu brut ne suffit pas.

  La sonde v3 mesure (a) et (b), et F8 rejoue la même animation sur le PNJ le plus proche pour tester (c).

## Sonde v3 en jeu (2026-10-16)
- **La copie est vivante** : elle est dans la zone de traitement (`IsProcessingRangeEntered` = oui), le moteur lui lance environ 1 `PlayMotion`/s (attente, quarts de tour), et elle tombe au sol (physique active).
- **Le rejeu fonctionne** : F7 a joué une animation visible sur la copie. Le mauvais geste venait du choix du clip : le filtre « Attack » a retenu `AttackStumbleRight` / `AttackRecover1`, des fins de coup. Le moteur remet ensuite la copie en attente au bout d’environ 1 s.
- **Conséquence pour l’avatar (AD-8)** : rejouer **chaque** `PlayMotion` du joueur (nom → ressource → `SetMotion` + `PlayMotion`) et **bloquer** dans le hook ceux que le moteur lance de lui-même sur l’avatar. La position se pose avec `Entity::SetPose` (Script.dll, simple `SetToWorldMatrix`). La sonde v4 teste exactement ce schéma en local (mode miroir, F7).
- F8 déclenche la sauvegarde automatique de Risen : la touche passe à F4.

## Sonde v4 en jeu : mode miroir (2026-10-16)
- **Validé : la copie reproduit en direct les animations et les déplacements du joueur, de façon fluide.** Le schéma de l’avatar J1 (AD-8 a, synchro par nom) fonctionne en local :
  - rejeu de chaque `PlayMotion` du joueur : `SetMotion` puis `PlayMotion` ;
  - blocage dans le hook des `PlayMotion` que le moteur lance de lui-même sur l’avatar ;
  - pose imposée à chaque frame.
- **Défauts observés et pistes :**
  - **Saut** : le personnage décolle puis retombe aussitôt, et l’équipement porté dans le dos ne suit pas. `SetPose` ne fait que `SetToWorldMatrix` : le contrôleur de personnage et la physique restent au sol. `Entity::MoveTo(bCMatrix)` fait en plus le recalage des property sets 0xd et 5 ; la sonde v5 l’utilise.
  - **Légères saccades** : la pose est imposée sans interpolation. Ce sera le rôle de l’interpolation d’AD-7.
  - **Arme absente de la main** en combat : l’animation d’attaque est jouée, mais l’arme reste rangée. Sortir ou ranger l’arme est un état de combat ou d’inventaire (`PSCombatSystem`, `gECombatMode`), pas une animation. À synchroniser à part (J2).
  - **Traverse les objets et les PNJ** : attendu avec une pose imposée. Pour un avatar distant, c’est plutôt souhaitable (ses collisions sont résolues chez son propriétaire). En revanche, les PNJ doivent pouvoir toucher l’avatar en J2 (AD-9), à vérifier à ce moment-là.

## Sonde v5 en jeu (2026-10-16)
- **`Entity::MoveTo` confirmé :** en mode miroir, au saut, l’équipement porté dans le dos reste avec le personnage en l’air (constaté par l’utilisateur). Le placement d’avatar se fait donc par `MoveTo`.
- **Non testés à ce stade :**
  - le chemin récepteur par nom (`eCResource2::GetMotion`) ;
  - un descripteur reconstruit (le miroir rejoue le pointeur de ressource et le descripteur brut) ;
  - le retour au menu ;
  - le chargement rapide (AD-11).

## Story 2.1 — chemin par nom et descripteur (2026-10-17)

Méthode : désassemblage (`objdump -d`). REA/Hopper ne démarre toujours pas (pont, timeout).

- **`eCResource2::GetMotion(bCString const&)`** (`?GetMotion@eCResource2@@SAPEAVeCMotionResource2@@AEBVbCString@@@Z`,
  Engine.dll, `0x1800a06a0`) : statique, sans sret. Elle appelle la recherche du cache des
  ressources (type 3 = motion, `0x180684050` → `0x180681980`). Celle-ci normalise le nom, cherche
  l'entrée et la crée si besoin, puis appelle `QueryMotion` (vtable `+0x110`). Le pointeur rendu est
  **emprunté** : aucune référence n'est prise à l'aller, on ne le libère jamais, et `SetMotion`
  prend la sienne (`res + 0x24`).
- **`eCAnimation_PS::PlayMotion`** (`0x1804d4f10`) : lit l'emplacement `this + 0x120 + type*0x10`,
  rend la ressource prête (`0x180684100`), puis appelle `eCWrapper_emfx3ActorInstance::PlayMotion`
  (`0x1804e0770`, sur `this + 0x30`) avec la ressource, le descripteur et le `bool`.
- **Descripteur `eSMotionPlayBackDesc`** : le wrapper n'en lit **que les octets 0x00 à 0x1B**, qu'il
  recopie dans la structure de lecture EMotionFX (sur la pile) :

  | Offset | Type | Usage observé |
  |---|---|---|
  | `+0x00` | f32 | copié tel quel (sauf type 4 quand une parade est en cours) ; fondu d'entrée ? |
  | `+0x04` | f32 | copié tel quel (sauf type 4) ; fondu de sortie ? |
  | `+0x08` | u32 | retenu seulement s'il vaut 0 ou 1 ; mémorisé pour les types 0 et 4 |
  | `+0x0C` | f32 | copié tel quel ; poids ? |
  | `+0x10` | u32 | copié tel quel |
  | `+0x14` | f32 | copié tel quel ; poids ou vitesse ? |
  | `+0x18` | u32 | comparé à 2 (booléen) |

  Les octets `0x1C`–`0x5F` (pointeurs compris) ne sont pas lus sur ce chemin. Un descripteur
  **nul** donne la lecture par défaut. D'où la disposition de `Motion` dans `PROTOCOL.md` (v2) :
  les 7 scalaires, et un descripteur reconstruit à zéro autour d'eux (`game/motion_desc`).
- **Hook et threads (AD-2)** : la sonde journalise une fois le thread du hook `PlayMotion`, et
  indique si c'est celui d'`OnProcess`. Résultat à reporter ci‑dessous.

### Test en jeu de la story 2.1

*À compléter avec le log du test (miroir par nom : marche, course, saut, attaques, mort).*

- Rejeu par nom (`GetMotion` → `SetMotion` → `PlayMotion`, descripteur reconstruit) : à observer.
- `GetMotion(nom)` identique à la ressource capturée : à observer.
- Thread du hook `PlayMotion` : à observer.
- Scalaires observés par type d'animation, et champs constants : à relever.

