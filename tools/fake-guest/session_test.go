package main

import (
	"bytes"
	"context"
	"fmt"
	"math"
	"net"
	"net/netip"
	"os"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"risen-multiplayer/tools/fake-guest/protocol"
)

// Délais réduits pour les tests (les valeurs par défaut de la CLI restent celles du §8).
const (
	testHello          = 100 * time.Millisecond
	testConnectTimeout = time.Second
	testHeartbeat      = 100 * time.Millisecond
	testPeerTimeout    = 500 * time.Millisecond
	testLinger         = 300 * time.Millisecond
)

// syncBuffer : journal partagé entre la goroutine de l'invité et le test.
type syncBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (s *syncBuffer) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.Write(p)
}

func (s *syncBuffer) String() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.String()
}

func guestConfig(host netip.AddrPort) sessionConfig {
	return sessionConfig{
		Host: host,
		Hello: protocol.Hello{ProtocolVersion: protocol.ProtocolVersion, ModVersion: "0.1.0", Fingerprint: steamFingerprint,
			Nickname: "Invite"},
		Seed:              1,
		HelloInterval:     testHello,
		ConnectTimeout:    testConnectTimeout,
		HeartbeatInterval: testHeartbeat,
		PeerTimeout:       testPeerTimeout,
		Linger:            testLinger,
	}
}

type guestRun struct {
	res     sessionResult
	log     string
	elapsed time.Duration
}

func runGuest(t *testing.T, ctx context.Context, cfg sessionConfig) guestRun {
	t.Helper()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	var log syncBuffer
	start := time.Now()
	res := runSession(ctx, conn, cfg, &log)
	r := guestRun{res: res, log: log.String(), elapsed: time.Since(start)}
	t.Logf("journal de l'invité :\n%s", r.log)
	return r
}

func expectCode(t *testing.T, r guestRun, code int) {
	t.Helper()
	if r.res.Code != code {
		t.Fatalf("code de sortie %d, attendu %d", r.res.Code, code)
	}
}

func expectLog(t *testing.T, r guestRun, parts ...string) {
	t.Helper()
	for _, p := range parts {
		if !strings.Contains(r.log, p) {
			t.Errorf("journal sans %q", p)
		}
	}
}

// Le Hello de --bad-version et les empreintes par défaut sont ceux des vecteurs.
func TestToolMatchesVectors(t *testing.T) {
	future, err := os.ReadFile("../../protocol/vectors/hello_future.bin")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(future, badVersionHello) {
		t.Fatal("badVersionHello ≠ hello_future.bin")
	}
	ref, err := os.ReadFile("../../protocol/vectors/hello.bin")
	if err != nil {
		t.Fatal(err)
	}
	h := protocol.Hello{ProtocolVersion: protocol.ProtocolVersion, ModVersion: "0.1.0", Fingerprint: steamFingerprint, Nickname: "Leo",
		Equipment: "It_Sword_01"}
	if b, err := protocol.EncodeControl(h); err != nil || !bytes.Equal(b, ref) {
		t.Fatal("empreintes par défaut ≠ hello.bin")
	}
}

func TestNominal(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 1300 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	expectLog(t, r, "Connecting host=", fmt.Sprintf("-> Hello version_protocole=%d", protocol.ProtocolVersion), "<- Welcome conv=", "InSession conv=0x",
		"-> Spawn netid=2 type=0", "<- Spawn netid=1 type=0 pos=(1, 2, 3) lacet=0.5", "Heartbeat envoyés=",
		"-> Despawn netid=2", "-> SessionEnd raison=1", "Ended raison=départ du joueur")
	if r.res.SpawnsRx != 1 {
		t.Errorf("Spawn reçus par l'invité : %d, attendu 1", r.res.SpawnsRx)
	}
	if n := strings.Count(r.log, "Heartbeat"); n > 3 {
		t.Errorf("%d lignes de heartbeat : un résumé par seconde attendu", n)
	}
	rec, _ := host.waitFor(time.Second, func(r hostRecord) bool { return len(r.sessionEnds) > 0 })
	if rec.heartbeats < 8 || r.res.HeartbeatsRx < 8 {
		t.Errorf("heartbeats reçus : hôte %d, invité %d (≈13 attendus)", rec.heartbeats, r.res.HeartbeatsRx)
	}
	if len(rec.sessionEnds) != 1 || rec.sessionEnds[0] != protocol.EndPlayerLeft {
		t.Errorf("SessionEnd reçus par l'hôte : %v", rec.sessionEnds)
	}
	// Spawn de l'invité 2 m devant l'hôte (pose du Welcome : (1, 2, 3), lacet 0.5), tourné vers lui.
	want := guestPoseNear(protocol.Welcome{X: 1, Y: 2, Z: 3, Yaw: 0.5})
	if len(rec.spawns) != 1 || rec.spawns[0] != want {
		t.Errorf("Spawn reçus par l'hôte : %v, attendu %v", rec.spawns, want)
	}
	if len(rec.despawns) != 1 || rec.despawns[0] != protocol.GuestNetId {
		t.Errorf("Despawn reçus par l'hôte : %v", rec.despawns)
	}
	// Weapon (v7) juste après le Spawn : ici arme rangée (config de test sans --weapon).
	wantOrder := []protocol.MsgType{protocol.TypeSpawn, protocol.TypeWeapon, protocol.TypeDespawn, protocol.TypeSessionEnd}
	if fmt.Sprint(rec.order) != fmt.Sprint(wantOrder) {
		t.Errorf("ordre reçu par l'hôte : %v, attendu %v", rec.order, wantOrder)
	}
	if len(rec.welcomes) == 0 || rec.welcomes[0] != r.res.Conv || r.res.Conv == 0 {
		t.Errorf("conv : hôte %v, invité 0x%x", rec.welcomes, r.res.Conv)
	}
}

func TestSlowHost(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.AnswerFromHello = 3
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 200 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	expectLog(t, r, "InSession conv=")
	rec := host.record()
	if len(rec.hellos) != 3 || r.res.HellosSent != 3 {
		t.Fatalf("%d Hello reçus, %d envoyés, 3 attendus", len(rec.hellos), r.res.HellosSent)
	}
	for i := 1; i < 3; i++ {
		gap := rec.hellos[i].at.Sub(rec.hellos[i-1].at)
		if gap < testHello*8/10 || gap > testHello*3 {
			t.Errorf("écart entre Hello %d et %d : %v (≈%v attendu)", i, i+1, gap, testHello)
		}
	}
}

func TestUnreachable(t *testing.T) {
	t.Parallel()
	// Port libre : lié puis relâché, personne n'y répond.
	c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	addr := c.LocalAddr().(*net.UDPAddr).AddrPort()
	c.Close()
	r := runGuest(t, context.Background(), guestConfig(addr))
	expectCode(t, r, exitUnreachable)
	expectLog(t, r, "Ended raison=hôte injoignable")
	if r.res.HellosSent < 9 || r.res.HellosSent > 11 {
		t.Errorf("%d Hello envoyés en %v (≈10 attendus)", r.res.HellosSent, testConnectTimeout)
	}
	if r.elapsed < testConnectTimeout || r.elapsed > testConnectTimeout+time.Second {
		t.Errorf("abandon après %v", r.elapsed)
	}
}

func TestModVersionRejected(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.ModVersion = "0.2.0"
	host := startHost(t, hc)
	r := runGuest(t, context.Background(), guestConfig(host.addr()))
	expectCode(t, r, exitRejected)
	expectLog(t, r, "<- Reject raison=2 (version du mod différente)", "Ended raison=rejet")
	if rec := host.record(); len(rec.rejects) != 1 || rec.rejects[0] != protocol.RejectModVersion {
		t.Errorf("rejets de l'hôte : %v", rec.rejects)
	}
}

func TestFingerprintRejected(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.Fingerprint[protocol.ModScript].SizeOfImage++
	host := startHost(t, hc)
	r := runGuest(t, context.Background(), guestConfig(host.addr()))
	expectCode(t, r, exitRejected)
	expectLog(t, r, "<- Reject raison=3 (empreinte du jeu différente)")
}

func TestBadVersion(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.BadVersion = true
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitRejected)
	expectLog(t, r, "-> Hello version_protocole=65535", "<- Reject raison=1 (version du protocole différente)")
	rec := host.record()
	if len(rec.hellos) == 0 || rec.hellos[0].version != 0xffff {
		t.Errorf("version lue par l'hôte : %+v", rec.hellos)
	}
	if len(rec.rejects) != 1 || rec.rejects[0] != protocol.RejectProtocol {
		t.Errorf("rejets : %v", rec.rejects)
	}
}

func TestDupHello(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.DupHello = true
	cfg.Duration = 500 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if len(rec.hellos) != 3 || r.res.HellosSent != 3 {
		t.Fatalf("%d Hello reçus, %d envoyés (2 + 1 après le Welcome attendus)", len(rec.hellos), r.res.HellosSent)
	}
	// Les deux Hello d'avant KCP reçoivent chacun le même Welcome ; celui d'après le premier
	// segment KCP est ignoré (§8).
	if len(rec.welcomes) != 2 || rec.ignoredHello != 1 {
		t.Fatalf("%d Welcome (2 attendus), %d Hello ignorés (1 attendu)", len(rec.welcomes), rec.ignoredHello)
	}
	if n := regexp.MustCompile(`<- Welcome conv=0x[0-9a-f]+ pos=\(1, 2, 3\) lacet=0\.5`).FindAllString(r.log, -1); len(n) != 2 {
		t.Error("les Welcome renvoyés ne sont pas identiques")
	}
	for _, c := range rec.welcomes {
		if c != r.res.Conv {
			t.Errorf("Welcome conv=0x%x, session 0x%x", c, r.res.Conv)
		}
	}
	if len(rec.sessionEnds) != 1 {
		t.Errorf("SessionEnd : %v", rec.sessionEnds)
	}
}

func TestZeroConvWelcomeIgnored(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.ZeroConv = true
	host := startHost(t, hc)
	r := runGuest(t, context.Background(), guestConfig(host.addr()))
	expectCode(t, r, exitUnreachable)
	expectLog(t, r, "<- Welcome conv=0x00000000", "Welcome invalide (conv nul")
	if strings.Contains(r.log, "InSession") {
		t.Error("session ouverte sur un conv nul")
	}
	if r.res.HellosSent < 5 {
		t.Errorf("%d Hello envoyés : l'invité doit continuer après un Welcome invalide", r.res.HellosSent)
	}
}

func TestDrop20(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Drop = 20
	cfg.Seed = 7
	cfg.Duration = 1500 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	expectLog(t, r, "InSession conv=")
	rec, ok := host.waitFor(time.Second, func(r hostRecord) bool { return len(r.sessionEnds) > 0 })
	if r.res.Dropped == 0 {
		t.Error("aucun datagramme jeté")
	}
	if !ok || rec.sessionEnds[0] != protocol.EndPlayerLeft {
		t.Errorf("SessionEnd reçus par l'hôte malgré les pertes : %v", rec.sessionEnds)
	}
	// KCP a rattrapé chaque perte : l'hôte a reçu tous les heartbeats envoyés.
	if rec.heartbeats != r.res.HeartbeatsTx || rec.heartbeats < 10 {
		t.Errorf("heartbeats : %d reçus par l'hôte, %d envoyés", rec.heartbeats, r.res.HeartbeatsTx)
	}
}

func TestQuit(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 200 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec, ok := host.waitFor(time.Second, func(r hostRecord) bool { return len(r.sessionEnds) > 0 })
	if !ok || rec.sessionEnds[0] != protocol.EndPlayerLeft {
		t.Fatalf("SessionEnd reçus par l'hôte : %v", rec.sessionEnds)
	}
}

func TestKill(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 200 * time.Millisecond
	cfg.Kill = true
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	if strings.Contains(r.log, "-> SessionEnd") || strings.Contains(r.log, "-> Despawn") {
		t.Error("SessionEnd ou Despawn envoyé avec --kill")
	}
	rec, ok := host.waitFor(testPeerTimeout+2*time.Second, func(r hostRecord) bool { return !r.timeoutAt.IsZero() })
	if !ok {
		t.Fatalf("l'hôte n'a pas constaté le timeout (%q)", rec.ended)
	}
	if len(rec.sessionEnds) != 0 {
		t.Errorf("SessionEnd reçus : %v", rec.sessionEnds)
	}
	if gap := rec.timeoutAt.Sub(rec.lastDatagram); gap < testPeerTimeout || gap > testPeerTimeout+time.Second {
		t.Errorf("timeout constaté %v après le dernier datagramme (%v attendu)", gap, testPeerTimeout)
	}
}

func TestHostSilent(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SilentAfterWelcome = true
	host := startHost(t, hc)
	r := runGuest(t, context.Background(), guestConfig(host.addr()))
	expectCode(t, r, exitTimeout)
	expectLog(t, r, "InSession conv=", "Ended raison=timeout")
	if r.elapsed < testPeerTimeout || r.elapsed > testPeerTimeout+time.Second {
		t.Errorf("timeout après %v (≈%v attendu)", r.elapsed, testPeerTimeout)
	}
}

// Story 3.4a : un hôte au jeu figé n'envoie plus que des acquittements KCP. Par défaut ils
// maintiennent la session ; avec --strict-timeout, seul un message compte : timeout.
func TestStrictTimeout(t *testing.T) {
	t.Parallel()
	for _, strict := range []bool{false, true} {
		hc := defaultHost()
		hc.AcksOnly = true
		host := startHost(t, hc)
		cfg := guestConfig(host.addr())
		cfg.StrictTimeout = strict
		cfg.Duration = 3 * testPeerTimeout
		r := runGuest(t, context.Background(), cfg)
		host.stop()
		if !strict {
			expectCode(t, r, exitOK)
			continue
		}
		expectCode(t, r, exitTimeout)
		expectLog(t, r, "Ended raison=timeout")
		if r.elapsed > testPeerTimeout+time.Second {
			t.Errorf("timeout strict après %v (≈%v attendu)", r.elapsed, testPeerTimeout)
		}
	}
}

func TestHostSessionEnd(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SessionEndAfter = 200 * time.Millisecond
	hc.SessionEndReason = protocol.EndHostDied
	host := startHost(t, hc)
	r := runGuest(t, context.Background(), guestConfig(host.addr()))
	expectCode(t, r, exitOK)
	expectLog(t, r, "<- SessionEnd raison=3 (mort de l'hôte)", "Ended raison=mort de l'hôte")
}

func TestSecondGuestRejected(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	// Le premier invité tourne sur sa propre goroutine : pas de t.Fatal/t.Logf là-bas.
	type outcome struct {
		res sessionResult
		log string
	}
	first := make(chan outcome, 1)
	go func() {
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		if err != nil {
			first <- outcome{res: sessionResult{Code: -1}, log: err.Error()}
			return
		}
		defer conn.Close()
		cfg := guestConfig(host.addr())
		cfg.Duration = time.Second
		var log syncBuffer
		res := runSession(context.Background(), conn, cfg, &log)
		first <- outcome{res: res, log: log.String()}
	}()
	_, inSession := host.waitFor(time.Second, func(r hostRecord) bool { return len(r.welcomes) > 0 })
	var second guestRun
	if inSession {
		second = runGuest(t, context.Background(), guestConfig(host.addr()))
	}
	a := <-first // attendu avant tout t.Fatal
	t.Logf("journal du premier invité :\n%s", a.log)
	if !inSession {
		t.Fatal("premier invité pas en session")
	}
	expectCode(t, second, exitRejected)
	expectLog(t, second, "<- Reject raison=4 (session pleine)")
	if a.res.Code != exitOK {
		t.Errorf("premier invité : code %d", a.res.Code)
	}
	if rec := host.record(); len(rec.sessionEnds) != 1 {
		t.Errorf("la première session a été perturbée : %+v", rec)
	}
}

func TestInterruptActsAsQuit(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		host.waitFor(time.Second, func(r hostRecord) bool { return r.heartbeats > 0 })
		cancel()
	}()
	r := runGuest(t, ctx, guestConfig(host.addr()))
	expectCode(t, r, exitOK)
	if rec, ok := host.waitFor(time.Second, func(r hostRecord) bool { return len(r.sessionEnds) > 0 }); !ok ||
		rec.sessionEnds[0] != protocol.EndPlayerLeft {
		t.Fatalf("SessionEnd après Ctrl-C : %v", rec.sessionEnds)
	}
}

func TestCLI(t *testing.T) {
	var out, errOut bytes.Buffer
	if code := run([]string{"--help"}, &out, &errOut); code != exitOK {
		t.Fatalf("--help : code %d", code)
	}
	for _, opt := range []string{"-host", "-port", "-nickname", "-mod-version", "-duration", "-drop", "-seed",
		"-quit", "-kill", "-dup-hello", "-bad-version", "Codes de sortie", "hôte injoignable", "timeout"} {
		if !strings.Contains(errOut.String(), opt) {
			t.Errorf("usage sans %q", opt)
		}
	}
	bad := [][]string{
		{},
		{"--host", "::1"},
		{"--host", "example.org"},
		{"--host", "127.0.0.1", "--drop", "101"},
		{"--host", "127.0.0.1", "--quit", "--kill", "--duration", "1s"},
		{"--host", "127.0.0.1", "--kill"},
		{"--host", "127.0.0.1", "--nickname", ""},
		{"--host", "127.0.0.1", "--nickname", strings.Repeat("x", 33)},
		{"--host", "127.0.0.1", "--port", "70000"},
		{"--host", "127.0.0.1", "--bogus"},
		{"--host", "127.0.0.1", "extra"},
		{"--host", "127.0.0.1", "--weapon", strings.Repeat("w", 256)},
		{"--host", "127.0.0.1", "--armor", strings.Repeat("a", 256)},
		{"--host", "127.0.0.1", "--attack-move", "4"},                             // sans --hit-interval
		{"--host", "127.0.0.1", "--hit-interval", "1s", "--attack-netid", "1004"}, // sans --attack-move
		{"--host", "127.0.0.1", "--hit-interval", "1s", "--attack-move", "4", "--attack-netid", "0"},
	}
	for _, args := range bad {
		if code := run(args, &out, &errOut); code != exitError {
			t.Errorf("%q : code %d, attendu %d", args, code, exitError)
		}
	}
}

func TestParseConfig(t *testing.T) {
	var errOut bytes.Buffer
	cfg, code := parseConfig([]string{"--host", "10.0.0.7"}, &errOut)
	if code != parseOK {
		t.Fatalf("code %d : %s", code, errOut.String())
	}
	// Délais du §8 de PROTOCOL.md.
	if cfg.HelloInterval != 500*time.Millisecond || cfg.ConnectTimeout != 10*time.Second ||
		cfg.HeartbeatInterval != time.Second || cfg.PeerTimeout != 5*time.Second {
		t.Errorf("délais par défaut : %+v", cfg)
	}
	if cfg.Host != netip.MustParseAddrPort("10.0.0.7:27960") || cfg.Hello.Nickname != "Invite" ||
		cfg.Hello.ModVersion != "0.1.0" || cfg.Hello.Fingerprint != steamFingerprint || cfg.Hello.ProtocolVersion != protocol.ProtocolVersion ||
		cfg.Duration != 0 || cfg.Kill || cfg.Drop != 0 || cfg.Seed != 1 || cfg.DupHello || cfg.BadVersion ||
		cfg.Weapon != "It_1H_Sword" || cfg.Armor != nil || cfg.StrictTimeout || cfg.HitInterval != 0 || cfg.HitTarget != 0 ||
		cfg.AttackMove != 0 {
		t.Errorf("config par défaut : %+v", cfg)
	}

	base := []string{"--host", "10.0.0.7"}
	cases := []struct {
		args  []string
		check func(sessionConfig) bool
	}{
		{[]string{"--port", "1234"}, func(c sessionConfig) bool { return c.Host.Port() == 1234 }},
		{[]string{"--nickname", "Bob"}, func(c sessionConfig) bool { return c.Hello.Nickname == "Bob" }},
		{[]string{"--mod-version", "0.2.0"}, func(c sessionConfig) bool { return c.Hello.ModVersion == "0.2.0" }},
		{[]string{"--duration", "2s"}, func(c sessionConfig) bool { return c.Duration == 2*time.Second }},
		{[]string{"--drop", "20"}, func(c sessionConfig) bool { return c.Drop == 20 }},
		{[]string{"--seed", "9"}, func(c sessionConfig) bool { return c.Seed == 9 }},
		{[]string{"--quit", "--duration", "2s"}, func(c sessionConfig) bool { return !c.Kill && c.Duration == 2*time.Second }},
		{[]string{"--kill", "--duration", "2s"}, func(c sessionConfig) bool { return c.Kill }},
		{[]string{"--dup-hello"}, func(c sessionConfig) bool { return c.DupHello }},
		{[]string{"--bad-version"}, func(c sessionConfig) bool { return c.BadVersion }},
		{[]string{"--weapon", "It_2H_Axe"}, func(c sessionConfig) bool { return c.Weapon == "It_2H_Axe" }},
		{[]string{"--weapon", "-"}, func(c sessionConfig) bool { return c.Weapon == "" }},
		{[]string{"--armor", "It_Armor_Inquisitor"}, func(c sessionConfig) bool { return c.Armor != nil && *c.Armor == "It_Armor_Inquisitor" }},
		{[]string{"--armor", "-"}, func(c sessionConfig) bool { return c.Armor != nil && *c.Armor == "" }},
		{[]string{"--strict-timeout"}, func(c sessionConfig) bool { return c.StrictTimeout }},
		{[]string{"--hit-interval", "500ms"}, func(c sessionConfig) bool { return c.HitInterval == 500*time.Millisecond }},
		{[]string{"--hit-target", "1500"}, func(c sessionConfig) bool { return c.HitTarget == 1500 }},
		{[]string{"--hit-interval", "1s", "--attack-move", "4"}, func(c sessionConfig) bool { return c.AttackMove == 4 && c.AttackNetId == 2 }},
		{[]string{"--hit-interval", "1s", "--attack-move", "4", "--attack-netid", "1004"}, func(c sessionConfig) bool { return c.AttackNetId == 1004 }},
	}
	for _, c := range cases {
		cfg, code := parseConfig(append(append([]string{}, base...), c.args...), &errOut)
		if code != parseOK || !c.check(cfg) {
			t.Errorf("%q : code %d, config %+v", c.args, code, cfg)
		}
	}
}

// guestPoseNear doit donner les mêmes nombres que mp::core::session::guest_pose_near
// (client/tests/test_session.cpp) : valeurs littérales, pas recalculées par la fonction testée.
func TestGuestPoseNear(t *testing.T) {
	cases := []struct {
		w            protocol.Welcome
		x, y, z, yaw float64
	}{
		{protocol.Welcome{X: 1, Y: 2, Z: 3, Yaw: 0}, 1, 2, 203, math.Pi},
		{protocol.Welcome{Yaw: 1.5707963}, 200, 0, 0, 1.5707963 - math.Pi},
		{protocol.Welcome{Yaw: -3}, 200 * math.Sin(-3), 0, 200 * math.Cos(-3), -3 + math.Pi},
		{protocol.Welcome{X: 1, Y: 2, Z: 3, Yaw: 0.5}, 96.885, 2, 178.517, 0.5 - math.Pi},
	}
	near := func(a float32, b float64) bool { return math.Abs(float64(a)-b) < 1e-2 }
	for _, c := range cases {
		p := guestPoseNear(c.w)
		if p.NetId != protocol.GuestNetId || p.Kind != protocol.SpawnTypeHero ||
			!near(p.X, c.x) || !near(p.Y, c.y) || !near(p.Z, c.z) || !near(p.Yaw, c.yaw) {
			t.Errorf("guestPoseNear(%v) = %+v, attendu (%g, %g, %g) lacet %g", c.w, p, c.x, c.y, c.z, c.yaw)
		}
	}
}

// Story 3.3 : PNJ annoncés par l'hôte, suivis dans les snapshots, bilan par NetId (§8 v9).
func TestNpcReplicas(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SendNpcs = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1500 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	if r.res.SpawnsRx != 1 {
		t.Errorf("Spawn de héros reçus : %d, attendu 1 (les Spawn{1} sont comptés à part)", r.res.SpawnsRx)
	}
	if r.res.NpcSpawnsRx != 3 {
		t.Errorf("Spawn{1} acceptés : %d, attendu 3 (1000, 1001, puis 1001 réannoncé)", r.res.NpcSpawnsRx)
	}
	if r.res.NpcDespawnsRx != 1 {
		t.Errorf("Despawn de PNJ acceptés : %d, attendu 1", r.res.NpcDespawnsRx)
	}
	if r.res.NpcIgnored != 3 {
		t.Errorf("messages de PNJ ignorés : %d, attendu 3 (Spawn en double, NetId 999, Despawn répété)", r.res.NpcIgnored)
	}
	if r.res.NpcPosesRx < 20 || r.res.NpcUnknownRx < 10 {
		t.Errorf("poses de PNJ %d, entités inconnues %d", r.res.NpcPosesRx, r.res.NpcUnknownRx)
	}
	if r.res.MaxEntities < 3 || r.res.MaxEntities > protocol.MaxSnapshotEntities {
		t.Errorf("au plus %d entités par snapshot", r.res.MaxEntities)
	}
	a, b := r.res.Npcs[1000], r.res.Npcs[1001]
	if a == nil || b == nil {
		t.Fatalf("bilan : %v", r.res.Npcs)
	}
	if a.GUID != npcGuidA || b.GUID != npcGuidB || a.FirstHP != 150 || a.HPChanges == 0 || a.LastHP >= 150 {
		t.Errorf("PNJ 1000 : %+v", *a)
	}
	if b.Spawns != 2 || b.Despawns != 1 || !b.Present || b.LastHP != 80 {
		t.Errorf("PNJ 1001 : %+v", *b)
	}
	if _, ok := r.res.Npcs[999]; ok {
		t.Error("NetId 999 accepté")
	}
	expectLog(t, r, "<- Spawn PNJ netid=1000 guid=c3624668", "ignoré (NetId déjà annoncé)", "ignoré (NetId < 1000)",
		"<- Despawn PNJ netid=1001", "<- Despawn PNJ netid=1001 : ignoré (NetId non annoncé)", "PNJ netid=1000 PV 150 -> 149",
		"Bilan PNJ : 2 vus", "  PNJ netid=1000 guid=c3624668")
	// v10 (story 3.4) : animations comptées par NetId de PNJ annoncé ; retiré ou inconnu → ignorées.
	if a.Motions < 5 || a.Params != a.Motions || b.Motions != 1 || b.Params != 0 {
		t.Errorf("animations : PNJ 1000 Motion=%d MotionParam=%d, PNJ 1001 Motion=%d MotionParam=%d", a.Motions, a.Params,
			b.Motions, b.Params)
	}
	if r.res.NpcAnimIgnored != 2 || r.res.NpcMotionsRx != a.Motions+b.Motions || r.res.NpcParamsRx != a.Params {
		t.Errorf("animations : Motion=%d MotionParam=%d ignorés=%d", r.res.NpcMotionsRx, r.res.NpcParamsRx, r.res.NpcAnimIgnored)
	}
	if r.res.MotionsRx != 0 || r.res.ParamsRx != 0 {
		t.Errorf("Motion/MotionParam du héros comptés : %d/%d, attendu 0 (seuls des PNJ animés)", r.res.MotionsRx, r.res.ParamsRx)
	}
	if r.res.MaxAnimatedNpcs < 1 || r.res.MaxAnimatedNpcs > 2 {
		t.Errorf("PNJ animés en 1 s : %d", r.res.MaxAnimatedNpcs)
	}
	expectLog(t, r, "<- Motion PNJ netid=1000 premier geste ani=5 nom=Wolf_Run",
		"<- Motion PNJ netid=1001 : ignoré (NetId non annoncé ou retiré)",
		"<- MotionParam PNJ netid=1005 : ignoré (NetId non annoncé ou retiré)", "Bilan animations PNJ : Motion=",
		"  PNJ netid=1000 guid=c3624668")
}

// Story 3.4b : Death d'un PNJ (v11) : une fois, plus suivi, jamais réannoncé ; doublon, NetId
// inconnu et réannonce refusés.
func TestNpcDeath(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SendNpcs = true
	hc.NpcDeath = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1500 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	if r.res.NpcDeathsRx != 1 {
		t.Errorf("Death de PNJ acceptés : %d, attendu 1", r.res.NpcDeathsRx)
	}
	if r.res.DeathsRx != 0 {
		t.Errorf("Death du héros comptés : %d, attendu 0", r.res.DeathsRx)
	}
	if r.res.NpcIgnored != 6 {
		t.Errorf("messages de PNJ ignorés : %d, attendu 6 (3 de TestNpcReplicas, Death répété, Death{1006}, réannonce)",
			r.res.NpcIgnored)
	}
	a := r.res.Npcs[1000]
	if a == nil || !a.Dead || a.Present || a.Spawns != 1 || a.Despawns != 1 || !a.Released {
		t.Fatalf("PNJ 1000 : %+v", a)
	}
	// Poses de 1000 après sa mort (ticks 21 à 23) ignorées sans être inattendues : seule celle de
	// 1005 (jamais annoncé) l'est, une par snapshot (une de plus si un snapshot précède les Spawn).
	// 1000 : ticks 0 à 19, plus le tick 20 si son snapshot précède le Death (canal non fiable).
	if r.res.NpcUnknownRx > r.res.SnapshotsRx+1 || a.Poses > 21 {
		t.Errorf("entités inattendues %d, poses de 1000 %d", r.res.NpcUnknownRx, a.Poses)
	}
	if r.res.NpcAnimIgnored != 2 {
		t.Errorf("animations ignorées : %d, attendu 2 (aucune de 1000 après sa mort)", r.res.NpcAnimIgnored)
	}
	expectLog(t, r, "<- Death PNJ netid=1000 (poses reçues=", "<- Death PNJ netid=1000 : ignoré",
		"<- Death PNJ netid=1006 : ignoré", "<- Spawn PNJ netid=1000 guid=c3624668", "ignoré (NetId mort)",
		"<- Despawn PNJ netid=1000 (mort, libéré)", "Bilan PNJ : 2 vus, Spawn=3 Despawn=2 Death=1", " mort")
}

// Story 3.5 : HitRequest (v12) à la cadence de --hit-interval, sur le premier PNJ présent et vivant,
// ou sur --hit-target sans vérification (NetId inconnu compris).
func TestHitRequest(t *testing.T) {
	t.Parallel()
	for _, c := range []struct {
		target uint32
		want   uint32
	}{{0, 1000}, {1500, 1500}} {
		hc := defaultHost()
		hc.SendNpcs = true
		host := startHost(t, hc)
		cfg := guestConfig(host.addr())
		cfg.Duration = 1200 * time.Millisecond
		cfg.HitInterval = 200 * time.Millisecond
		cfg.HitTarget = c.target
		r := runGuest(t, context.Background(), cfg)
		expectCode(t, r, exitOK)
		if r.res.HitsTx < 3 {
			t.Errorf("cible %d : HitRequest envoyés %d, attendu au moins 3", c.target, r.res.HitsTx)
		}
		rec := host.record()
		if len(rec.hits) != r.res.HitsTx {
			t.Errorf("cible %d : HitRequest reçus par l'hôte %d, envoyés %d", c.target, len(rec.hits), r.res.HitsTx)
		}
		for _, id := range rec.hits {
			if id != c.want {
				t.Errorf("cible %d : HitRequest sur %d, attendu %d", c.target, id, c.want)
			}
		}
		expectLog(t, r, fmt.Sprintf("-> HitRequest PNJ netid=%d", c.want), "Bilan coups : HitRequest envoyés=")
	}
}

// Sans --hit-interval, aucun HitRequest.
func TestNoHitRequestByDefault(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SendNpcs = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 600 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	if r.res.HitsTx != 0 || len(host.record().hits) != 0 {
		t.Errorf("HitRequest envoyés %d, reçus %d, attendu 0", r.res.HitsTx, len(host.record().hits))
	}
}

// Story 3.6 : HitOnGuest (v13) de l'hôte, journalisé une fois par message reçu et compté au bilan,
// jamais appliqué.
func TestHitOnGuest(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.HitOnGuestEvery = 4 // toutes les 200 ms
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1200 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	sent := host.record().hitsOnGuest
	if r.res.HitsOnGuestRx < 3 {
		t.Errorf("HitOnGuest reçus %d, attendu au moins 3 (envoyés %d)", r.res.HitsOnGuestRx, sent)
	}
	if r.res.HitsOnGuestRx > sent || sent-r.res.HitsOnGuestRx > 1 {
		t.Errorf("HitOnGuest reçus %d, envoyés %d (au plus un en vol à la fin)", r.res.HitsOnGuestRx, sent)
	}
	expectLog(t, r, "<- HitOnGuest source=1000 dégâts=18", "HitOnGuest reçus=")
}

// Story 4.1 : DialogLine (v16) de l'hôte, journalisé une fois par message reçu et compté au bilan,
// jamais rejoué.
func TestDialogLine(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.DialogEvery = 4 // toutes les 200 ms
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1200 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	sent := host.record().dialogLines
	if r.res.DialogLinesRx < 3 {
		t.Errorf("DialogLine reçus %d, attendu au moins 3 (envoyés %d)", r.res.DialogLinesRx, sent)
	}
	if r.res.DialogLinesRx > sent || sent-r.res.DialogLinesRx > 1 {
		t.Errorf("DialogLine reçus %d, envoyés %d (au plus un en vol à la fin)", r.res.DialogLinesRx, sent)
	}
	expectLog(t, r, "<- DialogLine locuteur=1004 type=0 échantillon="+dialogSample,
		"<- DialogLine locuteur=1 type=3 échantillon="+dialogHeroSample, "Bilan dialogues : DialogLine reçus=")
}

// Story 3.8 : NpcAttack (v15) de l'hôte, journalisé une fois par message reçu et compté au bilan,
// jamais appliqué.
func TestNpcAttack(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.NpcAttackEvery = 4 // toutes les 200 ms
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1200 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	sent := host.record().npcAttacks
	if r.res.NpcAttacksRx < 3 {
		t.Errorf("NpcAttack reçus %d, attendu au moins 3 (envoyés %d)", r.res.NpcAttacksRx, sent)
	}
	if r.res.NpcAttacksRx > sent || sent-r.res.NpcAttacksRx > 1 {
		t.Errorf("NpcAttack reçus %d, envoyés %d (au plus un en vol à la fin)", r.res.NpcAttacksRx, sent)
	}
	expectLog(t, r, "<- NpcAttack netid=1005 move=11 cible=2", "NpcAttack reçus=")
}

// Story 3.8b : --attack-move envoie NpcAttack{2, M, cible} juste avant chaque HitRequest, même cible.
func TestGuestNpcAttack(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SendNpcs = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1200 * time.Millisecond
	cfg.HitInterval = 200 * time.Millisecond
	cfg.AttackMove = 4
	cfg.AttackNetId = protocol.GuestNetId
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if r.res.NpcAttacksTx < 3 || r.res.NpcAttacksTx != r.res.HitsTx || len(rec.guestAttacks) != r.res.NpcAttacksTx {
		t.Errorf("NpcAttack envoyés %d, reçus par l'hôte %d, HitRequest %d", r.res.NpcAttacksTx, len(rec.guestAttacks),
			r.res.HitsTx)
	}
	for i, a := range rec.guestAttacks {
		if a.NetId != protocol.GuestNetId || a.Move != 4 || i >= len(rec.hits) || a.Target != rec.hits[i] {
			t.Errorf("NpcAttack %d : %+v (HitRequest %v)", i, a, rec.hits)
		}
	}
	expectLog(t, r, "-> NpcAttack netid=2 move=4 cible=1000", "NpcAttack reçus=0 envoyés=")
}

// Sans --attack-move, aucun NpcAttack envoyé.
func TestNoGuestNpcAttackByDefault(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SendNpcs = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 600 * time.Millisecond
	cfg.HitInterval = 200 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	if r.res.NpcAttacksTx != 0 || len(host.record().guestAttacks) != 0 {
		t.Errorf("NpcAttack envoyés %d, reçus %d, attendu 0", r.res.NpcAttacksTx, len(host.record().guestAttacks))
	}
}

// Story 3.7 : --die-every envoie Death{2} ; l'hôte répond Respawn, l'invité journalise l'ordre, prend
// la pose et renvoie l'accusé avec le premier tick de snapshot qui la porte ; puis remeurt DieEvery
// après chaque Respawn.
func TestDieEveryRespawn(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.RespawnOnDeath = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1500 * time.Millisecond
	cfg.DieEvery = 300 * time.Millisecond
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if r.res.DownDeathsTx < 2 || r.res.RespawnsRx < 2 || r.res.RespawnAcks != r.res.RespawnsRx {
		t.Fatalf("Death envoyés %d, Respawn reçus %d, accusés %d : attendu au moins 2 cycles",
			r.res.DownDeathsTx, r.res.RespawnsRx, r.res.RespawnAcks)
	}
	if r.res.DownDeathsTx-r.res.RespawnsRx > 1 {
		t.Errorf("Death envoyés %d pour %d Respawn : un seul Death par mise à terre", r.res.DownDeathsTx, r.res.RespawnsRx)
	}
	if len(rec.respawnAcks) == 0 {
		t.Fatal("aucun accusé reçu par l'hôte")
	}
	first := rec.respawnAcks[0]
	if first.NetId != protocol.GuestNetId || first.X != respawnPose.X || first.Y != respawnPose.Y ||
		first.Z != respawnPose.Z || first.Yaw != respawnPose.Yaw || first.Tick == 0 {
		t.Errorf("accusé %+v : attendu la pose du Respawn et un tick non nul", first)
	}
	// Q13 : à partir du tick de l'accusé, les snapshots de l'invité portent la nouvelle pose.
	for _, sn := range rec.snapshots {
		if sn.Tick < first.Tick || len(sn.Entities) == 0 {
			continue
		}
		e := sn.Entities[0]
		if e.X != respawnPose.X || e.Z != respawnPose.Z {
			t.Errorf("snapshot tick %d (accusé %d) : pose (%g, %g), attendu celle du Respawn", sn.Tick, first.Tick, e.X, e.Z)
			break
		}
	}
	expectLog(t, r, "-> Death netid=2 (à terre, --die-every)", "<- Respawn netid=2 pose=(500, 10, -300, 0.5)",
		"-> Respawn accusé tick=", "Bilan à terre : Death envoyés=")
}
