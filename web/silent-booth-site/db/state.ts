import { env } from 'cloudflare:workers';

export const DEVICE_ID = 'booth-01';
export const OFFLINE_AFTER_MS = 25_000;

export const TARGET_TYPES = ['none', 'moving', 'static', 'both'] as const;
export type TargetType = (typeof TARGET_TYPES)[number];

export interface BoothUpdate {
  deviceId: string;
  occupied: boolean;
  targetType: TargetType;
  distanceCm: number | null;
  uptimeMs: number;
}

interface BoothRow {
  device_id: string;
  occupied: number;
  target_type: TargetType;
  distance_cm: number | null;
  uptime_ms: number;
  updated_at: string;
  occupied_since: string | null;
}

function getDatabase(): D1Database {
  if (!env.DB) {
    throw new Error('Database binding is unavailable');
  }
  return env.DB;
}

export function getDeviceToken(): string {
  const token = env.DEVICE_TOKEN;
  if (!token) {
    throw new Error('DEVICE_TOKEN is not configured');
  }
  return token;
}

export async function ensureSchema() {
  await getDatabase()
    .prepare(`
      CREATE TABLE IF NOT EXISTS booth_state (
        device_id TEXT PRIMARY KEY NOT NULL,
        occupied INTEGER NOT NULL,
        target_type TEXT NOT NULL,
        distance_cm INTEGER,
        uptime_ms INTEGER NOT NULL,
        updated_at TEXT NOT NULL,
        occupied_since TEXT
      )
    `)
    .run();
}

export async function saveBoothUpdate(update: BoothUpdate, receivedAt: string) {
  await ensureSchema();
  await getDatabase()
    .prepare(`
      INSERT INTO booth_state (
        device_id, occupied, target_type, distance_cm,
        uptime_ms, updated_at, occupied_since
      ) VALUES (?, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(device_id) DO UPDATE SET
        occupied = excluded.occupied,
        target_type = excluded.target_type,
        distance_cm = excluded.distance_cm,
        uptime_ms = excluded.uptime_ms,
        updated_at = excluded.updated_at,
        occupied_since = CASE
          WHEN excluded.occupied = 0 THEN NULL
          WHEN booth_state.occupied = 1 THEN booth_state.occupied_since
          ELSE excluded.updated_at
        END
    `)
    .bind(
      update.deviceId,
      update.occupied ? 1 : 0,
      update.targetType,
      update.distanceCm,
      update.uptimeMs,
      receivedAt,
      update.occupied ? receivedAt : null,
    )
    .run();
}

export async function readBoothState(deviceId: string): Promise<BoothRow | null> {
  await ensureSchema();
  return getDatabase()
    .prepare(`
      SELECT
        device_id, occupied, target_type, distance_cm,
        uptime_ms, updated_at, occupied_since
      FROM booth_state
      WHERE device_id = ?
      LIMIT 1
    `)
    .bind(deviceId)
    .first<BoothRow>();
}
