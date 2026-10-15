---
title: 'PRD — Risen Multiplayer'
status: final
created: '2026-10-15'
updated: '2026-10-15'
---

# PRD — Risen Multiplayer

## 0. Objet du document

Ce PRD traduit le brief (`briefs/brief-risen-multiplayer-2026-10-15/`) en exigences testables pour **J0 à J2**, la ligne d’arrivée du MVP. J3 à J5 sont seulement listés (§6.2). Les choix techniques (chargeur, toolchain, transport) ne sont pas répétés ici : ils vivent dans `addendum.md` et dans la recherche `technical-faisabilite-multijoueur-risen-2026-10-15`. Les hypothèses que j’ai faites sans confirmation sont balisées `[ASSUMPTION]` et indexées en §9.

## 1. Vision

Revivre l’île de Faranga **à deux** : explorer, combattre les mêmes monstres et voir l’autre joueur agir en temps réel, dans Risen 1 non modifié (version Steam 64 bits), sous Windows ou sous Proton. C’est un projet plaisir, open source sous GPL-3.0.

## 2. Utilisateurs

### 2.1 Jobs To Be Done
- Rejouer à Risen avec un ami, et pas seulement « à côté » de lui : partager les mêmes combats.
- Lancer une session sans manipulations compliquées : une DLL et un fichier de config à copier.
- *(Développeur)* Avancer palier par palier, chaque palier étant jouable.

### 2.2 Parcours clés
- **UJ-1. Léo et Max se retrouvent sur l’île.** Léo (l’hôte, sous Fedora/Proton) lance Risen et charge la sauvegarde préparée, ce qui démarre aussi le serveur. Max (l’invité) a renseigné l’IP de Léo dans sa config. Il lance Risen et charge la même sauvegarde. Son personnage apparaît à côté de celui de Léo. Chacun voit l’autre marcher, courir, sauter et dégainer. **Cas limite :** Max n’arrive pas à se connecter. Il joue seul, et le log indique la raison (hôte injoignable, version différente).
- **UJ-2. Léo et Max tuent ensemble une meute de loups.** Ils s’approchent d’une meute. Les loups attaquent l’un ou l’autre. Les deux frappent les mêmes loups, chaque coup fait baisser la même jauge de PV, et quand un loup meurt, il meurt chez les deux. **Cas limite :** Max meurt et réapparaît automatiquement à côté de Léo. Si Léo meurt, la session prend fin et les deux rechargent la sauvegarde.

## 3. Glossaire

- **Hôte** — Joueur dont le jeu fait foi pour le monde, les PNJ et les combats. Fait tourner le **serveur**. Un seul par **session**.
- **Invité** — Second joueur. Il se connecte au serveur de l’hôte. Exactement un par session.
- **Session** — Partie multijoueur entre l’hôte et l’invité. Commence à la connexion de l’invité, finit à sa déconnexion ou à la mort de l’hôte.
- **Serveur** — Processus qui relaie les messages entre hôte et invité. Il tourne sur la machine de l’hôte et ne simule pas le jeu.
- **Sauvegarde de départ** — Sauvegarde Risen préparée, placée en zone neutre, que les deux joueurs chargent.
- **Avatar** — Représentation, dans le jeu d’un joueur, du personnage de l’autre joueur.
- **Réplique** — PNJ ou monstre dans le jeu de l’invité dont l’état (position, PV, mort) est dicté par le jeu de l’hôte.
- **Config** — Fichier texte à côté de la DLL du mod : rôle (hôte/invité), IP, port, pseudo, équipement de l’invité.

## 4. Fonctionnalités

### 4.1 Chargement du mod (J0)
**Description :** Risen charge le mod au démarrage, sans injecteur, sous Windows comme sous Proton. Le mod écrit un log lisible.

#### FR-1 : Chargement natif
Le mod est chargé automatiquement par Risen dès qu’il est déposé dans le dossier prévu à cet effet.
- Le log contient la version du mod et la version détectée du jeu, à chaque lancement.
- Fonctionne sous Windows et sous Proton sans option de lancement Steam.

#### FR-2 : Vérification de la version du jeu
Le mod détecte une version de Risen non supportée et se désactive proprement.
- Le jeu reste jouable en solo, et le log indique « version non supportée ».

#### FR-3 : Config
Le mod lit la config au démarrage.
- Config absente ou invalide : le mod reste inactif, le jeu est jouable en solo, et le log nomme le champ fautif.

### 4.2 Session et présence (J1) — réalise UJ-1
**Description :** L’hôte héberge, l’invité rejoint, et chacun voit l’avatar de l’autre bouger et s’animer de façon fluide.

#### FR-4 : Héberger
Quand la config indique le rôle hôte, le chargement d’une partie démarre le serveur.

#### FR-5 : Rejoindre
Quand la config indique le rôle invité, le chargement de la sauvegarde de départ déclenche la connexion au serveur de l’hôte.
- La connexion échoue proprement si l’hôte est injoignable, ou si la version du mod ou du protocole diffère. Raison au log, jeu solo intact.
- Une deuxième connexion d’invité est refusée (2 joueurs maximum).

#### FR-6 : Apparition de l’invité
À la connexion, le personnage de l’invité est placé près du personnage de l’hôte, avec l’équipement défini dans sa config.

#### FR-7 : Avatar synchronisé
Chaque joueur voit l’avatar de l’autre se déplacer et jouer ses animations : marche, course, saut, dégainer, attaques de mêlée, mort.
- Aucune téléportation visible en conditions normales (LAN ou VPN, latence ≤ 100 ms). `[ASSUMPTION : seuil de latence]`
- L’avatar disparaît à la déconnexion de l’autre joueur.

#### FR-8 : Fin de session
La session prend fin quand l’invité se déconnecte ou quitte le jeu, ou quand l’hôte quitte le jeu ou meurt. Chaque jeu revient à un état solo, sans crash.

### 4.3 Combat coop (J2) — réalise UJ-2, ligne d’arrivée
**Description :** Les PNJ hostiles proches des joueurs sont simulés par le jeu de l’hôte. Chez l’invité, ce sont des répliques. Les deux joueurs combattent les mêmes monstres en mêlée.

#### FR-9 : Répliques
Dans le jeu de l’invité, la position, l’orientation, l’animation, les PV et la mort des PNJ proches de l’hôte suivent l’état du jeu de l’hôte. Le jeu de l’invité ne prend pas de décision d’IA pour eux.
- Un monstre tué chez l’hôte est mort chez l’invité, et inversement.
- Un monstre peut cibler et attaquer l’invité. `[ASSUMPTION : « proche » = zone chargée autour de l’hôte]`

#### FR-10 : Dégâts de l’invité
Un coup de mêlée porté par l’invité inflige des dégâts au monstre dans le jeu de l’hôte. La jauge de PV visible baisse chez les deux joueurs.

#### FR-11 : Dégâts subis par l’invité
Les attaques des monstres sur l’invité réduisent ses PV dans son propre jeu.

#### FR-12 : Mort et réapparition
- L’invité meurt : il réapparaît automatiquement près de l’hôte, PV pleins, et la session continue.
- L’hôte meurt : la session prend fin (FR-8). Les deux joueurs rechargent la sauvegarde de départ pour rejouer.

## 5. Non-objectifs
- Pas de quête principale, de factions, ni d’Inquisition.
- Pas plus de 2 joueurs, pas de PvP.
- Pas d’interface en jeu : ni menu, ni chat, ni overlay. Discord sert à communiquer.
- Pas de serveurs publics, de matchmaking ou de traversée de NAT. On se connecte par IP, avec redirection de port ou VPN.
- Pas d’anti-triche.

## 6. Périmètre MVP

### 6.1 Inclus
J0 (FR-1 à FR-3), J1 (FR-4 à FR-8), J2 (FR-9 à FR-12). Uniquement la version Steam 64 bits, sous Windows et Proton. Uniquement le combat en mêlée.

### 6.2 Après le MVP
- **J3 :** dialogues déclenchés en miroir. Chacun peut parler à un PNJ différent.
- **J4 :** persistance du profil de l’invité (équipement, XP, loot) sur le serveur, et quêtes annexes qui avancent pour les deux.
- **J5 :** relèvement d’un joueur à terre, en remplacement de la réapparition automatique.
- **Plus tard :** arc et magie en coop, laisse entre joueurs, version GOG.

## 7. Mesures de réussite
- **SM-1 (principale) :** Léo et Max nettoient ensemble un camp de monstres de la zone de départ, sans désynchronisation visible ni crash. Valide FR-7 et FR-9 à FR-12.
- **SM-2 :** une session de 30 minutes d’exploration et de combat se déroule sans crash. Valide FR-4 à FR-8. `[ASSUMPTION : durée cible]`
- **SM-C1 (contre-mesure) :** ne pas optimiser le nombre de PNJ ou de systèmes synchronisés. On vise la fiabilité du strict nécessaire, pas l’étendue.

## 8. Questions ouvertes
1. Zone de départ : la taverne de Severin ou Harbor Town ? Le critère est d’être neutre et d’avoir des monstres de mêlée faciles à proximité.
2. Peut-on publier la sauvegarde de départ sur GitHub ?
3. Un monstre peut-il être chargé chez l’invité et pas chez l’hôte, si les joueurs s’éloignent ? Comportement attendu à définir avec l’architecte. Pas de laisse en v1.

## 9. Index des hypothèses
- §4.2 FR-7 — Une latence ≤ 100 ms (LAN ou VPN) suffit pour un rendu fluide.
- §4.3 FR-9 — « PNJ proches » = ceux de la zone chargée autour de l’hôte.
- §7 SM-2 — La durée de session cible est de 30 minutes.
- §2.2 — « Léo » et « Max » sont des prénoms d’illustration.
