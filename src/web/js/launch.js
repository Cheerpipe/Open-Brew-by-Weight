// Launch intro gate. Loaded blocking in <head> (precached by sw.js) so the
// decision lands before the first paint: the installed app plays the intro
// once per fresh launch when Settings → This device turns it on. Browser tabs,
// reloads, back/forward and reduced motion keep the plain loading screen.
(() => {
  const root = document.documentElement;
  try {
    const first = !sessionStorage.getItem("obbwLaunched");
    sessionStorage.setItem("obbwLaunched", "1");
    if (
      !first ||
      !(matchMedia("(display-mode: standalone)").matches || navigator.standalone) ||
      matchMedia("(prefers-reduced-motion: reduce)").matches ||
      performance.getEntriesByType("navigation")[0]?.type !== "navigate" ||
      localStorage.getItem("obbwAppSplash") !== "1"
    )
      return;
  } catch {
    return;
  }
  root.classList.add("launchIntro");
  // The exit waits for the page and for the third droplet, whose landing
  // (3.504 s into the intro, read off its endless droplet loop) becomes the
  // start of the rush, so the rise never pauses. A tap skips at once, and the
  // cap keeps an unreachable controller from hiding behind the splash.
  const MIN_MS = 3504,
    CAP_MS = 8000;
  let gone = false;
  const go = () => {
    if (gone) return;
    gone = true;
    root.classList.add("introGo");
    setTimeout(() => root.classList.remove("launchIntro", "introGo"), 450);
  };
  document.addEventListener(
    "obbwpageready",
    () => {
      const at = document.querySelector(".launchBead.hold")?.getAnimations()[0]?.currentTime;
      setTimeout(go, Math.max(0, MIN_MS - (at ?? MIN_MS)));
    },
    { once: true },
  );
  setTimeout(go, CAP_MS);
  addEventListener(
    "pointerdown",
    (e) => {
      if (gone || !e.target.closest?.("#launchIntro")) return;
      e.preventDefault();
      e.stopPropagation();
      go();
    },
    true,
  );
})();
