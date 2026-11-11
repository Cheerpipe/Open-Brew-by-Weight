{
  // Notifications: the companion delivers an event unread, so its wording, its
  // tap target and every decision about a device live here. The generated
  // worker turns each event into the agreed text, always shows something (Safari
  // can revoke the permission of a push that shows nothing), and builds the tap
  // target itself, never from a path in the message.
  const assert = require("assert").strict;
  const vm = require("vm");

  const handlers = {},
    shown = [],
    opened = [],
    stored = {};
  const worker = vm.createContext({
    self: {
      addEventListener: (type, handler) => (handlers[type] = handler),
      registration: {
        showNotification: async (title, options) => shown.push({ title, ...options }),
        pushManager: { getSubscription: async () => ({ endpoint: "https://web.push.apple.com/x" }) },
      },
      clients: { matchAll: async () => [], openWindow: async (url) => opened.push(url) },
    },
    caches: { open: async () => ({ put: async (name, reply) => (stored[name] = reply) }) },
    fetch: async () => ({ ok: true }),
    location: { origin: "https://brew" },
    Response: class {
      constructor(body) {
        this.body = body;
      }
    },
    URL,
    JSON,
    Number,
    Object,
    Date,
  });
  vm.runInContext((await webUi.generate()).swJs, worker);
  for (const type of ["push", "notificationclick", "pushsubscriptionchange"])
    assert.equal(typeof handlers[type], "function", `the worker must handle ${type}`);

  const notify = async (message) => {
    shown.length = 0;
    let done;
    handlers.push({
      data: {
        json() {
          if (message === undefined) throw new Error("not JSON");
          return message;
        },
      },
      waitUntil: (work) => (done = work),
    });
    await done;
    assert.equal(shown.length, 1, "every push shows exactly one notification");
    return shown[0];
  };

  await (async () => {
    const shot = (d) => ({
      topic: "shot_result",
      key: "42-1003",
      mid: "m",
      d: { v: 1, id: 1003, b: 42, e: 900000, at: 1762268000, s: 28.4, ...d },
    });
    for (const [d, title, body] of [
      [{ r: "on_recipe", g: 36.2, w: "m" }, "On-recipe shot: 28.4 s", "Yield: 36.2 g"],
      [{ r: "fast", g: 36.25, w: "m" }, "Fast shot: 28.4 s", "Yield: 36.3 g"],
      [{ r: "slow", g: 36.1, w: "m" }, "Slow shot: 28.4 s", "Yield: 36.1 g"],
      [{ r: null, g: 36.2, w: "m" }, "Shot: 28.4 s", "Yield: 36.2 g"],
      [{ r: "fast", g: 36.2, w: "e" }, "Fast shot: 28.4 s", "Estimated yield: 36.2 g"],
      [{ r: "fast" }, "Fast shot: 28.4 s", "No yield measured"],
      [{ r: "constructor", g: 1, w: "m" }, "Shot: 28.4 s", "Yield: 1.0 g"],
      [{ r: "fast", g: 1, w: "m", test: true }, "Test. Fast shot: 28.4 s", "Yield: 1.0 g"],
    ]) {
      const note = await notify(shot(d));
      assert.equal(note.title, title);
      assert.equal(note.body, body);
      assert.equal(note.tag, "42-1003", "a redelivery replaces instead of duplicating");
      assert.equal(note.timestamp, 1762268000000);
      assert.equal(note.data.url, "/stats?shot=1003&boot=42&end=900000");
    }

    // Whatever arrives, something shows, and it never takes a path from the message.
    for (const message of [
      undefined,
      null,
      {},
      { topic: "another", d: {} },
      { topic: "shot_result", d: { v: 2, s: 1 } },
      { topic: "shot_result", d: { v: 1, s: "28" } },
    ]) {
      const note = await notify(message);
      assert.equal(note.title, "Open Brew by Weight");
      assert.equal(note.body, "Open the app to see what changed.");
    }
    for (const id of [-1, 1.5, "1&admin=1", "//evil.test"]) {
      const note = await notify(shot({ id, url: "https://evil.test" }));
      assert.equal(note.data.url, "/stats", "only whole numbers reach the tap target");
    }

    // La Marzocco's terms open Home, whose card says what to do; a tap never opens Shots.
    const terms = await notify({ topic: "la_marzocco_policy", key: "micra-policy", mid: "m", d: { v: 1 } });
    assert.equal(terms.title, "La Marzocco updated its terms");
    assert.match(terms.body, /Tap to see what to do/);
    assert.equal(terms.tag, "micra-policy", "a reminder replaces instead of stacking");
    assert.equal(terms.data.url, "/");
    assert.equal(
      (await notify({ topic: "la_marzocco_policy", d: { v: 1, test: true } })).title,
      "Test. La Marzocco updated its terms",
    );
    assert.equal(
      (await notify({ topic: "la_marzocco_policy", d: { v: 2 } })).title,
      "Open Brew by Weight",
      "a newer event still shows the generic notification",
    );

    let done;
    handlers.notificationclick({
      notification: { close() {}, data: { url: "/stats?shot=1&boot=2&end=3" } },
      waitUntil: (work) => (done = work),
    });
    await done;
    assert.deepEqual(opened, ["/stats?shot=1&boot=2&end=3"], "a tap with no window opens one");
  })();

  // Every word is the firmware's: none reaches the page as a literal.
  for (const key of ["push.title_fast", "push.yield", "push.generic_body"])
    if (!fs.readFileSync(path.join(sketchDir, "web", "sw.js"), "utf8").includes(key))
      throw new Error(`The worker must take ${key} from the catalog`);

  // Each setting shows only in an installed app, and the terms one on Linea Micra
  // builds. Its warning sits beside the label, not in it: a tap on the warning
  // must not flip the toggle. The samples use the Web UI's own button bar.
  const settingsSource = fs.readFileSync(path.join(sketchDir, "web", "js", "settings.js"), "utf8");
  for (const [id, gate] of [
    ["appPush", "pwaOnly"],
    ["appPushTerms", "laMarzoccoOnly pwaOnly"],
  ])
    if (
      !new RegExp(
        `<div class="${gate}">\\s*<label\\s*><input id="${id}"[^]*?</label\\s*>\\s*` +
          `<div id="${id}Note" class="homeCard resultFailed pushNote" role="status" hidden>`,
      ).test(rawPartialHtml.settings)
    )
      throw new Error(`${id} must keep its gate and its warning below the label`);
  if (
    !settingsSource.includes('["appPushTerms", "la_marzocco_policy"]') ||
    !/<div class="btnBar glassBar"><button class="btnGlyph" type="button" data-result="on_recipe">/.test(
      rawPartialHtml.diagnostic,
    )
  )
    throw new Error("The terms topic and the samples must keep their own controls");

  // A toggle holds the choice whatever push can do, and every way push can fail
  // has a reason to show.
  if (
    /\.disabled = !!blocked/.test(settingsSource) ||
    !settingsSource.includes("note.hidden = !($(id).checked && problem)")
  )
    throw new Error("A toggle follows the choice, with the warning only while it is on");
  if (runtimeJs.split("Notification.requestPermission(").length !== 2)
    throw new Error("A tap asks for the notification permission exactly once");
  if (!blockAt(runtimeJs, "async function pushSetTopic(topic, wanted) {").match(
    /^async function pushSetTopic\(topic, wanted\) \{\s*if \(wanted &&[^;{]*Notification\.requestPermission\(\);/,
  ))
    throw new Error("Safari spends the tap: the request comes before anything is awaited");
  const localeEntries = JSON.parse(
    fs.readFileSync(path.join(sketchDir, "web", "locales", "en.json"), "utf8"),
  ).strings;
  const reasons = localeEntries["settings.app_push_blocked"].split("|");
  if (reasons.length !== 8 || reasons.some((reason) => !reason.trim()))
    throw new Error("Every push problem needs its own reason");

  const pushSource = rawRuntimeJs.slice(
    rawRuntimeJs.indexOf("function pushBlocked() {"),
    rawRuntimeJs.indexOf("let companionHereLoad"),
  );
  const pushDevice = ({ protocol = "https:", here, permission = "default", answer, stale }) => {
    const device = {
      saved: {},
      posts: [],
      asked: 0,
      subscribed: 0,
      subscription: null,
      fail: null,
      permission,
    };
    const toSubscription = (key) => ({
      endpoint: "https://push/1",
      options: { applicationServerKey: key },
      unsubscribe: async () => (device.subscription = null),
    });
    const registration = {
      pushManager: {
        getSubscription: async () => device.subscription,
        subscribe: async ({ applicationServerKey }) => {
          device.subscribed++;
          return (device.subscription = toSubscription(applicationServerKey));
        },
      },
    };
    const Notification = {
      get permission() {
        return device.permission;
      },
      requestPermission: async () => {
        device.asked++;
        return (device.permission = answer ?? device.permission);
      },
    };
    device.context = vm.createContext({
      __WEBUI_TEXT__: (key) => localeEntries[key],
      uiState: { companionHere: here },
      location: { protocol },
      window: { PushManager: 1, Notification },
      Notification,
      navigator: {
        userAgent: "",
        serviceWorker: { ready: Promise.resolve(registration), getRegistration: async () => registration },
      },
      localStorage: {
        getItem: (name) => device.saved[name] ?? null,
        setItem: (name, value) => (device.saved[name] = value),
      },
      caches: {
        open: async () => ({
          match: async (name) => (name === "/topics" && stale ? { json: async () => stale } : undefined),
          put: async () => {},
        }),
      },
      Response: class {},
      companionLog: async (url, body) => {
        if (device.fail) throw device.fail;
        device.posts.push([url, body]);
      },
      atob: (text) => Buffer.from(text, "base64").toString("latin1"),
      Uint8Array,
      JSON,
      Error,
    });
    vm.runInContext(`${pushSource};Object.assign(globalThis, { pushTopics, pushSetTopic, pushApply, pushProblem, pushDisable, pushReconcile, pushProblemText });`, device.context);
    device.topics = async () => [...(await device.context.pushTopics())];
    return device;
  };
  const ready = { paired: true, push: { publicKey: "AAEC" } };

  await (async () => {
    // Off the companion's address nothing can ask, and the choice still holds.
    let device = pushDevice({ protocol: "http:", here: null });
    await device.context.pushSetTopic("shot_result", true);
    assert.deepEqual(await device.topics(), ["shot_result"]);
    assert.equal(device.asked, 0, "only the companion's address can ask");
    assert.equal(device.context.pushProblem(), "address");
    await device.context.pushSetTopic("shot_result", false);
    assert.deepEqual(await device.topics(), [], "turning off works while push cannot");

    // Unpaired: the tap still asks, and pairing later needs no second tap.
    device = pushDevice({ here: { paired: false }, answer: "granted" });
    await device.context.pushSetTopic("shot_result", true);
    assert.equal(device.asked, 1);
    assert.equal(device.subscribed, 0);
    assert.equal(device.context.pushProblem(), "pair");
    device.context.uiState.companionHere = ready;
    await device.context.pushApply();
    assert.equal(device.subscribed, 1, "the choice takes effect once nothing is missing");
    assert.equal(device.context.pushProblem(), "");
    device.posts.length = 0;
    await device.context.pushApply();
    assert.equal(device.posts.length, 0, "a device that looks right sends nothing");

    // Each choice stands on its own, and the last one off ends the subscription.
    await device.context.pushSetTopic("la_marzocco_policy", true);
    assert.deepEqual(device.posts.at(-1)[1].topics, ["shot_result", "la_marzocco_policy"]);
    await device.context.pushSetTopic("shot_result", false);
    await device.context.pushSetTopic("la_marzocco_policy", false);
    assert.equal(device.subscription, null);
    assert.equal(device.posts.at(-1)[0], "/__companion/push/unsubscribe");

    // A dismissed or refused prompt keeps the choice and says what to do.
    device = pushDevice({ here: ready });
    await device.context.pushSetTopic("shot_result", true);
    assert.deepEqual(await device.topics(), ["shot_result"]);
    assert.equal(device.subscribed, 0, "nothing subscribes without the permission");
    assert.equal(device.context.pushProblem(), "ask");
    device = pushDevice({ here: ready, answer: "denied" });
    await device.context.pushSetTopic("shot_result", true);
    assert.deepEqual(await device.topics(), ["shot_result"]);
    assert.equal(device.context.pushProblem(), "denied");

    // A failed sign-up keeps the choice, names the cause, and is retried.
    device = pushDevice({ here: ready, answer: "granted" });
    device.fail = { error: "CONTROLLER_UNREACHABLE" };
    await device.context.pushSetTopic("shot_result", true);
    assert.deepEqual(await device.topics(), ["shot_result"]);
    assert.equal(device.context.pushProblem(), "controller");
    device.fail = Error("500");
    await device.context.pushApply();
    assert.equal(device.context.pushProblem(), "failed");
    device.fail = null;
    await device.context.pushApply();
    assert.equal(device.context.pushProblem(), "");
    assert.equal(device.posts.length, 1, "the retry signs this device up");

    // A subscription the browser lost comes back from the choice on the next open.
    device.subscription = null;
    device.subscribed = 0;
    await device.context.pushReconcile();
    assert.equal(device.subscribed, 1);

    // Signing out ends the choice too.
    await device.context.pushDisable();
    assert.deepEqual(await device.topics(), []);

    // A device from before the choice was kept: subscribed means what it held,
    // and what an old "off" left behind in the worker's copy means nothing.
    device = pushDevice({ here: ready, permission: "granted", stale: ["la_marzocco_policy"] });
    assert.deepEqual(await device.topics(), [], "no subscription, no choice");
    device.subscription = { endpoint: "https://push/1" };
    assert.deepEqual(await device.topics(), ["la_marzocco_policy"]);
    device = pushDevice({ here: ready, permission: "granted" });
    device.subscription = { endpoint: "https://push/1" };
    assert.deepEqual(await device.topics(), ["shot_result"]);
  })();

  // Opening without a notification's address changes nothing on Shots.
  const statsSource = fs.readFileSync(path.join(sketchDir, "web", "js", "stats.js"), "utf8");
  if (
    !statsSource.includes('if (!(id > 0)) return;') ||
    !statsSource.includes('history.replaceState({}, "", location.pathname)') ||
    !statsSource.includes("r.id === id && r.bootId === boot && r.endedAtMs === end") ||
    !statsSource.includes("prefers-reduced-motion")
  )
    throw new Error("A tapped notification finds its own shot once, and only that shot");
}
