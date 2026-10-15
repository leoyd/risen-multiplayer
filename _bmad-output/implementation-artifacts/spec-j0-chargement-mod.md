---
title: 'J0 — Chargement du mod par Risen, vérification de version et lecture de la config'
type: 'feature'
created: '2026-10-15'
baseline_commit: '94413fe6a47524f08873ff0143bbd800c91313d6'
status: 'done'
route: 'dispatch'
review_loop_iteration: 1
context:
  - '{project-root}/_bmad-output/planning-artifacts/prds/prd-risen-multiplayer-2026-10-15/prd.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Il n’existe encore aucun code. Il faut prouver que Risen (Steam x64) charge une DLL à nous sous Windows et sous Proton, et poser le socle : le log, la vérification de version et la config (PRD FR-1 à FR-3).

**Approach:** Une DLL `Script_Multiplayer.dll`, compilée avec clang-cl + lld-link + xwin, déposée dans `bin/scripts/`. Elle exporte `ScriptInit`, qui retourne le descripteur attendu par `gCScriptAdmin::LoadScriptDLL` avec une seule entrée inoffensive. Elle écrit un log, vérifie l’empreinte des modules du jeu et lit `multiplayer.ini`. Si la version n’est pas reconnue ou si la config est invalide, le mod reste inactif et le jeu solo reste intact.

## Boundaries & Constraints

**Always :**
- Rester compatible avec le chargeur natif : `ScriptInit()` sans argument retourne un pointeur valide pour toute la durée du processus. Des tables toutes vides font **rejeter le module silencieusement** (`Game.dll` `0x1809632dc`). La boîte « Could not load script module » ne s’affiche que si la DLL elle-même ne se charge pas.
- Si la version du jeu n’est pas reconnue ou si l’ABI est introuvable, `ScriptInit` retourne des tables vides et n’appelle aucun code du jeu. La config est lue et ses erreurs sont journalisées quand même. `MP_Probe` n’est enregistré que si la version est reconnue.
- Résoudre les fonctions du jeu par `GetProcAddress`, sans lier de `.lib` du jeu.
- Le log et la config sont des fichiers à côté de la DLL.
- La logique pure (parsing de la config, comparaison de version) est testable sous Linux sans le jeu.

**Never :**
- Pas de réseau, pas de hook MinHook, pas de serveur : c’est le périmètre de J1.
- Ne jamais commiter ni redistribuer de fichier du jeu (`risen_ori/`).
- Pas d’injecteur ni de proxy DLL, pas de `WINEDLLOVERRIDES`.
- Aucun crash et aucune boîte de dialogue ne doivent sortir du mod, quel que soit le cas d’erreur.

## I/O & Edge-Case Matrix

| Scénario | Entrée / état | Résultat attendu | Gestion d’erreur |
|---|---|---|---|
| Nominal | Version connue, `multiplayer.ini` valide | Log : version du mod, version du jeu, « config OK (rôle=…) », « mod actif » | N/A |
| Version inconnue | Empreinte de `Game.dll`/`Engine.dll` hors liste blanche | Log : « version non supportée » + empreinte lue ; mod inactif | Le jeu continue en solo |
| Config absente | Pas de `multiplayer.ini` | Log : « config absente » ; mod inactif | Le jeu continue en solo |
| Champ invalide | ex. `role=chef`, `port=99999`, `ip` vide en invité | Log : nom du champ fautif et valeur ; mod inactif | Le jeu continue en solo |
| Champ inconnu | ex. `couleur=bleu` | Ignoré, avec un avertissement au log | N/A |
| Log impossible | Dossier en lecture seule | Le mod continue sans log | Silencieux |

**Décisions (2026-10-15) :**
- Amendement après relecture (validé par l’humain) : rejet silencieux par des tables vides, à la place de la règle erronée « au moins une table non vide ». Les corrections sont ciblées : le code existant est conservé, sans annulation.
- Toolchain : l’utilisateur accepte la licence Microsoft. xwin est installé avec le CRT et le SDK dans `~/.xwin`, local et jamais commité.
- clang, lld et llvm sont installés par l’utilisateur via `sudo dnf install clang lld llvm`.
- La spec est gardée entière malgré sa taille (environ 2 000 tokens), car il s’agit d’un seul objectif.

</frozen-after-approval>

## Code Map

Dépôt vierge côté code. Faits établis par rétro-ingénierie (REA/Hopper + objdump, binaires Steam x64) :
- `Game.dll` `gCScriptAdmin::LoadScriptDLL` (≈ `0x180962c58`–`0x180963363`) : `eCVirtualFileSystem::FindFiles(…, "*.dll")`, puis `g_LoadModule`, `g_GetProcAddress(mod, "ScriptInit")` et l’appel `ScriptInit()`. Il lit 4 tables `{T* data ; int count ; int cap}` (16 octets) aux offsets `0x00`/`0x10`/`0x20`/`0x30` : AIState, AIFunction, AICallback, Script. Si les 4 comptes valent 0 → `0x1809632dc` (rejet silencieux, retourne false). `g_MessageBox("Could not load script module…")` n’est atteint que si `g_LoadModule` échoue (`0x180962c42`).
- Entrée de table (pas de `0x18`) : `+0x00 bCString nom` (passé par `const bCString&` à `eCScriptFunctionMap::Register*`), `+0x08 bCString` (copiée par `operator=`), `+0x10` pointeur de fonction. Une entrée dont le nom est déjà enregistré est ignorée.
- `Script_Game.dll` `ScriptInit` (`0x1800a46d0`) : retourne l’adresse d’une statique initialisée une fois.
- `SharedBase.dll` exporte `??0bCString@@QEAA@PEBD@Z` (constructeur depuis `const char*`) et `??1bCString@@QEAA@XZ`.
- Empreintes actuelles (en-tête PE, TimeDateStamp/SizeOfImage) : `Game.dll` `0x63c9aff6`/`0x1417000`, `Engine.dll` `0x63ca945d`/`0xd3a000`.

## Tasks & Acceptance

**Execution :**
- [x] `cmake/clang-cl-xwin.cmake` -- toolchain croisée (clang-cl, lld-link, llvm-lib, `XWIN_DIR`) -- build Windows reproductible depuis Fedora
- [x] `client/CMakeLists.txt` -- cible `Script_Multiplayer` (SHARED, x64) + cible `client_tests` native Linux -- séparer la logique pure, testable sans le jeu
- [x] `client/src/core/config.{h,cpp}` -- parse `multiplayer.ini` (`role`, `host_ip`, `port`, `nickname`, `guest_equipment`) → résultat ou erreur nommant le champ -- FR-3
- [x] `client/src/core/version.{h,cpp}` -- liste blanche d’empreintes `{TimeDateStamp, SizeOfImage}` par module + comparaison -- FR-2
- [x] `client/tests/*.cpp` -- tests unitaires de la matrice I/O (config et version), sans framework externe ou avec doctest vendu -- TDD
- [x] `client/src/win/game_abi.{h,cpp}` -- résolution `GetProcAddress` du constructeur et du destructeur de `bCString` -- ABI MSVC explicite
- [x] `client/src/win/script_init.cpp` -- export `ScriptInit` : descripteur statique, 1 entrée dans la table Script (`"MP_Probe"`, callback qui retourne 0) ; lecture des en-têtes PE en mémoire (`GetModuleHandle`) ; log ; activation -- FR-1, FR-2
- [x] `client/src/win/log.{h,cpp}` -- log horodaté `Script_Multiplayer.log` à côté de la DLL, tronqué au lancement -- FR-1
- [x] `client/multiplayer.example.ini` + `client/README.md` -- exemple de config et procédure de build et d’installation -- utilisabilité

**Acceptance Criteria :**
- Given la DLL et une config valide dans `bin/scripts/` du jeu Steam, when Risen démarre sous Proton sans option de lancement, then le log contient la version du mod, l’empreinte du jeu, « mod actif », et aucune boîte de dialogue n’apparaît.
- Given le même dépôt sous Windows, when Risen démarre, then même résultat.
- Given la DLL installée, when on charge une sauvegarde et on joue 5 minutes en solo, then aucun crash ni comportement anormal.

## Implementation Notes

- Toolchain placée dans `client/cmake/clang-cl-xwin.cmake` : CMake résout un `CMAKE_TOOLCHAIN_FILE` relatif par rapport au dossier source (`client/`), ce qui rend la commande de vérification valide telle quelle.
- xwin 0.10.0 installé dans `~/.local/bin/xwin`, CRT/SDK extraits dans `~/.xwin` (absent au départ). xwin ne fournit pas le CRT debug : la toolchain force `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` et le build par défaut est `Release`. La DLL n'importe que `KERNEL32.dll`.
- Défauts de config choisis : `port` = 27960, `nickname` = « Joueur » (1–32 caractères), `host_ip` obligatoire seulement en invité. Une ligne sans `=` est une erreur nommée « ligne N ». Doublon : avertissement, dernière valeur gardée.
- Vérifié sous Wine avec un harnais hors dépôt qui charge les vraies `SharedBase/Engine/Game.dll` puis appelle `ScriptInit` : table Script = 1 entrée `MP_Probe`, bCString construites par le jeu, empreinte reconnue, cas config absente / `role=chef` / `port=99999` / dossier en lecture seule conformes. Le test en jeu (Proton, Windows, 5 min de solo) reste manuel.
- Ordre d'initialisation : log → version → config (toujours lue) → descripteur seulement si la version est reconnue. Version inconnue ou ABI introuvable : tables vides, rejet silencieux par le chargeur, aucun appel au code du jeu.

- Audit de la matrice (session principale) : les lignes « config absente » et « log impossible » n’étaient couvertes que par un banc Wine resté hors du dépôt. Ajout de `client/tests/wine/harness.cpp` (cible Windows `mp_harness`) et de `client/tests/wine/run_scenarios.sh`. Le script est enregistré dans ctest sous le nom `wine_scenarios` quand wine, le build Windows et les DLL du jeu sont présents. Il couvre 6 vérifications (nominal, champ inconnu, config absente, champ invalide, log impossible), toutes passantes. La ligne « version inconnue » est couverte par les tests unitaires.

## Spec Change Log

- **Boucle 1 (relecture #2)** : la règle figée « au moins une table non vide, sinon boîte de dialogue » était fausse (désassemblage de `Game.dll`). Elle est amendée avec l’accord de l’humain : rejet silencieux, vérification de version **avant** tout appel au code du jeu, lecture de la config indépendante de la version. État à éviter : du code du jeu exécuté sur un build non reconnu. KEEP : le descripteur statique et ses `static_assert`, la protection SEH des appels au jeu, le parseur de config, le banc d’essai Wine, le CRT statique.

## Review Triage Log

| # | Source | Constat | Verdict | Preuve |
|---|---|---|---|---|
| 1 | blind+edge | `run_scenarios.sh` : `$?` dans `if ! wine` vaut toujours 0 | low | Sémantique bash : `$?` après `!` reflète la négation |
| 2 | blind+edge+vgap | Descripteur construit avant la vérification de version ; un échec de l’ABI laisse des tables vides, ce qui provoquerait une boîte de dialogue | medium | Dans `Game.dll`, des tables vides sautent vers `0x1809632dc` (`xor bl,bl`), un **rejet silencieux**. La boîte de dialogue (`0x1809632e3`) n’est atteinte que depuis `0x180962c42` (échec du chargement du module). La règle figée « au moins une table non vide, sinon dialogue » est donc fausse, et l’ordre actuel exécute du code du jeu sur un build non vérifié → **intent_gap** |
| 3 | blind+edge | Erreurs de config masquées quand la version échoue (court-circuit `&&`) | low | `script_init.cpp` : `version_ok && load_config(dir)` |
| 4 | blind+vgap | Le scénario « invalide » ne vérifie pas « mod inactif » | low | `run_scenarios.sh` : grep du champ uniquement |
| 5 | blind+vgap | Aucun scénario Wine « version inconnue » | medium | Les scénarios chargent toujours les vraies DLL de la liste blanche, et `initialize()` n’est jamais exercé avec une version inconnue |
| 6 | vgap+blind+edge | `wine_scenarios` non enregistré sur un clone propre (ordre de build), DLL possiblement périmée, pas de vérification que `Script_Multiplayer.dll` existe, README muet | medium | `CMakeLists.txt` : enregistrement au moment du configure, conditionné à `mp_harness.exe` |
| 7 | blind+edge | Le pseudo est limité à 32 **octets**, alors que l’exemple d’ini annonce 32 caractères ; UTF-8 non validé | low | `multiplayer.example.ini:16` contre `valid_nickname` |
| 8 | blind+edge | Commentaires en fin de ligne non gérés, et non documentés | low | Le parseur ne retire que les commentaires en début de ligne |
| 9 | blind+edge | `valid_host` accepte `1.2.3.4:27960`, `...`, `-` | low | Erreur utilisateur plausible (port collé à l’IP), qui n’échouerait qu’en J1 |
| 10 | edge | En rôle hôte, un `host_ip` invalide fait rejeter la config, alors que l’exemple dit « ignoré pour l’hôte » | low | `multiplayer.example.ini:10` contre `config.cpp` |
| 11 | edge | `port =` vide est rejeté, alors que l’exemple annonce un défaut | low | `config.cpp` : valeur vide → `parse_port` échoue |
| 12 | blind+edge | `read_file` : un échec de `GetFileSizeEx` est journalisé comme `ERROR_FILE_TOO_LARGE` | low | Correction directe |
| 13 | blind+edge | `exit "$failures"` : boucle modulo 256 | low | Correction directe |
| 14 | blind+vgap | Le test « vraies DLL » compte `[ OK ]` quand les DLL sont absentes ; `RISEN_BIN_DIR` est toujours défini | low | `test_version.cpp` + variable de cache CMake |
| 15 | blind | Le log d’exemple du README omet l’horodatage | low | `log.cpp` préfixe `[AAAA-MM-JJ …]` |
| 16 | blind | Pas de log de repli si `bin/scripts/` n’est pas inscriptible | false | Rejeté : Steam rend sa bibliothèque inscriptible par l’utilisateur, et le dossier est inscriptible sous Proton ; cas improbable, et le repli ajouterait de la complexité |
| 17 | blind+edge | Le banc d’essai suppose que `bCString` est un `char*` direct | false | Le banc lit `MP_Probe` via `strcmp` sur une chaîne construite par le vrai constructeur du jeu, et le test passe : la disposition est vérifiée empiriquement |
| 18 | edge | Fins de ligne CR seules | low | Rejeté : improbable (aucun éditeur moderne), le correctif ajoute une branche |
| 19 | edge | `dll_directory` retombe sur `.` | low | Rejeté : `GetModuleHandleExW` sur une adresse de notre propre image n’échoue pas en pratique |
| 20 | edge | `ScriptInit` appelé en concurrence | false | `LoadScriptDLL` est appelé séquentiellement par `gCScriptAdmin` |
| 21 | edge | Options `/EHsc` passées à une variante GNU de Clang | false | Seule la toolchain clang-cl est supportée (`clang-cl-xwin.cmake`) |
| 22 | edge | Scénario « lecture seule » exécuté en root | low | Rejeté : improbable pour un dev, correctif non trivial |

## Design Notes

L’entrée `MP_Probe` dans la table Script est le moyen le plus inoffensif de satisfaire le chargeur : un script que personne n’appelle. Le mod ne s’accroche donc à rien en J0. L’empreinte est lue dans l’en-tête PE déjà chargé en mémoire, ce qui évite tout I/O et toute dépendance à une bibliothèque de hachage. Une mise à jour Steam change le TimeDateStamp.

## Verification

**Commands :**
- `cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- attendu : tous les tests passent
- `cmake -S client -B client/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake && cmake --build client/build` -- attendu : `Script_Multiplayer.dll` en PE32+ x86-64
- `objdump -p client/build/Script_Multiplayer.dll | grep ScriptInit` -- attendu : export présent

**Manual checks :**
- Copier la DLL et l’ini dans `<Steam>/Risen/bin/scripts/`, lancer le jeu et lire `Script_Multiplayer.log`.
