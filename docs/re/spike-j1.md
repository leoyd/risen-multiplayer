# Spike J1 — rétro-ingénierie (build Steam x64, janvier 2023)

Méthode : exports décorés (`objdump -p`). REA/Hopper n’a pas démarré (bridge timeout).

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

## Prochaine étape : DLL sonde en jeu
Hook `OnProcess` (thread, fps, état de session, pose du joueur, une fois par seconde) + hook `PlayMotion` (dump du type et des premiers octets du descripteur pour le joueur) + touche de debug qui fait apparaître une copie du joueur et lui fait rejouer la dernière animation capturée.
