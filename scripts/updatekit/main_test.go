package main

import (
	"testing"
	"time"
)

// The release-level flags decide what happens on other people's machines without
// asking them, and every way of getting them wrong is silent — the manifest still
// parses, still verifies, and the fleet just behaves differently than intended.
// So the rule gets pinned here rather than being re-derived from the merge body.

// Within one release the flags are STICKY. A release merges once per platform
// into the same file; a later call that forgot --critical must not un-mark it.
// That failure would point the unsafe way: a security update quietly demoted to
// routine by whichever platform happened to merge last.
func TestFlagsAreStickyWithinARelease(t *testing.T) {
	m := &manifest{}
	if err := applyReleaseFlags(m, "1.12.0", true, false, "1.10.0"); err != nil {
		t.Fatalf("first merge: %v", err)
	}
	// Second platform, same release, no flags passed.
	if err := applyReleaseFlags(m, "1.12.0", false, false, ""); err != nil {
		t.Fatalf("second merge: %v", err)
	}
	if !m.Critical {
		t.Error("critical was dropped by a second platform's merge")
	}
	if m.MinSupported != "1.10.0" {
		t.Errorf("min_supported = %q, want it to survive", m.MinSupported)
	}
}

// Across releases they RESET, so last month's flag cannot ride along into this
// month's release and force-restart a fleet nobody meant to touch.
func TestFlagsResetOnANewVersion(t *testing.T) {
	m := &manifest{}
	if err := applyReleaseFlags(m, "1.12.0", true, true, "1.10.0"); err != nil {
		t.Fatal(err)
	}
	if err := applyReleaseFlags(m, "1.13.0", false, false, ""); err != nil {
		t.Fatal(err)
	}
	if m.Critical || m.Rollback || m.MinSupported != "" {
		t.Errorf("flags rode along into a new release: %+v", m)
	}
}

func TestRollbackIsOptIn(t *testing.T) {
	m := &manifest{}
	if err := applyReleaseFlags(m, "1.12.0", false, false, ""); err != nil {
		t.Fatal(err)
	}
	if m.Rollback {
		t.Error("rollback set without being asked for — that would bypass the anti-replay floor on an ordinary release")
	}
	if err := applyReleaseFlags(m, "1.12.0", false, true, ""); err != nil {
		t.Fatal(err)
	}
	if !m.Rollback {
		t.Error("--rollback did not take")
	}
}

// A floor newer than the release demands a version nobody is publishing. Caught
// in the tool, so it never reaches a client — where the same check exists but the
// only thing it can do is refuse the whole manifest.
func TestFloorAboveTheReleaseIsRefused(t *testing.T) {
	m := &manifest{}
	if err := applyReleaseFlags(m, "1.12.0", false, false, "2.0.0"); err == nil {
		t.Fatal("a floor above the release was accepted")
	}
	// Equal is fine: "you must be on at least the version I am shipping".
	if err := applyReleaseFlags(&manifest{}, "1.12.0", false, false, "1.12.0"); err != nil {
		t.Errorf("min_supported == version was refused: %v", err)
	}
	// And a rollback carrying a floor above ITSELF is the same contradiction —
	// the floor would push every client straight back off the version the
	// rollback just sent them to.
	if err := applyReleaseFlags(&manifest{}, "1.11.0", false, true, "1.12.0"); err == nil {
		t.Fatal("a rollback whose floor sits above its own version was accepted")
	}
}

// newerSemver has to agree with the client's selfupdate.IsNewer: the two compare
// the SAME two strings, and disagreeing means publishing a floor the client then
// rejects. Pre-release and build suffixes do not count.
func TestNewerSemverMatchesTheClient(t *testing.T) {
	cases := []struct {
		a, b string
		want bool
	}{
		{"1.12.0", "1.11.0", true},
		{"1.11.0", "1.12.0", false},
		{"1.11.0", "1.11.0", false},
		{"1.11.1", "1.11.0", true},
		{"2.0.0", "1.99.99", true},
		{"1.12.0-rc1", "1.12.0", false}, // suffix ignored → equal
		{"1.12.0", "1.12.0-rc1", false},
		{"v1.12.0", "1.11.0", true}, // leading v tolerated
		{"nonsense", "1.11.0", false},
		{"1.11.0", "nonsense", false},
	}
	for _, c := range cases {
		if got := newerSemver(c.a, c.b); got != c.want {
			t.Errorf("newerSemver(%q, %q) = %v, want %v", c.a, c.b, got, c.want)
		}
	}
}

// ---- staged rollout ----

func intp(n int) *int { return &n }

// --hours starts the ramp NOW. The clock is the moment the command runs, which
// is why the runbook has it run immediately before the upload.
func TestRolloutHoursStartsTheRampNow(t *testing.T) {
	now := time.Date(2026, 9, 16, 12, 0, 0, 0, time.UTC)
	m := &manifest{Version: "1.12.0"}
	if err := applyRollout(m, now, rolloutOpts{hours: 48, cap: -1}); err != nil {
		t.Fatal(err)
	}
	if m.Rollout == nil || !m.Rollout.Start.Equal(now) || m.Rollout.Hours != 48 || m.Rollout.CapPercent != nil {
		t.Fatalf("rollout = %+v", m.Rollout)
	}
	if got := rolloutPercent(m.Rollout, now.Add(12*time.Hour)); got != 25 {
		t.Errorf("percent after 12h of 48 = %d, want 25", got)
	}
}

// --pause freezes the ceiling where the ramp is; restarting keeps the ceiling.
func TestRolloutPauseAndRestart(t *testing.T) {
	start := time.Date(2026, 9, 16, 12, 0, 0, 0, time.UTC)
	m := &manifest{Version: "1.12.0"}
	_ = applyRollout(m, start, rolloutOpts{hours: 100, cap: -1})
	if err := applyRollout(m, start.Add(30*time.Hour), rolloutOpts{hours: -1, cap: -1, pause: true}); err != nil {
		t.Fatal(err)
	}
	if m.Rollout.CapPercent == nil || *m.Rollout.CapPercent != 30 {
		t.Fatalf("paused cap = %v, want 30", m.Rollout.CapPercent)
	}
	if got := rolloutPercent(m.Rollout, start.Add(90*time.Hour)); got != 30 {
		t.Errorf("a paused rollout kept growing: %d%%", got)
	}
	// Restart over a new window: the ceiling stays until someone lifts it.
	_ = applyRollout(m, start.Add(40*time.Hour), rolloutOpts{hours: 10, cap: -1})
	if m.Rollout.CapPercent == nil || *m.Rollout.CapPercent != 30 {
		t.Errorf("restarting dropped the cap: %v", m.Rollout.CapPercent)
	}
}

func TestRolloutRejects(t *testing.T) {
	now := time.Now()
	for name, c := range map[string]struct {
		m manifest
		o rolloutOpts
	}{
		"no version":         {manifest{}, rolloutOpts{hours: 1, cap: -1}},
		"cap without ramp":   {manifest{Version: "1.12.0"}, rolloutOpts{hours: -1, cap: 50}},
		"cap over 100":       {manifest{Version: "1.12.0", Rollout: &rollout{Start: now}}, rolloutOpts{hours: -1, cap: 101}},
		"hours over a month": {manifest{Version: "1.12.0"}, rolloutOpts{hours: 721, cap: -1}},
		"clear plus hours":   {manifest{Version: "1.12.0"}, rolloutOpts{hours: 1, cap: -1, clear: true}},
		"pause plus cap":     {manifest{Version: "1.12.0", Rollout: &rollout{Start: now}}, rolloutOpts{hours: -1, cap: 10, pause: true}},
	} {
		m := c.m
		if err := applyRollout(&m, now, c.o); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

// A schedule belongs to one release. Merging a NEW version must not inherit last
// release's ramp — least of all a pause at 0%, which would hold the new release
// back from everyone with nothing saying why.
func TestRolloutResetsOnANewVersion(t *testing.T) {
	m := &manifest{Version: "1.12.0", Rollout: &rollout{Start: time.Now(), Hours: 48, CapPercent: intp(0)}}
	if err := applyReleaseFlags(m, "1.12.0", false, false, ""); err != nil {
		t.Fatal(err)
	}
	if m.Rollout == nil {
		t.Fatal("a second platform's merge dropped the rollout")
	}
	if err := applyReleaseFlags(m, "1.13.0", false, false, ""); err != nil {
		t.Fatal(err)
	}
	if m.Rollout != nil {
		t.Errorf("the rollout rode along into a new release: %+v", m.Rollout)
	}
}
