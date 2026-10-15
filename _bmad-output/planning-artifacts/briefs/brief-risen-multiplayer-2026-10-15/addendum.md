---
title: 'Addendum — Product Brief Risen Multiplayer'
created: '2026-10-15'
---

# Addendum : écarts avec le CLAUDE.md, à transmettre à l’architecte et au PM

Ces décisions, prises pendant le brief, modifient des hypothèses écrites dans le `CLAUDE.md` du projet. Elles relèvent de l’architecture ou du PRD plutôt que du brief.

| Sujet | CLAUDE.md actuel | Décision du brief | Conséquence |
|---|---|---|---|
| Nombre de joueurs | Coop 2 à 8 | **Strictement 2** | Le protocole et le serveur peuvent être simplifiés |
| Serveur | Binaire Go relais, sans état | **Tourne sur la machine de l’hôte** et **stocke le profil invité** (équipement, XP, loot) | Le serveur a un état persistant. Faut-il encore un processus séparé, ou peut-il être intégré au client hôte ? À arbitrer par l’architecte |
| MVP | Se voir bouger, sans combat ni PNJ | **La réussite est J2** : combat coop avec PNJ pilotés par l’hôte. « Se voir bouger » devient J1 | L’autorité de l’hôte sur les PNJ est au cœur de la v1, pas en option |
| Autorité | Le premier client simule les PNJ | **Le jeu de l’hôte fait foi** pour le monde, les combats et les quêtes. Les **dialogues sont déclenchés en miroir** chez l’invité | Deux voies de synchro distinctes : l’état autoritaire d’un côté, les événements de dialogue de l’autre |
| Démarrage | Non défini | **Sauvegarde préparée en zone neutre**, commune aux deux joueurs | Il faut produire cette sauvegarde. L’invité charge le monde de la même sauvegarde |
| Toolchain | MinGW-w64 (plan B : clang-cl) | **clang-cl + lld-link + xwin** (d’après la recherche) | Mettre à jour le CLAUDE.md |
| Chargement du client | DLL injectée | **Chargeur natif `bin/scripts/Script_*.dll` + `ScriptInit`** (d’après la recherche) | Ni injecteur ni proxy DLL |
| Connexion | Non défini | Par IP, avec redirection de port ou VPN (Tailscale, ZeroTier) | Pas de traversée de NAT en v1 |
| Licence | Non définie | **GPL-3.0** | Ajouter un fichier `LICENSE` |

**Nouvelle mécanique de gameplay (J5) :** un état « à terre » et le relèvement par l’autre joueur. Le jeu solo ne la possède pas. Il faudra définir ce qui se passe si les deux joueurs tombent.
