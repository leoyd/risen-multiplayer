package main

import (
	"crypto/rand"
	"encoding/binary"
	"net"
	"net/netip"
	"sync"
	"testing"
	"time"

	"risen-multiplayer/tools/fake-guest/kcp"
	"risen-multiplayer/tools/fake-guest/protocol"
)

// Hôte simulé : applique le §8 de PROTOCOL.md côté hôte (ordre de vérification du §6,
// même Welcome tant qu'aucun segment KCP n'est arrivé, Reject{4} aux autres adresses,
// heartbeat, timeout) et enregistre ce qu'il reçoit. Paquet de test seulement.

type hostConfig struct {
	Conv               uint32 // 0 = tiré au hasard (non nul)
	ZeroConv           bool   // hôte fautif : Welcome{conv=0}
	ModVersion         string
	Fingerprint        [protocol.FingerprintModule]protocol.Fingerprint
	AnswerFromHello    int           // ne répond qu'à partir du N-ième Hello (hôte lent)
	SilentAfterWelcome bool          // plus rien n'est envoyé après le premier Welcome
	SessionEndAfter    time.Duration // > 0 : envoie SessionEnd{SessionEndReason} après ce délai
	SessionEndReason   uint8
	HeartbeatInterval  time.Duration
	PeerTimeout        time.Duration
	SendMotions        bool   // AniName{0,"Host_Run"} puis un Motion toutes les 100 ms (story 2.4)
	SendWeapon         string // non vide : Weapon{1, nom} juste après le Spawn (story 3.2)
}

func defaultHost() hostConfig {
	return hostConfig{
		ModVersion:        "0.1.0",
		Fingerprint:       steamFingerprint,
		HeartbeatInterval: testHeartbeat,
		PeerTimeout:       testPeerTimeout,
	}
}

type helloRecord struct {
	from    netip.AddrPort
	at      time.Time
	version uint16
}

type hostRecord struct {
	hellos       []helloRecord
	welcomes     []uint32 // conv de chaque Welcome envoyé
	welcomesTo   []netip.AddrPort
	rejects      []uint8
	ignoredHello int // Hello du pair arrivés après un segment KCP (ignorés, §8)
	heartbeats   int
	sessionEnds  []uint8
	spawns       []protocol.Spawn    // Spawn reçus de l'invité (v3)
	despawns     []uint32            // NetId des Despawn reçus
	order        []protocol.MsgType  // Spawn, Weapon, Despawn et SessionEnd, dans l'ordre de réception
	snapshots    []protocol.Snapshot // snapshots reçus de l'invité (v4)
	aniNames     []protocol.AniName  // reçus de l'invité (v5)
	motions      []protocol.Motion
	params       []protocol.MotionParam
	deaths       []uint32
	weapons      []protocol.Weapon // reçus de l'invité (v7)
	snapshotAt   []time.Time       // heure de réception de chacun
	lastDatagram time.Time         // dernier datagramme du pair de session
	timeoutAt    time.Time         // fin « timeout » constatée
	ended        string
}

type simHost struct {
	cfg  hostConfig
	conn *net.UDPConn
	done chan struct{}
	wg   sync.WaitGroup

	mu  sync.Mutex
	rec hostRecord

	// Goroutine de l'hôte uniquement.
	start      time.Time
	silent     bool
	peer       netip.AddrPort
	conv       uint32
	welcome    protocol.Welcome // renvoyé tel quel aux Hello retransmis
	k          *kcp.KCP
	gotSegment bool
	lastPeer   time.Time
	nextHB     time.Time
	endAt      time.Time
	closingAt  time.Time // SessionEnd envoyé : KCP gardé jusqu'à cet instant
	hellosFrom map[netip.AddrPort]int

	sessionStart time.Time // base du tick des snapshots de l'hôte
	lastSnapTick int64
	aniNameSent  bool
}

func startHost(t *testing.T, cfg hostConfig) *simHost {
	t.Helper()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	h := &simHost{cfg: cfg, conn: conn, done: make(chan struct{}), start: time.Now(),
		hellosFrom: map[netip.AddrPort]int{}}
	h.wg.Add(1)
	go h.loop()
	t.Cleanup(h.stop)
	return h
}

func (h *simHost) addr() netip.AddrPort {
	return h.conn.LocalAddr().(*net.UDPAddr).AddrPort()
}

func (h *simHost) stop() {
	select {
	case <-h.done:
		return
	default:
	}
	close(h.done)
	h.conn.Close()
	h.wg.Wait()
}

func (h *simHost) record() hostRecord {
	h.mu.Lock()
	defer h.mu.Unlock()
	r := h.rec
	r.hellos = append([]helloRecord(nil), r.hellos...)
	r.welcomes = append([]uint32(nil), r.welcomes...)
	r.welcomesTo = append([]netip.AddrPort(nil), r.welcomesTo...)
	r.rejects = append([]uint8(nil), r.rejects...)
	r.sessionEnds = append([]uint8(nil), r.sessionEnds...)
	return r
}

// waitFor attend que `cond` soit vraie sur l'enregistrement, au plus `limit`.
func (h *simHost) waitFor(limit time.Duration, cond func(hostRecord) bool) (hostRecord, bool) {
	deadline := time.Now().Add(limit)
	for {
		r := h.record()
		if cond(r) {
			return r, true
		}
		if time.Now().After(deadline) {
			return r, false
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func (h *simHost) loop() {
	defer h.wg.Done()
	incoming := make(chan datagram, 256)
	go readLoop(h.conn, incoming, h.done)
	ticker := time.NewTicker(tickInterval)
	defer ticker.Stop()
	defer func() {
		if h.k != nil {
			h.k.Release()
		}
	}()
	for {
		select {
		case <-h.done:
			return
		case d := <-incoming:
			h.handle(d)
		case <-ticker.C:
		}
		h.tick()
	}
}

func (h *simHost) nowMs() uint32 { return uint32(time.Since(h.start).Milliseconds()) }

func (h *simHost) send(to netip.AddrPort, b []byte) {
	if h.silent {
		return
	}
	_, _ = h.conn.WriteToUDPAddrPort(b, to)
}

func (h *simHost) sendControl(to netip.AddrPort, m protocol.Message) {
	b, err := protocol.EncodeControl(m)
	if err != nil {
		panic(err)
	}
	h.mu.Lock()
	switch m := m.(type) {
	case protocol.Welcome:
		h.rec.welcomes = append(h.rec.welcomes, m.Conv)
		h.rec.welcomesTo = append(h.rec.welcomesTo, to)
	case protocol.Reject:
		h.rec.rejects = append(h.rec.rejects, m.Reason)
	}
	h.mu.Unlock()
	h.send(to, b)
}

func (h *simHost) sendReliable(m protocol.Message) {
	b, err := protocol.EncodeReliable(m)
	if err != nil {
		panic(err)
	}
	_ = h.k.Send(b)
}

func (h *simHost) inSession() bool { return h.k != nil && h.closingAt.IsZero() }

func (h *simHost) handle(d datagram) {
	if len(d.b) == 0 {
		return
	}
	if h.k != nil && d.from == h.peer {
		h.lastPeer = time.Now()
		h.mu.Lock()
		h.rec.lastDatagram = h.lastPeer
		h.mu.Unlock()
	}
	switch protocol.Channel(d.b[0]) {
	case protocol.ChannelControl:
		h.handleControl(d)
	case protocol.ChannelSnapshot:
		if h.k == nil || d.from != h.peer {
			return
		}
		if s, err := protocol.DecodeSnapshot(d.b); err == nil {
			h.mu.Lock()
			h.rec.snapshots = append(h.rec.snapshots, s)
			h.rec.snapshotAt = append(h.rec.snapshotAt, time.Now())
			h.mu.Unlock()
		}
	case protocol.ChannelReliable:
		if h.k == nil || d.from != h.peer {
			return
		}
		if conv, ok := kcp.SegmentConv(d.b); !ok || conv != h.conv {
			return
		}
		h.gotSegment = true
		_ = h.k.Input(d.b[1:])
		h.drain()
	}
}

// handleControl : ordre de vérification du §6.
func (h *simHost) handleControl(d datagram) {
	version, isHello := protocol.PeekHelloVersion(d.b)
	if !isHello {
		return // l'hôte ne reçoit que des Hello en contrôle
	}
	h.mu.Lock()
	h.rec.hellos = append(h.rec.hellos, helloRecord{from: d.from, at: time.Now(), version: version})
	h.mu.Unlock()
	h.hellosFrom[d.from]++
	if h.hellosFrom[d.from] < h.cfg.AnswerFromHello {
		return
	}
	if version != protocol.ProtocolVersion {
		h.sendControl(d.from, protocol.Reject{Reason: protocol.RejectProtocol})
		return
	}
	m, err := protocol.DecodeControl(d.b)
	if err != nil {
		h.sendControl(d.from, protocol.Reject{Reason: protocol.RejectBadHello})
		return
	}
	hello := m.(protocol.Hello)
	if hello.ModVersion != h.cfg.ModVersion {
		h.sendControl(d.from, protocol.Reject{Reason: protocol.RejectModVersion})
		return
	}
	if hello.Fingerprint != h.cfg.Fingerprint {
		h.sendControl(d.from, protocol.Reject{Reason: protocol.RejectGameFingerprint})
		return
	}
	if h.inSession() {
		if d.from != h.peer {
			h.sendControl(d.from, protocol.Reject{Reason: protocol.RejectSessionFull})
		} else if !h.gotSegment {
			h.sendControl(d.from, h.welcome) // même Welcome, même conv
		} else {
			h.mu.Lock()
			h.rec.ignoredHello++
			h.mu.Unlock()
		}
		return
	}
	h.openSession(d.from)
}

func (h *simHost) openSession(peer netip.AddrPort) {
	if h.k != nil {
		h.k.Release()
	}
	conv := h.cfg.Conv
	for conv == 0 && !h.cfg.ZeroConv {
		var b [4]byte
		_, _ = rand.Read(b[:])
		conv = binary.LittleEndian.Uint32(b[:])
	}
	k, err := kcp.New(conv, func(b []byte) { h.send(peer, b) })
	if err != nil {
		panic(err)
	}
	now := time.Now()
	h.k, h.peer, h.conv, h.gotSegment = k, peer, conv, false
	h.lastPeer, h.nextHB, h.closingAt = now, now, time.Time{}
	h.endAt = time.Time{}
	if h.cfg.SessionEndAfter > 0 {
		h.endAt = now.Add(h.cfg.SessionEndAfter)
	}
	h.sessionStart, h.lastSnapTick, h.aniNameSent = now, -1, false
	h.welcome = protocol.Welcome{Conv: conv, X: 1, Y: 2, Z: 3, Yaw: 0.5}
	h.sendControl(peer, h.welcome)
	h.sendReliable(protocol.Spawn{NetId: protocol.HostNetId, Kind: protocol.SpawnTypeHero, X: 1, Y: 2, Z: 3, Yaw: 0.5})
	if h.cfg.SendWeapon != "" {
		h.sendReliable(protocol.Weapon{NetId: protocol.HostNetId, Name: h.cfg.SendWeapon})
	}
	if h.cfg.SilentAfterWelcome {
		h.silent = true
	}
}

func (h *simHost) drain() {
	for {
		b, ok := h.k.Recv()
		if !ok {
			return
		}
		m, err := protocol.DecodeReliable(b)
		if err != nil || !h.closingAt.IsZero() {
			continue
		}
		switch m := m.(type) {
		case protocol.Heartbeat:
			h.mu.Lock()
			h.rec.heartbeats++
			h.mu.Unlock()
		case protocol.Spawn:
			h.mu.Lock()
			h.rec.spawns = append(h.rec.spawns, m)
			h.rec.order = append(h.rec.order, protocol.TypeSpawn)
			h.mu.Unlock()
		case protocol.AniName:
			h.mu.Lock()
			h.rec.aniNames = append(h.rec.aniNames, m)
			h.mu.Unlock()
		case protocol.Motion:
			h.mu.Lock()
			h.rec.motions = append(h.rec.motions, m)
			h.mu.Unlock()
		case protocol.MotionParam:
			h.mu.Lock()
			h.rec.params = append(h.rec.params, m)
			h.mu.Unlock()
		case protocol.Death:
			h.mu.Lock()
			h.rec.deaths = append(h.rec.deaths, m.NetId)
			h.mu.Unlock()
		case protocol.Weapon:
			h.mu.Lock()
			h.rec.weapons = append(h.rec.weapons, m)
			h.rec.order = append(h.rec.order, protocol.TypeWeapon)
			h.mu.Unlock()
		case protocol.Despawn:
			h.mu.Lock()
			h.rec.despawns = append(h.rec.despawns, m.NetId)
			h.rec.order = append(h.rec.order, protocol.TypeDespawn)
			h.mu.Unlock()
		case protocol.SessionEnd:
			h.mu.Lock()
			h.rec.sessionEnds = append(h.rec.sessionEnds, m.Reason)
			h.rec.order = append(h.rec.order, protocol.TypeSessionEnd)
			h.rec.ended = "SessionEnd"
			h.mu.Unlock()
			h.k.Flush() // acquittement
			h.closeSession()
			return
		}
	}
}

func (h *simHost) closeSession() {
	h.k.Release()
	h.k = nil
	h.closingAt = time.Time{}
}

func (h *simHost) tick() {
	if h.k == nil {
		return
	}
	now := time.Now()
	h.k.Update(h.nowMs())
	if !h.closingAt.IsZero() {
		if now.After(h.closingAt) || h.k.WaitSnd() == 0 {
			h.closeSession()
		}
		return
	}
	if now.Sub(h.lastPeer) >= h.cfg.PeerTimeout {
		h.mu.Lock()
		h.rec.timeoutAt = now
		h.rec.ended = "timeout"
		h.mu.Unlock()
		h.closeSession()
		return
	}
	if !h.endAt.IsZero() && !now.Before(h.endAt) {
		h.sendReliable(protocol.SessionEnd{Reason: h.cfg.SessionEndReason})
		h.k.Flush()
		h.closingAt = now.Add(300 * time.Millisecond)
		h.mu.Lock()
		h.rec.ended = "SessionEnd envoyé"
		h.mu.Unlock()
		return
	}
	if !now.Before(h.nextHB) {
		h.sendReliable(protocol.Heartbeat{})
		h.nextHB = now.Add(h.cfg.HeartbeatInterval)
	}
	// Snapshot de l'hôte à 20 Hz (pose du Welcome, x croissant de 1 cm par tick).
	if tick := int64(now.Sub(h.sessionStart) / (50 * time.Millisecond)); tick != h.lastSnapTick {
		h.lastSnapTick = tick
		b, _ := protocol.EncodeSnapshot(protocol.Snapshot{Tick: uint32(tick),
			Entities: []protocol.SnapshotEntity{{NetId: protocol.HostNetId, X: 1 + float32(tick), Y: 2, Z: 3, Yaw: 0.5}}})
		h.send(h.peer, b)
		if h.cfg.SendMotions && tick%2 == 0 {
			if !h.aniNameSent {
				h.aniNameSent = true
				h.sendReliable(protocol.AniName{AniId: 0, Name: "Host_Run"})
			}
			h.sendReliable(protocol.Motion{NetId: protocol.HostNetId, Ms: uint32(tick * 50), Kind: 0,
				Flags: protocol.MotionFlagPlay | protocol.MotionFlagDesc, D00: 0.2, D0C: float32(tick), D10: 1, D14: 1, D18: 1})
			h.sendReliable(protocol.MotionParam{NetId: protocol.HostNetId, Ms: uint32(tick * 50), Kind: 0,
				Param: protocol.ParamTargetWeight, Value: 1, Time: 0.25})
		}
	}
}
