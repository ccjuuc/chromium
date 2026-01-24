import {PageHandler} from './xenon.mojom-webui.js';

// Get the remote handler to communicate with the C++ backend
const handler = PageHandler.getRemote();

console.log('Xenon WebUI Loaded');

document.addEventListener('DOMContentLoaded', () => {
    const closeBtn = document.getElementById('close-btn');
    if (closeBtn) {
        closeBtn.addEventListener('click', () => {
             // Use Mojo to close the dialog
             console.log('Close requested via Mojo');
             handler.close();
        });
    }

    const actionBtn = document.getElementById('action-btn');
    if (actionBtn) {
        actionBtn.addEventListener('click', () => {
            const statusText = document.getElementById('status-text');
            if (statusText) {
                statusText.textContent = "Connecting...";
                statusText.style.color = "#ffd166";
                
                // Call Mojo C++ backend to connect to service
                handler.connectToService().then((result) => {
                    const {success, message} = result;
                    console.log('ConnectToService result:', success, message);
                    
                    if (success) {
                        statusText.textContent = "Connected";
                        statusText.style.color = "#06d6a0";
                        // Can also optionally display the message
                    } else {
                        statusText.textContent = "Error";
                        statusText.style.color = "#ef476f";
                        console.error('Connection failed:', message);
                    }
                });
            }
        });
    }
});
