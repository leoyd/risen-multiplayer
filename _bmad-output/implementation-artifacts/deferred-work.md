# Travail reporté

- source_spec: `_bmad-output/implementation-artifacts/spec-j1-sonde-reverse.md`
  summary: Déplacer la logique pure de la sonde (anneau de captures, limite de débit, formatage hexa/matrice) dans `core/` et la tester sous Linux si la sonde survit au spike.
  evidence: Revue J1 (verification-gap) : cette logique n’est liée à aucun test (`client_tests` ne lie que `mp_core`).
- source_spec: `_bmad-output/implementation-artifacts/spec-j1-sonde-reverse.md`
  summary: Détours MinHook et enveloppes ABI (`game/entity`) vérifiés uniquement en jeu ; chemin de retrait après échec MinHook jamais exercé.
  evidence: Le banc Wine n’appelle ni `OnProcess` ni `PlayMotion` ; aucun scénario ne fait échouer MinHook.
- source_spec: `_bmad-output/implementation-artifacts/spec-j1-sonde-reverse.md`
  summary: Rejeu F10 d’une copie brute de 0x60 octets d’`eSMotionPlayBackDesc` (taille et pointeurs internes inconnus) — non vérifié, medium si avéré.
  evidence: À trancher avec les dumps `PlayMotion` du log de la sonde (taille réelle, champs pointeurs).
- source_spec: `_bmad-output/implementation-artifacts/spec-j1-sonde-reverse.md`
  summary: Possible exception SEH à chaque frame dans les menus si `Entity::GetPlayer` faute hors monde — non vérifié, medium si avéré.
  evidence: Le tick journalisera « erreur (SEH dans GetPlayer) » dans ce cas ; ajouter alors un recul après échec.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-1-contrat-protocole.md`
  summary: [fait 2026-10-17, story 1.4] Tester l'effet de `log_level` (lignes `DEBUG` présentes en debug, absentes en info) dès le premier appel à `mp::log::debug`.
  evidence: Aucun appel à `log::debug` en story 1.1 ; l'assertion Wine ne vérifie que la ligne « config OK ». Ajouter une paire `expect_log`/`expect_no_log "DEBUG"` dans `run_scenarios.sh` (stories 1.2 ou 1.4).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-1-contrat-protocole.md`
  summary: Compléter `PROTOCOL.md` avec les règles de session (Hello toutes les 500 ms, abandon à 10 s, heartbeat 1 Hz, timeout 5 s, même Welcome/conv pour une retransmission, arrêt au premier segment KCP, `conv` non nul, comparaison exacte de la version du mod et des 4 empreintes, réaction de l'invité à un Reject).
  evidence: Ces règles ne vivent que dans la spine et `epic-1-context.md` ; le faux invité Go (story 1.3) doit pouvoir s'appuyer sur `PROTOCOL.md` seul.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-1-contrat-protocole.md`
  summary: Étendre la liste blanche (`SupportedVersion`, `version.cpp`) aux empreintes de Script.dll et SharedBase.dll, pour que l'hôte puisse vérifier les 4 modules du Hello avant `Reject{3}`.
  evidence: AD-3 dit que l'empreinte couvre 4 modules, mais `SupportedVersion` ne connaît que Game et Engine (code J0).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-2-transport-udp-kcp.md`
  summary: Signaler à la frame un lien KCP mort (`kcp->state == -1`) et la taille de la file d'envoi (`ikcp_waitsnd`), avec une limite.
  evidence: `ikcp_send` met en file sans limite ; inoffensif avec le seul heartbeat (story 1.4, timeout de 5 s), à traiter avant les flux `Motion` de l'Epic 2.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-2-transport-udp-kcp.md`
  summary: [fait 2026-10-17, story 1.4] Arrêter explicitement le transport avant DLL_PROCESS_DETACH : ne jamais faire le `join` du thread réseau sous le verrou du chargeur.
  evidence: `~Transport` appelle `stop()` (join) ; un `Transport` global détruit au déchargement de la DLL peut bloquer (story 1.4).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-2-transport-udp-kcp.md`
  summary: Faire échouer (ou marquer ignoré) ctest quand `wine_net` / `wine_scenarios` ne peuvent pas être enregistrés, au lieu d'un simple message STATUS.
  evidence: Configurer `build-tests` avant le build Windows donne 3/3 au lieu de 4/4 sans alerte ; le code Winsock n'est alors pas testé.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-2-transport-udp-kcp.md`
  summary: [fait 2026-10-17] Mettre `CLAUDE.md` à jour : le faux invité Go (`tools/fake-guest/`, outil de test jamais livré) est autorisé par AD-1 amendé.
  evidence: `CLAUDE.md` dit encore « Plus de Go ni de server/ pour le MVP », ce qui contredit la spine et `third_party/kcp/VERSION.txt`.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-3-faux-invite.md`
  summary: Tester les filtres adresse et `conv` du faux invité (datagramme d'un tiers, segment portant un autre `conv`), quand il sera utilisé contre la DLL.
  evidence: Aucun test n'envoie ce trafic à l'invité ; sans le filtre d'adresse, un tiers maintiendrait une session muette au-delà du timeout.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-3-faux-invite.md`
  summary: [fait 2026-10-17 : story 1.4, puis story 1.5 pour la durée du maintien du KCP après SessionEnd (1 s, §8)] Rendre normatif dans `PROTOCOL.md` §8 : Welcome ou Reject reçu en session, durée du maintien du KCP après SessionEnd, Hello invalide venant du pair accepté, `conv` neuf après un timeout.
  evidence: Ces comportements n'existent que dans le code et le README du faux invité ; la story 1.4 doit les fixer pour la DLL.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: Valider à deux machines le côté invité de la DLL (Welcome reçu, session, heartbeats, Reject reçu, timeout) ; une seule instance ne vérifie que « hôte injoignable ».
  evidence: Deux Risen ne tournent pas sur la même machine ; ce côté n'est couvert que par `tests/test_session.cpp` (logique pure).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: [fait 2026-10-17, story 1.5 : maintien de 1 s, normatif au §8] Durée de maintien du KCP après un `SessionEnd` envoyé, fixée dans §8 avec la fin de session volontaire. Vérifié côté hôte seulement (tests Linux et faux invité) ; côté invité de la DLL, non validé (deux machines).
  evidence: En 1.4, `Ended` suit toujours un timeout, un `SessionEnd`/`Reject` reçu ou un hôte injoignable : rien à envoyer.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: Mesurer en jeu si `OnProcess` s'arrête plus de 5 s (écran de chargement, saccade) ; si oui, envoyer le heartbeat depuis le thread réseau ou prévenir le pair.
  evidence: Les heartbeats partent de la frame ; une pause de plus de 5 s ferait conclure le pair à un timeout (non vérifié).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: Vérifier à deux machines, sous pertes, que compter l'activité du pair sur les messages KCP (et non les datagrammes) ne cause pas de faux timeout.
  evidence: Le blocage en tête de file de KCP peut retenir les heartbeats alors que des datagrammes arrivent ; le faux invité, lui, compte les datagrammes.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: Partager l'exécuteur de commandes (`core::session::Output` → `net::Transport`) entre la DLL et `session_host`, pour que les tests sans le jeu couvrent le câblage réseau de la DLL.
  evidence: `sync/session.cpp::execute` est une copie de la boucle de `session_host_main.cpp` ; seul le test en jeu l'exerce.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-5-fin-de-session.md`
  summary: Faire le test en jeu de la story 1.5 (3 chargements rapides puis menu, mort de l'hôte, `log_level = debug`) et recopier les lignes `[sync] signaux` dans `docs/re/fin-de-monde.md`.
  evidence: La règle de fin de monde (`player`, `running`, `dead`) repose sur le désassemblage ; son comportement pendant un chargement rapide n'est pas observé.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-5-fin-de-session.md`
  summary: Si le test en jeu montre que `running` ne retombe pas et que `player` garde son adresse pendant un chargement rapide, ajouter le signal de remplacement `eCEntity::GetID` du héros (story suivante).
  evidence: Sans ce signal, un rechargement d'un monde vivant ne serait pas détecté (seul le repli « dead 1 → 0 » couvre la mort de l'hôte).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-5-fin-de-session.md`
  summary: Valider à deux machines le côté invité de la story 1.5 (SessionEnd{2}/{3} reçu de l'hôte, fin de monde de l'invité avec SessionEnd{2}, attente du monde suivant).
  evidence: Une seule instance de Risen par machine ; ce côté n'est couvert que par `tests/test_session.cpp`.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-5-fin-de-session.md`
  summary: Observer en jeu le retour au menu principal en cours de session (attendu : `player` nul ou `running` faux, donc « fin de monde » et `SessionEnd{2}`).
  evidence: Non fait pendant le test du 2026-10-17 ; couvert seulement par les tests de `core/session`.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-5-fin-de-session.md`
  summary: Marge faible entre la pause d'`OnProcess` pendant un chargement rapide (environ 3,5 s mesurées) et le timeout de 5 s ; à revoir si des timeouts apparaissent à la place de « fin de monde » (heartbeat depuis le thread réseau, ou timeout plus long).
  evidence: `docs/re/fin-de-monde.md`, réponse 4 ; complète l'entrée de la story 1.4 sur la pause d'`OnProcess`.

- source_spec: `_bmad-output/implementation-artifacts/spec-2-1-miroir-par-nom.md`
  summary: Codec Go de `Motion` (et `ErrBadValue`) à écrire en story 2.4, en retirant `motion.bin` et `motion_bad_type.bin` de la liste blanche de `TestAllVectorsCovered`.
  evidence: En 2.1, le Go est passé en v2 sans décoder `Motion` (report prévu par l'intent). La liste blanche fait passer ces vecteurs pour couverts.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-1-miroir-par-nom.md`
  summary: Côté réception de `Motion` (story 2.4), refuser ou neutraliser les flottants non finis (NaN, ±Inf) avant `PlayMotion`.
  evidence: Medium non vérifié. `valid()` accepte NaN et Inf ; l'effet d'un NaN dans le descripteur sur le moteur est inconnu. Un test en jeu avec un descripteur NaN, ou la lecture du code EMotionFX, trancherait.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-1-miroir-par-nom.md`
  summary: La sonde garde le PS de la copie comme réplique si le moteur détruit la copie sans changement de joueur.
  evidence: Medium non vérifié, existait déjà en v5. À trancher en jeu : une copie temporaire peut-elle être détruite hors chargement ? L'avatar (2.2) devra de toute façon le détecter.

- source_spec: `_bmad-output/implementation-artifacts/spec-2-1b-ragdoll-miroir.md`
  summary: Couvrir par des tests Linux le routage des captures par nature (`Motion` ou ragdoll) quand l'avatar le reprendra (événement `Death`, AD-10).
  evidence: Dans la sonde (`probe.cpp`, réservée à Windows), ce routage n'est vérifié que par le test en jeu. Inverser les branches passerait tous les tests automatiques.

- source_spec: `_bmad-output/implementation-artifacts/spec-2-2-apparition-retrait-avatars.md`
  summary: Lier l'avatar à un identifiant de monde plus sûr que l'adresse du héros (par exemple `eCEntity::GetID`, ou une génération tenue par la session), pour ne jamais appeler `Entity::Kill` sur un avatar détruit avec un monde rechargé à la même adresse, ni garder son PS dans l'`AnimFilter`.
  evidence: maybe-false, medium si vrai. `docs/re/fin-de-monde.md` : l'adresse a changé à chaque rechargement observé, mais rien ne garantit qu'elle ne sera jamais réutilisée. Même cause que l'entrée « signal de remplacement `eCEntity::GetID` » de la story 1.5.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-2-apparition-retrait-avatars.md`
  summary: Détecter un avatar détruit par le moteur hors fin de monde avant de l'oublier ou de le tuer (complète l'entrée de la 2.1 sur la copie de la sonde).
  evidence: maybe-false. Une copie temporaire est-elle détruite hors chargement (sortie de la zone de traitement…) ? À observer en jeu ; `Kill` relirait alors `+0x28` dans de la mémoire libérée.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-2-apparition-retrait-avatars.md`
  summary: Rendre portables et tester sous Linux `matrix_of` (convention du lacet, AD-7) et les prédicats des hooks de l'avatar (`on_set_ragdoll` laisse passer `on=false`), en story 2.4.
  evidence: `sync/avatar.cpp` n'est compilé que pour Windows : inverser le signe de `m[2]`/`m[8]` ou retirer la règle `!on` ne fait échouer aucun test automatique.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-2-apparition-retrait-avatars.md`
  summary: Comprendre pourquoi l'overlay Steam (`gameoverlayui`) cesse d'accepter les connexions du jeu, quelques secondes après la création d'un avatar, après plusieurs sessions ; ou documenter durablement l'overlay désactivé comme prérequis.
  evidence: 3 gels sur 3 avec l'overlay, 0 sur 5 cycles sans. La coïncidence avec les créations d'avatar n'est pas expliquée ; aucun appel du mod ne passe par l'overlay.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-2-apparition-retrait-avatars.md`
  summary: Tester en jeu le chargement rapide en session (avatar « oublié : monde détruit », sans `Kill`).
  evidence: Étape 4 de la procédure du README, non faite le 2026-10-19.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-3-deplacements-fluides-avatar.md`
  summary: Fondu de rattrapage (environ 100 ms) quand les snapshots reprennent après une extrapolation, pour éviter un retour en arrière jusqu'à 250 ms × vitesse.
  evidence: maybe-false, medium si vrai. Il faut plus de 150 ms sans snapshot (trois pertes de suite). Simulation : 4 frames extrapolées sur 3 167 avec 10 % de pertes isolées ; à surveiller en jeu et sur Internet.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-3-deplacements-fluides-avatar.md`
  summary: Tester automatiquement la glu `sync/session.cpp` des snapshots (pose lue en session, `SendSnapshot` et `SnapshotReceived` transmis), par exemple en extrayant le routage en code portable.
  evidence: `session_host` en est une copie : supprimer la branche `SendSnapshot` de `sync/session.cpp` ne fait échouer aucun test.
- source_spec: `_bmad-output/implementation-artifacts/spec-2-4-animations-avatar.md`
  summary: Tester les compteurs `Motion` (et snapshots) du bilan `debug` de 5 s.
  evidence: Inverser deux compteurs dans le `snprintf` ne fait échouer aucun test ; le README demande pourtant de lire cette ligne.
