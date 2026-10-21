package main

import (
	"bytes"
	"strconv"
	"strings"
	"testing"
)

// Même matrice que client/tests/test_equipment.cpp.
func TestParseEquipment(t *testing.T) {
	ok := []struct{ in, want string }{
		{"It_Armor_Don_Fighter , It_1H_Sword_Rusty", "It_Armor_Don_Fighter,It_1H_Sword_Rusty"},
		{"  \tA\t,B  ", "A,B"},
		{"\vA\f,\rB\n", "A,B"},
		{"", ""},
		{"   ", ""},
		{"A,a", "A,a"},
		{strings.Repeat("x", 64), strings.Repeat("x", 64)},
	}
	for _, c := range ok {
		names, err := parseEquipment(c.in)
		if err != nil || joinEquipment(names) != c.want {
			t.Errorf("%q : %v, %q (attendu %q)", c.in, err, joinEquipment(names), c.want)
		}
	}
	var sixteen []string
	for i := 0; i < 16; i++ {
		sixteen = append(sixteen, "Item_"+strconv.Itoa(i))
	}
	if names, err := parseEquipment(strings.Join(sixteen, ",")); err != nil || len(names) != 16 {
		t.Errorf("16 noms : %v", err)
	}
	a, b, c := strings.Repeat("a", 64), strings.Repeat("b", 64), strings.Repeat("c", 64)
	if names, err := parseEquipment(a + " , " + b + " , " + c + " , " + strings.Repeat("d", 60)); err != nil ||
		len(joinEquipment(names)) != 255 {
		t.Errorf("255 octets : %v", err)
	}
	bad := []string{
		"A\u00a0", "\u00a0A", "A,\u2003B", // blancs Unicode : non retirés, comme en C++
		"It-Sword", "A,,B", ",A", "A,", "A, ,B", "A B", "Épée", "A,B,A",
		strings.Repeat("x", 65),
		strings.Join(sixteen, ",") + ",Item_16",
		a + "," + b + "," + c + "," + strings.Repeat("d", 61),
	}
	for _, in := range bad {
		if names, err := parseEquipment(in); err == nil {
			t.Errorf("%q accepté : %q", in, names)
		}
	}
}

func TestEquipmentOption(t *testing.T) {
	var errOut bytes.Buffer
	cfg, code := parseConfig([]string{"--host", "10.0.0.7"}, &errOut)
	if code != parseOK || cfg.Hello.Equipment != "" {
		t.Fatalf("défaut : code %d, équipement %q", code, cfg.Hello.Equipment)
	}
	cfg, code = parseConfig([]string{"--host", "10.0.0.7", "--equipment", " It_Armor_Don_Fighter , It_1H_Sword "}, &errOut)
	if code != parseOK || cfg.Hello.Equipment != "It_Armor_Don_Fighter,It_1H_Sword" {
		t.Errorf("--equipment : code %d, équipement %q", code, cfg.Hello.Equipment)
	}
	for _, v := range []string{"It-Sword", "A,,B", "A,A"} {
		errOut.Reset()
		if _, code := parseConfig([]string{"--host", "10.0.0.7", "--equipment", v}, &errOut); code != exitError ||
			!strings.Contains(errOut.String(), "--equipment") {
			t.Errorf("--equipment %q : code %d, %s", v, code, errOut.String())
		}
	}
}
