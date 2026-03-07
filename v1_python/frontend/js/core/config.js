// Frontend Configuration
// Centralized configuration for API endpoints, constants, and settings

// =============================================================================
// API CONFIGURATION
// =============================================================================

const API_CONFIG = {
    BASE_URL: '/api',
    ENDPOINTS: {
        // Gateway endpoints (renamed from system)
        GATEWAY: {
            CONNECT: '/gateway/connect',
            DISCONNECT: '/gateway/disconnect',
            STATUS: '/gateway/status'
        },
        
        // Strategy endpoints
        STRATEGIES: {
            LIST: '/strategies',
            UPDATES: '/strategies/updates',
            CLEAR_UPDATES: '/strategies/updates/clear',
            ADD: '/strategies',
            RESTORE: '/strategies/restore',
            INIT: (name) => `/strategies/${name}/init`,
            START: (name) => `/strategies/${name}/start`,
            STOP: (name) => `/strategies/${name}/stop`,
            REMOVE: (name) => `/strategies/${name}/remove`,
            DELETE: (name) => `/strategies/${name}/delete`,
            HOLDINGS: '/strategies/holdings',
            META: {
                STRATEGY_CLASSES: '/strategies/meta/strategy-classes',
                PORTFOLIOS: '/strategies/meta/portfolios',
                REMOVED_STRATEGIES: '/strategies/meta/removed-strategies'
            }
        },
        
        // Data endpoints (only orders-trades exists)
        DATA: {
            ORDERS_TRADES: '/orders-trades',
            PORTFOLIOS: '/data/portfolios'
        },
        
        // Log endpoints
        LOGS: {
            LIST: '/logs'
        }
    },
    
    // WebSocket endpoints
    WEBSOCKET: {
        LOGS: '/ws/logs',
        STRATEGIES: '/ws/strategies'
    }
};

// =============================================================================
// APPLICATION CONSTANTS
// =============================================================================

const APP_CONSTANTS = {
    // Status indicators
    STATUS: {
        RUNNING: 'running',
        STOPPED: 'stopped',
        INITIALIZED: 'initialized',
        ERROR: 'error',
        CONNECTED: 'connected',
        DISCONNECTED: 'disconnected',
        ACTIVE: 'active'
    },
    
    // Event names
    EVENTS: {
        SYSTEM_STARTED: 'system:started',
        SYSTEM_STOPPED: 'system:stopped',
        SYSTEM_CONNECTED: 'system:connected',
        SYSTEM_DISCONNECTED: 'system:disconnected',
        STRATEGY_ADDED: 'strategy:added',
        STRATEGY_REMOVED: 'strategy:removed',
        STRATEGY_STARTED: 'strategy:started',
        STRATEGY_STOPPED: 'strategy:stopped',
        LOG_MESSAGE: 'log:message',
        ERROR_OCCURRED: 'error:occurred',
        LOADING_START: 'loading:start',
        LOADING_END: 'loading:end'
    },
    
    // UI Constants
    UI: {
        NOTIFICATION_DURATION: {
            SUCCESS: 3000,
            ERROR: 5000,
            WARNING: 4000,
            INFO: 3000
        },
        MAX_LOG_ENTRIES: 100,
        RECONNECT_ATTEMPTS: 5,
        RECONNECT_DELAY: 3000
    },
    
    // Currency formatting
    CURRENCY: {
        LOCALE: 'en-US',
        CURRENCY: 'USD',
        DECIMAL_PLACES: 2
    }
};

// =============================================================================
// ERROR MESSAGES
// =============================================================================

const ERROR_MESSAGES = {
    NETWORK_ERROR: 'Network error occurred. Please check your connection.',
    SERVER_ERROR: 'Server error occurred. Please try again later.',
    VALIDATION_ERROR: 'Validation error. Please check your input.',
    UNAUTHORIZED: 'Unauthorized access. Please log in.',
    NOT_FOUND: 'Resource not found.',
    TIMEOUT: 'Request timeout. Please try again.',
    GENERIC: 'An unexpected error occurred.',
    
    // Specific error messages
    SYSTEM_START_FAILED: 'Failed to start system',
    SYSTEM_CONNECT_FAILED: 'Failed to connect to trading gateway',
    SYSTEM_DISCONNECT_FAILED: 'Failed to disconnect from trading gateway',
    STRATEGY_ADD_FAILED: 'Failed to add strategy',
    STRATEGY_REMOVE_FAILED: 'Failed to remove strategy',
    STRATEGY_START_FAILED: 'Failed to start strategy',
    STRATEGY_STOP_FAILED: 'Failed to stop strategy',
    STRATEGY_INIT_FAILED: 'Failed to initialize strategy',
    HOLDINGS_LOAD_FAILED: 'Failed to load holdings data',
    PORTFOLIO_LOAD_FAILED: 'Failed to load portfolio data',
    DATA_STATUS_LOAD_FAILED: 'Failed to load data status'
};

// =============================================================================
// SUCCESS MESSAGES
// =============================================================================

const SUCCESS_MESSAGES = {
    SYSTEM_STARTED: 'System started successfully',
    SYSTEM_CONNECTED: 'Connected to trading gateway',
    SYSTEM_DISCONNECTED: 'Disconnected from trading gateway',
    STRATEGY_ADDED: 'Strategy added successfully',
    STRATEGY_RESTORED: 'Strategy restored successfully',
    STRATEGY_STARTED: 'Strategy started successfully',
    STRATEGY_STOPPED: 'Strategy stopped successfully',
    STRATEGY_INITIALIZED: 'Strategy initialized successfully',
    STRATEGY_REMOVED: 'Strategy removed successfully',
    STRATEGY_DELETED: 'Strategy deleted successfully',
    DATA_WIPED: 'Trading data wiped successfully',
    WEBSOCKET_CONNECTED: 'Connected to real-time updates',
    WEBSOCKET_DISCONNECTED: 'Disconnected from real-time updates'
};

// =============================================================================
// EXPORT FOR MODULE SYSTEM
// =============================================================================

if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        API_CONFIG,
        APP_CONSTANTS,
        ERROR_MESSAGES,
        SUCCESS_MESSAGES
    };
}
