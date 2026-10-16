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
