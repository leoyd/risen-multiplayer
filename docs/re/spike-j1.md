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
