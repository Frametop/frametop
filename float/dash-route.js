// Put into Steam's SharedJSContext by float/ft-dashroute. The SteamVR dashboard's "Launch a
// program" list (Steam's VR menu) runs SteamClient.Apps.LaunchNonSteamApp(cmdline) when an app
// is picked. This wraps it: while ft-dashroute is there and the setting is on, the command line
// goes to ft-dashroute (its CDP binding, window.__ftDashLaunch), which floats the app in the
// Frametop desktop, or, if it can't, runs Steam's own launch after all (state.original). With
// ft-dashroute gone (no heartbeat for STALE_MS) every launch is Steam's own again.
(() => {
    const VERSION = 1;
    const STALE_MS = 10000;
    const apps = SteamClient.Apps;
    const state = window.__ftDashRoute || (window.__ftDashRoute = {});
    if (!state.original || !apps.LaunchNonSteamApp.__ftDashRoute) {
        state.original = apps.LaunchNonSteamApp;  // Steam's own, not a wrapper from before
    }
    state.version = VERSION;
    state.alive = Date.now();
    state.on = true;
    const wrapper = function (cmdline, ...rest) {
        const s = window.__ftDashRoute;
        if (s && s.on && Date.now() - s.alive < STALE_MS && typeof cmdline === "string"
            && typeof window.__ftDashLaunch === "function") {
            try {
                window.__ftDashLaunch(JSON.stringify({ cmdline }));
                return;
            } catch (e) {
                // the binding went with its DevTools session: Steam's own launch below
            }
        }
        return state.original.call(apps, cmdline, ...rest);
    };
    wrapper.__ftDashRoute = true;
    apps.LaunchNonSteamApp = wrapper;
    return "dashboard apps open in Frametop (v" + VERSION + ")";
})()
