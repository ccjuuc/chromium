// Function to initialize the extension UI
function initExtension() {
  console.log('Xenon Overlay Extension: Initializing...');

  const closeBtn = document.getElementById('closeBtn');
  if (closeBtn) {
    closeBtn.addEventListener('click', () => {
      console.log('Close button clicked');
      window.close();
    });
    console.log('Close button handler attached');
  } else {
    console.error('Close button not found!');
  }

  const actionBtn = document.getElementById('actionBtn');
  if (actionBtn) {
    actionBtn.addEventListener('click', () => {
      console.log('Action button clicked');
      // Example extension API call
      if (chrome && chrome.runtime) {
        console.log('Extension ID:', chrome.runtime.id);
      } else {
        console.warn('chrome.runtime not available');
      }
      alert('Action triggered!');
    });
    console.log('Action button handler attached');
  } else {
    console.error('Action button not found!');
  }
  
  console.log('Xenon Overlay Extension: Initialization complete');
}

// Try to initialize immediately if DOM is already loaded
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initExtension);
} else {
  // DOM is already loaded, initialize immediately
  initExtension();
}
