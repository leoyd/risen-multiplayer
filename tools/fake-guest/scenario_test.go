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
	var err error
	if cfg.Scenario, err = parseScenario(strings.NewReader("cercle 300 6\n")); err != nil {
		t.Fatal(err)
	}
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

func TestScenarioEvents(t *testing.T) {
	sc, err := parseScenario(strings.NewReader("pause 2\nanim 0.5 0 Hero_Run\nanim 1 2 Hero_Attack 0.1 0 0 1 -1 1 1\nmort 1.5\n"))
	if err != nil {
		t.Fatal(err)
	}
	if len(sc.events) != 3 || sc.events[0].name != "Hero_Run" || sc.events[0].motion.D00 != 0.2 ||
		sc.events[0].motion.Flags != protocol.MotionFlagPlay|protocol.MotionFlagDesc {
		t.Fatalf("événements : %+v", sc.events)
	}
	if m := sc.events[1].motion; m.Kind != 2 || m.D00 != 0.1 || m.D10 != 0xFFFFFFFF {
		t.Fatalf("anim avec scalaires : %+v", m)
	}
	// Première boucle : les trois ; deuxième : les deux anim, pas la mort.
	var names []string
	prev := -1.0
	for ms := 0.0; ms <= 4000; ms += 10 {
		for _, ev := range sc.due(prev, ms) {
			if ev.death {
				names = append(names, "mort")
			} else {
				names = append(names, ev.name)
			}
		}
		prev = ms
	}
	if got := strings.Join(names, ","); got != "Hero_Run,Hero_Attack,mort,Hero_Run,Hero_Attack" {
		t.Fatalf("ordre : %s", got)
	}
	for _, bad := range []string{"pause 1\nanim 2 0 X\n", "pause 1\nanim 0 8 X\n", "pause 1\nanim 0 0\n",
		"pause 1\nmort\n", "anim 0 0 X\n", "pause 1\nanim 0 0 X 1 2\n", "pause 1\nanim -1 0 X\n",
		"pause 1\nanim 0 0 X 0.2 0 0.5 1 1 1 1\n", "pause 1\nanim 0 0 X 0.2 0 0 1 -2 1 1\n"} {
		if _, err := parseScenario(strings.NewReader(bad)); err == nil {
			t.Errorf("%q accepté", bad)
		}
	}
	for _, f := range []string{"scenarios/gestes.txt", "scenarios/mort.txt"} {
		if _, err := loadScenario(f); err != nil {
			t.Errorf("%s : %v", f, err)
		}
	}
	// Ordre par instant, quel que soit l'ordre du fichier.
	sc2, _ := parseScenario(strings.NewReader("pause 2\nanim 1 0 B\nanim 0.5 0 A\n"))
	if sc2 == nil || sc2.events[0].name != "A" || sc2.events[1].name != "B" {
		t.Fatalf("gestes non triés : %+v", sc2)
	}
}

func TestScenarioSendsMotionsAndDeath(t *testing.T) {
	t.Parallel()
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 900 * time.Millisecond
	var err error
	if cfg.Scenario, err = parseScenario(strings.NewReader("pause 0.4\nanim 0.1 0 Run\nanim 0.2 2 Attack\nmort 0.3\n")); err != nil {
		t.Fatal(err)
	}
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if len(rec.aniNames) != 2 || rec.aniNames[0] != (protocol.AniName{AniId: 0, Name: "Run"}) ||
		rec.aniNames[1] != (protocol.AniName{AniId: 1, Name: "Attack"}) {
		t.Fatalf("AniName : %+v", rec.aniNames)
	}
	if n := len(rec.motions); n < 4 || n > 6 { // 2 par boucle de 0,4 s, sur 0,9 s
		t.Fatalf("%d Motion", n)
	}
	for i, m := range rec.motions {
		if m.NetId != protocol.GuestNetId || (i > 0 && m.Ms < rec.motions[i-1].Ms) {
			t.Fatalf("Motion %d : %+v", i, m)
		}
	}
	if len(rec.deaths) != 1 || rec.deaths[0] != protocol.GuestNetId {
		t.Fatalf("Death : %v", rec.deaths)
	}
}

func TestEcho(t *testing.T) {
	t.Parallel()
	hc := defaultHost()
	hc.SendMotions = true
	host := startHost(t, hc)
	cfg := guestConfig(host.addr())
	cfg.Duration = 1200 * time.Millisecond
	cfg.Echo = true
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if len(rec.snapshots) < 15 {
		t.Fatalf("%d snapshots renvoyés", len(rec.snapshots))
	}
	// Pose renvoyée = pose de l'hôte (x = 1 + tick) décalée du Spawn de l'invité.
	spawn := guestPoseNear(protocol.Welcome{X: 1, Y: 2, Z: 3, Yaw: 0.5})
	last := rec.snapshots[len(rec.snapshots)-1].Entities[0]
	if last.NetId != protocol.GuestNetId || math.Abs(float64(last.Z-spawn.Z)) > 1e-3 || last.X-spawn.X < 5 {
		t.Fatalf("écho : %+v (Spawn %+v)", last, spawn)
	}
	if len(rec.aniNames) != 1 || rec.aniNames[0].Name != "Host_Run" {
		t.Fatalf("AniName renvoyés : %+v", rec.aniNames)
	}
	if len(rec.params) < 5 || rec.params[0].NetId != protocol.GuestNetId || rec.params[0].Value != 1 || rec.params[0].Time != 0.25 {
		t.Fatalf("MotionParam renvoyés : %d, premier %+v", len(rec.params), rec.params)
	}
	if len(rec.motions) < 5 || rec.motions[0].D0C != 0 || rec.motions[0].NetId != protocol.GuestNetId {
		t.Fatalf("Motion renvoyés : %d, premier %+v", len(rec.motions), rec.motions)
	}
	expectLog(t, r, "<- AniName id=0 nom=Host_Run", "Motion reçus=")
}

func TestScenarioParam(t *testing.T) {
	sc, err := parseScenario(strings.NewReader("pause 1\nanim 0.1 0 Run\nparam 0.2 0 poids 1 0.25\nparam 0.3 1 vitesse 0.5\n"))
	if err != nil {
		t.Fatal(err)
	}
	if len(sc.events) != 3 || sc.events[1].param == nil || *sc.events[1].param != (protocol.MotionParam{Kind: 0, Param: protocol.ParamTargetWeight, Value: 1, Time: 0.25}) ||
		sc.events[2].param == nil || *sc.events[2].param != (protocol.MotionParam{Kind: 1, Param: protocol.ParamPlaySpeed, Value: 0.5}) {
		t.Fatalf("événements : %+v", sc.events)
	}
	for _, bad := range []string{"pause 1\nparam 0 8 poids 1\n", "pause 1\nparam 0 0 force 1\n", "pause 1\nparam 0 0 vitesse 1 0.2\n",
		"pause 1\nparam 0 0 poids\n", "pause 1\nparam 0 0 poids 1 -1\n", "pause 1\nparam 0 0 fondu 0.5 0.2\n"} {
		if _, err := parseScenario(strings.NewReader(bad)); err == nil {
			t.Errorf("%q accepté", bad)
		}
	}
	if sc2, err := parseScenario(strings.NewReader("pause 1\nparam 0 0 fondu 1 0.3\nparam 0.5 0 arret 0 0.2\n")); err != nil ||
		sc2.events[0].param.Param != protocol.ParamFadeOut || sc2.events[1].param.Param != protocol.ParamStopMotion {
		t.Fatalf("fondu/arret : %v", err)
	}
	host := startHost(t, defaultHost())
	cfg := guestConfig(host.addr())
	cfg.Duration = 600 * time.Millisecond
	cfg.Scenario = sc
	r := runGuest(t, context.Background(), cfg)
	expectCode(t, r, exitOK)
	rec := host.record()
	if len(rec.params) < 2 || rec.params[0].NetId != protocol.GuestNetId || rec.params[0].Param != protocol.ParamTargetWeight {
		t.Fatalf("MotionParam reçus : %+v", rec.params)
	}
}
