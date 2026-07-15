import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import "./globals.css";
import SidebarNav from "./components/SidebarNav";
import { LogProvider } from "./context/LogContext";
import LogDock from "./components/LogDock";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export const metadata: Metadata = {
  title: "FACTT",
  description: "Option strategy backtest and trading",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body
        className={`${geistSans.variable} ${geistMono.variable} flex h-screen overflow-hidden bg-[color:var(--background)] text-[color:var(--text-primary)] antialiased`}
      >
        <LogProvider>
          <SidebarNav />
          {/* 右侧：上 65% 为 page content，下 35% 为 LogDock，整体高度锁死为一屏。
              左右分栏使用比例宽度：sidebar 占 ~12%，右侧 body 占其余空间。 */}
          <div className="flex-1 flex flex-col md:ml-[12%] min-h-0 h-screen">
            <main className="basis-[65%] min-h-0 overflow-hidden">
              {children}
            </main>
            <div className="basis-[35%] min-h-0">
              <LogDock />
            </div>
          </div>
        </LogProvider>
      </body>
    </html>
  );
}
