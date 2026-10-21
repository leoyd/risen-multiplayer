---
title: 'Story 3.2b — Équipement de l’invité'
type: 'feature'
created: '2026-10-21'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: '08c006dedcdc0f975200a26f793b1ae12a2ea0f6'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** `guest_equipment` est lu comme du texte libre et n’a aucun effet. L’invité commence donc la coop sans l’équipement de sa config, et son avatar chez l’hôte est nu, sans armure.

**Décisions (utilisateur, 2026-10-20 et 2026-10-21) :** la story 3.2 a été découpée, puis la 3.2b aussi. L’armure portée par l’hôte, transmise à son avatar, part en story 3.2c (`deferred-work.md`). Sur le héros de l’invité, un objet n’est créé **que s’il manque**.

**Approach:** `guest_equipment` devient une liste de noms de templates, validée au chargement, et voyage dans le champ `Hello.equipment`, qui existe déjà. Chez l’invité, son héros est équipé à l’entrée en session. Chez l’hôte, l’avatar de l’invité est équipé dès sa création.

## Boundaries & Constraints

**Always :**
- Format : des noms séparés par des virgules, avec des espaces tolérés autour de chaque nom. Un nom compte 1 à 64 octets `[A-Za-z0-9_]`. On accepte 16 noms au plus, sans doublon, et 255 octets au plus dans la forme canonique `A,B` (sans espaces). Une valeur vide signifie aucun équipement. Une valeur invalide fait échouer le chargement de la config, avec `guest_equipment` comme champ fautif (comme les autres champs). La config garde la forme canonique, et c’est elle qui part dans `Hello`.
- Équiper une entité, nom par nom : `Template(nom)` puis `IsValid`. La pile est cherchée (`FindStackIndex(-1, false)`) ou, à défaut, créée (`CreateItems(…, 1, 0)`). Ensuite `EquipStack`, sauf si la pile est déjà équipée (`IsStackEquiped`). Un nom en échec est journalisé et n’empêche pas les suivants.
- L’invité équipe son héros à **chaque** entrée en session, après la téléportation. Le rôle hôte ignore sa propre `guest_equipment`.
- L’hôte équipe l’avatar de l’invité juste après sa création, avec les noms du `Hello` accepté. Les piles créées sont partagées avec l’arme de la 3.2 (une pile par nom et par avatar).
- Protocole : aucun message ni champ ajouté, la version reste **v7**. `PROTOCOL.md` documente le format du champ `équipement` et son application.
- AD-3 : `IsStackEquiped` est résolu en tout ou rien dans `resolve_inventory`. Le jeu n’est appelé que depuis la frame (AD-2), sans log par frame hors `debug`.

**Never :**
- Pas d’armure de l’hôte transmise (story 3.2c), ni d’emplacement `gEEquipSlot`.
- Pas de main gauche (bouclier, torche), pas de quantités, pas de nouveau hook.
- Pas de `Reject` pour un équipement invalide.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Config valide | `It_Armor_Don_Fighter , It_1H_Sword_Rusty` | canonique `It_Armor_Don_Fighter,It_1H_Sword_Rusty` | N/A |
| Config invalide | `It-Sword`, `A,,B`, 17 noms, nom de 65 octets, doublon, > 255 octets | chargement refusé | champ `guest_equipment` et raison au log |
| Héros invité, objet absent | entrée en session | pile créée puis équipée | N/A |
| Héros invité, objet présent | même template déjà dans l’inventaire | aucun objet créé ; équipé s’il ne l’était pas | N/A |
| Nom inconnu du jeu | `IsValid` faux | log, noms suivants appliqués | N/A |
| Avatar chez l’hôte | `Hello.equipment` valide, `Spawn` reçu | avatar créé puis équipé | N/A |
| `Hello.equipment` invalide | reçu par l’hôte | session acceptée, avatar non équipé | Warn au log une fois |
| `Weapon` d’une arme équipée | même nom que dans l’équipement | pile retrouvée, pas de seconde copie | N/A |

</frozen-after-approval>

## Code Map

- `client/src/core/equipment.{h,cpp}` (nouveau, logique pure) : `parse_equipment(string_view, vector<string>& names, string& error)` et `join_equipment(names)`. Il sert à la config et à la session. Il faut l’ajouter à `mp_core` dans `client/CMakeLists.txt`. Tests dans `client/tests/test_equipment.cpp` (nouveau).
- `client/src/core/config.{h,cpp}` : branche `guest_equipment` (config.cpp:145) avec validation et forme canonique. Le commentaire de config.h:28 est à mettre à jour. `client/tests/test_config.cpp:25-31` attend désormais la forme canonique.
- `client/src/core/session.{h,cpp}` :
  - deux nouvelles commandes, `EquipHero{names}` et `EquipAvatar{net_id, names}` ;
  - invité : `EquipHero` après `Teleport` dans `guest_on_control` (session.cpp:446-450), si la liste n’est pas vide ;
  - hôte : les noms sont lus dans `host_on_hello` à l’acceptation (session.cpp:~400) et gardés jusqu’à `Ended`, puis `EquipAvatar` part juste après `SpawnAvatar` (session.cpp:537) ;
  - `Settings.equipment` est analysé dans le constructeur ; s’il est invalide, il est traité comme vide, avec un Warn ;
  - tests dans `client/tests/test_session.cpp` (voir le cas de la l. 602, équipement de 300 octets).
- `client/src/game/combat.{h,cpp}` : `is_stack_equipped` (`?IsStackEquiped@PSInventory@@QEBA_NH@Z`), ajouté à `InventoryAbi` et à `resolve_inventory`. Les enveloppes existantes (`template_from_name`, `find_stack_index`, `create_items`, `equip_stack`) sont réutilisées.
- `client/src/sync/avatar.{h,cpp}` :
  - extraire de `set_weapon` (avatar.cpp:313-396) la recherche ou création de pile (`ensure_stack`), sans changer son comportement, et l’appliquer aussi au héros, sans mémoire de piles ;
  - ajouter `equip(net_id, names)` pour l’avatar et `equip_player(names)` pour le héros local.
- `client/src/sync/session.cpp` : brancher `EquipHero` et `EquipAvatar` dans `execute()` (l. 135+).
- `client/src/win/hooks.cpp` : la ligne `[hooks] inventaire de session résolu` reste inchangée. `client/tests/wine/run_scenarios.sh:108` doit inclure le nouvel export dans la liste vérifiée.
- `tools/fake-guest/` : option `--equipment` (défaut vide) placée dans `Hello.Equipment` (main.go:150) et vérifiée par le même format, avec un test Go.
- `client/tests/session_fake_guest.sh` : un `--equipment` donne `EquipAvatar` au log de `session_host`.
- `protocol/PROTOCOL.md` : `Hello` (§4, l. 73) et §8 (règle « Équipement »). Les vecteurs sont inchangés.
- `client/multiplayer.example.ini:24-25` : commentaire et exemple. `client/README.md` : procédure de test en jeu.

## Tasks & Acceptance

**Execution :**
- [x] `client/src/core/equipment.{h,cpp}`, `client/CMakeLists.txt`, `client/tests/test_equipment.cpp` : format et tests (matrice, lignes 1-2). Motif : logique pure testée sous Linux.
- [x] `client/src/core/config.{h,cpp}` et `client/tests/test_config.cpp` : validation au chargement. Motif : epic, équipement.
- [x] `client/src/core/session.{h,cpp}` et `client/tests/test_session.cpp` : `EquipHero` et `EquipAvatar`, `Hello` invalide, remise à zéro à `Ended`. Motif : matrice.
- [x] `client/src/game/combat.{h,cpp}` : `is_stack_equipped` et `resolve_inventory`. Motif : AD-3.
- [x] `client/src/sync/avatar.{h,cpp}` et `client/src/sync/session.cpp` : `ensure_stack`, `equip`, `equip_player`, exécution des commandes. Motif : matrice.
- [x] `tools/fake-guest/` : `--equipment` et son test. Motif : banc.
- [x] `client/tests/session_fake_guest.sh` et `client/tests/wine/run_scenarios.sh` : non-régression, `EquipAvatar` et nouvel export. Motif : banc.
- [x] `protocol/PROTOCOL.md`, `client/multiplayer.example.ini`, `client/README.md` : format, exemple, procédure. Motif : AD-10, utilisabilité.

**Acceptance Criteria :**
- Given les tests Linux, Go et le banc Wine, when on construit, then tout passe au vert.
- Given Risen en hôte et `fake-guest --equipment It_Armor_Don_Fighter`, when l’avatar apparaît, then il porte l’armure. *(Manuel.)*
- Given un invité réel avec `guest_equipment` sur un objet absent de son inventaire, when il entre en session, then l’objet est créé une fois et équipé ; à la session suivante, aucune copie n’est ajoutée. *(Manuel, deux machines ou Risen en invité.)*

## Implementation Notes

- L'invité équipe son héros même si le `Welcome` porte une pose nulle (pas de Teleport) : l'`EquipHero` part au même endroit, avant le `Spawn`.
- Équipement invalide dans `Settings` (invité) : Warn au premier tick, Hello envoyé avec un équipement vide (le test de la l. 602 attend désormais ce comportement au lieu de « Hello non encodable »).
- `equip_entity` journalise chaque nom (Info/Warn/Error) puis un bilan ; appelé une fois par entrée en session ou création d'avatar, jamais par frame.

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge | Héros : `FindStackIndex` en SEH → `CreateItems` quand même, copie à chaque session | low | patch | Sans pile mémorisée, `!found` menait à la création. Corrigé : rien n’est créé, échec journalisé. |
| 2 | blind | `EquipStack → false` journalisé « équipé » | low | patch | Ligne Info trompeuse au test en jeu ; Warn « non équipé ». |
| 3 | blind, edge | Commentaires « l’hôte ignore `guest_equipment` » alors que la config la valide dans les deux rôles | low | patch | `parse_config` refuse une valeur invalide quel que soit le rôle (conforme à l’intention) ; commentaires corrigés. |
| 4 | blind, edge | Go `TrimSpace` (Unicode) ≠ C++ `isspace` (ASCII) | low | patch | `"A\u00a0"` accepté par le faux invité, refusé par la DLL ; trim ASCII et tests des deux côtés. |
| 5 | blind, edge | Contrôle « rien sans `--equipment` » hors du bloc nominal : vert sans log | low | patch | `grep -c` vide comparé à 0 ; contrôle déplacé dans le bloc. |
| 6 | blind | Règle du récepteur absente de `PROTOCOL.md` | low | patch | §4 et §8 complétés (même règle, casse respectée, invalide ignoré). |
| 7 | verif-gap, blind | Colle sync/game de l’équipement et `set_weapon` réécrit jamais exécutés par un test | medium | defer | Même couture `game::` que 3.1 et 3.2 ; entrée ajoutée à `deferred-work.md`. |
| 8 | blind | `EquipAvatar` dans la frame de création : inventaire prêt ? | maybe-false | defer | Medium si vrai ; à trancher au test en jeu (EquipStack → true, armure visible). |
| 9 | blind | Duplication : vendre l’objet puis se reconnecter le recrée | low | rejet | Conséquence directe de la décision utilisateur (« créé seulement s’il manque ») ; coop entre amis. Signalé à l’utilisateur. |
| 10 | blind | `hello.equipment` brut au log de l’hôte | low | rejet | Pair de confiance ; même verdict qu’en 3.2 (n° 16). |
| 11 | blind, edge | Avatar mort ou absent à `EquipAvatar` : équipement perdu | false | rejet | `EquipAvatar` part dans le même lot, juste après `SpawnAvatar` : l’avatar vient d’être créé et ne peut pas être mort. |
| 12 | blind | Libellé `[hooks] inventaire de session résolu (arme de l’avatar)` périmé ; `IsStackEquiped` couple l’arme | low | rejet | La spec fige la ligne ; tout ou rien voulu (AD-3). |
| 13 | blind | `has_known` reste vrai après `erase` | false | rejet | L’effacement n’a lieu que si `found` ; le repli exige `!found`. |
| 14 | blind | `Hello` retransmis avec un autre équipement | low | rejet | Le premier `Hello` accepté fait foi (même `Welcome`) ; testé (pas de second Warn). |
| 15 | blind | Statut de la spec et vérification non consignés | false | rejet | Correction de la spec elle-même, consignée à l’étape suivante. |
| 16 | edge | Inventaire non résolu → faux « SEH » au log | false | rejet | Sans `resolve_inventory`, la session ne s’installe pas : `equip` n’est jamais appelé. |
| 17 | edge | Deux noms sur le même emplacement : bilan faux | low | rejet | Erreur de config de l’utilisateur ; chaque nom est journalisé. |

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : `client_tests`, `wine_scenarios` et `session_fake_guest` passent.
- `cd tools/fake-guest && go test ./...` : tout passe au vert.

**Manual checks :**
- En jeu, `probe = 0`, Risen en hôte et faux invité avec `--equipment` : relever les lignes `[sync] équipement …`.
