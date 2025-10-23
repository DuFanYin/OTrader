// API Client - Centralized API management with error handling and loading states
// Replaces scattered fetch calls with a unified interface

class ApiClient {
    constructor() {
        this.baseURL = API_CONFIG.BASE_URL;
        this.defaultHeaders = {
            'Content-Type': 'application/json',
        };
        this.loadingStates = new Map();
        this.requestInterceptors = [];
        this.responseInterceptors = [];
    }

    // =============================================================================
    // REQUEST INTERCEPTORS
    // =============================================================================

    addRequestInterceptor(interceptor) {
        this.requestInterceptors.push(interceptor);
    }

    addResponseInterceptor(interceptor) {
        this.responseInterceptors.push(interceptor);
    }

    // =============================================================================
    // LOADING STATE MANAGEMENT
    // =============================================================================

    setLoading(key, isLoading) {
        this.loadingStates.set(key, isLoading);
        this.dispatchLoadingEvent(key, isLoading);
    }

    isLoading(key) {
        return this.loadingStates.get(key) || false;
    }

    dispatchLoadingEvent(key, isLoading) {
        const event = new CustomEvent(isLoading ? APP_CONSTANTS.EVENTS.LOADING_START : APP_CONSTANTS.EVENTS.LOADING_END, {
            detail: { key, isLoading }
        });
        document.dispatchEvent(event);
    }

    // =============================================================================
    // CORE REQUEST METHOD
    // =============================================================================

    async request(endpoint, options = {}) {
        const key = options.loadingKey || endpoint;
        
        try {
            // Set loading state
            this.setLoading(key, true);

            // Apply request interceptors
            let requestOptions = { ...options };
            for (const interceptor of this.requestInterceptors) {
                requestOptions = await interceptor(requestOptions);
            }

            // Merge default headers
            const headers = { ...this.defaultHeaders, ...requestOptions.headers };
            
            // Build full URL
            const url = endpoint.startsWith('http') ? endpoint : `${this.baseURL}${endpoint}`;

            // Make request
            const response = await fetch(url, {
                ...requestOptions,
                headers
            });

            // Apply response interceptors
            let processedResponse = response;
            for (const interceptor of this.responseInterceptors) {
                processedResponse = await interceptor(processedResponse);
            }

            // Handle HTTP errors
            if (!processedResponse.ok) {
                throw new ApiError(
                    `HTTP ${processedResponse.status}: ${processedResponse.statusText}`,
                    processedResponse.status,
                    processedResponse
                );
            }

            // Parse response
            const data = await processedResponse.json();
            return data;

        } catch (error) {
            // Handle different error types
            if (error instanceof ApiError) {
                throw error;
            } else if (error.name === 'TypeError' && error.message.includes('fetch')) {
                throw new ApiError(ERROR_MESSAGES.NETWORK_ERROR, 0, null, error);
            } else {
                throw new ApiError(ERROR_MESSAGES.GENERIC, 0, null, error);
            }
        } finally {
            // Clear loading state
            this.setLoading(key, false);
        }
    }

    // =============================================================================
    // CONVENIENCE METHODS
    // =============================================================================

    async get(endpoint, options = {}) {
        return this.request(endpoint, {
            method: 'GET',
            ...options
        });
    }

    async post(endpoint, data = null, options = {}) {
        return this.request(endpoint, {
            method: 'POST',
            body: data ? JSON.stringify(data) : undefined,
            ...options
        });
    }

    async put(endpoint, data = null, options = {}) {
        return this.request(endpoint, {
            method: 'PUT',
            body: data ? JSON.stringify(data) : undefined,
            ...options
        });
    }

    async delete(endpoint, options = {}) {
        return this.request(endpoint, {
            method: 'DELETE',
            ...options
        });
    }

    // =============================================================================
    // API-SPECIFIC METHODS
    // =============================================================================

    // Gateway API (renamed from System)
    async startSystem() {
        // System auto-starts, no endpoint needed
        return { status: 'ok', message: 'System auto-starts on server startup' };
    }

    async connectSystem() {
        return this.post(API_CONFIG.ENDPOINTS.GATEWAY.CONNECT, {}, {
            loadingKey: 'gateway-connect'
        });
    }

    async disconnectSystem() {
        return this.post(API_CONFIG.ENDPOINTS.GATEWAY.DISCONNECT, {}, {
            loadingKey: 'gateway-disconnect'
        });
    }

    async getSystemStatus() {
        return this.get(API_CONFIG.ENDPOINTS.GATEWAY.STATUS, {
            loadingKey: 'gateway-status'
        });
    }

    // Strategy API
    async getStrategies() {
        return this.get(API_CONFIG.ENDPOINTS.STRATEGIES.LIST, {
            loadingKey: 'strategies-list'
        });
    }

    async addStrategy(data) {
        return this.post(API_CONFIG.ENDPOINTS.STRATEGIES.ADD, data, {
            loadingKey: 'strategy-add'
        });
    }

    async restoreStrategy(data) {
        return this.post(API_CONFIG.ENDPOINTS.STRATEGIES.RESTORE, data, {
            loadingKey: 'strategy-restore'
        });
    }

    async initStrategy(strategyName) {
        return this.post(API_CONFIG.ENDPOINTS.STRATEGIES.INIT(strategyName), {}, {
            loadingKey: `strategy-init-${strategyName}`
        });
    }

    async startStrategy(strategyName) {
        return this.post(API_CONFIG.ENDPOINTS.STRATEGIES.START(strategyName), {}, {
            loadingKey: `strategy-start-${strategyName}`
        });
    }

    async stopStrategy(strategyName) {
        return this.post(API_CONFIG.ENDPOINTS.STRATEGIES.STOP(strategyName), {}, {
            loadingKey: `strategy-stop-${strategyName}`
        });
    }

    async removeStrategy(strategyName) {
        return this.delete(API_CONFIG.ENDPOINTS.STRATEGIES.REMOVE(strategyName), {
            loadingKey: `strategy-remove-${strategyName}`
        });
    }

    async deleteStrategy(strategyName) {
        return this.delete(API_CONFIG.ENDPOINTS.STRATEGIES.DELETE(strategyName), {
            loadingKey: `strategy-delete-${strategyName}`
        });
    }

    async getStrategyHoldings() {
        return this.get(API_CONFIG.ENDPOINTS.STRATEGIES.HOLDINGS, {
            loadingKey: 'strategy-holdings'
        });
    }

    async getStrategyClasses() {
        return this.get(API_CONFIG.ENDPOINTS.STRATEGIES.META.STRATEGY_CLASSES, {
            loadingKey: 'strategy-classes'
        });
    }

    async getPortfolios() {
        return this.get(API_CONFIG.ENDPOINTS.STRATEGIES.META.PORTFOLIOS, {
            loadingKey: 'portfolios'
        });
    }

    async getRemovedStrategies() {
        return this.get(API_CONFIG.ENDPOINTS.STRATEGIES.META.REMOVED_STRATEGIES, {
            loadingKey: 'removed-strategies'
        });
    }

    // Data API (only orders-trades endpoint exists)
    async getOrdersAndTrades() {
        return this.get(API_CONFIG.ENDPOINTS.DATA.ORDERS_TRADES, {
            loadingKey: 'orders-trades'
        });
    }

    async getPortfolioNames() {
        return this.get(API_CONFIG.ENDPOINTS.DATA.PORTFOLIOS, {
            loadingKey: 'portfolio-names'
        });
    }

    // Log API
    async getLogs() {
        return this.get(API_CONFIG.ENDPOINTS.LOGS.LIST, {
            loadingKey: 'logs'
        });
    }
}

// =============================================================================
// CUSTOM ERROR CLASS
// =============================================================================

class ApiError extends Error {
    constructor(message, status = 0, response = null, originalError = null) {
        super(message);
        this.name = 'ApiError';
        this.status = status;
        this.response = response;
        this.originalError = originalError;
    }
}

// =============================================================================
// GLOBAL INSTANCE
// =============================================================================

const apiClient = new ApiClient();

// =============================================================================
// EXPORT FOR MODULE SYSTEM
// =============================================================================

if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        ApiClient,
        ApiError,
        apiClient,
        apiGet,
        apiPost,
        apiDelete,
        apiRequest
    };
}
