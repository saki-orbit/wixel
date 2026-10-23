import type { Metadata } from 'next';
import './globals.css';

const siteUrl = new URL('https://library-silent-booth-status.z9myjwv29s.chatgpt.site');

export const metadata: Metadata = {
  metadataBase: siteUrl,
  title: '静音舱实时状态',
  description: '查看图书馆静音舱 01 的实时占用与设备在线状态。',
  openGraph: {
    type: 'website',
    url: siteUrl,
    title: '静音舱实时状态',
    description: '图书馆静音舱 01 · 实时占用与设备在线状态',
    images: [{ url: '/og.png', width: 1200, height: 630, alt: '静音舱实时状态' }],
  },
  twitter: {
    card: 'summary_large_image',
    title: '静音舱实时状态',
    description: '图书馆静音舱 01 · 实时占用与设备在线状态',
    images: ['/og.png'],
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN">
      <body>{children}</body>
    </html>
  );
}
