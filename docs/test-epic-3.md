# Test en jeu de l’epic 3 « Combattre ensemble » (stories 3.4 à 3.7)

Document à dérouler en une ou deux sessions. **Léo** est l’hôte (Fedora, Proton), **Stone** est
l’invité (Windows). La partie A se fait seul, la partie B à deux. Chaque étape dit quoi faire,
quoi voir à l’écran et quelles lignes chercher dans le log. Le détail technique est dans les specs
(`_bmad-output/implementation-artifacts/spec-3-*.md`) ; les liens sont donnés à chaque partie.

Toutes les lignes de log citées entre guillemets inversés existent telles quelles dans le code
(`client/src/` pour la DLL, `tools/fake-guest/` pour le faux invité). `…` remplace une valeur
variable (NetId, PV, position). Dans les lignes de la DLL, `N` est le NetId d’un PNJ (1000 ou plus).

Contexte : au premier test à deux machines (2026-10-23), les animations des PNJ (3.4) ont marché
chez l’invité. La session s’est coupée par timeout pendant la cinématique de capture par
l’Inquisition : c’est corrigé par la 3.4a. La mort du héros de l’hôte termine la session : c’est
voulu.

---

## 1. Préparation

### 1.1 Compiler (Léo, une fois)

Depuis la racine du dépôt, sur la branche `auto/epic-3` :

```bash
git switch auto/epic-3
cmake --build client/build && cmake --build client/build-tests && ctest --test-dir client/build-tests
(cd tools/fake-guest && go build)
sha256sum client/build/Script_Multiplayer.dll
```

Attendu : `ctest` passe à 6/6. Noter l’empreinte `sha256` de la DLL.

### 1.2 Installer la DLL sur les deux machines

- **Léo** :
  ```bash
  cp client/build/Script_Multiplayer.dll ~/.local/share/Steam/steamapps/common/Risen/bin/scripts/
  ```
- **Stone** : envoyer **le même fichier** `Script_Multiplayer.dll` à Stone. Il le copie dans
  `D:\SteamLibrary\steamapps\common\Risen\bin\scripts\` (ou le dossier `bin\scripts\`
  de son install Steam). Il vérifie l’empreinte :
  ```powershell
  Get-FileHash "D:\SteamLibrary\steamapps\common\Risen\bin\scripts\Script_Multiplayer.dll"
  ```
  Le résultat doit être identique à celui de Léo (même valeur, en majuscules).

**Les deux DLL doivent être identiques.** La version du mod est toujours `0.1.0`, donc deux DLL
différentes mais de même protocole ne sont pas refusées : elles se comprendraient mal sans
prévenir. Protocole attendu : **v14**. Si les versions diffèrent, le log de Léo affiche
`[sync] Hello reçu de … : version du protocole … (attendue 14)` et Stone est refusé.

### 1.3 `multiplayer.ini`

Le fichier est à côté de la DLL, dans `bin/scripts/`. Pas de commentaire en fin de ligne (il
ferait partie de la valeur).

**Léo (hôte)** :

```ini
role = host
port = 27960
nickname = Leo
guest_equipment =
probe = 0
damage_hook = 1
log_level = debug
```

**Stone (invité)** : remplacer `IP_DE_LEO` par l’IPv4 de Léo (adresse publique si Stone vient
d’Internet, adresse locale `192.168.…` en réseau local ; jamais un nom).

```ini
role = guest
host_ip = IP_DE_LEO
port = 27960
nickname = Stone
guest_equipment = It_1H_Sword
probe = 0
damage_hook = 1
log_level = debug
```

`guest_equipment = It_1H_Sword` garantit une épée à Stone (créée seulement si elle manque). Les
coups de Stone ne comptent que s’il tient une arme : à mains nues, l’hôte les refuse (voir F1).
Laisser vide si Stone a déjà une arme de mêlée.

### 1.4 Réseau (Léo)

- Ouvrir le port UDP sur Fedora, pour la session en cours :
  `sudo firewall-cmd --add-port=27960/udp`.
- Si Stone vient d’Internet : rediriger le port **UDP** 27960 de la box vers la machine de Léo.
- Stone n’a rien à ouvrir.

### 1.5 Steam et sauvegarde

- **Overlay Steam désactivé** chez Léo (Risen → Propriétés → Général → « Activer l’overlay Steam
  en jeu » décoché). Sous Proton, l’overlay fige le jeu. Conseillé aussi chez Stone.
- **Même sauvegarde des deux côtés.** Léo prépare une sauvegarde en zone neutre : hors trame
  principale, loin des templiers et de l’Inquisition, près d’un camp de monstres faciles (loups,
  par exemple), avec quelques PV de marge. Il l’envoie à Stone, qui la copie dans son dossier de
  sauvegardes de Risen :
  - Léo (Proton) : `~/.local/share/Steam/steamapps/compatdata/40300/pfx/drive_c/users/steamuser/AppData/Local/Risen/SaveGames/` ;
  - Stone (Windows) : `%LOCALAPPDATA%\Risen\SaveGames\`.
  Le fichier s’appelle `Risen_Save_XXXX.save` : Stone le copie sous un numéro libre.
- Garder aussi une **sauvegarde rapide** de ce point : elle sert à revenir en arrière.

### 1.6 Où sont les logs

- DLL : `bin/scripts/Script_Multiplayer.log`.
  - Léo : `~/.local/share/Steam/steamapps/common/Risen/bin/scripts/Script_Multiplayer.log`
  - Stone : `D:\SteamLibrary\steamapps\common\Risen\bin\scripts\Script_Multiplayer.log`
- **Le log est réécrit à chaque lancement de Risen.** Le copier avant de relancer le jeu.
- Faux invité : il écrit dans le terminal. Le garder avec `tee` (commandes de la partie A).
- En `log_level = debug`, le bilan `[sync] snapshots (5 s) : …` est écrit toutes les 5 s. Ses
  compteurs sont décrits au §8 ; les derniers ajoutés sont en fin de ligne :
  `… ; morts de PNJ : envoyées …, appliquées …, ignorées … ; coups : envoyés …, appliqués …,
  ignorés …, bloqués … ; coups subis : HitOnGuest envoyés …, reçus …, ignorés … ; à terre : Death …,
  Respawn envoyés …, reçus …, accusés …, ignorés …, snapshots périmés …`.

Préparer un dossier pour les copies (Léo) :

```bash
mkdir -p ~/risen-tests
L=~/.local/share/Steam/steamapps/common/Risen/bin/scripts/Script_Multiplayer.log
FG=~/Documents/project/risen-multiplayer/tools/fake-guest/fake-guest
```

---

## 2. Partie A — une machine (Léo seul : Risen en hôte + faux invité)

Durée : environ 1 h. Un seul lancement de Risen suffit. Les tests sont regroupés par commande du
faux invité. Lancer chaque commande dans un terminal **après** avoir chargé la partie. `Ctrl-C`
dans le terminal termine proprement le faux invité (comme `--quit`).

Rappels :
- sans `--echo`, l’avatar reste en T-pose : c’est normal ;
- avec `--echo`, l’avatar copie l’arme de Léo : **garder l’arme sortie** quand le faux invité
  frappe (session 2), sinon l’hôte refuse ses coups (`avatar sans arme en main`).

Specs : [3.4](../_bmad-output/implementation-artifacts/spec-3-4-animations-pnj.md),
[3.4a](../_bmad-output/implementation-artifacts/spec-3-4a-robustesse-session.md),
[3.4b](../_bmad-output/implementation-artifacts/spec-3-4b-mort-repliques-pnj.md),
[3.5](../_bmad-output/implementation-artifacts/spec-3-5-degats-invite.md),
[3.6](../_bmad-output/implementation-artifacts/spec-3-6-degats-subis-invite.md),
[3.7](../_bmad-output/implementation-artifacts/spec-3-7-a-terre-reapparition.md).

### Session 0 — chargement (sans faux invité)

**A1. Installation.** Lancer Risen, charger la sauvegarde de test.
- Attendu dans le log, dans cet ordre (à peu près) :
  - `config OK (rôle=hôte, port=27960, pseudo=Leo, log_level=debug)`
  - `[hooks] PNJ de session résolus (répliques)`
  - `[hooks] hooks de session installés (OnProcess, PlayMotion, SetRagDollEnabled, locomotion EMotionFX, gCScriptRoutine_PS::OnProcess)`
  - `[hooks] point d'accroche des dégâts posé (gCDamage_PS::DoDamage)`
  - `[hooks] détours DownOrDeadBy* de session posés (mise à terre de l'invité)`
  - `[sync] Inactive → Hosting : monde chargé, en attente d'un invité sur le port 27960`
- Échec : une ligne `[hooks] dégâts inactifs : …`, `[hooks] DownOrDeadBy* inactifs : …`,
  `session inactive`, ou un crash au chargement.
- Si crash au chargement : mettre `damage_hook = 0`, relancer, et le signaler.
- M’envoyer : `grep -E '\[hooks\]|ERROR|WARN|config' "$L"`.

### Session 1 — écho simple (`--echo`) : animations, mort d’un monstre, coups natifs, fin de session

```bash
$FG --host 127.0.0.1 --duration 600s --echo | tee ~/risen-tests/A-s1-fg.log
```

**A2. Connexion.**
- Hôte : `[sync] Hosting → InSession : invité 127.0.0.1:… « Invite » accepté, conv=0x…, pose envoyée …`.
- Faux invité : `InSession conv=0x…`.
- Écran : l’avatar apparaît 2 m devant Léo et l’imite.
- Échec : pas de `InSession`, ou un `Reject`. M’envoyer les 30 dernières lignes des deux logs.

**A3. Animations des PNJ (3.4).** Marcher vers le camp de monstres. Laisser les monstres
approcher et attaquer.
- Faux invité : `<- Spawn PNJ netid=1000 guid=…`, puis `<- Motion PNJ netid=… premier geste ani=… nom=…`
  pour chaque PNJ animé. Dans la ligne par seconde (`Heartbeat envoyés=… ; … animés=N`), N vaut 8
  au plus (9 possible pendant une relève).
- Hôte : `[sync] PNJ N annoncé (Spawn) : GUID …, … m, PV …`. Dans le bilan
  `[sync] snapshots (5 s)`, la partie `animations des PNJ : animés N, Motion envoyés …, …, … o/s`
  a N ≤ 8 et un débit sous 32768 o/s.
- Échec : `[sync] animations des PNJ : … o/s envoyés, au-delà du budget de 32768 o/s`, ou
  `<- Motion PNJ netid=… : ignoré (NetId non annoncé ou retiré)` chez le faux invité, ou
  `[sync] captures perdues (anneau plein) : …` souvent répété.
- M’envoyer : `grep -E 'animations des PNJ|captures perdues|capture échouée|PNJ animé perdu' "$L"`
  et `grep -E 'animés=|ignoré' ~/risen-tests/A-s1-fg.log | tail -n 20`.

**A4. Coups du héros de Léo, et coups des monstres sur Léo (3.5, 3.6).** Frapper un monstre sans
le tuer. Se laisser frapper un peu.
- Écran : dégâts normaux dans les deux sens.
- Hôte : **aucune** ligne `[sync] coup natif bloqué : …` pour ces coups.
- Faux invité : `PNJ netid=N PV a -> b` quand le monstre perd des PV.
- Échec : une ligne `coup natif bloqué` avec `héros` comme agresseur ou comme cible (le mod
  bloquerait les coups de l’hôte).
- M’envoyer : `grep -E 'coup natif bloqué|dégâts :' "$L" | tail -n 30`.

**A5. Mort d’un monstre (3.4b).** Tuer ce monstre avec le héros de Léo.
- Hôte : `[sync] PNJ N mort (Death)` ; **pas** de `[sync] PNJ N retiré (Despawn) : …` pour ce N
  au même moment. Au bilan suivant : `morts de PNJ : envoyées 1, …`.
- Faux invité : `<- Death PNJ netid=N (poses reçues=…, PV=…)`.
- Échec : `[sync] PNJ N retiré (Despawn) : absent de la liste depuis 2 s` au lieu du `Death` (le
  cadavre quitte la liste du moteur : hypothèse H-3.4b-1 du §7).
- M’envoyer : `grep -E 'PNJ [0-9]+ (mort|retiré|annoncé)' "$L"` et
  `grep -E 'Death PNJ' ~/risen-tests/A-s1-fg.log`.

**A6. Pas de réannonce du mort (3.4b).** Noter le GUID du monstre mort (dans sa ligne
`annoncé (Spawn) : GUID …`). S’éloigner à plus de 50 m, attendre 10 s, revenir.
- Hôte : aucun nouveau `annoncé (Spawn) : GUID …` avec ce GUID.
- Échec : le GUID est annoncé de nouveau.
- M’envoyer : même `grep` qu’en A5.

**A7. L’avatar attaque (3.5, hypothèse D5).** Près d’un monstre vivant, attaquer dans le vide pour
que l’avatar, qui copie le geste, touche le monstre.
- Noter si l’hôte écrit `[sync] coup natif bloqué : avatar invité -> … (seuls les HitRequest blessent)`.
- Les deux résultats sont acceptables : la ligne prouve que l’avatar déclenche des coups natifs
  (bloqués) ; son absence prouve qu’il n’en déclenche pas. Le monstre ne doit pas perdre de PV
  par l’avatar.
- M’envoyer : `grep -E 'avatar invité ->' "$L" | head`.

**A8. Tir ami (3.6).** Frapper l’avatar avec le héros de Léo.
- Hôte : `[sync] coup natif bloqué : héros -> avatar invité (pas de tir ami)`.
- Faux invité : aucun `<- HitOnGuest`.
- Écran : l’avatar ne perd rien, ne tombe pas.
- M’envoyer : `grep -E 'avatar invité|HitOnGuest' "$L" | tail -n 20`.

**A9. Humain à terre (3.4b), si un humain hostile ou un adversaire d’arène est proche.** Le mettre
à terre sans le tuer (mains nues ou arme contondante).
- Hôte : pas de `[sync] PNJ N mort (Death)` pour lui. Faux invité : pas de `<- Death PNJ` pour lui.
- Sauter cette étape s’il n’y a pas d’humain à frapper sans conséquence.

**A10. Fin de session par le faux invité (3.4a).** Attendre la fin des 600 s, ou `Ctrl-C`.
- Hôte : `[sync] InSession → Ended : fin de session par le pair : départ du joueur (1) ; retour au solo`.
  Pour chaque PNJ animé : **une seule** ligne `[sync] PNJ N : plus animé` et une ligne
  `[sync] PNJ N : retiré du registre des captures`.
- Hôte, juste après : retour en `Hosting` (réarmement immédiat).
- Échec : deux lignes `plus animé` pour le même N.
- M’envoyer : `grep -E 'plus animé|registre des captures|→' "$L" | tail -n 40`.

### Session 2 — le faux invité frappe (`--hit-interval`) : dégâts dans les deux sens

Se placer près du camp, **arme sortie**, puis :

```bash
$FG --host 127.0.0.1 --duration 300s --echo --hit-interval 2s | tee ~/risen-tests/A-s2-fg.log
```

Le faux invité vise le premier PNJ vivant (plus petit NetId). Si c’est un humain qu’il ne faut pas
fâcher : `Ctrl-C`, lire le NetId d’un monstre dans `annoncé (Spawn)`, et relancer avec
`--hit-target <NetId>` en plus.

**A11. Coups de l’invité appliqués (3.5).**
- Hôte, une fois : `[sync] dégâts : 1er passage au point d'accroche (thread T, thread OnProcess T0)`
  avec **T = T0**.
- Hôte, à chaque coup : `[sync] HitRequest PNJ N appliqué : DoDamage -> 1, PV a -> b` avec b < a.
- Faux invité : `-> HitRequest PNJ netid=N`, puis `PNJ netid=N PV a -> b`.
- Écran : la jauge du monstre baisse ; il se tourne vers l’avatar.
- Échec : `DoDamage -> 0`, PV inchangés, T ≠ T0, ou un Warn `[sync] HitRequest PNJ N ignoré : …`
  (autre que `PNJ mort`).
- M’envoyer : `grep -E 'HitRequest|1er passage' "$L" | head -n 40`.

**A12. Le monstre frappe l’avatar (3.6).** Laisser le monstre attaquer l’avatar.
- Hôte : `[sync] coup natif bloqué : PNJ N -> avatar invité (HitOnGuest, dégâts bruts D)`, puis
  `[sync] coup sur l'avatar invité (source PNJ N) : HitOnGuest envoyé, dégâts bruts D`.
- Faux invité : `<- HitOnGuest source=N dégâts=D`, une ligne par coup.
- Écran : l’avatar ne perd pas de PV, ne tombe pas, ne meurt pas.
- Noter D (dégâts bruts) pour A13.
- Échec : source `inconnue` alors qu’un monstre animé frappe (hypothèse H3 de la 3.6), D = 0, ou
  `dégâts bruts illisibles`.
- M’envoyer : `grep -E 'avatar invité|HitOnGuest|dégâts :' "$L" | tail -n 40`.

**A13. Comparer les dégâts (3.6, hypothèse H2).** Se laisser frapper par le même type de monstre
avec le héros de Léo. Noter la perte de PV sur la jauge. La comparer à D.
- Pas d’échec ici : noter les deux valeurs dans la grille (D, perte native).

**A14. Arme rangée (3.5, D8).** Ranger l’arme 5 s (l’avatar la range aussi), puis la ressortir.
- Hôte, pendant ces 5 s : `[sync] HitRequest PNJ N ignoré : avatar sans arme en main`.
- Après : de nouveau `appliqué : DoDamage -> 1`.

**A15. Mort par les coups du faux invité (3.5 + 3.4b).** Laisser le faux invité finir le monstre.
- Hôte : `[sync] PNJ N mort (Death)`. S’il reste un coup en vol :
  `[sync] HitRequest PNJ N ignoré : PNJ mort`. Le faux invité passe au PNJ suivant.
- Échec : un `HitRequest PNJ N appliqué : DoDamage -> …` **après** le `mort (Death)` du même N
  (coup sur un cadavre), ou un second gain d’XP.
- Bilan de fin chez le faux invité : `Bilan coups : HitRequest envoyés=… HitOnGuest reçus=…`.
- `Ctrl-C` pour finir la session 2.

### Session 3 — NetId inconnu (`--hit-target 1500`)

```bash
$FG --host 127.0.0.1 --duration 20s --hit-target 1500 --hit-interval 1s | tee ~/risen-tests/A-s3-fg.log
```

**A16. Refus d’un NetId inconnu (3.5).**
- Hôte : `[sync] HitRequest PNJ 1500 ignoré : NetId inconnu` (WARN), une fois par seconde.
- Aucun crash ; la session finit seule après 20 s.

### Session 4 — l’invité « meurt » toutes les 10 s (`--die-every`)

```bash
$FG --host 127.0.0.1 --duration 120s --echo --die-every 10s | tee ~/risen-tests/A-s4-fg.log
```

**A17. À terre et réapparition (3.7).** Entre deux morts, marcher et tourner sur place pour que le
saut de l’avatar soit visible. Laisser passer au moins 3 morts.
- Hôte, à chaque mort :
  - `[sync] Death reçu : invité à terre, Respawn à envoyer`
  - `[sync] Respawn envoyé : invité replacé (…) lacet=…`
  - `[sync] réapparition confirmée par l'invité (tick T)`
- Faux invité : `-> Death netid=2 (à terre, --die-every)`, `<- Respawn netid=2 pose=(…)`,
  `-> Respawn accusé tick=T`.
- Écran : l’avatar ne passe **jamais** en ragdoll ; il saute 2 m devant Léo, tourné vers lui, puis
  reprend l’imitation **sans repasser** par son ancienne position.
- Échec : ragdoll, avatar qui revient un instant à l’ancienne place (hypothèse H3 de la 3.7), ou
  `[sync] pas d'accusé de Respawn depuis 5 s : snapshots de l'invité repris`.
- M’envoyer : `grep -E 'Death reçu|Respawn|réapparition|accusé' "$L"` et
  `grep -E 'Respawn|Death' ~/risen-tests/A-s4-fg.log`.

**A18. Bilans de la session 4.**
- Hôte, bilan `[sync] snapshots (5 s)` : `à terre : Death 1, Respawn envoyés 1, reçus 0, accusés 1, …`
  dans les bilans qui contiennent une mort.
- Faux invité, à la fin : `Bilan à terre : Death envoyés=N Respawn reçus=N accusés=N` (N ≈ 11 sur
  120 s ; une mort en vol à la fin peut manquer).

### Session 5 — pause du jeu et chargement rapide (`--strict-timeout`)

Se placer près du camp (des PNJ animés doivent exister), puis :

```bash
$FG --host 127.0.0.1 --duration 180s --strict-timeout --echo | tee ~/risen-tests/A-s5-fg.log
```

**A19. Menu Échap (3.4a).** Ouvrir le menu Échap 15 s, puis revenir au jeu.
- Faux invité : pas de `Ended raison=timeout`. La session continue.
- Hôte : si le menu a figé le jeu plus de 1,5 s, `[sync] jeu en pause : N heartbeat(s) envoyé(s) par le thread réseau`.
  Son absence est normale si le menu ne fige pas la boucle du jeu.

**A20. Chargement rapide (3.4a).** Faire une sauvegarde rapide, puis un chargement rapide.
- Hôte :
  - `[sync] PNJ : N PNJ animé(s) retiré(s) : fin de monde`
  - `[sync] SessionEnd envoyé : fin de monde (2)`
  - `[sync] InSession → Ended : fin de monde ; retour au solo`
  - **aucune** ligne `réplique perdue` ni `PNJ animé perdu` ;
  - éventuellement `[sync] jeu en pause : N heartbeat(s) envoyé(s) par le thread réseau`.
- Faux invité : `<- SessionEnd raison=2 (fin de monde)`, puis il sort. Pas de `Ended raison=timeout`.
- Après le chargement : `[sync] Inactive → Hosting : …` de nouveau.
- Échec : `timeout` chez le faux invité, crash au chargement, ou `perdu` au log.
- M’envoyer : `grep -E 'fin de monde|jeu en pause|perdu|→|timeout' "$L" | tail -n 30` et
  `tail -n 15 ~/risen-tests/A-s5-fg.log`.

### Session 6 — mort de Léo

```bash
$FG --host 127.0.0.1 --duration 300s --echo | tee ~/risen-tests/A-s6-fg.log
```

**A21. Mort du héros de l’hôte (3.7, inchangé).** Se laisser tuer par les monstres (ou une chute).
- Hôte : `[sync] SessionEnd envoyé : mort de l'hôte (3)`, puis
  `[sync] InSession → Ended : mort de l'hôte ; retour au solo`.
- Faux invité : `<- SessionEnd raison=3 (mort de l'hôte)`.
- C’est le comportement voulu : la mort de l’hôte termine la session.

**Fin de la partie A.** Copier le log avant de quitter Risen :
`cp "$L" ~/risen-tests/hote-partieA.log`.

---

## 3. Partie B — deux machines (Léo hôte, Stone invité)

Durée : environ 1 h. Un scénario de jeu dans l’ordre. Chaque étape dit ce que **Léo** et
**Stone** voient. Rester en voix (Discord) pour dire à l’autre quand on passe à l’étape suivante.

**B1. Connexion.** Léo lance Risen et charge la sauvegarde. Puis Stone lance Risen et charge
**la même** sauvegarde.
- Léo, log : `[sync] Inactive → Hosting : …`, puis
  `[sync] Hosting → InSession : invité … « Stone » accepté, conv=0x…, …`.
- Stone, log : `[sync] Inactive → Connecting : monde chargé, connexion à IP_DE_LEO:27960`,
  `[sync] Connecting → InSession : Welcome de …, conv=0x…, pose de l'hôte …`, puis
  `[sync] héros local placé devant l'hôte : …` et `[sync] équipement : héros : …`.
- Écran : Stone est téléporté 2 m devant Léo. Chacun voit l’avatar de l’autre, qui bouge et
  s’anime.
- Échec : `Connecting → Ended : hôte injoignable (aucune réponse en 10 s)` chez Stone (pare-feu,
  redirection, IP), ou `version du protocole` chez Léo (DLL différentes).

**B2. Équipement et armes.** Chacun sort son arme, puis la range.
- Écran : l’avatar de l’autre sort et range l’arme en même temps (moins d’une seconde de retard).
- Stone garde ensuite **son épée sortie** pour la suite.

**B3. Approche d’un camp (3.3, 3.4, 3.4a).** Léo mène Stone vers le camp de monstres.
- Stone, log : `[sync] PNJ N : Spawn reçu (GUID …, …) : réplique`, puis
  `[sync] PNJ N répliqué : « … », entité 0x…, PS 0x…, GUID …, IA et animations figées`.
- Stone, bilan `[sync] snapshots (5 s)` : `entités inconnues` proche de 0, `PNJ reçus avant leur Spawn`
  supérieur à 0 au moins une fois, et dans `animations des PNJ : …` des `rejoués` non nuls.
- Stone, écran : les monstres bougent dès leur apparition, au même endroit que chez Léo. Ils ne
  poursuivent pas Stone d’eux-mêmes : ils suivent ce qui se passe chez Léo.
- Échec : `[sync] PNJ N : GUID … introuvable chez l'invité : ignoré` en nombre (sauvegardes
  différentes ?), ou des monstres qui glissent sans marcher.

**B4. Léo engage le combat (3.4).** Léo frappe un monstre, sans le tuer.
- Stone, écran : le monstre marche, attaque et réagit comme chez Léo ; sa jauge baisse quand
  Léo frappe.
- Léo, écran : rien de nouveau.

**B5. Stone frappe une réplique (3.5).** Stone frappe le même monstre, épée en main.
- Stone, log : `[sync] coup natif bloqué : héros -> PNJ N (HitRequest)`, puis
  `[sync] coup sur PNJ N : HitRequest envoyé`.
- Léo, log : `[sync] HitRequest PNJ N appliqué : DoDamage -> 1, PV a -> b` avec b < a.
- Écran, chez les deux : la jauge du monstre baisse. Chez Stone, elle ne baisse pas toute seule
  au moment du coup : elle suit Léo un instant plus tard. Pas d’effet d’impact chez Stone (voulu).
- Échec : aucun `coup natif bloqué` chez Stone alors qu’il touche (hypothèse D1 de la 3.5).
  Stone m’envoie alors les lignes `[sync] dégâts : agresseur …` (§8).

**B6. Stone est frappé (3.6).** Stone se laisse attaquer par un monstre.
- Léo, log (correctif du 2026-10-23, coup mesuré) : `[sync] coup sur l'avatar invité mesuré (source PNJ N) : DoDamage -> 1, dégâts D (HitOnGuest), PV de l'avatar rendus à P`,
  puis `[sync] coup sur l'avatar invité (source PNJ N) : HitOnGuest envoyé, dégâts bruts D` (D > 0).
- Stone, log : `[sync] HitOnGuest (source PNJ N) appliqué : dégâts bruts D, PV a -> b`. Peut-être
  aussi `[sync] coup natif bloqué : PNJ N -> héros (seuls les HitOnGuest blessent)` (noter s’il
  apparaît : hypothèse H4 de la 3.6).
- Stone, écran : sa jauge baisse à chaque coup (H6). Pas de recul (voulu).
- Léo, écran : l’avatar de Stone ne perd rien et ne tombe jamais.
- Échec : la jauge de Stone ne bouge pas, ou baisse sans ligne `HitOnGuest … appliqué`.

**B7. Stone tombe à 0 PV et réapparaît (3.7).** Stone continue de se faire frapper jusqu’au
coup mortel. **Rester loin des falaises et de l’eau profonde.**
- Stone, log :
  - `[sync] héros à terre (HitOnGuest) : Death envoyé, attente du Respawn`
  - `[sync] Respawn reçu : héros replacé (…) lacet=…, PV pleins ; accusé envoyé (tick T)`
  - `[sync] héros local replacé (réapparition) : …`
  - `[sync] réapparition : PV du héros remis à … ; placé en …`
- Léo, log : `[sync] Death reçu : invité à terre, Respawn à envoyer`,
  `[sync] Respawn envoyé : invité replacé …`, `[sync] réapparition confirmée par l'invité (tick T)`.
- Stone, écran : il réapparaît 2 m devant Léo, jauge pleine. Ni ragdoll, ni écran de mort, ni
  menu de chargement. La session continue.
- Léo, écran : l’avatar de Stone saute devant lui, sans ragdoll, et ne revient pas sur le lieu de
  la mort.
- Échec : écran de mort ou menu de chargement chez Stone ; `[sync] pas de Respawn de l'hôte depuis 10 s : PV rendus sur place`
  chez Stone ; `[sync] pas d'accusé de Respawn depuis 5 s : …` chez Léo ; une ligne `Error` ou
  `Warn` `réapparition` chez Stone (hypothèses H1, H2 de la 3.7).

**B8. Mort d’un monstre (3.4b).** Les deux frappent un monstre jusqu’à sa mort.
- Léo, log : `[sync] PNJ N mort (Death)`.
- Stone, log : `[sync] PNJ N : Death reçu : mort à l'instant d'affichage`, puis **une seule fois**
  `[sync] PNJ N mort : PV 0, AIMode 12, ragdoll`.
- Stone, écran : le monstre tombe en ragdoll et reste au sol. **Aucun** message d’XP ni de quête
  chez Stone (hypothèse D2 de la 3.4b). Léo gagne l’XP normalement.
- Échec : `(en échec : …)` en fin de ligne `mort`, monstre resté debout ou qui se relève, XP chez
  Stone.

**B9. Butin (3.4b).** Chacun fouille le cadavre de son côté.
- Écran : chacun trouve le butin de **son propre** monstre (le butin n’est pas partagé). Fouiller
  ne dérange pas l’autre.

**B10. Pause chez Stone (3.4a).** Stone déclenche une pause longue : une cinématique ou un dialogue
avec transition s’il y en a un à portée ; sinon il reste 20 s dans le menu Échap.
- Stone, log, à la reprise : peut-être `[sync] jeu en pause : N heartbeat(s) envoyé(s) par le thread réseau`.
- Léo, log : **pas** de `timeout (5 s sans nouvelles du pair)`. La session continue (les bilans
  suivants ont des snapshots reçus).
- Échec : timeout chez Léo pendant une pause de moins de 60 s.

**B11. Léo recharge (3.4a, 3.4b).** Léo fait un chargement rapide.
- Léo, log : `[sync] PNJ : N PNJ animé(s) retiré(s) : fin de monde`,
  `[sync] SessionEnd envoyé : fin de monde (2)`, aucune ligne `réplique perdue` ni
  `PNJ animé perdu`, pas de crash. Puis `Inactive → Hosting` après le chargement.
- Stone, log : `[sync] InSession → Ended : fin de session par le pair : fin de monde (2) ; retour au solo`,
  `[sync] PNJ : N réplique(s) libérée(s) : fin de session`, et pour le monstre tué en B8
  `[sync] PNJ N libéré : reste morte`.
- Stone, écran : le cadavre reste au sol, sans son de mort ni XP (hypothèse D4 de la 3.4b). Les
  monstres vivants reprennent leur IA et attaquent Stone (normal : il est en solo).
- Stone recharge à son tour la même sauvegarde rapide de départ pour revenir en session (B1).

**B12. Fin de session par Stone.** Une fois reconnectés, Stone fait un chargement rapide.
- Stone, log : `[sync] SessionEnd envoyé : fin de monde (2)`.
- Léo, log : `[sync] InSession → Ended : fin de session par le pair : fin de monde (2) ; retour au solo`,
  une seule ligne `[sync] PNJ N : plus animé` par PNJ animé, puis retour immédiat en `Hosting`.

**Fin de la partie B.** Copier les logs avant de relancer le jeu (commandes au §8).

---

## 4. Tests facultatifs et limites connues

À faire après la partie B, s’il reste du temps. Plusieurs terminent la session : les garder pour
la fin.

**F1. Mains nues (3.5, D8).** Stone range son arme et frappe à poings nus.
- Léo : `[sync] HitRequest PNJ N ignoré : avatar sans arme en main`. Le monstre ne perd rien.

**F2. Arc ou magie (3.5, 3.6, hors périmètre).** Stone tire à l’arc ou lance un sort sur une
réplique ; puis un monstre à distance (ou un archer) vise Stone.
- Noter la ligne observée chez Stone : `coup natif bloqué : … -> PNJ N (HitRequest)` (ça marche)
  ou `… (autre agresseur, sans HitRequest)` (dégâts à distance perdus, limite connue).
- Chez Léo, pour un projectile sur l’avatar : `HitOnGuest envoyé` avec `source inconnue` est
  attendu.

**F3. Filet `DownOrDeadBy*` (3.7, H4).** Stone quitte Risen, met `damage_hook = 0`, relance, revient
en session, et se fait tuer par un monstre. **Remettre `damage_hook = 1` après.**
- Stone : `[hooks] point d'accroche des dégâts désactivé (damage_hook = 0)` au chargement ; au coup
  mortel `[sync] coup natif mortel intercepté (DownOrDeadBy…) : héros à 1 PV, à terre`, puis la
  réapparition comme en B7.
- Attention : sans le détour, les coups de Stone blessent ses répliques localement (voulu pour ce
  test seulement).

**F4. Chute ou noyade (3.7, limite Q9).** Stone saute d’une falaise (ou se noie). **Termine la
session.**
- Stone : `[sync] mort native du héros (chute, eau profonde ?) : Death envoyé ; réapparition impossible, la session finira au rechargement`,
  puis `[sync] Respawn reçu (NetId 2 (invité), …) : ignoré (héros mort nativement : pas de relève d'un mort)`.
  L’écran de mort natif s’affiche (limite connue).
- Léo : `Death reçu : invité à terre`, `Respawn envoyé`, puis 5 s plus tard
  `[sync] pas d'accusé de Respawn depuis 5 s : snapshots de l'invité repris`. L’avatar saute devant
  Léo puis revient sur le lieu de la chute : c’est la limite connue.
- Stone recharge : Léo voit `fin de session par le pair : fin de monde (2)` et se réarme.
- Échec : crash, ou session terminée autrement (hypothèse H5 de la 3.7).

**F5. Tir ami à deux (3.5, 3.6).** Léo frappe l’avatar de Stone ; Stone frappe l’avatar de Léo.
- Léo : `[sync] coup natif bloqué : héros -> avatar invité (pas de tir ami)`. Stone ne perd rien.
- Stone frappant l’avatar de Léo : rien n’est prévu de ce côté. Noter ce qui se passe (dégâts
  locaux sur l’avatar ? rien chez Léo attendu).

**F6. Humain à terre à deux (3.4b, D5).** Léo met un humain à terre sans le tuer.
- Stone : `[sync] PNJ N : PV = 1` (debug), la réplique ne meurt pas chez Stone.

**F7. Mort de Léo à deux.** Léo se laisse tuer. **Termine la session.**
- Léo : `[sync] SessionEnd envoyé : mort de l'hôte (3)`. Stone : `fin de session par le pair : mort de l'hôte (3)`.

---

## 5. Grille de résultats

Recopier ou cocher dans ce fichier. Remarque : valeurs notées, lignes bizarres, ressenti.

| Étape | Attendu court | OK | KO | Remarque |
|---|---|:-:|:-:|---|
| A1 | hooks posés, `Hosting` | ☐ | ☐ | |
| A2 | faux invité accepté, avatar visible | ☐ | ☐ | |
| A3 | `<- Motion PNJ`, animés ≤ 8, < 32768 o/s | ☐ | ☐ | débit : |
| A4 | coups de Léo non bloqués | ☐ | ☐ | |
| A5 | `PNJ N mort (Death)`, pas de `Despawn` | ☐ | ☐ | |
| A6 | GUID mort jamais réannoncé | ☐ | ☐ | |
| A7 | avatar qui attaque : ligne `avatar invité ->` ? | ☐ | ☐ | présente / absente : |
| A8 | `(pas de tir ami)` | ☐ | ☐ | |
| A9 | humain à terre : pas de `Death` | ☐ | ☐ | sauté ? |
| A10 | une seule ligne `plus animé` par PNJ | ☐ | ☐ | |
| A11 | `DoDamage -> 1`, PV baissent, T = T0 | ☐ | ☐ | |
| A12 | `HitOnGuest envoyé`, avatar intact | ☐ | ☐ | D = |
| A13 | perte native comparée à D | ☐ | ☐ | perte native = |
| A14 | arme rangée : `avatar sans arme en main` | ☐ | ☐ | |
| A15 | mort par HitRequest, pas de coup sur cadavre | ☐ | ☐ | |
| A16 | `NetId inconnu`, pas de crash | ☐ | ☐ | |
| A17 | réapparition sans ragdoll ni retour arrière | ☐ | ☐ | |
| A18 | bilans `à terre` cohérents | ☐ | ☐ | |
| A19 | menu Échap : pas de timeout | ☐ | ☐ | ligne « jeu en pause » ? |
| A20 | chargement : `fin de monde`, rien de `perdu` | ☐ | ☐ | |
| A21 | mort de Léo : fin raison 3 | ☐ | ☐ | |
| B1 | Stone connecté, téléporté devant Léo | ☐ | ☐ | |
| B2 | armes visibles des deux côtés | ☐ | ☐ | |
| B3 | répliques chez Stone, animées dès l’apparition | ☐ | ☐ | |
| B4 | monstre animé et blessé chez Stone | ☐ | ☐ | |
| B5 | coup de Stone : `HitRequest` appliqué | ☐ | ☐ | |
| B6 | Stone blessé par `HitOnGuest` seulement | ☐ | ☐ | ligne `-> héros` vue ? |
| B7 | Stone à terre puis réapparu, PV pleins | ☐ | ☐ | |
| B8 | monstre mort chez les deux, pas d’XP chez Stone | ☐ | ☐ | |
| B9 | butin propre à chacun | ☐ | ☐ | |
| B10 | pause chez Stone sans timeout | ☐ | ☐ | type de pause : |
| B11 | rechargement de Léo : cadavre reste mort | ☐ | ☐ | |
| B12 | départ de Stone propre | ☐ | ☐ | |
| F1 | mains nues refusées | ☐ | ☐ | |
| F2 | arc/magie : ligne observée | ☐ | ☐ | |
| F3 | filet `DownOrDeadBy*` | ☐ | ☐ | |
| F4 | chute : limite, pas de crash | ☐ | ☐ | |
| F5 | tir ami à deux | ☐ | ☐ | |
| F6 | humain à terre : PV = 1 chez Stone | ☐ | ☐ | |
| F7 | mort de Léo à deux | ☐ | ☐ | |

---

## 6. Décisions prises sans toi, à valider

Le détail est dans la section « Design Notes » de chaque spec.

**3.4 — animations des PNJ**
- Un PNJ qui entre dans les 8 animés reste figé jusqu’à son geste suivant (pas d’envoi de son
  animation en cours) : un monstre peut rester immobile une fraction de seconde chez Stone.

**3.4a — robustesse**
- Pendant une pause du jeu, le thread réseau envoie lui-même un heartbeat par seconde : une
  cinématique ne coupe plus la session.
- Ce secours s’arrête après 60 s : un jeu vraiment figé est détecté en 65 s environ, au lieu de 5 s.
- Un snapshot de PNJ arrivé avant son annonce est gardé 500 ms : les monstres bougent dès leur
  apparition.
- Pas de nouvelle version du protocole pour cette story.

**3.4b — mort des monstres**
- Chez Stone, un monstre tué meurt sans `Kill` natif : pas d’XP, pas de quête, pas de script
  pour Stone.
- Chacun pille son propre monstre ; le butin n’est pas partagé.
- Le cadavre reste mort chez Stone après la session, y compris dans sa sauvegarde.
- Un humain à terre chez Léo garde 1 PV chez Stone (jamais 0), pour éviter une mort locale.
- Un monstre mort de la session n’est plus jamais répliqué, même s’il ressuscite.
- Un monstre qui meurt hors du rayon de 50 m reste vivant (avec son IA) chez Stone.

**3.5 — coups de Stone**
- Les coups de Stone sur un monstre sont recalculés chez Léo avec l’arme et les compétences de
  l’avatar, pas celles du héros de Stone : la force peut différer.
- À mains nues ou arme rangée, les coups de Stone ne comptent pas.
- Pas d’effet d’impact ni de recul du monstre chez Stone au moment du coup.
- Aucune limite de cadence ni de distance (coop de confiance).
- `damage_hook = 0` coupe tout le dispositif : interrupteur de diagnostic seulement.

**3.6 — coups subis par Stone**
- Dégâts reçus par Stone = dégâts bruts de l’arme du monstre, **sans armure** : Stone peut prendre
  plus (ou moins) que Léo face au même monstre.
- Tout coup natif sur le héros de Stone est bloqué : un ennemi que Léo ne voit pas ne peut plus
  blesser Stone.
- Pas de tir ami : Léo ne blesse pas l’avatar de Stone, et l’avatar ne blesse pas Léo.
- Pas de recul chez Stone quand il est touché.

**3.7 — à terre et réapparition**
- « À terre » ne dure qu’un aller-retour réseau : Stone réapparaît aussitôt devant Léo, PV
  pleins, sans animation au sol.
- L’avatar de Stone n’est plus mis en ragdoll chez Léo (il est juste déplacé).
- Point de réapparition : 2 m devant Léo, tourné vers lui.
- Chute et noyade ne sont pas gérées : écran de mort natif chez Stone, la session finit à son
  rechargement.
- Replis : sans réponse de Léo en 10 s, Stone récupère ses PV sur place ; sans accusé de Stone en
  5 s, Léo reprend ses positions.

---

## 7. Hypothèses que ce test va trancher

| Hypothèse (story) | Étape | Ligne à regarder |
|---|---|---|
| H-3.4b-1 : le cadavre reste dans la liste du moteur, donc `Death` et pas `Despawn` (3.4b) | A5, B8 | `PNJ N mort (Death)` contre `retiré (Despawn) : absent de la liste depuis 2 s` |
| D2 : `AIMode = 12` sans `Kill` ne donne ni XP ni script (3.4b) | B8 | écran de Stone ; `PNJ N mort : PV 0, AIMode 12, ragdoll` sans `(en échec : …)` |
| D4 : le cadavre libéré ne se relève pas (3.4b) | B11 | `PNJ N libéré : reste morte` ; écran de Stone |
| D5 : un humain à terre garde 1 PV chez l’invité (3.4b) | F6 | `[sync] PNJ N : PV = 1` |
| Anneau de captures assez grand (3.4) | A3 | `captures perdues (anneau plein)` rare ou absente |
| Budget des animations tenu (3.4) | A3 | `animations des PNJ : … o/s` sous 32768 |
| D1 : l’agresseur lu est bien le personnage (3.5) | A11, B5 | `coup natif bloqué : héros -> PNJ N (HitRequest)` ; sinon `dégâts : agresseur …` |
| Le détour tourne sur le thread du jeu (3.5) | A11 | `1er passage au point d'accroche (thread T, thread OnProcess T0)` avec T = T0 |
| D5 : l’avatar déclenche des coups natifs (3.5) | A7 | `coup natif bloqué : avatar invité -> …` présente ou non |
| Pas de `DoDamage` sur un cadavre (3.5) | A15 | aucun `appliqué : DoDamage` après `mort (Death)` du même N |
| H1 : les dégâts bruts sont lisibles (3.6) | A12 | `HitOnGuest, dégâts bruts D` avec D > 0 ; pas de `dégâts bruts illisibles` |
| H2 : les dégâts bruts sont proches des dégâts natifs (3.6) | A13 | D comparé à la perte native sur la jauge |
| H3 : la source d’un coup sur l’avatar est le PNJ capturé (3.6) | A12, B6 | `source PNJ N` et non `source inconnue` |
| H4 : les répliques déclenchent des coups natifs sur Stone (3.6) | B6 | `coup natif bloqué : PNJ N -> héros (seuls les HitOnGuest blessent)` |
| H6 : `SetHitPoints` met à jour la jauge de Stone (3.6) | B6 | `HitOnGuest (source PNJ N) appliqué : … PV a -> b` et jauge |
| H1 : `MoveTo` replace le héros en combat (3.7) | B7 | `héros local replacé (réapparition) : …` ; écran |
| H2 : `SetHitPoints(max)` remplit la jauge (3.7) | B7 | `réapparition : PV du héros remis à …` ; écran |
| H3 : pas de retour de l’avatar sur l’ancienne position (3.7) | A17, B7 | écran de Léo ; `réapparition confirmée par l'invité (tick T)` |
| H4 : `DownOrDeadBy*` voit le héros de l’invité (3.7) | F3 | `coup natif mortel intercepté (DownOrDeadBy…)` |
| H5 : une mort native passe par `IsDead` avant le chargement (3.7) | F4 | `mort native du héros (chute, eau profonde ?) : …` |
| Pause longue sans timeout (3.4a) | A19, B10 | pas de `timeout (5 s sans nouvelles du pair)` ; `jeu en pause : N heartbeat(s)…` |
| Fin de monde sans pointeur périmé (3.4a) | A20, B11 | `… : fin de monde` sans `réplique perdue` ni `PNJ animé perdu` |

---

## 8. Quoi me renvoyer

Pas le log entier : seulement les lignes utiles, plus le faux invité et la grille.

### Léo (Linux)

```bash
L=~/.local/share/Steam/steamapps/common/Risen/bin/scripts/Script_Multiplayer.log
cp "$L" ~/risen-tests/hote-$(date +%H%M).log        # avant de relancer Risen
cd ~/risen-tests
# Erreurs, avertissements, hooks et transitions
grep -E 'ERROR|WARN|\[hooks\]|→' "$L" > leo-1-erreurs.txt
# Lignes de combat et de PNJ (sans le bilan de 5 s)
grep -E 'PNJ [0-9]+ (mort|retiré|annoncé|libéré)|HitRequest|HitOnGuest|coup natif|coup sur|dégâts :|Death|Respawn|réapparition|à terre|jeu en pause|fin de monde|plus animé|registre des captures' "$L" | grep -v 'snapshots (5 s)' > leo-2-combat.txt
# Les 5 derniers bilans
grep 'snapshots (5 s)' "$L" | tail -n 5 > leo-3-bilans.txt
# Faux invité : bilans et lignes clés
grep -hE 'Bilan|HitOnGuest|Respawn|Death|timeout|SessionEnd|ignoré' A-s*-fg.log > leo-4-faux-invite.txt
wc -l leo-*.txt
```

Envoyer les fichiers `leo-*.txt` et la grille du §5. Si un fichier dépasse 500 lignes, envoyer
seulement la partie autour de l’étape en échec (`grep -n` donne les numéros de ligne).

### Stone (Windows, PowerShell)

```powershell
$log = "D:\SteamLibrary\steamapps\common\Risen\bin\scripts\Script_Multiplayer.log"
Copy-Item $log "$HOME\Desktop\stone-complet.log"     # avant de relancer Risen
# Erreurs, avertissements, hooks et transitions
Select-String -Path $log -Encoding UTF8 -Pattern 'ERROR|WARN|\[hooks\]|→' |
  ForEach-Object Line | Out-File -Encoding utf8 "$HOME\Desktop\stone-1-erreurs.txt"
# Lignes de combat et de PNJ
Select-String -Path $log -Encoding UTF8 -Pattern 'PNJ [0-9]+ (mort|Death|libéré|répliqué)|HitRequest|HitOnGuest|coup natif|coup sur|dégâts :|Respawn|réapparition|à terre|jeu en pause|fin de monde|introuvable' |
  ForEach-Object Line | Where-Object { $_ -notmatch 'snapshots \(5 s\)' } |
  Out-File -Encoding utf8 "$HOME\Desktop\stone-2-combat.txt"
# Les 5 derniers bilans
Select-String -Path $log -Encoding UTF8 -Pattern 'snapshots \(5 s\)' |
  Select-Object -Last 5 | ForEach-Object Line | Out-File -Encoding utf8 "$HOME\Desktop\stone-3-bilans.txt"
# Les 40 dernières lignes (utile après un crash)
Get-Content $log -Encoding UTF8 -Tail 40 | Out-File -Encoding utf8 "$HOME\Desktop\stone-4-fin.txt"
```

Stone envoie les quatre fichiers `stone-*.txt` de son bureau. En cas de crash, `stone-4-fin.txt`
suffit souvent, avec l’heure du crash.
