// Optional admin PIN. With a PIN set, the clock answers 401 to a change that has no token: ask for the PIN once,
// keep the token for this browser tab, and repeat the request.
(function () {
  var KEY = 'umToken', plain = window.fetch.bind(window);
  function token() { try { return sessionStorage.getItem(KEY) || ''; } catch (e) { return ''; } }
  window.umToken = function (value) { try { value ? sessionStorage.setItem(KEY, value) : sessionStorage.removeItem(KEY); } catch (e) {} };
  function login() {
    var pin = prompt('This clock is protected. Enter its PIN:');
    if (pin === null) return Promise.resolve(false);
    return plain('/api/auth', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'pin=' + encodeURIComponent(pin) })
      .then(function (r) { return r.json().then(function (j) {
        if (r.ok && j.token !== undefined) { window.umToken(j.token); return true; }
        alert(j.message || 'Wrong PIN.');
        return j.wait ? false : login();
      }); });
  }
  function withToken(init) {
    var copy = Object.assign({}, init), headers = new Headers(init.headers || {});
    if (token()) headers.set('X-Token', token());
    copy.headers = headers;
    return copy;
  }
  window.fetch = function (url, init) {
    init = init || {};
    var post = String(init.method || 'GET').toUpperCase() === 'POST';
    if (!post) return plain(url, init);
    return plain(url, withToken(init)).then(function (r) {
      if (r.status !== 401) return r;
      return r.clone().json().then(function (j) {
        return j.pin ? login().then(function (ok) { return ok ? plain(url, withToken(init)) : r; }) : r;
      }, function () { return r; });
    });
  };
})();
