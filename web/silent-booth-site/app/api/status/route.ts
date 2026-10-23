import { env } from 'cloudflare:workers';
import { NextResponse } from 'next/server';
import { DEVICE_ID, OFFLINE_AFTER_MS, TARGET_TYPES, type TargetType } from '../../../db/state';

export const runtime = 'edge';

const FEED_KEY = 'silent-booth-01-status';

interface DevicePayload {
  device_id: string;
  occupied: boolean;
  target_type: TargetType;
  distance_cm: number | null;
  uptime_ms: number;
  occupied_since_unix: number;
}

interface AdafruitDataPoint {
  value?: unknown;
  created_at?: unknown;
}

function json(body: unknown, status = 200) {
  return NextResponse.json(body, {
    status,
    headers: {
      'Cache-Control': 'no-store, no-cache, must-revalidate',
      'X-Content-Type-Options': 'nosniff',
    },
  });
}

function emptyStatus() {
  return {
    device_id: DEVICE_ID,
    occupied: null,
    target_type: null,
    distance_cm: null,
    updated_at: null,
    online: false,
    occupied_since: null,
    occupied_seconds: 0,
    offline_after_seconds: OFFLINE_AFTER_MS / 1_000,
  };
}

function parseDevicePayload(value: unknown): DevicePayload | null {
  let raw: unknown = value;
  if (typeof raw === 'string') {
    try {
      raw = JSON.parse(raw);
    } catch {
      return null;
    }
  }

  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const candidate = raw as Record<string, unknown>;
  if (
    candidate.device_id !== DEVICE_ID ||
    typeof candidate.occupied !== 'boolean' ||
    typeof candidate.target_type !== 'string' ||
    !TARGET_TYPES.includes(candidate.target_type as TargetType) ||
    typeof candidate.uptime_ms !== 'number' ||
    !Number.isFinite(candidate.uptime_ms) ||
    typeof candidate.occupied_since_unix !== 'number' ||
    !Number.isFinite(candidate.occupied_since_unix)
  ) {
    return null;
  }

  const distance = candidate.distance_cm;
  if (distance !== null && (typeof distance !== 'number' || !Number.isFinite(distance))) {
    return null;
  }

  return {
    device_id: DEVICE_ID,
    occupied: candidate.occupied,
    target_type: candidate.target_type as TargetType,
    distance_cm: distance as number | null,
    uptime_ms: candidate.uptime_ms,
    occupied_since_unix: candidate.occupied_since_unix,
  };
}

export async function GET() {
  try {
    const username = env.ADAFRUIT_IO_USERNAME?.trim();
    const apiKey = env.ADAFRUIT_IO_KEY?.trim();
    if (!username || !apiKey) throw new Error('Adafruit IO is not configured');

    const endpoint = `https://io.adafruit.com/api/v2/${encodeURIComponent(username)}/feeds/${FEED_KEY}/data/last`;
    const response = await fetch(endpoint, {
      headers: {
        Accept: 'application/json',
        'X-AIO-Key': apiKey,
      },
      cache: 'no-store',
    });

    if (response.status === 404) return json(emptyStatus());
    if (!response.ok) throw new Error(`Adafruit IO returned ${response.status}`);

    const point = (await response.json()) as AdafruitDataPoint;
    const payload = parseDevicePayload(point.value);
    const updatedAtMs = typeof point.created_at === 'string' ? Date.parse(point.created_at) : NaN;
    if (!payload || !Number.isFinite(updatedAtMs)) {
      throw new Error('Adafruit IO returned an invalid data point');
    }

    const now = Date.now();
    const online = now - updatedAtMs <= OFFLINE_AFTER_MS;
    const occupiedSinceMs = payload.occupied_since_unix * 1_000;
    const hasValidOccupiedSince =
      payload.occupied &&
      payload.occupied_since_unix > 1_700_000_000 &&
      occupiedSinceMs <= now + 60_000;
    const occupiedSince = hasValidOccupiedSince
      ? new Date(occupiedSinceMs).toISOString()
      : payload.occupied
        ? new Date(updatedAtMs).toISOString()
        : null;
    const occupiedSeconds = payload.occupied && occupiedSince
      ? Math.max(0, Math.floor((now - Date.parse(occupiedSince)) / 1_000))
      : 0;

    return json({
      device_id: payload.device_id,
      occupied: payload.occupied,
      target_type: payload.target_type,
      distance_cm: payload.distance_cm,
      updated_at: new Date(updatedAtMs).toISOString(),
      online,
      occupied_since: occupiedSince,
      occupied_seconds: occupiedSeconds,
      offline_after_seconds: OFFLINE_AFTER_MS / 1_000,
    });
  } catch (error) {
    console.error('Failed to read booth status', error);
    return json({ error: 'service_unavailable' }, 503);
  }
}
