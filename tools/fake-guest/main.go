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

Faux invité Risen Multiplayer (protocole v1, protocol/PROTOCOL.md §8).
Envoie Hello toutes les 500 ms (abandon après 10 s), puis tient la session :
Heartbeat à 1 Hz, timeout après 5 s sans datagramme de l'hôte.

Options :
`

const exitCodesText = `
Fautes : --drop, --quit, --kill, --dup-hello, --bad-version (cumulables, sauf --quit/--kill).
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
	badVersion := fs.Bool("bad-version", false, "envoie le Hello de hello_v2.bin (version 2) au lieu du Hello v1")
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
	hello := protocol.Hello{
		ProtocolVersion: protocol.ProtocolVersion,
		ModVersion:      *modVersion,
		Fingerprint:     steamFingerprint,
		Nickname:        *nickname,
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
	}
	defaultTimings(&cfg)
	return cfg, parseOK
}
