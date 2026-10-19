package main

// Scénario de trajectoire (--scenario, story 2.3) : une directive par ligne, jouées dans
// l'ordre puis en boucle. Chaque directive part de la pose d'ancrage (celle du Spawn de
// l'invité) et y revient, si bien que la trajectoire reste continue d'une directive à l'autre :
//
//	cercle <rayon cm> <durée d'un tour s>    tour complet, en tournant vers la droite
//	aller-retour <longueur cm> <vitesse cm/s> tout droit, demi-tour sec, retour
//	pause <durée s>                          immobile
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
		return nil, fmt.Errorf("scénario vide")
	}
	return sc, nil
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
