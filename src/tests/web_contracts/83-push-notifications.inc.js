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

  // The setting is off until a tap asks, and only an installed app shows it.
  if (
    !/class="pwaOnly"\s*><input id="appPush"/.test(rawPartialHtml.settings) ||
    !blockAt(runtimeJs, "async function pushEnable() {").includes("Notification.requestPermission()")
  )
    throw new Error("Notifications turn on only from a tap in the installed app");

  // A blocked setting's reason stays under its label: the row's grid places every
  // other child beside the toggle. The samples use the Web UI's own button bar.
  if (
    !/<small class="fieldHint">[^<]*<span id="appPushNote" hidden><\/span><\/small>/.test(
      rawPartialHtml.settings,
    ) ||
    !/<div class="btnBar glassBar"><button class="btnGlyph" type="button" data-result="on_recipe">/.test(
      rawPartialHtml.diagnostic,
    )
  )
    throw new Error("The push setting and samples must keep the Web UI's own layout");

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
