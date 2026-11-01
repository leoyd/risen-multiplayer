# Protocole Risen Multiplayer — version 17

Contrat binaire entre l'hôte et l'invité (DLL `Script_Multiplayer.dll`) et le faux invité de
test (`tools/fake-guest/`, Go). Ce document fait foi : le codec C++ (`client/src/core/protocol.*`)
et le codec Go le suivent, et les vecteurs de `protocol/vectors/` en sont tirés.

La v1 couvrait **uniquement la session** (poignée de main, présence, fin de session). La v2
(story 2.1) ajoute `Motion`, la v3 (story 2.2) `Spawn` et `Despawn`, la v4 (story 2.3) le
snapshot du canal 1, la v5 (story 2.4) `AniName` et `Death`, la v6 (story 2.4b) `MotionParam`, la v7 (story 3.2) `Weapon`, la v8 (story 3.2c) `Armor`, la v9 (story 3.3) les répliques de PNJ
(`Spawn` de type 1 avec GUID, PV dans le snapshot), la v10 (story 3.4) leurs animations (`Motion` et
`MotionParam` de PNJ, sans changement de format), la v11 (story 3.4b) la mort des répliques de PNJ
(`Death` d'un PNJ à la place du `Despawn` de mort, sans changement de format), la v12 (story 3.5)
`HitRequest` (coup du héros de l'invité sur une réplique de PNJ), la v13 (story 3.6) `HitOnGuest`
(coup d'un monstre sur l'avatar de l'invité, dégâts bruts), la v14 (story 3.7) `Respawn` (mise à
terre et réapparition de l'invité, avec accusé ; `Death{2}` signifie désormais « à terre »), la v15
(story 3.8) `NpcAttack` (début d'une attaque d'un PNJ, rejouée nativement par la réplique de l'invité ;
story 3.8b, sans changement de format ni de version : aussi invité → hôte, `NetId` 2, début d'une
attaque du héros invité rejouée par son avatar), la v16 (story 4.1) `DialogLine` (réplique d'un dialogue de
l'hôte, rejouée chez l'invité sur l'entité du locuteur), la v17 (story 4.2) `DialogLine` dans l'autre sens
(locuteur 2, héros de l'invité) et `DialogState` (début et fin d'un dialogue de l'invité avec une réplique
de PNJ, que l'hôte tient pendant ce temps).

## 1. Règles générales

- **Ordre des octets** : little-endian partout.
- **Sérialisation champ par champ**, dans l'ordre du tableau de chaque message, sans
  bourrage ni alignement. Jamais de copie brute d'une struct C++.
- **Types** : `u8`, `u16`, `u32` entiers non signés ; `f32` flottant IEEE 754 simple précision ;
  `str8` = `u8 longueur` suivi de `longueur` octets UTF-8 (pas de zéro terminal).
- **Taille** : un datagramme UDP fait au plus **1200 octets**, octet de canal compris.
- **Un seul socket UDP** par pair. L'octet 0 de chaque datagramme est le **canal**.

## 2. Canaux et cadrage

| Canal | Valeur | Contenu | Story |
|---|---|---|---|
| Fiable | `0` | segments KCP (`conv` = identifiant de session tiré par l'hôte) | 1.2 |
| Snapshot | `1` | état complet à 20 Hz, non fiable (§4 bis) | 2.3 |
| Contrôle | `2` | `Hello`, `Welcome`, `Reject`, hors KCP | 1.1 |

- **Snapshot** = un datagramme complet : `[u8 canal=1][u32 tick][u8 nombre][entités]`, sans
  `MsgType` (§4 bis).
- **Message de contrôle** = un datagramme complet : `[u8 canal=2][u8 MsgType][charge]`.
- **Message fiable** = l'unité confiée à KCP (`ikcp_send`) et rendue par `ikcp_recv` :
  `[u8 MsgType][charge]`, **sans** octet de canal. L'octet de canal `0` est ajouté autour
  des segments KCP par la couche transport (story 1.2), pas par le codec.

## 3. Types de message (`MsgType`, `u8`)

| Valeur | Nom | Canal | Sens |
|---|---|---|---|
| 1 | `Hello` | contrôle | invité → hôte, renvoyé toutes les 500 ms jusqu'à la réponse |
| 2 | `Welcome` | contrôle | hôte → invité |
| 3 | `Reject` | contrôle | hôte → invité |
| 16 | `Heartbeat` | fiable | les deux sens, 1 Hz |
| 17 | `SessionEnd` | fiable | les deux sens |
| 18 | `AniName` | fiable | les deux sens : nom d'animation interné, avant son premier `Motion` (v5) |
| 19 | `Motion` | fiable | les deux sens : une animation lancée sur une entité dont l'émetteur a l'autorité (v2) ; PNJ répliqué, hôte → invité (v10) |
| 20 | `Spawn` | fiable | les deux sens : apparition d'une entité dont l'émetteur a l'autorité (v3) ; type 1 = PNJ, hôte → invité (v9) |
| 21 | `Despawn` | fiable | les deux sens : retrait d'une entité annoncée par `Spawn` (v3) ; plus jamais pour la mort d'un PNJ (v11) |
| 24 | `Death` | fiable | les deux sens : mort d'une entité dont l'émetteur a l'autorité ; le récepteur met la réplique en ragdoll (v5) ; PNJ répliqué, hôte → invité (v11) |
| 26 | `MotionParam` | fiable | les deux sens : réglage de locomotion (poids cible, vitesse de lecture) d'une entité animée (v6) ; PNJ répliqué, hôte → invité (v10) |
| 27 | `Weapon` | fiable | les deux sens : arme en main d'une entité dont l'émetteur a l'autorité, vide si rangée (v7) |
| 28 | `Armor` | fiable | les deux sens : armure portée par une entité dont l'émetteur a l'autorité, vide si aucune (v8) |
| 22 | `HitRequest` | fiable | invité → hôte : coup du héros de l'invité sur une réplique de PNJ, appliqué par l'hôte (v12) |
| 23 | `HitOnGuest` | fiable | hôte → invité : coup d'un monstre sur l'avatar de l'invité, dégâts bruts à retirer aux PV de son héros (v13) |
| 25 | `Respawn` | fiable | hôte → invité : ordre de réapparition de l'invité ; invité → hôte : accusé, avec le premier tick de snapshot valide (v14) |
| 29 | `NpcAttack` | fiable | hôte → invité : début d'une attaque d'un PNJ capturé (mouvement de combat et cible), lancée nativement par la réplique (v15) ; invité → hôte (story 3.8b) : début d'une attaque du héros invité (`NetId` 2), lancée nativement par son avatar |
| 30 | `DialogLine` | fiable | hôte → invité : une réplique d'un dialogue de l'hôte (locuteur, type de sortie, fichier voix), rejouée par l'invité sur l'entité du locuteur (v16) ; invité → hôte (v17) : une réplique d'un dialogue de l'invité, rejouée par l'hôte |
| 31 | `DialogState` | fiable | invité → hôte : début ou fin d'un dialogue de l'invité avec une réplique de PNJ ; l'hôte tient ce PNJ (routine figée) pendant le dialogue (v17) |

Aucune valeur n'est réservée (depuis la v14). Toute autre valeur, et un type reçu sur le mauvais canal (ex.
`Heartbeat` en contrôle) sont refusés comme **type inconnu**. Ajouter un type exige de
mettre à jour ce document et d'incrémenter la version du protocole.

## 4. Charges

### `Hello` (1) — contrôle

| Champ | Type | Contrainte |
|---|---|---|
| `version_protocole` | `u16` | `17` en v17 — **position figée pour toujours** |
| `version_mod` | `str8` | 0 à 255 octets, ex. `0.1.0` |
| empreinte Game.dll | `u32 TimeDateStamp`, `u32 SizeOfImage` | en-tête PE du module chargé |
| empreinte Engine.dll | `u32`, `u32` | idem |
| empreinte Script.dll | `u32`, `u32` | idem |
| empreinte SharedBase.dll | `u32`, `u32` | idem |
| `pseudo` | `str8` | **1 à 32** octets UTF-8 |
| `équipement` | `str8` | 0 à 255 octets : liste de templates, voir ci-dessous (appliquée depuis la story 3.2b) |

Taille : 40 à 581 octets (canal compris).

Format du champ `équipement` (story 3.2b, même règle que `guest_equipment` dans
`multiplayer.ini`) : noms de templates séparés par des virgules, sans espaces (forme canonique,
ex. `It_Armor_Don_Fighter,It_1H_Sword_Rusty`). Chaque nom compte 1 à 64 octets `[A-Za-z0-9_]` ;
16 noms au plus, sans doublon. Vide : aucun équipement. Le récepteur applique la même règle
d'analyse que l'émetteur : blancs ASCII tolérés autour de chaque nom, noms sensibles à la casse et
comparés octet par octet (doublons compris) ; une valeur invalide est ignorée (aucun nom appliqué)
et journalisée. Le décodeur ne vérifie que la longueur
(`str8` de 0 à 255 octets) : un contenu hors format n'est **pas** une cause de `Reject` (voir §8,
« Équipement »).

### `Welcome` (2) — contrôle

| Champ | Type | Contrainte |
|---|---|---|
| `conv` | `u32` | identifiant de session KCP tiré par l'hôte |
| position x, y, z | 3 × `f32` | position de l'avatar de l'hôte (unités du monde) |
| `lacet` | `f32` | radians, `atan2(m[8], m[10])` sur la matrice de pose (AD-7) |

Taille : 22 octets.

### `Reject` (3) — contrôle — **forme figée pour toujours**

| Champ | Type |
|---|---|
| `raison` | `u8` |

| Code | Raison |
|---|---|
| 1 | version du protocole différente |
| 2 | version du mod différente |
| 3 | empreinte du jeu différente |
| 4 | session pleine |
| 5 | `Hello` invalide |

Taille : 3 octets.

### `Heartbeat` (16) — fiable

Charge vide. Taille : 1 octet.

### `SessionEnd` (17) — fiable

| Champ | Type |
|---|---|
| `raison` | `u8` |

| Code | Raison |
|---|---|
| 1 | départ du joueur |
| 2 | fin de monde (rechargement, chargement rapide, retour au menu) |
| 3 | mort de l'hôte |
| 4 | erreur interne |

Taille : 2 octets.

### `Motion` (19) — fiable — v2

Un appel à `eCAnimation_PS::PlayMotion(type, descripteur, flag)` (AD-8). Le récepteur rejoue :
`eCResource2::GetMotion(nom de ani_id)` → `SetMotion(type, ressource)` → `PlayMotion(type,
descripteur reconstruit, flag)`. Le descripteur `eSMotionPlayBackDesc` (0x60 octets) n'est
jamais transmis : le moteur n'en lit que les 7 scalaires ci‑dessous (offsets `0x00`–`0x1B`,
`docs/re/spike-j1.md`). Le récepteur reconstruit un descripteur de 0x60 octets à zéro, avec ces
7 valeurs à leur offset.

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | entité animée (AD-6) |
| `ms` | `u32` | millisecondes depuis l'entrée en session de l'émetteur (base du `tick`, AD-7) |
| `type` | `u8` | `eEMotionType`, **0 à 7** |
| `ani_id` | `u16` | nom d'animation interné par l'émetteur (`AniName`, à venir) |
| `flags` | `u8` | bit 0 = argument `flag` de `PlayMotion` ; bit 1 = descripteur présent ; autres bits **à 0** |
| `d00` | `f32` | descripteur `+0x00` |
| `d04` | `f32` | descripteur `+0x04` |
| `d08` | `u32` | descripteur `+0x08` (le moteur ne retient que 0 et 1) |
| `d0C` | `f32` | descripteur `+0x0C` |
| `d10` | `u32` | descripteur `+0x10` |
| `d14` | `f32` | descripteur `+0x14` |
| `d18` | `u32` | descripteur `+0x18` (le moteur le compare à 2) |

Bit 1 de `flags` à 0 : les 7 scalaires valent 0 (sinon `BadValue`) et le récepteur passe un descripteur **nul**
(lecture par défaut du moteur). Taille : 41 octets.

### `AniName` (18) — fiable — v5

| Champ | Type | Contrainte |
|---|---|---|
| `ani_id` | `u16` | identifiant que portera `Motion.ani_id` ; espace propre à l'émetteur, remis à zéro à chaque session |
| `nom` | `str8` | **1 à 255** octets : titre de la ressource de motion (`eCResource2::GetFileTitleA`) |

Taille : 5 à 259 octets.

### `Spawn` (20) — fiable — v3, taille variable depuis la v9

Apparition d'une entité dont l'émetteur a l'autorité. Chaque joueur annonce son propre héros
(`type` 0), sous son propre `NetId` (AD-6) : `1` pour l'hôte, `2` pour l'invité. Depuis la v9,
l'hôte annonce aussi les PNJ répliqués (`type` 1), sous un `NetId` ≥ 1000, avec leur GUID.

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | identifiant de l'entité (AD-6) ; ≥ 1000 pour un PNJ |
| `type` | `u8` | `0` = héros d'un joueur ; `1` = PNJ (v9) ; **autre → `BadValue`** (la taille du message en dépend) |
| position x, y, z | 3 × `f32` | unités du monde |
| `lacet` | `f32` | radians, `atan2(m[8], m[10])` (AD-7) |
| `guid` | 16 × `u8` | **type 1 seulement** : 16 premiers octets du `bCPropertyID` de `eCDynamicEntity::GetID` (absent pour le type 0) |

Taille : 22 octets pour le type 0, 38 octets pour le type 1. Le décodeur lit d'abord les 22
octets communs (`Truncated` s'ils manquent), puis branche sur `type` : type inconnu → `BadValue`,
type 1 → 16 octets de GUID (`Truncated` s'ils manquent). L'encodeur refuse un type inconnu.

### `Despawn` (21) — fiable — v3

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | entité annoncée par `Spawn` |

Taille : 5 octets.

### `Death` (24) — fiable — v5

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | entité morte (AD-6) |

Taille : 5 octets. Le récepteur met la réplique en ragdoll (`SetRagDollEnabled(1, 0, nul)`).
v11 (story 3.4b) : même format ; un `net_id` ≥ 1000 est la mort d'une réplique de PNJ, émise par
l'hôte seul (§8, « Mort des répliques »).

### `HitRequest` (22) — fiable — v12

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | réplique de PNJ frappée (≥ 1000, annoncée par l'hôte) |

Taille : 5 octets. Invité → hôte seulement : le héros de l'invité a touché la réplique `net_id`.
L'hôte, qui fait autorité, applique le coup lui-même (§8, « Coups de l'invité ») ; un invité qui
en reçoit un l'ignore.

### `HitOnGuest` (23) — fiable — v13

| Champ | Type | Contrainte |
|---|---|---|
| `source_net_id` | `u32` | NetId du PNJ agresseur (≥ 1000), **0** = source inconnue — valeurs émises par l'hôte, indicatives |
| `damage` | `u32` | dégâts du coup, mesurés sur l'avatar chez l'hôte (0 à 1 000 000) — valeurs émises par l'hôte, indicatives |

Ces bornes décrivent ce qu'émet l'hôte ; elles ne sont **pas vérifiées au décodage** (aucun
`BadValue`) : toute valeur `u32` se décode.

Taille : 9 octets. Hôte → invité seulement : un monstre a touché l'avatar de l'invité chez l'hôte.
Les dégâts sont la perte de PV de l'avatar quand l'hôte laisse le coup natif s'appliquer sur lui
(scripts de dégâts du jeu, armure de l'avatar comprise ; avatar remis à ses PV max le temps du
coup, puis à ses PV d'avant). Avant 2026-10-23 (même v13), c'était `(DamageAmount + DamageBonus) ×
DamageMultiplier` de l'arme, nul pour les monstres. L'invité les retire aux PV de son héros (§8, « Coups
subis par l'invité ») ; un hôte qui en reçoit un l'ignore. `source_net_id` ne sert qu'au journal.

### `Respawn` (25) — fiable — v14

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | `2` (héros de l'invité) |
| `x`, `y`, `z` | `f32` ×3 | position de réapparition (unités du monde), finie |
| `lacet` | `f32` | radians, `atan2(m[8], m[10])` (AD-7), fini |
| `tick` | `u32` | ordre : `0` (ignoré à la réception) ; accusé : premier tick de snapshot de l'invité qui porte la nouvelle pose |

Taille : 25 octets. Deux sens, même format :
- **ordre**, hôte → invité : en réponse à un `Death{2}`, l'hôte indique où l'invité réapparaît ;
- **accusé**, invité → hôte : l'invité a replacé son héros à cette pose ; `tick` est le premier tick
  de ses snapshots à porter la nouvelle pose (§8, « Mort et réapparition de l'invité »).

Le décodeur ne vérifie aucune valeur (aucun `BadValue`) : le récepteur ignore un `net_id` ≠ 2 ou
une pose non finie (journalisé).

### `NpcAttack` (29) — fiable — v15

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | attaquant : hôte → invité, PNJ (≥ 1000, annoncé par l'hôte) ; invité → hôte, **2** (héros invité) |
| `mouvement` | `u32` | `gECombatMove` brut du jeu (`PSCombatSystem::GetLastAttackMove` de l'émetteur) |
| `cible` | `u32` | NetId de la cible chez l'émetteur. Hôte → invité : `1` héros de l'hôte, `2` avatar de l'invité, ≥ 1000 PNJ annoncé ; invité → hôte : `1` avatar de l'hôte, ≥ 1000 réplique de PNJ ; **0** = aucune ou inconnue |

Taille : 13 octets. Deux sens, même format (v15) :
- **hôte → invité** : un PNJ capturé de l'hôte commence une attaque. L'invité fixe la cible de la
  réplique et lance ce mouvement par son système de combat (§8, « Attaques natives des répliques ») :
  le coup qui en résulte n'est qu'un rendu (sang, recul, flash), les dégâts restent ceux de
  `HitOnGuest` ;
- **invité → hôte** (story 3.8b) : le héros invité commence une attaque (`net_id` = 2). L'hôte fixe
  les cibles de l'avatar et lance ce mouvement (§8, « Attaques natives de l'avatar ») : son coup
  local sur un PNJ est bloqué avec effet d'impact, seuls les `HitRequest` blessent.

Le décodeur ne vérifie aucune valeur (aucun `BadValue`). Un hôte qui reçoit un `net_id` autre que 2
l'ignore (journalisé) ; un invité qui reçoit `net_id` = 2 l'ignore (NetId non annoncé).

### `DialogLine` (30) — fiable — v16, deux sens en v17

| Champ | Type | Contrainte |
|---|---|---|
| `locuteur` | `u32` | NetId du locuteur : **1** = héros de l'hôte, **2** = héros de l'invité (v17), **≥ 1000** = PNJ annoncé ; autre → `BadValue` |
| `type` | `u8` | `gEOutputType` brut passé à `gCScriptProcessingUnit::StartOutput` : **0 à 3** (3 pour le héros) ; autre → `BadValue` |
| `échantillon` | `u8` longueur + octets | nom du fichier voix, **5 à 128** octets, caractères `[A-Za-z0-9_%.]` seulement, jamais `..` (aucun chemin), suffixe **`.wav`** précédé d'un nom non vide (ex. `INFO_Sara_2_Sara_DE_%_Sara.wav`) ; autre → `BadValue` |

Taille : 7 + longueur, soit 12 à 135 octets. Hôte → invité en v16 (locuteur 1 ou PNJ) ; v17 : aussi
invité → hôte (locuteur 2, ou le PNJ de son dialogue en cours annoncé par `DialogState`) (§8,
« Dialogues »). Le décodeur accepte 1, 2 et ≥ 1000 dans les deux sens ; le sens est vérifié par la
session (un hôte ignore le locuteur 1, un invité le locuteur 2). Le pair rejoue la réplique sur l'entité
du locuteur, sans dialogue, caméra ni menu. Les bornes de
l'échantillon sont vérifiées par l'encodeur, par le décodeur et par l'émetteur avant l'envoi : un nom
reçu n'est jamais un chemin. Un tampon trop court pour la longueur annoncée → `Truncated`.

### `DialogState` (31) — fiable — v17

| Champ | Type | Contrainte |
|---|---|---|
| `pnj` | `u32` | NetId du PNJ partenaire du dialogue de l'invité, **≥ 1000** ; autre → `BadValue` |
| `actif` | `u8` | **1** = dialogue commencé, **0** = dialogue fini ; autre → `BadValue` |

Taille : 6 octets. Invité → hôte seulement (§8, « Dialogues ») : l'hôte tient le PNJ (routine figée)
de `actif = 1` à `actif = 0`, 60 s au plus. Un invité qui reçoit `DialogState` l'ignore.

### `MotionParam` (26) — fiable — v6

Un réglage de l'enveloppe EMotionFX (`eCWrapper_emfx3ActorInstance`) que Risen applique chaque
frame à la locomotion : il démarre une animation avec un poids presque nul (`PlayMotion`), puis
fait monter ce poids et règle sa vitesse de lecture.

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | entité animée (AD-6) |
| `ms` | `u32` | même base que `Motion.ms` |
| `type` | `u8` | `eEMotionType`, **0 à 7** |
| `nature` | `u8` | `0` = `SetTargetWeight(type, valeur, durée)` ; `1` = `SetPlaySpeed(type, valeur)` ; `2` = `SwitchFadeOut(type, valeur ≠ 0, durée)` ; `3` = `StopMotion(type, durée, valeur ≠ 0)` ; autre → `BadValue` |
| `valeur` | `f32` | poids cible ou vitesse de lecture, **dans [0, 100]** ; **0 ou 1** pour les natures 2 et 3 (sinon `BadValue`) |
| `durée` | `f32` | durée du fondu (s), **dans [0, 60]** ; **exactement 0** pour `SetPlaySpeed` (sinon `BadValue`) |

Taille : 19 octets.

### `Weapon` (27) — fiable — v7

Arme que tient en main droite une entité dont l'émetteur a l'autorité. Il n'y a pas de mode de
combat à transmettre : la posture est dans le nom des animations rejouées (`AniName`).

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | entité armée (AD-6) |
| `nom` | `str8` | **0 à 255** octets : nom de l'objet en main (`Entity::GetName` de `PSInventory::GetRightHoldItem`), ex. `It_1H_Sword` ; **vide** = arme rangée |

Taille : 6 à 261 octets.

### `Armor` (28) — fiable — v8

Armure que porte une entité dont l'émetteur a l'autorité (emplacement d'équipement 8,
`gEEquipSlot` armure). Même forme que `Weapon`.

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | entité équipée (AD-6) |
| `nom` | `str8` | **0 à 255** octets : nom de l'objet de la pile équipée à l'emplacement 8 (`PSInventory::FindStackIndex(gEEquipSlot 8)`, puis `Entity::GetName` de `GetTemplateItem`), ex. `It_Armor_Inquisitor` ; **vide** = aucune armure (tenue par défaut) |

Taille : 6 à 261 octets.

**Raison inconnue** (`Reject` ou `SessionEnd`) : le message se décode quand même ; la valeur
brute est conservée et l'appelant la journalise telle quelle (« raison inconnue (200) »).

## 4 bis. Snapshot (canal 1) — non fiable — v4

État **complet** des entités dont l'émetteur a l'autorité (AD-7), sans delta. Chaque pair y met
son héros, sous son propre `NetId` ; depuis la v9, l'hôte y ajoute les PNJ répliqués (`NetId` ≥
1000), les plus proches d'abord (§8, « Répliques des PNJ »).

| Champ | Type | Contrainte |
|---|---|---|
| canal | `u8` | `1` |
| `tick` | `u32` | `floor((t − t0) / 50 ms)` à l'horloge monotone de l'émetteur, `t0` = son entrée en session |
| `nombre` | `u8` | **0 à 54** entités |
| puis, par entité (22 octets) : | | |
| `net_id` | `u32` | AD-6 |
| position x, y, z | 3 × `f32` | unités du monde |
| `lacet` | `f32` | radians, `atan2(m[8], m[10])` |
| `pv` | `u16` | points de vie (`PSSkills::GetHitPoints`), bornés à [0, 65535] (v9 ; `0` avant la v9) |

Taille : 6 + 22 × `nombre` octets, soit 1194 au plus. Le décodeur refuse un tampon trop court pour
`nombre` entités (`Truncated`), des octets en trop (`TrailingBytes`), un autre canal
(`UnknownChannel`) et un `nombre` > 54 (`BadValue`) ; l'encodeur refuse plus de 54 entités.
Tout datagramme de canal 1 venu du pair compte comme activité (§8), même illisible. Le format
n'a pas changé en v9 : seul le sens de `pv` (lu à chaque snapshot, PNJ compris).

## 5. Décodage : refus

Le décodeur ne lit jamais hors du tampon. Il refuse avec une erreur typée :

| Erreur | Cas |
|---|---|
| `Truncated` | tampon plus court que le message (y compris tampon vide), ou que les `nombre` entités d'un snapshot |
| `UnknownChannel` | octet 0 ≠ 2 pour un datagramme de contrôle, ≠ 1 pour un snapshot |
| `UnknownType` | `MsgType` inconnu, réservé ou hors de son canal |
| `UnsupportedVersion` | `Hello` dont `version_protocole` ≠ 17 (la suite n'est pas lue) |
| `BadLength` | longueur de `str8` hors bornes (ex. pseudo vide ou de 33 octets, nom d'`AniName` vide) ; l'encodeur refuse aussi un nom de `Weapon` ou d'`Armor` de plus de 255 octets |
| `TrailingBytes` | octets restants après la fin du message |
| `BadValue` | champ hors de son domaine (`Motion` : `type` > 7, bit inconnu dans `flags`, scalaire non nul sans le bit 1 ; `Spawn` : `type` autre que 0 ou 1 ; snapshot : `nombre` > 54 ; `MotionParam` : `type` > 7, `nature` > 3, valeur ou durée hors domaine, durée non nulle pour une vitesse ; `DialogLine` : locuteur autre que 1, 2 ou ≥ 1000, `type` > 3, échantillon de moins de 5 ou de plus de 128 octets, hors `[A-Za-z0-9_%.]`, contenant `..` ou sans suffixe `.wav` ; `DialogState` : `pnj` < 1000, `actif` autre que 0 ou 1) |

L'encodeur vérifie les mêmes bornes **avant** d'écrire et refuse (sans tronquer) un texte
hors bornes, une version autre que 17, un champ hors domaine ou un résultat de plus de 1200
octets.

## 6. Compatibilité

Sont **figés pour toujours**, quelle que soit la version :

1. le début de `Hello` : `[u8 canal=2][u8 MsgType=1][u16 version_protocole]` ;
2. la forme de `Reject` : `[u8 canal=2][u8 MsgType=3][u8 raison]`, et le code 1 = « version du
   protocole différente ».

Pourquoi : un hôte doit toujours pouvoir lire la version d'un `Hello` venu d'une autre
version (fonction `peek_hello_version`, qui ne lit que ces 4 octets) et lui répondre par un
`Reject{1}`. Un invité v1 face à un hôte v2 reçoit ainsi un refus qu'il sait lire et
journaliser, et non un silence suivi d'un timeout. Tout le reste peut changer d'une version
à l'autre ; aucune négociation n'est prévue : les deux pairs doivent avoir la même version.

L'hôte vérifie dans l'ordre : version du protocole (sinon `Reject{1}`), `Hello` décodable
(sinon `Reject{5}`), version du mod (`Reject{2}`), empreinte du jeu (`Reject{3}`), place libre
(`Reject{4}`).

## 7. Vecteurs de référence

Chaque vecteur est un fichier `protocol/vectors/<nom>.bin`. Les blocs ci‑dessous sont
**exactement** la sortie de `xxd <nom>.bin` ; les `.bin` sont régénérés à partir de ces blocs
par `xxd -r` (voir `protocol/vectors/README.md` et `protocol/vectors/regen.sh`), jamais par
l'encodeur testé. Le commentaire `<!-- vector: … -->` qui précède chaque bloc sert au script.

### Valides

#### `hello.bin` — `Hello`, 58 octets

`version_protocole=17`, `version_mod="0.1.0"`, empreintes de Risen Steam x64 (janvier 2023) :
Game `0x63c9aff6`/`0x01417000`, Engine `0x63ca945d`/`0x00d3a000`, Script `0x63c9b014`/`0x004d1000`,
SharedBase `0x63c569b6`/`0x0025a000`, `pseudo="Leo"`, `équipement="It_Sword_01"`.

<!-- vector: hello.bin -->
```text
00000000: 0201 1100 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63 00a0 d300 14b0 c963 0010  A.]..c.......c..
00000020: 4d00 b669 c563 00a0 2500 034c 656f 0b49  M..i.c..%..Leo.I
00000030: 745f 5377 6f72 645f 3031                 t_Sword_01
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` | canal contrôle |
| 0x01 | `01` | `MsgType` Hello |
| 0x02 | `11 00` | `version_protocole` = 17 |
| 0x04 | `05` `30 2e 31 2e 30` | `version_mod` = "0.1.0" |
| 0x0a | `f6 af c9 63` `00 70 41 01` | Game.dll |
| 0x12 | `5d 94 ca 63` `00 a0 d3 00` | Engine.dll |
| 0x1a | `14 b0 c9 63` `00 10 4d 00` | Script.dll |
| 0x22 | `b6 69 c5 63` `00 a0 25 00` | SharedBase.dll |
| 0x2a | `03` `4c 65 6f` | `pseudo` = "Leo" |
| 0x2e | `0b` `49 74 … 31` | `équipement` = "It_Sword_01" |

#### `welcome.bin` — `Welcome`, 22 octets

`conv=0x12345678`, position `(100.0, -50.5, 2000.0)`, `lacet=1.5`.

<!-- vector: welcome.bin -->
```text
00000000: 0202 7856 3412 0000 c842 0000 4ac2 0000  ..xV4....B..J...
00000010: fa44 0000 c03f                           .D...?
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` | canal contrôle |
| 0x01 | `02` | `MsgType` Welcome |
| 0x02 | `78 56 34 12` | `conv` = 0x12345678 |
| 0x06 | `00 00 c8 42` | x = 100.0 |
| 0x0a | `00 00 4a c2` | y = -50.5 |
| 0x0e | `00 00 fa 44` | z = 2000.0 |
| 0x12 | `00 00 c0 3f` | lacet = 1.5 |

#### `reject.bin` — `Reject`, 3 octets

`raison=2` (version du mod différente).

<!-- vector: reject.bin -->
```text
00000000: 0203 02                                  ...
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` | canal contrôle |
| 0x01 | `03` | `MsgType` Reject |
| 0x02 | `02` | raison = 2 |

#### `heartbeat.bin` — `Heartbeat` (fiable, sans octet de canal), 1 octet

<!-- vector: heartbeat.bin -->
```text
00000000: 10                                       .
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `10` | `MsgType` Heartbeat (16) |

#### `session_end.bin` — `SessionEnd` (fiable, sans octet de canal), 2 octets

`raison=1` (départ du joueur).

<!-- vector: session_end.bin -->
```text
00000000: 1101                                     ..
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `11` | `MsgType` SessionEnd (17) |
| 0x01 | `01` | raison = 1 |

#### `motion.bin` — `Motion` (fiable, sans octet de canal), 41 octets

`net_id=2`, `ms=1234`, `type=0`, `ani_id=7`, `flags=3` (flag + descripteur), `d00=0.25`,
`d04=0.5`, `d08=1`, `d0C=1.0`, `d10=0`, `d14=0.75`, `d18=2`.

<!-- vector: motion.bin -->
```text
00000000: 1302 0000 00d2 0400 0000 0700 0300 0080  ................
00000010: 3e00 0000 3f01 0000 0000 0080 3f00 0000  >...?.......?...
00000020: 0000 0040 3f02 0000 00                   ...@?....
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `13` | `MsgType` Motion (19) |
| 0x01 | `02 00 00 00` | `net_id` = 2 |
| 0x05 | `d2 04 00 00` | `ms` = 1234 |
| 0x09 | `00` | `type` = 0 |
| 0x0a | `07 00` | `ani_id` = 7 |
| 0x0c | `03` | `flags` = flag + descripteur |
| 0x0d | `00 00 80 3e` | `d00` = 0.25 |
| 0x11 | `00 00 00 3f` | `d04` = 0.5 |
| 0x15 | `01 00 00 00` | `d08` = 1 |
| 0x19 | `00 00 80 3f` | `d0C` = 1.0 |
| 0x1d | `00 00 00 00` | `d10` = 0 |
| 0x21 | `00 00 40 3f` | `d14` = 0.75 |
| 0x25 | `02 00 00 00` | `d18` = 2 |

#### `motion_npc.bin` — `Motion` d'un PNJ (fiable, sans octet de canal), 41 octets

v10 (story 3.4) : même format, `net_id` d'un PNJ annoncé par l'hôte. `net_id=1000`, `ms=1234`,
`type=0`, `ani_id=3`, `flags=1` (flag, sans descripteur : les 7 scalaires valent 0).

<!-- vector: motion_npc.bin -->
```text
00000000: 13e8 0300 00d2 0400 0000 0300 0100 0000  ................
00000010: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000020: 0000 0000 0000 0000 00                   .........
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `13` | `MsgType` Motion (19) |
| 0x01 | `e8 03 00 00` | `net_id` = 1000 (PNJ) |
| 0x05 | `d2 04 00 00` | `ms` = 1234 |
| 0x09 | `00` | `type` = 0 |
| 0x0a | `03 00` | `ani_id` = 3 |
| 0x0c | `01` | `flags` = flag seul |
| 0x0d | 28 × `00` | `d00` à `d18` = 0 |

#### `hit_request.bin` — `HitRequest` (fiable, sans octet de canal), 5 octets

v12 (story 3.5) : coup de l'invité sur la réplique `net_id=1000`.

<!-- vector: hit_request.bin -->
```text
00000000: 16e8 0300 00                             .....
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `16` | `MsgType` HitRequest (22) |
| 0x01 | `e8 03 00 00` | `net_id` = 1000 (PNJ) |

#### `hit_on_guest.bin` — `HitOnGuest` (fiable, sans octet de canal), 9 octets

v13 (story 3.6) : coup du PNJ `source_net_id=1000` sur l'avatar de l'invité, 18 dégâts bruts.

<!-- vector: hit_on_guest.bin -->
```text
00000000: 17e8 0300 0012 0000 00                   .........
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `17` | `MsgType` HitOnGuest (23) |
| 0x01 | `e8 03 00 00` | `source_net_id` = 1000 (PNJ) |
| 0x05 | `12 00 00 00` | `damage` = 18 |

#### `respawn.bin` — `Respawn` (fiable, sans octet de canal), 25 octets

v14 (story 3.7) : réapparition du héros de l'invité (`net_id=2`) en `(100.0, 0.0, -50.0)`, lacet
`1.5`, `tick=42` (forme d'un accusé ; un ordre porte `tick=0`).

<!-- vector: respawn.bin -->
```text
00000000: 1902 0000 0000 00c8 4200 0000 0000 0048  ........B......H
00000010: c200 00c0 3f2a 0000 00                   ....?*...
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `19` | `MsgType` Respawn (25) |
| 0x01 | `02 00 00 00` | `net_id` = 2 (héros de l'invité) |
| 0x05 | `00 00 c8 42` | `x` = 100.0 |
| 0x09 | `00 00 00 00` | `y` = 0.0 |
| 0x0d | `00 00 48 c2` | `z` = -50.0 |
| 0x11 | `00 00 c0 3f` | `lacet` = 1.5 |
| 0x15 | `2a 00 00 00` | `tick` = 42 |

#### `npc_attack.bin` — `NpcAttack` (fiable, sans octet de canal), 13 octets

v15 (story 3.8) : le PNJ `net_id=1005` commence l'attaque `mouvement=11` sur l'avatar de l'invité
(`cible=2`).

<!-- vector: npc_attack.bin -->
```text
00000000: 1ded 0300 000b 0000 0002 0000 00         .............
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1d` | `MsgType` NpcAttack (29) |
| 0x01 | `ed 03 00 00` | `net_id` = 1005 (PNJ) |
| 0x05 | `0b 00 00 00` | `mouvement` = 11 |
| 0x09 | `02 00 00 00` | `cible` = 2 (avatar de l'invité) |

#### `dialog_line.bin` — `DialogLine` (fiable, sans octet de canal), 37 octets

v16 (story 4.1) : le PNJ `locuteur=1004` dit la réplique `INFO_Sara_2_Sara_DE_%_Sara.wav`, `type=0`.

<!-- vector: dialog_line.bin -->
```text
00000000: 1eec 0300 0000 1e49 4e46 4f5f 5361 7261  .......INFO_Sara
00000010: 5f32 5f53 6172 615f 4445 5f25 5f53 6172  _2_Sara_DE_%_Sar
00000020: 612e 7761 76                             a.wav
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1e` | `MsgType` DialogLine (30) |
| 0x01 | `ec 03 00 00` | `locuteur` = 1004 (PNJ) |
| 0x05 | `00` | `type` = 0 |
| 0x06 | `1e` `49 4e … 76` | `échantillon` = "INFO_Sara_2_Sara_DE_%_Sara.wav" (30 octets) |

#### `dialog_line_guest.bin` — `DialogLine` de l'invité (fiable, sans octet de canal), 43 octets

v17 (story 4.2) : le héros de l'invité, `locuteur=2`, dit la réplique `INFO_Sara_1_PC_Hero_DE_%_PC_Hero.wav`,
`type=3`.

<!-- vector: dialog_line_guest.bin -->
```text
00000000: 1e02 0000 0003 2449 4e46 4f5f 5361 7261  ......$INFO_Sara
00000010: 5f31 5f50 435f 4865 726f 5f44 455f 255f  _1_PC_Hero_DE_%_
00000020: 5043 5f48 6572 6f2e 7761 76              PC_Hero.wav
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1e` | `MsgType` DialogLine (30) |
| 0x01 | `02 00 00 00` | `locuteur` = 2 (héros de l'invité) |
| 0x05 | `03` | `type` = 3 |
| 0x06 | `24` `49 4e … 76` | `échantillon` = "INFO_Sara_1_PC_Hero_DE_%_PC_Hero.wav" (36 octets) |

#### `dialog_state.bin` — `DialogState` (fiable, sans octet de canal), 6 octets

v17 (story 4.2) : l'invité commence un dialogue avec la réplique `pnj=1033`, `actif=1`.

<!-- vector: dialog_state.bin -->
```text
00000000: 1f09 0400 0001                           ......
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1f` | `MsgType` DialogState (31) |
| 0x01 | `09 04 00 00` | `pnj` = 1033 |
| 0x05 | `01` | `actif` = 1 (dialogue commencé) |

#### `spawn.bin` — `Spawn` (fiable, sans octet de canal), 22 octets

`net_id=2`, `type=0`, position `(100.0, -50.5, 2000.0)`, `lacet=1.5`.

<!-- vector: spawn.bin -->
```text
00000000: 1402 0000 0000 0000 c842 0000 4ac2 0000  .........B..J...
00000010: fa44 0000 c03f                           .D...?
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `14` | `MsgType` Spawn (20) |
| 0x01 | `02 00 00 00` | `net_id` = 2 |
| 0x05 | `00` | `type` = 0 (héros) |
| 0x06 | `00 00 c8 42` | x = 100.0 |
| 0x0a | `00 00 4a c2` | y = -50.5 |
| 0x0e | `00 00 fa 44` | z = 2000.0 |
| 0x12 | `00 00 c0 3f` | lacet = 1.5 |

#### `spawn_npc.bin` — `Spawn` d'un PNJ (fiable, sans octet de canal), 38 octets

`net_id=1000`, `type=1`, position `(100.0, -50.5, 2000.0)`, `lacet=1.5`,
`guid=c3624668 11223344 55667788 99aabbcc` (début du GUID d'un Vulture relevé par la sonde du
spike J2, complété).

<!-- vector: spawn_npc.bin -->
```text
00000000: 14e8 0300 0001 0000 c842 0000 4ac2 0000  .........B..J...
00000010: fa44 0000 c03f c362 4668 1122 3344 5566  .D...?.bFh."3DUf
00000020: 7788 99aa bbcc                           w.....
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `14` | `MsgType` Spawn (20) |
| 0x01 | `e8 03 00 00` | `net_id` = 1000 |
| 0x05 | `01` | `type` = 1 (PNJ) |
| 0x06 | `00 00 c8 42` | x = 100.0 |
| 0x0a | `00 00 4a c2` | y = -50.5 |
| 0x0e | `00 00 fa 44` | z = 2000.0 |
| 0x12 | `00 00 c0 3f` | lacet = 1.5 |
| 0x16 | `c3 62 … bb cc` | `guid` (16 octets) |

#### `despawn.bin` — `Despawn` (fiable, sans octet de canal), 5 octets

`net_id=2`.

<!-- vector: despawn.bin -->
```text
00000000: 1502 0000 00                             .....
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `15` | `MsgType` Despawn (21) |
| 0x01 | `02 00 00 00` | `net_id` = 2 |

#### `aniname.bin` — `AniName` (fiable, sans octet de canal), 57 octets

`ani_id=7`, `nom="Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400"` (53 octets, relevé par la
sonde).

<!-- vector: aniname.bin -->
```text
00000000: 1207 0035 4865 726f 5f53 7461 6e64 5f4e  ...5Hero_Stand_N
00000010: 6f6e 655f 4e6f 6e65 5f50 305f 4d6f 7665  one_None_P0_Move
00000020: 5f52 756e 5f4e 5f46 7764 5f30 305f 255f  _Run_N_Fwd_00_%_
00000030: 3030 5f50 305f 3430 30                   00_P0_400
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `12` | `MsgType` AniName (18) |
| 0x01 | `07 00` | `ani_id` = 7 |
| 0x03 | `35` `48 65 … 30` | `nom` (53 octets) |

#### `death.bin` — `Death` (fiable, sans octet de canal), 5 octets

`net_id=2`.

<!-- vector: death.bin -->
```text
00000000: 1802 0000 00                             .....
```

#### `death_npc.bin` — `Death` d'un PNJ (fiable, sans octet de canal), 5 octets

v11 (story 3.4b) : même format, `net_id` d'un PNJ annoncé par l'hôte. `net_id=1000`.

<!-- vector: death_npc.bin -->
```text
00000000: 18e8 0300 00                             .....
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `18` | `MsgType` Death (24) |
| 0x01 | `e8 03 00 00` | `net_id` = 1000 (PNJ) |

#### `motion_param.bin` — `MotionParam` (fiable, sans octet de canal), 19 octets

`net_id=2`, `ms=1234`, `type=0`, `nature=0` (poids cible), `valeur=1.0`, `durée=0.25`.

<!-- vector: motion_param.bin -->
```text
00000000: 1a02 0000 00d2 0400 0000 0000 0080 3f00  ..............?.
00000010: 0080 3e                                  ..>
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1a` | `MsgType` MotionParam (26) |
| 0x01 | `02 00 00 00` | `net_id` = 2 |
| 0x05 | `d2 04 00 00` | `ms` = 1234 |
| 0x09 | `00` | `type` = 0 |
| 0x0a | `00` | `nature` = 0 (poids cible) |
| 0x0b | `00 00 80 3f` | `valeur` = 1.0 |
| 0x0f | `00 00 80 3e` | `durée` = 0.25 |

#### `weapon.bin` — `Weapon` (fiable, sans octet de canal), 17 octets

`net_id=2`, `nom="It_1H_Sword"` (11 octets, relevé par la sonde du spike J2).

<!-- vector: weapon.bin -->
```text
00000000: 1b02 0000 000b 4974 5f31 485f 5377 6f72  ......It_1H_Swor
00000010: 64                                       d
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1b` | `MsgType` Weapon (27) |
| 0x01 | `02 00 00 00` | `net_id` = 2 |
| 0x05 | `0b` `49 74 … 64` | `nom` = "It_1H_Sword" |

#### `weapon_sheathed.bin` — `Weapon` arme rangée (fiable, sans octet de canal), 6 octets

`net_id=2`, `nom=""` (longueur 0 : arme rangée).

<!-- vector: weapon_sheathed.bin -->
```text
00000000: 1b02 0000 0000                           ......
```

#### `armor.bin` — `Armor` (fiable, sans octet de canal), 25 octets

`net_id=1` (héros de l'hôte), `nom="It_Armor_Inquisitor"` (19 octets).

<!-- vector: armor.bin -->
```text
00000000: 1c01 0000 0013 4974 5f41 726d 6f72 5f49  ......It_Armor_I
00000010: 6e71 7569 7369 746f 72                   nquisitor
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `1c` | `MsgType` Armor (28) |
| 0x01 | `01 00 00 00` | `net_id` = 1 |
| 0x05 | `13` `49 74 … 72` | `nom` = "It_Armor_Inquisitor" |

#### `armor_removed.bin` — `Armor` sans armure (fiable, sans octet de canal), 6 octets

`net_id=1`, `nom=""` (longueur 0 : armure retirée, tenue par défaut).

<!-- vector: armor_removed.bin -->
```text
00000000: 1c01 0000 0000                           ......
```

#### `snapshot.bin` — snapshot (canal 1), 28 octets

`tick=1234`, une entité : `net_id=2`, position `(100.0, -50.5, 2000.0)`, `lacet=1.5`, `pv=75`.

<!-- vector: snapshot.bin -->
```text
00000000: 01d2 0400 0001 0200 0000 0000 c842 0000  .............B..
00000010: 4ac2 0000 fa44 0000 c03f 4b00            J....D...?K.
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `01` | canal snapshot |
| 0x01 | `d2 04 00 00` | `tick` = 1234 |
| 0x05 | `01` | `nombre` = 1 |
| 0x06 | `02 00 00 00` | `net_id` = 2 |
| 0x0a | `00 00 c8 42` | x = 100.0 |
| 0x0e | `00 00 4a c2` | y = -50.5 |
| 0x12 | `00 00 fa 44` | z = 2000.0 |
| 0x16 | `00 00 c0 3f` | lacet = 1.5 |
| 0x1a | `4b 00` | `pv` = 75 |

#### `snapshot_multi.bin` — snapshot à trois entités (canal 1), 72 octets

`tick=1234` ; héros de l'hôte `net_id=1`, `(100.0, -50.5, 2000.0)`, `lacet=1.5`, `pv=75` ; PNJ
`net_id=1000`, `(150.0, -50.5, 2500.0)`, `lacet=-0.5`, `pv=150` ; PNJ `net_id=1001`,
`(-300.0, 10.0, 1800.0)`, `lacet=3.0`, `pv=65535` (borne haute).

<!-- vector: snapshot_multi.bin -->
```text
00000000: 01d2 0400 0003 0100 0000 0000 c842 0000  .............B..
00000010: 4ac2 0000 fa44 0000 c03f 4b00 e803 0000  J....D...?K.....
00000020: 0000 1643 0000 4ac2 0040 1c45 0000 00bf  ...C..J..@.E....
00000030: 9600 e903 0000 0000 96c3 0000 2041 0000  ............ A..
00000040: e144 0000 4040 ffff                      .D..@@..
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `01` | canal snapshot |
| 0x01 | `d2 04 00 00` | `tick` = 1234 |
| 0x05 | `03` | `nombre` = 3 |
| 0x06 | `01 00 00 00` … `4b 00` | entité 1 : héros de l'hôte, `pv` = 75 |
| 0x1c | `e8 03 00 00` … `96 00` | entité 2 : PNJ 1000, `pv` = 150 |
| 0x32 | `e9 03 00 00` … `ff ff` | entité 3 : PNJ 1001, `pv` = 65535 |

### Invalides

#### `hello_future.bin` — `Hello` d'une version future, 8 octets

`peek_hello_version` → 65535 ; décodage complet → `UnsupportedVersion`. La suite
(`ff ff ff ff`) est volontairement illisible. La version `0xFFFF` ne sera jamais atteinte : ce
vecteur reste valable à chaque incrémentation.

<!-- vector: hello_future.bin -->
```text
00000000: 0201 ffff ffff ffff                      ........
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` `01` | canal contrôle, `MsgType` Hello |
| 0x02 | `ff ff` | `version_protocole` = 65535 |
| 0x04 | `ff ff ff ff` | charge d'une autre version |

#### `hello_truncated.bin` — `Hello` coupé, 22 octets

Les 22 premiers octets de `hello.bin` (coupé après le `TimeDateStamp` d'Engine.dll) →
`Truncated` ; `peek_hello_version` → 17.

<!-- vector: hello_truncated.bin -->
```text
00000000: 0201 1100 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63                           A.]..c
```

#### `hello_long_nickname.bin` — pseudo de 33 octets, 88 octets

Identique à `hello.bin` sauf `pseudo` = 33 × `A` (`21` puis 33 × `41`) → `BadLength`.

<!-- vector: hello_long_nickname.bin -->
```text
00000000: 0201 1100 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63 00a0 d300 14b0 c963 0010  A.]..c.......c..
00000020: 4d00 b669 c563 00a0 2500 2141 4141 4141  M..i.c..%.!AAAAA
00000030: 4141 4141 4141 4141 4141 4141 4141 4141  AAAAAAAAAAAAAAAA
00000040: 4141 4141 4141 4141 4141 4141 0b49 745f  AAAAAAAAAAAA.It_
00000050: 5377 6f72 645f 3031                      Sword_01
```

#### `unknown_type.bin` — type inconnu, 2 octets

Canal contrôle, `MsgType` 99 → `UnknownType`.

<!-- vector: unknown_type.bin -->
```text
00000000: 0263                                     .c
```

#### `motion_bad_type.bin` — `Motion` de type 8, 41 octets

`motion.bin` avec `type` = 8 (offset 0x09) → `BadValue`.

<!-- vector: motion_bad_type.bin -->
```text
00000000: 1302 0000 00d2 0400 0008 0700 0300 0080  ................
00000010: 3e00 0000 3f01 0000 0000 0080 3f00 0000  >...?.......?...
00000020: 0000 0040 3f02 0000 00                   ...@?....
```

#### `aniname_empty.bin` — `AniName` au nom vide, 4 octets

`ani_id=7`, longueur 0 → `BadLength`.

<!-- vector: aniname_empty.bin -->
```text
00000000: 1207 0000                                ....
```

#### `motion_param_bad.bin` — `MotionParam` de nature 4, 19 octets

`motion_param.bin` avec `nature` = 4 (offset 0x0a) → `BadValue`.

<!-- vector: motion_param_bad.bin -->
```text
00000000: 1a02 0000 00d2 0400 0000 0400 0080 3f00  ..............?.
00000010: 0080 3e                                  ..>
```

#### `spawn_bad_type.bin` — `Spawn` de type 2, 22 octets

`spawn.bin` avec `type` = 2 (offset 0x05) → `BadValue` (v9 : la taille d'un `Spawn` dépend de son
type ; un type inconnu n'est pas lisible).

<!-- vector: spawn_bad_type.bin -->
```text
00000000: 1402 0000 0002 0000 c842 0000 4ac2 0000  .........B..J...
00000010: fa44 0000 c03f                           .D...?
```

#### `snapshot_truncated.bin` — snapshot annonçant 2 entités, 28 octets

`snapshot.bin` avec `nombre` = 2 (offset 0x05) : une seule entité suit → `Truncated`.

<!-- vector: snapshot_truncated.bin -->
```text
00000000: 01d2 0400 0002 0200 0000 0000 c842 0000  .............B..
00000010: 4ac2 0000 fa44 0000 c03f 4b00            J....D...?K.
```

## 8. Règles de session

Règles **normatives** depuis la v1 ; la v3 ajoute les avatars (`Spawn`, `Despawn`), la v4 les snapshots, la v5 les animations (`AniName`, `Motion`) et la mort (`Death`), la v6 les réglages de locomotion (`MotionParam`), la v7 l'arme en main (`Weapon`), la v8 l'armure portée (`Armor`), la v9 les répliques de PNJ, la v10 leurs animations, la v11 leur mort, la v12 les coups de l'invité, la v13 les coups subis par l'invité, la v14 la mise à terre et la réapparition de l'invité, la v15 les attaques natives des répliques, la v16 les répliques de dialogue de l'hôte, la v17 celles de l'invité et la tenue du PNJ de son dialogue. Elles s'appliquent à la
DLL (hôte et invité, stories 1.4 et 1.5) comme au faux invité `tools/fake-guest/`. Tous les
délais se mesurent à l'**horloge monotone** (QPC sous Windows), jamais en nombre d'images ou
d'appels.

### Invité

- Envoie `Hello` **toutes les 500 ms** jusqu'à recevoir `Welcome` ou `Reject`.
- **Abandon après 10 s** sans `Welcome` ni `Reject` : fin « hôte injoignable ».
- Sur `Reject`, la raison est journalisée (code brut si inconnu) et l'invité arrête : pas de
  nouvel essai automatique.
- Un `Welcome` portant `conv = 0` est ignoré : l'invité continue d'envoyer `Hello`.
- Un `Welcome` ou un `Reject` reçu **en session** est ignoré (il compte seulement comme
  activité de l'hôte).
- Seuls les datagrammes venus de l'adresse de l'hôte sont pris en compte.

### Hôte

- Vérifie chaque `Hello` dans l'ordre du §6 : version du protocole lue par
  `peek_hello_version` (`Reject{1}`), `Hello` décodable (`Reject{5}`), version du mod
  (`Reject{2}`), empreinte du jeu (`Reject{3}`), place libre (`Reject{4}`).
- Comparaisons **exactes** : `version_mod` octet par octet (sans normalisation), et les 4
  empreintes (`TimeDateStamp` et `SizeOfImage` de Game, Engine, Script, SharedBase).
- Tire un `conv` **aléatoire non nul** pour chaque nouvelle session.
- Un `Hello` venu de la **même `ip:port`** que l'invité accepté reçoit le **même `Welcome`**
  (même `conv`) jusqu'au premier message fiable reçu de cette adresse ; ensuite, ces `Hello`
  sont ignorés.
- Un `Hello` venu d'**une autre adresse** pendant une session reçoit `Reject{4}` (session
  pleine), sans effet sur la session en cours.
  Les vérifications 1 à 4 du §6 s'appliquent d'abord : un `Hello` d'une autre version venu
  d'une autre adresse reçoit `Reject{1}`, pas `Reject{4}`.
- Un `Hello` venu du **pair accepté** qui échoue à une vérification (version, décodage, mod,
  empreinte) est **ignoré** : ni `Reject`, ni `Welcome`, ni effet sur la session.
- `Welcome` et `Reject` reçus par l'hôte sont ignorés.
- Le `conv` est tiré à neuf (aléatoire, non nul) à chaque session, y compris après un timeout
  ou une fin de session.

### Les deux pairs

- Le KCP (canal 0) n'est créé qu'**après `Welcome`** : par l'hôte quand il l'envoie, par
  l'invité quand il le reçoit. Avant, tout datagramme de canal 0 est ignoré.
- `Heartbeat` à **1 Hz** sur le canal fiable, dans les deux sens, dès l'entrée en session.
- **5 s sans aucun datagramme** du pair (tous canaux confondus) → fin « timeout », sans rien
  envoyer.
  Une implémentation peut ne compter que les **messages** reçus du pair (contrôle, ou fiables
  rendus par KCP) plutôt que chaque datagramme : les heartbeats à 1 Hz suffisent à maintenir
  l'activité. C'est le cas de la DLL.
- **Pause du jeu** (story 3.4a, sans changement de format ni de version) : quand la boucle du jeu
  d'un pair est suspendue (cinématique, chargement, menu bloquant), ses heartbeats ne doivent pas
  s'interrompre. Après **1,5 s** sans que le jeu ait lu le transport, l'émetteur continue d'envoyer
  `Heartbeat` à **1 Hz** sur le canal fiable depuis son thread réseau, **60 s** au plus ; au-delà,
  il se tait, et le pair conclut au timeout 5 s plus tard (un jeu figé pour de bon est donc
  détecté en 65 s environ). Ces `Heartbeat` sont des messages ordinaires : le récepteur ne les
  distingue pas. Une fin de monde reste annoncée par `SessionEnd{2}` à la reprise du jeu.
  Si le jeu se fige pendant le maintien du KCP après `SessionEnd` (chargement juste après une fin
  de monde), ces `Heartbeat` peuvent partir après le `SessionEnd`, tant que le KCP est gardé : le
  pair les ignore (il est lui-même en maintien, ou son KCP est fermé).
- Fin volontaire = envoi de `SessionEnd{raison}` sur le canal fiable, puis fermeture du KCP.
  La session est terminée dès l'envoi, mais le KCP est **gardé 1 s** (horloge monotone) pour
  laisser partir le segment et recevoir son acquittement ; pendant ce maintien, tout message
  reçu est ignoré. Ensuite le KCP est fermé et le socket aussi. Un `SessionEnd` reçu termine la
  session avec la raison qu'il porte, sans rien renvoyer.
- **Raisons locales** envoyées par la DLL en session : `2` fin de monde (rechargement,
  chargement rapide, retour au menu), `3` mort de l'hôte (hôte seulement), `4` erreur interne.
  Hors session (`Hosting`, `Connecting`), une fin locale n'envoie rien.
- **Animations** (v5, AD-8) :
  - l'émetteur envoie un `Motion` par `PlayMotion` de son héros, en session seulement, daté en
    `ms` dans la base du `tick` (depuis son entrée en session). Le premier usage d'un nom part
    d'abord en `AniName{ani_id, nom}`, sur le même canal fiable : il précède donc toujours son
    premier `Motion`. Les `ani_id` repartent de 0 à chaque session ;
  - le récepteur n'accepte un `Motion` que du `NetId` de l'avatar du pair, avatar présent et
    vivant, ou (invité, v10) d'un `NetId` de PNJ annoncé par l'hôte et pas encore retiré (« Animations
    des PNJ ») ; un `ani_id` inconnu ou un scalaire non fini le fait ignorer (journalisé) ;
  - il rejoue chaque `Motion` quand son temps d'affichage (horloge distante − 150 ms, comme les
    snapshots) atteint `ms` ; un `Motion` en retard (ou reçu avant tout snapshot) part aussitôt ;
    parmi ceux qui partent à la même frame, seul le plus récent des types 0 et 1 est rejoué.
- **Réglages de locomotion** (v6) : l'émetteur envoie un `MotionParam` quand le poids cible ou la
  vitesse de lecture d'un type change chez son héros (écart de plus de 0,01 pour un poids, de 0,02
  pour une vitesse, durée différente de plus de 0,005 s, valeur finale exacte de 0 ou 1, ou premier
  réglage après un `Motion` de ce type), ou quand un fondu (`SwitchFadeOut`) ou un arrêt
  (`StopMotion`) diffère du précédent, en session seulement. Le récepteur l'accepte aux
  mêmes conditions qu'un `Motion` et le rejoue sur l'avatar au même instant d'affichage, dans l'ordre
  d'arrivée ; parmi ceux qui partent à la même frame, un réglage des types 0/1 placé avant le
  `Motion` gardé de son type est écarté (il visait l'animation remplacée).
- **Mort** (v5, sens de `Death{2}` changé en v14) : seul l'invité envoie `Death`, et `Death{2}`
  signifie « **à terre** » (« Mort et réapparition de l'invité », plus bas) : l'hôte ne met plus
  l'avatar en ragdoll. `Death{1}` n'est jamais émis : la mort de l'hôte termine la session
  (raison 3) avant tout envoi, et l'invité retire alors l'avatar. S'il en reçoit un quand même,
  l'invité garde le comportement v5 : il l'applique au même instant d'affichage que la pose (celui
  du dernier snapshot reçu avec le `Death`), après les gestes encore en attente ; l'avatar passe en
  ragdoll, puis n'est plus ni déplacé ni animé jusqu'à son retrait (un `Spawn` suivant lève cet
  état).
- **Arme en main** (v7) : à chaque frame, l'émetteur lit l'objet que tient son héros en main
  droite (`PSInventory::GetRightHoldStack` ≥ 0, puis le nom de `GetRightHoldItem`) ; il envoie
  `Weapon{son NetId, nom}` quand ce nom change (vide : arme rangée), en session seulement, et une
  fois à l'entrée en session, juste **après** son `Spawn` (dernier nom lu, vide compris). Une
  lecture en échec garde le dernier état et n'envoie rien.
  Le récepteur n'accepte un `Weapon` que du `NetId` de l'avatar du pair, avatar présent et
  vivant, sans `Death` en attente d'affichage (sinon ignoré, journalisé en debug). Il l'applique au même instant d'affichage qu'un `Death`
  (celui du dernier snapshot reçu), pour que les gestes suivants se rejouent arme en main : nom
  non vide → l'objet de ce nom est créé dans l'inventaire de l'avatar s'il n'y est pas, une seule
  fois par nom et par avatar, puis tenu en main (`HoldRightStack`) ; nom vide → l'objet en main
  est rangé (`EquipStack`), sans effet si la main est vide. Un nom que le jeu ne connaît pas est
  journalisé une fois et laisse l'avatar inchangé.
- **Équipement** (story 3.2b, sans changement de format : champ `équipement` du `Hello`) :
  - l'invité envoie la forme canonique de sa `guest_equipment`, validée au chargement de la
    config ; l'hôte ignore la sienne ;
  - à **chaque** entrée en session, juste après sa téléportation devant l'hôte (ou à la place,
    si le `Welcome` porte une pose nulle), l'invité équipe son propre héros ;
  - l'hôte lit les noms du `Hello` accepté avec la même règle d'analyse que l'émetteur (blancs
    ASCII tolérés autour de chaque nom, noms sensibles à la casse, comparés octet par octet) et
    les garde jusqu'à la fin de la session ; il équipe l'avatar de l'invité **juste après chaque
    création** (`Spawn{2}` reçu). Une valeur invalide est ignorée et journalisée (Warn, une
    fois) : la session est acceptée et l'avatar reste non équipé ;
  - équiper une entité, nom par nom : `Template(nom)` puis `IsValid` ; la pile de ce template est
    cherchée dans l'inventaire (`FindStackIndex`) ou, à défaut, créée (`CreateItems`, 1 objet :
    rien n'est créé si l'objet est déjà là) ; puis `EquipStack`, sauf si la pile est déjà
    équipée (`IsStackEquiped`). Un nom en échec (inconnu du jeu…) est journalisé et n'empêche
    pas les suivants. Sur l'avatar, ces piles sont les mêmes que celles de l'arme (`Weapon`) :
    une pile par nom et par avatar.
- **Armure portée** (v8, story 3.2c) : à chaque frame, l'émetteur lit la pile équipée à
  l'emplacement d'équipement 8 de son héros (`PSInventory::FindStackIndex(gEEquipSlot 8)`), puis
  le nom de son objet (`GetTemplateItem`, `Entity::GetName`), vide si aucune pile n'y est
  (`−1`) ; il envoie `Armor{son NetId, nom}` quand ce nom change, en session seulement, et une
  fois à l'entrée en session, juste **après** son `Spawn` et son `Weapon` (dernier nom lu, vide
  compris). Une lecture en échec garde le dernier état et n'envoie rien.
  Le récepteur l'accepte aux mêmes conditions qu'un `Weapon` (avatar du pair présent, vivant,
  sans `Death` en attente ; sinon ignoré, journalisé en debug) et l'applique au même instant
  d'affichage, dans l'ordre d'arrivée des `Weapon` et `Armor` : nom non vide → la pile de ce nom
  est trouvée ou créée dans l'inventaire de l'avatar (les mêmes piles que l'arme et
  l'équipement : une par nom), puis équipée (`EquipStack`) sauf si elle l'est déjà ; nom vide →
  la pile équipée à l'emplacement 8 de l'avatar, s'il y en a une, est déséquipée
  (`UnEquipStack`), et la tenue par défaut revient. Un nom que le jeu ne connaît pas est
  journalisé une fois et laisse l'avatar inchangé. Le champ `équipement` du `Hello` n'est pas
  modifié : il reste appliqué à la création de l'avatar, et un `Armor` reçu ensuite le remplace
  pour l'emplacement 8.
- **Avatars** (v3) :
  - à l'entrée en session, chaque pair envoie le `Spawn` de son héros : l'hôte `Spawn{1, 0, sa
    pose du Welcome}` juste après le `Welcome`, l'invité `Spawn{2, 0, sa pose}` dès le `Welcome`
    reçu ;
  - **placement de l'invité** : à réception du `Welcome`, l'invité téléporte son héros à 2 m
    (200 unités) devant l'hôte, le long de l'axe avant (`sin lacet, 0, cos lacet`), tourné vers
    lui (`lacet + π`, ramené dans ]−π, π]). C'est cette pose que porte son `Spawn` ;
  - chaque pair n'accepte que le `Spawn` du `NetId` de l'autre, de `type` 0 et de pose finie,
    et que le `Despawn` de ce même `NetId` ; tout autre est ignoré (journalisé), sauf les
    `Spawn` de `type` 1 et les `Despawn` de PNJ reçus par l'invité (v9, « Répliques des PNJ »). Un `Spawn` reçu alors que l'avatar existe est ignoré :
    jamais deux avatars ;
  - un `Despawn` reçu retire l'avatar ; la session continue ;
  - fin volontaire en session : `Despawn{son NetId}` est envoyé juste avant `SessionEnd` ;
  - toute fin de session retire l'avatar du pair, `Despawn` reçu ou non.
- **Snapshots** (v4, AD-7) :
  - en session, chaque pair envoie un snapshot à la **première frame où son `tick` change**
    (20 Hz à l'horloge, jamais au nombre de frames), avec la pose de son héros lue à cette frame ;
    sans pose lisible, il n'envoie rien ;
  - un snapshot n'est accepté que du pair de session, une fois le KCP ouvert ; illisible, il est
    ignoré et compté ; une entité d'un autre `NetId` que celui de l'avatar (ou, chez l'invité,
    d'un PNJ annoncé : v9), ou à champ non fini, est ignorée et comptée ;
  - le récepteur affiche l'avatar avec **150 ms** de retard sur l'horloge distante, par
    interpolation (Hermite pour la position, chemin le plus court pour le lacet), extrapole au
    plus 250 ms après le dernier snapshot, puis fige la pose ; il n'attend jamais une
    retransmission. Un `Spawn` remet l'interpolation à zéro.
- **Répliques des PNJ** (v9, story 3.3) : l'hôte fait autorité sur les PNJ ; chez l'invité, ses
  propres PNJ (même sauvegarde) deviennent des répliques sans IA locale.
  - **Sélection** (hôte, à chaque snapshot) : les PNJ de `Entity::ms_arrNPCs`, sauf son héros et
    l'avatar de l'invité, vivants (`Entity::IsDead` = 0), de pose finie, à **50 m** (5000 unités)
    au plus de son héros ou de la pose affichée de l'avatar de l'invité (s'il existe), triés du
    plus proche au plus loin (distance au plus proche des deux joueurs). Le snapshot porte au plus
    54 entités, héros de l'hôte compris : les **53** PNJ les plus proches sont retenus, les autres
    attendent. Une lecture de la liste en échec laisse les PNJ annoncés tels quels et le snapshot
    ne porte que le héros.
  - **NetId** : attribué par l'hôte à la première sélection d'un GUID, à partir de **1000**, puis
    lié à ce GUID pour toute la session et jamais réutilisé pendant celle-ci (un PNJ qui revient
    dans le rayon reprend le même). Le compteur et la table repartent à chaque session.
  - **Annonce** : un PNJ qui entre dans la sélection est annoncé par `Spawn{NetId, 1, pose, guid}`
    et n'est porté par les snapshots qu'à partir du suivant (le `Spawn` passe par KCP, le snapshot
    part aussitôt : l'invité ignorerait l'entité, faute d'annonce) ; un PNJ annoncé qui en sort
    (hors du rayon, absent de la liste des PNJ du moteur depuis 2 s — 40 ticks —, ou évincé par plus proche) reçoit `Despawn{NetId}`,
    envoyé avant le snapshot du même tick, qui ne le porte plus. Depuis la v11, un PNJ annoncé qui
    sort parce qu'il est **mort** reçoit `Death{NetId}` et non `Despawn` (« Mort des répliques »). Pendant le délai de grâce, un PNJ absent n'est pas dans les snapshots ; revenu, il y reprend sa place sous le même NetId, sans nouveau `Spawn`. Il n'y a rien à envoyer à la fin de session : l'invité libère tout à `Ended`.
  - **PV** : chaque entrée du snapshot porte `PSSkills::GetHitPoints`, borné à [0, 65535] (héros
    compris).
  - **Invité, `Spawn{1}`** : accepté seulement de l'hôte, `NetId` ≥ 1000, pose finie (sinon
    ignoré, journalisé) ; un `NetId` déjà annoncé est ignoré (journalisé). Le PNJ natif est
    retrouvé par `gCRuntimeInfo::GetEntity(guid + 4 octets nuls)`, inscrit comme réplique (IA
    figée : son `gCScriptRoutine_PS::OnProcess` n'est plus appelé) et placé à la pose du `Spawn`.
    **GUID introuvable** : ignoré, journalisé une fois par `NetId` ; ses snapshots sont ignorés et
    le PNJ éventuel de l'invité garde son IA.
  - **Invité, snapshots** : les entités d'un `NetId` annoncé suivent la même horloge distante et le
    même retard de **150 ms** que l'avatar (interpolation propre à chaque PNJ, remise à zéro par
    son `Spawn`) ; la réplique est placée par `Entity::MoveTo` à chaque frame, et ses PV sont
    écrits (`SetHitPoints(pv, fixer)`) quand la valeur affichée change. Une entité d'un `NetId`
    non annoncé est ignorée et comptée.
  - **Libération** : sur `Despawn{NetId}` (un `NetId` non annoncé est ignoré, journalisé), et pour
    toutes les répliques à `Ended` (fin de session ou de monde), la réplique sort du registre (IA
    rendue) et est oubliée ; elle n'est jamais détruite. Une réplique morte (v11) est libérée de la
    même façon et reste morte ; aucune réplique n'est tuée par `Kill`.
- **Animations des PNJ** (v10, story 3.4) : mêmes messages (`AniName`, `Motion`, `MotionParam`),
  avec le `NetId` du PNJ ; l'hôte seul les émet.
  - **Hôte, PNJ animés** : à chaque sélection (snapshot), les **8** premiers PNJ retenus (les plus
    proches d'un des deux joueurs) sont animés ; un PNJ entre dans ce groupe ou en sort à la
    sélection suivante, et en sort aussi à son `Despawn` et à la fin de session. Seuls les
    `PlayMotion` et réglages de locomotion d'un PNJ animé sont envoyés, à partir du `PlayMotion`
    suivant son entrée dans le groupe ; ceux d'un PNJ qui n'y est plus sont jetés.
  - **Hôte, émission** : un `Motion` part comme celui du héros (daté en `ms`, `AniName` avant le
    premier usage d'un nom). L'espace des `ani_id` est **unique par émetteur** : héros et PNJ
    partagent le même. Les `MotionParam` d'un PNJ sont regroupés : au plus un par frame et par
    (type, nature), le dernier ; un `Motion` d'un type fait d'abord partir les réglages en attente
    de ce type pour ce PNJ (leur ordre est gardé). Le filtre des réglages identiques est celui du
    héros (mêmes écarts, valeurs finales 0 ou 1 toujours envoyées, premier réglage après un `Motion`
    du type), tenu par PNJ ; il repart à neuf quand le PNJ rentre dans le groupe.
  - **Budget** : 32 Ko/s au plus pour l'ensemble des `Motion`, `MotionParam` et `AniName` des PNJ
    (octets des messages, sans l'en-tête KCP). Un dépassement est journalisé, une fois par bilan de
    5 s ; rien n'est retenu ni retardé.
  - **Invité** : un `Motion` ou un `MotionParam` d'un `NetId` ≥ 1000 n'est accepté que d'un PNJ
    annoncé (`Spawn{1}`) et pas encore retiré (`Despawn`, `Ended`) ; sinon il est ignoré et compté.
    Il est rejoué sur la réplique avec les règles de l'avatar, appliquées **par PNJ** : 150 ms de
    retard (même horloge distante), seul le plus récent `Motion` des types 0 et 1 qui partent à la
    même frame est rejoué, les réglages des types 0/1 placés avant lui sont écartés. Le moteur de
    l'invité n'anime jamais une réplique (ses `PlayMotion` et réglages de locomotion sont bloqués) :
    hors du groupe des 8, elle reste figée dans sa dernière animation. À sa libération, sa file
    d'animations est vidée et son moteur l'anime de nouveau.
- **Mort des répliques** (v11, story 3.4b) : l'hôte fait autorité sur la mort des PNJ.
  - **Hôte** : un PNJ annoncé qui sort de la sélection parce qu'il est mort (`Entity::IsDead` = 1)
    reçoit `Death{NetId}` à la place de `Despawn`, envoyé avant le snapshot du même tick, une fois
    par `NetId` ; il sort du groupe des 8 animés au même tick. Ce `NetId` est mort jusqu'à la fin
    de la session : jamais réannoncé ni porté par un snapshot, même si le PNJ redevient vivant ou
    revient dans le rayon. Un PNJ qui meurt hors de la sélection (hors du rayon, ou pendant son
    délai de grâce « absent de la liste ») a déjà reçu `Despawn` : rien de plus n'est envoyé.
  - **Invité, réception** : `Death{NetId ≥ 1000}` n'est accepté que d'une réplique annoncée, ni
    morte ni déjà en attente de mort ; sinon il est ignoré et journalisé (`NetId` non annoncé,
    réplique déjà morte). L'hôte ignore tout `Death` d'un `NetId` ≥ 1000.
  - **Invité, application** : la mort s'applique au même instant d'affichage que le dernier
    snapshot reçu de cette réplique (aussitôt si aucun), après les PV et gestes dus et la dernière
    pose ; les gestes encore en attente sont jetés. La réplique meurt **une seule fois**, sans `Kill`
    natif (ni XP, ni script, ni quête chez l'invité) : PV à 0 (`SetHitPoints(0, fixer)`),
    `AIMode` = 12, puis ragdoll (`SetRagDollEnabled(1, 0, nul)`). Elle n'est plus ni déplacée, ni
    animée, ni ses PV écrits ; ses `Motion`/`MotionParam` reçus ensuite sont ignorés et comptés, ses
    entrées de snapshot ignorées. Elle reste dans la table jusqu'à `Ended` : un `Spawn{1}` de ce
    `NetId` est refusé (« déjà annoncé ») ; un `Despawn` la libère normalement. À `Ended`, elle est
    libérée comme les autres et reste morte. Le butin reste propre à chaque joueur : chacun pille
    son propre PNJ natif.
  - **PV d'une réplique vivante** : une valeur affichée de 0 (humain à terre chez l'hôte) est
    écrite **1** ; seule la mort met 0 PV.
- **Coups de l'invité** (v12, story 3.5) : l'hôte fait autorité sur les PV des PNJ ; un coup de
  l'invité n'est qu'une demande.
  - **Invité** : un coup natif de son héros sur une réplique de PNJ annoncée, vivante et sans mort
    en attente est bloqué localement (la réplique ne perd aucun PV de son fait) et devient
    `HitRequest{NetId}`, une fois par coup et par cible. Tout autre coup natif sur une réplique est
    bloqué sans rien émettre. Hors `InSession`, rien n'est émis. Les PV de la réplique suivent
    ensuite les snapshots de l'hôte, et sa mort le `Death` de l'hôte.
  - **Hôte** : `HitRequest{NetId}` n'est accepté que d'un `NetId` ≥ 1000, annoncé et pas mort ;
    sinon il est ignoré et journalisé (`NetId` inconnu, PNJ mort, `NetId` < 1000). Il est appliqué
    par le jeu de l'hôte : `PSDamage::DoDamage(arme en main de l'avatar de l'invité, PNJ, avatar)`,
    dégâts calculés par les scripts de l'hôte. Sans avatar, sans arme en main ou sans PNJ trouvé,
    le coup est ignoré (journalisé) ; l'hôte n'écrit jamais directement les PV d'un PNJ. Tout coup
    natif dont l'agresseur est l'avatar de l'invité est bloqué chez l'hôte : seuls les
    `HitRequest` blessent. La mort éventuelle suit la règle « Mort des répliques ».
  - Un invité qui reçoit `HitRequest` l'ignore. Pas de limite de cadence.
- **Coups subis par l'invité** (v13, story 3.6) : l'hôte fait autorité sur les combats ; les PV du
  héros de l'invité ne baissent que par `HitOnGuest`.
  - **Hôte** : l'avatar de l'invité ne perd jamais de PV au bilan. Un coup natif du héros de l'hôte
    (pas de tir ami) ou d'un agresseur nul est bloqué, rien n'est émis. Sinon le coup est **mesuré** :
    l'avatar est remis à ses PV max, le coup natif s'applique (sa mise à terre ou sa mort est
    annulée), la perte de PV est envoyée dans `HitOnGuest{source, dégâts}`, une fois par coup, puis
    les PV d'avant sont rendus. `source` = `NetId` du PNJ agresseur s'il est annoncé, sinon 0. Une
    perte nulle n'émet rien. Hors `InSession`, rien n'est émis.
  - **Invité** : tout coup natif sur son héros est bloqué, quel que soit l'agresseur (v15 : sauf le
    coup d'une réplique, appliqué pour son rendu seulement puis annulé, « Attaques natives des
    répliques »). Un
    `HitOnGuest` reçu en session retire `damage` aux PV de son héros ; `damage = 0` ne change rien.
    Un coup mortel laisse le héros à **1 PV** et le met à terre (v14, « Mort et réapparition de
    l'invité ») : il ne meurt jamais nativement. Un `HitOnGuest` reçu à terre est ignoré.
  - Un hôte qui reçoit `HitOnGuest` l'ignore. Pas de limite de cadence.
- **Mort et réapparition de l'invité** (v14, story 3.7) : l'invité ne meurt jamais nativement sous
  un coup ; il passe **à terre**, puis l'hôte le fait réapparaître près de lui, PV pleins.
  - **Invité, mise à terre** : un `HitOnGuest` mortel, ou un coup natif mortel intercepté dans
    `DownOrDeadBy*`, laisse le héros à **1 PV** et le met à terre. Il envoie `Death{2}` **une seule
    fois** : à terre, les mises à terre suivantes n'envoient rien (verrou jusqu'au `Respawn`), et les
    `HitOnGuest` reçus sont ignorés. Jamais 0 PV, ni `Kill`, ni ragdoll.
  - **Hôte, ordre** : un `Death{2}` reçu avec l'avatar présent, hors réapparition en cours, rend une
    réapparition due : au premier tick où la pose de son héros est lisible, l'hôte envoie
    `Respawn{2, pose, 0}`, avec `pose` = 2 m devant son héros, tournée vers lui (la règle du
    « placement de l'invité »), place aussitôt l'avatar à cette pose, remet son interpolation à zéro
    et **attend l'accusé**. Pas de ragdoll de l'avatar. Un `Death{2}` sans avatar ou pendant une
    réapparition en cours est ignoré (journalisé).
  - **Invité, ordre reçu** : accepté seulement à terre, héros non mort nativement, `net_id` = 2 et
    pose finie (sinon ignoré, journalisé). Le héros est téléporté à la pose (`Entity::MoveTo`), ses PV
    remis au maximum, l'état à terre levé, et l'invité renvoie l'**accusé** `Respawn{2, même pose,
    tick}` avec `tick` = son tick de snapshot courant + 1 : le premier de ses snapshots qui porte la
    nouvelle pose.
  - **Hôte, accusé et snapshots périmés** : en attente de l'accusé, toute entrée de snapshot du
    `NetId` 2 est ignorée (l'avatar reste à la pose du `Respawn`). L'accusé reçu met fin à l'attente,
    remet l'interpolation à zéro et fixe un tick minimal : ensuite, une entrée du `NetId` 2 de tick
    inférieur est ignorée (périmée). Un accusé hors attente est ignoré (journalisé).
  - **Replis** : l'invité à terre depuis **10 s** sans `Respawn` (et sans mort native) remet ses PV
    au maximum sur place et lève l'état à terre ; un `Respawn` arrivé ensuite est ignoré. L'hôte sans
    accusé depuis **5 s** reprend les snapshots de l'invité (tick minimal 0).
  - **Mort native** (chute, eau profonde : hors `DoDamage` et `DownOrDeadBy*`) : quand
    `Entity::IsDead` passe à 1 chez l'invité, il envoie `Death{2}` s'il ne l'a pas déjà fait (même
    verrou). Le héros est mort pour son jeu : l'invité refuse le `Respawn` (pas de relève d'un mort)
    et la session se termine à son rechargement (fin de monde, `SessionEnd{2}`) ; l'hôte, qui a
    replacé l'avatar, se réarme. Limite connue de la v14.
  - Tout cet état repart de zéro à l'entrée en session, au retrait de l'avatar et à la fin de
    session.
- **Attaques natives des répliques** (v15, story 3.8) : rendu des coups des monstres chez l'invité ;
  les dégâts restent ceux de `HitOnGuest`.
  - **Hôte, détection** : à chaque frame, pour chaque PNJ capturé (groupe des 8 animés, « Animations
    des PNJ »), il lit `PSCombatSystem::GetLastAttackMove` et `GetCurrentMove`. Une attaque commence
    quand le dernier mouvement d'attaque change depuis la frame précédente, ou quand le mouvement en
    cours redevient égal au dernier mouvement d'attaque après avoir valu autre chose ; un mouvement 0
    n'est jamais émis, et la première lecture d'un PNJ ne fait que l'initialiser. Il envoie alors
    `NpcAttack{NetId, mouvement, cible}` une fois, en session seulement, pour un `NetId` annoncé et pas
    mort ; `cible` = `PSCombatSystem::GetCurrentTarget` traduite : héros de l'hôte → 1, avatar de
    l'invité → 2, PNJ capturé → son `NetId`, sinon 0.
  - **Invité, réception** : `NpcAttack` n'est accepté que d'une réplique annoncée, ni morte ni en
    attente de mort ; sinon il est ignoré et compté. Il s'applique au même instant d'affichage que le
    dernier snapshot reçu de cette réplique (aussitôt si aucun), après les gestes dus : cible 1 →
    avatar de l'hôte, 2 → son héros, ≥ 1000 → la réplique de ce `NetId`, 0 (ou cible introuvable) →
    aucune cible fixée ; `PSCombatSystem::SetCurrentTarget(cible)` puis `StartMove(mouvement)` sur la
    réplique. L'animation visible reste celle rejouée depuis l'hôte (`Motion`).
  - **Invité, coup qui en résulte** (« coup cosmétique ») : un coup natif d'une réplique sur son héros
    est appliqué pour son rendu seulement (effet d'impact, script de dégâts : recul, flash), puis ses
    PV d'avant sont rendus ; une mise à terre ou une mort pendant ce coup est annulée. Au bilan, ses PV
    ne baissent que par `HitOnGuest`. Le recul générique de la v13 (`StartStumble`) n'est joué, à un
    `HitOnGuest` non mortel, que 500 ms après lui et si aucun coup cosmétique n'a touché le héros dans
    les 500 ms avant ou après (un `HitOnGuest` plus récent remplace l'attente). Héros à terre : coups
    des répliques bloqués comme en v14. Un coup natif sur l'avatar de l'hôte chez l'invité (une réplique peut désormais
    l'attaquer) est bloqué : l'hôte fait autorité sur ses PV.
  - Clé de configuration `native_attacks = 0` (diagnostic) : ni envoi ni application de `NpcAttack`
    (reçus ignorés et comptés), tout coup natif sur le héros invité est bloqué (comportement v14).
  - Un hôte qui reçoit `NpcAttack` d'un `net_id` autre que 2 l'ignore (Warn). Pas de limite de cadence.
- **Attaques natives de l'avatar** (v15, story 3.8b) : symétrique, rendu des coups de l'invité chez
  l'hôte (sang, effet d'impact sur le monstre) ; les dégâts restent ceux de `HitRequest`.
  - **Invité, détection** : à chaque frame en session, même règle sur son héros (`GetLastAttackMove`,
    `GetCurrentMove`) ; un début d'attaque envoie `NpcAttack{2, mouvement, cible}` une fois, sauf
    héros à terre ; `cible` = `PSCombatSystem::GetCurrentTarget` traduite : avatar de l'hôte → 1,
    réplique → son `NetId`, sinon 0.
  - **Hôte, réception** : accepté seulement avec un avatar présent et pas mort, invité pas à terre
    (refusé entre `Death{2}` et l'accusé de `Respawn`) ; au plus 8 en attente (au-delà : ignoré,
    compté), file vidée (comptée) au retrait de l'avatar, au `Death{2}` et à l'accusé de `Respawn`.
    Une cible déjà morte chez l'hôte (`Death` envoyé) est remplacée par 0. Chaque attaque s'applique
    au même instant d'affichage que le dernier snapshot reçu de l'invité (aussitôt si aucun), après
    l'arme, les gestes et la pose dus : arme du dernier `Weapon` reprise si la main est vide, puis
    cibles fixées (`PSNpc::SetCurrentTarget` et `PSCombatSystem::SetCurrentTarget` : PNJ capturé de
    ce `NetId` ; 1, 0 ou PNJ non capturé → cibles effacées) et `StartMove(mouvement)`. Sans arme en
    main : ignorée (comptée au journal).
  - **Hôte, coup qui en résulte** : un coup natif de l'avatar reste bloqué (effet d'impact sur un
    PNJ, aucun sur le héros de l'hôte) ; seuls les `HitRequest` blessent.
  - `native_attacks = 0`, d'un côté ou de l'autre : ni envoi ni application (reçus ignorés et
    comptés ; une ligne Info par session chez celui qui les ignore).
- **Dialogues** (v16, story 4.1) : l'invité voit et entend les répliques des dialogues de l'hôte ;
  rien d'autre du dialogue n'est synchronisé (ni ses effets, ni sous-titres, ni caméra).
  - **Hôte, détection** : chaque appel de `gCScriptProcessingUnit::StartOutput` est relevé (locuteur =
    `GetSelfEntity`, échantillon, type) avec l'état du dialogue **à cet instant** (`InfoSystem::IsRunning`,
    `InfoSystem::GetNPC`). La frame n'émet que si un dialogue était en cours et que le locuteur est son
    héros (`locuteur` = 1) ou le partenaire de dialogue ; les commentaires ambiants des PNJ ne partent
    pas et ne sont pas comptés. Pendant un dialogue, un autre locuteur est ignoré et compté. Le
    partenaire est traduit par son GUID en NetId : un PNJ non annoncé à l'invité, ou mort, est ignoré
    et compté. Un échantillon invalide (§4) n'est pas envoyé. `DialogLine` part une fois par réplique, en session
    seulement.
  - **Invité, rejeu** : aussitôt reçu (pas de retard d'affichage), `DialogLine` est rejoué sur l'avatar
    de l'hôte (`locuteur` = 1) ou sur la réplique de ce `NetId` (≥ 1000) :
    `gCScriptProcessingUnit::sStartOutput(entité, échantillon, type)` puis
    `Entity::StartFacialAni(échantillon)` (voix et lèvres). Jamais `StartSay` (il recalcule l'échantillon
    avec la voix locale), ni caméra, menu ou état `ZS_Talk`. Ignoré et compté : avatar absent ou mort,
    réplique non annoncée ou morte, entité à plus de **20 m** du héros de l'invité, invité lui-même en
    dialogue (`InfoSystem::IsRunning`).
  - Clé `dialogues = 0` (diagnostic), d'un côté ou de l'autre : ni envoi ni rejeu, ni tenue ni
    libération (reçus ignorés et comptés ; une ligne Info par session chez celui qui les ignore).
- **Dialogues de l'invité** (v17, story 4.2) : symétrique, plus la tenue du PNJ. Le dialogue se
  déroule dans le jeu de l'invité (ses effets y restent) ; l'hôte le voit et l'entend.
  - **Invité, détection** : à chaque frame, `InfoSystem::IsRunning` et `InfoSystem::GetNPC`. Un
    dialogue qui commence avec une réplique vivante (NetId ≥ 1000) envoie `DialogState{NetId, 1}` ;
    sa fin (ou un changement de partenaire, ou la fin de session) `DialogState{NetId, 0}`. Ses répliques
    suivent la règle de l'hôte (dialogue en cours au moment de la réplique, locuteur = son héros ou le
    partenaire) : `DialogLine{2, …}` pour son héros, `DialogLine{NetId, …}` pour la réplique partenaire.
    Une réplique dite après la fin du dialogue, ou par un PNJ qui n'est pas une réplique, n'est pas
    envoyée (comptée).
  - **Invité, libération** : pendant son dialogue, la réplique partenaire sort du blocage de routine et
    d'animation (son IA et ses animations natives jouent le dialogue) ; ses poses, PV, `Motion`,
    `MotionParam` et `NpcAttack` venus de l'hôte sont ignorés (le dernier `Motion` de chaque type est
    gardé et rejoué au re-gel). À la fin, elle revient au registre (IA figée) et ses PV sont réécrits au
    prochain instant d'affichage. Un `Death` de cette réplique la rend d'abord au
    registre puis la tue. Une fin de session libère toutes les répliques, comme toujours.
  - **Hôte, tenue** : à `DialogState{NetId, 1}` d'un PNJ annoncé et vivant, sa routine est figée (IA
    bloquée, comme une réplique ; voix et lèvres jouent toujours). Ignoré et compté : PNJ non annoncé
    ou mort, PNJ en dialogue avec l'hôte lui-même. Un seul PNJ tenu à la fois : un nouveau
    `DialogState{…, 1}` rend d'abord le précédent. Le PNJ est rendu à `DialogState{NetId, 0}`, à sa
    sortie de la sélection (`Despawn` ou `Death`), à la fin de session, quand l'hôte se met à lui parler,
    ou après **60 s** sans activité du dialogue (Warn ; chaque `DialogLine` de l'invité accepté et chaque
    `DialogState{NetId, 1}` répété relancent ce délai). Un `DialogState{…, 0}` pour un PNJ qui n'est pas
    tenu est ignoré. Hôte lui-même en dialogue avec un partenaire illisible : tenue refusée.
  - **Refus silencieux** : un refus de l'hôte (PNJ non annoncé, mort, en dialogue avec l'hôte…) n'est
    pas signalé à l'invité ; seul le journal de l'hôte dit pourquoi, et l'hôte ne voit ni n'entend alors
    rien du PNJ (l'avatar de l'invité reste rejoué).
  - **Fin de session** : aucun `DialogState{NetId, 0}` n'est envoyé ; chaque côté nettoie à `Ended`
    (hôte : PNJ rendu ; invité : répliques libérées, rendues au jeu).
  - **Hôte, rejeu** : `DialogLine{2, …}` est rejoué sur l'avatar de l'invité, `DialogLine{NetId, …}`
    sur le PNJ natif de ce NetId s'il est le PNJ tenu ; mêmes règles que chez l'invité (20 m du héros
    de l'hôte, hôte pas lui-même en dialogue, entité présente et vivante). `DialogLine{1, …}` reçu par
    l'hôte : ignoré (Warn une fois par session, puis Debug).
  - **Gestes et pose du partenaire** (invité → hôte) : tant qu'elle est le partenaire en cours, chaque
    `PlayMotion` et réglage de locomotion que le moteur de l'invité joue sur la réplique libérée part
    en `Motion{NetId}` / `MotionParam{NetId}` (même espace d'`AniId` et mêmes règles que ceux de son
    héros), et sa pose entre dans les snapshots de l'invité (entrée `NetId`, PV non lus). Chez l'hôte,
    ceux du PNJ tenu sont rejoués sur lui au même instant d'affichage que l'avatar (gestes, puis
    `MoveTo`), sans être renvoyés à l'invité ; ceux d'un autre NetId ≥ 1000 sont ignorés (comptés).
    Files vidées quand le PNJ est rendu. Pas de nouveau message : formats inchangés.
  - Un invité qui reçoit `DialogState` ou `DialogLine{2, …}` l'ignore (Warn une fois par session, puis
    Debug).
- **Quitter le jeu** n'envoie rien (aucun travail réseau à la fin du processus) : le pair
  conclut au timeout de 5 s.

### Réarmement

- Après une fin et son maintien de 1 s, la session revient à l'état inactif. Elle se réarme
  (hôte : nouvelle écoute ; invité : nouveaux `Hello`) au **monde chargé suivant**, avec un
  `conv` tiré à neuf par l'hôte.
- Exception : l'**hôte** dont l'invité part dans le même monde (timeout ou `SessionEnd` reçu) se
  réarme aussitôt, sans attendre un rechargement.
- L'**invité** attend toujours le monde suivant (après `Reject`, hôte injoignable, timeout ou
  `SessionEnd` reçu) : pas de nouvel essai automatique dans le même monde.
  Conséquence voulue (décision du 2026-10-17) : quand l'hôte recharge ou meurt, l'invité reçoit
  `SessionEnd{2}` ou `{3}`, revient au solo et ne se reconnecte qu'à son **propre** prochain
  chargement. Pas de reconnexion automatique (v1, inchangé en v17).
