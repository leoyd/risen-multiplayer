# Epic 2 Context: Voir l’autre joueur bouger

<!-- Compiled from planning artifacts. Edit freely. Regenerate with compile-epic-context if planning docs change. -->

## Goal

En session, chacun voit l’avatar de l’autre : celui de l’invité apparaît près de l’hôte, se déplace sans à-coups et rejoue les animations du joueur distant (marche, course, saut, dégainer, attaques de mêlée, mort), puis disparaît à la fin de la session. C’est la présence de J1, posée sur la session de l’Epic 1 et préalable au combat coop (Epic 3). La première story lève le risque principal de l’animation : rejouer une animation à partir de son seul nom et d’un descripteur reconstruit, sans réseau.

## Stories

- Story 2.1 : Miroir local par nom (lever le risque d’AD-8)
- Story 2.2 : Apparition et retrait des avatars
- Story 2.3 : Déplacements fluides de l’avatar
- Story 2.4 : Animations de l’avatar

## Requirements & Constraints

- **Apparition** : à la connexion, le héros de l’invité est placé près de celui de l’hôte, avec son propre équipement de sauvegarde (l’équipement de la config et l’arme en main sont reportés à J2).
- **Avatar synchronisé** : aucune téléportation visible en LAN/VPN (latence ≤ 100 ms, seuil supposé). Animations attendues : marche, course, saut, dégainer (animation seule), attaques de mêlée, mort.
- **Retrait** : l’avatar disparaît à toute fin de session ; il n’y a jamais deux avatars à la fois.
- **Robustesse** : aucun échec du mod ne fait crasher le jeu ; toute erreur laisse le jeu en solo avec la raison au log. Rien n’est journalisé par frame hors `log_level = debug`.
- **Tests** : une seule instance de Risen par machine. Tests quotidiens = Risen en hôte + faux invité Go (`tools/fake-guest/`) ; validation finale à deux machines.

## Technical Decisions

- **Threads (AD-2)** : le jeu n’est appelé que depuis le hook `eCApplication::OnProcess`, sauf le hook `PlayMotion`, qui ne fait que copier des octets ou bloquer l’appel. Le seul état partagé hook ↔ frame est un objet unique `sync::AnimFilter` sous verrou : PS d’animation du héros local (capturé), PS des répliques (appels du moteur bloqués), anneau des captures. Le rejeu (`SetMotion` + `PlayMotion` + `MoveTo`) se fait sous un garde `thread_local` que le hook laisse passer ; le hook ne bloque jamais le PS du héros local ; une entité sort du filtre avant d’être oubliée ou détruite. Hypothèse à journaliser : le hook tourne sur le thread principal.
- **Cadences à l’horloge (QPC)**, jamais en nombre d’appels d’`OnProcess` (160–250/s en jeu).
- **ABI (AD-3)** : seul `game/` appelle le jeu ; résolution par nom décoré en tout ou rien ; sret explicite `Ret* (*)(This*, Ret* out, …)` ; un objet passé par valeur est détruit par l’appelé, un objet retourné par valeur appartient au mod. Offsets codés en dur (emplacements de motion) documentés dans `docs/re/` et couverts par l’empreinte de version. SEH autour de chaque appel.
- **Animation (AD-8)** : hook de `eCAnimation_PS::PlayMotion` (Engine.dll). Nom = ressource de l’emplacement `this + 0x120 + type*0x10`, puis `eCResource2::GetFileTitleA()`. Noms internés : `AniName{AniId u16, nom}` la première fois, puis `Motion{NetId, tick/ms, type u8, AniId u16, flag u8, paramètres}` sur le canal fiable ; espace d’`AniId` par émetteur, remis à zéro à `Ended`. Rejeu : `eCResource2::GetMotion(nom)` → `SetMotion(type, ressource)` → `PlayMotion(type, descripteur reconstruit, flag)`. Le descripteur `eSMotionPlayBackDesc` (0x60 o) contient des pointeurs : jamais transmis brut, seuls des champs scalaires identifiés (hypothèse : fondu f32 à +0x00, poids à +0x0C/+0x14). Le moteur ne doit jamais animer une réplique de lui-même (hook bloquant). Types 0 et 1 = locomotion : seul le plus récent par type et par frame s’applique ; un `AniId` inconnu fait ignorer le `Motion` avec une ligne de log.
- **Déjà validé** (sonde, mode miroir v5) : rejeu par pointeur de ressource et descripteur brut dans le même processus, placement par `MoveTo`. **Non validé** : chemin par nom et descripteur reconstruit.
- **Avatar** : créé uniquement à réception de `Spawn{NetId, type, pose}` par `Entity::Spawn(template du héros local, pose, temporary = true)` (la copie ne prend pas le contrôle). `sync/avatar` est le seul propriétaire de sa pose et de son animation. Placement à chaque frame par `Entity::MoveTo(bCMatrix)` (pas `SetPose`). `bCMatrix` en lignes, translation `m[12..14]`, Y vertical, avant = ligne 2, centimètres. Sonde (`probe = 1`) et session exclusives : un seul détour `PlayMotion` actif.
- **Identité (AD-6)** : `NetId` u32 attribué par l’hôte ; 1 = avatar hôte, 2 = avatar invité. Table `NetId ↔ entité` par pair ; aucun pointeur sur le réseau ni gardé hors de cette table ou après `Ended`.
- **Snapshots (AD-7)** : canal 1, 20 Hz, état complet : `u8 canal, u32 tick, u8 nombre`, puis 22 o par entité (`NetId` u32, position 3×f32, lacet f32 = `atan2(m[8], m[10])`, PV u16). `tick = floor((QPC − t0)/50 ms)` depuis l’entrée en `InSession` ; `Motion` daté en ms (u32) dans la même base ; une horloge distante par pair dans `core/`. Affichage à 150 ms de retard (Hermite position, slerp rotation), extrapolation ≤ 250 ms, jamais d’attente de retransmission. `Spawn`/`Respawn` réinitialisent l’interpolation.
- **Messages (AD-10)** : liste fermée ; ajouter un type exige de mettre à jour `protocol/PROTOCOL.md`, d’incrémenter la version du protocole et d’ajouter les vecteurs `protocol/vectors/*.bin`, vérifiés par les tests C++ et Go.
- **Fin de session (AD-11)** : la transition unique vers `Ended` supprime les avatars (retrait de l’`AnimFilter` et de la table `NetId`, puis `Despawn`).
- **Conventions** : couches `core/` (pur, tests Linux), `net/`, `game/`, `sync/` (banc Wine + jeu réel) ; espaces `mp::core|net|game|sync|win|log` ; pas d’exception entre couches ; préfixes de log `[net]`, `[game]`, `[sync]` ; toute nouvelle clé de config a un défaut dans `multiplayer.example.ini`.

## Cross-Story Dependencies

- 2.1 fixe la disposition de `Motion` (PROTOCOL.md + vecteur) et l’`AnimFilter` réutilisés par 2.2 et 2.4.
- 2.2 s’appuie sur la session de l’Epic 1 (passage en `InSession`, pose de `Welcome`, transition `Ended`) et ajoute `Spawn`/`Despawn` au protocole et au faux invité.
- 2.3 ajoute le snapshot, l’horloge distante et l’interpolation (`core/`), et un mode `--scenario` au faux invité ; 2.4 réutilise ce scénario pour des noms d’animation horodatés.
- 2.4 dépend de 2.1 (chemin par nom, `Motion`), 2.2 (avatar) et 2.3 (instant d’interpolation).
