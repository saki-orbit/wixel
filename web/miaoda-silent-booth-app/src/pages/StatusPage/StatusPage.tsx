import { useEffect, useMemo, useState } from "react";

const ENDPOINT =
  "https://io.adafruit.com/api/v2/p4hamster/feeds/silent-booth-01-status/data?limit=1";
const OFFLINE_AFTER_MS = 75_000;

type TargetType = "none" | "moving" | "static" | "both";

interface DevicePayload {
  device_id: string;
  occupied: boolean;
  target_type: TargetType;
  distance_cm: number | null;
  uptime_ms: number;
  occupied_since_unix: number;
}

interface AdafruitPoint {
  value: string | DevicePayload;
  created_at: string;
}

interface StatusData extends DevicePayload {
  updated_at: string;
  occupied_since: string | null;
}

type ViewState =
  | { kind: "loading" }
  | { kind: "error" }
  | { kind: "ready"; data: StatusData };

const TARGET_LABELS: Record<TargetType, string> = {
  none: "未检测到目标",
  moving: "运动目标",
  static: "静止目标",
  both: "运动 + 静止",
};

function formatDuration(totalSeconds: number) {
  const seconds = Math.max(0, Math.floor(totalSeconds));
  const hours = Math.floor(seconds / 3_600);
  const minutes = Math.floor((seconds % 3_600) / 60);
  const remaining = seconds % 60;
  if (hours > 0) {
    return `${hours}:${String(minutes).padStart(2, "0")}:${String(remaining).padStart(2, "0")}`;
  }
  return `${String(minutes).padStart(2, "0")}:${String(remaining).padStart(2, "0")}`;
}

function formatLastUpdated(updatedAt: string | null, now: number) {
  if (!updatedAt) return "暂无上报";
  const elapsedSeconds = Math.max(0, Math.floor((now - Date.parse(updatedAt)) / 1_000));
  if (elapsedSeconds < 8) return "刚刚";
  if (elapsedSeconds < 60) return `${elapsedSeconds} 秒前`;
  if (elapsedSeconds < 3_600) return `${Math.floor(elapsedSeconds / 60)} 分钟前`;
  return new Intl.DateTimeFormat("zh-CN", {
    month: "numeric",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  }).format(new Date(updatedAt));
}

function parsePoint(point: AdafruitPoint): StatusData {
  const payload =
    typeof point.value === "string"
      ? (JSON.parse(point.value) as DevicePayload)
      : point.value;
  if (
    payload.device_id !== "booth-01" ||
    typeof payload.occupied !== "boolean" ||
    !Object.hasOwn(TARGET_LABELS, payload.target_type) ||
    !Number.isFinite(Date.parse(point.created_at))
  ) {
    throw new Error("Invalid device payload");
  }
  const occupiedSinceMs = payload.occupied_since_unix * 1_000;
  return {
    ...payload,
    updated_at: new Date(point.created_at).toISOString(),
    occupied_since:
      payload.occupied && payload.occupied_since_unix > 1_700_000_000
        ? new Date(occupiedSinceMs).toISOString()
        : payload.occupied
          ? new Date(point.created_at).toISOString()
          : null,
  };
}

export default function StatusPage() {
  const [viewState, setViewState] = useState<ViewState>({ kind: "loading" });
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    let stopped = false;
    let timeoutId: ReturnType<typeof setTimeout> | undefined;

    async function refresh() {
      const controller = new AbortController();
      const abortId = setTimeout(() => controller.abort(), 6_000);
      try {
        const response = await fetch(ENDPOINT, {
          cache: "no-store",
          signal: controller.signal,
        });
        if (!response.ok) throw new Error(`Status request failed: ${response.status}`);
        const points = (await response.json()) as AdafruitPoint[];
        if (points.length === 0) throw new Error("No status data");
        const data = parsePoint(points[0]);
        if (!stopped) setViewState({ kind: "ready", data });
      } catch {
        if (!stopped) {
          setViewState((current) =>
            current.kind === "ready" ? current : { kind: "error" },
          );
        }
      } finally {
        clearTimeout(abortId);
        if (!stopped) timeoutId = setTimeout(refresh, 2_500);
      }
    }

    void refresh();
    return () => {
      stopped = true;
      clearTimeout(timeoutId);
    };
  }, []);

  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 1_000);
    return () => clearInterval(timer);
  }, []);

  const display = useMemo(() => {
    if (viewState.kind === "loading") {
      return {
        variant: "loading",
        pill: "正在连接",
        symbol: "···",
        title: "正在获取",
        description: "正在读取静音舱的最新状态",
        target: "—",
        distance: "—",
        updated: "请稍候",
        duration: null,
      };
    }
    if (viewState.kind === "error") {
      return {
        variant: "error",
        pill: "连接异常",
        symbol: "!",
        title: "暂不可用",
        description: "网络或服务暂时不可用，页面会自动重试",
        target: "—",
        distance: "—",
        updated: "获取失败",
        duration: null,
      };
    }

    const { data } = viewState;
    const updated = formatLastUpdated(data.updated_at, now);
    const target = TARGET_LABELS[data.target_type];
    const distance = data.distance_cm === null ? "—" : `${data.distance_cm} cm`;
    const responseAge = Math.max(0, Math.floor((now - Date.parse(data.updated_at)) / 1_000));
    const online = responseAge * 1_000 <= OFFLINE_AFTER_MS;

    if (!online) {
      return {
        variant: "offline",
        pill: "状态未知",
        symbol: "!",
        title: "设备离线",
        description: "暂时无法确认舱内是否有人",
        target,
        distance,
        updated,
        duration: null,
      };
    }
    if (data.occupied) {
      const occupiedSeconds = data.occupied_since
        ? Math.max(0, Math.floor((now - Date.parse(data.occupied_since)) / 1_000))
        : 0;
      return {
        variant: "occupied",
        pill: "设备在线",
        symbol: "有人",
        title: "使用中",
        description: "静音舱当前有人使用",
        target,
        distance,
        updated,
        duration: formatDuration(occupiedSeconds),
      };
    }
    return {
      variant: "available",
      pill: "设备在线",
      symbol: "可用",
      title: "空闲",
      description: "静音舱当前可以使用",
      target,
      distance,
      updated,
      duration: null,
    };
  }, [now, viewState]);

  return (
    <main className="app-shell">
      <header className="site-header">
        <div className="brand-mark" aria-hidden="true">
          <span />
          <span />
          <span />
        </div>
        <div>
          <p className="eyebrow">图书馆公共设施</p>
          <h1>静音舱实时状态</h1>
        </div>
      </header>

      <section
        className={`status-card status-card--${display.variant}`}
        aria-labelledby="booth-name"
      >
        <div className="card-topline">
          <div>
            <p className="floor-label">静音空间</p>
            <h2 id="booth-name">静音舱 01</h2>
          </div>
          <span className="online-pill">
            <span className="status-dot" />
            {display.pill}
          </span>
        </div>

        <div className="primary-status" role="status" aria-live="polite" aria-atomic="true">
          <div className="status-symbol" aria-hidden="true">
            {display.symbol}
          </div>
          <p className="status-kicker">当前状态</p>
          <p className="status-title">{display.title}</p>
          {display.duration && (
            <p className="occupancy-duration">
              已连续使用 <strong>{display.duration}</strong>
            </p>
          )}
          <p className="status-description">{display.description}</p>
        </div>

        <div className="update-row">
          <span>最后更新</span>
          <strong>{display.updated}</strong>
        </div>
      </section>

      <section className="detail-card" aria-label="设备详情">
        <div>
          <span className="detail-label">目标类型</span>
          <strong>{display.target}</strong>
        </div>
        <div>
          <span className="detail-label">探测距离</span>
          <strong>{display.distance}</strong>
        </div>
      </section>

      <p className="page-note">
        页面每 2.5 秒自动刷新。设备离线时，不会将静音舱误报为空闲。
      </p>
    </main>
  );
}
