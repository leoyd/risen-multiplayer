// fake-guest : faux invité de session Risen Multiplayer, pour tester l'hôte (la DLL) sans
// second Risen. Outil de test, jamais livré. Il suit protocol/PROTOCOL.md (§8 pour la session).
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"os/signal"
	"syscall"

	"risen-multiplayer/tools/fake-guest/protocol"
)

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}

const usageText = `Usage : fake-guest --host <ipv4> [options]

Faux invité Risen Multiplayer (protocole v8, protocol/PROTOCOL.md §8).
Envoie Hello toutes les 500 ms (abandon après 10 s), puis tient la session :
Heartbeat à 1 Hz, timeout après 5 s sans datagramme de l'hôte.

Options :
`

const exitCodesText = `
Fautes : --drop, --delay, --jitter, --quit, --kill, --dup-hello, --bad-version (cumulables, sauf --quit/--kill).
Trajectoire : --scenario <fichier> (snapshots à 20 Hz, gestes horodatés ; sans lui, pose fixe),
ou --echo (l'avatar refait les gestes, les déplacements, l'arme et l'armure du joueur hôte, 150 ms plus tard).
Équipement : --equipment <A,B> part dans Hello (même format que guest_equipment).
Arme : --weapon <nom> part en Weapon juste après le Spawn (« - » : arme rangée) ; directive de
scénario « weapon [instant s] <nom|-> ».
Armure : --armor <nom> part en Armor juste après le Weapon (« - » : sans armure ; vide : pas
d'Armor) ; directive de scénario « armor [instant s] <nom|-> ».
--duration se compte à partir de l'entrée en session ; Ctrl-C vaut --quit.

Codes de sortie :
  0  fin normale (--quit, --kill, Ctrl-C ou SessionEnd reçu de l'hôte)
  1  erreur d'usage ou de socket
  2  Reject reçu de l'hôte
  3  hôte injoignable (aucune réponse en 10 s)
  4  timeout (5 s sans datagramme de l'hôte en session)
`

// run : ligne de commande complète, rend le code de sortie.
func run(args []string, stdout, stderr io.Writer) int {
	cfg, code := parseConfig(args, stderr)
	if code != parseOK {
		return code
	}

	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
	if err != nil {
		fmt.Fprintf(stderr, "fake-guest : socket : %v\n", err)
		return exitError
	}
	defer conn.Close()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return runSession(ctx, conn, cfg, stdout).Code
}

// parseOK : parseConfig a produit une config, la session peut démarrer.
const parseOK = -1

// parseConfig lit les options et rend la config de session (délais du §8), ou un code de
// sortie (exitOK pour --help, exitError pour une erreur d'usage) écrit sur `stderr`.
func parseConfig(args []string, stderr io.Writer) (sessionConfig, int) {
	fs := flag.NewFlagSet("fake-guest", flag.ContinueOnError)
	fs.SetOutput(stderr)
	host := fs.String("host", "", "adresse IPv4 de l'hôte (obligatoire)")
	port := fs.Uint("port", 27960, "port UDP de l'hôte")
	nickname := fs.String("nickname", "Invite", "pseudo envoyé dans Hello (1 à 32 octets)")
	modVersion := fs.String("mod-version", "0.1.0", "version du mod envoyée dans Hello (0 à 255 octets)")
	duration := fs.Duration("duration", 0, "durée de la session, ex. 30s (0 = jusqu'à Ctrl-C)")
	drop := fs.Int("drop", 0, "pourcentage de datagrammes sortants jetés (0 à 100)")
	seed := fs.Int64("seed", 1, "graine des pertes de --drop")
	quit := fs.Bool("quit", false, "à la fin de --duration, envoie SessionEnd{1} puis sort (défaut)")
	kill := fs.Bool("kill", false, "à la fin de --duration, sort sans rien envoyer")
	dupHello := fs.Bool("dup-hello", false, "chaque Hello part deux fois, et un Hello de plus part en session, après le premier heartbeat acquitté")
	scenarioPath := fs.String("scenario", "", "fichier de trajectoire des snapshots (cercle, aller-retour, pause ; voir scenarios/)")
	echo := fs.Bool("echo", false, "renvoie les snapshots et les Motion de l'hôte comme les siens (l'avatar imite le joueur)")
	delay := fs.Duration("delay", 0, "latence ajoutée à chaque datagramme sortant, ex. 100ms")
	jitter := fs.Duration("jitter", 0, "gigue ± ajoutée à --delay, ex. 20ms")
	badVersion := fs.Bool("bad-version", false, "envoie le Hello de hello_future.bin (version 0xFFFF) au lieu du Hello courant")
	equipment := fs.String("equipment", "", "équipement de l'invité envoyé dans Hello : templates séparés par des virgules (vide : aucun)")
	weapon := fs.String("weapon", "It_1H_Sword", "arme en main de l'invité, envoyée en Weapon après le Spawn (« - » : arme rangée)")
	armor := fs.String("armor", "", "armure de l'invité, envoyée en Armor après le Weapon (« - » : sans armure ; vide : pas d'Armor)")
	fs.Usage = func() {
		fmt.Fprint(fs.Output(), usageText)
		fs.PrintDefaults()
		fmt.Fprint(fs.Output(), exitCodesText)
	}
	if err := fs.Parse(args); err != nil {
		if err == flag.ErrHelp {
			return sessionConfig{}, exitOK
		}
		return sessionConfig{}, exitError
	}

	usageError := func(format string, a ...any) (sessionConfig, int) {
		fmt.Fprintf(stderr, "fake-guest : "+format+"\n", a...)
		fs.Usage()
		return sessionConfig{}, exitError
	}
	if fs.NArg() > 0 {
		return usageError("argument inattendu %q", fs.Arg(0))
	}
	addr, err := netip.ParseAddr(*host)
	if err != nil || !addr.Is4() {
		return usageError("--host doit être une adresse IPv4 (reçu %q)", *host)
	}
	if *port == 0 || *port > 65535 {
		return usageError("--port hors bornes (%d)", *port)
	}
	if *drop < 0 || *drop > 100 {
		return usageError("--drop doit être entre 0 et 100 (reçu %d)", *drop)
	}
	if *quit && *kill {
		return usageError("--quit et --kill sont exclusifs")
	}
	if *duration < 0 {
		return usageError("--duration négative")
	}
	if (*quit || *kill) && *duration == 0 {
		return usageError("--quit et --kill demandent --duration > 0")
	}
	if *delay < 0 || *jitter < 0 {
		return usageError("--delay et --jitter doivent être positifs")
	}
	if *echo && *scenarioPath != "" {
		return usageError("--echo et --scenario sont exclusifs")
	}
	weaponName := *weapon
	if weaponName == "-" {
		weaponName = ""
	}
	if len(weaponName) > protocol.MaxWeaponName {
		return usageError("--weapon : nom de plus de %d octets", protocol.MaxWeaponName)
	}
	var armorName *string
	if *armor != "" {
		name := *armor
		if name == "-" {
			name = ""
		}
		if len(name) > protocol.MaxArmorName {
			return usageError("--armor : nom de plus de %d octets", protocol.MaxArmorName)
		}
		armorName = &name
	}
	equipmentNames, err := parseEquipment(*equipment)
	if err != nil {
		return usageError("--equipment : %v", err)
	}
	var sc *scenario
	if *scenarioPath != "" {
		var err error
		if sc, err = loadScenario(*scenarioPath); err != nil {
			return usageError("--scenario %s : %v", *scenarioPath, err)
		}
	}
	hello := protocol.Hello{
		ProtocolVersion: protocol.ProtocolVersion,
		ModVersion:      *modVersion,
		Fingerprint:     steamFingerprint,
		Nickname:        *nickname,
		Equipment:       joinEquipment(equipmentNames),
	}
	if _, err := protocol.EncodeControl(hello); err != nil {
		return usageError("Hello hors bornes (pseudo 1 à 32 octets, version du mod 0 à 255 octets)")
	}

	cfg := sessionConfig{
		Host:       netip.AddrPortFrom(addr, uint16(*port)),
		Hello:      hello,
		BadVersion: *badVersion,
		DupHello:   *dupHello,
		Duration:   *duration,
		Kill:       *kill,
		Drop:       *drop,
		Seed:       *seed,
		Scenario:   sc,
		Delay:      *delay,
		Jitter:     *jitter,
		Echo:       *echo,
		Weapon:     weaponName,
		Armor:      armorName,
	}
	defaultTimings(&cfg)
	return cfg, parseOK
}
