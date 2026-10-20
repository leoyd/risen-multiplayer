---
title: 'Story 3.2 — Arme de l’avatar'
type: 'feature'
created: '2026-10-20'
status: 'done'
route: 'dispatch'
review_loop_iteration: 0
baseline_commit: 'e955947734815ddb8cf5de1efe1b329defa7c297'
context:
  - '{project-root}/_bmad-output/implementation-artifacts/epic-3-context.md'
  - '{project-root}/docs/re/spike-j2.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** L’avatar a toujours les mains vides : quand l’autre joueur sort son arme, on voit l’animation, mais pas l’arme, et ses attaques rejouées se font à mains nues. La story 3.5 (`DoDamage` avec l’arme en main de l’avatar) en dépend.

**Décision (utilisateur, 2026-10-20) :** story découpée. L’équipement (`guest_equipment`, héros invité, armure de l’hôte) part en story 3.2b (`deferred-work.md`). Cette story ne livre que l’arme sortie ou rangée.

**Approach:** Un nouveau message fiable `Weapon{net_id, nom}` circule dans les deux sens (protocole v7). Il porte le nom de l’arme en main du héros, vide si l’arme est rangée. Le récepteur crée cette arme dans l’inventaire de l’avatar si elle manque, puis la lui fait tenir (`HoldRightStack`) ou ranger (`EquipStack`).

## Boundaries & Constraints

**Always :**
- Émission : à chaque frame, `GetRightHoldStack` du héros local ; s’il vaut ≥ 0, le nom de l’objet en main (`GetRightHoldItem` + `Entity::GetName`). `Weapon` part au changement de nom, puis une fois à l’entrée en session, après le `Spawn`.
- Réception : appliquée au même instant d’affichage que les `Motion` (comme `Death`), pour que les attaques rejouées se fassent arme en main.
  - Nom non vide : `Template(const char*)` + `IsValid`, puis la pile de ce template, cherchée (`FindStackIndex`) ou créée (`CreateItems(…, 1, 0)`), puis `HoldRightStack`.
  - Nom vide : `EquipStack` de la pile en main de l’avatar, s’il y en a une.
- AD-3 : les symboles d’inventaire de la session sont résolus à l’installation des hooks de session, en tout ou rien ; un manque laisse la session inactive, cause au log.
- AD-10 : `PROTOCOL.md` v7, vecteurs, codecs C++ et Go, tests des deux côtés.
- Le jeu n’est appelé que depuis la frame (AD-2). Aucun log par frame hors `debug`.

**Never :**
- Pas de mode de combat transmis (posture = nom des animations).
- Pas de hook nouveau ; pas d’équipement ni d’armure (story 3.2b).
- `resolve_combat` (sonde) inchangé.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Erreur |
|---|---|---|---|
| Arme sortie | pile en main passe de −1 à ≥ 0, nom `It_1H_Sword` | `Weapon{nom}` envoyé ; l’avatar la tient dans la seconde | Template inconnu : log une fois par nom, avatar inchangé |
| Arme rangée | pile en main → −1 | `Weapon{""}` ; `EquipStack` sur l’avatar | Aucune pile en main : sans effet |
| Changement d’arme | autre nom en main | nouveau `Weapon` ; pile créée une seule fois par nom et par avatar | N/A |
| Entrée en session | arme déjà sortie | `Weapon` envoyé après `Spawn` ; avatar armé dès l’apparition | N/A |
| Lecture du héros | SEH sur `GetRightHoldStack`/`GetName` | dernier état gardé, log une fois | Pas de `Weapon` envoyé |
| `Weapon` avant l’avatar | avatar absent | ignoré, log debug | N/A |
| Décodage | `Weapon` tronqué ou nom > 255 octets | `Truncated` / refus | Message ignoré, log |

</frozen-after-approval>

## Code Map

- `client/src/core/protocol.{h,cpp}` : `kProtocolVersion` = 7 (l. 25). `Weapon` (type 27, `net_id u32`, `nom str8` 0 à 255) sur le modèle de `Death`/`AniName`. Tests : `client/tests/test_protocol.cpp`.
- `client/src/core/session.{h,cpp}` : logique pure.
  - Entrée `LocalWeapon{nom}` → `Weapon` envoyé au changement ; dernier nom renvoyé à l’entrée en session, après le `Spawn`.
  - `Weapon` reçu → commande `AvatarWeapon{net_id, nom}` au même instant d’affichage que `Death` / `AvatarDeath`.
  - Remise à zéro à `Ended`.
  - Tests : `client/tests/test_session.cpp`.
- `client/src/game/combat.{h,cpp}` :
  - Réutiliser `get_right_hold_stack`, `get_right_hold_item`, `get_entity_name`, `create_items`, `hold_right_stack`, `equip_stack` (combat.h:64-71) et `destroy_template` (entity.h).
  - Ajouter `template_from_name` (`??0Template@@QEAA@PEBD@Z` + `?IsValid@Template@@QEBA_NXZ`) et `find_stack_index` (`?FindStackIndex@PSInventory@@QEBAHAEBVTemplate@@H_N@Z` ; le sens de `int` et `bool` est à lire au désassemblage ; repli sur une pile mémorisée par nom). Symboles présents dans `Script.dll` (vérifié).
  - Créer `resolve_inventory(missing)` : ces symboles en tout ou rien. Les enveloppes marchent avec `resolve_inventory` ou `resolve_combat`.
- `client/src/win/hooks.cpp`, `install_session()` (l. 217) : `game::resolve_inventory` après `resolve_entities`.
- `client/src/sync/avatar.{h,cpp}` : `set_weapon(net_id, nom)`, avec les piles par nom dans `Entry` (vidées au retrait), et `local_weapon(nom&)` pour lire l’arme du héros local.
- `client/src/sync/session.cpp` : brancher `AvatarWeapon` dans `execute()` (l. 131). Dans `local_captures()` (l. 215), monde chargé, lire l’arme du héros et pousser `LocalWeapon` au changement.
- `tools/fake-guest/` (codec `protocol/protocol.go`, `session.go`, `scenario.go`, tests) :
  - `Weapon` encodé et décodé, vérifié contre les vecteurs ;
  - `--weapon It_1H_Sword` (défaut), envoyé après le `Spawn` ;
  - directive de scénario `weapon <nom|->` ;
  - `Weapon` reçus journalisés.
- `protocol/PROTOCOL.md`, `protocol/vectors/` (`regen.sh`) : v7 (§3, §4, §5, §7), `weapon.bin`, `weapon_sheathed.bin` ; vecteurs portant la version régénérés.
- `client/tests/session_fake_guest.sh`, `client/tests/wine/run_scenarios.sh` : la session s’installe (inventaire résolu) ; les `Weapon` circulent.
- `client/README.md` : protocole de test en jeu.
- Référence validée au spike : `num1_weapon`, `client/src/sync/probe_j2.cpp:154-228`.

## Tasks & Acceptance

**Execution :**
- [x] `client/src/core/protocol.{h,cpp}` + `client/tests/test_protocol.cpp` : `Weapon`, v7. Motif : AD-10.
- [x] `protocol/PROTOCOL.md` + `protocol/vectors/` : v7, nouveaux vecteurs. Motif : AD-10.
- [x] `client/src/core/session.{h,cpp}` + `client/tests/test_session.cpp` : `LocalWeapon` → `Weapon`, `AvatarWeapon` à l’instant d’affichage, cas de la matrice. Motif : logique testée sous Linux.
- [x] `client/src/game/combat.{h,cpp}` : `template_from_name`, `find_stack_index`, `resolve_inventory`. Motif : AD-3.
- [x] `client/src/win/hooks.cpp` : `resolve_inventory` dans `install_session`. Motif : AD-3.
- [x] `client/src/sync/avatar.{h,cpp}` + `client/src/sync/session.cpp` : arme de l’avatar, lecture de l’arme du héros. Motif : matrice.
- [x] `tools/fake-guest/` : `Weapon`, `--weapon`, directive, tests Go. Motif : AC de l’epic.
- [x] `client/tests/session_fake_guest.sh`, `client/tests/wine/run_scenarios.sh` : non-régression et `Weapon`. Motif : banc.
- [x] `client/README.md` : test en jeu. Motif : utilisabilité.

**Acceptance Criteria :**
- Given les tests Linux, Go et le banc Wine, when on construit, then tout est au vert et les vecteurs v7 concordent en C++ et en Go.
- Given Risen en hôte et le faux invité (`--weapon It_1H_Sword`), when l’avatar apparaît, then il tient l’épée. *(Manuel.)*
- Given une session, when l’hôte sort puis range son arme, then le faux invité journalise `Weapon{nom}` puis `Weapon{""}`. *(Manuel.)*
- Given l’avatar armé, when le faux invité rejoue une attaque (`--echo`), then elle se fait arme en main. *(Manuel.)*
- Given deux machines, when chacun sort et range son arme, then l’avatar de l’autre suit dans la seconde. *(Manuel, validation finale.)*

## Implementation Notes

- `FindStackIndex(Template const&, int, bool)` lu au désassemblage : l'`int` est un index **exclusif**
  (la recherche part de `int + 1`, via `gCInventory_PS::FindStackIndex(eCTemplateEntity*, int)`), le
  `bool` saute les piles posées dans un emplacement (`gCInventorySlot::GetSlot` ≠ 0). Appel retenu :
  `(-1, false)`, n'importe quelle pile. Repli : la pile mémorisée pour ce nom n'est reprise que si
  `FindStackIndex` lève une exception (SEH, journalisé une fois) ; s'il rend -1 (objet sorti de
  l'inventaire), l'entrée mémorisée est oubliée et l'objet recréé (`CreateItems`).
- Core : `LocalWeapon` est retenu dans **tous** les états (balayage en tête de `tick`), envoyé en
  session au changement ; le dernier nom repart après le `Spawn` à chaque entrée en session (rien
  si l'arme n'a jamais été lue). Le dernier nom envoyé et la file des `Weapon` du pair sont remis à
  zéro à `Ended` (et au retrait de l'avatar / à sa mort pour la file). Un `Weapon` reçu pour un avatar
  mort, ou dont le `Death` attend l'affichage, est ignoré comme pour l'absence d'avatar (log debug). Les `AvatarWeapon` dus partent avant les
  `PlayAvatarMotion` du même tick (attaques rejouées arme en main).
- `game/combat` : `InventoryAbi` séparée de la sonde ; les enveloppes partagées choisissent le pointeur
  de `resolve_inventory`, sinon celui de `resolve_combat` (`resolve_combat` inchangé). Les appels
  génériques testent désormais le pointeur de fonction au lieu de `g_resolved`.
- `win/hooks` : nouvelle ligne `[hooks] inventaire de session résolu (arme de l'avatar)`, vérifiée par
  le banc Wine (avec la présence des 9 exports dans la `Script.dll` du jeu).
- Faux invité : `Weapon{2, --weapon}` envoyé après le `Spawn` (y compris vide avec `--weapon -`) ;
  directive `weapon [instant s] <nom|->` (instant facultatif, 0 par défaut) ; en `--echo`, les `Weapon`
  de l'hôte sont renvoyés comme les siens. `session_host` (banc Linux) gagne `--weapon NOM` et
  `SIGHUP` (ranger / ressortir).

- **2026-10-21, test en jeu (utilisateur, une machine, Risen hôte + faux invité) : validé.**
  - `[hooks] inventaire de session résolu` présent au démarrage.
  - `--weapon It_1H_Sword` : l'avatar apparaît l'épée en main (pile 2 créée, `HoldRightStack` → true). Il reste en T-pose : c'est attendu, car le faux invité sans `--echo` n'envoie aucun `Motion` et le moteur est bloqué sur l'avatar (comportement de la 2.2).
  - Écho : sortir, ranger, frapper, esquiver, avec des animations fluides. Les 7 armes testées s'affichent : `It_1H_Sword`, `It_1H_Knife_Hunting`, `It_1H_Sword_Rusty`, `It_Fist`, `It_1H_Club`, `It_1H_Sickle`, `It_Axe_Lumberjack`.
  - Aucune ligne « inconnu du jeu » : le nom lu par `Entity::GetName` est accepté par `Template(const char*)`. Une pile est créée une fois par nom, puis retrouvée (`trouvée`).
  - Non fait : la validation à deux machines, et le pillage de l'avatar mort (point reporté).

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Route | Preuve |
|---|---|---|---|---|---|
| 1 | blind, edge, gap | Pile mémorisée réutilisée quand `FindStackIndex` rend −1 : l’avatar peut tenir un autre objet | medium | patch | `set_weapon` prend le repli dès `stack < 0` ; un objet sorti de l’inventaire décale les index. Repli limité à l’échec SEH de `find_stack_index`, recréation sinon. |
| 2 | blind, gap | `Weapon` accepté pendant un `Death` en attente : `HoldRightStack` juste avant le ragdoll | low | patch | `on_peer_weapon` ne teste pas `death_pending_`, et `apply_due_weapons` passe avant le test de mort dans `place_avatar`. Correction directe. |
| 3 | blind | §8 de `PROTOCOL.md` muet sur l’avatar mort | low | patch | La règle du récepteur ne cite que « avatar présent ». |
| 4 | blind, edge | `g_weapon_error_logged` jamais remis à zéro | low | patch | Une seule ligne pour tout le processus ; remise à zéro au changement de héros. |
| 5 | blind, edge | `on_local_weapon` compare `in.name` mais envoie `local_weapon_` | low | patch | Correction directe (comparer la valeur envoyée). |
| 6 | blind | Aller-retour nom d’objet → `Template(nom)` non vérifié | maybe-false | patch | Vu une fois au spike (`It_1H_Sword`). Étape explicite ajoutée au README. |
| 7 | blind, edge, gap | Arme d’un ancien monde renvoyée si la 1ʳᵉ lecture du nouveau monde lève une SEH | low | rejet | SEH jamais observée sur ces lecteurs ; le correctif ajoute une entrée à la session. |
| 8 | blind | Objets créés dans l’inventaire de l’avatar pillables sur son cadavre | maybe-false | defer | À vérifier en jeu : l’hôte peut-il fouiller l’avatar mort (entité temporaire) ? |
| 9 | blind | Main gauche (bouclier, torche) non répliquée | low | defer | Hors du « nom de l’arme en main » de l’intention ; à reprendre avec la 3.2b. |
| 10 | gap | Colle `sync`/`game` de l’arme (`set_weapon`, `local_weapon`, `pick`) jamais exécutée par un test | medium | defer | Aucun banc n’appelle `OnProcess` ; même trou que la 3.1 (couture `game::` simulée). |
| 11 | blind | Échec de `resolve_inventory` = session entière inactive | false | rejet | Voulu par la spec (AD-3, tout ou rien). |
| 12 | blind | Contrôle Wine par `grep` des chaînes, pas des exports | low | rejet | `objdump -p` a confirmé les exports ; le banc vérifie aussi la résolution réelle sous Wine. |
| 13 | blind | `GetName` et un `bCString` alloué à chaque frame, arme sortie | low | rejet | Coût négligeable ; le cache ajouterait de l’état. |
| 14 | blind, edge | Défaut `--weapon It_1H_Sword` du faux invité, y compris en `--echo` | false | rejet | Défaut fixé par la spec ; en écho, l’arme de l’hôte remplace ensuite celle du faux invité. |
| 15 | blind | Tests manquants (débordement de file, `Weapon` avant `Spawn`, Despawn+Spawn) | low | rejet | Chemins de la même logique déjà testée ; le cas `death_pending_` est ajouté avec le n° 2. |
| 16 | blind | Nom de pair avec NUL, ensembles par nom non bornés | low | rejet | Pair de confiance (coop à 2) ; aucune conséquence hors du log. |
| 17 | edge | `Weapon` appliqué avant un `Motion` plus ancien du même tick | low | rejet | Ranger l’arme demande une animation : jamais une attaque et un rangement dans la même frame. |
| 18 | edge | Directive `weapon 1.5` lue comme un nom | low | rejet | Outil de test ; une coquille se voit au log. |
| 19 | edge | Nom de 255 octets tronqué par `char buf[255]` | low | rejet | Les noms de templates sont courts. |

## Verification

**Commands :**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` : `client_tests`, `wine_scenarios` et `session_fake_guest` passent.
- `cd tools/fake-guest && go test ./...` : vert, vecteurs v7 compris.

**Manual checks :**
- En jeu, `probe = 0`, Risen hôte et faux invité : suivre la section du README et rapporter les lignes `[sync] arme …`.
