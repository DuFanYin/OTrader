'use client';

import Link from 'next/link';
import { usePathname } from 'next/navigation';
import { useState } from 'react';
import { NAV_ITEMS } from '@/lib/nav';

export default function HamburgerNav() {
  const pathname = usePathname();
  const [open, setOpen] = useState(false);

  return (
    <>
      <header className="sticky top-0 z-30 flex items-center justify-between border-b border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] px-3 py-2">
        <button
          type="button"
          onClick={() => setOpen((o) => !o)}
          className="flex h-9 w-9 items-center justify-center border border-[color:var(--border-subtle)] text-[color:var(--text-soft)] hover:border-[color:var(--border-strong)] hover:bg-[color:var(--surface-raised)] hover:text-[color:var(--text-primary)]"
          aria-label="Toggle menu"
        >
          <svg className="h-5 w-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M4 6h16M4 12h16M4 18h16" />
          </svg>
        </button>
        <span className="flex-1 text-center text-sm font-medium text-[color:var(--text-muted)]">
          {NAV_ITEMS.find((i) => pathname.startsWith(i.href))?.label ?? 'FACTT'}
        </span>
        <div className="h-9 w-9" />
      </header>

      {open && (
        <>
          <div
            className="fixed inset-0 z-40 bg-black/60"
            onClick={() => setOpen(false)}
            aria-hidden
          />
          <nav
            className="fixed left-0 top-0 z-50 h-full w-44 border-r border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] py-4"
            aria-label="Main navigation"
          >
            <div className="flex items-center justify-between px-4 pb-4 border-b border-[color:var(--border-subtle)]">
              <span className="text-sm font-semibold text-[color:var(--text-primary)]">
                Menu
              </span>
              <button
                type="button"
                onClick={() => setOpen(false)}
                className="flex h-8 w-8 items-center justify-center text-[color:var(--text-soft)] hover:bg-[color:var(--surface-raised)] hover:text-[color:var(--text-primary)]"
                aria-label="Close menu"
              >
                <svg className="h-5 w-5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                  <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
                </svg>
              </button>
            </div>
            <ul className="mt-2 space-y-0.5 px-2">
              {NAV_ITEMS.map((item) => {
                const isActive =
                  item.href === pathname || pathname.startsWith(item.href);
                return (
                  <li key={item.href}>
                    <Link
                      href={item.href}
                      onClick={() => setOpen(false)}
                      className={`block px-3 py-2 text-sm font-medium transition-colors border-l-2 ${
                        isActive
                          ? 'border-l-[color:var(--border-strong)] text-[color:var(--text-primary)] bg-transparent'
                          : 'border-l-transparent text-[color:var(--text-soft)] hover:bg-[color:var(--surface-subtle)] hover:text-[color:var(--text-primary)]'
                      }`}
                    >
                      {item.label}
                    </Link>
                  </li>
                );
              })}
            </ul>
          </nav>
        </>
      )}
    </>
  );
}
