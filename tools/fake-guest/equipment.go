package main

import (
	"fmt"
	"strings"
)

// Format de l'équipement de l'invité (story 3.2b, protocol/PROTOCOL.md §4 et §8), même règle que
// client/src/core/equipment.cpp : noms séparés par des virgules, espaces tolérés autour de chaque
// nom ; un nom compte 1 à 64 octets [A-Za-z0-9_] ; 16 noms au plus, sans doublon ; forme
// canonique (« A,B ») de 255 octets au plus. Vide = aucun équipement.
const (
	maxEquipmentNames     = 16
	maxEquipmentName      = 64
	maxEquipmentCanonical = 255
)

// asciiSpace : mêmes blancs que std::isspace (locale C) côté C++.
const asciiSpace = " \t\r\n\v\f"

// parseEquipment rend les noms dans l'ordre (nil si aucun), ou une erreur.
func parseEquipment(text string) ([]string, error) {
	text = strings.Trim(text, asciiSpace)
	if text == "" {
		return nil, nil
	}
	var names []string
	canonical := 0
	for i, part := range strings.Split(text, ",") {
		name := strings.Trim(part, asciiSpace)
		if name == "" {
			return nil, fmt.Errorf("nom %d vide (virgule en trop ?)", i+1)
		}
		if len(name) > maxEquipmentName {
			return nil, fmt.Errorf("nom %d trop long (%d octets, %d au plus)", i+1, len(name), maxEquipmentName)
		}
		for j := 0; j < len(name); j++ {
			c := name[j]
			if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
				return nil, fmt.Errorf("nom %q : caractères autorisés A-Z, a-z, 0-9 et _", name)
			}
		}
		for _, seen := range names {
			if seen == name {
				return nil, fmt.Errorf("nom %q en double", name)
			}
		}
		if len(names) == maxEquipmentNames {
			return nil, fmt.Errorf("%d noms au plus", maxEquipmentNames)
		}
		canonical += len(name)
		if len(names) > 0 {
			canonical++
		}
		if canonical > maxEquipmentCanonical {
			return nil, fmt.Errorf("liste trop longue (%d octets au plus, sans les espaces)", maxEquipmentCanonical)
		}
		names = append(names, name)
	}
	return names, nil
}

// joinEquipment rend la forme canonique.
func joinEquipment(names []string) string { return strings.Join(names, ",") }
