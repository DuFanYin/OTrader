// Dialog components

class Dialog {
    constructor(title, width = 420, height = 200) {
        this.title = title;
        this.width = width;
        this.height = height;
        this.modal = null;
        this.content = null;
        this.isOpen = false;
    }

    create() {
        // Create modal backdrop
        this.modal = document.createElement('div');
        this.modal.className = 'fixed inset-0 bg-black bg-opacity-50 hidden z-50 dialog-modal';
        this.modal.id = `modal-${this.title.toLowerCase().replace(/\s+/g, '-')}`;

        // Create modal content
        this.content = document.createElement('div');
        this.content.className = 'flex items-center justify-center min-h-screen p-4';
        
        const dialogBox = document.createElement('div');
        dialogBox.className = 'bg-dark-surface rounded-lg border border-dark-border p-6 w-full max-w-md max-h-screen overflow-y-auto dialog-content';

        // Create header
        const header = document.createElement('h3');
        header.className = 'text-lg font-semibold mb-4 text-white';
        header.textContent = this.title;

        // Create body container
        const body = document.createElement('div');
        body.className = 'mb-4';
        body.id = `${this.title.toLowerCase().replace(/\s+/g, '-')}-body`;

        // Create footer with buttons
        const footer = document.createElement('div');
        footer.className = 'flex justify-end space-x-2';

        dialogBox.appendChild(header);
        dialogBox.appendChild(body);
        dialogBox.appendChild(footer);

        this.content.appendChild(dialogBox);
        this.modal.appendChild(this.content);

        // Add click handler to close dialog when clicking backdrop
        this.modal.addEventListener('click', (e) => {
            if (e.target === this.modal) {
                this.close();
            }
        });

        // Prevent dialog content clicks from closing the dialog
        dialogBox.addEventListener('click', (e) => {
            e.stopPropagation();
        });

        // Add to document
        document.body.appendChild(this.modal);

        return {
            header,
            body,
            footer,
            dialogBox
        };
    }

    open() {
        if (!this.modal) {
            this.create();
        }
        this.modal.classList.remove('hidden');
        this.isOpen = true;
        
        // Add escape key handler
        this.escapeHandler = (e) => {
            if (e.key === 'Escape') {
                this.close();
            }
        };
        document.addEventListener('keydown', this.escapeHandler);
        
        // Focus first input if any
        const firstInput = this.modal.querySelector('input, select, textarea');
        if (firstInput) {
            setTimeout(() => firstInput.focus(), 100);
        }
    }

    close() {
        if (this.modal) {
            this.modal.classList.add('hidden');
            this.isOpen = false;
            
            // Remove escape key handler
            if (this.escapeHandler) {
                document.removeEventListener('keydown', this.escapeHandler);
                this.escapeHandler = null;
            }
        }
    }

    destroy() {
        if (this.modal && this.modal.parentNode) {
            this.modal.parentNode.removeChild(this.modal);
            this.modal = null;
            this.content = null;
            this.isOpen = false;
        }
    }
}

class ConfirmDialog extends Dialog {
    constructor(title, message, onConfirm, onCancel = null) {
        super(title, 400, 150);
        this.message = message;
        this.onConfirm = onConfirm;
        this.onCancel = onCancel;
    }

    create() {
        const elements = super.create();
        
        // Add message
        const messageEl = document.createElement('p');
        messageEl.className = 'text-dark-text-secondary mb-4';
        messageEl.textContent = this.message;
        elements.body.appendChild(messageEl);

        // Add buttons
        const confirmBtn = createButton('Confirm', () => {
            this.close();
            if (this.onConfirm) {
                this.onConfirm();
            }
        }, true, true, 80);
        confirmBtn.classList.add('bg-error', 'hover:bg-red-600');

        const cancelBtn = createButton('Cancel', () => {
            this.close();
            if (this.onCancel) {
                this.onCancel();
            }
        }, true, true, 80);
        cancelBtn.classList.add('bg-gray-500', 'hover:bg-gray-600');

        elements.footer.appendChild(cancelBtn);
        elements.footer.appendChild(confirmBtn);

        return elements;
    }
}

class InputDialog extends Dialog {
    constructor(title, fields, onSubmit, onCancel = null) {
        // Calculate height based on number of fields
        const baseHeight = 120; // Header + footer space
        const fieldHeight = 60; // Each field takes ~60px
        const calculatedHeight = baseHeight + (fields.length * fieldHeight);
        
        super(title, 450, Math.max(calculatedHeight, 200));
        this.fields = fields;
        this.onSubmit = onSubmit;
        this.onCancel = onCancel;
        this.inputs = {};
    }

    create() {
        const elements = super.create();
        
        // Create form
        const form = document.createElement('form');
        form.className = 'space-y-4';
        
        this.fields.forEach(field => {
            const fieldContainer = document.createElement('div');
            fieldContainer.className = 'dialog-field';
            
            // Label
            const label = createLabel(field.label);
            fieldContainer.appendChild(label);
            
            // Input
            let input;
            if (field.type === 'select') {
                input = createSelect(field.options || [], 400, 40);
            } else if (field.type === 'textarea') {
                input = document.createElement('textarea');
                input.rows = field.rows || 3;
            } else {
                input = createInput(field.placeholder || '', 400, 40);
                input.type = field.type || 'text';
            }
            
            if (field.value) {
                input.value = field.value;
            }
            
            if (field.required) {
                input.required = true;
            }
            
            this.inputs[field.name] = input;
            fieldContainer.appendChild(input);
            
            form.appendChild(fieldContainer);
        });
        
        elements.body.appendChild(form);

        // Add buttons
        const submitBtn = createButton('Submit', (e) => {
            e.preventDefault();
            this.handleSubmit();
        }, true, true, 100);
        submitBtn.type = 'submit';
        submitBtn.className = 'dialog-button dialog-button-primary';

        const cancelBtn = createButton('Cancel', () => {
            this.close();
            if (this.onCancel) {
                this.onCancel();
            }
        }, true, true, 100);
        cancelBtn.className = 'dialog-button dialog-button-secondary';

        elements.footer.className = 'dialog-buttons';
        elements.footer.appendChild(cancelBtn);
        elements.footer.appendChild(submitBtn);

        // Handle form submission
        form.addEventListener('submit', (e) => {
            e.preventDefault();
            this.handleSubmit();
        });

        return elements;
    }

    handleSubmit() {
        try {
            const data = {};
            let isValid = true;

            this.fields.forEach(field => {
                const input = this.inputs[field.name];
                const value = input.value.trim();

                if (field.required && !value) {
                    showError(`${field.label} is required`);
                    isValid = false;
                    return;
                }

                if (field.validate) {
                    try {
                        data[field.name] = field.validate(value);
                    } catch (error) {
                        showError(error.message);
                        isValid = false;
                        return;
                    }
                } else {
                    data[field.name] = value;
                }
            });

            if (isValid) {
                this.close();
                if (this.onSubmit) {
                    this.onSubmit(data);
                }
            }
        } catch (error) {
            showError(error.message);
        }
    }
}

class ProgressDialog extends Dialog {
    constructor(title, message = 'Processing...') {
        super(title, 350, 120);
        this.message = message;
        this.progress = 0;
        this.progressBar = null;
    }

    create() {
        const elements = super.create();
        
        // Add message
        const messageEl = document.createElement('p');
        messageEl.className = 'text-dark-text-secondary mb-3 text-center';
        messageEl.textContent = this.message;
        elements.body.appendChild(messageEl);

        // Add progress bar
        const progressContainer = document.createElement('div');
        progressContainer.className = 'w-full bg-dark-bg rounded-full h-2 mb-3';
        
        this.progressBar = document.createElement('div');
        this.progressBar.className = 'bg-accent h-2 rounded-full transition-all duration-300';
        this.progressBar.style.width = '0%';
        
        progressContainer.appendChild(this.progressBar);
        elements.body.appendChild(progressContainer);

        // Add cancel button
        const cancelBtn = createButton('Cancel', () => {
            this.close();
        }, true, true, 80);
        cancelBtn.classList.add('bg-gray-500', 'hover:bg-gray-600');

        elements.footer.appendChild(cancelBtn);

        return elements;
    }

    setProgress(progress) {
        this.progress = Math.max(0, Math.min(100, progress));
        if (this.progressBar) {
            this.progressBar.style.width = `${this.progress}%`;
        }
    }

    setMessage(message) {
        this.message = message;
        const messageEl = this.modal.querySelector('p');
        if (messageEl) {
            messageEl.textContent = message;
        }
    }
}

// Convenience functions
function showConfirmDialog(title, message, onConfirm, onCancel = null) {
    const dialog = new ConfirmDialog(title, message, onConfirm, onCancel);
    dialog.open();
    return dialog;
}

function showInputDialog(title, fields, onSubmit, onCancel = null) {
    const dialog = new InputDialog(title, fields, onSubmit, onCancel);
    dialog.open();
    return dialog;
}

function showProgressDialog(title, message = 'Processing...') {
    const dialog = new ProgressDialog(title, message);
    dialog.open();
    return dialog;
}

// Export for module system
if (typeof module !== 'undefined' && module.exports) {
    module.exports = {
        Dialog,
        ConfirmDialog,
        InputDialog,
        ProgressDialog,
        showConfirmDialog,
        showInputDialog,
        showProgressDialog
    };
}
