# Client — `Script_Multiplayer.dll`

DLL chargée nativement par Risen (Steam, x64) depuis `bin/scripts/`, comme le module
officiel `Script_Game.dll`. Aucun injecteur, aucune option de lancement : le chargeur
de scripts du jeu (`gCScriptAdmin::LoadScriptDLL`) charge toutes les `*.dll` de ce
dossier et appelle leur export `ScriptInit`.

État J0 : le mod écrit un log, vérifie la version du jeu et lit sa config. Il
n'expose qu'un script inoffensif (`MP_Probe`) et ne s'accroche à rien.

## Prérequis (Fedora)

```bash
sudo dnf install clang lld llvm cmake ninja-build
# CRT + SDK Windows (licence Microsoft à accepter), extraits localement, jamais commités :
xwin --accept-license --arch x86_64 splat --output ~/.xwin
```

`xwin` : binaire publié sur <https://github.com/Jake-Shadle/xwin/releases>.
Autre emplacement : `-DXWIN_DIR=/chemin` ou variable d'environnement `XWIN_DIR`.

## Build

Depuis la racine du dépôt :

```bash
# Tests unitaires (natif Linux, sans le jeu)
cmake -S client -B client/build-tests && cmake --build client/build-tests && ctest --test-dir client/build-tests

# DLL Windows x64 (clang-cl + lld-link + xwin)
cmake -S client -B client/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake
cmake --build client/build
objdump -p client/build/Script_Multiplayer.dll | grep ScriptInit   # export présent
```

### Scénarios d'intégration sous Wine (`wine_scenarios`)

`tests/wine/run_scenarios.sh` rejoue la matrice I/O de J0 (nominal, config absente,
champ invalide, version inconnue, dossier en lecture seule) avec `mp_harness.exe`, qui
charge les vraies DLL du jeu de `risen_ori/bin/` puis appelle `ScriptInit`. Le test
CTest `wine_scenarios` n'est enregistré que si `wine`, `risen_ori/bin/Game.dll`,
`client/build/mp_harness.exe` et `client/build/Script_Multiplayer.dll` existent **au
moment de la configuration**. Ordre à respecter :

```bash
cmake --build client/build                       # 1. build Windows d'abord
cmake -S client -B client/build-tests            # 2. (re)configurer les tests
cmake --build client/build-tests && ctest --test-dir client/build-tests   # 3. lance aussi wine_scenarios
```

Préfixe Wine utilisé : `client/build/wineprefix` (créé au premier lancement), ou
`MP_WINEPREFIX` s'il est défini.

Le chemin du toolchain est relatif au dossier source `client/`. La DLL est liée au CRT
statique : sa seule dépendance est `KERNEL32.dll`.

## Installation

1. Copier `client/build/Script_Multiplayer.dll` dans `<Steam>/steamapps/common/Risen/bin/scripts/`.
2. Copier `client/multiplayer.example.ini` au même endroit sous le nom `multiplayer.ini`, puis l'adapter.
3. Lancer Risen normalement (Windows ou Proton).
4. Lire `bin/scripts/Script_Multiplayer.log` (réécrit à chaque lancement).

Désinstallation : supprimer ces fichiers.

Log attendu (cas nominal) — chaque ligne est préfixée par `[AAAA-MM-JJ HH:MM:SS.mmm] NIVEAU` :

```
[2026-10-15 22:42:24.559] INFO  Risen Multiplayer 0.1.0 chargé depuis …\bin\scripts
[2026-10-15 22:42:24.559] INFO  empreinte Game.dll : TimeDateStamp=0x63c9aff6 SizeOfImage=0x1417000
[2026-10-15 22:42:24.559] INFO  empreinte Engine.dll : TimeDateStamp=0x63ca945d SizeOfImage=0xd3a000
[2026-10-15 22:42:24.559] INFO  version du jeu : Steam x64 (janvier 2023)
[2026-10-15 22:42:24.559] INFO  config OK (rôle=hôte, port=27960, pseudo=Joueur)
[2026-10-15 22:42:24.559] INFO  script « MP_Probe » exposé au chargeur
[2026-10-15 22:42:24.559] INFO  mod actif
```

Version non reconnue (mise à jour Steam), config absente ou invalide : le log en donne
la raison, le mod reste inactif et le jeu fonctionne en solo. Sur une version non
reconnue, le mod n'appelle aucun code du jeu et rend des tables vides : le chargeur
de Risen l'ignore alors silencieusement (pas de boîte de dialogue). Après une mise à jour du
jeu, il faut revérifier la rétro-ingénierie puis ajouter la nouvelle empreinte à la
liste blanche (`src/core/version.cpp`).

## Organisation

```
cmake/clang-cl-xwin.cmake   toolchain croisée Windows x64
src/core/                   logique pure (config, version), testée sous Linux
src/win/                    code Windows : ScriptInit, log, ABI du jeu (GetProcAddress)
tests/                      tests unitaires (mini-framework maison, sans dépendance)
```
