// Frontend UI utilities
// Provides reusable UI components and helper functions for consistent interface elements

// =============================================================================
// UI COMPONENT FACTORIES
// =============================================================================

function createButton(text, callback = null, enabled = true, bold = false, minWidth = 100) {
    const button = document.createElement('button');
    button.textContent = text;
    button.className = `px-4 py-2 rounded-md font-medium transition-colors duration-200 disabled:opacity-50 disabled:cursor-not-allowed min-h-8`;
    
    if (bold) {
        button.classList.add('font-bold');
    }
    
    if (!enabled) {
        button.disabled = true;
        button.classList.add('bg-gray-500', 'text-gray-300');
    } else {
        button.classList.add('bg-accent', 'text-white', 'hover:bg-accent-hover');
    }
    
    if (callback) {
        button.addEventListener('click', callback);
    }
    
    return button;
}

function createInput(placeholder = '', width = 250, height = 30) {
    const input = document.createElement('input');
    input.type = 'text';
    input.placeholder = placeholder;
    input.className = 'px-3 py-2 bg-dark-bg border border-dark-border rounded-md text-white placeholder-dark-text-secondary focus:outline-none focus:border-accent w-64 h-8';
    
    return input;
}

function createSelect(options = [], width = 280, height = 30) {
    const select = document.createElement('select');
    select.className = 'px-3 py-2 bg-dark-bg border border-dark-border rounded-md text-white focus:outline-none focus:border-accent w-72 h-8';
    
    options.forEach(option => {
        const optionElement = document.createElement('option');
        optionElement.value = option.value || option;
        optionElement.textContent = option.text || option;
        select.appendChild(optionElement);
    });
    
    return select;
}

function createLabel(text, bold = false) {
    const label = document.createElement('label');
    label.textContent = text;
    label.className = 'text-sm text-dark-text-secondary';
    if (bold) {
        label.classList.add('font-semibold');
    }
    return label;
}

function createSection(title, content) {
    const section = document.createElement('div');
    section.className = 'bg-dark-surface rounded-lg border border-dark-border p-4 mb-4';
    
    if (title) {
        const header = document.createElement('h3');
        header.textContent = title;
        header.className = 'text-lg font-semibold mb-3 text-white';
        section.appendChild(header);
    }
    
    if (content) {
        section.appendChild(content);
    }
    
    return section;
}

// Cache status styling classes for better performance
const STATUS_CLASSES = {
    'Error': 'text-error font-medium',
    'Running': 'text-success font-medium',
    'Initialized': 'text-warning font-medium',
    'Stopped': 'text-gray-400 font-medium'
};

function createTable(headers, data = [], className = '') {
    const table = document.createElement('table');
    table.className = `w-full text-sm border-collapse ${className}`;
    
    // Create header
    const thead = document.createElement('thead');
    const headerRow = document.createElement('tr');
    headerRow.className = 'border-b border-dark-border bg-dark-surface';
    
    headers.forEach(header => {
        const th = document.createElement('th');
        th.textContent = header;
        th.className = 'text-left py-3 px-4 font-semibold text-white border-r border-dark-border last:border-r-0 bg-dark-surface';
        headerRow.appendChild(th);
    });
    
    thead.appendChild(headerRow);
    table.appendChild(thead);
    
    // Create body
    const tbody = document.createElement('tbody');
    data.forEach(row => {
        const tr = document.createElement('tr');
        tr.className = 'border-b border-dark-border hover:bg-dark-surface';
        
        row.forEach((cell, cellIndex) => {
            const td = document.createElement('td');
            td.textContent = cell;
            td.className = 'py-2 px-4 text-white border-r border-dark-border last:border-r-0';
            
            // Apply status styling to first column (Status)
            if (cellIndex === 0 && STATUS_CLASSES[cell]) {
                td.className += ` ${STATUS_CLASSES[cell]}`;
            }
            
            tr.appendChild(td);
        });
        
        tbody.appendChild(tr);
    });
    
    table.appendChild(tbody);
    return table;
}

// Cache scrollbar styles to avoid recreating them
let scrollbarStylesAdded = false;

function createLogContainer(maxHeight = 300) {
    const container = document.createElement('div');
    container.className = 'bg-dark-bg border border-dark-border rounded-md p-3 font-mono text-xs max-h-96 overflow-y-auto custom-scrollbar';
    
    // Add custom scrollbar styling only once
    if (!scrollbarStylesAdded) {
        container.style.cssText = `
            scrollbar-width: thin;
            scrollbar-color: #404040 #2a2a2a;
        `;
        
        const style = document.createElement('style');
        style.textContent = `
            .custom-scrollbar::-webkit-scrollbar { width: 8px; }
            .custom-scrollbar::-webkit-scrollbar-track { background: #2a2a2a; }
            .custom-scrollbar::-webkit-scrollbar-thumb { background: #404040; border-radius: 4px; }
            .custom-scrollbar::-webkit-scrollbar-thumb:hover { background: #555; }
        `;
        document.head.appendChild(style);
        scrollbarStylesAdded = true;
    }
    
    return container;
}

// Cache level colors and styles for performance
const LEVEL_COLORS = {
    'DEBUG': '#9ca3af',
    'INFO': '#60a5fa', 
    'WARNING': '#fbbf24',
    'ERROR': '#f87171',
    'CRITICAL': '#dc2626'
};

const VALID_LEVELS = ['DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'];

function addLogEntry(container, message, level = 'info', gatewayName = '') {
    const entry = document.createElement('div');
    
    // Apply log entry styling with inline styles
    entry.style.cssText = `
        font-family: 'Inter', 'Segoe UI', 'Roboto', 'Helvetica Neue', Arial, sans-serif;
        font-size: 14px;
        line-height: 1.5;
        font-weight: 400;
        display: flex;
        align-items: center;
        gap: 0;
        margin-bottom: 4px;
    `;
    
    // Add hover effect
    entry.addEventListener('mouseenter', () => {
        entry.style.backgroundColor = 'rgba(59, 130, 246, 0.1)';
    });
    entry.addEventListener('mouseleave', () => {
        entry.style.backgroundColor = 'transparent';
    });
    
    // Parse log message to extract components
    const logComponents = parseLogMessage(message);
    
    // Get the actual level (uppercase) and ensure it's valid
    const actualLevel = logComponents.level || level.toUpperCase();
    const safeLevel = VALID_LEVELS.includes(actualLevel) ? actualLevel : 'INFO';
    
    // Format: time | level | gateway | message
    const time = (logComponents.time || new Date().toLocaleTimeString()).padEnd(8);
    const levelText = safeLevel.padEnd(7);
    const gateway = (logComponents.gateway || gatewayName || 'SYSTEM').padEnd(10);
    const messageText = logComponents.message || message;

    // Create spans with optimized styling
    const createSpan = (text, styles) => {
        const span = document.createElement('span');
        span.style.cssText = styles;
        span.textContent = text;
        return span;
    };

    const separatorStyle = 'color: #d1d5db; margin: 0 4px;';
    
    // Build entry with all spans
    entry.appendChild(createSpan(time, 'color: #4ade80; width: 95px; flex-shrink: 0; white-space: nowrap;'));
    entry.appendChild(createSpan(' | ', separatorStyle));
    entry.appendChild(createSpan(levelText, `font-weight: 600; width: 70px; flex-shrink: 0; white-space: nowrap; color: ${LEVEL_COLORS[safeLevel] || '#ffffff'};`));
    entry.appendChild(createSpan(' | ', separatorStyle));
    entry.appendChild(createSpan(gateway, 'color: #22d3ee; width: 100px; flex-shrink: 0; white-space: nowrap;'));
    entry.appendChild(createSpan(' | ', separatorStyle));
    entry.appendChild(createSpan(messageText, 'color: #ffffff; flex: 1;'));
    
    container.appendChild(entry);
    container.scrollTop = container.scrollHeight;
    
    // Keep only last 100 entries
    if (container.children.length > 100) {
        container.removeChild(container.firstChild);
    }
}

// Cache regex pattern for better performance
const LOG_PATTERN = /^(\d{2}\/\d{2} \d{2}:\d{2}:\d{2})\s*\|\s*(\w+)\s*\|\s*(\w+)\s*\|\s*(.*)$/;

function parseLogMessage(message) {
    try {
        // Ensure message is a string
        if (typeof message !== 'string') {
            message = String(message);
        }
        
        // Try to parse structured log format: "DD/MM HH:mm:ss | LEVEL | GATEWAY | message"
        const match = message.match(LOG_PATTERN);
        
        if (match && match.length >= 5) {
            return {
                time: match[1],
                level: match[2],
                gateway: match[3],
                message: match[4]
            };
        }
    } catch (error) {
        console.error('Error parsing log message:', error);
    }
    
    // Fallback for unstructured messages
    return {
        time: null,
        level: null,
        gateway: null,
        message: message
    };
}

// =============================================================================
// VALIDATION HELPERS
// =============================================================================

function validateRequired(value, fieldName) {
    if (!value || value.trim() === '') {
        throw new Error(`${fieldName} is required`);
    }
    return value.trim();
}

function validateNumber(value, fieldName, min = null, max = null) {
    const num = parseFloat(value);
    if (isNaN(num)) {
        throw new Error(`${fieldName} must be a valid number`);
    }
    if (min !== null && num < min) {
        throw new Error(`${fieldName} must be at least ${min}`);
    }
    if (max !== null && num > max) {
        throw new Error(`${fieldName} must be at most ${max}`);
    }
    return num;
}

// =============================================================================
// MODULE EXPORTS
// =============================================================================

if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        createButton,
        createInput,
        createSelect,
        createLabel,
        createSection,
        createTable,
        createLogContainer,
        addLogEntry,
        validateRequired,
        validateNumber
    };
}
