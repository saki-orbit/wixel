import { integer, sqliteTable, text } from 'drizzle-orm/sqlite-core';

export const boothState = sqliteTable('booth_state', {
  deviceId: text('device_id').primaryKey(),
  occupied: integer('occupied', { mode: 'boolean' }).notNull(),
  targetType: text('target_type').notNull(),
  distanceCm: integer('distance_cm'),
  uptimeMs: integer('uptime_ms').notNull(),
  updatedAt: text('updated_at').notNull(),
  occupiedSince: text('occupied_since'),
});
