// Package kcp enveloppe, via cgo, le même ikcp.c que la DLL (client/third_party/kcp/) avec
// les mêmes réglages : ikcp_nodelay(1,10,2,1), ikcp_setmtu(1199). Chaque segment produit
// par KCP sort préfixé de l'octet de canal 0, soit un datagramme d'au plus 1200 octets.
//
// Un KCP n'est pas sûr entre goroutines : l'appelant le garde sur une seule goroutine.
package kcp

/*
#cgo CFLAGS: -I${SRCDIR}/../../../client/third_party/kcp -w
#include <stdint.h>
#include "ikcp.h"
ikcpcb *fg_create(IUINT32 conv, uintptr_t handle);
*/
import "C"

import (
	"errors"
	"runtime/cgo"
	"unsafe"
)

const (
	Mtu       = 1199 // segment KCP ; + octet de canal = 1200
	Overhead  = 24   // en-tête d'un segment KCP (IKCP_OVERHEAD)
	channelID = 0    // canal fiable (PROTOCOL.md §2)
)

var (
	ErrCreate = errors.New("kcp : création impossible")
	ErrSend   = errors.New("kcp : envoi refusé")
	ErrInput  = errors.New("kcp : segment refusé")
	ErrClosed = errors.New("kcp : déjà libéré")
)

// KCP est un contrôle de session KCP ouvert.
type KCP struct {
	c      *C.ikcpcb
	handle cgo.Handle
	output func(datagram []byte)
}

// New crée un KCP pour `conv`. `output` reçoit chaque datagramme à envoyer
// ([0][segment KCP]) ; il est appelé pendant Update/Flush, sur la goroutine appelante,
// et le tampon lui appartient.
func New(conv uint32, output func(datagram []byte)) (*KCP, error) {
	k := &KCP{output: output}
	k.handle = cgo.NewHandle(k)
	k.c = C.fg_create(C.IUINT32(conv), C.uintptr_t(k.handle))
	if k.c == nil {
		k.handle.Delete()
		return nil, ErrCreate
	}
	return k, nil
}

//export fgKcpOutput
func fgKcpOutput(handle C.uintptr_t, buf *C.char, n C.int) C.int {
	k, ok := cgo.Handle(handle).Value().(*KCP)
	if !ok || n < 0 || int(n) > Mtu {
		return -1
	}
	datagram := make([]byte, 1+int(n))
	datagram[0] = channelID
	copy(datagram[1:], C.GoBytes(unsafe.Pointer(buf), n))
	if k.output != nil {
		k.output(datagram)
	}
	return 0
}

// Release libère le KCP ; les segments non acquittés sont perdus.
func (k *KCP) Release() {
	if k.c == nil {
		return
	}
	C.ikcp_release(k.c)
	k.c = nil
	k.handle.Delete()
}

// Send confie un message ([MsgType][charge]) à KCP.
func (k *KCP) Send(msg []byte) error {
	if k.c == nil {
		return ErrClosed
	}
	if len(msg) == 0 || C.ikcp_send(k.c, (*C.char)(unsafe.Pointer(&msg[0])), C.int(len(msg))) < 0 {
		return ErrSend
	}
	return nil
}

// Input donne à KCP un segment reçu, sans l'octet de canal.
func (k *KCP) Input(segment []byte) error {
	if k.c == nil {
		return ErrClosed
	}
	if len(segment) == 0 || C.ikcp_input(k.c, (*C.char)(unsafe.Pointer(&segment[0])), C.long(len(segment))) < 0 {
		return ErrInput
	}
	return nil
}

// Update fait avancer KCP à `nowMs` (horloge monotone, en millisecondes).
func (k *KCP) Update(nowMs uint32) {
	if k.c != nil {
		C.ikcp_update(k.c, C.IUINT32(nowMs))
	}
}

// Check rend l'instant (ms) du prochain Update utile.
func (k *KCP) Check(nowMs uint32) uint32 {
	if k.c == nil {
		return nowMs
	}
	return uint32(C.ikcp_check(k.c, C.IUINT32(nowMs)))
}

// Flush envoie tout de suite les segments et acquittements en attente.
func (k *KCP) Flush() {
	if k.c != nil {
		C.ikcp_flush(k.c)
	}
}

// Recv rend le prochain message complet, ou false s'il n'y en a pas.
func (k *KCP) Recv() ([]byte, bool) {
	if k.c == nil {
		return nil, false
	}
	size := C.ikcp_peeksize(k.c)
	if size < 0 {
		return nil, false
	}
	buf := make([]byte, int(size)+1) // +1 : jamais de &buf[0] sur une tranche vide
	n := C.ikcp_recv(k.c, (*C.char)(unsafe.Pointer(&buf[0])), size)
	if n < 0 {
		return nil, false
	}
	return buf[:int(n)], true
}

// WaitSnd : segments envoyés ou en file, pas encore acquittés.
func (k *KCP) WaitSnd() int {
	if k.c == nil {
		return 0
	}
	return int(C.ikcp_waitsnd(k.c))
}

// SegmentConv lit le conv d'un datagramme de canal 0 ([0][segment]) avec ikcp_getconv.
// false si ce n'est pas un datagramme de canal 0 ou si le segment est trop court.
func SegmentConv(datagram []byte) (uint32, bool) {
	if len(datagram) < 1+Overhead || datagram[0] != channelID {
		return 0, false
	}
	return uint32(C.ikcp_getconv(unsafe.Pointer(&datagram[1]))), true
}
