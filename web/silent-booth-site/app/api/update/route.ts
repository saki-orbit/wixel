import { NextResponse } from 'next/server';
import {
  DEVICE_ID,
  getDeviceToken,
  saveBoothUpdate,
  TARGET_TYPES,
  type BoothUpdate,
  type TargetType,
} from '../../../db/state';

export const runtime = 'edge';

const MAX_BODY_BYTES = 1_024;

function secureEqual(left: string, right: string): boolean {
  const encoder = new TextEncoder();
  const leftBytes = encoder.encode(left);
  const rightBytes = encoder.encode(right);
  const length = Math.max(leftBytes.length, rightBytes.length);
  let difference = leftBytes.length ^ rightBytes.length;

  for (let index = 0; index < length; index += 1) {
    difference |= (leftBytes[index] ?? 0) ^ (rightBytes[index] ?? 0);
  }

  return difference === 0;
}

function isIntegerInRange(value: unknown, min: number, max: number): value is number {
  return Number.isSafeInteger(value) && Number(value) >= min && Number(value) <= max;
}

function parseUpdate(value: unknown): BoothUpdate | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;

  const body = value as Record<string, unknown>;
  const targetType = body.target_type;
  const distanceCm = body.distance_cm;

  if (
    body.device_id !== DEVICE_ID ||
    typeof body.occupied !== 'boolean' ||
    typeof targetType !== 'string' ||
    !TARGET_TYPES.includes(targetType as TargetType) ||
    !(distanceCm === null || isIntegerInRange(distanceCm, 0, 1_000)) ||
    !isIntegerInRange(body.uptime_ms, 0, Number.MAX_SAFE_INTEGER)
  ) {
    return null;
  }

  return {
    deviceId: body.device_id,
    occupied: body.occupied,
    targetType: targetType as TargetType,
    distanceCm,
    uptimeMs: body.uptime_ms,
  };
}

function json(body: unknown, status = 200) {
  return NextResponse.json(body, {
    status,
    headers: {
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
    },
  });
}

export async function POST(request: Request) {
  try {
    const expectedToken = getDeviceToken();
    const authorization = request.headers.get('authorization') ?? '';
    const suppliedToken = authorization.startsWith('Bearer ')
      ? authorization.slice(7)
      : '';

    if (!secureEqual(suppliedToken, expectedToken)) {
      return json({ error: 'unauthorized' }, 401);
    }

    const contentType = request.headers.get('content-type') ?? '';
    if (!contentType.toLowerCase().startsWith('application/json')) {
      return json({ error: 'content_type_must_be_json' }, 415);
    }

    const contentLength = Number(request.headers.get('content-length') ?? 0);
    if (contentLength > MAX_BODY_BYTES) {
      return json({ error: 'request_too_large' }, 413);
    }

    let rawBody: unknown;
    try {
      rawBody = await request.json();
    } catch {
      return json({ error: 'invalid_json' }, 400);
    }

    const update = parseUpdate(rawBody);
    if (!update) {
      return json({ error: 'invalid_payload' }, 400);
    }

    const receivedAt = new Date().toISOString();
    await saveBoothUpdate(update, receivedAt);
    return json({ ok: true, device_id: update.deviceId, updated_at: receivedAt });
  } catch (error) {
    console.error('Failed to save booth update', error);
    return json({ error: 'service_unavailable' }, 503);
  }
}
