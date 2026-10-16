// Package protocol est le codec du contrat binaire hôte ⇄ invité, version 1, écrit à partir
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
	ProtocolVersion   = 1
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
	ChannelSnapshot Channel = 1 // Epic 2, non défini en v1
	ChannelControl  Channel = 2 // Hello, Welcome, Reject
)

// MsgType : 18 à 25 sont réservés et se décodent en ErrUnknownType.
type MsgType uint8

const (
	TypeHello      MsgType = 1
	TypeWelcome    MsgType = 2
	TypeReject     MsgType = 3
	TypeHeartbeat  MsgType = 16
	TypeSessionEnd MsgType = 17
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
)

// ErrOutOfBounds : refus de l'encodeur (texte hors bornes, version ≠ 1, > 1200 octets,
// ou message absent de ce canal). Rien n'est produit.
var ErrOutOfBounds = errors.New("message hors bornes, non encodé")

// Fingerprint : empreinte PE d'un module du jeu.
type Fingerprint struct {
	TimeDateStamp uint32
	SizeOfImage   uint32
}

// Message est l'un de Hello, Welcome, Reject (contrôle) ou Heartbeat, SessionEnd (fiable).
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

func (Hello) Type() MsgType      { return TypeHello }
func (Welcome) Type() MsgType    { return TypeWelcome }
func (Reject) Type() MsgType     { return TypeReject }
func (Heartbeat) Type() MsgType  { return TypeHeartbeat }
func (SessionEnd) Type() MsgType { return TypeSessionEnd }

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

// EncodeReliable rend le message [MsgType][charge] d'un Heartbeat ou SessionEnd (sans canal).
func EncodeReliable(m Message) ([]byte, error) {
	switch m.(type) {
	case Heartbeat, SessionEnd:
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
	}
	return nil, ErrUnknownType
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
