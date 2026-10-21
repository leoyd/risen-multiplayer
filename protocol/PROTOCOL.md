# Protocole Risen Multiplayer — version 7

Contrat binaire entre l'hôte et l'invité (DLL `Script_Multiplayer.dll`) et le faux invité de
test (`tools/fake-guest/`, Go). Ce document fait foi : le codec C++ (`client/src/core/protocol.*`)
et le codec Go le suivent, et les vecteurs de `protocol/vectors/` en sont tirés.

La v1 couvrait **uniquement la session** (poignée de main, présence, fin de session). La v2
(story 2.1) ajoute `Motion`, la v3 (story 2.2) `Spawn` et `Despawn`, la v4 (story 2.3) le
snapshot du canal 1, la v5 (story 2.4) `AniName` et `Death`, la v6 (story 2.4b) `MotionParam`, la v7 (story 3.2) `Weapon`. Les combats arrivent avec les
stories suivantes et incrémenteront la version.

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
| 19 | `Motion` | fiable | les deux sens : une animation lancée sur une entité dont l'émetteur a l'autorité (v2) |
| 20 | `Spawn` | fiable | les deux sens : apparition d'une entité dont l'émetteur a l'autorité (v3) |
| 21 | `Despawn` | fiable | les deux sens : retrait d'une entité annoncée par `Spawn` (v3) |
| 24 | `Death` | fiable | les deux sens : mort d'une entité dont l'émetteur a l'autorité ; le récepteur met la réplique en ragdoll (v5) |
| 26 | `MotionParam` | fiable | les deux sens : réglage de locomotion (poids cible, vitesse de lecture) d'une entité animée (v6) |
| 27 | `Weapon` | fiable | les deux sens : arme en main d'une entité dont l'émetteur a l'autorité, vide si rangée (v7) |
| 22, 23, 25 | *réservés* | fiable | `HitRequest`, `HitOnGuest`, `Respawn` — **non définis en v7** |

Toute autre valeur, les valeurs réservées, et un type reçu sur le mauvais canal (ex.
`Heartbeat` en contrôle) sont refusés comme **type inconnu**. Ajouter un type exige de
mettre à jour ce document et d'incrémenter la version du protocole.

## 4. Charges

### `Hello` (1) — contrôle

| Champ | Type | Contrainte |
|---|---|---|
| `version_protocole` | `u16` | `7` en v7 — **position figée pour toujours** |
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

### `Spawn` (20) — fiable — v3

Apparition d'une entité dont l'émetteur a l'autorité. En v3, chaque joueur n'annonce que son
propre héros (`type` 0), sous son propre `NetId` (AD-6) : `1` pour l'hôte, `2` pour l'invité.

| Champ | Type | Contrainte |
|---|---|---|
| `net_id` | `u32` | identifiant de l'entité (AD-6) |
| `type` | `u8` | `0` = héros d'un joueur ; gardé brut au décodage (l'appelant ignore un type inconnu) |
| position x, y, z | 3 × `f32` | unités du monde |
| `lacet` | `f32` | radians, `atan2(m[8], m[10])` (AD-7) |

Taille : 22 octets.

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

**Raison inconnue** (`Reject` ou `SessionEnd`) : le message se décode quand même ; la valeur
brute est conservée et l'appelant la journalise telle quelle (« raison inconnue (200) »).

## 4 bis. Snapshot (canal 1) — non fiable — v4

État **complet** des entités dont l'émetteur a l'autorité (AD-7), sans delta. En v4, chaque pair
n'y met que son héros, sous son propre `NetId`.

| Champ | Type | Contrainte |
|---|---|---|
| canal | `u8` | `1` |
| `tick` | `u32` | `floor((t − t0) / 50 ms)` à l'horloge monotone de l'émetteur, `t0` = son entrée en session |
| `nombre` | `u8` | **0 à 54** entités |
| puis, par entité (22 octets) : | | |
| `net_id` | `u32` | AD-6 |
| position x, y, z | 3 × `f32` | unités du monde |
| `lacet` | `f32` | radians, `atan2(m[8], m[10])` |
| `pv` | `u16` | points de vie (`0` en v4 : non lus avant J2) |

Taille : 6 + 22 × `nombre` octets, soit 1194 au plus. Le décodeur refuse un tampon trop court pour
`nombre` entités (`Truncated`), des octets en trop (`TrailingBytes`), un autre canal
(`UnknownChannel`) et un `nombre` > 54 (`BadValue`) ; l'encodeur refuse plus de 54 entités.
Tout datagramme de canal 1 venu du pair compte comme activité (§8), même illisible.

## 5. Décodage : refus

Le décodeur ne lit jamais hors du tampon. Il refuse avec une erreur typée :

| Erreur | Cas |
|---|---|
| `Truncated` | tampon plus court que le message (y compris tampon vide), ou que les `nombre` entités d'un snapshot |
| `UnknownChannel` | octet 0 ≠ 2 pour un datagramme de contrôle, ≠ 1 pour un snapshot |
| `UnknownType` | `MsgType` inconnu, réservé ou hors de son canal |
| `UnsupportedVersion` | `Hello` dont `version_protocole` ≠ 7 (la suite n'est pas lue) |
| `BadLength` | longueur de `str8` hors bornes (ex. pseudo vide ou de 33 octets, nom d'`AniName` vide) ; l'encodeur refuse aussi un nom de `Weapon` de plus de 255 octets |
| `TrailingBytes` | octets restants après la fin du message |
| `BadValue` | champ hors de son domaine (`Motion` : `type` > 7, bit inconnu dans `flags`, scalaire non nul sans le bit 1 ; snapshot : `nombre` > 54 ; `MotionParam` : `type` > 7, `nature` > 3, valeur ou durée hors domaine, durée non nulle pour une vitesse) |

L'encodeur vérifie les mêmes bornes **avant** d'écrire et refuse (sans tronquer) un texte
hors bornes, une version autre que 7, un champ hors domaine ou un résultat de plus de 1200
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

`version_protocole=6`, `version_mod="0.1.0"`, empreintes de Risen Steam x64 (janvier 2023) :
Game `0x63c9aff6`/`0x01417000`, Engine `0x63ca945d`/`0x00d3a000`, Script `0x63c9b014`/`0x004d1000`,
SharedBase `0x63c569b6`/`0x0025a000`, `pseudo="Leo"`, `équipement="It_Sword_01"`.

<!-- vector: hello.bin -->
```text
00000000: 0201 0700 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63 00a0 d300 14b0 c963 0010  A.]..c.......c..
00000020: 4d00 b669 c563 00a0 2500 034c 656f 0b49  M..i.c..%..Leo.I
00000030: 745f 5377 6f72 645f 3031                 t_Sword_01
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` | canal contrôle |
| 0x01 | `01` | `MsgType` Hello |
| 0x02 | `07 00` | `version_protocole` = 7 |
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
`Truncated` ; `peek_hello_version` → 7.

<!-- vector: hello_truncated.bin -->
```text
00000000: 0201 0700 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63                           A.]..c
```

#### `hello_long_nickname.bin` — pseudo de 33 octets, 88 octets

Identique à `hello.bin` sauf `pseudo` = 33 × `A` (`21` puis 33 × `41`) → `BadLength`.

<!-- vector: hello_long_nickname.bin -->
```text
00000000: 0201 0700 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
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

#### `snapshot_truncated.bin` — snapshot annonçant 2 entités, 28 octets

`snapshot.bin` avec `nombre` = 2 (offset 0x05) : une seule entité suit → `Truncated`.

<!-- vector: snapshot_truncated.bin -->
```text
00000000: 01d2 0400 0002 0200 0000 0000 c842 0000  .............B..
00000010: 4ac2 0000 fa44 0000 c03f 4b00            J....D...?K.
```

## 8. Règles de session

Règles **normatives** depuis la v1 ; la v3 ajoute les avatars (`Spawn`, `Despawn`), la v4 les snapshots, la v5 les animations (`AniName`, `Motion`) et la mort (`Death`), la v6 les réglages de locomotion (`MotionParam`), la v7 l'arme en main (`Weapon`). Elles s'appliquent à la
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
    vivant ; un `ani_id` inconnu ou un scalaire non fini le fait ignorer (journalisé) ;
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
- **Mort** (v5) : quand le jeu déclare le héros mort (`Entity::IsDead` passe à 1), l'émetteur
  envoie `Death{son NetId}`, une fois par mort (à nouveau après un retour à la vie). Un ragdoll sans
  mort (chute, renversement) n'envoie rien.
  Le récepteur l'applique au même instant d'affichage que la pose (celui du dernier snapshot reçu
  avec le `Death`), après les gestes encore en attente : l'avatar passe en ragdoll, puis n'est plus
  ni déplacé ni animé jusqu'à son retrait (un `Spawn` suivant lève cet état). En pratique, seul
  l'invité envoie `Death` : la mort de l'hôte termine la session (raison 3) avant tout envoi, et
  l'invité retire alors l'avatar.
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
- **Avatars** (v3) :
  - à l'entrée en session, chaque pair envoie le `Spawn` de son héros : l'hôte `Spawn{1, 0, sa
    pose du Welcome}` juste après le `Welcome`, l'invité `Spawn{2, 0, sa pose}` dès le `Welcome`
    reçu ;
  - **placement de l'invité** : à réception du `Welcome`, l'invité téléporte son héros à 2 m
    (200 unités) devant l'hôte, le long de l'axe avant (`sin lacet, 0, cos lacet`), tourné vers
    lui (`lacet + π`, ramené dans ]−π, π]). C'est cette pose que porte son `Spawn` ;
  - chaque pair n'accepte que le `Spawn` du `NetId` de l'autre, de `type` 0 et de pose finie,
    et que le `Despawn` de ce même `NetId` ; tout autre est ignoré (journalisé). Un `Spawn` reçu alors que l'avatar existe est ignoré :
    jamais deux avatars ;
  - un `Despawn` reçu retire l'avatar ; la session continue ;
  - fin volontaire en session : `Despawn{son NetId}` est envoyé juste avant `SessionEnd` ;
  - toute fin de session retire l'avatar du pair, `Despawn` reçu ou non.
- **Snapshots** (v4, AD-7) :
  - en session, chaque pair envoie un snapshot à la **première frame où son `tick` change**
    (20 Hz à l'horloge, jamais au nombre de frames), avec la pose de son héros lue à cette frame ;
    sans pose lisible, il n'envoie rien ;
  - un snapshot n'est accepté que du pair de session, une fois le KCP ouvert ; illisible, il est
    ignoré et compté ; une entité d'un autre `NetId` que celui de l'avatar, ou à champ non fini,
    est ignorée ;
  - le récepteur affiche l'avatar avec **150 ms** de retard sur l'horloge distante, par
    interpolation (Hermite pour la position, chemin le plus court pour le lacet), extrapole au
    plus 250 ms après le dernier snapshot, puis fige la pose ; il n'attend jamais une
    retransmission. Un `Spawn` remet l'interpolation à zéro.
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
  chargement. Pas de reconnexion automatique (v1, inchangé en v7).
