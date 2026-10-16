package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"net/netip"
	"sync"
	"time"

	"risen-multiplayer/tools/fake-guest/kcp"
	"risen-multiplayer/tools/fake-guest/protocol"
)

// Codes de sortie (voir usage).
const (
	exitOK          = 0
	exitError       = 1
	exitRejected    = 2
	exitUnreachable = 3
	exitTimeout     = 4
)

// Délais du §8 de PROTOCOL.md. Les tests les raccourcissent ; la CLI garde ceux-ci.
const (
	defaultHelloInterval     = 500 * time.Millisecond
	defaultConnectTimeout    = 10 * time.Second
	defaultHeartbeatInterval = time.Second
	defaultPeerTimeout       = 5 * time.Second
	defaultLinger            = time.Second // attente de l'acquittement du SessionEnd
	tickInterval             = 5 * time.Millisecond
)

// badVersionHello : le Hello de protocol/vectors/hello_v2.bin (version 2, suite illisible).
var badVersionHello = []byte{0x02, 0x01, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff}

// Empreintes par défaut : celles de hello.bin (Risen Steam x64, janvier 2023).
var steamFingerprint = [protocol.FingerprintModule]protocol.Fingerprint{
	{TimeDateStamp: 0x63c9aff6, SizeOfImage: 0x01417000}, // Game.dll
	{TimeDateStamp: 0x63ca945d, SizeOfImage: 0x00d3a000}, // Engine.dll
	{TimeDateStamp: 0x63c9b014, SizeOfImage: 0x004d1000}, // Script.dll
	{TimeDateStamp: 0x63c569b6, SizeOfImage: 0x0025a000}, // SharedBase.dll
}

// sessionConfig : tout ce que fait l'invité, délais compris.
type sessionConfig struct {
	Host       netip.AddrPort
	Hello      protocol.Hello
	BadVersion bool          // envoie badVersionHello au lieu de Hello
	DupHello   bool          // chaque Hello part deux fois, + un Hello après le Welcome
	Duration   time.Duration // 0 = jusqu'à l'annulation du contexte (Ctrl-C)
	Kill       bool          // à la fin : sortie sans rien envoyer (sinon SessionEnd{1})
	Drop       int           // pourcentage de datagrammes sortants jetés
	Seed       int64

	HelloInterval     time.Duration
	ConnectTimeout    time.Duration
	HeartbeatInterval time.Duration
	PeerTimeout       time.Duration
	Linger            time.Duration
}

func defaultTimings(c *sessionConfig) {
	c.HelloInterval = defaultHelloInterval
	c.ConnectTimeout = defaultConnectTimeout
	c.HeartbeatInterval = defaultHeartbeatInterval
	c.PeerTimeout = defaultPeerTimeout
	c.Linger = defaultLinger
}

// sessionResult : bilan pour les tests.
type sessionResult struct {
	Code         int
	Conv         uint32
	HellosSent   int
	HeartbeatsTx int
	HeartbeatsRx int
	Dropped      int
	EndReason    string
}

// logger : une ligne horodatée par événement.
type logger struct {
	mu  sync.Mutex
	out io.Writer
}

func (l *logger) printf(format string, args ...any) {
	l.mu.Lock()
	defer l.mu.Unlock()
	fmt.Fprintf(l.out, "%s %s\n", time.Now().Format("2006-01-02 15:04:05.000"), fmt.Sprintf(format, args...))
}

type datagram struct {
	from netip.AddrPort
	b    []byte
}

type state int

const (
	connecting state = iota
	inSession
)

type guest struct {
	cfg   sessionConfig
	conn  *net.UDPConn
	log   *logger
	rng   *rand.Rand
	start time.Time // origine de l'horloge monotone de KCP
	res   sessionResult

	st         state
	kcp        *kcp.KCP
	lastPeer   time.Time // dernier datagramme reçu de l'hôte
	nextHB     time.Time
	nextHBLog  time.Time
	hbTxPeriod int
	hbRxPeriod int
	endAt      time.Time // fin de --duration (zéro = aucune)
	extraHello bool      // --dup-hello : Hello supplémentaire encore à envoyer
	incoming   chan datagram
}

// runSession joue l'invité sur `conn` jusqu'à la fin de session et rend le bilan.
func runSession(ctx context.Context, conn *net.UDPConn, cfg sessionConfig, out io.Writer) sessionResult {
	g := &guest{cfg: cfg, conn: conn, log: &logger{out: out}, rng: rand.New(rand.NewSource(cfg.Seed)),
		start: time.Now()}
	return g.run(ctx)
}

func (g *guest) nowMs() uint32 { return uint32(time.Since(g.start).Milliseconds()) }

func (g *guest) run(ctx context.Context) sessionResult {
	g.incoming = make(chan datagram, 256)
	done := make(chan struct{})
	defer close(done)
	go readLoop(g.conn, g.incoming, done)

	ticker := time.NewTicker(tickInterval)
	defer ticker.Stop()

	g.log.printf("Connecting host=%s pseudo=%q mod=%q drop=%d%%", g.cfg.Host, g.cfg.Hello.Nickname,
		g.cfg.Hello.ModVersion, g.cfg.Drop)
	connectDeadline := time.Now().Add(g.cfg.ConnectTimeout)
	nextHello := time.Now()

	for {
		now := time.Now()
		switch g.st {
		case connecting:
			if !now.Before(connectDeadline) {
				return g.end(exitUnreachable, "hôte injoignable")
			}
			if !now.Before(nextHello) {
				g.sendHello(g.helloCopies())
				nextHello = now.Add(g.cfg.HelloInterval)
			}
		case inSession:
			if now.Sub(g.lastPeer) >= g.cfg.PeerTimeout {
				return g.endSession(exitTimeout, "timeout", false)
			}
			if !g.endAt.IsZero() && !now.Before(g.endAt) {
				if g.cfg.Kill {
					return g.endSession(exitOK, "kill (rien envoyé)", false)
				}
				return g.endSession(exitOK, protocol.EndReasonName(protocol.EndPlayerLeft), true)
			}
			// --dup-hello : le Hello supplémentaire part une fois le premier heartbeat échangé
			// et acquitté, donc après l'arrivée d'un segment KCP chez l'hôte (§8 : il doit
			// alors l'ignorer).
			if g.extraHello && g.res.HeartbeatsTx > 0 && g.res.HeartbeatsRx > 0 && g.kcp.WaitSnd() == 0 {
				g.extraHello = false
				g.sendHello(1)
			}
			if !now.Before(g.nextHB) {
				g.sendReliable(protocol.Heartbeat{})
				g.nextHB = now.Add(g.cfg.HeartbeatInterval)
			}
			if !now.Before(g.nextHBLog) {
				g.log.printf("Heartbeat envoyés=%d reçus=%d (total %d/%d)", g.hbTxPeriod, g.hbRxPeriod,
					g.res.HeartbeatsTx, g.res.HeartbeatsRx)
				g.hbTxPeriod, g.hbRxPeriod = 0, 0
				g.nextHBLog = now.Add(time.Second)
			}
			g.kcp.Update(g.nowMs())
		}

		select {
		case <-ctx.Done():
			if g.st == inSession {
				return g.endSession(exitOK, protocol.EndReasonName(protocol.EndPlayerLeft)+" (interruption)", true)
			}
			return g.end(exitOK, "interruption")
		case d := <-g.incoming:
			if res, done := g.handle(d); done {
				return res
			}
		case <-ticker.C:
		}
	}
}

// readLoop pousse chaque datagramme reçu jusqu'à la fermeture du socket ou à `done`.
func readLoop(conn *net.UDPConn, out chan<- datagram, done <-chan struct{}) {
	buf := make([]byte, protocol.MaxDatagram+1)
	for {
		n, from, err := conn.ReadFromUDPAddrPort(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		if n > protocol.MaxDatagram {
			continue
		}
		select {
		case out <- datagram{from: netip.AddrPortFrom(from.Addr().Unmap(), from.Port()), b: append([]byte(nil), buf[:n]...)}:
		case <-done:
			return
		}
	}
}

// send : sortie de tout datagramme, avec la perte simulée de --drop.
func (g *guest) send(b []byte) {
	if g.cfg.Drop > 0 && g.rng.Intn(100) < g.cfg.Drop {
		g.res.Dropped++
		return
	}
	_, _ = g.conn.WriteToUDPAddrPort(b, g.cfg.Host)
}

func (g *guest) helloCopies() int {
	if g.cfg.DupHello {
		return 2
	}
	return 1
}

// sendHello envoie `copies` fois le Hello (ou celui de hello_v2.bin avec --bad-version).
func (g *guest) sendHello(copies int) {
	b := badVersionHello
	desc := "Hello version_protocole=2 (hello_v2.bin)"
	if !g.cfg.BadVersion {
		var err error
		if b, err = protocol.EncodeControl(g.cfg.Hello); err != nil {
			g.log.printf("Hello non encodable : %v", err)
			return
		}
		h := g.cfg.Hello
		desc = fmt.Sprintf("Hello version_protocole=%d mod=%q pseudo=%q équipement=%q empreintes=%s",
			h.ProtocolVersion, h.ModVersion, h.Nickname, h.Equipment, fingerprintString(h.Fingerprint))
	}
	for i := 0; i < copies; i++ {
		g.res.HellosSent++
		g.log.printf("-> %s", desc)
		g.send(b)
	}
}

func fingerprintString(fp [protocol.FingerprintModule]protocol.Fingerprint) string {
	s := ""
	for i, f := range fp {
		if i > 0 {
			s += ","
		}
		s += fmt.Sprintf("%08x/%08x", f.TimeDateStamp, f.SizeOfImage)
	}
	return s
}

func (g *guest) sendReliable(m protocol.Message) {
	b, err := protocol.EncodeReliable(m)
	if err == nil {
		err = g.kcp.Send(b)
	}
	if err != nil {
		g.log.printf("envoi fiable impossible (%T) : %v", m, err)
		return
	}
	switch m := m.(type) {
	case protocol.Heartbeat:
		g.res.HeartbeatsTx++
		g.hbTxPeriod++
	case protocol.SessionEnd:
		g.log.printf("-> SessionEnd raison=%d (%s)", m.Reason, protocol.EndReasonName(m.Reason))
	}
}

// handle traite un datagramme ; done = la session est finie.
func (g *guest) handle(d datagram) (sessionResult, bool) {
	if d.from != g.cfg.Host {
		return sessionResult{}, false // autre émetteur : ignoré
	}
	if len(d.b) == 0 {
		return sessionResult{}, false
	}
	if g.st == inSession {
		g.lastPeer = time.Now() // tout datagramme de l'hôte compte (§8)
	}
	switch protocol.Channel(d.b[0]) {
	case protocol.ChannelControl:
		return g.handleControl(d.b)
	case protocol.ChannelReliable:
		if g.st != inSession || !g.acceptSegment(d) {
			return sessionResult{}, false // pas de KCP avant le Welcome ; autre conv
		}
		if err := g.kcp.Input(d.b[1:]); err != nil {
			g.log.printf("segment KCP refusé : %v", err)
		}
		return g.drain()
	}
	return sessionResult{}, false
}

// acceptSegment : datagramme de canal 0 venu de l'hôte et portant le conv de la session.
func (g *guest) acceptSegment(d datagram) bool {
	if d.from != g.cfg.Host {
		return false
	}
	conv, ok := kcp.SegmentConv(d.b)
	return ok && conv == g.res.Conv
}

func (g *guest) handleControl(b []byte) (sessionResult, bool) {
	m, err := protocol.DecodeControl(b)
	if err != nil {
		g.log.printf("<- contrôle illisible : %v", err)
		return sessionResult{}, false
	}
	switch m := m.(type) {
	case protocol.Welcome:
		g.log.printf("<- Welcome conv=0x%08x pos=(%g, %g, %g) lacet=%g", m.Conv, m.X, m.Y, m.Z, m.Yaw)
		if m.Conv == 0 {
			g.log.printf("Welcome invalide (conv nul, interdit par le §8) : ignoré")
			return sessionResult{}, false
		}
		if g.st == inSession {
			if m.Conv != g.res.Conv {
				g.log.printf("Welcome ignoré : conv 0x%08x différent de la session 0x%08x", m.Conv, g.res.Conv)
			}
			return sessionResult{}, false
		}
		g.enterSession(m.Conv)
	case protocol.Reject:
		g.log.printf("<- Reject raison=%d (%s)", m.Reason, protocol.RejectReasonName(m.Reason))
		if g.st == connecting {
			return g.end(exitRejected, fmt.Sprintf("rejet (%s)", protocol.RejectReasonName(m.Reason))), true
		}
	default:
		g.log.printf("<- %T inattendu, ignoré", m)
	}
	return sessionResult{}, false
}

func (g *guest) enterSession(conv uint32) {
	k, err := kcp.New(conv, g.send)
	if err != nil {
		g.log.printf("KCP : %v", err)
		return
	}
	now := time.Now()
	g.kcp = k
	g.st = inSession
	g.res.Conv = conv
	g.lastPeer = now
	g.nextHB = now
	g.nextHBLog = now.Add(time.Second)
	if g.cfg.Duration > 0 {
		g.endAt = now.Add(g.cfg.Duration)
	}
	g.log.printf("InSession conv=0x%08x", conv)
	g.extraHello = g.cfg.DupHello
}

// drain lit les messages complets rendus par KCP.
func (g *guest) drain() (sessionResult, bool) {
	for {
		b, ok := g.kcp.Recv()
		if !ok {
			return sessionResult{}, false
		}
		m, err := protocol.DecodeReliable(b)
		if err != nil {
			g.log.printf("<- message fiable illisible : %v", err)
			continue
		}
		switch m := m.(type) {
		case protocol.Heartbeat:
			g.res.HeartbeatsRx++
			g.hbRxPeriod++
		case protocol.SessionEnd:
			g.log.printf("<- SessionEnd raison=%d (%s)", m.Reason, protocol.EndReasonName(m.Reason))
			g.kcp.Flush() // acquitte le SessionEnd avant de fermer
			return g.endSession(exitOK, protocol.EndReasonName(m.Reason), false), true
		}
	}
}

// endSession : SessionEnd{1} si demandé (puis attente bornée de son acquittement),
// fermeture du KCP, et fin.
func (g *guest) endSession(code int, reason string, sendEnd bool) sessionResult {
	if sendEnd {
		g.sendReliable(protocol.SessionEnd{Reason: protocol.EndPlayerLeft})
		g.kcp.Flush()
		g.linger()
	}
	g.kcp.Release()
	return g.end(code, reason)
}

// linger fait tourner KCP jusqu'à l'acquittement de ce qui reste, ou au plus cfg.Linger.
// Les messages reçus pendant ce temps sont ignorés : la session est déjà finie.
func (g *guest) linger() {
	deadline := time.NewTimer(g.cfg.Linger)
	defer deadline.Stop()
	ticker := time.NewTicker(tickInterval)
	defer ticker.Stop()
	for g.kcp.WaitSnd() > 0 {
		g.kcp.Update(g.nowMs())
		select {
		case <-deadline.C:
			return
		case d := <-g.incoming:
			if g.acceptSegment(d) {
				_ = g.kcp.Input(d.b[1:])
			}
		case <-ticker.C:
		}
	}
}

func (g *guest) end(code int, reason string) sessionResult {
	g.res.Code = code
	g.res.EndReason = reason
	g.log.printf("Ended raison=%s (Hello envoyés=%d, heartbeats envoyés=%d reçus=%d, jetés=%d) sortie=%d",
		reason, g.res.HellosSent, g.res.HeartbeatsTx, g.res.HeartbeatsRx, g.res.Dropped, code)
	return g.res
}
