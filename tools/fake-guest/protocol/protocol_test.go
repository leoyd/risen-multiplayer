package protocol

import (
	"bytes"
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Vecteurs de référence partagés avec les tests C++ (relatif au paquet).
const vectorsDir = "../../../protocol/vectors"

func vector(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(vectorsDir, name))
	if err != nil {
		t.Fatalf("vecteur %s : %v", name, err)
	}
	return b
}

// Empreintes de Risen Steam x64 (janvier 2023), celles de hello.bin.
var steamFingerprint = [FingerprintModule]Fingerprint{
	{0x63c9aff6, 0x01417000},
	{0x63ca945d, 0x00d3a000},
	{0x63c9b014, 0x004d1000},
	{0x63c569b6, 0x0025a000},
}

func TestValidVectorsRoundTrip(t *testing.T) {
	cases := []struct {
		file     string
		reliable bool
		want     Message
	}{
		{"hello.bin", false, Hello{ProtocolVersion: ProtocolVersion, ModVersion: "0.1.0", Fingerprint: steamFingerprint,
			Nickname: "Leo", Equipment: "It_Sword_01"}},
		{"welcome.bin", false, Welcome{Conv: 0x12345678, X: 100.0, Y: -50.5, Z: 2000.0, Yaw: 1.5}},
		{"reject.bin", false, Reject{Reason: 2}},
		{"heartbeat.bin", true, Heartbeat{}},
		{"session_end.bin", true, SessionEnd{Reason: 1}},
		{"spawn.bin", true, Spawn{NetId: 2, Kind: SpawnTypeHero, X: 100.0, Y: -50.5, Z: 2000.0, Yaw: 1.5}},
		{"despawn.bin", true, Despawn{NetId: 2}},
		{"aniname.bin", true, AniName{AniId: 7, Name: "Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400"}},
		{"death.bin", true, Death{NetId: 2}},
		{"motion_param.bin", true, MotionParam{NetId: 2, Ms: 1234, Kind: 0, Param: ParamTargetWeight, Value: 1.0, Time: 0.25}},
		{"motion.bin", true, Motion{NetId: 2, Ms: 1234, Kind: 0, AniId: 7, Flags: 3, D00: 0.25, D04: 0.5, D08: 1, D0C: 1.0,
			D10: 0, D14: 0.75, D18: 2}},
	}
	for _, c := range cases {
		t.Run(c.file, func(t *testing.T) {
			raw := vector(t, c.file)
			var got Message
			var err error
			if c.reliable {
				got, err = DecodeReliable(raw)
			} else {
				got, err = DecodeControl(raw)
			}
			if err != nil {
				t.Fatalf("décodage : %v", err)
			}
			if got != c.want {
				t.Fatalf("décodé %#v, attendu %#v", got, c.want)
			}
			var enc []byte
			if c.reliable {
				enc, err = EncodeReliable(got)
			} else {
				enc, err = EncodeControl(got)
			}
			if err != nil {
				t.Fatalf("encodage : %v", err)
			}
			if !bytes.Equal(enc, raw) {
				t.Fatalf("réencodage différent :\n% x\n% x", enc, raw)
			}
		})
	}
}

func TestInvalidVectors(t *testing.T) {
	cases := []struct {
		file string
		want error
	}{
		{"hello_future.bin", ErrUnsupportedVersion},
		{"hello_truncated.bin", ErrTruncated},
		{"hello_long_nickname.bin", ErrBadLength},
		{"unknown_type.bin", ErrUnknownType},
	}
	for _, c := range cases {
		t.Run(c.file, func(t *testing.T) {
			if _, err := DecodeControl(vector(t, c.file)); !errors.Is(err, c.want) {
				t.Fatalf("erreur %v, attendu %v", err, c.want)
			}
		})
	}
	if v, ok := PeekHelloVersion(vector(t, "hello_future.bin")); !ok || v != 0xffff {
		t.Fatalf("peek hello_future = %d, %v", v, ok)
	}
	if v, ok := PeekHelloVersion(vector(t, "hello_truncated.bin")); !ok || v != ProtocolVersion {
		t.Fatalf("peek hello_truncated = %d, %v", v, ok)
	}
}

func TestAllVectorsCovered(t *testing.T) {
	entries, err := os.ReadDir(vectorsDir)
	if err != nil {
		t.Fatal(err)
	}
	known := map[string]bool{"hello.bin": true, "welcome.bin": true, "reject.bin": true, "heartbeat.bin": true,
		"session_end.bin": true, "hello_future.bin": true, "hello_truncated.bin": true,
		"hello_long_nickname.bin": true, "unknown_type.bin": true, "spawn.bin": true, "despawn.bin": true,
		"snapshot.bin": true, "snapshot_truncated.bin": true,
		"motion.bin": true, "motion_bad_type.bin": true, "aniname.bin": true, "death.bin": true,
		"aniname_empty.bin": true, "motion_param.bin": true, "motion_param_bad.bin": true}
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".bin") && !known[e.Name()] {
			t.Errorf("vecteur %s non testé", e.Name())
		}
	}
}

func TestDecodeErrors(t *testing.T) {
	hello := vector(t, "hello.bin")
	cases := []struct {
		name     string
		b        []byte
		reliable bool
		want     error
	}{
		{"vide contrôle", nil, false, ErrTruncated},
		{"vide fiable", nil, true, ErrTruncated},
		{"canal seul", []byte{2}, false, ErrTruncated},
		{"canal fiable", []byte{0, 1}, false, ErrUnknownChannel},
		{"canal snapshot", []byte{1, 1}, false, ErrUnknownChannel},
		{"heartbeat en contrôle", []byte{2, 16}, false, ErrUnknownType},
		{"hello en fiable", []byte{1, 1, 0}, true, ErrUnknownType},
		{"réservé", []byte{25}, true, ErrUnknownType},
		{"réservé 22", []byte{22}, true, ErrUnknownType},
		{"spawn en contrôle", []byte{2, 20}, false, ErrUnknownType},
		{"spawn tronqué", []byte{20, 2, 0, 0, 0, 0}, true, ErrTruncated},
		{"despawn trop long", []byte{21, 2, 0, 0, 0, 0}, true, ErrTrailingBytes},
		{"reject tronqué", []byte{2, 3}, false, ErrTruncated},
		{"reject trop long", []byte{2, 3, 1, 0}, false, ErrTrailingBytes},
		{"heartbeat trop long", []byte{16, 0}, true, ErrTrailingBytes},
		{"session_end tronqué", []byte{17}, true, ErrTruncated},
		{"welcome tronqué", []byte{2, 2, 1, 2, 3}, false, ErrTruncated},
		{"hello + octet", append(append([]byte{}, hello...), 0), false, ErrTrailingBytes},
		{"hello version tronquée", []byte{2, 1, 1}, false, ErrTruncated},
	}
	for _, c := range cases {
		var err error
		if c.reliable {
			_, err = DecodeReliable(c.b)
		} else {
			_, err = DecodeControl(c.b)
		}
		if !errors.Is(err, c.want) {
			t.Errorf("%s : erreur %v, attendu %v", c.name, err, c.want)
		}
	}
	// Pseudo vide → BadLength.
	empty := Hello{ProtocolVersion: ProtocolVersion, Nickname: "x"}
	b, err := EncodeControl(empty)
	if err != nil {
		t.Fatal(err)
	}
	b[len(b)-3] = 0 // longueur du pseudo
	if _, err := DecodeControl(b[:len(b)-1]); !errors.Is(err, ErrBadLength) {
		t.Errorf("pseudo vide : %v", err)
	}
}

func TestUnknownReasonsKept(t *testing.T) {
	m, err := DecodeControl([]byte{2, 3, 200})
	if err != nil || m != (Reject{Reason: 200}) {
		t.Fatalf("reject 200 : %v %v", m, err)
	}
	m, err = DecodeReliable([]byte{17, 200})
	if err != nil || m != (SessionEnd{Reason: 200}) {
		t.Fatalf("session_end 200 : %v %v", m, err)
	}
	if RejectReasonName(200) != "inconnue" || EndReasonName(200) != "inconnue" {
		t.Fatal("nom de raison inconnue")
	}
}

func TestEncoderBounds(t *testing.T) {
	ok := Hello{ProtocolVersion: ProtocolVersion, ModVersion: "0.1.0", Nickname: "Leo"}
	bad := []Hello{
		{ProtocolVersion: 1, Nickname: "Leo"},
		{ProtocolVersion: ProtocolVersion, Nickname: ""},
		{ProtocolVersion: ProtocolVersion, Nickname: strings.Repeat("A", 33)},
		{ProtocolVersion: ProtocolVersion, Nickname: "Leo", ModVersion: strings.Repeat("v", 256)},
		{ProtocolVersion: ProtocolVersion, Nickname: "Leo", Equipment: strings.Repeat("e", 256)},
	}
	if _, err := EncodeControl(ok); err != nil {
		t.Fatal(err)
	}
	for i, h := range bad {
		if b, err := EncodeControl(h); !errors.Is(err, ErrOutOfBounds) || b != nil {
			t.Errorf("hello %d accepté", i)
		}
	}
	// Bornes maximales : 581 octets.
	max := Hello{ProtocolVersion: ProtocolVersion, ModVersion: strings.Repeat("v", 255), Nickname: strings.Repeat("n", 32),
		Equipment: strings.Repeat("e", 255)}
	if b, err := EncodeControl(max); err != nil || len(b) != 581 {
		t.Fatalf("hello maximal : %d %v", len(b), err)
	}
	if _, err := EncodeControl(Heartbeat{}); !errors.Is(err, ErrOutOfBounds) {
		t.Error("heartbeat accepté en contrôle")
	}
	if _, err := EncodeReliable(Reject{}); !errors.Is(err, ErrOutOfBounds) {
		t.Error("reject accepté en fiable")
	}
}

func TestPeekHelloVersion(t *testing.T) {
	for _, b := range [][]byte{nil, {2, 1, 1}, {2, 2, 1, 0}, {0, 1, 1, 0}} {
		if _, ok := PeekHelloVersion(b); ok {
			t.Errorf("peek % x accepté", b)
		}
	}
}

func TestSnapshotVectors(t *testing.T) {
	raw := vector(t, "snapshot.bin")
	want := Snapshot{Tick: 1234, Entities: []SnapshotEntity{{NetId: 2, X: 100, Y: -50.5, Z: 2000, Yaw: 1.5, HP: 75}}}
	got, err := DecodeSnapshot(raw)
	if err != nil || got.Tick != want.Tick || len(got.Entities) != 1 || got.Entities[0] != want.Entities[0] {
		t.Fatalf("snapshot.bin : %+v, %v", got, err)
	}
	enc, err := EncodeSnapshot(want)
	if err != nil || !bytes.Equal(enc, raw) {
		t.Fatalf("réencodage différent :\n% x\n% x (%v)", enc, raw, err)
	}
	if _, err := DecodeSnapshot(vector(t, "snapshot_truncated.bin")); !errors.Is(err, ErrTruncated) {
		t.Fatalf("snapshot_truncated.bin : %v", err)
	}
	for n := 0; n < len(raw); n++ {
		if _, err := DecodeSnapshot(raw[:n]); !errors.Is(err, ErrTruncated) {
			t.Fatalf("coupé à %d : %v", n, err)
		}
	}
	if _, err := DecodeSnapshot(append(append([]byte{}, raw...), 0)); !errors.Is(err, ErrTrailingBytes) {
		t.Fatalf("+1 octet : %v", err)
	}
	other := append([]byte{}, raw...)
	other[0] = 2
	if _, err := DecodeSnapshot(other); !errors.Is(err, ErrUnknownChannel) {
		t.Fatalf("autre canal : %v", err)
	}
	if _, err := EncodeSnapshot(Snapshot{Entities: make([]SnapshotEntity, MaxSnapshotEntities+1)}); err == nil {
		t.Fatal("55 entités encodées")
	}
	b, err := EncodeSnapshot(Snapshot{Entities: make([]SnapshotEntity, MaxSnapshotEntities)})
	if err != nil || len(b) != 1194 {
		t.Fatalf("54 entités : %d octets, %v", len(b), err)
	}
	b[5] = MaxSnapshotEntities + 1
	b = append(b, make([]byte, SnapshotEntitySize)...)
	if _, err := DecodeSnapshot(b); !errors.Is(err, ErrBadValue) {
		t.Fatalf("55 entités : %v", err)
	}
}

func TestMotionAndAniNameErrors(t *testing.T) {
	if _, err := DecodeReliable(vector(t, "motion_bad_type.bin")); !errors.Is(err, ErrBadValue) {
		t.Fatalf("motion_bad_type.bin : %v", err)
	}
	if _, err := DecodeReliable(vector(t, "motion_param_bad.bin")); !errors.Is(err, ErrBadValue) {
		t.Fatalf("motion_param_bad.bin : %v", err)
	}
	if _, err := EncodeReliable(MotionParam{Kind: 8}); err == nil {
		t.Fatal("MotionParam de type 8 encodé")
	}
	for _, bad := range []MotionParam{{Value: float32(math.NaN())}, {Param: 4}, {Param: ParamFadeOut, Value: 0.5}, {Time: float32(math.Inf(1))},
		{Value: -1}, {Value: 101}, {Time: 61}, {Param: ParamPlaySpeed, Value: 1, Time: 0.1}} {
		if _, err := EncodeReliable(bad); err == nil {
			t.Fatalf("MotionParam hors domaine encodé : %+v", bad)
		}
	}
	for _, ok := range []MotionParam{{Param: ParamFadeOut, Value: 1, Time: 0.3}, {Param: ParamStopMotion, Value: 0, Time: 0.2}} {
		if _, err := EncodeReliable(ok); err != nil {
			t.Fatalf("MotionParam valide refusé : %+v", ok)
		}
	}
	if _, err := DecodeControl([]byte{2, 26}); !errors.Is(err, ErrUnknownType) {
		t.Fatalf("MotionParam en contrôle : %v", err)
	}
	if _, err := DecodeReliable(vector(t, "aniname_empty.bin")); !errors.Is(err, ErrBadLength) {
		t.Fatalf("aniname_empty.bin : %v", err)
	}
	bad := vector(t, "motion.bin")
	bad = append([]byte{}, bad...)
	bad[12] |= 0x04 // bit de flags inconnu
	if _, err := DecodeReliable(bad); !errors.Is(err, ErrBadValue) {
		t.Fatalf("bit de flags inconnu : %v", err)
	}
	if _, err := EncodeReliable(Motion{Kind: 8}); err == nil {
		t.Fatal("Motion de type 8 encodé")
	}
	if _, err := EncodeReliable(Motion{Flags: MotionFlagPlay, D0C: 1}); err == nil {
		t.Fatal("scalaire non nul sans descripteur encodé")
	}
	if _, err := EncodeReliable(AniName{Name: ""}); err == nil {
		t.Fatal("AniName vide encodé")
	}
	if _, err := EncodeReliable(AniName{Name: strings.Repeat("a", 256)}); err == nil {
		t.Fatal("AniName de 256 octets encodé")
	}
	for _, typ := range []byte{22, 23, 25} {
		if _, err := DecodeReliable([]byte{typ}); !errors.Is(err, ErrUnknownType) {
			t.Fatalf("type réservé %d : %v", typ, err)
		}
	}
}
