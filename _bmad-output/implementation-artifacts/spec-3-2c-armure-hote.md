---
title: 'Story 3.2c — Armure portée, transmise à l’avatar'
type: 'feature'
created: '2026-10-21'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'be8f190e0ec92cc7f8cd2e7e8baddb867a90008a'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/docs/re/equipement.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Chez l’invité, l’avatar de l’hôte porte toujours la tunique de départ du héros, quelle que soit l’armure de l’hôte. De même, une armure que l’invité change en cours de session ne se voit pas chez l’hôte : `Hello.equipment` n’est appliqué qu’à la création de l’avatar.

**Décisions (utilisateur, 2026-10-21) :** story découpée de la 3.2b. Elle transmet l’armure portée par l’hôte à son avatar, **dans les deux sens** : un changement d’armure de l’invité en cours de session se voit aussi chez l’hôte.

**Approach:** Un message fiable `Armor{net_id, nom}` (protocole **v8**) circule dans les deux sens, sur le modèle de `Weapon`. Il porte le nom de l’armure équipée par le héros (emplacement d’équipement 8), vide s’il n’en porte pas. Le récepteur équipe cette armure sur l’avatar, ou la retire pour que la tunique par défaut revienne.

## Boundaries & Constraints

**Always :**
- Émission, à chaque frame : `FindStackIndex(gEEquipSlot 8)` du héros local, puis le nom de l’objet de cette pile (`GetTemplateItem` + `GetName`), vide si la pile vaut −1. `Armor` part au changement, puis une fois à l’entrée en session, après le `Spawn` et le `Weapon`. Une lecture en échec garde le dernier état et n’envoie rien.
- Réception, aux mêmes conditions et au même instant d’affichage qu’un `Weapon` (avatar du pair présent, vivant, sans `Death` en attente) :
  - nom non vide : pile trouvée ou créée (`ensure_stack`, partagée avec l’arme et l’équipement), puis `EquipStack` sauf si elle est déjà équipée ;
  - nom vide : `UnEquipStack` de la pile de l’emplacement 8 de l’avatar, s’il y en a une.
- L’inventaire de l’avatar n’est touché qu’une fois prêt : un `Armor` reçu avant est différé comme l’arme (`docs/re/equipement.md`).
- AD-3 : les symboles sont ajoutés à `resolve_inventory`, en tout ou rien. AD-10 : `PROTOCOL.md` v8, vecteurs, codecs C++ et Go, tests des deux côtés. Le jeu n’est appelé que depuis la frame (AD-2), sans log par frame hors `debug`.

**Never :**
- Pas d’autre emplacement (anneaux, amulettes, main gauche), pas de nouveau hook.
- `Hello.equipment` et `EquipHero`/`EquipAvatar` de la 3.2b restent inchangés.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Entrée en session | hôte en `It_Armor_Inquisitor` | `Armor` après `Spawn` ; avatar en armure, sans tunique | Template inconnu : log une fois par nom, avatar inchangé |
| Changement d’armure | A → B en session | nouvel `Armor` ; B équipée à la place de A, une pile par nom | N/A |
| Armure retirée | emplacement 8 vide | `Armor{""}` ; `UnEquipStack`, tunique par défaut de retour | Aucune pile équipée : sans effet |
| Avatar pas encore prêt | `Armor` dans les 15 premières frames | appliqué une fois l’inventaire prêt (dernier reçu) | N/A |
| Lecture du héros | SEH | dernier état gardé, log une fois | Pas d’`Armor` |
| `Armor` avant l’avatar, ou avatar mort | avatar absent ou mort | ignoré, log debug | N/A |
| Décodage | `Armor` tronqué ou nom > 255 octets | `Truncated` / refus | Message ignoré, log |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` : `kProtocolVersion` = 8. `Armor` (type 28) a la forme de `Weapon` (type 27, `net_id u32`, `nom str8` de 0 à 255 octets). Tests dans `client/tests/test_protocol.cpp`.
- `client/src/core/session.{h,cpp}` : la logique de l’arme (`LocalWeapon`, `on_local_weapon`, `send_weapon`, `on_peer_weapon`, `apply_due_weapons`, `pending_weapons_`, remises à zéro) est dupliquée pour `LocalArmor` → `Armor` et `Armor` → `AvatarArmor`. On factorise avec l’arme quand ça évite de copier : par exemple un état par type d’objet tenu ou porté, et une même file datée qui conserve l’ordre d’arrivée. Tests dans `client/tests/test_session.cpp`.
- `client/src/game/combat.{h,cpp}` : à ajouter à `InventoryAbi` / `resolve_inventory` :
  - `find_stack_by_equip_slot` (`?FindStackIndex@PSInventory@@QEBAHW4gEEquipSlot@@@Z`, enum passé en `int`) ;
  - `get_template_item` (`?GetTemplateItem@PSInventory@@QEBA?AVEntity@@H@Z`, sret après `this`), qui sort du diagnostic facultatif pour entrer dans le tout ou rien ;
  - `unequip_stack` (`?UnEquipStack@PSInventory@@QEAA_NH@Z`).
  La constante `kEquipSlotArmor = 8` est vérifiée par `GetEquipSlotFromStack` en 3.2b.
- `client/src/sync/avatar.{h,cpp}` :
  - `set_armor(net_id, nom)` sur le modèle de `set_weapon`. Il réutilise `ensure_stack` et l’attente d’inventaire prêt (`pending_*`, `apply_pending_inventory`, ordre équipement → armure → arme).
  - `local_armor(nom&)` lit l’armure du héros.
- `client/src/sync/session.cpp` : `AvatarArmor` dans `execute()`, et lecture de l’armure dans `local_captures()` à côté de l’arme (l. 242-266 : mêmes drapeaux, remise à zéro au changement de héros).
- `tools/fake-guest/` : codec `Armor` vérifié contre les vecteurs, `--armor NOM` (défaut vide = pas d’`Armor`, `-` = `Armor{""}`), directive de scénario `armor [instant] <nom|->`, renvoi en `--echo`, `Armor` reçus journalisés. Tests Go.
- `protocol/PROTOCOL.md` et `protocol/vectors/` (`regen.sh`) : v8 (§3, §4, §5, §7, §8 « Armure portée »), `armor.bin`, `armor_removed.bin`, vecteurs portant la version régénérés.
- `client/tests/session_fake_guest.sh`, `client/tests/session_host_main.cpp`, `client/tests/wine/run_scenarios.sh` : les `Armor` circulent, et les nouveaux exports sont vérifiés.
- `client/README.md` et `README.md` (racine, en anglais) : procédure de test en jeu, protocole v8, ligne 🛡️ du tableau.

## Tasks & Acceptance

**Execution :**
- [x] `client/src/core/protocol.{h,cpp}`, `client/tests/test_protocol.cpp`, `protocol/PROTOCOL.md`, `protocol/vectors/` : `Armor` en v8. Motif : AD-10.
- [x] `client/src/core/session.{h,cpp}` et `client/tests/test_session.cpp` : `LocalArmor` → `Armor` → `AvatarArmor`, cas de la matrice. Motif : logique testée sous Linux.
- [x] `client/src/game/combat.{h,cpp}` : les trois enveloppes, dans `resolve_inventory`. Motif : AD-3.
- [x] `client/src/sync/avatar.{h,cpp}` et `client/src/sync/session.cpp` : `set_armor`, `local_armor`, armure différée, exécution. Motif : matrice.
- [x] `tools/fake-guest/` : `Armor`, `--armor`, directive, écho, tests. Motif : banc.
- [x] `client/tests/session_fake_guest.sh`, `session_host_main.cpp`, `wine/run_scenarios.sh` : non-régression et `Armor`. Motif : banc.
- [x] `client/README.md`, `README.md` : test en jeu, protocole v8. Motif : utilisabilité.

**Acceptance Criteria :**
- Given les tests Linux, Go et le banc Wine, when on construit, then tout passe au vert et les vecteurs v8 concordent en C++ et en Go.
- Given Risen en hôte et le faux invité en `--echo`, when l’hôte change d’armure, puis la retire, then le faux invité journalise `Armor{nom}` puis `Armor{""}`. *(Manuel.)*
- Given Risen en hôte et `fake-guest --armor It_Armor_Inquisitor`, when l’avatar apparaît, then il porte cette armure sans tunique ; avec `armor -` dans un scénario, la tunique revient. *(Manuel.)*

## Implementation Notes

- `core/session` : arme et armure factorisées (`Carried`, un `CarriedState` par type, une file
  `pending_carried_` qui garde l'ordre d'arrivée des `Weapon` et `Armor`, bornée à
  `kMaxPendingCarried` ; pleine, elle évince le plus ancien du même type). Textes de log de l'arme inchangés.
- `sync/avatar` : `avatar_stack` (Template + `ensure_stack`, échec journalisé une fois par nom)
  partagé par `set_weapon` et `set_armor` ; `weapon_stacks`/`weapon_failed` renommés
  `item_stacks`/`item_failed`. `local_armor` traite comme « sans armure » une entité nulle rendue
  par `GetTemplateItem` pour une pile ≥ 0 (comme `local_weapon`).
- `session_host` : `--armor NOM`, `SIGHUP` bascule aussi l'armure. Vecteurs : `armor.bin` porte
  `net_id = 1` (hôte). La ligne `[hooks] inventaire de session résolu (arme de l'avatar)` est gardée
  telle quelle (attendue par le banc Wine).
- Non vérifié en jeu : le retour de la tunique après `UnEquipStack` (plan B :
  `docs/re/equipement.md`).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | edge, blind | `local_armor` : objet nul sans SEH → « illisible (SEH) » et armure figée | low | patch | Retour `false` alors que `local_weapon` traite ce cas comme vide ; aligné. |
| 2 | edge, blind, verif-gap | File partagée `Weapon`/`Armor` : un afflux de `Weapon` évince le seul `Armor` en attente | medium | patch | `pop_front()` sans regarder le type ; `Armor` n’est renvoyé qu’au changement. Éviction du même type, `kMaxPendingCarried`, test ajouté. |
| 3 | edge, blind, verif-gap | Contrôle « pas d’`AvatarArmor` sans `--armor` » hors du bloc : vert sans log | low | patch | Même défaut qu’en 3.2b ; déplacé dans le bloc. |
| 4 | blind | Rien ne montre que B remplace A sur l’avatar | low | patch | Inventaire journalisé en debug après chaque armure appliquée, étape ajoutée au README. |
| 5 | verif-gap, edge | Colle sync/game de l’armure et retour de la tunique vérifiés seulement en jeu | medium | defer | Même couture `game::` que 3.1 à 3.2b ; entrée dans `deferred-work.md`. |
| 6 | blind | `Entity` rendu par valeur jamais détruit (fuite chaque frame) | false | rejet | `??1Entity@@QEAA@XZ` (Script.dll @`18020f750`) est un simple `ret` : rien à libérer. |
| 7 | blind | Invité : `Armor` d’avant `EquipHero` envoyé à l’entrée, scintillement chez l’hôte | low | rejet | Les deux `Armor` arrivent pendant les 15 frames d’attente de l’avatar : seul le dernier est appliqué. |
| 8 | blind | `local_armor` confond « pas d’armure » et « pas d’inventaire » | low | rejet | Le héros a toujours un inventaire dans un monde chargé. |
| 9 | edge | Armure changée pendant que l’avatar est mort : perdue | low | rejet | Même règle que `Weapon` ; le retour à la vie relève de la 3.7 (`Respawn`). |
| 10 | edge | Armure différée perdue si l’avatar meurt dans les 15 premières frames | low | rejet | Un avatar mort n’est plus équipé (comme `equip`). |
| 11 | edge | `Armor` portant un template qui n’est pas une armure | low | rejet | Pair de confiance (coop à 2). |
| 12 | edge | Les 3 symboles dans le tout ou rien coupent aussi l’arme | false | rejet | Voulu par la spec (AD-3). |
| 13 | blind | `item_failed` partagé entre arme, équipement et armure | low | rejet | Seul le log est concerné ; un nom invalide l’est partout. |
| 14 | blind | Ligne « nom > 255 octets » de la matrice impossible au décodage | low | rejet | Correction de la partie figée de la spec ; l’encodeur refuse (testé). |
| 15 | blind | `SIGHUP` bascule arme et armure ensemble | low | rejet | Banc de test ; les tests unitaires couvrent chaque type seul. |
| 16 | blind | `start_host_armor` copie `start_host_weapon` | low | rejet | Duplication de script de test, sans défaut. |
| 17 | blind | Despawn pendant une armure différée non couvert | false | rejet | L’attente vit dans l’entrée de l’avatar, effacée au retrait. |

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : les 6 tests passent.
- `cd tools/fake-guest && go test ./...` : tout passe, vecteurs v8 compris.

**Manual checks :**
- En jeu, `probe = 0`, Risen en hôte et faux invité : relever les lignes `[sync] armure …`.
