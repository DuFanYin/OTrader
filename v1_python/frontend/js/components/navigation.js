// Navigation Component
class Navigation {
    constructor() {
        this.currentPage = this.getCurrentPage();
    }

    getCurrentPage() {
        const path = window.location.pathname;
        if (path.includes('strategy/strategy-manager')) return 'strategy-manager';
        if (path.includes('strategy/strategy-holding')) return 'strategy-holding';
        if (path.includes('admin/orders-trades')) return 'orders-trades';
        return 'strategy-manager'; // default
    }

    createHeader() {
        const header = document.createElement('header');
        header.className = 'bg-dark-surface border-b border-dark-border px-6 py-4';
        
        header.innerHTML = `
            <div class="flex items-center justify-between">
                <div class="flex items-center space-x-8">
                    <h1 class="text-2xl font-bold text-white">OTrader</h1>
                    
                    <!-- Navigation Links -->
                    <nav class="flex space-x-1 bg-dark-bg rounded-lg p-1">
                        <a href="/strategy/strategy-manager.html" 
                           class="nav-link px-4 py-2 rounded-md text-sm font-medium transition-colors duration-200 ${this.currentPage === 'strategy-manager' ? 'bg-accent text-white' : 'text-dark-text-secondary hover:text-white'}">
                            Strategy Manager
                        </a>
                        <a href="/strategy/strategy-holding.html" 
                           class="nav-link px-4 py-2 rounded-md text-sm font-medium transition-colors duration-200 ${this.currentPage === 'strategy-holding' ? 'bg-accent text-white' : 'text-dark-text-secondary hover:text-white'}">
                            Strategy Holding
                        </a>
                        <a href="/admin/orders-trades.html" 
                           class="nav-link px-4 py-2 rounded-md text-sm font-medium transition-colors duration-200 ${this.currentPage === 'orders-trades' ? 'bg-accent text-white' : 'text-dark-text-secondary hover:text-white'}">
                            Orders & Trades
                        </a>
                    </nav>
                </div>
                
                <div class="flex items-center space-x-4">
                    <div id="system-status" class="flex items-center space-x-2">
                        <div class="w-3 h-3 bg-gray-500 rounded-full status-indicator" id="status-indicator"></div>
                        <span class="text-sm text-dark-text-secondary" id="status-text">System Status</span>
                    </div>
                </div>
            </div>
        `;

        return header;
    }

    updateSystemStatus(data) {
        const statusIndicator = document.getElementById('status-indicator');
        const statusText = document.getElementById('status-text');
        
        if (!statusIndicator || !statusText) return;

        if (data.status === 'running') {
            statusIndicator.className = data.connected ? 
                'w-3 h-3 bg-success rounded-full status-indicator' : 
                'w-3 h-3 bg-warning rounded-full status-indicator';
            statusText.textContent = data.connected ? 'Connected' : 'Disconnected';
        } else {
            statusIndicator.className = 'w-3 h-3 bg-error rounded-full status-indicator';
            statusText.textContent = 'Stopped';
        }
    }

    async updateStatus() {
        try {
            const response = await fetch('/api/gateway/status');
            const data = await response.json();
            this.updateSystemStatus(data);
        } catch (error) {
            console.error('Failed to update system status:', error);
            const statusIndicator = document.getElementById('status-indicator');
            const statusText = document.getElementById('status-text');
            if (statusIndicator && statusText) {
                statusIndicator.className = 'w-3 h-3 bg-error rounded-full status-indicator';
                statusText.textContent = 'Error';
            }
        }
    }

    startPeriodicUpdates() {
        // Update system status every 5 seconds
        setInterval(() => {
            this.updateStatus();
        }, 5000);
        
        // Initial status update
        this.updateStatus();
    }
}

// Export for module system
if (typeof module !== 'undefined' && module.exports) {
    module.exports = Navigation;
}
