// Enhanced WebSocket Manager - Centralized WebSocket management with improved error handling
// Consolidates WebSocket logic and provides a unified interface

class WebSocketManager {
    constructor() {
        this.connections = new Map();
        this.reconnectAttempts = new Map();
        this.reconnectTimers = new Map();
        this.maxReconnectAttempts = APP_CONSTANTS.UI.RECONNECT_ATTEMPTS;
        this.reconnectDelay = APP_CONSTANTS.UI.RECONNECT_DELAY;
        this.connectionStates = new Map();
        this.messageHandlers = new Map();
        this.isDestroyed = false;
    }

    // =============================================================================
    // CONNECTION MANAGEMENT
    // =============================================================================

    connect(endpoint, callbacks = {}) {
        if (this.isDestroyed) {
            console.warn('WebSocketManager is destroyed, cannot create new connections');
            return null;
        }

        const connectionId = endpoint;
        
        // Prevent duplicate connections
        if (this.connections.has(connectionId)) {
            console.warn(`WebSocket connection already exists: ${endpoint}`);
            return this.connections.get(connectionId);
        }

        try {
            // Build WebSocket URL
            const wsUrl = this.buildWebSocketUrl(endpoint);
            
            const ws = new WebSocket(wsUrl);
            
            // Store connection and callbacks
            this.connections.set(connectionId, ws);
            this.messageHandlers.set(connectionId, callbacks);
            this.connectionStates.set(connectionId, 'connecting');
            
            // Set up event handlers
            this.setupConnectionHandlers(ws, connectionId, callbacks);
            
            return ws;
            
        } catch (error) {
            console.error(`Failed to create WebSocket connection: ${endpoint}`, error);
            this.handleConnectionError(connectionId, error, callbacks);
            return null;
        }
    }

    setupConnectionHandlers(ws, connectionId, callbacks) {
        ws.onopen = () => {
            this.connectionStates.set(connectionId, 'connected');
            this.reconnectAttempts.set(connectionId, 0);
            this.clearReconnectTimer(connectionId);
            
            // Dispatch connection event
            this.dispatchEvent(APP_CONSTANTS.EVENTS.SYSTEM_CONNECTED, { endpoint: connectionId });
            
            if (callbacks.onOpen) {
                try {
                    callbacks.onOpen();
                } catch (error) {
                    console.error('Error in onOpen callback:', error);
                }
            }
        };
        
        ws.onmessage = (event) => {
            try {
                // Parse message if it's JSON
                let data = event.data;
                try {
                    data = JSON.parse(event.data);
                } catch {
                    // Keep as string if not JSON
                }
                
                // Dispatch message event
                this.dispatchEvent(APP_CONSTANTS.EVENTS.LOG_MESSAGE, { 
                    endpoint: connectionId, 
                    data 
                });
                
                if (callbacks.onMessage) {
                    callbacks.onMessage(data);
                }
            } catch (error) {
                console.error('Error handling WebSocket message:', error);
            }
        };
        
        ws.onclose = (event) => {
            this.connectionStates.set(connectionId, 'disconnected');
            this.connections.delete(connectionId);
            
            // Dispatch disconnection event
            this.dispatchEvent(APP_CONSTANTS.EVENTS.SYSTEM_DISCONNECTED, { endpoint: connectionId });
            
            if (callbacks.onClose) {
                try {
                    callbacks.onClose(event);
                } catch (error) {
                    console.error('Error in onClose callback:', error);
                }
            }
            
            // Attempt to reconnect if not intentionally closed
            if (!event.wasClean && !this.isDestroyed) {
                this.attemptReconnect(connectionId, callbacks);
            }
        };
        
        ws.onerror = (error) => {
            console.error(`WebSocket error: ${connectionId}`, error);
            this.handleConnectionError(connectionId, error, callbacks);
            
            if (callbacks.onError) {
                try {
                    callbacks.onError(error);
                } catch (callbackError) {
                    console.error('Error in onError callback:', callbackError);
                }
            }
        };
    }

    handleConnectionError(connectionId, error, callbacks) {
        this.connectionStates.set(connectionId, 'error');
        
        // Dispatch error event
        this.dispatchEvent(APP_CONSTANTS.EVENTS.ERROR_OCCURRED, { 
            endpoint: connectionId, 
            error 
        });
        
        // Attempt to reconnect
        if (!this.isDestroyed) {
            this.attemptReconnect(connectionId, callbacks);
        }
    }

    // =============================================================================
    // RECONNECTION LOGIC
    // =============================================================================

    attemptReconnect(connectionId, callbacks) {
        const attempts = this.reconnectAttempts.get(connectionId) || 0;
        
        if (attempts >= this.maxReconnectAttempts) {
            console.error(`Max reconnection attempts reached for WebSocket: ${connectionId}`);
            this.connectionStates.set(connectionId, 'failed');
            
            // Dispatch failure event
            this.dispatchEvent(APP_CONSTANTS.EVENTS.ERROR_OCCURRED, {
                endpoint: connectionId,
                error: new Error(`Failed to reconnect after ${this.maxReconnectAttempts} attempts`)
            });
            
            return;
        }
        
        const newAttempts = attempts + 1;
        this.reconnectAttempts.set(connectionId, newAttempts);
        
        // Calculate delay with exponential backoff
        const delay = this.reconnectDelay * Math.pow(2, attempts - 1);
        
        const timer = setTimeout(() => {
            if (!this.isDestroyed) {
                this.connect(connectionId, callbacks);
            }
        }, delay);
        
        this.reconnectTimers.set(connectionId, timer);
    }

    clearReconnectTimer(connectionId) {
        const timer = this.reconnectTimers.get(connectionId);
        if (timer) {
            clearTimeout(timer);
            this.reconnectTimers.delete(connectionId);
        }
    }

    // =============================================================================
    // CONNECTION CONTROL
    // =============================================================================

    disconnect(endpoint) {
        const connectionId = endpoint;
        const ws = this.connections.get(connectionId);
        
        if (ws) {
            this.clearReconnectTimer(connectionId);
            this.reconnectAttempts.delete(connectionId);
            this.connectionStates.set(connectionId, 'disconnecting');
            
            ws.close(1000, 'Intentional disconnect');
            this.connections.delete(connectionId);
            this.messageHandlers.delete(connectionId);
            
        }
    }

    disconnectAll() {
        // Clear all timers
        for (const timer of this.reconnectTimers.values()) {
            clearTimeout(timer);
        }
        this.reconnectTimers.clear();
        
        // Close all connections
        for (const [connectionId, ws] of this.connections) {
            this.connectionStates.set(connectionId, 'disconnecting');
            ws.close(1000, 'Disconnecting all');
        }
        
        // Clear all maps
        this.connections.clear();
        this.reconnectAttempts.clear();
        this.connectionStates.clear();
        this.messageHandlers.clear();
    }

    destroy() {
        this.isDestroyed = true;
        this.disconnectAll();
    }

    // =============================================================================
    // STATUS AND UTILITIES
    // =============================================================================

    isConnected(endpoint) {
        const ws = this.connections.get(endpoint);
        return ws && ws.readyState === WebSocket.OPEN;
    }

    getConnectionState(endpoint) {
        return this.connectionStates.get(endpoint) || 'disconnected';
    }

    getConnectionCount() {
        return this.connections.size;
    }

    getAllConnectionStates() {
        const states = {};
        for (const [endpoint, state] of this.connectionStates) {
            states[endpoint] = state;
        }
        return states;
    }

    // =============================================================================
    // MESSAGE SENDING
    // =============================================================================

    send(endpoint, message) {
        const ws = this.connections.get(endpoint);
        if (ws && ws.readyState === WebSocket.OPEN) {
            try {
                const data = typeof message === 'string' ? message : JSON.stringify(message);
                ws.send(data);
                return true;
            } catch (error) {
                console.error(`Failed to send message to ${endpoint}:`, error);
                return false;
            }
        } else {
            console.warn(`Cannot send message to ${endpoint}: connection not open`);
            return false;
        }
    }

    // =============================================================================
    // EVENT SYSTEM
    // =============================================================================

    dispatchEvent(eventName, detail) {
        const event = new CustomEvent(eventName, { detail });
        document.dispatchEvent(event);
    }

    // =============================================================================
    // URL BUILDING
    // =============================================================================

    buildWebSocketUrl(endpoint) {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const host = window.location.host;
        return `${protocol}//${host}${endpoint}`;
    }
}

// =============================================================================
// CONVENIENCE FUNCTIONS
// =============================================================================

// Global WebSocket manager instance
const wsManager = new WebSocketManager();

// Convenience functions for common WebSocket operations
function connectLogWebSocket(callbacks = {}) {
    return wsManager.connect(API_CONFIG.WEBSOCKET.LOGS, {
        onMessage: callbacks.onMessage,
        onOpen: callbacks.onOpen || (() => {}),
        onClose: callbacks.onClose || (() => {}),
        onError: callbacks.onError || (() => {})
    });
}

function connectStrategyWebSocket(callbacks = {}) {
    return wsManager.connect(API_CONFIG.WEBSOCKET.STRATEGIES, {
        onMessage: callbacks.onMessage,
        onOpen: callbacks.onOpen || (() => showSuccess('Connected to strategy updates')),
        onClose: callbacks.onClose || (() => showWarning('Disconnected from strategy updates')),
        onError: callbacks.onError || (() => showError('Strategy WebSocket connection error'))
    });
}

function disconnectLogWebSocket() {
    wsManager.disconnect(API_CONFIG.WEBSOCKET.LOGS);
}

function disconnectStrategyWebSocket() {
    wsManager.disconnect(API_CONFIG.WEBSOCKET.STRATEGIES);
}

function disconnectAllWebSockets() {
    wsManager.disconnectAll();
}

// =============================================================================
// EVENT LISTENERS FOR GLOBAL STATE
// =============================================================================

// Listen for page visibility changes to manage connections
document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
        // Page is hidden, could pause some connections
    } else {
        // Page is visible, ensure connections are active
        // Could add logic to reconnect if needed
    }
});

// Clean up on page unload
window.addEventListener('beforeunload', () => {
    wsManager.destroy();
});

// =============================================================================
// EXPORT FOR MODULE SYSTEM
// =============================================================================

if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        WebSocketManager,
        wsManager,
        connectLogWebSocket,
        connectStrategyWebSocket,
        disconnectLogWebSocket,
        disconnectStrategyWebSocket,
        disconnectAllWebSockets
    };
}
