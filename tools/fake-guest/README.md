# `fake-guest` — faux invité de session (outil de test)

Invité Risen Multiplayer scriptable, écrit en Go, **jamais livré**. Il sert à tester l'hôte
(la DLL, story 1.4) sans second Risen : deux instances du jeu ne tournent pas sur la même
machine. Il suit [`protocol/PROTOCOL.md`](../../protocol/PROTOCOL.md), §8 « Règles de session »
compris.

- `protocol/` : codec v4 écrit à partir de `PROTOCOL.md` (mêmes erreurs que le C++ :
  `ErrTruncated`, `ErrUnknownChannel`, `ErrUnknownType`, `ErrUnsupportedVersion`,
  `ErrBadLength`, `ErrTrailingBytes`, plus `PeekHelloVersion`), vérifié contre
  `protocol/vectors/`.
- `kcp/` : le **même** `ikcp.c` que la DLL, inclus via cgo depuis `client/third_party/kcp/`
  (pas de copie), avec les mêmes réglages : `nodelay(1,10,2,1)`, MTU 1199, segments préfixés
  de l'octet de canal 0.
- `main.go`, `session.go` : ligne de commande et machine à états de l'invité. En session
  (v3, story 2.2), il envoie le `Spawn{2}` de son avatar, placé 2 m devant l'hôte d'après le
  `Welcome` ; il journalise les `Spawn` et `Despawn` reçus (`<- Spawn netid=1 …`) ; à la fin
  par `--quit`, il envoie `Despawn{2}` puis `SessionEnd{1}` (rien avec `--kill`).
  `Motion` n'est pas encore décodé (story 2.4).

Prérequis : Go ≥ 1.22 et un compilateur C (cgo), par exemple `sudo dnf install golang gcc`.
Aucune dépendance externe.

## Build et tests

```bash
cd tools/fake-guest
go build                         # produit ./fake-guest (ignoré par git)
go vet ./... && go test ./...    # vecteurs, KCP avec pertes, matrice de session
```

Les tests jouent la matrice I/O de la story 1.3 contre un **hôte simulé**
(`fakehost_test.go`, paquet de test seulement), qui applique le §8 côté hôte, avec des délais
raccourcis (100 ms au lieu de 500 ms, etc.). Ils sont aussi lancés par `ctest` (test
`fake_guest`, enregistré si `go` est trouvé ; voir `client/README.md`).

## Usage

```bash
./fake-guest --host 192.168.1.20                       # jusqu'à Ctrl-C (qui vaut --quit)
./fake-guest --host 192.168.1.20 --duration 30s        # SessionEnd{1} après 30 s de session
./fake-guest --host 192.168.1.20 --drop 20 --duration 60s --seed 3
./fake-guest --help
```

| Option | Défaut | Effet |
|---|---|---|
| `--host <ipv4>` | — (obligatoire) | adresse de l'hôte |
| `--port` | `27960` | port UDP de l'hôte |
| `--nickname` | `Invite` | pseudo du `Hello` (1 à 32 octets) |
| `--mod-version` | `0.1.0` | version du mod du `Hello` (comparée octet par octet par l'hôte) |
| `--duration` | `0` | durée de la session, comptée à partir de `InSession` ; `0` = jusqu'à Ctrl-C |

Empreintes du `Hello` : celles de `hello.bin` (Risen Steam x64, janvier 2023). Équipement : vide.

### Fautes

| Option | Effet |
|---|---|
| `--drop <0..100>` | jette ce pourcentage des datagrammes **sortants** (contrôle, KCP et snapshots), graine `--seed` (défaut 1) |
| `--delay <durée>` | retarde chaque datagramme sortant (ex. `100ms`) : latence simulée sans `tc netem` (qui demande root) |
| `--jitter <durée>` | ajoute à `--delay` une gigue uniforme ± cette durée (ex. `20ms`) ; l'ordre des datagrammes peut changer |
| `--quit` | à la fin de `--duration`, envoie `SessionEnd{1}` puis sort (comportement par défaut) |
| `--kill` | à la fin de `--duration`, sort **sans rien envoyer** : l'hôte doit constater le timeout 5 s plus tard |
| `--dup-hello` | chaque `Hello` part deux fois (l'hôte doit renvoyer le même `Welcome`), et un `Hello` supplémentaire part en session, une fois le premier heartbeat échangé et acquitté (l'hôte doit l'ignorer) |
| `--bad-version` | envoie le `Hello` de `hello_future.bin` (version 0xFFFF) : l'hôte doit répondre `Reject{1}` |

`--quit` et `--kill` sont exclusifs et demandent `--duration` > 0.

### Trajectoire (`--scenario`, story 2.3)

En session, le faux invité envoie à 20 Hz (canal 1) le snapshot de son avatar : la pose de son
`Spawn` (2 m devant l'hôte), ou celle d'un scénario. `--scenario <fichier>` : une directive par
ligne, jouées dans l'ordre puis en boucle ; chacune part de la pose du `Spawn` et y revient.

| Directive | Effet |
|---|---|
| `cercle <rayon cm> <durée d'un tour s>` | tour complet en tournant vers la droite |
| `aller-retour <longueur cm> <vitesse cm/s>` | tout droit, **demi-tour sec**, retour |
| `pause <durée s>` | immobile |
| `# …` | commentaire (ligne entière) |

Exemples dans `scenarios/` : `cercle.txt`, `aller-retour.txt`, `mixte.txt` (tout à la fois, à deux
vitesses). La ligne de bilan donne chaque seconde les snapshots envoyés et reçus de l'hôte.

### Comportement (PROTOCOL.md §8)

`Hello` toutes les 500 ms jusqu'à `Welcome` ou `Reject` ; abandon après 10 s. Le KCP est
créé au `Welcome` ; `Heartbeat` à 1 Hz ; 5 s sans datagramme de l'hôte → timeout. Après un
`SessionEnd` envoyé, le KCP est gardé au plus 1 s pour laisser partir le segment et recevoir
son acquittement. Un `Welcome` reçu en session, ou portant `conv=0`, est journalisé et ignoré.

### Journal (stdout)

Une ligne horodatée par transition et par message, sauf les heartbeats, résumés une fois
par seconde :

```
2026-10-17 00:27:45.251 Connecting host=127.0.0.1:27960 pseudo="Invite" mod="0.1.0" drop=0%
2026-10-17 00:27:45.251 -> Hello version_protocole=2 mod="0.1.0" pseudo="Invite" équipement="" empreintes=63c9aff6/01417000,…
2026-10-17 00:27:45.251 <- Welcome conv=0xfe24b1fd pos=(1, 2, 3) lacet=0.5
2026-10-17 00:27:45.251 InSession conv=0xfe24b1fd
2026-10-17 00:27:46.251 Heartbeat envoyés=1 reçus=1 (total 1/1)
2026-10-17 00:27:46.556 -> SessionEnd raison=1 (départ du joueur)
2026-10-17 00:27:46.556 Ended raison=départ du joueur (Hello envoyés=1, heartbeats envoyés=2 reçus=2, jetés=0) sortie=0
```

### Codes de sortie

| Code | Cas |
|---|---|
| 0 | fin normale : `--quit`, `--kill`, Ctrl-C, ou `SessionEnd` reçu de l'hôte |
| 1 | erreur d'usage ou de socket |
| 2 | `Reject` reçu (raison journalisée) |
| 3 | hôte injoignable (aucune réponse en 10 s) |
| 4 | timeout (5 s sans datagramme de l'hôte en session) |
