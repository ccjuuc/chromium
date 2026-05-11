// JS for Xenon Video Sniffer WebUI

document.addEventListener('DOMContentLoaded', () => {
    const refreshBtn = document.getElementById('refreshBtn');
    const clearBtn = document.getElementById('clearBtn');
    
    refreshBtn.addEventListener('click', loadMedia);
    clearBtn.addEventListener('click', clearMedia);
    
    // Initial load
    loadMedia();
});

function loadMedia() {
    chrome.send('getSniffedMedia', []);
}

function clearMedia() {
    chrome.send('clearSniffedMedia', []);
    renderMedia([]);
}

window.receiveSniffedMedia = function(mediaList) {
    renderMedia(mediaList);
};

window.onSynthesisTriggered = function(url) {
    alert('Synthesis triggered for: ' + url + '\n(Logic integration pending)');
};

function renderMedia(mediaList) {
    const tbody = document.getElementById('resultsBody');
    const emptyState = document.getElementById('emptyState');
    const tableContainer = document.querySelector('.table-container');
    
    // Use while loop instead of innerHTML = '' to avoid TrustedHTML issues
    while (tbody.firstChild) {
        tbody.removeChild(tbody.firstChild);
    }
    
    if (!mediaList || mediaList.length === 0) {
        tableContainer.style.display = 'none';
        emptyState.classList.add('visible');
        return;
    }
    
    tableContainer.style.display = 'block';
    emptyState.classList.remove('visible');
    
    // Reverse so newest is at the top
    const displayList = [...mediaList].reverse();
    
    displayList.forEach(item => {
        const tr = document.createElement('tr');
        
        const tdMime = document.createElement('td');
        tdMime.textContent = item.mimeType;
        
        const tdUrl = document.createElement('td');
        tdUrl.textContent = item.url;
        if (item.segmentCount > 0) {
            const badge = document.createElement('span');
            badge.className = 'segment-badge';
            badge.textContent = ` (${item.segmentCount} segments)`;
            tdUrl.appendChild(badge);
        }
        
        const tdAction = document.createElement('td');
        tdAction.className = 'action-cell';
        
        const copyBtn = document.createElement('button');
        copyBtn.className = 'copy-btn';
        copyBtn.title = 'Copy URL';
        copyBtn.textContent = '📋';
        copyBtn.onclick = () => {
            navigator.clipboard.writeText(item.url);
            const original = copyBtn.textContent;
            copyBtn.textContent = '✅';
            setTimeout(() => { copyBtn.textContent = original; }, 2000);
        };
        
        const downloadBtn = document.createElement('button');
        downloadBtn.className = 'download-btn-small';
        downloadBtn.title = 'Download';
        downloadBtn.textContent = '⬇️';
        downloadBtn.onclick = () => {
            chrome.send('downloadMedia', [item.url]);
        };

        const mergeBtn = document.createElement('button');
        mergeBtn.className = 'merge-btn-small';
        mergeBtn.title = 'Merge / Synthesize';
        mergeBtn.textContent = '⚙️';
        mergeBtn.onclick = () => {
            chrome.send('mergeMedia', [item.url]);
        };
        
        tdAction.appendChild(copyBtn);
        tdAction.appendChild(downloadBtn);
        tdAction.appendChild(mergeBtn);
        
        tr.appendChild(tdMime);
        tr.appendChild(tdUrl);
        tr.appendChild(tdAction);
        
        tbody.appendChild(tr);
    });
}
