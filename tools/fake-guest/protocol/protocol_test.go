package protocol

import (
	"bytes"
	"errors"
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
		{"hello.bin", false, Hello{ProtocolVersion: 2, ModVersion: "0.1.0", Fingerprint: steamFingerprint,
			Nickname: "Leo", Equipment: "It_Sword_01"}},
		{"welcome.bin", false, Welcome{Conv: 0x12345678, X: 100.0, Y: -50.5, Z: 2000.0, Yaw: 1.5}},
		{"reject.bin", false, Reject{Reason: 2}},
		{"heartbeat.bin", true, Heartbeat{}},
		{"session_end.bin", true, SessionEnd{Reason: 1}},
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
	if v, ok := PeekHelloVersion(vector(t, "hello_truncated.bin")); !ok || v != 2 {
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
		"hello_long_nickname.bin": true, "unknown_type.bin": true,
		// Motion (v2) : codec Go et vérification de ces vecteurs en story 2.4.
		"motion.bin": true, "motion_bad_type.bin": true}
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
		{"réservé", []byte{18}, true, ErrUnknownType},
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
	empty := Hello{ProtocolVersion: 2, Nickname: "x"}
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
	ok := Hello{ProtocolVersion: 2, ModVersion: "0.1.0", Nickname: "Leo"}
	bad := []Hello{
		{ProtocolVersion: 1, Nickname: "Leo"},
		{ProtocolVersion: 2, Nickname: ""},
		{ProtocolVersion: 2, Nickname: strings.Repeat("A", 33)},
		{ProtocolVersion: 2, Nickname: "Leo", ModVersion: strings.Repeat("v", 256)},
		{ProtocolVersion: 2, Nickname: "Leo", Equipment: strings.Repeat("e", 256)},
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
	max := Hello{ProtocolVersion: 2, ModVersion: strings.Repeat("v", 255), Nickname: strings.Repeat("n", 32),
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
