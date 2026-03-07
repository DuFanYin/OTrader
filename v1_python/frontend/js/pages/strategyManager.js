// Strategy Manager Page Controller

class StrategyManagerPage {
    constructor() {
        this.selectedStrategy = null;
        this.strategyTable = null;
        this.logContainer = null;
        this.logWebSocket = null;
        this.logRefreshInterval = null;
        this.initialized = false;
    }

    init() {
        this.setupEventListeners();
        this.initControlStates();
        this.updateButtonStates();
        this.refreshStrategies();
        this.refreshLogs();
        this.connectLogWebSocket();
        this.initialized = true;
    }


    setupEventListeners() {
        // System control buttons
        const connectBtn = document.getElementById('connect-btn');
        const disconnectBtn = document.getElementById('disconnect-btn');
        
        connectBtn.addEventListener('click', () => {
            this.connectSystem();
        });
        disconnectBtn.addEventListener('click', () => this.disconnectSystem());

        // Strategy management buttons
        document.getElementById('add-strategy-btn').addEventListener('click', () => this.showAddStrategyDialog());
        document.getElementById('restore-strategy-btn').addEventListener('click', () => this.showRestoreStrategyDialog());

        // Strategy action buttons
        document.getElementById('init-strategy-btn').addEventListener('click', () => this.initStrategy());
        document.getElementById('start-strategy-btn').addEventListener('click', () => this.startStrategy());
        document.getElementById('stop-strategy-btn').addEventListener('click', () => this.stopStrategy());
        document.getElementById('remove-strategy-btn').addEventListener('click', () => this.removeStrategy());
        document.getElementById('delete-strategy-btn').addEventListener('click', () => this.deleteStrategy());

        // Log buttons
        document.getElementById('clear-logs-btn').addEventListener('click', () => this.clearLogs());
        document.getElementById('refresh-logs-btn').addEventListener('click', () => this.refreshLogs());
    }

    initControlStates() {
        this.logContainer = document.getElementById('log-container');
        this.strategyTable = document.getElementById('strategy-table-container');
    }

    async updateButtonStates() {
        try {
            const response = await apiClient.getSystemStatus();
            const isSystemStarted = response.status === APP_CONSTANTS.STATUS.RUNNING;
            const isConnected = response.connected;

            // System control buttons
            const connectBtn = document.getElementById('connect-btn');
            const disconnectBtn = document.getElementById('disconnect-btn');
            
            connectBtn.disabled = !isSystemStarted || isConnected;
            disconnectBtn.disabled = !isSystemStarted || !isConnected;

            // Strategy management buttons
            document.getElementById('add-strategy-btn').disabled = !isSystemStarted;
            document.getElementById('restore-strategy-btn').disabled = !isSystemStarted;

            // Strategy action buttons
            const hasSelection = this.selectedStrategy !== null;
            document.getElementById('init-strategy-btn').disabled = !hasSelection;
            document.getElementById('start-strategy-btn').disabled = !hasSelection;
            document.getElementById('stop-strategy-btn').disabled = !hasSelection;
            document.getElementById('remove-strategy-btn').disabled = !hasSelection;
            document.getElementById('delete-strategy-btn').disabled = !hasSelection;

        } catch (error) {
            handleError(error, { context: 'updateButtonStates' });
        }
    }


    async connectSystem() {
        try {
            await apiClient.connectSystem();
            await this.updateButtonStates();
        } catch (error) {
            handleError(error, { context: 'connectSystem' });
        }
    }

    async disconnectSystem() {
        try {
            await apiClient.disconnectSystem();
            await this.updateButtonStates();
        } catch (error) {
            handleError(error, { context: 'disconnectSystem' });
        }
    }

    async loadStrategyClasses() {
        try {
            const response = await apiClient.getStrategyClasses();
            const select = document.getElementById('strategy-class-select');
            if (select) {
                select.innerHTML = '<option value="">Select Strategy...</option>';
                
                response.classes.forEach(className => {
                    const option = document.createElement('option');
                    option.value = className;
                    option.textContent = className;
                    select.appendChild(option);
                });
            }
            return response.classes;
        } catch (error) {
            handleError(error, { context: 'loadStrategyClasses' });
            return [];
        }
    }

    async loadPortfolios() {
        try {
            const response = await apiClient.getPortfolioNames();
            const select = document.getElementById('portfolio-select');
            if (select) {
                select.innerHTML = '<option value="">Select Portfolio...</option>';
                
                response.portfolios.forEach(portfolio => {
                    const option = document.createElement('option');
                    option.value = portfolio;
                    option.textContent = portfolio;
                    select.appendChild(option);
                });
            }
            return response.portfolios;
        } catch (error) {
            handleError(error, { context: 'loadPortfolios' });
            return [];
        }
    }

    async showAddStrategyDialog() {
        try {
            // Load options for dropdowns first
            const [classes, portfolios] = await Promise.all([
                this.loadStrategyClasses(),
                this.loadPortfolios()
            ]);


            const fields = [
                {
                    name: 'strategy_class',
                    label: 'Strategy Class',
                    type: 'select',
                    required: true,
                    options: classes.length > 0 ? classes.map(c => ({ value: c, text: c })) : []
                },
                {
                    name: 'portfolio_name',
                    label: 'Portfolio',
                    type: portfolios.length > 0 ? 'select' : 'text',
                    required: true,
                    options: portfolios.length > 0 ? portfolios.map(p => ({ value: p, text: p })) : undefined,
                    placeholder: portfolios.length > 0 ? undefined : 'Enter portfolio name (e.g., AAPL, TSLA)'
                }
            ];

            showInputDialog('Add Strategy', fields, (data) => {
                this.addStrategy(data);
            });
        } catch (error) {
            handleError(error, { context: 'showAddStrategyDialog' });
        }
    }

    async showRestoreStrategyDialog() {
        try {
            // Load removed strategies first
            const response = await apiClient.getRemovedStrategies();
            
            const fields = [
                {
                    name: 'strategy_name',
                    label: 'Strategy Name',
                    type: 'select',
                    required: true,
                    options: response.removed_strategies.map(s => ({ value: s, text: s }))
                }
            ];

            showInputDialog('Restore Strategy', fields, (data) => {
                this.restoreStrategy(data);
            });
        } catch (error) {
            handleError(error, { context: 'showRestoreStrategyDialog' });
        }
    }

    async addStrategy(data) {
        try {
            const requestData = {
                strategy_class: data.strategy_class,
                portfolio_name: data.portfolio_name,
                setting: {}
            };
            
            const response = await apiClient.addStrategy(requestData);
            
            showSuccess(SUCCESS_MESSAGES.STRATEGY_ADDED);
            await this.refreshStrategies();
        } catch (error) {
            handleError(error, { context: 'addStrategy' });
        }
    }

    async restoreStrategy(data) {
        try {
            await apiClient.restoreStrategy({
                strategy_name: data.strategy_name
            });
            showSuccess(SUCCESS_MESSAGES.STRATEGY_RESTORED);
            await this.refreshStrategies();
        } catch (error) {
            handleError(error, { context: 'restoreStrategy' });
        }
    }

    async refreshStrategies() {
        try {
            const response = await apiClient.getStrategies();
            this.renderStrategyTable(response.strategies);
        } catch (error) {
            handleError(error, { context: 'refreshStrategies' });
        }
    }

    renderStrategyTable(strategies) {
        const container = this.strategyTable;
        container.innerHTML = '';

        // Always create table with headers, even when no strategies exist
        const headers = ['Status', 'Strategy Name', 'Underlying', 'PnL', 'Net Value', 'Cost', 'Delta', 'Gamma', 'Theta', 'Vega'];
        
        // Create table data - empty array if no strategies
        const tableData = strategies.length === 0 ? [] : strategies.map(strategy => {
            const [strategyPart, underlying] = this.splitStrategyName(strategy.strategy_name || '');
            return [
                this.getStatusIndicator(strategy),
                strategyPart || 'N/A',
                underlying || 'N/A',
                this.formatCurrency(strategy.pnl || 0),
                this.formatCurrency(strategy.current_value || 0),
                this.formatCurrency(strategy.total_cost || 0),
                this.formatNumber(strategy.delta || 0),
                this.formatNumber(strategy.gamma || 0),
                this.formatNumber(strategy.theta || 0),
                this.formatNumber(strategy.vega || 0)
            ];
        });

        const table = createTable(headers, tableData);

        // Make rows clickable for selection (only if strategies exist)
        if (strategies.length > 0) {
            table.addEventListener('click', (e) => {
                const row = e.target.closest('tr');
                if (row && row.rowIndex > 0) { // Skip header row
                    const strategyName = strategies[row.rowIndex - 1].strategy_name;
                    this.selectStrategy(strategyName);
                }
            });
        }

        container.appendChild(table);
        
        // Store strategies for selection
        this.currentStrategies = strategies;
    }

    selectStrategy(strategyName) {
        this.selectedStrategy = strategyName;
        this.updateButtonStates();
        
        // Update visual selection
        const table = this.strategyTable.querySelector('table');
        if (table && this.currentStrategies && this.currentStrategies.length > 0) {
            table.querySelectorAll('tr').forEach((row, index) => {
                if (index > 0) { // Skip header
                    row.classList.remove('bg-accent', 'bg-opacity-20');
                    if (this.currentStrategies[index - 1].strategy_name === strategyName) {
                        row.classList.add('bg-accent', 'bg-opacity-20');
                    }
                }
            });
        }
    }

    getStatusIndicator(strategy) {
        if (strategy.error) return 'Error';
        if (strategy.started) return 'Running';
        if (strategy.inited) return 'Initialized';
        return 'Stopped';
    }

    splitStrategyName(strategyName) {
        if (strategyName.includes('_')) {
            const parts = strategyName.split('_');
            if (parts.length >= 2) {
                const underlying = parts[parts.length - 1];
                const strategyPart = parts.slice(0, -1).join('_');
                return [strategyPart, underlying];
            }
        }
        return [strategyName, ''];
    }

    formatCurrency(value) {
        if (value === null || value === undefined) return '$0.00';
        return new Intl.NumberFormat('en-US', {
            style: 'currency',
            currency: 'USD'
        }).format(value);
    }

    formatNumber(value) {
        if (value === null || value === undefined) return '0.00';
        return parseFloat(value).toFixed(2);
    }

    async initStrategy() {
        if (!this.selectedStrategy) {
            showError('Please select a strategy');
            return;
        }

        try {
            await apiClient.initStrategy(this.selectedStrategy);
            showSuccess(SUCCESS_MESSAGES.STRATEGY_INITIALIZED);
            await this.refreshStrategies();
        } catch (error) {
            handleError(error, { context: 'initStrategy', strategy: this.selectedStrategy });
        }
    }

    async startStrategy() {
        if (!this.selectedStrategy) {
            showError('Please select a strategy');
            return;
        }

        try {
            await apiClient.startStrategy(this.selectedStrategy);
            showSuccess(SUCCESS_MESSAGES.STRATEGY_STARTED);
            await this.refreshStrategies();
        } catch (error) {
            handleError(error, { context: 'startStrategy', strategy: this.selectedStrategy });
        }
    }

    async stopStrategy() {
        if (!this.selectedStrategy) {
            showError('Please select a strategy');
            return;
        }

        try {
            await apiClient.stopStrategy(this.selectedStrategy);
            showSuccess(SUCCESS_MESSAGES.STRATEGY_STOPPED);
            await this.refreshStrategies();
        } catch (error) {
            handleError(error, { context: 'stopStrategy', strategy: this.selectedStrategy });
        }
    }

    async removeStrategy() {
        if (!this.selectedStrategy) {
            showError('Please select a strategy');
            return;
        }

        const confirmed = await new Promise(resolve => {
            showConfirmDialog(
                'Remove Strategy',
                `Are you sure you want to remove strategy "${this.selectedStrategy}"?`,
                () => resolve(true),
                () => resolve(false)
            );
        });

        if (confirmed) {
            try {
                await apiClient.removeStrategy(this.selectedStrategy);
                showSuccess(SUCCESS_MESSAGES.STRATEGY_REMOVED);
                this.selectedStrategy = null;
                await this.refreshStrategies();
                await this.updateButtonStates();
            } catch (error) {
                handleError(error, { context: 'removeStrategy', strategy: this.selectedStrategy });
            }
        }
    }

    async deleteStrategy() {
        if (!this.selectedStrategy) {
            showError('Please select a strategy');
            return;
        }

        const confirmed = await new Promise(resolve => {
            showConfirmDialog(
                'Delete Strategy',
                `Are you sure you want to permanently delete strategy "${this.selectedStrategy}"? This action cannot be undone.`,
                () => resolve(true),
                () => resolve(false)
            );
        });

        if (confirmed) {
            try {
                await apiClient.deleteStrategy(this.selectedStrategy);
                showSuccess(SUCCESS_MESSAGES.STRATEGY_DELETED);
                this.selectedStrategy = null;
                await this.refreshStrategies();
                await this.updateButtonStates();
            } catch (error) {
                handleError(error, { context: 'deleteStrategy', strategy: this.selectedStrategy });
            }
        }
    }

    async refreshLogs() {
        try {
            const response = await apiClient.getLogs();
            this.renderLogs(response.logs);
        } catch (error) {
            handleError(error, { context: 'refreshLogs' });
        }
    }

    renderLogs(logs) {
        this.logContainer.innerHTML = '';
        logs.forEach(log => {
            addLogEntry(this.logContainer, log, 'info');
        });
    }

    handleLogMessage(message) {
        addLogEntry(this.logContainer, message, 'info');
    }


    clearLogs() {
        this.logContainer.innerHTML = '';
    }

    connectLogWebSocket() {
        this.logWebSocket = connectLogWebSocket({
            onMessage: (message) => this.handleLogMessage(message),
            onOpen: () => {},
            onClose: () => {},
            onError: (error) => {}
        });
    }

    disconnectLogWebSocket() {
        disconnectLogWebSocket();
        this.logWebSocket = null;
    }

    show() {
        // Initialize if not done yet
        if (!this.initialized) {
            this.init();
        }
        this.refreshStrategies();
        this.refreshLogs();
    }

    hide() {
        // Disconnect WebSocket when leaving page
        this.disconnectLogWebSocket();
    }
}
