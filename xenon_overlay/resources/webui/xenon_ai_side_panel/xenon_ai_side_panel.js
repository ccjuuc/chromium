import {XenonAiPageHandler, XenonAiPageReceiver} from './xenon_ai.mojom-webui.js';

class PageImpl {
  constructor(app) {
    this.app = app;
  }
  
  onPromptReceived(prompt) {
    this.app.updatePrompt(prompt);
  }
}

class XenonAiSidePanelApp {
  constructor() {
    this.handler = XenonAiPageHandler.getRemote();
    this.receiver = new XenonAiPageReceiver(new PageImpl(this));
    
    this.init();
  }

  async init() {
    try {
      // Connect specifically for callbacks
      await this.handler.setPage(this.receiver.$.bindNewPipeAndPassRemote());
      
      const response = await this.handler.getInitialPrompt();
      if (response && response.prompt) {
        this.updatePrompt(response.prompt);
      }
    } catch (e) {
      console.error('Failed to init Xenon AI Mojo:', e);
    }
  }

  updatePrompt(text) {
    if (!text) return;
    const el = document.getElementById('prompt-text');
    if (el) {
      el.textContent = text;
    }
  }
}

document.addEventListener('DOMContentLoaded', () => {
  window.app = new XenonAiSidePanelApp();
});
