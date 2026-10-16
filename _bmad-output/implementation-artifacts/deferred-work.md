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
  summary: [partiellement fait 2026-10-17, story 1.4 ; reporté en 1.5 : durée du maintien du KCP après SessionEnd] Rendre normatif dans `PROTOCOL.md` §8 : Welcome ou Reject reçu en session, durée du maintien du KCP après SessionEnd, Hello invalide venant du pair accepté, `conv` neuf après un timeout.
  evidence: Ces comportements n'existent que dans le code et le README du faux invité ; la story 1.4 doit les fixer pour la DLL.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: Valider à deux machines le côté invité de la DLL (Welcome reçu, session, heartbeats, Reject reçu, timeout) ; une seule instance ne vérifie que « hôte injoignable ».
  evidence: Deux Risen ne tournent pas sur la même machine ; ce côté n'est couvert que par `tests/test_session.cpp` (logique pure).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-4-heberger-rejoindre.md`
  summary: Durée de maintien du KCP après un `SessionEnd` envoyé : la DLL n'envoie pas encore de `SessionEnd` (story 1.5), à fixer dans §8 avec la fin de session volontaire.
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
