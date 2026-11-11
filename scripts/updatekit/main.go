// updatekit — release tooling for the calabi desktop self-update flow.
//
//	go run. keygen --out <privkey-file>
//	    Generate an ed25519 keypair. The base64 PRIVATE key → <privkey-file>.
// The base64 PUBLIC key →
//	    stdout (bake into the daemon: cmd/calabi selfupdate_wire.go updatePubKeyB64).
//
//	go run. sign --installer <f> --url <u> --platform <key> --privkey <file>
//	    Print one latest.json platform entry (sha256 + ed25519 signature).
//
//	go run. sign-file --file <f> --privkey <file>
//	    Print a detached base64 ed25519 signature over <f>'s exact bytes. Used
//	    for the release build manifest, which is not an
//	    installer and has no platform key — signing it attests to every artifact
//	    it lists AND the source commit they were built from, with one signature
//	    and the key the desktop updater already trusts.
//
//	go run. verify-file --file <f> --sig <f.sig> --pubkey <base64>
//	    Check that signature. Deliberately included: a signing tool with no
//	    verifier is one nobody can hold to account, and the person who most needs
//	    to check a release manifest is not the person who signed it.
//
//	go run. merge --manifest <latest.json> --version <v> --platform <key> \
//	               --installer <f> --url <u> --privkey <file> \
//	               [--critical] [--min-supported <v>] [--rollback]
//	    Sign the installer and add/update its <key> entry in <latest.json> in
//	    place (creating the file if absent), setting version + pub_date. Idempotent
//	    per platform, so each platform's build merges its own entry into the
//	    shared manifest. <key> is a selfupdate.PlatformKey: darwin-universal,
//	    windows-x86_64, …
//	    ALSO writes <latest.json>.sig — a detached signature over the manifest
//	    itself. Upload both: clients refuse a manifest they cannot verify.
//
//	    --critical marks a security release: it installs for everyone except
//	    machines set to "tell me only", and it ignores the maintenance window.
//	    --min-supported <v> is the floor: clients below <v> install no matter
//	    what their setting says. That is the ONLY thing that overrides an
//	    explicit "never without me" — use it when leaving people where they are
//	    has become worse than restarting them unasked.
//	    --rollback marks a DELIBERATE DOWNGRADE: clients accept this version even
//	    though they have already seen a higher one. It is the documented recovery
//	    for a bad release and the ONE thing that bypasses the
//	    anti-replay version floor — which is exactly why it lives inside the
//	    signed manifest, where an attacker can neither add nor strip it.
//	    All three are sticky within a release and reset across one: pass them on
//	    whichever platform merges, they survive the rest.
package main

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type manifest struct {
	Version string `json:"version"`
	Notes   string `json:"notes,omitempty"`
	PubDate string `json:"pub_date,omitempty"`
	// Release-level flags the client acts on. They MUST all be listed here even
	// though merge only sets two: a field this struct does not know about is
	// dropped on the round-trip, silently. `rollback` used to be missing — so a
	// merge run after a hand-edited rollback manifest would have quietly erased
	// the one flag that makes the documented recovery procedure work, and the
	// re-signed result would have looked perfectly valid.
	Rollback     bool                     `json:"rollback,omitempty"`
	Critical     bool                     `json:"critical,omitempty"`
	MinSupported string                   `json:"min_supported,omitempty"`
	Rollout      *rollout                 `json:"rollout,omitempty"`
	Platforms    map[string]platformEntry `json:"platforms"`
}

// rollout staggers a release across machines. The client's
// selfupdate.Rollout reads the same three fields; manifest_wire_test.go there
// pins the names against a verbatim copy of what this tool writes.
type rollout struct {
	Start      time.Time `json:"start"`
	Hours      int       `json:"hours"`
	CapPercent *int      `json:"cap_percent,omitempty"`
}

type platformEntry struct {
	URL       string `json:"url"`
	SHA256    string `json:"sha256"`
	Signature string `json:"signature"`
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	switch os.Args[1] {
	case "keygen":
		keygen(os.Args[2:])
	case "sign":
		sign(os.Args[2:])
	case "sign-file":
		signFile(os.Args[2:])
	case "verify-file":
		verifyFile(os.Args[2:])
	case "merge":
		merge(os.Args[2:])
	case "rollout":
		rolloutCmd(os.Args[2:])
	default:
		usage()
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage:")
	fmt.Fprintln(os.Stderr, "  updatekit keygen --out <privkey-file>")
	fmt.Fprintln(os.Stderr, "  updatekit sign  --installer <f> --url <u> --platform <key> --privkey <file>")
	fmt.Fprintln(os.Stderr, "  updatekit sign-file   --file <f> --privkey <file>")
	fmt.Fprintln(os.Stderr, "  updatekit verify-file --file <f> --sig <f.sig> --pubkey <base64>")
	fmt.Fprintln(os.Stderr, "  updatekit merge --manifest <latest.json> --version <v> --platform <key> --installer <f> --url <u> --privkey <file> [--critical] [--min-supported <v>] [--rollback]")
	fmt.Fprintln(os.Stderr, "  updatekit rollout --manifest <latest.json> [--privkey <file> (--hours <n> [--cap <pct>] | --cap <pct> | --pause | --clear)]")
	os.Exit(2)
}

func keygen(args []string) {
	fs := flag.NewFlagSet("keygen", flag.ExitOnError)
	out := fs.String("out", "", "path to write the base64 PRIVATE key (required; keep secret, never commit)")
	_ = fs.Parse(args)
	if *out == "" {
		fmt.Fprintln(os.Stderr, "keygen: --out is required")
		os.Exit(2)
	}
	pub, priv, err := ed25519.GenerateKey(nil)
	if err != nil {
		fatal(err)
	}
	if err := os.WriteFile(*out, []byte(base64.StdEncoding.EncodeToString(priv)+"\n"), 0o600); err != nil {
		fatal(err)
	}
	fmt.Fprintf(os.Stderr, "private key -> %s (0600). STORE SECURELY, NEVER COMMIT.\n", *out)
	fmt.Fprintln(os.Stderr, "public key (base64) — bake into daemon updatePubKeyB64:")
	fmt.Println(base64.StdEncoding.EncodeToString(pub))
}

func sign(args []string) {
	fs := flag.NewFlagSet("sign", flag.ExitOnError)
	installer := fs.String("installer", "", "path to the installer file")
	url := fs.String("url", "", "public download URL for the installer")
	platform := fs.String("platform", "", "manifest platform key (darwin-universal, windows-x86_64, …)")
	privkeyFile := fs.String("privkey", "", "path to the base64 private key file from keygen")
	_ = fs.Parse(args)
	if *installer == "" || *url == "" || *platform == "" || *privkeyFile == "" {
		fmt.Fprintln(os.Stderr, "sign: --installer, --url, --platform, --privkey are all required")
		os.Exit(2)
	}
	e := signInstaller(*installer, *url, *privkeyFile)
	fmt.Printf("    %q: {\"url\": %q, \"sha256\": %q, \"signature\": %q}\n",
		*platform, e.URL, e.SHA256, e.Signature)
}

func merge(args []string) {
	fs := flag.NewFlagSet("merge", flag.ExitOnError)
	manifestPath := fs.String("manifest", "", "path to latest.json (created/updated in place)")
	version := fs.String("version", "", "release version, e.g. 1.7.0")
	installer := fs.String("installer", "", "path to the installer file")
	url := fs.String("url", "", "public download URL for the installer")
	platform := fs.String("platform", "", "manifest platform key")
	privkeyFile := fs.String("privkey", "", "path to the base64 private key file")
	critical := fs.Bool("critical", false, "security release: installs for everyone except \"tell me only\", ignoring the maintenance window")
	minSupported := fs.String("min-supported", "", "oldest supported version; clients below it update whatever their setting says")
	rollback := fs.Bool("rollback", false, "DELIBERATE DOWNGRADE: let clients move to this version even though they have seen a higher one")
	_ = fs.Parse(args)
	if *manifestPath == "" || *version == "" || *installer == "" || *url == "" || *platform == "" || *privkeyFile == "" {
		fmt.Fprintln(os.Stderr, "merge: --manifest, --version, --installer, --url, --platform, --privkey are all required")
		os.Exit(2)
	}
	e := signInstaller(*installer, *url, *privkeyFile)

	m := manifest{Platforms: map[string]platformEntry{}}
	if b, err := os.ReadFile(*manifestPath); err == nil {
		if jerr := json.Unmarshal(b, &m); jerr != nil {
			fatal(fmt.Errorf("parse existing %s: %w", *manifestPath, jerr))
		}
		if m.Platforms == nil {
			m.Platforms = map[string]platformEntry{}
		}
	}
	if err := applyReleaseFlags(&m, *version, *critical, *rollback, *minSupported); err != nil {
		fatal(err)
	}
	m.PubDate = time.Now().UTC().Format(time.RFC3339)
	m.Platforms[*platform] = e

	sigPath := writeSignedManifest(*manifestPath, m, *privkeyFile)
	fmt.Fprintf(os.Stderr, "merged %s into %s (version %s)\n", *platform, *manifestPath, *version)
	printReleaseFlags(m)
	fmt.Fprintf(os.Stderr, "signed manifest -> %s (upload it NEXT TO latest.json; clients refuse an unsigned manifest)\n", sigPath)
}

// writeSignedManifest writes the manifest and its detached signature.
func writeSignedManifest(manifestPath string, m manifest, privkeyFile string) string {
	out, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(manifestPath), 0o755); err != nil {
		fatal(err)
	}
	body := append(out, '\n')
	if err := os.WriteFile(manifestPath, body, 0o644); err != nil {
		fatal(err)
	}
	// Sign the manifest ITSELF, beside it, over the exact bytes just written.
	//
	// The per-installer signature proves "we built this file"; it says nothing
	// about which VERSION the manifest claims it is. Without this, anyone who
	// could serve the manifest could point a high version number at an old,
	// genuinely signed installer and silently downgrade every install — with the
	// root service doing the applying.
	//
	// Written here rather than as a separate release step because merge already
	// holds the private key: one fewer thing for a release to forget.
	priv := loadPrivKey(privkeyFile)
	sigPath := manifestPath + ".sig"
	sig := base64.StdEncoding.EncodeToString(ed25519.Sign(priv, body)) + "\n"
	if err := os.WriteFile(sigPath, []byte(sig), 0o644); err != nil {
		fatal(err)
	}
	return sigPath
}

// printReleaseFlags says what this manifest will do to other people's machines.
// These change what happens there without asking; whoever runs a release should
// see that in the output, not discover it from the manifest afterwards.
func printReleaseFlags(m manifest) {
	if m.Critical {
		fmt.Fprintln(os.Stderr, "  critical: ON -- installs for everyone not set to \"tell me only\", ignoring the maintenance window")
	}
	if m.MinSupported != "" {
		fmt.Fprintf(os.Stderr, "  min_supported: %s -- anyone below this updates REGARDLESS of their setting\n", m.MinSupported)
	}
	if m.Rollback {
		fmt.Fprintln(os.Stderr, "  rollback: ON -- clients will DOWNGRADE to this version, and the anti-replay")
		fmt.Fprintln(os.Stderr, "            version floor is bypassed. Make sure this version is one you still")
		fmt.Fprintln(os.Stderr, "            want people running: it is what the fleet lands on.")
	}
	if m.Rollout != nil {
		fmt.Fprintf(os.Stderr, "  rollout: %s\n", describeRollout(m.Rollout, time.Now()))
	}
}

// applyReleaseFlags decides the release-level flags for this merge.
//
// They are STICKY WITHIN a release and reset ACROSS one.
//
// A release merges once per platform into the same file, so a later call that
// forgot --critical must not un-mark it: that failure would be silent and in the
// unsafe direction — a security update quietly demoted to routine by whichever
// platform merged last. A NEW version starts clean, so last month's flag cannot
// ride along into this month's release.
//
// To clear a flag on a release already merged: delete latest.json and merge every
// platform again.
func applyReleaseFlags(m *manifest, version string, critical, rollback bool, minSupported string) error {
	if m.Version != version {
		m.Critical, m.MinSupported, m.Rollback, m.Rollout = false, "", false, nil
	}
	if critical {
		m.Critical = true
	}
	if rollback {
		m.Rollback = true
	}
	if minSupported != "" {
		m.MinSupported = minSupported
	}
	if m.MinSupported != "" && newerSemver(m.MinSupported, version) {
		return fmt.Errorf("min-supported %s is newer than the release %s — that demands a version you are not publishing",
			m.MinSupported, version)
	}
	// Stamp the version HERE, not in the caller. "Same release or not" is decided
	// by comparing against it, so leaving the assignment outside made the
	// stickiness silently depend on a line in another function running afterwards
	// — it worked only because merge re-reads the manifest from disk each run.
	// A rollback that also names a floor ABOVE itself is self-contradicting: the
	// floor would force every client back off the version the rollback just sent
	// them to. The check above already catches it, because min_supported is
	// compared against this same version — noted here so the combination is not
	// mistaken for an oversight.
	m.Version = version
	return nil
}

// loadPrivKey reads a base64 ed25519 private key file.
func loadPrivKey(path string) ed25519.PrivateKey {
	privB64, err := os.ReadFile(path)
	if err != nil {
		fatal(err)
	}
	priv, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(privB64)))
	if err != nil || len(priv) != ed25519.PrivateKeySize {
		fatal(fmt.Errorf("bad private key file (want a base64 ed25519 private key)"))
	}
	return ed25519.PrivateKey(priv)
}

// signInstaller computes the sha256 + ed25519 signature of an installer file.
func signInstaller(installer, url, privkeyFile string) platformEntry {
	privB64, err := os.ReadFile(privkeyFile)
	if err != nil {
		fatal(err)
	}
	priv, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(privB64)))
	if err != nil || len(priv) != ed25519.PrivateKeySize {
		fatal(fmt.Errorf("bad private key file (want a base64 ed25519 private key)"))
	}
	data, err := os.ReadFile(installer)
	if err != nil {
		fatal(err)
	}
	sum := sha256.Sum256(data)
	return platformEntry{
		URL:       url,
		SHA256:    hex.EncodeToString(sum[:]),
		Signature: base64.StdEncoding.EncodeToString(ed25519.Sign(ed25519.PrivateKey(priv), data)),
	}
}

// signFile writes a DETACHED signature over the file's exact bytes to stdout.
//
// Detached, not embedded, for one reason: the thing being signed is JSON that
// people read and tools parse, and embedding a signature inside it would make
// the signed bytes depend on how the file is serialised. A .sig beside the file
// keeps "what was signed" identical to "what you downloaded".
func signFile(args []string) {
	fs := flag.NewFlagSet("sign-file", flag.ExitOnError)
	file := fs.String("file", "", "path to the file to sign")
	privkeyFile := fs.String("privkey", "", "path to the base64 private key file from keygen")
	_ = fs.Parse(args)
	if *file == "" || *privkeyFile == "" {
		fmt.Fprintln(os.Stderr, "sign-file: --file and --privkey are both required")
		os.Exit(2)
	}
	privB64, err := os.ReadFile(*privkeyFile)
	if err != nil {
		fatal(err)
	}
	priv, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(privB64)))
	if err != nil || len(priv) != ed25519.PrivateKeySize {
		fatal(fmt.Errorf("bad private key file (want a base64 ed25519 private key)"))
	}
	data, err := os.ReadFile(*file)
	if err != nil {
		fatal(err)
	}
	sum := sha256.Sum256(data)
	fmt.Fprintf(os.Stderr, "signed %s (sha256 %s)\n", *file, hex.EncodeToString(sum[:]))
	fmt.Println(base64.StdEncoding.EncodeToString(ed25519.Sign(ed25519.PrivateKey(priv), data)))
}

// verifyFile is the other half. It exits non-zero on a bad signature so it can
// be used as a gate, and says which key it checked against so a "valid"
// result cannot be mistaken for "valid under the key you meant".
func verifyFile(args []string) {
	fs := flag.NewFlagSet("verify-file", flag.ExitOnError)
	file := fs.String("file", "", "path to the signed file")
	sig := fs.String("sig", "", "path to the detached base64 signature")
	pubkey := fs.String("pubkey", "", "base64 ed25519 PUBLIC key")
	_ = fs.Parse(args)
	if *file == "" || *sig == "" || *pubkey == "" {
		fmt.Fprintln(os.Stderr, "verify-file: --file, --sig and --pubkey are all required")
		os.Exit(2)
	}
	pub, err := base64.StdEncoding.DecodeString(strings.TrimSpace(*pubkey))
	if err != nil || len(pub) != ed25519.PublicKeySize {
		fatal(fmt.Errorf("bad public key (want a base64 ed25519 public key)"))
	}
	sigB64, err := os.ReadFile(*sig)
	if err != nil {
		fatal(err)
	}
	raw, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sigB64)))
	if err != nil {
		fatal(fmt.Errorf("signature file is not base64: %w", err))
	}
	data, err := os.ReadFile(*file)
	if err != nil {
		fatal(err)
	}
	if !ed25519.Verify(ed25519.PublicKey(pub), data, raw) {
		fmt.Fprintf(os.Stderr, "SIGNATURE INVALID for %s under key %s\n", *file, strings.TrimSpace(*pubkey))
		os.Exit(1)
	}
	fmt.Printf("signature OK: %s under key %s\n", *file, strings.TrimSpace(*pubkey))
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "updatekit:", err)
	os.Exit(1)
}

// newerSemver reports whether a is a strictly higher x.y.z than b.
//
// A deliberate copy of selfupdate.IsNewer rather than an import: updatekit is
// its own module in go.work, and pulling the client package in for one
// comparison would drag it into the release tooling's build. The rule it
// encodes — pre-release and build suffixes do not count — has to stay identical
// to the client's, because the two compare the SAME two strings and disagreeing
// means publishing a floor the client then rejects.
func newerSemver(a, b string) bool {
	pa, pb := parseSemver(a), parseSemver(b)
	if pa == nil || pb == nil {
		return false
	}
	for i := 0; i < 3; i++ {
		if pa[i] != pb[i] {
			return pa[i] > pb[i]
		}
	}
	return false
}

func parseSemver(s string) []int {
	s = strings.TrimPrefix(strings.TrimSpace(s), "v")
	if i := strings.IndexAny(s, "-+"); i >= 0 {
		s = s[:i]
	}
	parts := strings.Split(s, ".")
	if len(parts) != 3 {
		return nil
	}
	out := make([]int, 3)
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil || n < 0 {
			return nil
		}
		out[i] = n
	}
	return out
}

// ---- staged rollout --------------------------------------------------

// maxRolloutHours matches the client: longer than a month is a typo for a pause.
const maxRolloutHours = 720

type rolloutOpts struct {
	hours int // -1 = not given
	cap   int // -1 = not given
	pause bool
	clear bool
}

// applyRollout changes the schedule. Kept apart from the command so the rules
// are testable without a key or a file.
//
//	--hours N   (re)start the ramp NOW, over N hours. Run it right before the
//	            manifest is uploaded: the clock starts here, not at upload.
//	--cap P     change only the ceiling.
//	--pause     set the ceiling to where the ramp is right now.
//	--clear     remove the schedule: every machine, at once.
func applyRollout(m *manifest, now time.Time, o rolloutOpts) error {
	if m.Version == "" {
		return fmt.Errorf("the manifest has no version — merge a platform into it first")
	}
	if o.clear {
		if o.hours >= 0 || o.cap >= 0 || o.pause {
			return fmt.Errorf("--clear cannot be combined with --hours, --cap or --pause")
		}
		m.Rollout = nil
		return nil
	}
	if o.pause && o.cap >= 0 {
		return fmt.Errorf("--pause sets the cap itself; do not pass --cap with it")
	}
	if o.hours >= 0 {
		if o.hours > maxRolloutHours {
			return fmt.Errorf("--hours must be 0-%d, got %d", maxRolloutHours, o.hours)
		}
		r := &rollout{Start: now.UTC().Truncate(time.Second), Hours: o.hours}
		if m.Rollout != nil {
			r.CapPercent = m.Rollout.CapPercent
		}
		m.Rollout = r
	}
	if o.cap >= 0 || o.pause {
		if m.Rollout == nil {
			return fmt.Errorf("there is no rollout to cap — start one with --hours")
		}
		c := o.cap
		if o.pause {
			c = rolloutPercent(m.Rollout, now)
		}
		if c > 100 {
			return fmt.Errorf("--cap must be 0-100, got %d", c)
		}
		m.Rollout.CapPercent = &c
	}
	return nil
}

// rolloutPercent is the share of machines allowed to install at now. The SAME
// formula as the client's RolloutState.Percent — a copy for the same reason as
// newerSemver.
func rolloutPercent(r *rollout, now time.Time) int {
	if r == nil {
		return 100
	}
	capPct := 100
	if r.CapPercent != nil {
		capPct = *r.CapPercent
	}
	if now.Before(r.Start) {
		return 0
	}
	if r.Hours == 0 {
		return capPct
	}
	pct := int(100 * now.Sub(r.Start) / (time.Duration(r.Hours) * time.Hour))
	if pct > capPct {
		pct = capPct
	}
	return pct
}

func describeRollout(r *rollout, now time.Time) string {
	capPct := 100
	if r.CapPercent != nil {
		capPct = *r.CapPercent
	}
	return fmt.Sprintf("started %s, 0%% -> %d%% over %dh; %d%% of machines may install now",
		r.Start.UTC().Format(time.RFC3339), capPct, r.Hours, rolloutPercent(r, now))
}

func rolloutCmd(args []string) {
	fs := flag.NewFlagSet("rollout", flag.ExitOnError)
	manifestPath := fs.String("manifest", "", "path to latest.json")
	privkeyFile := fs.String("privkey", "", "path to the base64 private key file (needed for any change)")
	hours := fs.Int("hours", -1, "start the ramp NOW, reaching the cap after this many hours (0 = the cap at once)")
	capPct := fs.Int("cap", -1, "ceiling: the share of machines (0-100) the ramp stops at")
	pause := fs.Bool("pause", false, "set the ceiling to the current percentage")
	clear := fs.Bool("clear", false, "remove the schedule: every machine may install at once")
	_ = fs.Parse(args)
	if *manifestPath == "" {
		fmt.Fprintln(os.Stderr, "rollout: --manifest is required")
		os.Exit(2)
	}
	b, err := os.ReadFile(*manifestPath)
	if err != nil {
		fatal(err)
	}
	var m manifest
	if err := json.Unmarshal(b, &m); err != nil {
		fatal(fmt.Errorf("parse %s: %w", *manifestPath, err))
	}
	now := time.Now()
	o := rolloutOpts{hours: *hours, cap: *capPct, pause: *pause, clear: *clear}
	if o.hours < 0 && o.cap < 0 && !o.pause && !o.clear {
		// Read-only: what is published right now.
		if m.Rollout == nil {
			fmt.Printf("%s: no rollout — every machine may install at once\n", m.Version)
		} else {
			fmt.Printf("%s: %s\n", m.Version, describeRollout(m.Rollout, now))
		}
		return
	}
	if *privkeyFile == "" {
		fmt.Fprintln(os.Stderr, "rollout: --privkey is required to change the schedule (the manifest is re-signed)")
		os.Exit(2)
	}
	if err := applyRollout(&m, now, o); err != nil {
		fatal(err)
	}
	sigPath := writeSignedManifest(*manifestPath, m, *privkeyFile)
	if m.Rollout == nil {
		fmt.Fprintf(os.Stderr, "%s: rollout cleared — every machine may install at once\n", m.Version)
	} else {
		fmt.Fprintf(os.Stderr, "%s: %s\n", m.Version, describeRollout(m.Rollout, now))
	}
	fmt.Fprintf(os.Stderr, "re-signed -> %s. Upload latest.json AND its .sig; the schedule is only what is published.\n", sigPath)
}
