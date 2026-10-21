# Vecteurs de référence du protocole v9

Tampons binaires partagés par les tests C++ (`client/tests/test_protocol.cpp`) et Go
(faux invité, story 1.3). Ils sont la référence **indépendante de l'encodeur** : chaque
`.bin` est produit par `xxd -r` à partir d'un hexdump écrit à la main dans
[`../PROTOCOL.md`](../PROTOCOL.md) (section 7), jamais par le codec testé.

## Contenu

| Fichier | Message | Attendu |
|---|---|---|
| `hello.bin` | `Hello` v9, `"0.1.0"`, empreintes Steam x64 2023, `"Leo"`, `"It_Sword_01"` | décodé, réencodé à l'octet près |
| `welcome.bin` | `Welcome` `conv=0x12345678`, `(100.0, -50.5, 2000.0)`, lacet `1.5` | décodé, réencodé à l'octet près |
| `reject.bin` | `Reject` raison 2 | décodé, réencodé à l'octet près |
| `heartbeat.bin` | `Heartbeat` (fiable, sans canal) | décodé, réencodé à l'octet près |
| `session_end.bin` | `SessionEnd` raison 1 (fiable, sans canal) | décodé, réencodé à l'octet près |
| `motion.bin` | `Motion` v2 (fiable, sans canal), `net_id=2`, `type=0`, `ani_id=7`, descripteur présent | décodé, réencodé à l'octet près |
| `spawn.bin` | `Spawn` v3 (fiable, sans canal), `net_id=2`, `type=0`, `(100.0, -50.5, 2000.0)`, lacet `1.5` | décodé, réencodé à l'octet près |
| `spawn_npc.bin` | `Spawn` v9 de type 1 (PNJ), `net_id=1000`, `(100.0, -50.5, 2000.0)`, lacet `1.5`, GUID de 16 octets | décodé, réencodé à l'octet près |
| `despawn.bin` | `Despawn` v3 (fiable, sans canal), `net_id=2` | décodé, réencodé à l'octet près |
| `aniname.bin` | `AniName` v5 (fiable, sans canal), `ani_id=7`, nom de course relevé par la sonde | décodé, réencodé à l'octet près |
| `death.bin` | `Death` v5 (fiable, sans canal), `net_id=2` | décodé, réencodé à l'octet près |
| `motion_param.bin` | `MotionParam` v6, `net_id=2`, `ms=1234`, poids cible 1.0 en 0,25 s sur le type 0 | décodé, réencodé à l'octet près |
| `weapon.bin` | `Weapon` v7, `net_id=2`, `"It_1H_Sword"` | décodé, réencodé à l'octet près |
| `weapon_sheathed.bin` | `Weapon` v7, `net_id=2`, nom vide (arme rangée) | décodé, réencodé à l'octet près |
| `armor.bin` | `Armor` v8, `net_id=1`, `"It_Armor_Inquisitor"` | décodé, réencodé à l'octet près |
| `armor_removed.bin` | `Armor` v8, `net_id=1`, nom vide (aucune armure) | décodé, réencodé à l'octet près |
| `snapshot.bin` | snapshot v4 (canal 1), `tick=1234`, une entité `net_id=2`, `(100.0, -50.5, 2000.0)`, lacet `1.5`, `pv=75` | décodé, réencodé à l'octet près |
| `snapshot_multi.bin` | snapshot v9, `tick=1234`, héros de l'hôte (`pv=75`) et PNJ 1000 (`pv=150`) et 1001 (`pv=65535`) | décodé, réencodé à l'octet près |
| `hello_future.bin` | `Hello` `version_protocole=0xFFFF`, suite illisible | `peek_hello_version` → 65535, décodage `UnsupportedVersion` |
| `hello_truncated.bin` | 22 premiers octets de `hello.bin` | `Truncated` |
| `hello_long_nickname.bin` | `hello.bin` avec un pseudo de 33 octets | `BadLength` |
| `unknown_type.bin` | contrôle, `MsgType` 99 | `UnknownType` |
| `motion_bad_type.bin` | `motion.bin` avec `type=8` | `BadValue` |
| `aniname_empty.bin` | `AniName` au nom vide | `BadLength` |
| `motion_param_bad.bin` | `motion_param.bin` avec `nature=4` | `BadValue` |
| `spawn_bad_type.bin` | `spawn.bin` avec `type=2` | `BadValue` |
| `snapshot_truncated.bin` | `snapshot.bin` annonçant 2 entités | `Truncated` |

Les valeurs détaillées champ par champ sont dans `PROTOCOL.md`.

## Régénération et vérification

```bash
protocol/vectors/regen.sh            # réécrit les .bin depuis les hexdumps de PROTOCOL.md (xxd -r)
protocol/vectors/regen.sh --check    # vérifie que `xxd <f>.bin` redonne exactement chaque hexdump
```

Pour modifier ou ajouter un vecteur : écrire l'hexdump à la main dans `PROTOCOL.md`, au format
exact de `xxd` (offset, groupes de 2 octets, colonne ASCII), précédé de
`<!-- vector: <nom>.bin -->`, puis lancer `regen.sh` et `regen.sh --check`. Les tests C++ lisent
ce dossier via la définition `MP_VECTORS_DIR`.
