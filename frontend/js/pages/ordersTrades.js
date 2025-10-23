// Orders & Trades Page Controller

class OrdersTradesPage {
    constructor() {
        this.ordersTradesTable = null;
        this.initialized = false;
    }

    init() {
        this.setupEventListeners();
        this.loadOrdersTradesData();
        this.initialized = true;
    }

    setupEventListeners() {
        document.getElementById('refresh-orders-trades-btn').addEventListener('click', () => {
            this.loadOrdersTradesData();
        });
    }

    async loadOrdersTradesData() {
        const refreshBtn = document.getElementById('refresh-orders-trades-btn');
        try {
            refreshBtn.disabled = true;
            refreshBtn.textContent = 'Loading...';
            const response = await apiClient.getOrdersAndTrades();
            this.renderOrdersTradesData(response);
        } catch (error) {
            handleError(error, { context: 'loadOrdersTradesData' });
        } finally {
            refreshBtn.disabled = false;
            refreshBtn.textContent = 'Refresh';
        }
    }

    renderOrdersTradesData(response) {
        if (response.error) {
            document.getElementById('orders-trades-table-container').innerHTML = 
                `<p class="text-error">Error: ${response.error}</p>`;
            return;
        }

        const records = response.records || [];
        
        this.renderOrdersTradesTable(records);
    }

    renderOrdersTradesTable(records) {
        const container = document.getElementById('orders-trades-table-container');
        
        if (!container) {
            console.error('orders-trades-table-container element not found!');
            return;
        }
        
        container.innerHTML = '';

        if (records.length === 0) {
            container.innerHTML = '<p class="text-dark-text-secondary text-center py-4">No orders and trades</p>';
            return;
        }

        // Create table container
        const tableContainer = document.createElement('div');
        tableContainer.className = 'bg-dark-surface rounded-lg border border-dark-border overflow-x-auto';
        
        // Create table
        const table = document.createElement('table');
        table.className = 'w-full font-mono text-sm';
        
        // Create table header
        const thead = document.createElement('thead');
        thead.innerHTML = `
            <tr class="border-b border-dark-border">
                <th class="text-left py-2 px-3 font-semibold text-white">Type</th>
                <th class="text-left py-2 px-3 font-semibold text-white">Time</th>
                <th class="text-left py-2 px-3 font-semibold text-white">Symbol</th>
                <th class="text-left py-2 px-3 font-semibold text-white">Side</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Quantity</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Price</th>
                <th class="text-right py-2 px-3 font-semibold text-white">Status</th>
                <th class="text-left py-2 px-3 font-semibold text-white">Strategy</th>
                <th class="text-left py-2 px-3 font-semibold text-white">Order ID</th>
                <th class="text-left py-2 px-3 font-semibold text-white">Trade ID</th>
            </tr>
        `;
        table.appendChild(thead);

        // Create table body
        const tbody = document.createElement('tbody');

        // Process each record (already sorted by backend)
        records.forEach(record => {
            const row = this.createRecordRow(record);
            tbody.appendChild(row);
        });

        table.appendChild(tbody);
        tableContainer.appendChild(table);
        container.appendChild(tableContainer);
    }

    createRecordRow(record) {
        const row = document.createElement('tr');
        row.className = 'hover:bg-dark-surface border-b border-dark-border';

        // Use pre-formatted data from backend
        const timestamp = record.formatted_timestamp || '-';
        
        // Determine type styling
        const typeClass = record.record_type === 'Order' ? 'text-blue-400' : 'text-green-400';
        
        // Use pre-formatted values from backend
        const formattedPrice = record.formatted_price || '-';
        const formattedQuantity = record.formatted_quantity || '-';
        
        // Format status
        const status = record.status || '-';
        const statusClass = this.getStatusClass(status);

        // Get IDs based on record type
        const orderId = record.orderid || '-';
        const tradeId = record.tradeid || '-';

        row.innerHTML = `
            <td class="text-left py-2 px-3 ${typeClass} font-semibold">${record.record_type}</td>
            <td class="text-left py-2 px-3 text-dark-text-primary">${timestamp}</td>
            <td class="text-left py-2 px-3 text-white font-medium">${record.symbol || '-'}</td>
            <td class="text-left py-2 px-3 text-dark-text-primary">${record.direction || '-'}</td>
            <td class="text-right py-2 px-3 text-dark-text-primary">${formattedQuantity}</td>
            <td class="text-right py-2 px-3 text-dark-text-primary">${formattedPrice}</td>
            <td class="text-right py-2 px-3 ${statusClass}">${status}</td>
            <td class="text-left py-2 px-3 text-dark-text-secondary">${record.strategy_name || '-'}</td>
            <td class="text-left py-2 px-3 text-dark-text-secondary font-mono text-xs">${orderId}</td>
            <td class="text-left py-2 px-3 text-dark-text-secondary font-mono text-xs">${tradeId}</td>
        `;

        return row;
    }

    getStatusClass(status) {
        if (!status) return 'text-white';
        
        const statusLower = status.toLowerCase();
        if (statusLower.includes('filled') || statusLower.includes('complete')) {
            return 'text-green-400';
        } else if (statusLower.includes('pending') || statusLower.includes('submitted')) {
            return 'text-yellow-400';
        } else if (statusLower.includes('cancelled') || statusLower.includes('rejected')) {
            return 'text-red-400';
        } else {
            return 'text-white';
        }
    }

    show() {
        // Initialize if not done yet
        if (!this.initialized) {
            this.init();
        }
        this.loadOrdersTradesData();
    }

    hide() {
        // No need to hide since it's a full page
    }
}
