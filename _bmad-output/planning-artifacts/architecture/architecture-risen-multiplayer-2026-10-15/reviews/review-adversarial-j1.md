---
type: review
lens: adversarial
target: ARCHITECTURE-SPINE.md (J1–J2, mise à jour post-spike J1 du 2026-10-16)
focus: AD-2, AD-7, AD-8, AD-10, AD-11
date: 2026-10-16
verdict: needs-revision
---

# Revue adversariale — spine Risen Multiplayer après le spike J1

**Verdict : à resserrer avant de découper les stories J1.** Les AD mis à jour après le spike sont justes sur le *quoi* (rejeu par nom, MoveTo, horloge QPC, détection du monde chargé), mais laissent ouverts le *quand* (sémantique du `tick`), le *qui* (propriétaire unique de la création, du placement et de l'animation d'une entité) et le *comment partagé* (état commun au hook `PlayMotion` et à la frame). On peut écrire deux stories qui respectent chaque AD à la lettre et qui ne s'emboîtent pas.

Méthode : pour chaque trou, deux unités du niveau inférieur (story ou module de `core/`, `net/`, `game/`, `sync/`, `win/`) conformes à la lettre, puis la divergence concrète et le resserrement d'AD qui la ferme.

Sévérité : **B** = bloquant (incompatibilité binaire, crash ou comportement faux garanti), **M** = majeur (divergence probable, coûteuse à rattraper), **m** = mineur.

---

## B1 — Le `tick` n'a pas de définition : deux horloges incompatibles (AD-2, AD-7, AD-8, Conventions)

**Textes en jeu.** Conventions : « temps en `tick` (`u32`, 20 Hz) ». AD-2 : « toute cadence se mesure à l'horloge ». AD-7 : snapshot = `tick` + entités ; retard d'affichage de 150 ms. AD-8 : `Motion{NetId, tick, …}`, appliqué « au même retard d'interpolation que les snapshots ».

**Unité A — `sync/tick` (émetteur).** `tick = floor((QPC_now − QPC_t0) / 50 ms)`, `t0` = entrée en `InSession` du pair. Après une frame de 120 ms (chargement d'une zone), les ticks sautent de 2 : conforme (cadence à l'horloge).
**Unité B — `sync/tick` (autre lecture).** Un compteur incrémenté à chaque envoi, l'envoi étant déclenché dès que 50 ms se sont écoulées. Après la même frame de 120 ms, le tick n'avance que de 1 : conforme aussi (20 Hz « en moyenne », mesuré à l'horloge).
**Unité C — `core/interpolation` (récepteur).** Convertit `tick × 50 ms` en temps distant, puis estime le décalage avec l'horloge locale. Face à B, le temps distant se comprime après chaque à-coup : l'avatar accélère puis se fige.

**Deuxième divergence, sur `Motion`.** À 160–250 images/s, plusieurs `PlayMotion` tombent dans le même tick de 50 ms (le spike voit les types 0 et 1 en paires, et une attaque peut suivre un pas de course). Une story `sync/avatar` estampille `Motion` avec le tick du *prochain* snapshot, une autre avec celui du *dernier* envoyé : l'animation est décalée de ±50 ms par rapport à la pose, et l'ordre relatif motion/pose au sein d'un tick n'est pas défini. Enfin, rien ne dit si `tick` est **par pair** (chacun sa base) ou **commun** (base de l'hôte, transmise dans `Welcome`) : le récepteur ne peut pas mettre sur une même ligne de temps les snapshots et les `Motion` d'un même émetteur s'il ne sait pas qu'ils partagent la même base.

**Resserrement (AD-7, nouvel alinéa « Temps »).**
- `tick` = `floor((QPC − t0) / 50 ms)` de **l'émetteur**, `t0` = son passage en `InSession`. Les trous de tick sont normaux ; un tick n'est jamais réémis.
- Chaque pair tient, dans `core/`, **une** horloge distante par pair (`RemoteClock`) : décalage = min glissant de `(arrivée locale − tick × 50 ms)`. Snapshots **et** événements datés d'un même émetteur passent par elle.
- `Motion` porte un temps à résolution milliseconde dans la même base : `tick` u32 + `offset_ms` u8 (0..49), ou directement `t_ms` u32. Le récepteur ordonne snapshots et `Motion` d'un émetteur sur cette seule ligne de temps.

---

## B2 — État partagé hook `PlayMotion` ↔ frame : non gouverné, et « de lui-même » indécidable (AD-2, AD-8)

**Textes en jeu.** AD-2 : le thread réseau et la frame ne partagent que deux files ; exception : le hook `PlayMotion` « ne fait que copier des octets ou bloquer ». AD-8 : le hook ne retient que le PS du héros local, et **bloque** tout `PlayMotion` que le moteur lance « de lui-même » sur le PS d'un avatar.

AD-2 règle le partage réseau ↔ frame, mais pas hook ↔ frame. Or le hook doit lire au moins : le PS du héros local, l'ensemble des PS d'avatars, et un indicateur « c'est notre rejeu ». Et il doit déposer ses captures quelque part.

**Unité A — `win/hooks` + `sync/avatar`.** Les PS sont des `std::atomic<void*>` ; « notre rejeu » est un booléen global posé autour de l'appel `PlayMotion` seul (comme la sonde : `g_replaying`). Les captures vont dans un anneau protégé par SRWLOCK.
**Unité B — `game/animation`.** Expose `replay(ps, type, res, desc)`, qui pose le garde autour de `SetMotion` + `PlayMotion`. `sync/avatar` appelle ensuite `MoveTo` **hors** garde.

**Divergences.**
1. `MoveTo` recale les property sets 0xd et 5 (spike v4). S'il déclenche un `PlayMotion` sur l'avatar, A le bloque, B le bloque aussi, mais une troisième story qui pose le garde autour de tout `apply_avatar()` le laisse passer. Le comportement visible dépend de l'endroit du garde, que la spine ne fixe pas.
2. Le thread d'appel de `PlayMotion` n'est **pas vérifié** (seul `OnProcess` l'est). Le jeu lie `VCOMP140` (OpenMP). Si un `PlayMotion` moteur tombe sur un thread de travail pendant notre fenêtre de rejeu, un garde global le laisse passer. Un garde `thread_local` le bloque. Les deux lectures sont conformes.
3. Durée de vie : à la transition `Ended` (AD-11), l'avatar est détruit. Si la frame détruit l'entité **avant** de vider le filtre, le hook compare encore l'ancien pointeur de PS ; si l'allocateur le réattribue à un PNJ, ses animations sont bloquées à vie (PNJ figé), ou ses octets sont lus via un pointeur pendant.
4. Anneau plein : la sonde perd des captures au-delà de 64 par frame. Une story perd silencieusement, une autre bloque l'appel original pour ne rien perdre (conforme : « bloquer »). La seconde gèle le héros local.

**Resserrement (AD-2 + AD-8).**
- AD-2 : « Le hook `PlayMotion` et la frame ne partagent qu'**un** objet, `sync::AnimFilter` : PS du héros (atomique), ensemble fixe d'au plus 2 PS d'avatars (atomiques), anneau de captures sous verrou. Le hook n'appelle aucune fonction du jeu, n'alloue pas, et **ne bloque jamais un appel sur le PS du héros local** ; un anneau plein perd la capture et incrémente un compteur. »
- AD-8 : « Un appel est “le nôtre” si et seulement si un garde **`thread_local`** est posé ; `game/animation` le pose autour de **`SetMotion` + `PlayMotion` + `MoveTo`** de l'avatar. Tout autre `PlayMotion` sur un PS d'avatar est bloqué, quel que soit le thread. »
- AD-11 : « Sur la transition vers `Ended`, `AnimFilter` est vidé **avant** toute destruction d'entité. »
- Ajouter au spike (ou à la story avatar) : journaliser le thread de chaque `PlayMotion` pendant une session de 10 minutes.

---

## B3 — Fin de monde mal détectée : pointeur du joueur réutilisé, retour au menu non testé, mort de l'invité (AD-11)

**Textes en jeu.** AD-11 : « un **changement du pointeur du joueur** vaut fin de session » ; « monde chargé = `GetPlayer()` non nul (vérifié) ». Le spike dit : « le retour au menu n'a pas encore été testé ».

**Unité A — `sync/session`.** Compare chaque frame `GetPlayer()` au pointeur mémorisé ; s'il change, transition vers `Ended`.
**Unité B — `sync/avatar`.** Garde `eCDynamicEntity*` de l'avatar entre les frames et appelle `MoveTo` dessus.

**Divergence (crash).** Chargement rapide d'une sauvegarde : le monde est détruit puis recréé, et l'allocateur rend souvent **la même adresse** pour le nouveau héros. Si aucune frame n'observe `GetPlayer() == nullptr` entre les deux (rien ne le garantit : le chargement peut se faire dans un seul appel d'`OnProcess`), A ne voit aucun changement. B appelle `MoveTo` sur l'avatar détruit avec le monde. SEH rattrape peut-être l'accès (AD-3), mais pas une écriture dans un bloc réattribué.

**Seconde divergence (J2, FR-12).** AD-11 : « à la mort de l'invité, l'hôte envoie `Respawn` ». Mais la mort du héros en solo mène Risen à l'écran de mort puis au chargement d'une sauvegarde, donc à un changement du pointeur du joueur, donc à `Ended`. Une story `sync/session` applique la règle du pointeur et termine la session ; une story `sync/respawn` attend `Respawn`. Les deux sont conformes ; la session ne survit jamais à la mort de l'invité. Il faut empêcher la séquence de mort native chez l'invité, et rien dans la spine ne l'annonce.

**Resserrement (AD-11).**
- « Fin de monde = l'un des signaux suivants, testés à chaque frame **avant** tout appel sur une entité : `GetPlayer()` nul ; `GetPlayer()` différent ; changement d'un **compteur de génération du monde** (par exemple `gCSession::AccessState()` qui quitte l'état “en jeu”, ou hook de la fonction de chargement). Le choix du signal est vérifié en jeu (retour au menu, chargement rapide vers la même sauvegarde) **avant** la story session. »
- « Aucun module ne garde un pointeur d'entité d'une frame à l'autre hors de la table `NetId ↔ entité` de `sync/` ; cette table est vidée par la transition `Ended`. »
- Ajouter une question ouverte J2 : « intercepter la mort du héros invité (pas d'écran de mort, pas de rechargement) avant la story FR-12 ». Tant qu'elle n'est pas résolue, la clause `Respawn` d'AD-11 est une hypothèse, à marquer `[ASSUMPTION]`.

---

## B4 — Animation des PNJ répliqués : aucun chemin légal, deux propriétaires (AD-7, AD-8, AD-9)

**Textes en jeu.** AD-9 : chez l'invité, « position, **animation**, PV et mort [des PNJ] viennent exclusivement des snapshots et des événements ». AD-7 : « l'animation **n'est pas** dans le snapshot : c'est un flux d'événements `Motion` (AD-8) ». AD-8 : le hook « ne retient que les appels sur le PS d'animation **du héros local** » et ne bloque que sur le PS **d'un avatar** ; « seul `sync/avatar` anime un avatar ».

**Unité A — `sync/npc` (hôte).** Doit émettre l'animation des PNJ. AD-8 interdit d'élargir le filtre de capture au-delà du héros ; AD-7 interdit de la mettre dans le snapshot. Elle invente donc un état d'animation dérivé (`GetMovementMode`, etc.) dans un nouveau message : conforme à AD-10 si la version du protocole est incrémentée.
**Unité B — `sync/npc` (autre lecture).** Élargit le hook à « toute entité répliquée », en considérant que `Motion{NetId}` le permet déjà puisqu'il porte un `NetId`.

**Divergences.** Deux encodages d'animation, ce qu'AD-8 dit justement empêcher. Côté invité, le hook ne bloque pas les `PlayMotion` du moteur sur les répliques de PNJ, qui ne sont pas des avatars. Même avec l'IA neutralisée, le moteur leur lance des attentes et des quarts de tour (la sonde en a vu ≈ 1/s sur la copie) : **deux propriétaires** de l'animation d'un PNJ. Le volume compte aussi : 20 PNJ en combat à 160–250 images/s produisent un flux fiable qui peut saturer KCP (fenêtre, blocage en tête de file) et retarder les `Motion` de l'avatar.

**Resserrement (AD-8 généralisé).**
- « Le rejeu par nom s'applique à **toute entité dont le pair local n'a pas l'autorité** (avatar distant, réplique de PNJ). Chez l'émetteur, la capture couvre le héros local et, pour l'hôte, les PNJ répliqués. Chez le récepteur, tout `PlayMotion` du moteur sur le PS d'une entité non autoritaire est bloqué. »
- « Seul `sync/replica` (avatar ou PNJ) anime une entité non autoritaire » remplace « seul `sync/avatar` anime un avatar ».
- Fixer un budget : au plus N `Motion`/s par entité PNJ (ou fusion par tick et par type), avec `AnimFilter` limité à un ensemble borné. Sinon, renvoyer explicitement l'animation des PNJ à un AD J2 distinct, à écrire **avant** toute story `sync/npc`, comme AD-9 le fait déjà pour l'appariement.

---

## B5 — Poignée de main : un `Hello` retransmis est une « seconde demande » (AD-1, AD-4, AD-10, AD-11)

**Textes en jeu.** AD-4 : `Hello`/`Welcome`/`Reject` « renvoyés toutes les 500 ms jusqu'à la réponse ». AD-1 : « une seconde demande de connexion est refusée par un message `Reject` ».

**Unité A — `sync/session` hôte.** Au premier `Hello`, elle passe en `InSession`, tire un `conv` et crée son KCP. Tout `Hello` suivant est une « seconde demande » : `Reject`.
**Unité B — `net/` invité.** Le `Welcome` s'est perdu ; elle renvoie `Hello` 500 ms plus tard, reçoit `Reject` et passe en `Ended` (AD-11 : `Reject` termine la session).

Résultat : 1 % de perte suffit à faire échouer la connexion de temps en temps, alors que les deux unités respectent la lettre. Le même problème existe à l'envers : un `Welcome` « renvoyé toutes les 500 ms jusqu'à la réponse », mais `Welcome` n'appelle aucune réponse sur le canal de contrôle.

**Resserrement (AD-1 / AD-5).** « L'hôte identifie l'invité par son adresse `ip:port`. Un `Hello` de la même adresse pendant la session renvoie **le même** `Welcome` (même `conv`). Seul un `Hello` d'une autre adresse reçoit `Reject`. L'hôte arrête de renvoyer `Welcome` dès qu'il reçoit le premier segment KCP portant ce `conv`. »

---

## M1 — Cycle de vie de l'avatar : trois déclencheurs de création, deux propriétaires de la pose (AD-7, AD-8, AD-10, AD-11)

**Textes en jeu.** `Welcome{conv, pose de l'avatar hôte}` (AD-5) ; `Spawn{NetId, type, pose}` (AD-10) ; AD-8 : avatar créé par `Entity::Spawn(template du héros local…)` et placé **à chaque frame** par `MoveTo` ; `Respawn{NetId, pose}` (AD-10, AD-11).

**Création.** L'unité `sync/session` (invité) crée l'avatar `1` à la réception de `Welcome`, qui porte une pose. L'unité `sync/avatar` le crée au premier snapshot contenant un `NetId` inconnu. Une troisième attend `Spawn{1}`. Les trois sont conformes, et deux d'entre elles combinées donnent **deux avatars**. Côté hôte, rien ne dit qui émet `Spawn{2}`, ni si l'invité doit l'attendre avant d'envoyer ses snapshots. Le champ `type` de `Spawn` n'a pas de valeurs définies.

**Placement.** À `Respawn{2, pose}`, l'invité téléporte son héros. Côté hôte, `sync/avatar` continue d'interpoler par Hermite entre la dernière pose avant la mort et la nouvelle : l'avatar **glisse** sur 150 ms à travers la carte. Une story « respawn » qui appelle `MoveTo` directement sur l'avatar est écrasée à la frame suivante par l'interpolation. On a deux propriétaires de la pose. Même chose pour la téléportation de l'invité près de l'hôte à `Welcome` : qui la fait (le jeu de l'invité sur son propre héros ?), et comment l'hôte évite-t-il d'interpoler depuis la position d'origine ?

**Resserrement.**
- AD-10 / AD-8 : « Une entité distante n'est créée **que** sur `Spawn` (avatar compris : l'hôte envoie `Spawn{2}` et `Spawn{1}` juste après `Welcome`). Un snapshot portant un `NetId` inconnu est ignoré. `Spawn.type` ∈ {`Avatar`=0, `Npc`=1}. »
- AD-7 : « Chaque entité du snapshot porte un **numéro d'époque** `u8` (ou un bit “discontinuité”). Quand l'époque change, le récepteur vide le tampon d'interpolation de cette entité et place la nouvelle pose sans interpoler. Une téléportation (respawn, arrivée près de l'hôte) incrémente l'époque. **Seul** `sync/replica` appelle `MoveTo` sur une entité distante. »

---

## M2 — Disposition du snapshot : quatre détails qui cassent l'interopérabilité (AD-4, AD-7)

AD-7 donne « `tick`, puis 22 octets par entité : `NetId` u32, position 3×f32, lacet f32, PV u16 ». Deux implémentations conformes de `core/protocol` divergent sur :

1. **`MsgType` présent ou non.** AD-4 : « structures … avec un `MsgType` sur `u8` ». A écrit `[canal=1][MsgType][tick u32][n×22]`, B écrit `[canal=1][tick u32][n×22]`, puisque le canal suffit. Le décalage d'un octet rend tout illisible.
2. **Nombre d'entités.** Il est implicite (déduit de la longueur) chez A, et un `u8 count` chez B.
3. **Lacet.** Ni le signe, ni l'origine, ni l'unité ne sont fixés. `core/` encode `atan2(m[8], m[10])` en radians ; `game/` reconstruit la matrice avec `atan2(m[10], m[8])` ou en degrés. L'avatar marche de côté, ou en miroir. Le « slerp » d'AD-7 n'a pas de sens sur un seul angle : c'est une interpolation d'angle avec repliement, et elle doit être spécifiée.
4. **Hermite sans vitesse.** Les vitesses ne sont pas transmises (déjà relevé en M4 de la revue rubrique, toujours ouvert) : une story les dérive de deux snapshots, une autre de trois, avec des besoins de tampon différents (150 ms couvrent 3 snapshots, avec zéro marge en cas de perte).

S'y ajoutent : PV absolus ou en pourcentage, et la famine des entités lointaines (« les autres au tick suivant » peut ne jamais arriver si les proches remplissent toujours le datagramme).

**Resserrement (AD-7).** Fixer octet par octet : `[u8 canal=1][u8 MsgType=Snapshot][u32 tick][u8 n][n × {u32 NetId, f32 x,y,z, f32 lacet, u16 pv, u8 époque}]`. Lacet = `atan2(fwd.x, fwd.z)` en radians, `fwd = m[8..10]`, reconstruction avec Y vertical (AD-8, Repères). Vitesse Hermite = différence finie des deux derniers échantillons (sinon : lerp). PV = valeur absolue du jeu. Les entités lointaines tournent : priorité par distance, puis tourniquet.

---

## M3 — `Motion` : charge utile, espace des `AniId` et retards (AD-8, AD-10)

1. **Paramètres.** « `paramètres` » et l'`[ASSUMPTION]` sur `eSMotionPlayBackDesc` (fondu à +0x00, poids à +0x0C/+0x14) laissent chaque story choisir ses champs. `core/protocol` (Linux) fige `{f32 fondu}`. `game/animation` a besoin de `{fondu, poids, vitesse}` pour la locomotion, qui arrive en paires pondérées (types 0 et 1). Soit le message change en cours de J1, soit le descripteur est reconstruit avec des zéros.
2. **Espace des `AniId`.** Les deux pairs émettent des `Motion`. Rien ne dit si l'`AniId` est propre à l'émetteur ou à la session. Une story tient une table globale, une autre une table par direction : l'`AniId` 3 de l'hôte (« Run ») et l'`AniId` 3 de l'invité (« Attack ») se télescopent. La clé d'internement n'est pas fixée non plus (pointeur de ressource d'après le spike, nom d'après la spine) : deux ressources de même titre reçoivent deux identifiants, ce qui est sans danger mais doit être choisi.
3. **`Motion` en retard.** KCP est ordonné, donc une perte bloque toute la file. Un `Motion` peut ainsi arriver au-delà des 150 ms. Story A : il est joué tout de même, et l'avatar enchaîne en accéléré les gestes en retard. Story B : il est jeté, et une course démarrée n'est jamais jouée, donc l'avatar glisse en attente. AD-7 dit « n'attend jamais une retransmission » pour les snapshots seulement.
4. **Échec de `GetMotion(nom)`** (ressource non chargée chez le récepteur) : ignorer, réessayer, ou mettre fin à la session ?

**Resserrement (AD-8).** Fixer la liste fermée des champs de `Motion` (au minimum `type u8, AniId u16, flag u8, fondu f32, poids f32`), après lecture du descripteur dans la story avatar, **avant** de geler `PROTOCOL.md`. « Les `AniId` sont propres à **l'émetteur** et à la session ; la clé est le nom. » « Un `Motion` dont l'heure d'application est dépassée de plus de 150 ms n'est appliqué que s'il est le dernier reçu pour son `type` ; les autres sont jetés. » « Un nom introuvable est journalisé une fois, puis ignoré. »

---

## M4 — AD-8 se dit « validé » sur un schéma qui n'est pas celui qu'il prescrit

AD-8 prescrit `MoveTo` à chaque frame et conclut « Validé en local par le mode miroir de la sonde ». Or `docs/re/spike-j1.md` ne documente que la sonde **v4**, faite avec `SetPose`. Les défauts relevés en v4 (saut raté, équipement qui ne suit pas) motivent `MoveTo`, mais le résultat de la **v5** (commit `d88d2e0`) n'est pas consigné. Une story avatar qui s'appuie sur la mention « validé » découvrira en intégration si `MoveTo` à 200 images/s se bat avec la physique de l'avatar (la copie « tombe au sol, physique active », sonde v3), ou s'il déclenche des `PlayMotion` (voir B2).

**Resserrement.** Consigner les résultats de la v5 dans le spike. Dans AD-8, écrire « Validé : rejeu par nom et blocage (v4) ; placement par `MoveTo` : `[ASSUMPTION]` tant que la v5 n'est pas consignée ».

---

## M5 — Mort : qui émet `Death{2}` ? (AD-9, AD-10, AD-11)

Chez l'hôte, l'avatar de l'invité est invulnérable : l'hôte ne peut voir sa mort qu'à `PV = 0` dans le snapshot. Story A (invité) : le propriétaire émet `Death{2}` quand son héros meurt. Story B (hôte) : `sync/npc` déduit la mort de `PV == 0` et déclenche `Respawn`. Si les deux existent, il y a deux `Respawn` (deux téléportations) ; si aucune, il n'y en a pas. AD-6 dit que l'hôte attribue les `NetId`, mais pas qui fait autorité sur l'événement de mort d'une entité.

**Resserrement (AD-10).** « `Death{NetId}` est émis **uniquement par le pair qui a l'autorité** sur l'entité (AD-7) : l'invité pour `2`, l'hôte pour `1` et les PNJ. Le PV du snapshot sert à l'affichage et ne déclenche rien. » Cela s'ajoute à B3 (empêcher la séquence de mort native).

---

## m1 — Les avatars dans l'énumération des PNJ, et l'avatar hôte chez l'invité (AD-6, AD-9)

- L'avatar invité, chez l'hôte, est une entité « héros » dans un rayon de 50 m. Une story `sync/npc` qui énumère les entités proches peut lui attribuer un `NetId ≥ 1000` et le répliquer : l'invité voit une réplique de lui-même. **Fix :** « une entité présente dans la table `NetId` (avatars compris) n'est jamais énumérée comme PNJ ».
- AD-9 rend invulnérable l'avatar invité chez l'hôte, mais rien n'est prévu pour l'avatar hôte chez l'invité : les PNJ non répliqués (au-delà de 50 m, ou non hostiles) gardent leur IA locale et peuvent l'attaquer. **Fix :** « tout avatar est invulnérable chez le pair qui ne le possède pas », et définir le passage IA locale → réplique quand un PNJ entre dans le rayon (avec hystérésis, voir m1 de la revue rubrique).

## m2 — Liste fermée d'AD-10 déjà incomplète pour J2

La question ouverte 5 (arme dégainée, `gECombatMode`) implique un message d'état de combat absent de la liste. AD-10 prévoit bien d'incrémenter la version, mais deux stories J2 (combat, avatar) vont chacune proposer leur message. **Fix :** réserver dès maintenant `CombatState{NetId, mode u8}` dans AD-10, avec un format fixé par le spike de début de J2.

## m3 — Identité de la sauvegarde absente de `Hello` (AD-5, AD-9)

L'appariement des PNJ par GUID (AD-9) suppose que les deux joueurs ont chargé la même sauvegarde. `Hello` vérifie l'empreinte des DLL, pas celle de la sauvegarde. Si les sauvegardes diffèrent, la session démarre puis désynchronise en silence, exactement ce qu'AD-5 veut empêcher. **Fix :** ajouter un identifiant de sauvegarde (hash du nom de fichier ou d'un en-tête) à `Hello`, avec un `Reject` dédié.

---

## Synthèse des resserrements

| # | Sév. | AD touchés | Resserrement |
| --- | --- | --- | --- |
| B1 | B | AD-7, AD-8, Conv. | `tick` = QPC de l'émetteur / 50 ms depuis `InSession` ; une `RemoteClock` par pair dans `core/` ; `Motion` daté à la ms dans la même base |
| B2 | B | AD-2, AD-8, AD-11 | Objet partagé unique `AnimFilter` ; garde `thread_local` autour de SetMotion + PlayMotion + MoveTo ; jamais de blocage sur le héros ; vidage avant destruction ; vérifier le thread de PlayMotion |
| B3 | B | AD-11 | Signal de génération du monde en plus du pointeur ; tester le retour au menu et le chargement rapide ; pas de pointeur d'entité hors table ; mort native de l'invité = question ouverte J2 |
| B4 | B (J2) | AD-7, AD-8, AD-9 | Rejeu par nom généralisé aux entités non autoritaires ; blocage moteur sur toutes les répliques ; budget de `Motion` PNJ, ou AD dédié avant `sync/npc` |
| B5 | B | AD-1, AD-4, AD-5 | `Hello` de la même adresse → même `Welcome` ; arrêt du renvoi au premier segment KCP |
| M1 | M | AD-7, AD-8, AD-10 | Création uniquement sur `Spawn` (avatars compris) ; époque de téléportation dans le snapshot ; `MoveTo` réservé à `sync/replica` |
| M2 | M | AD-4, AD-7 | Disposition du snapshot à l'octet près, convention du lacet, vitesse Hermite, tourniquet |
| M3 | M | AD-8 | Champs de `Motion` figés ; `AniId` par émetteur ; règle pour les `Motion` en retard ; nom introuvable |
| M4 | M | AD-8 | Consigner la sonde v5 ; `MoveTo` en `[ASSUMPTION]` jusque-là |
| M5 | M | AD-10 | `Death` émis seulement par le pair qui a l'autorité |
| m1–m3 | m | AD-5, AD-6, AD-9, AD-10 | Avatars exclus de l'énumération des PNJ ; invulnérabilité symétrique ; `CombatState` réservé ; identité de la sauvegarde dans `Hello` |
