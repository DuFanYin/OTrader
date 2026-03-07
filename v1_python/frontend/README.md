# OTrader Frontend

Modern, modular frontend architecture for OTrader built with vanilla JavaScript and Tailwind CSS. Features a centralized core system with enhanced error handling, real-time WebSocket communication, and a clean separation of concerns.

## 🏗️ Architecture Overview

The frontend follows a **layered architecture** with clear separation between core functionality, UI components, and page-specific logic:

```
┌─────────────────────────────────────────────────────────────┐
│                    PRESENTATION LAYER                       │
├─────────────────────────────────────────────────────────────┤
│  HTML Pages  │  UI Components  │  Page Controllers          │
│  (Layout)    │  (Reusable)     │  (Business Logic)          │
├─────────────────────────────────────────────────────────────┤
│                    CORE LAYER                               │
├─────────────────────────────────────────────────────────────┤
│  API Client  │  WebSocket     │  Error Handler   │  Config  │
│  (HTTP)      │  Manager       │  (Notifications) │          │
├─────────────────────────────────────────────────────────────┤
│                    UTILITY LAYER                            │
├─────────────────────────────────────────────────────────────┤
│  UI Utils    │  Helpers       │  Validators                 │
└─────────────────────────────────────────────────────────────┘
```

### Key Architectural Principles

- **Separation of Concerns**: Core logic separated from UI components
- **Centralized Configuration**: All settings and constants in one place
- **Unified Error Handling**: Consistent error management across the application
- **Real-time Communication**: WebSocket management with automatic reconnection
- **Modular Design**: Reusable components and utilities
- **Progressive Enhancement**: Works without JavaScript, enhanced with it

## 📁 File Structure

```
frontend/
├── __init__.py                       # Python package marker
├── README.md                         # This documentation
├── pages/                            # HTML presentation layer
│   ├── strategy-manager.html         # Main control panel
│   ├── strategy-holding.html         # Holdings management
│   └── orders-trades.html            # Orders and trades view
├── css/                              # Styling layer
│   └── tailwind.config.js            # Tailwind CSS configuration
├── js/                               # JavaScript application layer
│   ├── core/                         # Core application modules
│   │   ├── apiClient.js              # Centralized API client
│   │   ├── config.js                 # Configuration and constants
│   │   ├── error_handler.js          # Error handling and notifications
│   │   └── websocket_manager.js      # WebSocket management
│   ├── components/                   # Reusable UI components
│   │   ├── dialog.js                 # Modal dialogs
│   │   └── navigation.js             # Navigation component
│   ├── pages/                        # Page-specific controllers
│   │   ├── ordersTrades.js
│   │   ├── strategyHolding.js
│   │   └── strategyManager.js
│   └── utils/                        # Utility functions
│       └── ui_utils.js               # UI helper functions
└── dist/                             # Build output (generated)
    ├── assets/
    │   ├── index-BGkVxgYV.js
    │   └── index-Cplv8gwS.css
    └── index.html
```

## 🔧 Core Layer (`js/core/`)

The core layer provides essential application functionality:

### `apiClient.js` - Centralized API Management
- **Purpose**: Unified HTTP client with error handling and loading states
- **Features**:
  - Request/response interceptors
  - Automatic loading state management
  - Structured error handling
  - System status monitoring
- **Usage**: `apiClient.getStrategies()`, `apiClient.connectGateway()`, `apiClient.disconnectGateway()`

### `config.js` - Configuration Management
- **Purpose**: Centralized configuration for endpoints, constants, and settings
- **Contents**:
  - API endpoints configuration
  - Application constants (status, events, UI settings)
  - Error and success messages
  - Currency formatting settings
- **Usage**: `API_CONFIG.ENDPOINTS.GATEWAY.CONNECT`, `APP_CONSTANTS.STATUS.RUNNING`

### `error_handler.js` - Error Management
- **Purpose**: Centralized error handling with user-friendly notifications
- **Features**:
  - Error categorization and rate limiting
  - Loading overlays and progress indicators
  - Notification system with icons
  - Global error handlers
- **Usage**: `showError()`, `showSuccess()`, `handleError()`

### `websocket_manager.js` - Real-time Communication
- **Purpose**: Enhanced WebSocket management with automatic reconnection
- **Features**:
  - Connection state tracking
  - Exponential backoff reconnection
  - Event system integration
  - Message parsing and handling
  - Silent connection management (no popup notifications)
- **Usage**: `connectLogWebSocket()`, `connectStrategyWebSocket()`

## 🎨 Component Layer (`js/components/`)

Reusable UI components for consistent user interface:

### `dialog.js` - Modal Dialogs
- **Classes**: `Dialog`, `ConfirmDialog`, `InputDialog`, `ProgressDialog`
- **Features**: Form validation, progress tracking, keyboard navigation
- **Usage**: `showConfirmDialog()`, `showInputDialog()`

### `navigation.js` - Header Navigation
- **Features**: Active state detection, responsive design, periodic status updates
- **Usage**: Automatic navigation highlighting and system status display


## 📄 Page Layer (`js/pages/`)

Page-specific controllers that handle business logic:

### `strategyManager.js` - Strategy Management
- **Features**: Strategy CRUD operations, gateway control, real-time logs
- **Key Methods**: `connectGateway()`, `addStrategy()`, `refreshStrategies()`, `renderStrategyTable()`
- **Status Updates**: Periodic button state updates, WebSocket log streaming

### `strategyHolding.js` - Holdings Management
- **Features**: Position display, holdings refresh
- **Key Methods**: `loadHoldingsData()`, `renderHoldingsTable()`

### `ordersTrades.js` - Orders and Trades View
- **Features**: Combined orders and trades display, real-time updates
- **Key Methods**: `loadOrdersTradesData()`, `renderOrdersTradesTable()`

## 🛠️ Utility Layer (`js/utils/`)

### `ui_utils.js` - Optimized UI Helper Functions
- **Purpose**: High-performance DOM manipulation utilities and UI helpers
- **Features**: 
  - Optimized table creation with cached styling
  - High-performance log entry rendering with inline styles
  - Custom scrollbar styling with single injection
  - Cached constants for better performance
- **Functions**: `createTable()`, `addLogEntry()`, `createLogContainer()`
- **Performance**: Cached regex patterns, level colors, and status classes

## 🚀 Key Features

### System Management
- **Gateway Control**: Connect/disconnect from IBKR gateway
- **Status Monitoring**: Real-time gateway status with periodic updates
- **Error Handling**: Comprehensive error management with user notifications

### Strategy Management
- **Strategy Operations**: Add, restore, initialize, start, stop, remove, delete strategies
- **Real-time Updates**: WebSocket-based strategy status updates
- **Portfolio Integration**: Strategy holdings and P&L tracking

### Data Management
- **Orders & Trades**: Combined view of orders and trades data
- **Real-time Updates**: WebSocket-based data updates
- **Clean Interface**: Streamlined data management

### Real-time Communication
- **WebSocket Integration**: Log streaming and strategy updates
- **Silent Operation**: No popup notifications for connection events
- **Automatic Reconnection**: Robust connection management with exponential backoff

## 🎯 Usage

### Basic Setup
1. Include the core JavaScript files in your HTML
2. Initialize page controllers after DOM content loads
3. Navigation and breadcrumbs are automatically initialized

### Adding New Pages
1. Create HTML file in `pages/` directory
2. Create corresponding JavaScript controller in `js/pages/`
3. Add navigation link in `navigation.js`
4. Initialize controller in HTML script section

### Extending Functionality
- **API Endpoints**: Add to `config.js` under `API_CONFIG.ENDPOINTS`
- **Error Handling**: Use `handleError()` for consistent error management
- **WebSocket**: Use `websocket_manager.js` for real-time features
- **UI Components**: Extend `ui_utils.js` for common UI patterns

## 🔧 Development

### Dependencies
- **Tailwind CSS**: For styling and responsive design
- **Vanilla JavaScript**: No external JavaScript frameworks
- **WebSocket API**: For real-time communication

### Browser Support
- Modern browsers with ES6+ support
- WebSocket API support required
- CSS Grid and Flexbox support recommended

### Performance Considerations
- **Lazy Loading**: Components loaded on demand
- **Event Delegation**: Efficient event handling
- **Periodic Updates**: Configurable update intervals
- **Error Rate Limiting**: Prevents notification spam
- **Optimized UI Utils**: Cached constants, inline styles, single DOM injection
- **Tailwind-Only Styling**: No custom CSS files, all styling via Tailwind classes