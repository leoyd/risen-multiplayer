package kcp

import (
	"bytes"
	"fmt"
	"math/rand"
	"testing"
)

// Deux KCP reliés en mémoire, 20 % des datagrammes jetés dans chaque sens : tous les
// messages arrivent, dans l'ordre, et aucun datagramme ne dépasse 1200 octets.
func TestLossyExchange(t *testing.T) {
	const conv = 0xC0FFEE
	rng := rand.New(rand.NewSource(42))
	var toB, toA [][]byte
	maxSize := 0
	lossy := func(queue *[][]byte) func([]byte) {
		return func(d []byte) {
			if len(d) > maxSize {
				maxSize = len(d)
			}
			if d[0] != 0 {
				t.Fatalf("octet de canal %d", d[0])
			}
			if c, ok := SegmentConv(d); !ok || c != conv {
				t.Fatalf("conv %x %v", c, ok)
			}
			if rng.Intn(100) < 20 {
				return
			}
			*queue = append(*queue, d)
		}
	}
	a, err := New(conv, lossy(&toB))
	if err != nil {
		t.Fatal(err)
	}
	defer a.Release()
	b, err := New(conv, lossy(&toA))
	if err != nil {
		t.Fatal(err)
	}
	defer b.Release()

	const count = 200
	var want [][]byte
	for i := 0; i < count; i++ {
		msg := []byte(fmt.Sprintf("message %d", i))
		if i%50 == 7 {
			msg = bytes.Repeat([]byte{byte(i)}, 3000) // fragmenté en plusieurs segments
		}
		want = append(want, msg)
		if err := a.Send(msg); err != nil {
			t.Fatal(err)
		}
	}
	var got [][]byte
	for now := uint32(0); now < 60000 && len(got) < count; now += 10 {
		a.Update(now)
		b.Update(now)
		for _, d := range toB {
			if err := b.Input(d[1:]); err != nil {
				t.Fatal(err)
			}
		}
		toB = nil
		for _, d := range toA {
			if err := a.Input(d[1:]); err != nil {
				t.Fatal(err)
			}
		}
		toA = nil
		for {
			m, ok := b.Recv()
			if !ok {
				break
			}
			got = append(got, m)
		}
	}
	if len(got) != count {
		t.Fatalf("%d messages reçus sur %d", len(got), count)
	}
	for i := range want {
		if !bytes.Equal(got[i], want[i]) {
			t.Fatalf("message %d différent", i)
		}
	}
	if maxSize > 1200 {
		t.Fatalf("datagramme de %d octets", maxSize)
	}
}

func TestReleasedAndInvalid(t *testing.T) {
	k, err := New(1, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := k.Send(nil); err != ErrSend {
		t.Fatalf("envoi vide : %v", err)
	}
	if err := k.Input([]byte{1, 2, 3}); err != ErrInput {
		t.Fatalf("segment court : %v", err)
	}
	k.Release()
	k.Release()
	if err := k.Send([]byte{16}); err != ErrClosed {
		t.Fatalf("après libération : %v", err)
	}
	if _, ok := SegmentConv([]byte{2, 0}); ok {
		t.Fatal("conv d'un datagramme de contrôle")
	}
}
