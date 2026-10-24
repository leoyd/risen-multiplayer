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

### Utilisation de la sonde

`multiplayer.ini` : `probe = 1`. Au démarrage, le log doit contenir `[hooks] J3 : hook StartOutput
posé (journal des répliques, Num7 à Num9)` ; sinon un Warn nomme le symbole manquant et seules les
sondes J1 et J2 restent. Verr. Num actif ; Num0 rappelle les touches. Lignes du log : `[probe] J3`.

1. **H1** : parler à un PNJ (plusieurs répliques, dont au moins une du héros), jusqu'à la fin du
   dialogue. Relever les lignes `dialogue : … — DÉBUT` / `— FIN` (partenaire, commande `Say (0x1b)`,
   `texte=« id »`), et pour chaque réplique `StartOutput : locuteur=… (HÉROS|autre) échantillon=« … »
   type=… texte Say=« … »` puis `fin de la sortie … après N ms`. H1 est confirmée si chaque
   réplique entendue a **une** ligne `StartOutput`, celles du héros comprises (`HÉROS`), et si
   `texte Say` correspond à la réplique (sinon, noter le décalage d'une frame). Noter aussi la
   ligne `thread du détour` (attendu : celui d'OnProcess).
2. **H2** : hors dialogue, près d'un autre PNJ, **Num7** : voix entendue ? lèvres qui bougent ?
   le héros reste-t-il libre (pas de caméra de dialogue) ? Lignes `sStartOutput … -> true`,
   `StartFacialAni … OK`, `sIsOutputPlaying à la 1re frame=oui` puis `fin de la sortie … après N
   ms`. Puis **Num7 deux fois** (< 2 s) : `StartSay … OK`, texte « nom : réplique » dans le journal
   à l'écran ? voix (celle du PNJ cible : le fichier peut manquer) ?
3. **H3** : **Num3** sur ce PNJ, **Num4** (IA figée), puis **Num7** (`IA figée (Num4)=oui`). Voix
   et lèvres ? Attendu possible : `sortie toujours en attente après 30000 ms` (désassemblage :
   `StartOutput` ne fait que ranger la ressource à `+0x188` de l'unité, la lecture vient du
   traitement de l'unité, peut-être bloqué avec `gCScriptRoutine_PS::OnProcess`). Num4 pour libérer.
4. **H4** : **F6** (copie), **F7** facultatif, puis **Num8** (dernière réplique du héros sur la
   copie) et Num8 deux fois. Relever `PS 0x24=` de la copie (sans lui, `sStartOutput` rend false),
   voix, lèvres.
5. **H5** : **Num9** après une réplique dite pendant un dialogue : `eCLocAdmin2::GetString(« id »)
   -> « texte »` (texte localisé lisible ?), `gCHudSystem2::ShowSubtitle … gCHudSystem2=0x…` : un
   sous-titre apparaît-il, combien de temps ? Puis **Num9 deux fois** : `GUI2::StartTalk … -> true`,
   la page de dialogue du HUD s'ouvre-t-elle avec le nom et le texte ? bloque-t-elle le héros ?
   Num9 pour la fermer (`StopTalk`).

Coller ensuite tout `Script_Multiplayer.log` (lignes `[probe] J3` et `[hooks] J3`).

Faits du désassemblage utiles à la lecture du log (Steam x64 de janvier 2023) :
- `StartSay(auditeur, texte, bool, bool)` calcule l'échantillon par `GetSampleTitle(texte)`
  (format `%s_%s_%s_%%_%s.wav`, lu comme texte, voix du PNJ (PS 0x1e), langue des voix, nom de
  l'entité : ordre à confirmer dans le log ; sans PS 0x1e, échantillon vide et StartSay ne fait rien), puis appelle
  `StartSayEx(auditeur, échantillon, texte, bool, bool)` (ordre corrigé par rapport au § 2), qui
  passe `échantillon + ".wav"` à `StartOutput` et à `StartFacialAni`. Type : 0 par défaut, 1 si
  l'auditeur est le joueur, 2 si le locuteur est le joueur, 3 si le 2ᵉ bool est vrai. Le 1ᵉʳ bool
  écrit « nom : texte localisé » dans le journal à l'écran.
- L'identifiant de texte ne se déduit donc pas sûrement de l'échantillon : la sonde le lit dans la
  commande `Say` courante d'`InfoSystem` (`InfComSay::GetTextID`) à la frame de la réplique.
- `GUI2::ShowSubtitle` (Script.dll) est un simple `ret` : la sonde utilise
  `gCHudSystem2::ShowSubtitle` (Game.dll). `InfoSystem::GetNPC` = `gCInfoSystem::GetDialogPartnerNPC`.

Résultats : section 5, à remplir après le test.

## 5. Résultats (test en jeu du 2026-10-24, une instance, sonde)

| Hypothèse | Résultat |
|---|---|
| **H1** détection | **Oui.** Chaque réplique passe une fois par le détour de `StartOutput`, héros compris (`INFO_<dialogue>_<n°>_<voix>_<langue>_%_<locuteur>.wav`, type 3 pour le héros), sur le thread d'`OnProcess`. Début et fin du dialogue par `InfoSystem` (partenaire, commande `Say` 0x1b avec l'identifiant de texte, 0x3d = aucune). Durée de chaque réplique par `sIsOutputPlaying`. Les commentaires ambiants des PNJ (hors dialogue) y passent aussi. |
| **H2** rejeu sur un PNJ libre | **Oui** : `sStartOutput(entité, échantillon capturé, type)` puis `StartFacialAni(échantillon)` : voix et lèvres, sans dialogue ni blocage du héros. `StartSay` sur un autre PNJ échoue (`retour=false`) : il recalcule l'échantillon avec la voix de ce PNJ, fichier inexistant. **Rejouer l'échantillon capturé, jamais `StartSay`.** |
| **H3** PNJ à l'IA figée | **Oui** : voix et lèvres malgré le gel de `gCScriptRoutine_PS::OnProcess` (réplique en session). |
| **H4** entité créée par le mod (copie F6, comme l'avatar) | **Oui** : voix et lèvres du héros sur la copie. |
| **H5** sous-titre | **Non** pour l'instant : `eCLocAdmin2::GetString(id)` rend une chaîne vide, et `gCHudSystem2::ShowSubtitle` n'affiche rien de visible. `StartSay` avec « afficher le texte » écrit le texte en jaune dans le journal à l'écran (`PrintGameLog`), mais pas à chaque fois. Piste : `eCLocString(id).GetString()` comme `StartSayEx`, puis le journal à l'écran. Bonus de FR-13 : reporté. |

Remarque de l'utilisateur : la sonde fait parler n'importe quel PNJ proche (voulu pour le test) ; le vrai
rejeu fera parler le PNJ locuteur lui-même, retrouvé par NetId/GUID.

## 6. Décisions pour J3 (reportées dans la spine, AD-13)

- **Détection, chez celui qui parle** : détour de `gCScriptProcessingUnit::StartOutput` (nom exporté) ;
  seules les répliques d'un dialogue en cours (`InfoSystem::IsRunning`) dont le locuteur est le héros ou
  le partenaire de dialogue sont émises (pas les commentaires ambiants).
- **Message fiable `DialogLine`** : locuteur (NetId : 1 hôte, 2 invité, ≥ 1000 PNJ), échantillon
  (nom de fichier validé : caractères `[A-Za-z0-9_%.]`, 1 à 128 octets, suffixe `.wav`), type de sortie.
  Début et fin de dialogue : `DialogState{NetId PNJ, en cours}` pour les PNJ tenus.
- **Rejeu, chez l'autre** : si l'entité locuteur est à 20 m au plus du héros local et que ce dernier
  n'est pas lui-même en dialogue : `sStartOutput(entité, échantillon, type)` + `StartFacialAni`. Entité =
  avatar (1, 2) ou réplique/PNJ capturé (≥ 1000). Pas de caméra, pas de `ZS_Talk`, pas de menu.
- **PNJ tenu pendant un dialogue de l'invité** : chez l'hôte, le PNJ natif est immobilisé (IA figée par
  le registre) le temps du dialogue, puis rendu. Chez l'invité, la réplique est libérée pour que le
  dialogue natif tourne (à valider en 4.2).
- **Sous-titres** : reportés (H5).
