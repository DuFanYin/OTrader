"use client";

import * as Select from "@radix-ui/react-select";
import React from "react";

export type TerminalSelectOption = {
  value: string;
  label: string;
  disabled?: boolean;
};

type TerminalSelectProps = {
  value: string;
  onValueChange: (value: string) => void;
  options: TerminalSelectOption[];
  placeholder?: string;
  disabled?: boolean;
  className?: string; // applied to Trigger
  contentClassName?: string; // applied to Content
};

export default function TerminalSelect({
  value,
  onValueChange,
  options,
  placeholder = "Select...",
  disabled = false,
  className,
  contentClassName,
}: TerminalSelectProps) {
  return (
    <Select.Root value={value} onValueChange={onValueChange} disabled={disabled}>
      <Select.Trigger
        className={`h-7 form-input text-xs inline-flex items-center justify-between gap-2 select-none disabled:opacity-50 disabled:cursor-not-allowed ${className ?? ""}`}
        aria-label={placeholder}
      >
        <Select.Value placeholder={placeholder} />
        <Select.Icon className="text-[color:var(--text-muted)]">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" aria-hidden>
            <path
              d="M6 9l6 6 6-6"
              stroke="currentColor"
              strokeWidth="2"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
          </svg>
        </Select.Icon>
      </Select.Trigger>

      <Select.Portal>
        <Select.Content
          position="popper"
          sideOffset={6}
          className={`z-50 border border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] text-[color:var(--text-primary)] shadow-none ${contentClassName ?? ""}`}
        >
          <Select.Viewport className="p-1 max-h-72 overflow-auto">
            {options.map((opt) => (
              <Select.Item
                key={opt.value}
                value={opt.value}
                disabled={opt.disabled}
                className="text-xs px-2 py-1.5 outline-none select-none cursor-pointer data-[highlighted]:bg-[color:var(--surface-raised)] data-[highlighted]:text-[color:var(--text-primary)] data-[disabled]:opacity-40 data-[disabled]:cursor-not-allowed"
              >
                <Select.ItemText>{opt.label}</Select.ItemText>
              </Select.Item>
            ))}
          </Select.Viewport>
        </Select.Content>
      </Select.Portal>
    </Select.Root>
  );
}

