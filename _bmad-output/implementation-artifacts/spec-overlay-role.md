---
title: 'Overlay à l’écran : rôle, pseudo et état de session'
type: 'feature'
created: '2026-10-23'
status: 'done'
baseline_commit: 'e8c7948b28d71591cd2b2aa6c94fe5d70f0e5291'
route: 'dispatch'
review_loop_iteration: 0
context:
  - '{project-root}/_bmad-output/planning-artifacts/architecture/architecture-risen-multiplayer-2026-10-15/ARCHITECTURE-SPINE.md'
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Avec deux Risen lancés sur la même machine (hôte depuis Steam, invité par `~/Games/lancer-risen-invite.sh`), rien à l’écran ne dit quelle fenêtre est l’hôte et laquelle est l’invité, ni où en est la session.

**Approach:** Une ligne de texte permanente en haut à gauche de l’image du jeu : rôle, pseudo, état de session. Elle est dessinée par un détour de `IDirect3DDevice9::Present` avec `ID3DXFont` (`d3dx9_39.dll`, déjà chargée par le jeu), et s’affiche tant que le mod est actif.

Décisions (prises sans l’utilisateur, modifiables) :
- Textes (UTF-8, convertis en UTF-16 pour le dessin) : `HÔTE · <pseudo> · <état>` ou `INVITÉ · <pseudo> · <état>`. États : Inactive = `hors partie`, Hosting = `en attente d’un invité`, Connecting = `connexion à <ip>:<port>…`, InSession = `en session`, Ended = `session terminée`.
- Couleur : hôte or (`0xFFFFC040`), invité cyan (`0xFF40D0FF`), avec une ombre noire décalée d’un pixel. Police Arial, 18 px, gras.
- Nouvelle clé `overlay` dans `multiplayer.ini` : `1` (défaut, vide compris) ou `0` (pas de détour).

## Boundaries & Constraints

**Always:** l’overlay est facultatif : tout échec (pas de `d3dx9_39.dll`, device factice impossible, MinHook) journalise un Warn `[overlay] …` et laisse la session intacte. Le détour de `Present` appelle toujours l’original, ne lève jamais d’exception et protège le dessin par SEH. Le texte passe de la frame au détour par une chaîne sous verrou court, sans autre état partagé. `Reset` relâche les ressources de la police avant l’original et les recrée après succès.

**Never:** pas d’appel au jeu (Genome) depuis le détour ni depuis `win/overlay` ; pas de changement de protocole ; pas de dépendance au SDK DirectX (l’interface `ID3DXFont` est déclarée à la main, `D3DXCreateFontW` résolue par `GetProcAddress`) ; pas d’overlay en mode sonde (`probe = 1`).

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Output / Behavior | Error Handling |
|----------|--------------|---------------------------|----------------|
| Hôte en attente | rôle hôte, `Hosting`, pseudo `Leoyd` | `HÔTE · Leoyd · en attente d’un invité` en or | N/A |
| Invité en connexion | rôle invité, `Connecting`, `127.0.0.1:27960` | `INVITÉ · Invite · connexion à 127.0.0.1:27960…` en cyan | N/A |
| Menu, pas de monde | `Inactive` | `… · hors partie` | N/A |
| `overlay = 0` | config | aucun détour posé, log Info `[overlay] désactivé (overlay = 0)` | N/A |
| `overlay = 2` | config | config invalide (comme `damage_hook = 2`) | message `valeurs possibles : 0, 1` |
| d3dx absent | `GetModuleHandle(d3dx9_39)` nul | pas d’overlay, session normale | Warn `[overlay] inactif : …` |
| Reset du device | changement de résolution | texte toujours affiché après Reset | Reset original non bloqué |

</frozen-after-approval>

## Code Map

- `client/src/core/config.{h,cpp}` -- `Config` + parsing par clé ; ajouter `overlay` sur le modèle exact de `damage_hook`.
- `client/tests/test_config.cpp` -- tests de config existants à étendre.
- `client/src/core/session.h:135-136` -- `State` et `state_name` ; à réutiliser pour le texte.
- `client/src/sync/session.cpp:418-424, 459` -- `on_frame` : `g_session->tick(...)` puis `state()` ; endroit où pousser le texte après le tick.
- `client/src/win/hooks.cpp` -- modèle de détours MinHook (`create`, `remove`) ; MinHook déjà initialisé par `install_session`.
- `client/src/win/script_init.cpp:243-260` -- `start_session()` : installer l’overlay après `install_session`, hors mode sonde.
- `client/CMakeLists.txt:61-84, 120-135` -- sources de la DLL et de `client_tests`.

## Tasks & Acceptance

**Execution:**
- [x] `client/src/core/config.{h,cpp}` -- clé `overlay` (bool, défaut vrai) -- interrupteur.
- [x] `client/src/core/overlay_text.{h,cpp}` -- fonction pure `overlay_text(Role, nickname, State, host_ip, port) → std::string` -- testable sous Linux.
- [x] `client/tests/test_overlay_text.cpp`, `client/tests/test_config.cpp` -- les lignes de la matrice (textes et clé).
- [x] `client/src/win/overlay.{h,cpp}` -- `install()`/`remove()`/`set_text(utf8, argb)` : device factice (fenêtre cachée 1×1, fenêtré) pour lire les entrées 16 (`Reset`) et 17 (`Present`) de la vtable, détours MinHook, police créée paresseusement sur le device reçu (recréée si le device change), dessin sur le back buffer 0 (cible de rendu sauvée et restaurée) entre `BeginScene`/`EndScene`. Journal Info au premier dessin avec l’id du thread.
- [x] `client/src/sync/session.cpp` -- après le tick, calculer le texte et appeler `win::overlay::set_text` seulement s’il change (via une fonction de rappel ou un en-tête neutre, sans inclure de Windows dans `sync/` au-delà de l’existant).
- [x] `client/src/win/script_init.cpp`, `client/CMakeLists.txt`, `client/README.md` -- branchement, `d3d9` résolue dynamiquement, documentation de la clé.

**Acceptance Criteria:**
- Given deux Risen sur la même machine en fenêtré, when les deux ont chargé la sauvegarde, then chaque fenêtre affiche son rôle et son état, et l’invité passe à `en session` quand l’hôte l’accepte.
- Given `overlay = 0`, when le jeu charge, then aucun texte et le reste du log est identique à avant.

## Implementation Notes

## Spec Change Log

## Review Triage Log

| # | Source | Constat | Verdict | Preuve / suite |
|---|---|---|---|---|
| 1 | blind, edge | Texte figé (« en session ») après une exception de session (`g_broken`) | medium | `on_frame` sort tôt après `g_broken`, `push_overlay` n'est plus appelé → patch : texte Ended poussé dans le catch |
| 2 | blind, edge | `SetRenderTarget` remet le viewport, non restauré | medium | comportement documenté de D3D9 (viewport remis à la taille de la cible) → patch : Get/SetViewport |
| 3 | edge | `OnLostDevice` sur un Reset d'un autre device | low | correction directe d'une condition → patch |
| 4 | verif, blind | Scénario Wine accepte `d3dx9_39 non chargée` / `D3DXCreateFontW introuvable` | medium | le banc charge Engine.dll (d3dx importée) et pose les détours → patch : deux `expect_no_log` |
| 5 | verif, blind | `push_overlay` et le branchement du sink non testés | medium | `sync/session.cpp` hors `client_tests`, le banc ne fait aucun tick → defer |
| 6 | edge | Le jeu présente peut-être par `IDirect3DSwapChain9::Present` | maybe-false | à trancher en jeu : `détours posés` sans `premier dessin` → defer (medium non vérifié) |
| 7 | blind, edge | Détours posés même si `configure()` abandonne | low | rien n'est dessiné (texte vide) ; cas QPC indisponible improbable → rejeté |
| 8 | blind, edge | `g_failed_device` / police gardée sur un device recyclé à la même adresse | low | un seul device pour la vie du processus chez Risen → rejeté |
| 9 | edge | Deux devices qui présentent en alternance | low | Risen n'a qu'un device → rejeté |
| 10 | edge | Paire de substitution coupée à 255 unités | low | pseudo ≤ 32 octets, IP courte : texte < 80 caractères → rejeté |
| 11 | edge | `switch` sans `default` | false | les 5 énumérateurs sont couverts, `-Wswitch` actif |
| 12 | blind, edge, verif | `overlay::remove()` jamais appelé ; état divergent après `MH_Uninitialize` | false | `hooks::remove()` n'est appelé qu'avant l'installation de l'overlay (`script_init.cpp:255`) |
| 13 | blind | Profondeur liée pendant le dessin | false | le sprite de D3DX coupe le Z-test |
| 14 | blind | Une allocation de chaîne par frame | low | négligeable devant le coût d'une frame → rejeté |
| 15 | blind | `LoadLibraryW(d3d9)` sans `FreeLibrary` | low | module chargé par le jeu pour toute sa vie → rejeté |
| 16 | blind | Couleur et texte couplés implicitement | low | la couleur ne dépend que du rôle, fixe → rejeté |
| 17 | blind | Spec incomplète, critère `overlay = 0` mal formulé | rejeté | correctif = éditer la spec |

## Design Notes

Ordre de la vtable `ID3DXFont` (à déclarer telle quelle) : `QueryInterface, AddRef, Release, GetDevice, GetDescA, GetDescW, GetTextMetricsA, GetTextMetricsW, GetDC, GetGlyphData, PreloadCharacters, PreloadGlyphs, PreloadTextA, PreloadTextW, DrawTextA, DrawTextW, OnLostDevice, OnResetDevice`. `DrawTextW(sprite=nullptr, text, -1, &rect, DT_NOCLIP|DT_LEFT|DT_TOP, color)`. Les détours patchent le code de `Present`/`Reset` (partagé par tous les devices du module) : le device factice est libéré avant la pose.

## Verification

**Commands:**
- `cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests` -- expected: build sans avertissement nouveau, tests verts.

**Manual checks (if no CLI):**
- Lancer l’hôte depuis Steam et l’invité par le script : textes or / cyan corrects ; `grep '\[overlay\]'` dans les deux logs.
