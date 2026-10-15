---
title: 'Product Brief — Risen Multiplayer'
status: final
created: '2026-10-15'
updated: '2026-10-15'
---

# Product Brief — Risen Multiplayer

> **En bref.** Mod coop à 2 joueurs pour Risen 1 (Steam 64 bits) : explorer l’île et combattre ensemble depuis une sauvegarde neutre, le jeu de l’hôte faisant foi. Réussite = J2 (combat coop). Faisabilité technique : GO. Projet plaisir, open source sous GPL-3.0.

## Pourquoi ce projet

Fan de Gothic 3, j’ai trouvé en Risen (Piranha Bytes, 2009) son digne héritier : une île libre à explorer, un univers dense, des PNJ qui vivent leur vie. Risen n’a jamais eu de mode multijoueur, et aucune tentative n’est connue sur son moteur (Genome). L’envie est simple : **se replonger dans Faranga, mais à deux**. Parcourir l’île avec un ami, nettoyer un donjon ensemble, faire les quêtes annexes côte à côte.

C’est un **projet plaisir**, sans ambition commerciale ni communautaire. Il est publié en open source sur GitHub pour que d’autres puissent le reprendre s’ils le souhaitent.

## L’expérience visée

**Deux joueurs, une île.** L’hôte et l’invité partent de la même **sauvegarde préparée, placée en zone neutre**, hors de la trame principale : pas de choix de faction, pas de capture par l’Inquisition. L’hôte y fait ensuite évoluer son propre personnage. L’invité rejoint la partie et **apparaît à côté de l’hôte** avec un héros neuf, équipé d’après son **profil d’invité**. Ce profil (équipement, XP, loot) est conservé d’une session à l’autre.

Ensemble, ils :
- **explorent** librement l’île, chacun se déplaçant où il veut (aucune contrainte de distance en v1) ;
- **combattent les mêmes monstres** : chaque monstre n’a qu’une seule jauge de PV, et sa mort d’un monstre vaut pour les deux ;
- **parlent aux PNJ** : quand l’un déclenche un dialogue, il se joue aussi chez l’autre, qui voit le PNJ parler et entend la voix. Chacun peut parler à un PNJ différent en même temps ;
- **font les quêtes annexes**, qui avancent pour les deux ;
- **se relèvent** : un joueur qui tombe au combat est à terre et doit être relevé par l’autre. C’est une mécanique nouvelle, absente du jeu solo.

**Qui fait autorité.** Le **jeu de l’hôte fait foi** pour tout ce qui doit être juste : le monde, les monstres, les combats, l’état des quêtes. Chez l’invité, les monstres et PNJ ne sont que des répliques. Les dialogues, eux, sont simplement déclenchés en miroir.

## Pour qui

- **Moi et un ami.** Deux fans de Piranha Bytes, sur PC, dont un sous Linux/Proton, qui veulent revivre Risen ensemble sans rien installer de compliqué.
- **Accessoirement**, des curieux de la communauté Gothic/Risen (World of Players, Nexus) qui trouveraient le dépôt et voudraient l’essayer ou le reprendre.

## À quoi on reconnaît la réussite

**Réussi = J2 atteint** : se balader sur l’île avec un ami *et* combattre les mêmes monstres ensemble. Chaque jalon est jouable et testable à deux.

| Jalon | Ce que vous vivez | Ce que ça prouve |
|---|---|---|
| J0 — Ça charge | Risen charge le mod sous Proton ; le mod écrit dans un log | Le chargeur natif `Script_*.dll` et la toolchain fonctionnent |
| J1 — Je te vois | Les deux joueurs se voient marcher et s’animer sur l’île | Le réseau, la création de l’avatar distant et la synchro des animations fonctionnent |
| **J2 — On se bat ensemble** | Vous tuez le même loup : PV partagés, sa mort se voit des deux côtés | Les PNJ obéissent au jeu de l’hôte. **Ligne d’arrivée.** |
| J3 — On parle au monde | Un dialogue lancé chez l’un se joue chez l’autre | Les dialogues sont déclenchés en miroir |
| J4 — On progresse | Le profil invité (XP, loot) persiste, les quêtes annexes avancent pour les deux | La persistance et les quêtes partagées fonctionnent |
| J5 — On se relève | Un joueur à terre est relevé par l’autre | Une nouvelle mécanique de gameplay fonctionne |

## Périmètre

**Dans la v1 (jusqu’à J2) :**
- 2 joueurs ; le serveur tourne sur la machine de l’hôte ;
- un départ depuis une sauvegarde préparée en zone neutre ;
- l’invité apparaît à côté de l’hôte, avec l’équipement de son profil ;
- les déplacements et animations sont visibles des deux côtés ;
- un combat coop contre les mêmes monstres, piloté par le jeu de l’hôte ;
- la cible est le build Steam 64 bits, sous Windows et sous Proton/Wine.

**Après J2 :** voir les jalons J3 à J5.

**Hors périmètre :**
- la quête principale, les factions et l’Inquisition ;
- plus de 2 joueurs ;
- le PvP ;
- une laisse ou une téléportation vers l’hôte (envisagée plus tard) ;
- le chat en jeu (Discord suffit) ;
- toute interface en jeu ou overlay ;
- un anti-triche ;
- la traversée de NAT et les serveurs publics : on se connecte par IP, avec une redirection de port ou un VPN ;
- le build 32 bits « original » et la version GOG [HYPOTHÈSE : GOG non testé].

## Contraintes et risques

**Contrainte légale :** aucun fichier du jeu n’est commité ni redistribué. Il faut posséder Risen sur Steam pour jouer.

| Risque | Gravité | Réponse |
|---|---|---|
| Faire piloter les PNJ par l’hôte (J2) : aucun précédent sur le moteur Genome | Haute | J2 est un jalon à part entière. On étudie GMP (Gothic) et Skyrim Together avant de coder |
| Plus de 1 300 fonctions exportées retournent un objet par valeur, ce qui déclenche le piège de l’ABI MSVC | Moyenne | Toolchain clang-cl + lld-link + xwin, et une convention d’appel explicite |
| Une mise à jour Steam de Risen casse le mod | Moyenne | Vérifier la version du jeu au lancement et refuser proprement si elle ne correspond pas |
| Le hook D3D9 sous Proton/DXVK n’est pas prouvé | Faible (v1) | Aucune interface en jeu en v1 |
| Le temps disponible n’est pas défini | Inconnue | Les jalons sont jouables un par un, chaque palier se suffit à lui-même |

Faisabilité technique : voir la recherche `technical-faisabilite-multijoueur-risen-2026-10-15` (verdict **GO** avec trois ajustements : chargeur natif `Script_*.dll`, toolchain clang-cl, KCP avec démultiplexage par canal).

## Open source et suite

Le projet est publié sur GitHub sous **GPL-3.0** : quiconque le reprend le garde ouvert. Rien n’est promis au-delà. Si J2 est atteint et que l’envie est là, la suite naturelle est J3 à J5, puis une laisse entre joueurs. Plus tard peut-être, un mode qui suit la quête principale avec des phases adaptées.

## Questions ouvertes

- **Zone de départ** : environs de la taverne de Severin ou Harbor Town ? Le critère est d’être neutre vis-à-vis des factions et d’avoir des monstres faciles à proximité. À trancher en jouant.
- **Redistribution de la sauvegarde préparée** : peut-on la publier sur GitHub sans redistribuer de données du jeu ? À vérifier avant publication.
- **Temps disponible** par semaine : non défini.
