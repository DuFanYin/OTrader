// Tailwind CSS Configuration for OTrader
// Custom theme configuration for dark mode interface

module.exports = {
    content: [
        './pages/**/*.html',
        './js/**/*.js',
        './dist/**/*.html'
    ],
    theme: {
        extend: {
            colors: {
                'dark-bg': '#1a1a1a',
                'dark-surface': '#2a2a2a',
                'dark-border': '#404040',
                'dark-text': '#e0e0e0',
                'dark-text-secondary': '#a0a0a0',
                'accent': '#3b82f6',
                'accent-hover': '#2563eb',
                'success': '#10b981',
                'warning': '#f59e0b',
                'error': '#ef4444',
                'log-debug': '#9ca3af',
                'log-info': '#60a5fa',
                'log-warning': '#fbbf24',
                'log-error': '#f87171',
                'log-critical': '#dc2626'
            },
            fontFamily: {
                'mono': ['Courier New', 'monospace']
            },
            animation: {
                'pulse': 'pulse 2s infinite',
                'spin': 'spin 1s linear infinite',
                'fade-in': 'fadeIn 0.3s ease-in',
                'slide-in': 'slideIn 0.3s ease-out'
            },
            keyframes: {
                fadeIn: {
                    '0%': { opacity: '0' },
                    '100%': { opacity: '1' }
                },
                slideIn: {
                    '0%': { transform: 'translateY(-10px)', opacity: '0' },
                    '100%': { transform: 'translateY(0)', opacity: '1' }
                }
            }
        }
    },
    plugins: []
};
