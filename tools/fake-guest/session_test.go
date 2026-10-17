package main

import (
	"bytes"
	"context"
	"net"
	"net/netip"
	"os"
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
		Hello: protocol.Hello{ProtocolVersion: 2, ModVersion: "0.1.0", Fingerprint: steamFingerprint,
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
	h := protocol.Hello{ProtocolVersion: 2, ModVersion: "0.1.0", Fingerprint: steamFingerprint, Nickname: "Leo",
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
	expectLog(t, r, "Connecting host=", "-> Hello version_protocole=2", "<- Welcome conv=", "InSession conv=0x",
		"Heartbeat envoyés=", "-> SessionEnd raison=1", "Ended raison=départ du joueur")
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
	if strings.Count(r.log, "pos=(1, 2, 3) lacet=0.5") != 2 {
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
	if strings.Contains(r.log, "-> SessionEnd") {
		t.Error("SessionEnd envoyé avec --kill")
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
		cfg.Duration != 0 || cfg.Kill || cfg.Drop != 0 || cfg.Seed != 1 || cfg.DupHello || cfg.BadVersion {
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
	}
	for _, c := range cases {
		cfg, code := parseConfig(append(append([]string{}, base...), c.args...), &errOut)
		if code != parseOK || !c.check(cfg) {
			t.Errorf("%q : code %d, config %+v", c.args, code, cfg)
		}
	}
}
