// Simple WebUI JavaScript
// Demonstrates traditional message-based communication with C++ backend
// using chrome.send() and cr.sendWithPromise()

/**
 * Initialize the page when DOM is ready.
 */
document.addEventListener('DOMContentLoaded', () => {
  initMessageCallbacks();
  logPageInfo();
});

/**
 * Set up button click handlers for demonstrating message callbacks.
 */
function initMessageCallbacks() {
  // Demo button - simple click counter (client-side only)
  const demoButton = document.getElementById('demo-button');
  const demoOutput = document.getElementById('demo-output');
  let clickCount = 0;

  if (demoButton && demoOutput) {
    demoButton.addEventListener('click', () => {
      clickCount++;
      const timestamp = new Date().toLocaleTimeString();
      demoOutput.textContent = `Clicked ${clickCount} time(s) at ${timestamp}`;
      demoOutput.style.backgroundColor = '#e8f5e9';
      setTimeout(() => {
        demoOutput.style.backgroundColor = '';
      }, 300);
    });
  }

  // System Info button - gets data from C++
  const sysInfoButton = document.getElementById('sys-info-button');
  const sysInfoOutput = document.getElementById('sys-info-output');

  if (sysInfoButton && sysInfoOutput) {
    sysInfoButton.addEventListener('click', async () => {
      sysInfoOutput.textContent = 'Loading...';
      try {
        const info = await sendWithPromise('getSystemInfo');
        sysInfoOutput.innerHTML = formatSystemInfo(info);
        sysInfoOutput.classList.add('success');
      } catch (error) {
        sysInfoOutput.textContent = 'Error: ' + error.message;
        sysInfoOutput.classList.add('error');
      }
    });
  }

  // Log button - sends a log message to C++
  const logButton = document.getElementById('log-button');
  const logInput = document.getElementById('log-input');
  const logOutput = document.getElementById('log-output');

  if (logButton && logInput && logOutput) {
    logButton.addEventListener('click', () => {
      const message = logInput.value || 'Hello from WebUI!';
      // This is a one-way message, no response expected
      chrome.send('logMessage', [message, 'info']);
      logOutput.textContent = `✓ Logged: "${message}" (check Chrome console/logs)`;
      logOutput.classList.add('success');
    });
  }

  // Action button - performs an action in C++ and gets result
  const actionButton = document.getElementById('action-button');
  const actionSelect = document.getElementById('action-select');
  const actionParam = document.getElementById('action-param');
  const actionOutput = document.getElementById('action-output');

  if (actionButton && actionSelect && actionOutput) {
    actionButton.addEventListener('click', async () => {
      actionOutput.textContent = 'Processing...';
      const actionName = actionSelect.value;
      const param = actionParam.value;

      try {
        let result;
        if (actionName === 'greet') {
          result = await sendWithPromise('performAction', actionName, param);
        } else if (actionName === 'calculate') {
          const nums = param.split(',').map(n => parseInt(n.trim(), 10) || 0);
          result = await sendWithPromise('performAction', actionName, nums[0], nums[1] || 0);
        } else {
          result = await sendWithPromise('performAction', actionName);
        }
        actionOutput.innerHTML = formatActionResult(result);
        actionOutput.classList.add('success');
      } catch (error) {
        actionOutput.textContent = 'Error: ' + error.message;
        actionOutput.classList.add('error');
      }
    });
  }
}

/**
 * Wrapper for chrome.send() that returns a Promise.
 * This mimics the behavior of cr.sendWithPromise() from Chrome's WebUI framework.
 *
 * @param {string} methodName - The name of the message handler in C++
 * @param {...any} args - Additional arguments to pass to the handler
 * @returns {Promise<any>} - A promise that resolves with the result from C++
 */
function sendWithPromise(methodName, ...args) {
  return new Promise((resolve, reject) => {
    // Generate a unique callback ID
    const callbackId = methodName + '_' + Date.now() + '_' + Math.random().toString(36).substr(2, 9);

    // Store the resolve function so it can be called when C++ responds
    window['cr'] = window['cr'] || {};
    window['cr'].webUIResponse = function(id, success, response) {
      if (id === callbackId) {
        if (success) {
          resolve(response);
        } else {
          reject(new Error(response || 'Unknown error'));
        }
      }
    };

    // The standard WebUI pattern: first arg is the callback ID
    chrome.send(methodName, [callbackId, ...args]);
  });
}

/**
 * Format system info as HTML.
 */
function formatSystemInfo(info) {
  return `
    <div class="info-grid">
      <div class="info-item"><strong>OS:</strong> ${info.operatingSystem}</div>
      <div class="info-item"><strong>Version:</strong> ${info.osVersion}</div>
      <div class="info-item"><strong>Architecture:</strong> ${info.architecture}</div>
      <div class="info-item"><strong>CPU Cores:</strong> ${info.cpuCount}</div>
      <div class="info-item"><strong>Memory:</strong> ${info.physicalMemoryMB} MB</div>
    </div>
  `;
}

/**
 * Format action result as HTML.
 */
function formatActionResult(result) {
  let html = `<div class="result-item"><strong>Action:</strong> ${result.action}</div>`;
  html += `<div class="result-item"><strong>Success:</strong> ${result.success ? '✓' : '✗'}</div>`;
  html += `<div class="result-item"><strong>Message:</strong> ${result.message}</div>`;
  if (result.result !== undefined) {
    html += `<div class="result-item"><strong>Result:</strong> ${result.result}</div>`;
  }
  return html;
}

/**
 * Log page information to the console for debugging.
 */
function logPageInfo() {
  console.log('=== Simple WebUI Loaded ===');
  console.log('URL:', window.location.href);
  console.log('Protocol:', window.location.protocol);
  console.log('Host:', window.location.host);
  console.log('Message API: chrome.send() available:', typeof chrome !== 'undefined' && typeof chrome.send === 'function');
  console.log('===========================');
}
