{
  // An installed app over the companion never shows the inactive overlay in
  // ordinary use: it holds its claim instead of arming the 15-minute lock,
  // re-claims first when it returns with the stream down, and holds the
  // overlay through one per-epoch takeover re-claim. Plain http, browser
  // tabs and unknown companion state keep today's lock, overlay and Reload.
  const assert = require("assert").strict;
  const vm = require("vm");

  // The class gate reads only what every view can observe: the protocol, the
  // display mode and the companion status the boot resolves. Anything less —
  // http, a tab, an unresolved status — stays on the standard lock.
  {
    const run = vm.createContext({
      location: { protocol: "https:" },
      matchMedia: () => ({ matches: true }),
      navigator: { standalone: false },
      uiState: { companionHere: null },
    });
    vm.runInContext(
      blockAt(runtimeJs, "function webUiStandalone() {") +
        blockAt(runtimeJs, "function webUiPwaClass() {"),
      run,
    );
    assert.equal(run.webUiPwaClass(), false, "an unresolved companion is not app-class");
    run.uiState.companionHere = { paired: true };
    assert.equal(run.webUiPwaClass(), true, "https + installed + paired is app-class");
    run.matchMedia = () => ({ matches: false });
    run.navigator = { standalone: true };
    assert.equal(run.webUiPwaClass(), true, "the legacy iOS standalone property counts");
    run.navigator = { standalone: false };
    assert.equal(run.webUiPwaClass(), false, "a browser tab is not app-class");
    run.matchMedia = () => ({ matches: true });
    run.location.protocol = "http:";
    assert.equal(run.webUiPwaClass(), false, "a direct http page is never app-class");
    run.location.protocol = "https:";
    run.uiState.companionHere = { paired: false };
    assert.equal(run.webUiPwaClass(), false, "an unpaired companion is not app-class");
  }

  // The hold: while app-class outside Admin the lock never arms — Infinity,
  // no timer — and every transition (class flip either way, Admin either
  // way) lands on the right state. The lazy poll never expires Infinity.
  {
    let hold = false,
      now = 1e6;
    const timers = new Map();
    let nextTimer = 0;
    const fired = [];
    const run = vm.createContext({
      sessionShotLive: () => false,
      deactivateWebUi: (reason) => fired.push(reason),
      Date: { now: () => now },
      WEB_UI_INACTIVITY_MS: 1000,
      setTimeout: (fn, delay) => {
        timers.set(++nextTimer, { fn, delay });
        return nextTimer;
      },
      clearTimeout: (id) => timers.delete(id),
    });
    run.webUiPwaClass = () => hold;
    run.activeView = "home";
    vm.runInContext(
      "let webUiOwner=true,setupSession=false,webUiActiveUntil=0,webUiInactivityTimer=0;" +
        blockAt(runtimeJs, "function webUiHoldClaim() {") +
        blockAt(runtimeJs, "function refreshWebUiHold() {") +
        blockAt(runtimeJs, "function resetWebUiInactivity() {") +
        blockAt(runtimeJs, "function webUiPollingActive() {") +
        "this.api={refresh:refreshWebUiHold,polling:webUiPollingActive," +
        "until:()=>webUiActiveUntil,arm:resetWebUiInactivity};",
      run,
    );
    const lockTimer = () => [...timers.values()].find((t) => t.delay === 1000);
    const setHold = (value) => {
      hold = value;
      run.webUiPwaClass = () => value;
    };
    const setView = (value) => {
      run.activeView = value;
    };
    // Standard class: the lock arms and fires today's deactivation.
    run.api.arm();
    assert.ok(run.api.until() < Infinity && lockTimer(), "a standard page arms the lock");
    now += 1000;
    lockTimer().fn();
    assert.deepEqual(fired, ["inactivity"], "the lock fires the inactivity reason");
    // The hold engages from either direction and never arms a timer.
    setHold(true);
    run.api.refresh();
    assert.equal(run.api.until(), Infinity, "the hold is Infinity");
    assert.ok(!lockTimer(), "the hold arms no timer");
    assert.equal(run.api.polling(), true, "Infinity never expires lazily");
    // Interactions keep Infinity instead of arming a finite window.
    run.api.arm();
    assert.equal(run.api.until(), Infinity, "reset keeps Infinity under the hold");
    // Leaving the hold (relay off, or entering Admin) re-arms the lock.
    setHold(false);
    run.api.refresh();
    assert.ok(run.api.until() < Infinity && lockTimer(), "dropping the hold re-arms the lock");
    // The Admin view alone drops the hold on an app-class page.
    setHold(true);
    setView("admin");
    run.api.refresh();
    assert.ok(run.api.until() < Infinity, "Admin keeps today's lock");
    setView("home");
    run.api.refresh();
    assert.equal(run.api.until(), Infinity, "leaving Admin re-engages the hold");
  }

  // Deactivation: the overlay is held only while hidden on a reclaimable
  // app-class loss, or while the epoch's visible takeover re-claim runs;
  // every visible loss and every "direct" pause keeps its explanation.
  {
    const shown = [],
      hints = [];
    const run = vm.createContext({
      webUiEpochClaimPending: false,
      document: { hidden: true },
      webUiOwner: true,
      setupSession: false,
      webUiActiveUntil: 5,
      webUiInactivityTimer: 0,
      stopUiStream() {},
      clearTimeout() {},
      stopViewPolls() {},
      lockAdminUi() {},
      setMutable() {},
      showInactiveOverlay: () => shown.push(1),
      setInactiveHint: (text) => hints.push(text),
      __WEBUI_TEXT__: (key) => key,
    });
    run.webUiHoldClaim = () => true;
    vm.runInContext(
      blockAt(runtimeJs, "function deactivateWebUi(reason) {") + "this.deactivate=deactivateWebUi;",
      run,
    );
    // A hidden app-class page holds the overlay for reclaimable reasons; the
    // firmware's bare takeover close (no reason text) reads "takeover".
    for (const reason of ["inactivity", "takeover", ""])
      (run.webUiOwner = true), run.deactivate(reason);
    assert.equal(shown.length, 0, "a hidden app-class page paints nothing");
    assert.equal(run.webUiInactiveReason, "takeover", "an empty close reason defaults to takeover");
    // A visible loss always explains itself.
    run.document.hidden = false;
    run.webUiOwner = true;
    run.deactivate("takeover");
    assert.equal(shown.length, 1, "a visible loss paints the overlay");
    // The held epoch re-claim keeps the overlay while the claim runs.
    run.webUiEpochClaimPending = true;
    run.webUiOwner = true;
    run.deactivate("takeover");
    assert.equal(shown.length, 1, "the held takeover re-claim paints nothing");
    assert.equal(run.webUiEpochClaimPending, false, "the pending flag is consumed once");
    // "direct" explains itself even hidden; the standard class always paints.
    run.document.hidden = true;
    run.webUiOwner = true;
    run.deactivate("direct");
    assert.equal(shown.length, 2, "direct paints its overlay");
    assert.equal(hints.length, 1, "direct keeps its explanation");
    run.webUiHoldClaim = () => false;
    run.webUiOwner = true;
    run.deactivate("inactivity");
    assert.equal(shown.length, 3, "a standard page paints while hidden");
    // A non-owner call is a no-op that still clears a stale pending flag.
    run.webUiEpochClaimPending = true;
    run.webUiOwner = false;
    run.deactivate("takeover");
    assert.equal(run.webUiEpochClaimPending, false, "a stale pending flag never leaks");
  }

  // Claim-first: only an app-class page outside Admin with its stream down
  // and its ownership plausible (still held, or lost to a takeover) claims
  // before anything else — never for inactivity, "direct", reconnect or a
  // still-open socket.
  {
    const claims = [];
    const run = vm.createContext({
      setupSession: false,
      webUiInactiveReason: "",
      claimWebUiOwnership: () => claims.push(1),
    });
    vm.runInContext(
      blockAt(runtimeJs, "function webUiClaimFirst() {") + "this.claim=webUiClaimFirst;",
      run,
    );
    const cases = [
      [{}, true, "a returning app page claims first"],
      [{ ws: { readyState: 1 } }, false, "an open socket sends nothing"],
      [{ owner: false, reason: "takeover" }, true, "a takeover lost while hidden re-claims"],
      [{ owner: false, reason: "inactivity" }, false, "inactivity never re-claims"],
      [{ owner: false, reason: "direct" }, false, "a direct pause stays manual"],
      [{ owner: false, reason: "reconnect" }, false, "reconnect-wait owns its polling"],
      [{ view: "admin" }, false, "Admin stays on today's lock"],
      [
        { pwa: false, owner: false, reason: "takeover" },
        false,
        "a standard page never claims first",
      ],
    ];
    for (const [patch, expected, why] of cases) {
      claims.length = 0;
      run.webUiPwaClass = () => ("pwa" in patch ? patch.pwa : true);
      run.activeView = "view" in patch ? patch.view : "home";
      run.shotWs = "ws" in patch ? patch.ws : null;
      run.webUiPollingActive = () => ("owner" in patch ? patch.owner : true);
      run.webUiInactiveReason = "reason" in patch ? patch.reason : "";
      run.claim();
      assert.equal(claims.length, expected ? 1 : 0, why);
    }
    run.webUiPwaClass = () => true;
    run.activeView = "home";
    run.shotWs = null;
    run.webUiPollingActive = () => true;
    run.setupSession = true;
    run.claim();
    assert.equal(claims.length, 0, "a setup session claims nothing");
  }

  // The epoch: one held re-claim per visibility epoch on a takeover close
  // the page actually sees; hidden pages, Admin and the standard class spend
  // nothing, and a second visible loss in one epoch paints today's overlay.
  {
    const run = vm.createContext({
      document: { hidden: false },
      webUiEpochClaimUsed: false,
      webUiEpochClaimPending: false,
      webUiPwaClass: () => true,
      activeView: "home",
    });
    vm.runInContext(
      blockAt(runtimeJs, "function webUiTakeoverVisible() {") +
        "this.take=webUiTakeoverVisible;this.used=()=>webUiEpochClaimUsed;" +
        "this.pending=()=>webUiEpochClaimPending;",
      run,
    );
    assert.equal(run.take(), true, "the first visible takeover holds the overlay");
    assert.equal(run.used(), true, "the epoch is spent");
    assert.equal(run.pending(), true, "the deactivation is told to hold");
    assert.equal(run.take(), false, "a second loss in the epoch paints instead");
    run.webUiEpochClaimUsed = false;
    run.document.hidden = true;
    assert.equal(run.take(), false, "a hidden page spends nothing");
    assert.equal(run.used(), false, "its return re-claims instead");
    run.document.hidden = false;
    run.activeView = "admin";
    assert.equal(run.take(), false, "Admin never holds the overlay");
    run.activeView = "home";
    run.webUiPwaClass = () => false;
    assert.equal(run.take(), false, "a standard page never holds the overlay");
  }

  // Placement beats intent: claim-first must run before the polling gates in
  // both handlers, so a page deactivated while hidden (takeover under 5 min)
  // cannot dead-end behind webUiPollingActive() returning false.
  {
    const visibility = blockAt(runtimeJs, "function onPageVisibility() {");
    assert.ok(
      visibility.indexOf("webUiClaimFirst()") >= 0 &&
        visibility.indexOf("webUiClaimFirst()") <
          visibility.indexOf("if (!webUiPollingActive() || setupSession) return;"),
      "claim-first precedes the polling gate on visibility return",
    );
    const pageshow = blockAt(runtimeJs, 'window.addEventListener("pageshow", () => {');
    assert.ok(
      pageshow.indexOf("webUiClaimFirst()") >= 0 &&
        pageshow.indexOf("webUiClaimFirst()") <
          pageshow.indexOf("if (webUiPollingActive() && !setupSession) startUiStream();"),
      "claim-first precedes the polling gate on pageshow",
    );
    assert.ok(
      codeIncludes(pageshow, "if (webUiClaimFirst()) return;"),
      "a claimed return does not also start the stream",
    );
  }

  // Source needles: the boot resolves the companion on every https load
  // (the manifest shortcuts open /stats, /history and /settings directly),
  // the view change re-evaluates the hold, and the takeover close holds the
  // overlay through its re-claim. The claim's own in-flight guard is what
  // keeps a fresh load's pageshow from double-claiming.
  {
    assert.ok(
      codeIncludes(
        runtimeJs,
        'if(location.protocol==="https:")companionHere().then(refreshWebUiHold);',
      ) &&
        codeIncludes(runtimeJs, 'document.addEventListener("visibilitychange", onPageVisibility);'),
      "every https page load resolves its companion, whatever view it opens",
    );
    assert.ok(
      codeIncludes(blockAt(runtimeJs, "function setActiveView("), "refreshWebUiHold();"),
      "every view change re-evaluates the hold",
    );
    assert.ok(
      codeIncludes(
        blockAt(runtimeJs, "function claimWebUiOwnership("),
        "if (webUiClaiming) return;",
      ),
      "an in-flight claim absorbs the pageshow call",
    );
    assert.ok(
      codeIncludes(
        blockAt(runtimeJs, "function webUiClaimFirst() {"),
        '!webUiPollingActive() && webUiInactiveReason !== "takeover") return false;',
      ) &&
        codeIncludes(
          blockAt(runtimeJs, "function webUiTakeoverVisible() {"),
          'webUiPwaClass() ||\n      activeView === "admin") return false;',
        ),
      "the predicates exclude Admin and non-takeover reasons",
    );
    assert.ok(
      codeIncludes(
        blockAt(runtimeJs, "function deactivateWebUi(reason) {"),
        'reason ||= "takeover";',
      ) &&
        codeIncludes(
          blockAt(runtimeJs, "function deactivateWebUi(reason) {"),
          "if (!(heldOverlay || (document.hidden && webUiHoldClaim() &&",
        ) &&
        codeIncludes(
          blockAt(runtimeJs, "function deactivateWebUi(reason) {"),
          '(reason === "inactivity" || reason === "takeover")))) {',
        ),
      "the overlay holds only for the hidden reclaimable matrix",
    );
    assert.ok(
      codeIncludes(
        runtimeJs,
        'sessionShotLive()\n      ? resetWebUiInactivity()\n      : deactivateWebUi("inactivity");',
      ) &&
        codeIncludes(runtimeJs, 'deactivateWebUi("inactivity");\n  return false;') &&
        codeIncludes(runtimeJs, 'if (webUiOwner) deactivateWebUi("reconnect");') &&
        codeIncludes(runtimeJs, 'if (data.error === "UI_TAKEN_OVER") deactivateWebUi("takeover");'),
      "every deactivation names its reason",
    );
    assert.ok(
      codeIncludes(runtimeJs, "const held = webUiTakeoverVisible();") &&
        codeIncludes(
          runtimeJs,
          "deactivateWebUi(event.reason);\n      if (held) claimWebUiOwnership();",
        ),
      "the takeover close holds the overlay through its re-claim",
    );
    assert.ok(
      codeIncludes(runtimeJs, 'webUiOwner = true;\n    webUiInactiveReason = "";'),
      "a successful claim clears the reason",
    );
    assert.ok(
      codeIncludes(
        blockAt(runtimeJs, "function resetWebUiInactivity() {"),
        "if (webUiHoldClaim()) {\n    webUiActiveUntil = Infinity;\n    clearTimeout(webUiInactivityTimer);\n    webUiInactivityTimer = 0;\n    return;\n  }",
      ) &&
        codeIncludes(
          blockAt(runtimeJs, "function refreshWebUiHold() {"),
          "} else if (webUiActiveUntil === Infinity) resetWebUiInactivity();",
        ),
      "the hold never arms the lock and both flips are symmetric",
    );
    assert.ok(
      codeIncludes(
        viewJs.diagnostic,
        '$("dUiMode").textContent = R.webUiStandalone() ? t[1] : t[0];',
      ),
      "Diagnostic reads the shared standalone helper",
    );
  }
}
