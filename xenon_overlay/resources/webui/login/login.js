// Passwords only exist while the user completes an authentication request.
// Session restoration and credentials are owned by the native profile service.
const $ = id => document.getElementById(id);
let sendWithPromise = null;
let registerMode = false;
let loginPending = false;
let loginState = {authenticated: false, restoring: true};
let loginVersion = 0;
let restoreTimer = null;
let restoreInFlight = false;
let continued = false;
let pageClosed = false;

function showLoginError(message) {
  $('loginError').textContent = message;
  $('loginError').hidden = !message;
}

function updateLoginControls() {
  const locked = !sendWithPromise || loginPending || loginState.restoring ||
      loginState.busy || loginState.authenticated;
  $('loginFields').disabled = locked;
  $('loginSubmit').disabled = locked;
  $('loginMode').disabled = locked;
  $('registerMode').disabled = locked;
  $('loginMode').setAttribute('aria-pressed', String(!registerMode));
  $('registerMode').setAttribute('aria-pressed', String(registerMode));
  $('confirmPasswordField').hidden = !registerMode;
  $('confirmPassword').disabled = !registerMode;
  $('password').autocomplete = registerMode ? 'new-password' : 'current-password';
  $('loginHeading').textContent = registerMode ? '创建 Xenon 账号' : '登录 Xenon';
  $('loginSubmit').textContent = loginPending ? '请稍候…' :
      registerMode ? '注册并登录' : '登录';
  $('loginForm').setAttribute('aria-busy',
      String(Boolean(loginPending || loginState.restoring || loginState.busy)));
}

function renderLoginState(next) {
  if (!next || typeof next.authenticated !== 'boolean') {
    throw new Error('浏览器返回了无效的账号状态。');
  }
  loginState = next;
  showLoginError(typeof next.error === 'string' ? next.error : '');
  $('loginStatus').textContent = next.authenticated ? '登录成功，正在打开浏览器…' :
      next.restoring ? '正在恢复登录状态…' : next.busy ? '正在更新账号状态…' : '';
  updateLoginControls();
  if (next.authenticated && !continued && !pageClosed) {
    continued = true;
    chrome.send('xenonAccountContinue', []);
  }
}

function scheduleRestore() {
  clearTimeout(restoreTimer);
  if (!pageClosed && sendWithPromise && !loginState.authenticated) {
    // The native service retries restored sessions after a network failure.
    // Keep observing that retry even while the form is available to the user.
    restoreTimer = setTimeout(loadLoginState,
        loginState.restoring || loginState.busy ? 1000 : 5000);
  }
}

async function loadLoginState() {
  if (!sendWithPromise || pageClosed || restoreInFlight || loginPending) {
    return;
  }
  const version = loginVersion;
  restoreInFlight = true;
  try {
    const next = await sendWithPromise('xenonAccountGetState');
    if (version === loginVersion && !pageClosed) {
      renderLoginState(next);
    }
  } catch {
    if (version === loginVersion && !pageClosed) {
      loginState = {authenticated: false, restoring: false};
      $('loginStatus').textContent = '';
      showLoginError('无法恢复登录状态，请重新登录。');
      updateLoginControls();
    }
  } finally {
    restoreInFlight = false;
    scheduleRestore();
  }
}

function setLoginMode(register) {
  if (loginPending || loginState.restoring || loginState.busy ||
      loginState.authenticated) {
    return;
  }
  registerMode = register;
  $('password').value = '';
  $('confirmPassword').value = '';
  $('confirmPassword').setCustomValidity('');
  showLoginError('');
  updateLoginControls();
}

$('loginMode').addEventListener('click', () => setLoginMode(false));
$('registerMode').addEventListener('click', () => setLoginMode(true));
$('confirmPassword').addEventListener('input', () =>
  $('confirmPassword').setCustomValidity(''));
$('password').addEventListener('input', () =>
  $('confirmPassword').setCustomValidity(''));
$('loginForm').addEventListener('submit', async event => {
  event.preventDefault();
  if (!sendWithPromise || loginPending || loginState.restoring ||
      loginState.busy || loginState.authenticated || pageClosed) {
    return;
  }
  $('username').value = $('username').value.trim();
  $('confirmPassword').setCustomValidity(registerMode &&
      $('password').value !== $('confirmPassword').value ? '两次输入的密码不一致。' : '');
  if (!$('loginForm').reportValidity()) {
    return;
  }
  const credentials = {username: $('username').value, password: $('password').value};
  const version = ++loginVersion;
  loginPending = true;
  showLoginError('');
  $('loginStatus').textContent = registerMode ? '正在创建账号…' : '正在登录…';
  updateLoginControls();
  try {
    const next = await sendWithPromise(
        registerMode ? 'xenonAccountRegister' : 'xenonAccountLogin', credentials);
    if (version === loginVersion && !pageClosed) {
      renderLoginState(next);
    }
  } catch {
    if (version === loginVersion && !pageClosed) {
      $('loginStatus').textContent = '';
      showLoginError('无法连接账号服务，请稍后重试。');
    }
  } finally {
    credentials.password = '';
    $('password').value = '';
    $('confirmPassword').value = '';
    loginPending = false;
    updateLoginControls();
    scheduleRestore();
  }
});

$('login-close-btn').addEventListener('click', () => {
  if (location.protocol === 'chrome:' &&
      typeof globalThis.chrome?.send === 'function') {
    chrome.send('xenonLoginClose', []);
  }
});

async function initialize() {
  updateLoginControls();
  if (location.protocol !== 'chrome:' || location.hostname !== 'xenon-login' ||
      typeof globalThis.chrome?.send !== 'function') {
    loginState = {authenticated: false, restoring: false};
    $('loginStatus').textContent = '静态预览：请在 Xenon 浏览器中登录。';
    updateLoginControls();
    return;
  }
  try {
    const bridge = await import('chrome://resources/js/cr.js');
    if (typeof bridge.sendWithPromise !== 'function') {
      throw new Error('Browser bridge unavailable');
    }
    sendWithPromise = bridge.sendWithPromise;
    await loadLoginState();
  } catch {
    loginState = {authenticated: false, restoring: false};
    $('loginStatus').textContent = '';
    showLoginError('无法连接浏览器的账号服务，请重新打开登录窗口。');
    updateLoginControls();
  }
}

window.addEventListener('pagehide', () => {
  pageClosed = true;
  ++loginVersion;
  clearTimeout(restoreTimer);
  $('password').value = '';
  $('confirmPassword').value = '';
});
void initialize();
