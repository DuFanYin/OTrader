// Strategy Holdings Page Controller

class StrategyHoldingsPage {
    constructor() {
        this.holdingsTable = null;
        this.selectedHolding = null;
        this.initialized = false;
    }

    init() {
        this.setupEventListeners();
        this.loadHoldingsData();
        this.initialized = true;
    }


    setupEventListeners() {
        document.getElementById('refresh-holdings-btn').addEventListener('click', () => {
            this.loadHoldingsData();
        });
    }

    async loadHoldingsData() {
        const refreshBtn = document.getElementById('refresh-holdings-btn');
        try {
            refreshBtn.disabled = true;
            refreshBtn.textContent = 'Loading...';
            const response = await apiClient.getStrategyHoldings();
            this.renderHoldingsData(response);
        } catch (error) {
            handleError(error, { context: 'loadHoldingsData' });
        } finally {
            refreshBtn.disabled = false;
            refreshBtn.textContent = 'Refresh';
        }
    }

    renderHoldingsData(data) {
        if (data.error) {
            document.getElementById('holdings-container').innerHTML = `<p class="text-error">Error: ${data.error}</p>`;
            return;
        }

        // Check for debug information
        if (data.debug) {
            const { strategies, holdings } = data.debug;
            
            if (strategies && strategies.length === 0) {
                document.getElementById('holdings-container').innerHTML = 
                    '<p class="text-dark-text-secondary text-center py-4">No strategies found - Please add and start strategies first</p>';
                return;
            }
            
            if (strategies && strategies.length > 0 && holdings && holdings.length === 0) {
                document.getElementById('holdings-container').innerHTML = 
                    `<p class="text-dark-text-secondary text-center py-4">Strategies found (${strategies.join(', ')}) but no holdings created yet - Strategies may need to be initialized or take positions</p>`;
                return;
            }
        }

        this.renderHoldingsCards(data.holdings);
    }

    renderHoldingsCards(holdings) {
        const container = document.getElementById('holdings-container');
        
        if (!container) {
            console.error('holdings-container element not found!');
            return;
        }
        
        container.innerHTML = '';

        if (!holdings || Object.keys(holdings).length === 0) {
            container.innerHTML = '<p class="text-dark-text-secondary text-center py-4">No holdings found - System may not be started or no strategies are running</p>';
            return;
        }

        this.renderHoldingsTree(holdings);
    }

    renderHoldingsTree(holdings) {
        const container = document.getElementById('holdings-container');
        
        // Create table container
        const tableContainer = document.createElement('div');
        tableContainer.className = 'bg-dark-bg rounded-lg border border-dark-border overflow-x-auto';
        
        // Create table
        const table = document.createElement('table');
        table.className = 'w-full font-mono text-sm';
        
        // Create table header
        const thead = document.createElement('thead');
        thead.innerHTML = `
            <tr class="border-b border-dark-border">
                <th class="text-left py-2 px-3 font-semibold text-white">Strategy / Position</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Qty</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Cost</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Value</th>
                <th class="text-right py-2 px-3 font-semibold text-white">PnL</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Delta</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Gamma</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Theta</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Vega</th>
            </tr>
        `;
        table.appendChild(thead);

        // Create table body
        const tbody = document.createElement('tbody');

        // Process each strategy
        Object.entries(holdings).forEach(([strategyName, holding]) => {
            // Strategy header
            const strategyRow = this.createTableRow(strategyName, 'STRATEGY', null, true);
            tbody.appendChild(strategyRow);

            let hasPositions = false;

            // Add underlying position
            if (holding.underlying && holding.underlying.quantity !== 0) {
                hasPositions = true;
                const underlyingRow = this.createTableRow('├─ Underlying', holding.underlying.symbol, holding.underlying, false, 1);
                tbody.appendChild(underlyingRow);
            }

            // Add option positions
            if (holding.options && Array.isArray(holding.options)) {
                holding.options.forEach((option, index) => {
                    if (option.quantity !== 0) {
                        hasPositions = true;
                        const isLastOption = index === holding.options.length - 1;
                        const prefix = isLastOption ? '└─' : '├─';
                        const optionRow = this.createTableRow(`${prefix} Option`, option.symbol, option, false, 1);
                        tbody.appendChild(optionRow);
                    }
                });
            }

            // Add combo positions with legs
            if (holding.combos && Array.isArray(holding.combos)) {
                holding.combos.forEach((combo, comboIndex) => {
                    if (combo.quantity !== 0) {
                        hasPositions = true;
                        
                        // Main combo row
                        const isLastCombo = comboIndex === holding.combos.length - 1;
                        const comboPrefix = isLastCombo ? '└─' : '├─';
                        const comboRow = this.createTableRow(`${comboPrefix} Combo`, combo.symbol || combo.combo_name, combo, false, 1);
                        tbody.appendChild(comboRow);
                        
                        // Add combo legs as sub-rows
                        if (combo.legs && Array.isArray(combo.legs)) {
                            combo.legs.forEach((leg, legIndex) => {
                                const isLastLeg = legIndex === combo.legs.length - 1;
                                const legPrefix = isLastLeg ? '└─' : '├─';
                                const legRow = this.createTableRow(`${legPrefix} Leg`, leg.symbol, leg, false, 2);
                                tbody.appendChild(legRow);
                            });
                        }
                    }
                });
            }

            // Add empty state if no positions
            if (!hasPositions) {
                const emptyRow = this.createTableRow('└─ No Positions', '', null, false, 1);
                tbody.appendChild(emptyRow);
            }

            // Add strategy summary
            if (holding.summary) {
                const summaryRow = this.createTableRow('└─ TOTAL', 'SUMMARY', holding.summary, false, 1, true);
                tbody.appendChild(summaryRow);
            }

            // Add spacing between strategies
            const spacerRow = document.createElement('tr');
            spacerRow.innerHTML = '<td colspan="9" class="h-2"></td>';
            tbody.appendChild(spacerRow);
        });

        table.appendChild(tbody);
        tableContainer.appendChild(table);
        container.appendChild(tableContainer);
    }

    createTableRow(label, symbol, position, isStrategy = false, indentLevel = 0, isSummary = false) {
        const row = document.createElement('tr');
        
        // Set row styling
        if (isStrategy) {
            row.className = 'border-b border-dark-border';
        } else if (isSummary) {
            row.className = 'bg-gray-800 font-semibold';
        } else {
            row.className = 'hover:bg-gray-800';
        }

        // Calculate indentation using Tailwind classes
        let indentClass = '';
        if (indentLevel === 1) {
            indentClass = 'pl-12'; // 48px indentation for first level (positions)
        } else if (indentLevel === 2) {
            indentClass = 'pl-16'; // 64px indentation for second level (combo legs)
        }
        
        // Use pre-formatted data from backend
        let qty = '-';
        let cost = '-';
        let value = '-';
        let pnl = '-';
        let delta = '-';
        let gamma = '-';
        let theta = '-';
        let vega = '-';
        let pnlClass = 'text-white';

        if (position) {
            qty = position.quantity || 0;
            cost = this.formatCurrency(position.cost_value || 0);
            value = this.formatCurrency(position.current_value || 0);
            pnl = this.formatCurrency(position.realized_pnl || 0);
            delta = this.formatNumber(position.delta || 0);
            gamma = this.formatNumber(position.gamma || 0);
            theta = this.formatNumber(position.theta || 0);
            vega = this.formatNumber(position.vega || 0);
            pnlClass = (position.realized_pnl || 0) >= 0 ? 'text-green-400' : 'text-red-400';
        } else if (isStrategy) {
            // For strategy rows, show empty cells instead of dashes
            qty = '';
            cost = '';
            value = '';
            pnl = '';
            delta = '';
            gamma = '';
            theta = '';
            vega = '';
        }

        // Set text styling based on row type
        let textClass = 'text-white';
        if (isStrategy) {
            textClass = 'text-accent font-bold';
        }

        row.innerHTML = `
            <td class="text-left py-2 px-3 ${textClass} ${indentClass}">${label} ${symbol || ''}</td>
            <td class="text-right py-2 px-3 text-white">${qty}</td>
            <td class="text-right py-2 px-3 text-white">${cost}</td>
            <td class="text-right py-2 px-3 text-white">${value}</td>
            <td class="text-right py-2 px-3 ${pnlClass}">${pnl}</td>
            <td class="text-right py-2 px-3 text-white">${delta}</td>
            <td class="text-right py-2 px-3 text-white">${gamma}</td>
            <td class="text-right py-2 px-3 text-white">${theta}</td>
            <td class="text-right py-2 px-3 text-white">${vega}</td>
        `;

        return row;
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

    show() {
        // Initialize if not done yet
        if (!this.initialized) {
            this.init();
        }
        this.loadHoldingsData();
    }

    hide() {
        // No need to hide since it's a full page
    }
}
