package main

// Scénario de trajectoire (--scenario, story 2.3) : une directive par ligne, jouées dans
// l'ordre puis en boucle. Chaque directive part de la pose d'ancrage (celle du Spawn de
// l'invité) et y revient, si bien que la trajectoire reste continue d'une directive à l'autre :
//
//	cercle <rayon cm> <durée d'un tour s>    tour complet, en tournant vers la droite
//	aller-retour <longueur cm> <vitesse cm/s> tout droit, demi-tour sec, retour
//	pause <durée s>                          immobile
//	anim <instant s> <type> <nom> [d00 d04 d08 d0C d10 d14 d18]
//	                                         Motion à cet instant de la boucle (story 2.4) ;
//	                                         scalaires par défaut : 0.2 0 0 1 1 1 1 (d10 = -1 : 0xFFFFFFFF)
//	mort <instant s>                         Death à cet instant (une seule fois)
//	param <instant s> <type> <poids|vitesse|fondu|arret> <valeur> [durée s]
//	                                         (fondu, arret : valeur 0 ou 1)
//	                                         MotionParam à cet instant (story 2.4b)
//	# commentaire (ligne entière)
//
// Axes : avant = (sin lacet, 0, cos lacet), droite = (cos lacet, 0, −sin lacet), comme la
// bCMatrix du jeu (AD-7).

import (
	"bufio"
	"fmt"
	"io"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"

	"risen-multiplayer/tools/fake-guest/protocol"
)

type stepKind int

const (
	stepCircle stepKind = iota
	stepBackAndForth
	stepPause
)

type step struct {
	kind     stepKind
	a, b     float64 // cercle : rayon, durée (s) ; aller-retour : longueur, vitesse ; pause : durée
	duration float64 // ms
}

type scenario struct {
	steps  []step
	period float64 // ms, somme des durées
	events []event // anim et mort, triés par instant (ordre du fichier à instant égal)
}

// event : un geste horodaté (ms depuis le début de la boucle).
type event struct {
	at     float64
	death  bool
	name   string
	motion protocol.Motion       // Kind, Flags et scalaires ; NetId, AniId et Ms posés à l'envoi
	param  *protocol.MotionParam // directive param : Kind, Param, Value, Time ; NetId et Ms à l'envoi
}

func loadScenario(path string) (*scenario, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return parseScenario(f)
}

func parseScenario(r io.Reader) (*scenario, error) {
	sc := &scenario{}
	lines := bufio.NewScanner(r)
	for n := 1; lines.Scan(); n++ {
		line := strings.TrimSpace(lines.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Fields(line)
		if f[0] == "anim" || f[0] == "mort" || f[0] == "param" {
			ev, err := parseEvent(f)
			if err != nil {
				return nil, fmt.Errorf("ligne %d : %v", n, err)
			}
			sc.events = append(sc.events, ev)
			continue
		}
		args := make([]float64, 0, len(f)-1)
		for _, a := range f[1:] {
			v, err := strconv.ParseFloat(a, 64)
			if err != nil || !(v > 0) || math.IsInf(v, 0) {
				return nil, fmt.Errorf("ligne %d : %q n'est pas un nombre positif", n, a)
			}
			args = append(args, v)
		}
		var s step
		switch {
		case f[0] == "cercle" && len(args) == 2:
			s = step{kind: stepCircle, a: args[0], b: args[1], duration: args[1] * 1000}
		case f[0] == "aller-retour" && len(args) == 2:
			s = step{kind: stepBackAndForth, a: args[0], b: args[1], duration: 2 * args[0] / args[1] * 1000}
		case f[0] == "pause" && len(args) == 1:
			s = step{kind: stepPause, a: args[0], duration: args[0] * 1000}
		default:
			return nil, fmt.Errorf("ligne %d : %q attendu « cercle <rayon> <durée> », « aller-retour <longueur> <vitesse> » ou « pause <durée> »", n, line)
		}
		sc.steps = append(sc.steps, s)
		sc.period += s.duration
	}
	if err := lines.Err(); err != nil {
		return nil, err
	}
	if len(sc.steps) == 0 {
		return nil, fmt.Errorf("scénario sans trajectoire (au moins un cercle, aller-retour ou pause)")
	}
	sort.SliceStable(sc.events, func(i, j int) bool { return sc.events[i].at < sc.events[j].at })
	for _, ev := range sc.events {
		if ev.at >= sc.period {
			return nil, fmt.Errorf("geste à %g s au-delà de la boucle (%g s)", ev.at/1000, sc.period/1000)
		}
	}
	return sc, nil
}

func parseEvent(f []string) (event, error) {
	num := func(s string) (float64, error) {
		v, err := strconv.ParseFloat(s, 64)
		if err != nil || math.IsNaN(v) || math.IsInf(v, 0) {
			return 0, fmt.Errorf("%q n'est pas un nombre", s)
		}
		return v, nil
	}
	if len(f) < 2 {
		return event{}, fmt.Errorf("instant manquant")
	}
	at, err := num(f[1])
	if err != nil || at < 0 {
		return event{}, fmt.Errorf("instant %q invalide", f[1])
	}
	if f[0] == "param" {
		if len(f) != 5 && len(f) != 6 {
			return event{}, fmt.Errorf("« param <instant s> <type> <poids|vitesse> <valeur> [durée s] » attendu")
		}
		typ, err := strconv.Atoi(f[2])
		if err != nil || typ < 0 || typ >= protocol.MotionTypeCount {
			return event{}, fmt.Errorf("type %q hors de 0..7", f[2])
		}
		p := &protocol.MotionParam{Kind: uint8(typ)}
		switch f[3] {
		case "poids":
			p.Param = protocol.ParamTargetWeight
		case "vitesse":
			p.Param = protocol.ParamPlaySpeed
		case "fondu":
			p.Param = protocol.ParamFadeOut
		case "arret":
			p.Param = protocol.ParamStopMotion
		default:
			return event{}, fmt.Errorf("nature %q : « poids », « vitesse », « fondu » ou « arret » attendu", f[3])
		}
		v, err := num(f[4])
		if err != nil {
			return event{}, err
		}
		if v < 0 || v > protocol.MaxMotionParamValue {
			return event{}, fmt.Errorf("valeur %q hors de [0, 100]", f[4])
		}
		if (p.Param == protocol.ParamFadeOut || p.Param == protocol.ParamStopMotion) && v != 0 && v != 1 {
			return event{}, fmt.Errorf("valeur %q : 0 ou 1 attendu pour %s", f[4], f[3])
		}
		p.Value = float32(v)
		if len(f) == 6 {
			if p.Param == protocol.ParamPlaySpeed {
				return event{}, fmt.Errorf("pas de durée pour une vitesse")
			}
			d, err := num(f[5])
			if err != nil || d < 0 || d > protocol.MaxMotionParamTime {
				return event{}, fmt.Errorf("durée %q invalide", f[5])
			}
			p.Time = float32(d)
		}
		return event{at: at * 1000, param: p}, nil
	}
	if f[0] == "mort" {
		if len(f) != 2 {
			return event{}, fmt.Errorf("« mort <instant s> » attendu")
		}
		return event{at: at * 1000, death: true}, nil
	}
	if len(f) != 4 && len(f) != 11 {
		return event{}, fmt.Errorf("« anim <instant s> <type> <nom> [d00 d04 d08 d0C d10 d14 d18] » attendu")
	}
	typ, err := strconv.Atoi(f[2])
	if err != nil || typ < 0 || typ >= protocol.MotionTypeCount {
		return event{}, fmt.Errorf("type %q hors de 0..7", f[2])
	}
	if len(f[3]) > protocol.MaxAniName {
		return event{}, fmt.Errorf("nom de plus de 255 octets")
	}
	vals := []float64{0.2, 0, 0, 1, 1, 1, 1}
	if len(f) == 11 {
		for i := range vals {
			if vals[i], err = num(f[4+i]); err != nil {
				return event{}, err
			}
			// d08, d10 et d18 sont des u32 : entiers de -1 (0xFFFFFFFF) à 4294967295.
			if (i == 2 || i == 4 || i == 6) && (vals[i] != math.Trunc(vals[i]) || vals[i] < -1 || vals[i] > math.MaxUint32) {
				return event{}, fmt.Errorf("%q : entier attendu (-1 à 4294967295)", f[4+i])
			}
		}
	}
	u32 := func(v float64) uint32 { return uint32(int64(v)) } // -1 → 0xFFFFFFFF
	m := protocol.Motion{Kind: uint8(typ), Flags: protocol.MotionFlagPlay | protocol.MotionFlagDesc,
		D00: float32(vals[0]), D04: float32(vals[1]), D08: u32(vals[2]), D0C: float32(vals[3]), D10: u32(vals[4]),
		D14: float32(vals[5]), D18: u32(vals[6])}
	return event{at: at * 1000, name: f[3], motion: m}, nil
}

// due rend les gestes dont l'instant (répété à chaque boucle) tombe dans ]prev, now] (ms depuis
// l'entrée en session) ; une mort ne part qu'à la première boucle.
func (sc *scenario) due(prev, now float64) []event {
	var out []event
	for _, ev := range sc.events {
		k := math.Floor((now - ev.at) / sc.period)
		if k < 0 {
			continue
		}
		at := ev.at + k*sc.period
		if at > prev && at <= now && !(ev.death && k > 0) {
			out = append(out, ev)
		}
	}
	return out
}

func wrapAngle(a float64) float64 {
	a = math.Remainder(a, 2*math.Pi)
	if a <= -math.Pi {
		a += 2 * math.Pi
	}
	return a
}

// poseAt rend la pose au temps t (ms depuis l'entrée en session), relative à l'ancrage.
func (sc *scenario) poseAt(anchor protocol.Spawn, t float64) protocol.Spawn {
	t = math.Mod(t, sc.period)
	if t < 0 {
		t += sc.period
	}
	i := 0
	for i < len(sc.steps)-1 && t >= sc.steps[i].duration {
		t -= sc.steps[i].duration
		i++
	}
	s := sc.steps[i]
	yaw0 := float64(anchor.Yaw)
	fx, fz := math.Sin(yaw0), math.Cos(yaw0)  // avant
	rx, rz := math.Cos(yaw0), -math.Sin(yaw0) // droite
	var forward, right, yaw float64
	switch s.kind {
	case stepCircle:
		theta := 2 * math.Pi * math.Min(t, s.duration) / s.duration
		forward = s.a * math.Sin(theta)
		right = s.a * (1 - math.Cos(theta))
		// Vitesse : avant × cos θ + droite × sin θ.
		vx := fx*math.Cos(theta) + rx*math.Sin(theta)
		vz := fz*math.Cos(theta) + rz*math.Sin(theta)
		yaw = math.Atan2(vx, vz)
	case stepBackAndForth:
		d := s.b * t / 1000
		if d < s.a {
			forward, yaw = d, yaw0
		} else {
			forward, yaw = math.Max(2*s.a-d, 0), yaw0+math.Pi
		}
	case stepPause:
		yaw = yaw0
	}
	p := anchor
	p.X = anchor.X + float32(fx*forward+rx*right)
	p.Z = anchor.Z + float32(fz*forward+rz*right)
	p.Yaw = float32(wrapAngle(yaw))
	return p
}
