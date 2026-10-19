// Package protocol est le codec du contrat binaire hôte ⇄ invité, version 6, écrit à partir
// de protocol/PROTOCOL.md (qui fait foi). Il suit le codec C++ client/src/core/protocol.cpp :
// mêmes bornes, mêmes erreurs, raisons inconnues conservées telles quelles.
//
// Cadrage :
//   - message de contrôle (datagramme UDP complet) = [u8 canal=2][u8 MsgType][charge] ;
//   - message fiable (unité confiée à KCP)        = [u8 MsgType][charge], sans octet de canal.
//
// Le décodage ne lit jamais hors du tampon et ne panique pas : toute entrée invalide donne
// une des erreurs sentinelles ci-dessous.
package protocol

import (
	"encoding/binary"
	"errors"
	"math"
)

const (
	ProtocolVersion   = 6
	MaxDatagram       = 1200 // octets, canal compris
	MaxModVersion     = 255
	MinNickname       = 1
	MaxNickname       = 32
	MaxEquipment      = 255
	FingerprintModule = 4 // Game, Engine, Script, SharedBase
)

// Channel est l'octet 0 de chaque datagramme UDP.
type Channel uint8

const (
	ChannelReliable Channel = 0 // segments KCP
	ChannelSnapshot Channel = 1 // snapshots (v4, AD-7)
	ChannelControl  Channel = 2 // Hello, Welcome, Reject
)

// MsgType : 22, 23 et 25 sont réservés et se décodent en ErrUnknownType.
type MsgType uint8

const (
	TypeHello       MsgType = 1
	TypeWelcome     MsgType = 2
	TypeReject      MsgType = 3
	TypeHeartbeat   MsgType = 16
	TypeSessionEnd  MsgType = 17
	TypeSpawn       MsgType = 20 // v3
	TypeDespawn     MsgType = 21 // v3
	TypeAniName     MsgType = 18 // v5
	TypeMotion      MsgType = 19 // v2
	TypeDeath       MsgType = 24 // v5
	TypeMotionParam MsgType = 26 // v6
)

// NetId (AD-6) et type de Spawn.
const (
	HostNetId     uint32 = 1
	GuestNetId    uint32 = 2
	SpawnTypeHero uint8  = 0
)

// Raisons de Reject.
const (
	RejectProtocol        uint8 = 1
	RejectModVersion      uint8 = 2
	RejectGameFingerprint uint8 = 3
	RejectSessionFull     uint8 = 4
	RejectBadHello        uint8 = 5
)

// Raisons de SessionEnd.
const (
	EndPlayerLeft    uint8 = 1
	EndWorldEnded    uint8 = 2
	EndHostDied      uint8 = 3
	EndInternalError uint8 = 4
)

// Index dans Hello.Fingerprint.
const (
	ModGame = iota
	ModEngine
	ModScript
	ModSharedBase
)

// Erreurs de décodage (PROTOCOL.md §5), mêmes cas que mp::protocol::DecodeError.
var (
	ErrTruncated          = errors.New("message tronqué")
	ErrUnknownChannel     = errors.New("canal inconnu")
	ErrUnknownType        = errors.New("type de message inconnu")
	ErrUnsupportedVersion = errors.New("version du protocole non prise en charge")
	ErrBadLength          = errors.New("longueur de texte hors bornes")
	ErrTrailingBytes      = errors.New("octets en trop")
	ErrBadValue           = errors.New("champ hors domaine") // snapshot de plus de 54 entités
)

// ErrOutOfBounds : refus de l'encodeur (texte hors bornes, version ≠ ProtocolVersion, > 1200 octets,
// ou message absent de ce canal). Rien n'est produit.
var ErrOutOfBounds = errors.New("message hors bornes, non encodé")

// Fingerprint : empreinte PE d'un module du jeu.
type Fingerprint struct {
	TimeDateStamp uint32
	SizeOfImage   uint32
}

// Message est l'un de Hello, Welcome, Reject (contrôle) ou Heartbeat, SessionEnd, Spawn,
// Despawn (fiable).
type Message interface {
	Type() MsgType
}

type Hello struct {
	ProtocolVersion uint16
	ModVersion      string // 0 à 255 octets
	Fingerprint     [FingerprintModule]Fingerprint
	Nickname        string // 1 à 32 octets
	Equipment       string // 0 à 255 octets
}

type Welcome struct {
	Conv    uint32
	X, Y, Z float32
	Yaw     float32
}

type Reject struct{ Reason uint8 }

type Heartbeat struct{}

type SessionEnd struct{ Reason uint8 }

// Spawn : apparition d'une entité ; Kind (champ `type` du §4) est gardé brut (0 = héros).
type Spawn struct {
	NetId   uint32
	Kind    uint8
	X, Y, Z float32
	Yaw     float32
}

type Despawn struct{ NetId uint32 }

// AniName : nom d'animation interné (1 à 255 octets), avant son premier Motion.
type AniName struct {
	AniId uint16
	Name  string
}

// Motion : un PlayMotion (AD-8). Les 7 scalaires sont les octets du descripteur lus par le
// moteur ; sans MotionFlagDesc ils valent 0.
type Motion struct {
	NetId uint32
	Ms    uint32
	Kind  uint8 // eEMotionType, 0 à 7 (champ `type` du §4)
	AniId uint16
	Flags uint8
	D00   float32
	D04   float32
	D08   uint32
	D0C   float32
	D10   uint32
	D14   float32
	D18   uint32
}

type Death struct{ NetId uint32 }

// MotionParam : réglage de locomotion (v6). Nature 0 = SetTargetWeight(type, valeur, durée),
// 1 = SetPlaySpeed(type, valeur).
type MotionParam struct {
	NetId uint32
	Ms    uint32
	Kind  uint8 // type de motion, 0 à 7 (champ `type` du §4)
	Param uint8 // nature
	Value float32
	Time  float32
}

const (
	ParamTargetWeight = 0
	ParamPlaySpeed    = 1
)

func finite32(v float32) bool { return !math.IsNaN(float64(v)) && !math.IsInf(float64(v), 0) }

const (
	MaxMotionParamValue = 100 // poids ou vitesse : [0, 100]
	MaxMotionParamTime  = 60  // durée de fondu : [0, 60] s ; 0 pour une vitesse
)

func validMotionParam(m MotionParam) bool {
	if m.Kind >= MotionTypeCount || m.Param > ParamPlaySpeed || !finite32(m.Value) || !finite32(m.Time) {
		return false
	}
	if m.Value < 0 || m.Value > MaxMotionParamValue || m.Time < 0 || m.Time > MaxMotionParamTime {
		return false
	}
	return m.Param != ParamPlaySpeed || m.Time == 0
}

const (
	MinAniName      = 1
	MaxAniName      = 255
	MotionTypeCount = 8
	MotionFlagPlay  = 0x01
	MotionFlagDesc  = 0x02
)

func validMotion(m Motion) bool {
	if m.Kind >= MotionTypeCount || m.Flags&^(MotionFlagPlay|MotionFlagDesc) != 0 {
		return false
	}
	return m.Flags&MotionFlagDesc != 0 || (math.Float32bits(m.D00)|math.Float32bits(m.D04)|m.D08|
		math.Float32bits(m.D0C)|m.D10|math.Float32bits(m.D14)|m.D18) == 0
}

func (Hello) Type() MsgType       { return TypeHello }
func (Welcome) Type() MsgType     { return TypeWelcome }
func (Reject) Type() MsgType      { return TypeReject }
func (Heartbeat) Type() MsgType   { return TypeHeartbeat }
func (SessionEnd) Type() MsgType  { return TypeSessionEnd }
func (Spawn) Type() MsgType       { return TypeSpawn }
func (Despawn) Type() MsgType     { return TypeDespawn }
func (AniName) Type() MsgType     { return TypeAniName }
func (Motion) Type() MsgType      { return TypeMotion }
func (Death) Type() MsgType       { return TypeDeath }
func (MotionParam) Type() MsgType { return TypeMotionParam }

// --- Encodage ------------------------------------------------------------------------

type writer struct{ b []byte }

func (w *writer) u8(v uint8)   { w.b = append(w.b, v) }
func (w *writer) u16(v uint16) { w.b = binary.LittleEndian.AppendUint16(w.b, v) }
func (w *writer) u32(v uint32) { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *writer) f32(v float32) {
	w.u32(math.Float32bits(v))
}
func (w *writer) str8(s string) {
	w.u8(uint8(len(s)))
	w.b = append(w.b, s...)
}

func inBounds(s string, min, max int) bool { return len(s) >= min && len(s) <= max }

func validHello(m Hello) bool {
	return m.ProtocolVersion == ProtocolVersion && inBounds(m.ModVersion, 0, MaxModVersion) &&
		inBounds(m.Nickname, MinNickname, MaxNickname) && inBounds(m.Equipment, 0, MaxEquipment)
}

func (w *writer) message(m Message) {
	w.u8(uint8(m.Type()))
	switch m := m.(type) {
	case Hello:
		w.u16(m.ProtocolVersion)
		w.str8(m.ModVersion)
		for _, fp := range m.Fingerprint {
			w.u32(fp.TimeDateStamp)
			w.u32(fp.SizeOfImage)
		}
		w.str8(m.Nickname)
		w.str8(m.Equipment)
	case Welcome:
		w.u32(m.Conv)
		w.f32(m.X)
		w.f32(m.Y)
		w.f32(m.Z)
		w.f32(m.Yaw)
	case Reject:
		w.u8(m.Reason)
	case Heartbeat:
	case SessionEnd:
		w.u8(m.Reason)
	case Spawn:
		w.u32(m.NetId)
		w.u8(m.Kind)
		w.f32(m.X)
		w.f32(m.Y)
		w.f32(m.Z)
		w.f32(m.Yaw)
	case Despawn:
		w.u32(m.NetId)
	case AniName:
		w.u16(m.AniId)
		w.str8(m.Name)
	case Motion:
		w.u32(m.NetId)
		w.u32(m.Ms)
		w.u8(m.Kind)
		w.u16(m.AniId)
		w.u8(m.Flags)
		w.f32(m.D00)
		w.f32(m.D04)
		w.u32(m.D08)
		w.f32(m.D0C)
		w.u32(m.D10)
		w.f32(m.D14)
		w.u32(m.D18)
	case Death:
		w.u32(m.NetId)
	case MotionParam:
		w.u32(m.NetId)
		w.u32(m.Ms)
		w.u8(m.Kind)
		w.u8(m.Param)
		w.f32(m.Value)
		w.f32(m.Time)
	}
}

// EncodeControl rend le datagramme complet [canal=2][MsgType][charge] d'un Hello, Welcome
// ou Reject.
func EncodeControl(m Message) ([]byte, error) {
	switch m := m.(type) {
	case Hello:
		if !validHello(m) {
			return nil, ErrOutOfBounds
		}
	case Welcome, Reject:
	default:
		return nil, ErrOutOfBounds
	}
	w := writer{b: []byte{uint8(ChannelControl)}}
	w.message(m)
	if len(w.b) > MaxDatagram {
		return nil, ErrOutOfBounds
	}
	return w.b, nil
}

// EncodeReliable rend le message [MsgType][charge] d'un message fiable (Heartbeat, SessionEnd,
// Spawn, Despawn, AniName, Motion, Death, MotionParam), sans octet de canal.
func EncodeReliable(m Message) ([]byte, error) {
	switch m := m.(type) {
	case AniName:
		if !inBounds(m.Name, MinAniName, MaxAniName) {
			return nil, ErrOutOfBounds
		}
	case Motion:
		if !validMotion(m) {
			return nil, ErrOutOfBounds
		}
	case MotionParam:
		if !validMotionParam(m) {
			return nil, ErrOutOfBounds
		}
	case Heartbeat, SessionEnd, Spawn, Despawn, Death:
	default:
		return nil, ErrOutOfBounds
	}
	var w writer
	w.message(m)
	return w.b, nil
}

// --- Décodage ------------------------------------------------------------------------

// reader : curseur borné. La première erreur (troncature ou longueur hors bornes) est
// collante ; les lectures suivantes rendent 0.
type reader struct {
	b   []byte
	pos int
	err error
}

func (r *reader) take(n int) []byte {
	if r.err != nil {
		return nil
	}
	if n > len(r.b)-r.pos {
		r.err = ErrTruncated
		return nil
	}
	p := r.b[r.pos : r.pos+n]
	r.pos += n
	return p
}

func (r *reader) u8() uint8 {
	if p := r.take(1); p != nil {
		return p[0]
	}
	return 0
}

func (r *reader) u16() uint16 {
	if p := r.take(2); p != nil {
		return binary.LittleEndian.Uint16(p)
	}
	return 0
}

func (r *reader) u32() uint32 {
	if p := r.take(4); p != nil {
		return binary.LittleEndian.Uint32(p)
	}
	return 0
}

func (r *reader) f32() float32 { return math.Float32frombits(r.u32()) }

func (r *reader) str8(min, max int) string {
	n := int(r.u8())
	if r.err != nil {
		return ""
	}
	if n < min || n > max {
		r.err = ErrBadLength
		return ""
	}
	return string(r.take(n))
}

func (r *reader) finish() error {
	if r.err != nil {
		return r.err
	}
	if r.pos != len(r.b) {
		return ErrTrailingBytes
	}
	return nil
}

func readHello(r *reader) (Message, error) {
	var m Hello
	m.ProtocolVersion = r.u16()
	if r.err != nil {
		return nil, r.err
	}
	// La suite d'un Hello d'une autre version n'est pas lisible : on s'arrête là.
	if m.ProtocolVersion != ProtocolVersion {
		return nil, ErrUnsupportedVersion
	}
	m.ModVersion = r.str8(0, MaxModVersion)
	for i := range m.Fingerprint {
		m.Fingerprint[i].TimeDateStamp = r.u32()
		m.Fingerprint[i].SizeOfImage = r.u32()
	}
	m.Nickname = r.str8(MinNickname, MaxNickname)
	m.Equipment = r.str8(0, MaxEquipment)
	if err := r.finish(); err != nil {
		return nil, err
	}
	return m, nil
}

func readWelcome(r *reader) (Message, error) {
	m := Welcome{Conv: r.u32(), X: r.f32(), Y: r.f32(), Z: r.f32(), Yaw: r.f32()}
	if err := r.finish(); err != nil {
		return nil, err
	}
	return m, nil
}

func readReason(r *reader) (uint8, error) {
	v := r.u8()
	return v, r.finish()
}

// DecodeControl décode un datagramme de contrôle complet (octet de canal compris).
func DecodeControl(b []byte) (Message, error) {
	r := reader{b: b}
	channel := r.u8()
	if r.err != nil {
		return nil, r.err
	}
	if Channel(channel) != ChannelControl {
		return nil, ErrUnknownChannel
	}
	t := r.u8()
	if r.err != nil {
		return nil, r.err
	}
	switch MsgType(t) {
	case TypeHello:
		return readHello(&r)
	case TypeWelcome:
		return readWelcome(&r)
	case TypeReject:
		v, err := readReason(&r)
		if err != nil {
			return nil, err
		}
		return Reject{Reason: v}, nil
	}
	return nil, ErrUnknownType
}

// DecodeReliable décode un message fiable (sans octet de canal).
func DecodeReliable(b []byte) (Message, error) {
	r := reader{b: b}
	t := r.u8()
	if r.err != nil {
		return nil, r.err
	}
	switch MsgType(t) {
	case TypeHeartbeat:
		if err := r.finish(); err != nil {
			return nil, err
		}
		return Heartbeat{}, nil
	case TypeSessionEnd:
		v, err := readReason(&r)
		if err != nil {
			return nil, err
		}
		return SessionEnd{Reason: v}, nil
	case TypeSpawn:
		m := Spawn{NetId: r.u32(), Kind: r.u8(), X: r.f32(), Y: r.f32(), Z: r.f32(), Yaw: r.f32()}
		if err := r.finish(); err != nil {
			return nil, err
		}
		return m, nil
	case TypeDespawn:
		m := Despawn{NetId: r.u32()}
		if err := r.finish(); err != nil {
			return nil, err
		}
		return m, nil
	case TypeAniName:
		m := AniName{AniId: r.u16()}
		m.Name = r.str8(MinAniName, MaxAniName)
		if err := r.finish(); err != nil {
			return nil, err
		}
		return m, nil
	case TypeMotion:
		m := Motion{NetId: r.u32(), Ms: r.u32(), Kind: r.u8(), AniId: r.u16(), Flags: r.u8(), D00: r.f32(), D04: r.f32(),
			D08: r.u32(), D0C: r.f32(), D10: r.u32(), D14: r.f32(), D18: r.u32()}
		if err := r.finish(); err != nil {
			return nil, err
		}
		if !validMotion(m) {
			return nil, ErrBadValue
		}
		return m, nil
	case TypeMotionParam:
		m := MotionParam{NetId: r.u32(), Ms: r.u32(), Kind: r.u8(), Param: r.u8(), Value: r.f32(), Time: r.f32()}
		if err := r.finish(); err != nil {
			return nil, err
		}
		if !validMotionParam(m) {
			return nil, ErrBadValue
		}
		return m, nil
	case TypeDeath:
		m := Death{NetId: r.u32()}
		if err := r.finish(); err != nil {
			return nil, err
		}
		return m, nil
	}
	return nil, ErrUnknownType
}

// --- Snapshot (canal 1, v4) -----------------------------------------------------------

const (
	SnapshotTickMs      = 50 // 20 Hz
	SnapshotEntitySize  = 22
	MaxSnapshotEntities = 54 // (1200 − 6) / 22
)

// SnapshotEntity : une entité d'un snapshot (AD-7).
type SnapshotEntity struct {
	NetId   uint32
	X, Y, Z float32
	Yaw     float32
	HP      uint16
}

// Snapshot : état complet, daté par le tick de l'émetteur (50 ms depuis son entrée en session).
type Snapshot struct {
	Tick     uint32
	Entities []SnapshotEntity
}

// EncodeSnapshot rend le datagramme complet [canal=1][u32 tick][u8 nombre][entités].
func EncodeSnapshot(s Snapshot) ([]byte, error) {
	if len(s.Entities) > MaxSnapshotEntities {
		return nil, ErrOutOfBounds
	}
	w := writer{b: make([]byte, 0, 6+SnapshotEntitySize*len(s.Entities))}
	w.u8(uint8(ChannelSnapshot))
	w.u32(s.Tick)
	w.u8(uint8(len(s.Entities)))
	for _, e := range s.Entities {
		w.u32(e.NetId)
		w.f32(e.X)
		w.f32(e.Y)
		w.f32(e.Z)
		w.f32(e.Yaw)
		w.u16(e.HP)
	}
	return w.b, nil
}

// DecodeSnapshot décode un datagramme de canal 1 complet.
func DecodeSnapshot(b []byte) (Snapshot, error) {
	r := reader{b: b}
	ch := r.u8()
	if r.err != nil {
		return Snapshot{}, r.err
	}
	if Channel(ch) != ChannelSnapshot {
		return Snapshot{}, ErrUnknownChannel
	}
	s := Snapshot{Tick: r.u32()}
	n := int(r.u8())
	if r.err != nil {
		return Snapshot{}, r.err
	}
	if n > MaxSnapshotEntities {
		return Snapshot{}, ErrBadValue
	}
	s.Entities = make([]SnapshotEntity, 0, n)
	for i := 0; i < n; i++ {
		s.Entities = append(s.Entities, SnapshotEntity{NetId: r.u32(), X: r.f32(), Y: r.f32(), Z: r.f32(), Yaw: r.f32(), HP: r.u16()})
	}
	if err := r.finish(); err != nil {
		return Snapshot{}, err
	}
	return s, nil
}

// PeekHelloVersion lit la version d'un Hello de n'importe quelle version : seuls
// [canal=2][MsgType=1][u16 version] sont figés pour toujours (PROTOCOL.md §6).
// false si le datagramme n'est pas un Hello ou est trop court.
func PeekHelloVersion(b []byte) (uint16, bool) {
	if len(b) < 4 || Channel(b[0]) != ChannelControl || MsgType(b[1]) != TypeHello {
		return 0, false
	}
	return binary.LittleEndian.Uint16(b[2:4]), true
}

// RejectReasonName : nom lisible d'une raison de Reject, « inconnue » sinon.
func RejectReasonName(reason uint8) string {
	switch reason {
	case RejectProtocol:
		return "version du protocole différente"
	case RejectModVersion:
		return "version du mod différente"
	case RejectGameFingerprint:
		return "empreinte du jeu différente"
	case RejectSessionFull:
		return "session pleine"
	case RejectBadHello:
		return "Hello invalide"
	}
	return "inconnue"
}

// EndReasonName : nom lisible d'une raison de SessionEnd, « inconnue » sinon.
func EndReasonName(reason uint8) string {
	switch reason {
	case EndPlayerLeft:
		return "départ du joueur"
	case EndWorldEnded:
		return "fin de monde"
	case EndHostDied:
		return "mort de l'hôte"
	case EndInternalError:
		return "erreur interne"
	}
	return "inconnue"
}
