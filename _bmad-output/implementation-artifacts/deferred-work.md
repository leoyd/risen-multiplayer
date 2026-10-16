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
  summary: Tester l'effet de `log_level` (lignes `DEBUG` présentes en debug, absentes en info) dès le premier appel à `mp::log::debug`.
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
  summary: Arrêter explicitement le transport avant DLL_PROCESS_DETACH : ne jamais faire le `join` du thread réseau sous le verrou du chargeur.
  evidence: `~Transport` appelle `stop()` (join) ; un `Transport` global détruit au déchargement de la DLL peut bloquer (story 1.4).
- source_spec: `_bmad-output/implementation-artifacts/spec-1-2-transport-udp-kcp.md`
  summary: Faire échouer (ou marquer ignoré) ctest quand `wine_net` / `wine_scenarios` ne peuvent pas être enregistrés, au lieu d'un simple message STATUS.
  evidence: Configurer `build-tests` avant le build Windows donne 3/3 au lieu de 4/4 sans alerte ; le code Winsock n'est alors pas testé.
- source_spec: `_bmad-output/implementation-artifacts/spec-1-2-transport-udp-kcp.md`
  summary: Mettre `CLAUDE.md` à jour : le faux invité Go (`tools/fake-guest/`, outil de test jamais livré) est autorisé par AD-1 amendé.
  evidence: `CLAUDE.md` dit encore « Plus de Go ni de server/ pour le MVP », ce qui contredit la spine et `third_party/kcp/VERSION.txt`.
