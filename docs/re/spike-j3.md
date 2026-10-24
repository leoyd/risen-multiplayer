# Spike J3 : dialogues (story 4.0)

But : savoir comment Risen lance, joue et termine un dialogue, et comment rejouer une réplique
(voix, lèvres, geste, sous-titre) chez l'autre joueur sans ouvrir de dialogue ni bloquer son héros.

## 1. Ce que fait G3 Together (Gothic 3, même moteur Genome)

Étude du code source (`github.com/GGM1k3y23/g3-together`) et du SDK Gothic 3
(`github.com/georgeto/gothic3sdk`), le 2026-10-24.

- **Détection d'une réplique** : détour de `gCScriptProcessingUnit::StartOutput(bCString const& sample)`
  (par adresse chez eux). Toutes les répliques (`_AI_Say`, `_AI_Say_Ex`, `StartSay`, `StartSayEx`) y
  passent, une fois chacune. `sample` est le nom du fichier voix (`INFO_<voix>_<PNJ>_<textId>_<langue>`).
  Locuteur = `spu->GetSelfEntity()`. Fin de réplique : `gCScriptRoutine_PS::GetSPU().IsOutputPlaying()`
  interrogé à chaque tick (grâce de 300 ms). Début et fin du dialogue : polling de
  `gCInfoManager_PS::AccessIsRunning()` et `GetDialogPartnerNPC()`.
- **Rejeu chez l'observateur** : voix en 3D seulement (`eCAudioModule::PlayStream3D`, canal voix, fondu
  de 20 à 25 m) et sous-titre (`GUI2::StartSay` / `StopSay`). **Ni animation, ni lèvres, ni caméra,
  ni état `ZS_Talk`** : l'observateur reste libre. Réplique ignorée si l'observateur est lui-même en
  dialogue ou à plus de 20 m.
- **PNJ** : le dialogue tourne chez celui qui parle, même face à un PNJ miroir ; ce PNJ y est libéré
  pour jouer `ZS_Talk`. Chez l'hôte, le PNJ est « tenu » (tâche d'attente, IA bloquée) jusqu'à la fin.
- **Histoire** : synchronisation séparée (quêtes, infos données, événements), hôte qui fait autorité ;
  les effets des commandes de dialogue n'existent que chez celui qui parle (relève de J4 chez nous).
- **Pièges** : tailles réelles de `eCAudioSound` / `eCAudioChannel` plus grandes que dans le SDK
  (corruption du tas) ; `GUI2::StopSay` ferme aussi le sous-titre du dialogue local ; valider le nom
  de fichier voix reçu (pas de chemin) ; `AssessTalk` refuse parfois (combat).

## 2. Ce qui existe dans Risen (exports, Steam x64 de janvier 2023)

- **Système de dialogue** : `gCInfoSystem` (Game.dll) : `IsRunning`, `GetDialogPartnerNPC`,
  `GetDialogPartner1/2`, `GetCurrentSelf/Other`, `OnInfoStarted`, `OnInfoFinished`, `EndInfoSystem`,
  `ShowDialogManager`, `HideDialogManager`. Façade script `InfoSystem` (Script.dll) :
  `IsRunning`, `GetCurrentCommand`, `GetCurrentCommandSay`, `GetCurrentCommandType`.
- **Commandes** : contrairement à Gothic 3, Risen a des classes `gCInfoCommand*` (Game.dll) :
  `gCInfoCommandSay` (`Execute`, `GetSpeaker`, `GetListener`, `GetText`, `GetGesture`), `SaySVM`,
  `Give`, `RunScript`, `Attack`, `Teach`, `DoTrade`… `gCInfoCommandSay::Execute` (`0x180892f90`) ne
  joue pas la voix : il fixe les rôles de caméra (`gCCameraAdmin::SetDialogRoles`) et vérifie le
  texte localisé (`eCLocString`, `eCLocAdmin2::ExistsID`).
- **Parole** :
  - `gCScriptProcessingUnit::StartOutput(bCString const&, gEOutputType)` (Game.dll, exporté : point de
    détection par nom, celui de G3 Together), `StopOutput`, `IsOutputPlaying` ;
  - statiques `gCScriptProcessingUnit::sStartOutput(eCGeometryEntity*, bCString const&, gEOutputType)`,
    `sStopOutput`, `sIsOutputPlaying` ;
  - `gCScriptRoutine_PS::StartOutput` / `IsOutputPlaying`, `PSRoutine::StartOutput(bCString, gEOutputType)` ;
  - `Entity::StartSay(Entity& auditeur, bCString texte, bool, bool)` = `StartSayEx(auditeur, texte,
    GetSampleTitle(texte), bool, bool)` ; `StartSayEx` (`0x18020a520`, Script.dll) appelle
    `PSRoutine::StartOutput(échantillon, type)` (voix), `Entity::StartFacialAni` (lèvres) et
    `gCHudSystem2::PrintGameLog` (texte à l'écran) ; nom de fichier par
    `gCSvmAdmin::FormatSampleFileName` / `FormatSampleTitle`.
  - `Entity::StartSaySVM`, `Entity::StartOutput`, `Entity::StopOutput`.
- **Sous-titres** : `GUI2::ShowSubtitle(bCUnicodeString const&)`, `gCHudSystem2::ShowSubtitle`,
  `GUI2::StartTalk(Entity, bCString)` / `StopTalk`.
- **Caméra** : `Entity::SetDialogRoles`, `Entity::StartDialogCamera` (à ne pas appeler chez l'observateur).
- **PNJ** : `PSDialog` (`IsTalking`, `IsBeingTalkedTo`, `BeginTalkingTo`, `EndTalkingTo`).

## 3. Hypothèses à trancher en jeu (sonde)

- **H1** : le détour de `gCScriptProcessingUnit::StartOutput` voit chaque réplique d'un dialogue
  (locuteur, échantillon, type), une fois chacune, héros compris.
- **H2** : `Entity::StartSay` (ou `sStartOutput` + `StartFacialAni`) sur un PNJ hors dialogue joue la
  voix et les lèvres sans ouvrir le dialogue ni bloquer le héros.
- **H3** : idem sur un PNJ dont la routine est figée (comme une réplique) : la voix passe-t-elle
  malgré le gel de `gCScriptRoutine_PS::OnProcess` ?
- **H4** : idem sur l'avatar (entité créée par le mod, `PlayMotion` bloqué) : voix et lèvres ?
- **H5** : identifiant de texte → sous-titre (`eCLocString`), affichable par `GUI2::ShowSubtitle`.

## 4. Plan de la sonde (`probe = 1`)

- Passif : journal de chaque `StartOutput` (locuteur, échantillon, type) et des transitions de
  `gCInfoSystem::IsRunning` / `GetDialogPartnerNPC`, plus la commande courante (`InfoSystem`).
- **Num7** : rejoue la dernière réplique capturée sur le PNJ le plus proche hors dialogue (voix, lèvres,
  texte) — H2 ; avec la routine figée (Num4 du spike J2) — H3.
- **Num8** : rejoue la dernière réplique du héros sur la copie (Num1 du spike J2), entité créée par le
  mod comme l'avatar — H4.
- **Num9** : affiche le sous-titre de la dernière réplique — H5.

Résultats : section 5, à remplir après le test.
