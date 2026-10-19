package main

import (
	"context"
	"math"
	"strings"
	"testing"
	"time"

	"risen-multiplayer/tools/fake-guest/protocol"
)

func TestParseScenario(t *testing.T) {
	sc, err := parseScenario(strings.NewReader("# test\n\ncercle 300 6\naller-retour 600 300\npause 1.5\n"))
	if err != nil {
		t.Fatal(err)
	}
	if len(sc.steps) != 3 || sc.period != 6000+4000+1500 {
		t.Fatalf("étapes %d, période %g", len(sc.steps), sc.period)
	}
	for _, bad := range []string{"", "# rien\n", "cercle 300\n", "spirale 1 2\n", "pause -1\n", "aller-retour 600 abc\n", "pause 0\n"} {
		if _, err := parseScenario(strings.NewReader(bad)); err == nil {
			t.Errorf("%q accepté", bad)
		}
	}
	for _, f := range []string{"scenarios/cercle.txt", "scenarios/aller-retour.txt", "scenarios/mixte.txt"} {
		if _, err := loadScenario(f); err != nil {
			t.Errorf("%s : %v", f, err)
		}
	}
}

func near(a float32, b float64) bool { return math.Abs(float64(a)-b) < 1e-2 }

// Ancrage au lacet 0 : avant = +z, droite = +x.
func TestScenarioPoses(t *testing.T) {
	anchor := protocol.Spawn{X: 10, Y: 5, Z: 20, Yaw: 0}
	sc, _ := parseScenario(strings.NewReader("cercle 100 4\naller-retour 200 100\npause 1\n"))
	cases := []struct {
		t         float64
		x, z, yaw float64
		desc      string
	}{
		{0, 10, 20, 0, "départ du cercle"},
		{1000, 110, 120, math.Pi / 2, "quart de tour : vers la droite"},
		{2000, 210, 20, math.Pi, "demi-tour"},
		{4000, 10, 20, 0, "fin du cercle = début de l'aller"},
		{5000, 10, 120, 0, "aller à mi-chemin"},
		{7000, 10, 120, math.Pi, "retour à mi-chemin, demi-tour sec"},
		{8500, 10, 20, 0, "pause à l'ancrage"},
		{9000 + 1000, 110, 120, math.Pi / 2, "boucle"},
	}
	for _, c := range cases {
		p := sc.poseAt(anchor, c.t)
		yawOK := math.Abs(wrapAngle(float64(p.Yaw)-c.yaw)) < 1e-2
		if !near(p.X, c.x) || !near(p.Z, c.z) || p.Y != 5 || !yawOK {
			t.Errorf("%s (t=%g) : (%g, %g) lacet %g, attendu (%g, %g) lacet %g", c.desc, c.t, p.X, p.Z, p.Yaw, c.x, c.z, c.yaw)
		}
	}
	// Continuité : aucun saut de plus de 2 cm entre deux instants à 5 ms d'écart.
	prev := sc.poseAt(anchor, 0)
	for ms := 5.0; ms < 3*sc.period; ms += 5 {
		p := sc.poseAt(anchor, ms)
		if d := math.Hypot(float64(p.X-prev.X), float64(p.Z-prev.Z)); d > 2 {
			t.Fatalf("saut de %g cm à t=%g", d, ms)
		}
		prev = p
	}
}

func TestSnapshotsBothWays(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 1300 * time.Millisecond
	cfg.Scenario, _ = parseScenario(strings.NewReader("cercle 300 6\n"))
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if n := len(rec.snapshots); n < 20 || n > 28 {
		t.Fatalf("%d snapshots reçus par l'hôte en 1,3 s (26 attendus)", n)
	}
	for i, s := range rec.snapshots {
		if len(s.Entities) != 1 || s.Entities[0].NetId != protocol.GuestNetId {
			t.Fatalf("snapshot %d : %+v", i, s)
		}
		if i > 0 && s.Tick <= rec.snapshots[i-1].Tick {
			t.Fatalf("ticks non croissants : %d puis %d", rec.snapshots[i-1].Tick, s.Tick)
		}
	}
	// Le premier part de la pose du Spawn ; les suivants suivent le cercle.
	first, last := rec.snapshots[0].Entities[0], rec.snapshots[len(rec.snapshots)-1].Entities[0]
	want := guestPoseNear(protocol.Welcome{X: 1, Y: 2, Z: 3, Yaw: 0.5})
	if math.Hypot(float64(first.X-want.X), float64(first.Z-want.Z)) > 10 {
		t.Errorf("premier snapshot (%g, %g), Spawn (%g, %g)", first.X, first.Z, want.X, want.Z)
	}
	if math.Hypot(float64(last.X-first.X), float64(last.Z-first.Z)) < 100 {
		t.Errorf("l'avatar n'a pas bougé : (%g, %g) → (%g, %g)", first.X, first.Z, last.X, last.Z)
	}
	if r.res.SnapshotsRx < 10 {
		t.Errorf("%d snapshots de l'hôte reçus par l'invité", r.res.SnapshotsRx)
	}
	expectLog(t, r, "snapshots envoyés=")
}

func TestDelay(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 800 * time.Millisecond
	cfg.Delay = 100 * time.Millisecond
	cfg.Jitter = 20 * time.Millisecond
	start := time.Now()
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if len(rec.hellos) == 0 || len(rec.snapshotAt) == 0 {
		t.Fatalf("hellos %d, snapshots %d", len(rec.hellos), len(rec.snapshotAt))
	}
	// Le premier Hello part à t=0 : il ne peut pas arriver avant 80 ms.
	if d := rec.hellos[0].at.Sub(start); d < 80*time.Millisecond {
		t.Errorf("premier Hello reçu après %v (retard de 100 ± 20 ms attendu)", d)
	}
}
