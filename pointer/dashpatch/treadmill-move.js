// Frametop: SteamVR's floating windows move for the 3D mouse's treadmill device (pointer/dashpatch).
// The dashboard's UndockedOverlay.startFloatingWindowMove reads the pressing device from the mouse
// event's CapsLock/NumLock modifier bits (head, left hand, right hand) and gives up when there are
// none, as for a treadmill. This wraps the grab bar's onMouseDown: a press with no source runs the
// original as the right hand, with that one pose query answered by /user/treadmill, and the move
// then follows /user/treadmill.
(() => {
  const VERSION = 1;
  if (window.__ftTreadmillMove && window.__ftTreadmillMove.version >= VERSION) return 'already ' + window.__ftTreadmillMove.version;
  let req = null;
  self.webpackChunkvrwebui.push([[Symbol('frametop-treadmill-move')], {}, r => { req = r; }]);
  if (!req) return 'no webpack';
  // React: module 6540 in SteamVR 2.18.2's dashboard; else the module that defines React's element
  // symbol (loaded already, so requiring it runs nothing new)
  const isReact = m => m && typeof m.createElement === 'function' && typeof m.Component === 'function' && /^18\./.test(m.version || '');
  let React = req.m['6540'] ? req('6540') : null;
  if (!isReact(React)) {
    React = null;
    for (const id of Object.keys(req.m)) {
      const src = req.m[id].toString();
      if (src.length < 40000 && src.includes('Symbol.for("react.element")') && src.includes('createElement')) {
        const m = req(id);
        if (isReact(m)) { React = m; break; }
      }
    }
  }
  if (!React) return 'no React';
  const internals = React.__SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED;
  const TREADMILL = '/user/treadmill', RIGHT = '/user/hand/right';
  const state = window.__ftTreadmillMove = {version: VERSION, moves: 0, last: ''};
  const wrapped = new WeakMap();   // the original bound handler -> ours

  const wrap = (orig, inst) => function (e) {
    let src = 0;
    try { src = (e.getModifierState('CapsLock') ? 1 : 0) | (e.getModifierState('NumLock') ? 2 : 0); } catch (err) { src = -1; }
    if (src !== 0 || !inst) return orig(e);
    const [pose] = VRHTML.GetPose(TREADMILL, 0);
    if (!pose || !pose.bPoseIsValid) { state.last = 'treadmill pose invalid'; return orig(e); }
    const fake = new Proxy(e, {get(t, k) {
      if (k === 'getModifierState') return m => m === 'CapsLock' || m === 'NumLock';
      const v = t[k];
      return typeof v === 'function' ? v.bind(t) : v;
    }});
    const getPose = VRHTML.GetPose;
    let first = true;   // only the moving device's query (the first one); a hand-docked window's own stays
    VRHTML.GetPose = function (path, origin) {
      if (first && path === RIGHT) { first = false; path = TREADMILL; }
      return getPose.call(this, path, origin);
    };
    let r;
    try { r = orig(fake); } finally { VRHTML.GetPose = getPose; }
    if (inst.m_sMoveDevicePath === RIGHT) {
      inst.m_sMoveDevicePath = TREADMILL;
      inst.setState({sParentDevice: TREADMILL});
      state.moves++;
      state.last = 'moved by the treadmill';
    }
    return r;
  };

  const createElement = React.createElement;
  React.createElement = function (type, props) {
    if (props && typeof props.onMouseDown === 'function' && props.explicitSGID !== undefined &&
        props.onMouseDown.name === 'bound startFloatingWindowMove') {
      const owner = internals && internals.ReactCurrentOwner && internals.ReactCurrentOwner.current;
      const inst = owner && owner.stateNode;
      let ours = wrapped.get(props.onMouseDown);
      if (!ours) { ours = wrap(props.onMouseDown, inst); wrapped.set(props.onMouseDown, ours); }
      const args = Array.prototype.slice.call(arguments);
      args[1] = Object.assign({}, props, {onMouseDown: ours});
      return createElement.apply(this, args);
    }
    return createElement.apply(this, arguments);
  };
  return 'installed ' + VERSION;
})()
