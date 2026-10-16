---
title: 'Addendum — PRD Risen Multiplayer'
created: '2026-10-15'
---

# Addendum technique, pour l’architecte

Source : la recherche `technical-faisabilite-multijoueur-risen-2026-10-15` et le brief. Ces choix sont **techniques** et sortent du PRD.

- **FR-1, chargement :** chargeur natif `bin/scripts/Script_<nom>.dll` + export `ScriptInit`, vérifié en x64. La signature de `ScriptInit` reste à retrouver dans `Script_Game.dll`.
- **Toolchain :** clang-cl + lld-link + xwin. Les méthodes exportées qui retournent un objet par valeur s’appellent avec la forme explicite `Ret* (*)(This*, Ret* out, …)`.
- **Transport :** UDP + KCP (`ikcp.c` / `kcp-go`, sans FEC ni chiffrement), 1 octet de canal (fiable / non fiable), heartbeat applicatif.
- **FR-2, version :** une empreinte (hash) de `Game.dll`/`Engine.dll`, comparée à une liste blanche.
- **FR-3, config :** un fichier texte à côté de la DLL. Format au choix de l’architecte (INI suggéré).
- **FR-7, interpolation :** snapshots rendus avec environ 3 intervalles d’envoi de retard (environ 150 ms à 30 Hz). Hermite pour la position, slerp pour la rotation.
- **Serveur :** processus Go séparé ou intégré au client hôte. À arbitrer, puisque le serveur tourne de toute façon chez l’hôte. Le stockage du profil de l’invité (J4) est hors MVP.

# Écarts décidés après le spike J1 (2026-10-16)

- **FR-6, équipement de l’invité :** reporté de J1 à J2, avec l’arme en main de l’avatar (Epic 3 de `epics.md`). En J1, l’invité est placé près de l’hôte, avec son propre équipement de sauvegarde. Le champ équipement reste dans `Hello` dès la v1 du protocole.
- **Outil de test :** deux instances de Risen ne tournent pas sur la même machine. Les tests de session et d’avatar se font avec un faux invité en Go (`tools/fake-guest/`, jamais livré) ; la validation finale de J1 se fait à deux machines.
