// Error Handler - Centralized error handling with user-friendly messages and loading states
// Provides structured error handling, loading masks, and consistent user feedback

class ErrorHandler {
    constructor() {
        this.loadingOverlays = new Map();
        this.errorCounts = new Map();
        this.maxErrorsPerMinute = 10;
        this.errorWindow = 60000; // 1 minute
    }

    // =============================================================================
    // ERROR HANDLING
    // =============================================================================

    handleError(error, context = {}) {
        console.error('Error occurred:', error, context);
        
        // Rate limiting for error notifications
        if (this.shouldThrottleError(context.key)) {
            return;
        }
        
        // Determine error type and message
        const errorInfo = this.categorizeError(error);
        
        // Show user-friendly error message
        this.showError(errorInfo.message, errorInfo.type);
        
        // Dispatch error event
        this.dispatchErrorEvent(error, context, errorInfo);
        
        // Log error for debugging
        this.logError(error, context, errorInfo);
    }

    categorizeError(error) {
        // API Errors
        if (error instanceof ApiError) {
            return this.handleApiError(error);
        }
        
        // Network Errors
        if (error.name === 'TypeError' && error.message.includes('fetch')) {
            return {
                type: 'network',
                message: ERROR_MESSAGES.NETWORK_ERROR,
                severity: 'high'
            };
        }
        
        // Validation Errors
        if (error.name === 'ValidationError') {
            return {
                type: 'validation',
                message: error.message,
                severity: 'medium'
            };
        }
        
        // Timeout Errors
        if (error.name === 'TimeoutError') {
            return {
                type: 'timeout',
                message: ERROR_MESSAGES.TIMEOUT,
                severity: 'medium'
            };
        }
        
        // Generic Errors
        return {
            type: 'generic',
            message: ERROR_MESSAGES.GENERIC,
            severity: 'medium'
        };
    }

    handleApiError(error) {
        const status = error.status;
        
        switch (status) {
            case 400:
                return {
                    type: 'validation',
                    message: ERROR_MESSAGES.VALIDATION_ERROR,
                    severity: 'medium'
                };
            case 401:
                return {
                    type: 'auth',
                    message: ERROR_MESSAGES.UNAUTHORIZED,
                    severity: 'high'
                };
            case 404:
                return {
                    type: 'notfound',
                    message: ERROR_MESSAGES.NOT_FOUND,
                    severity: 'medium'
                };
            case 500:
            case 502:
            case 503:
            case 504:
                return {
                    type: 'server',
                    message: ERROR_MESSAGES.SERVER_ERROR,
                    severity: 'high'
                };
            default:
                return {
                    type: 'api',
                    message: error.message || ERROR_MESSAGES.GENERIC,
                    severity: 'medium'
                };
        }
    }

    // =============================================================================
    // LOADING STATE MANAGEMENT
    // =============================================================================

    showLoading(key, message = 'Loading...') {
        // Remove existing overlay if any
        this.hideLoading(key);
        
        // Create loading overlay
        const overlay = document.createElement('div');
        overlay.className = 'fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center z-50';
        overlay.id = `loading-overlay-${key}`;
        
        const content = document.createElement('div');
        content.className = 'bg-dark-surface rounded-lg p-6 text-center';
        
        // Spinner
        const spinner = document.createElement('div');
        spinner.className = 'animate-spin rounded-full h-8 w-8 border-b-2 border-accent mx-auto mb-4';
        
        // Message
        const messageEl = document.createElement('p');
        messageEl.className = 'text-white text-sm';
        messageEl.textContent = message;
        
        content.appendChild(spinner);
        content.appendChild(messageEl);
        overlay.appendChild(content);
        
        document.body.appendChild(overlay);
        this.loadingOverlays.set(key, overlay);
    }

    hideLoading(key) {
        const overlay = this.loadingOverlays.get(key);
        if (overlay && overlay.parentNode) {
            overlay.parentNode.removeChild(overlay);
            this.loadingOverlays.delete(key);
        }
    }

    hideAllLoading() {
        for (const [key, overlay] of this.loadingOverlays) {
            if (overlay && overlay.parentNode) {
                overlay.parentNode.removeChild(overlay);
            }
        }
        this.loadingOverlays.clear();
    }

    // =============================================================================
    // NOTIFICATION SYSTEM
    // =============================================================================

    showError(message, type = 'error') {
        this.showNotification(message, type, APP_CONSTANTS.UI.NOTIFICATION_DURATION.ERROR);
    }

    showSuccess(message) {
        this.showNotification(message, 'success', APP_CONSTANTS.UI.NOTIFICATION_DURATION.SUCCESS);
    }

    showWarning(message) {
        this.showNotification(message, 'warning', APP_CONSTANTS.UI.NOTIFICATION_DURATION.WARNING);
    }

    showInfo(message) {
        this.showNotification(message, 'info', APP_CONSTANTS.UI.NOTIFICATION_DURATION.INFO);
    }

    showNotification(message, type = 'info', duration = 3000) {
        // Remove existing notifications of the same type
        this.removeNotificationsByType(type);
        
        const notification = document.createElement('div');
        notification.className = `fixed top-1/2 left-1/2 transform -translate-x-1/2 -translate-y-1/2 p-4 rounded-lg shadow-lg z-50 transition-all duration-300 max-w-sm`;
        
        // Set type-specific styling
        const typeClasses = {
            'success': 'bg-success text-white',
            'error': 'bg-error text-white',
            'warning': 'bg-warning text-white',
            'info': 'bg-accent text-white'
        };
        
        const typeClass = typeClasses[type] || typeClasses['error'];
        notification.classList.add(...typeClass.split(' '));
        
        // Add icon
        const icons = {
            'success': '✓',
            'error': '✗',
            'warning': '⚠',
            'info': 'ℹ'
        };
        
        notification.innerHTML = `
            <div class="flex items-center">
                <span class="mr-2 text-lg">${icons[type]}</span>
                <span class="text-sm">${message}</span>
            </div>
        `;
        
        document.body.appendChild(notification);
        
        // Auto remove
        setTimeout(() => {
            if (notification.parentNode) {
                notification.style.opacity = '0';
                setTimeout(() => {
                    if (notification.parentNode) {
                        notification.parentNode.removeChild(notification);
                    }
                }, 300);
            }
        }, duration);
    }

    removeNotificationsByType(type) {
        const notifications = document.querySelectorAll('.fixed.top-1\\/2.left-1\\/2');
        notifications.forEach(notification => {
            if (notification.classList.contains(`bg-${type}`) || 
                notification.classList.contains(`text-${type}`)) {
                if (notification.parentNode) {
                    notification.parentNode.removeChild(notification);
                }
            }
        });
    }

    // =============================================================================
    // ERROR RATE LIMITING
    // =============================================================================

    shouldThrottleError(key) {
        const now = Date.now();
        const errorKey = key || 'global';
        
        if (!this.errorCounts.has(errorKey)) {
            this.errorCounts.set(errorKey, []);
        }
        
        const errors = this.errorCounts.get(errorKey);
        
        // Remove old errors outside the window
        const recentErrors = errors.filter(time => now - time < this.errorWindow);
        this.errorCounts.set(errorKey, recentErrors);
        
        // Check if we've exceeded the limit
        if (recentErrors.length >= this.maxErrorsPerMinute) {
            console.warn(`Error rate limit exceeded for key: ${errorKey}`);
            return true;
        }
        
        // Add current error
        recentErrors.push(now);
        this.errorCounts.set(errorKey, recentErrors);
        
        return false;
    }

    // =============================================================================
    // EVENT SYSTEM
    // =============================================================================

    dispatchErrorEvent(error, context, errorInfo) {
        const event = new CustomEvent(APP_CONSTANTS.EVENTS.ERROR_OCCURRED, {
            detail: {
                error,
                context,
                errorInfo,
                timestamp: new Date().toISOString()
            }
        });
        document.dispatchEvent(event);
    }

    // =============================================================================
    // LOGGING
    // =============================================================================

    logError(error, context, errorInfo) {
        const logEntry = {
            timestamp: new Date().toISOString(),
            error: {
                name: error.name,
                message: error.message,
                stack: error.stack
            },
            context,
            errorInfo,
            userAgent: navigator.userAgent,
            url: window.location.href
        };
        
        // Log to console in development
        console.group('Error Details');
        console.error('Error:', error);
        console.groupEnd();
        
        // Could send to external logging service here
        // this.sendToLoggingService(logEntry);
    }

    // =============================================================================
    // UTILITY METHODS
    // =============================================================================

    async withLoading(key, asyncFunction, loadingMessage = 'Loading...') {
        try {
            this.showLoading(key, loadingMessage);
            const result = await asyncFunction();
            return result;
        } catch (error) {
            this.handleError(error, { key });
            throw error;
        } finally {
            this.hideLoading(key);
        }
    }

    async withErrorHandling(asyncFunction, context = {}) {
        try {
            return await asyncFunction();
        } catch (error) {
            this.handleError(error, context);
            throw error;
        }
    }
}

// =============================================================================
// GLOBAL INSTANCE
// =============================================================================

const errorHandler = new ErrorHandler();

// =============================================================================
// CONVENIENCE FUNCTIONS
// =============================================================================

// Global error handling functions
function handleError(error, context = {}) {
    return errorHandler.handleError(error, context);
}

function showError(message) {
    return errorHandler.showError(message);
}

function showSuccess(message) {
    return errorHandler.showSuccess(message);
}

function showWarning(message) {
    return errorHandler.showWarning(message);
}

function showInfo(message) {
    return errorHandler.showInfo(message);
}

function showLoading(key, message) {
    return errorHandler.showLoading(key, message);
}

function hideLoading(key) {
    return errorHandler.hideLoading(key);
}

function withLoading(key, asyncFunction, loadingMessage) {
    return errorHandler.withLoading(key, asyncFunction, loadingMessage);
}

function withErrorHandling(asyncFunction, context) {
    return errorHandler.withErrorHandling(asyncFunction, context);
}

// =============================================================================
// GLOBAL ERROR HANDLERS
// =============================================================================

// Handle unhandled promise rejections
window.addEventListener('unhandledrejection', (event) => {
    console.error('Unhandled promise rejection:', event.reason);
    errorHandler.handleError(event.reason, { type: 'unhandledPromise' });
});

// Handle global JavaScript errors
window.addEventListener('error', (event) => {
    console.error('Global error:', event.error);
    errorHandler.handleError(event.error, { 
        type: 'globalError',
        filename: event.filename,
        lineno: event.lineno,
        colno: event.colno
    });
});

// =============================================================================
// EXPORT FOR MODULE SYSTEM
// =============================================================================

if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        ErrorHandler,
        errorHandler,
        handleError,
        showError,
        showSuccess,
        showWarning,
        showInfo,
        showLoading,
        hideLoading,
        withLoading,
        withErrorHandling
    };
}
