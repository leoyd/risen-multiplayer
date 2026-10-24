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
		{"spawn_npc.bin", true, Spawn{NetId: FirstNpcNetId, Kind: SpawnTypeNpc, X: 100.0, Y: -50.5, Z: 2000.0, Yaw: 1.5,
			GUID: [GuidSize]byte{0xc3, 0x62, 0x46, 0x68, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc}}},
		{"despawn.bin", true, Despawn{NetId: 2}},
		{"aniname.bin", true, AniName{AniId: 7, Name: "Hero_Stand_None_None_P0_Move_Run_N_Fwd_00_%_00_P0_400"}},
		{"death.bin", true, Death{NetId: 2}},
		{"weapon.bin", true, Weapon{NetId: 2, Name: "It_1H_Sword"}},
		{"weapon_sheathed.bin", true, Weapon{NetId: 2, Name: ""}},
		{"armor.bin", true, Armor{NetId: 1, Name: "It_Armor_Inquisitor"}},
		{"armor_removed.bin", true, Armor{NetId: 1, Name: ""}},
		{"motion_param.bin", true, MotionParam{NetId: 2, Ms: 1234, Kind: 0, Param: ParamTargetWeight, Value: 1.0, Time: 0.25}},
		{"motion.bin", true, Motion{NetId: 2, Ms: 1234, Kind: 0, AniId: 7, Flags: 3, D00: 0.25, D04: 0.5, D08: 1, D0C: 1.0,
			D10: 0, D14: 0.75, D18: 2}},
		// v10 (story 3.4) : Motion d'un PNJ, même format.
		{"motion_npc.bin", true, Motion{NetId: FirstNpcNetId, Ms: 1234, Kind: 0, AniId: 3, Flags: 1}},
		// v11 (story 3.4b) : Death d'un PNJ, même format.
		{"death_npc.bin", true, Death{NetId: FirstNpcNetId}},
		// v12 (story 3.5) : coup de l'invité sur une réplique.
		{"hit_request.bin", true, HitRequest{NetId: FirstNpcNetId}},
		// v13 (story 3.6) : coup d'un monstre sur l'avatar de l'invité.
		{"hit_on_guest.bin", true, HitOnGuest{Source: FirstNpcNetId, Damage: 18}},
		// v14 (story 3.7) : réapparition de l'invité (forme d'un accusé).
		{"respawn.bin", true, Respawn{NetId: GuestNetId, X: 100, Y: 0, Z: -50, Yaw: 1.5, Tick: 42}},
		// v15 (story 3.8) : début d'une attaque du PNJ 1005 sur l'avatar de l'invité.
		{"npc_attack.bin", true, NpcAttack{NetId: 1005, Move: 11, Target: GuestNetId}},
		// v16 (story 4.1) : réplique de dialogue du PNJ 1004.
		{"dialog_line.bin", true, DialogLine{Speaker: 1004, OutputType: 0, Sample: "INFO_Sara_2_Sara_DE_%_Sara.wav"}},
		// v17 (story 4.2) : réplique du héros de l'invité, début de son dialogue avec la réplique 1033.
		{"dialog_line_guest.bin", true, DialogLine{Speaker: GuestNetId, OutputType: 3, Sample: "INFO_Sara_1_PC_Hero_DE_%_PC_Hero.wav"}},
		{"dialog_state.bin", true, DialogState{NpcNetId: 1033, Active: 1}},
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
		"aniname_empty.bin": true, "motion_param.bin": true, "motion_param_bad.bin": true,
		"weapon.bin": true, "weapon_sheathed.bin": true, "armor.bin": true, "armor_removed.bin": true,
		"spawn_npc.bin": true, "spawn_bad_type.bin": true, "snapshot_multi.bin": true, "motion_npc.bin": true,
		"death_npc.bin": true, "hit_request.bin": true, "hit_on_guest.bin": true, "respawn.bin": true,
		"npc_attack.bin": true, "dialog_line.bin": true, "dialog_line_guest.bin": true, "dialog_state.bin": true}
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
		{"type absent", []byte{32}, true, ErrUnknownType},
		{"dialogstate en contrôle", []byte{2, 31}, false, ErrUnknownType},
		{"dialogstate tronqué", []byte{31, 9, 4, 0, 0}, true, ErrTruncated},
		{"dialogstate trop long", []byte{31, 9, 4, 0, 0, 1, 0}, true, ErrTrailingBytes},
		{"dialogline en contrôle", []byte{2, 30}, false, ErrUnknownType},
		{"dialogline sans longueur", []byte{30, 0xec, 3, 0, 0, 0}, true, ErrTruncated},
		{"dialogline tronqué", []byte{30, 0xec, 3, 0, 0, 0, 5, 'a', '.', 'w', 'a'}, true, ErrTruncated},
		{"dialogline trop long", []byte{30, 0xec, 3, 0, 0, 0, 5, 'a', '.', 'w', 'a', 'v', 0}, true, ErrTrailingBytes},
		{"npcattack tronqué", []byte{29, 0xed, 3, 0, 0, 0x0b, 0, 0, 0, 2, 0, 0}, true, ErrTruncated},
		{"npcattack trop long", []byte{29, 0xed, 3, 0, 0, 0x0b, 0, 0, 0, 2, 0, 0, 0, 0}, true, ErrTrailingBytes},
		{"npcattack en contrôle", []byte{2, 29}, false, ErrUnknownType},
		{"respawn tronqué", []byte{25, 2, 0, 0, 0, 0, 0, 0xc8, 0x42, 0, 0, 0, 0, 0, 0, 0x48, 0xc2, 0, 0, 0xc0, 0x3f, 0x2a, 0, 0}, true, ErrTruncated},
		{"respawn trop long", []byte{25, 2, 0, 0, 0, 0, 0, 0xc8, 0x42, 0, 0, 0, 0, 0, 0, 0x48, 0xc2, 0, 0, 0xc0, 0x3f, 0x2a, 0, 0, 0, 0}, true, ErrTrailingBytes},
		{"respawn en contrôle", []byte{2, 25}, false, ErrUnknownType},
		{"hitonguest tronqué", []byte{23, 0xe8, 3, 0, 0, 0x12, 0, 0}, true, ErrTruncated},
		{"hitonguest trop long", []byte{23, 0xe8, 3, 0, 0, 0x12, 0, 0, 0, 0}, true, ErrTrailingBytes},
		{"hitonguest en contrôle", []byte{2, 23}, false, ErrUnknownType},
		{"hitrequest tronqué", []byte{22, 0xe8, 3, 0}, true, ErrTruncated},
		{"hitrequest trop long", []byte{22, 0xe8, 3, 0, 0, 0}, true, ErrTrailingBytes},
		{"hitrequest en contrôle", []byte{2, 22}, false, ErrUnknownType},
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
	// Aucun type réservé (depuis la v14) ; un type absent de la liste reste inconnu.
	for _, typ := range []byte{4, 15, 32} {
		if _, err := DecodeReliable([]byte{typ}); !errors.Is(err, ErrUnknownType) {
			t.Fatalf("type inconnu %d : %v", typ, err)
		}
	}
	// Respawn : aucune valeur vérifiée au décodage (NetId quelconque, pose non finie).
	b, err := EncodeReliable(Respawn{NetId: 7, X: float32(math.NaN())})
	if err != nil {
		t.Fatalf("Respawn non fini : %v", err)
	}
	if m, err := DecodeReliable(b); err != nil || m.(Respawn).NetId != 7 {
		t.Fatalf("Respawn NetId 7 : %v %v", m, err)
	}
	// NpcAttack (v15) : aucune valeur vérifiée au décodage.
	b, err = EncodeReliable(NpcAttack{NetId: 7, Move: math.MaxUint32})
	if err != nil {
		t.Fatalf("NpcAttack : %v", err)
	}
	if m, err := DecodeReliable(b); err != nil || m.(NpcAttack) != (NpcAttack{NetId: 7, Move: math.MaxUint32}) {
		t.Fatalf("NpcAttack NetId 7 : %v %v", m, err)
	}
}

func TestWeaponBounds(t *testing.T) {
	max := Weapon{NetId: 0xffffffff, Name: strings.Repeat("w", MaxWeaponName)}
	b, err := EncodeReliable(max)
	if err != nil || len(b) != 261 {
		t.Fatalf("Weapon maximal : %d octets, %v", len(b), err)
	}
	if got, err := DecodeReliable(b); err != nil || got != max {
		t.Fatalf("Weapon maximal décodé : %v, %v", got, err)
	}
	if _, err := EncodeReliable(Weapon{Name: strings.Repeat("w", MaxWeaponName+1)}); !errors.Is(err, ErrOutOfBounds) {
		t.Fatalf("Weapon de 256 octets : %v", err)
	}
	raw := vector(t, "weapon.bin")
	for n := 0; n < len(raw); n++ {
		if _, err := DecodeReliable(raw[:n]); !errors.Is(err, ErrTruncated) {
			t.Fatalf("weapon.bin coupé à %d : %v", n, err)
		}
	}
	if _, err := DecodeReliable(append(append([]byte{}, raw...), 0)); !errors.Is(err, ErrTrailingBytes) {
		t.Fatalf("weapon.bin + 1 octet : %v", err)
	}
	if _, err := DecodeControl([]byte{2, 27}); !errors.Is(err, ErrUnknownType) {
		t.Fatalf("Weapon en contrôle : %v", err)
	}
}

func TestArmorBounds(t *testing.T) {
	max := Armor{NetId: 0xffffffff, Name: strings.Repeat("a", MaxArmorName)}
	b, err := EncodeReliable(max)
	if err != nil || len(b) != 261 {
		t.Fatalf("Armor maximal : %d octets, %v", len(b), err)
	}
	if got, err := DecodeReliable(b); err != nil || got != max {
		t.Fatalf("Armor maximal décodé : %v, %v", got, err)
	}
	if _, err := EncodeReliable(Armor{Name: strings.Repeat("a", MaxArmorName+1)}); !errors.Is(err, ErrOutOfBounds) {
		t.Fatalf("Armor de 256 octets : %v", err)
	}
	raw := vector(t, "armor.bin")
	for n := 0; n < len(raw); n++ {
		if _, err := DecodeReliable(raw[:n]); !errors.Is(err, ErrTruncated) {
			t.Fatalf("armor.bin coupé à %d : %v", n, err)
		}
	}
	if _, err := DecodeReliable(append(append([]byte{}, raw...), 0)); !errors.Is(err, ErrTrailingBytes) {
		t.Fatalf("armor.bin + 1 octet : %v", err)
	}
	if _, err := DecodeControl([]byte{2, 28}); !errors.Is(err, ErrUnknownType) {
		t.Fatalf("Armor en contrôle : %v", err)
	}
}

// v9 (story 3.3) : Spawn de taille variable, type inconnu refusé, snapshot multi-entités.
func TestSpawnNpcV9(t *testing.T) {
	raw := vector(t, "spawn_npc.bin")
	if len(raw) != SpawnNpcSize {
		t.Fatalf("spawn_npc.bin : %d octets", len(raw))
	}
	for n := 0; n < len(raw); n++ {
		if _, err := DecodeReliable(raw[:n]); !errors.Is(err, ErrTruncated) {
			t.Fatalf("coupé à %d : %v", n, err)
		}
	}
	if _, err := DecodeReliable(append(append([]byte{}, raw...), 0)); !errors.Is(err, ErrTrailingBytes) {
		t.Fatalf("+1 octet : %v", err)
	}
	if _, err := DecodeReliable(vector(t, "spawn_bad_type.bin")); !errors.Is(err, ErrBadValue) {
		t.Fatalf("spawn_bad_type.bin : %v", err)
	}
	for _, kind := range []uint8{2, 7, 200, 255} {
		if _, err := EncodeReliable(Spawn{NetId: 2, Kind: kind}); !errors.Is(err, ErrOutOfBounds) {
			t.Errorf("type %d encodé : %v", kind, err)
		}
		b := append([]byte{}, vector(t, "spawn.bin")...)
		b[5] = kind
		if _, err := DecodeReliable(append(b, make([]byte, GuidSize)...)); !errors.Is(err, ErrBadValue) {
			t.Errorf("type %d décodé : %v", kind, err)
		}
	}
	// Type 0 : GUID ni écrit ni lu.
	hero := Spawn{NetId: 2, Kind: SpawnTypeHero, X: 100, Y: -50.5, Z: 2000, Yaw: 1.5, GUID: [GuidSize]byte{1, 2, 3}}
	enc, err := EncodeReliable(hero)
	if err != nil || !bytes.Equal(enc, vector(t, "spawn.bin")) || len(enc) != SpawnHeroSize {
		t.Fatalf("Spawn{0} : % x (%v)", enc, err)
	}
	if _, err := DecodeReliable(append(enc, make([]byte, GuidSize)...)); !errors.Is(err, ErrTrailingBytes) {
		t.Fatalf("Spawn{0} + 16 octets : %v", err)
	}
}

func TestSnapshotMultiVector(t *testing.T) {
	raw := vector(t, "snapshot_multi.bin")
	want := Snapshot{Tick: 1234, Entities: []SnapshotEntity{
		{NetId: 1, X: 100, Y: -50.5, Z: 2000, Yaw: 1.5, HP: 75},
		{NetId: 1000, X: 150, Y: -50.5, Z: 2500, Yaw: -0.5, HP: 150},
		{NetId: 1001, X: -300, Y: 10, Z: 1800, Yaw: 3, HP: 65535},
	}}
	got, err := DecodeSnapshot(raw)
	if err != nil || got.Tick != want.Tick || len(got.Entities) != len(want.Entities) {
		t.Fatalf("snapshot_multi.bin : %+v, %v", got, err)
	}
	for i := range want.Entities {
		if got.Entities[i] != want.Entities[i] {
			t.Fatalf("entité %d : %+v, attendu %+v", i, got.Entities[i], want.Entities[i])
		}
	}
	enc, err := EncodeSnapshot(want)
	if err != nil || !bytes.Equal(enc, raw) {
		t.Fatalf("réencodage différent :\n% x\n% x (%v)", enc, raw, err)
	}
}

// Story 4.1 : DialogLine (v16), bornes du locuteur, du type et de l'échantillon.
func TestDialogLineBounds(t *testing.T) {
	raw := func(speaker uint32, typ uint8, sample string) []byte {
		b := []byte{30, byte(speaker), byte(speaker >> 8), byte(speaker >> 16), byte(speaker >> 24), typ, byte(len(sample))}
		return append(b, sample...)
	}
	good := "INFO_A_1_B_DE_%_C.wav"
	for _, m := range []DialogLine{{1004, 0, good}, {HostNetId, 3, good}, {GuestNetId, 3, good}, {FirstNpcNetId, 1, "a.wav"},
		{1004, 0, strings.Repeat("a", 124) + ".wav"}} {
		got, err := DecodeReliable(raw(m.Speaker, m.OutputType, m.Sample))
		if err != nil || got.(DialogLine) != m {
			t.Errorf("DialogLine %+v : %v %v", m, got, err)
		}
		if b, err := EncodeReliable(m); err != nil || !bytes.Equal(b, raw(m.Speaker, m.OutputType, m.Sample)) {
			t.Errorf("encodage %+v : %v", m, err)
		}
	}
	bad := []DialogLine{{0, 0, good}, {3, 0, good}, {999, 0, good}, {1004, 4, good}, {1004, 255, good},
		{1004, 0, ""}, {1004, 0, "../x.wav"}, {1004, 0, "..\\x.wav"}, {1004, 0, "dir/x.wav"}, {1004, 0, "INFO_A"}, {1004, 0, ".wav"}, {1004, 0, "a..wav"},
		{1004, 0, "INFO_A.WAV"}, {1004, 0, "INFO_A.wav "}, {1004, 0, "INFO A.wav"}, {1004, 0, "INFO_\xc3\xa9.wav"},
		{1004, 0, strings.Repeat("a", 125) + ".wav"}, {1004, 0, strings.Repeat("a", 251) + ".wav"}}
	for _, m := range bad {
		if _, err := DecodeReliable(raw(m.Speaker, m.OutputType, m.Sample)); !errors.Is(err, ErrBadValue) {
			t.Errorf("DialogLine %+v : %v, attendu ErrBadValue", m, err)
		}
		if _, err := EncodeReliable(m); err == nil {
			t.Errorf("DialogLine %+v encodé", m)
		}
	}
}

// Story 4.2 : DialogState (v17), NetId ≥ 1000 et actif 0 ou 1.
func TestDialogStateBounds(t *testing.T) {
	raw := func(npc uint32, active uint8) []byte {
		return []byte{31, byte(npc), byte(npc >> 8), byte(npc >> 16), byte(npc >> 24), active}
	}
	for _, m := range []DialogState{{1033, 1}, {FirstNpcNetId, 0}, {0xFFFFFFFF, 1}} {
		got, err := DecodeReliable(raw(m.NpcNetId, m.Active))
		if err != nil || got.(DialogState) != m {
			t.Errorf("DialogState %+v : %v %v", m, got, err)
		}
		if b, err := EncodeReliable(m); err != nil || !bytes.Equal(b, raw(m.NpcNetId, m.Active)) {
			t.Errorf("encodage %+v : %v", m, err)
		}
	}
	for _, m := range []DialogState{{0, 1}, {HostNetId, 1}, {GuestNetId, 0}, {999, 1}, {1033, 2}, {1033, 255}} {
		if _, err := DecodeReliable(raw(m.NpcNetId, m.Active)); !errors.Is(err, ErrBadValue) {
			t.Errorf("DialogState %+v : %v, attendu ErrBadValue", m, err)
		}
		if _, err := EncodeReliable(m); err == nil {
			t.Errorf("DialogState %+v encodé", m)
		}
	}
}
