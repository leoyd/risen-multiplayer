# Risen Multiplayer

Mod multijoueur en ligne (coop **strictement 2 joueurs**) pour **Risen 1** (Piranha Bytes, 2009), version Steam.
Projet perso, pour le fun, publié en open source (**GPL-3.0**). Méthode de travail : BMAD (Analyst → PM → Architect → Dev).

Docs de référence : brief `_bmad-output/planning-artifacts/briefs/brief-risen-multiplayer-2026-10-15/`
(+ `addendum.md` = écarts avec ce fichier), recherche technique
`_bmad-output/planning-artifacts/research/technical-faisabilite-multijoueur-risen-2026-10-15/research.md`.
Langue de travail : français.

## Cible : le jeu (`risen_ori/`, NON versionné)

Copie de l'install Steam (app 40300). **Ne jamais commiter ni redistribuer de fichier du jeu.**

- Version Steam actuelle = **64 bits** (PE32+ x86-64), runtime MSVC 2015+ (`VCOMP140`).
- Rendu : **Direct3D 9** (`d3d9.dll`, `d3dx9_39.dll`) dans `Engine.dll`.
- Moteur Genome découpé en DLL dans `risen_ori/bin/` :
  - `Risen.exe` — lanceur, charge les DLL ci‑dessous
  - `Engine.dll` (~29 700 exports) — préfixe `e` (`eCEntity`, …), rendu, input, physique
  - `Game.dll` (~21 700 exports) — préfixe `g` (`gCSession`, `gCNavigation*`, IA, quêtes…)
  - `Script.dll` (~5 200 exports) — logique de script du gameplay
  - `SharedBase.dll` (~9 600 exports) — préfixe `b` (`bCObjectBase`, conteneurs, maths)
  - `FFC.dll`, `FFCore.dll`, `GUI2.dll`, `FileSystem2.dll`, `Music.dll`, `Importer.dll`
  - Tiers : `steam_api64`, `discord_game_sdk`, `sentry`, `fmod`, `binkw64`
- **Atout majeur** : les DLL exportent des milliers de symboles C++ décorés MSVC
  (ex. `?CreatePlayerEntity@gCSession@@IEAAXXZ`). On peut résoudre les fonctions par nom
  (`GetProcAddress`) au lieu de chercher des signatures/adresses. Lister : `objdump -p <dll>`,
  démangler avec `llvm-undname` ou Ghidra.
- **Point d'entrée du mod** : chargeur natif de plugins — le jeu charge `bin/scripts/Script_*.dll` et
  appelle l'export `ScriptInit` (vérifié en x64 : `Script_Game.dll`, chaînes dans `Game.dll`). Pas
  d'injecteur ni de proxy DLL. `ScriptInit()` sans argument retourne 4 tables (AIState, AIFunction,
  AICallback, Script) ; tables vides = rejet silencieux. Implémenté et validé en jeu (J0).
- **Piège ABI** : >1 300 méthodes exportées retournent un objet par valeur (ex. `Entity::GetPose → bCMatrix`).
  MSVC x64 passe alors un pointeur de retour caché **après** `this` (RCX=this, RDX=sret). Déclarer ces
  appels explicitement `Ret* (*)(This*, Ret* out, …)` ; jamais de pointeur de membre C++.
- Données : archives `.pak` dans `risen_ori/data/{common,compiled}`, config XML dans `data/ini`.

## Architecture (spine : `_bmad-output/planning-artifacts/architecture/architecture-risen-multiplayer-2026-10-15/ARCHITECTURE-SPINE.md`)

Les invariants AD-1 à AD-11 de la spine font foi. Résumé :

```
client/src/core/    logique pure (config, version, protocole, interpolation) — testée sous Linux
client/src/net/     UDP + KCP (ikcp.c), démultiplexage des canaux, thread réseau
client/src/game/    adaptateur ABI Risen — SEUL endroit qui appelle le jeu
client/src/sync/    session, avatars, réplication des PNJ, tick par frame
client/src/win/     ScriptInit, hooks MinHook ; client/src/common/ : log
protocol/           PROTOCOL.md — contrat binaire hôte⇄invité
```

- **Pas de serveur séparé** : la DLL de l'hôte héberge (1 invité max). Pas de `server/` ni de Go dans
  le produit livré. Seule exception (AD-1 amendé) : `tools/fake-guest/`, faux invité en Go pour tester
  l'hôte avec une seule instance de Risen, jamais livré ; il suit `protocol/PROTOCOL.md`, est vérifié
  contre `protocol/vectors/*.bin` et compile le même `ikcp.c` via cgo.
- Transport : un socket UDP, octet 0 = canal (0 KCP fiable, 1 snapshot non fiable 20 Hz, 2 contrôle
  Hello/Welcome/Reject). KCP `ikcp_nodelay(1,10,2,1)`, `ikcp_setmtu(1199)`, datagramme ≤ 1200 o.
- Tout appel au jeu sur le thread principal, depuis le hook `eCApplication::OnProcess`.
- ABI : retours d'objets via sret explicite ; classes passées par valeur détruites par l'appelé.
- Sérialisation : structs binaires little-endian empaquetées. Pas de protobuf.
- Autorité : le jeu de l'hôte fait foi (monde, PNJ, combats, quêtes) ; chez l'invité les PNJ sont des
  répliques. Les dialogues sont simplement déclenchés en miroir.
- Départ : sauvegarde préparée en zone neutre (hors trame principale/factions), commune aux 2 joueurs.
- Jalons : J0 ça charge → J1 on se voit bouger → **J2 combat coop (réussite)** → J3 dialogues →
  J4 persistance profil invité + quêtes annexes → J5 relever un joueur à terre.

## Environnement

- Dev sous Fedora Linux, IDE GoLand. Le jeu tourne sous Proton/Wine.
- Client : **clang-cl + lld-link + xwin** (toolchain CMake LLVM `WinMsvc.cmake`), qui suit l'ABI MSVC.
  MinGW‑w64 g++ écarté : il gère mal les retours d'objets des méthodes MSVC (GCC PR 64384).
  Appeler les méthodes du jeu via pointeurs de fonction (pas en liant les .lib). Installé :
  clang/lld 23.1.3 (dnf), xwin 0.10.0 (`~/.local/bin`), CRT+SDK dans `~/.xwin`. Voir `client/README.md`.

<!-- rtk-instructions v2 -->
# RTK (Rust Token Killer) - Token-Optimized Commands

## Golden Rule

**Always prefix commands with `rtk`**. If RTK has a dedicated filter, it uses it. If not, it passes through unchanged. This means RTK is always safe to use.

**Important**: Even in command chains with `&&`, use `rtk`:
```bash
# ❌ Wrong
git add . && git commit -m "msg" && git push

# ✅ Correct
rtk git add . && rtk git commit -m "msg" && rtk git push
```

## RTK Commands by Workflow

### Build & Compile (80-90% savings)
```bash
rtk cargo build         # Cargo build output
rtk cargo check         # Cargo check output
rtk cargo clippy        # Clippy warnings grouped by file (80%)
rtk tsc                 # TypeScript errors grouped by file/code (83%)
rtk lint                # ESLint/Biome violations grouped (84%)
rtk prettier --check    # Files needing format only (70%)
rtk next build          # Next.js build with route metrics (87%)
```

### Test (60-99% savings)
```bash
rtk cargo test          # Cargo test failures only (90%)
rtk go test             # Go test failures only (90%)
rtk jest                # Jest failures only (99.5%)
rtk vitest              # Vitest failures only (99.5%)
rtk playwright test     # Playwright failures only (94%)
rtk pytest              # Python test failures only (90%)
rtk rake test           # Ruby test failures only (90%)
rtk rspec               # RSpec test failures only (60%)
rtk test <cmd>          # Generic test wrapper - failures only
```

### Git (59-80% savings)
```bash
rtk git status          # Compact status
rtk git log             # Compact log (works with all git flags)
rtk git diff            # Compact diff (80%)
rtk git show            # Compact show (80%)
rtk git add             # Ultra-compact confirmations (59%)
rtk git commit          # Ultra-compact confirmations (59%)
rtk git push            # Ultra-compact confirmations
rtk git pull            # Ultra-compact confirmations
rtk git branch          # Compact branch list
rtk git fetch           # Compact fetch
rtk git stash           # Compact stash
rtk git worktree        # Compact worktree
```

Note: Git passthrough works for ALL subcommands, even those not explicitly listed.

### GitHub (26-87% savings)
```bash
rtk gh pr view <num>    # Compact PR view (87%)
rtk gh pr checks        # Compact PR checks (79%)
rtk gh run list         # Compact workflow runs (82%)
rtk gh issue list       # Compact issue list (80%)
rtk gh api              # Compact API responses (26%)
```

### JavaScript/TypeScript Tooling (70-90% savings)
```bash
rtk pnpm list           # Compact dependency tree (70%)
rtk pnpm outdated       # Compact outdated packages (80%)
rtk pnpm install        # Compact install output (90%)
rtk npm run <script>    # Compact npm script output
rtk npx <cmd>           # Compact npx command output
rtk prisma              # Prisma without ASCII art (88%)
rtk uv run <cmd>        # Compact uv project command output
```

### Files & Search (60-75% savings)
```bash
rtk ls <path>           # Tree format, compact (65%)
rtk read <file>         # Code reading with filtering (60%)
rtk grep <pattern>      # Search grouped by file (75%). Format flags (-c, -l, -L, -o, -Z) run raw.
rtk find <pattern>      # Find grouped by directory (70%)
```

### Analysis & Debug (70-90% savings)
```bash
rtk err <cmd>           # Filter errors only from any command
rtk log <file>          # Deduplicated logs with counts
rtk json <file>         # JSON structure without values
rtk deps                # Dependency overview
rtk env                 # Environment variables compact
rtk summary <cmd>       # Smart summary of command output
rtk diff                # Ultra-compact diffs
```

### Infrastructure (85% savings)
```bash
rtk docker ps           # Compact container list
rtk docker images       # Compact image list
rtk docker logs <c>     # Deduplicated logs
rtk kubectl get         # Compact resource list
rtk kubectl logs        # Deduplicated pod logs
```

### Network (65-70% savings)
```bash
rtk curl <url>          # Compact HTTP responses (70%)
rtk wget <url>          # Compact download output (65%)
```

### Meta Commands
```bash
rtk gain                # View token savings statistics
rtk gain --history      # View command history with savings
rtk discover            # Analyze Claude Code sessions for missed RTK usage
rtk proxy <cmd>         # Run command without filtering (for debugging)
rtk init                # Add RTK instructions to CLAUDE.md
rtk init --global       # Add RTK to ~/.claude/CLAUDE.md
```

## Token Savings Overview

| Category | Commands | Typical Savings |
|----------|----------|-----------------|
| Tests | vitest, playwright, cargo test | 90-99% |
| Build | next, tsc, lint, prettier | 70-87% |
| Git | status, log, diff, add, commit | 59-80% |
| GitHub | gh pr, gh run, gh issue | 26-87% |
| Package Managers | pnpm, npm, npx | 70-90% |
| Files | ls, read, grep, find | 60-75% |
| Infrastructure | docker, kubectl | 85% |
| Network | curl, wget | 65-70% |

Overall average: **60-90% token reduction** on common development operations.
<!-- /rtk-instructions -->