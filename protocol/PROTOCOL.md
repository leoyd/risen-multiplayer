# Protocole Risen Multiplayer — version 1

Contrat binaire entre l'hôte et l'invité (DLL `Script_Multiplayer.dll`) et le faux invité de
test (`tools/fake-guest/`, Go). Ce document fait foi : le codec C++ (`client/src/core/protocol.*`)
et le codec Go le suivent, et les vecteurs de `protocol/vectors/` en sont tirés.

La v1 couvre **uniquement la session** (poignée de main, présence, fin de session). Les
snapshots, animations, apparitions et combats arrivent avec les epics suivants et
incrémenteront la version.

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
| Snapshot | `1` | état complet à 20 Hz, non fiable — **non défini en v1** | Epic 2 |
| Contrôle | `2` | `Hello`, `Welcome`, `Reject`, hors KCP | 1.1 |

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
| 18–25 | *réservés* | fiable | `AniName`, `Motion`, `Spawn`, `Despawn`, `HitRequest`, `HitOnGuest`, `Death`, `Respawn` — **non définis en v1** |

Toute autre valeur, les valeurs réservées, et un type reçu sur le mauvais canal (ex.
`Heartbeat` en contrôle) sont refusés comme **type inconnu**. Ajouter un type exige de
mettre à jour ce document et d'incrémenter la version du protocole.

## 4. Charges

### `Hello` (1) — contrôle

| Champ | Type | Contrainte |
|---|---|---|
| `version_protocole` | `u16` | `1` en v1 — **position figée pour toujours** |
| `version_mod` | `str8` | 0 à 255 octets, ex. `0.1.0` |
| empreinte Game.dll | `u32 TimeDateStamp`, `u32 SizeOfImage` | en-tête PE du module chargé |
| empreinte Engine.dll | `u32`, `u32` | idem |
| empreinte Script.dll | `u32`, `u32` | idem |
| empreinte SharedBase.dll | `u32`, `u32` | idem |
| `pseudo` | `str8` | **1 à 32** octets UTF-8 |
| `équipement` | `str8` | 0 à 255 octets (appliqué à partir de J2) |

Taille : 40 à 581 octets (canal compris).

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

**Raison inconnue** (`Reject` ou `SessionEnd`) : le message se décode quand même ; la valeur
brute est conservée et l'appelant la journalise telle quelle (« raison inconnue (200) »).

## 5. Décodage : refus

Le décodeur ne lit jamais hors du tampon. Il refuse avec une erreur typée :

| Erreur | Cas |
|---|---|
| `Truncated` | tampon plus court que le message (y compris tampon vide) |
| `UnknownChannel` | octet 0 ≠ 2 pour un datagramme de contrôle |
| `UnknownType` | `MsgType` inconnu, réservé ou hors de son canal |
| `UnsupportedVersion` | `Hello` dont `version_protocole` ≠ 1 (la suite n'est pas lue) |
| `BadLength` | longueur de `str8` hors bornes (ex. pseudo vide ou de 33 octets) |
| `TrailingBytes` | octets restants après la fin du message |

L'encodeur vérifie les mêmes bornes **avant** d'écrire et refuse (sans tronquer) un texte
hors bornes, une version autre que 1 ou un résultat de plus de 1200 octets.

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

`version_protocole=1`, `version_mod="0.1.0"`, empreintes de Risen Steam x64 (janvier 2023) :
Game `0x63c9aff6`/`0x01417000`, Engine `0x63ca945d`/`0x00d3a000`, Script `0x63c9b014`/`0x004d1000`,
SharedBase `0x63c569b6`/`0x0025a000`, `pseudo="Leo"`, `équipement="It_Sword_01"`.

<!-- vector: hello.bin -->
```text
00000000: 0201 0100 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63 00a0 d300 14b0 c963 0010  A.]..c.......c..
00000020: 4d00 b669 c563 00a0 2500 034c 656f 0b49  M..i.c..%..Leo.I
00000030: 745f 5377 6f72 645f 3031                 t_Sword_01
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` | canal contrôle |
| 0x01 | `01` | `MsgType` Hello |
| 0x02 | `01 00` | `version_protocole` = 1 |
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

### Invalides

#### `hello_v2.bin` — `Hello` d'une version future, 8 octets

`peek_hello_version` → 2 ; décodage complet → `UnsupportedVersion`. La suite (`ff ff ff ff`)
est volontairement illisible en v1.

<!-- vector: hello_v2.bin -->
```text
00000000: 0201 0200 ffff ffff                      ........
```

| Offset | Octets | Champ |
|---|---|---|
| 0x00 | `02` `01` | canal contrôle, `MsgType` Hello |
| 0x02 | `02 00` | `version_protocole` = 2 |
| 0x04 | `ff ff ff ff` | charge d'une autre version |

#### `hello_truncated.bin` — `Hello` coupé, 22 octets

Les 22 premiers octets de `hello.bin` (coupé après le `TimeDateStamp` d'Engine.dll) →
`Truncated` ; `peek_hello_version` → 1.

<!-- vector: hello_truncated.bin -->
```text
00000000: 0201 0100 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
00000010: 4101 5d94 ca63                           A.]..c
```

#### `hello_long_nickname.bin` — pseudo de 33 octets, 88 octets

Identique à `hello.bin` sauf `pseudo` = 33 × `A` (`21` puis 33 × `41`) → `BadLength`.

<!-- vector: hello_long_nickname.bin -->
```text
00000000: 0201 0100 0530 2e31 2e30 f6af c963 0070  .....0.1.0...c.p
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
