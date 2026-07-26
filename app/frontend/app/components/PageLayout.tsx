"use client";

import type { ReactNode } from "react";

interface PageLayoutProps {
  title: string;
  children: ReactNode;
}

export default function PageLayout({ title, children }: PageLayoutProps) {
  return (
    <div className="h-full text-[color:var(--text-primary)] py-3 px-3">
      <div className="w-full max-w-none h-full flex flex-col space-y-3 min-h-0">
        <h1 className="text-lg font-semibold tracking-wide text-[color:var(--text-primary)] border-b border-[color:var(--border-subtle)] pb-2">
          {title}
        </h1>
        {children}
      </div>
    </div>
  );
}
