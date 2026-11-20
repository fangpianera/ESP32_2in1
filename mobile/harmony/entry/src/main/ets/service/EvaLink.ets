/**
 * EvaLink.ets —— 鸿蒙端 TCP 长连接（板端 Server，手机 Client）
 *
 * 与 iOS 版本行为一致：
 *   - 只在 Wi-Fi 已连到 EVA-NAV-* 时连接
 *   - 断线指数退避重连（0.5s → 4s）
 *   - 收到 GPS_REQ → 取最新定位快照回 GPS_RSP（3s 去抖）
 *   - 路线整包 ROUTE_PUSH
 *
 * [待确认] 鸿蒙 socket 的 connect 超时与 on('message') 回调签名在
 * API 12/13/14 之间有细微差异，请以本机 SDK 的 d.ts 为准（见 SETUP.md 第 4 节）。
 */
import { socket } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { buffer } from '@kit.ArkTS';
import { MsgType, encode, decode, EvaFrame, headerLen } from '../common/EvaProtocol';

export enum LinkState { IDLE, CONNECTING, READY, FAILED }

export class EvaLink {
  private tcp: socket.TCPSocket | null = null;
  private acc: buffer.Buffer = buffer.alloc(0);
  private seq: number = 1;
  private wantConnect: boolean = false;
  private connecting: boolean = false;
  private retryTimer: number = -1;
  private retryMs: number = 500;
  private lastGpsReqAt: number = 0;
  /** 心跳：板端 30s 才发一次 GPS_REQ，靠它判断连接是否已经死掉 */
  private hbTimer: number = -1;
  private lastRxAt: number = 0;
  /** 待送达的路线：板子重启/断链时先存着，连上后自动补发 */
  private pendingRoute: string | null = null;

  onState: (s: LinkState, note?: string) => void = () => {};
  onRouteAck: (ok: boolean, pts: number, dist: number) => void = () => {};
  /** 由 LocationService 注入 */
  locationProvider: () => GpsSnapshot | null = () => null;
  /** 由 HealthStride 注入 */
  healthProvider: () => Record<string, Object> | null = () => null;

  start(): void {
    this.wantConnect = true;
    this.connect();
  }

  stop(): void {
    this.wantConnect = false;
    this.clearRetry();
    this.dropSocket();
    this.onState(LinkState.IDLE);
  }

  /**
   * 实测坑：上一个 connect 还在 pending 时再次 construct/connect，
   * 会稳定报 `2301115 Operation in progress`，而且旧 socket 一直挂着。
   * 所以重试前必须先关掉旧 socket，并用 connecting 做闸门。
   */
  private dropSocket(): void {
    this.stopHeartbeat();
    const old = this.tcp;
    this.tcp = null;
    this.acc = buffer.alloc(0);
    if (old !== null) {
      try {
        old.off('message');
        old.off('close');
        old.close();
      } catch (e) {
        // 已关闭的 socket 再 close 会抛，忽略即可
      }
    }
  }

  /**
   * 心跳/僵尸连接检测。
   * [实测] 板子被复位后（日志 rst:0x15 USB_UART_CHIP_RESET），手机这条 TCP 不会
   * 收到 close，只在下次 send 时报 2301107(ENOTCONN)。所以必须主动探活：
   * 超过 25 秒没收到任何帧就判定连接失效，重建。
   */
  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.lastRxAt = Date.now();
    this.hbTimer = setInterval(() => {
      if (this.tcp === null) {
        return;
      }
      const silent = Date.now() - this.lastRxAt;
      if (silent > 25000) {
        console.warn(`[EVA] 链路静默 ${Math.round(silent / 1000)}s，重建连接`);
        this.onState(LinkState.FAILED, '链路静默，重连中');
        this.dropSocket();
        this.retryMs = 500;
        this.scheduleRetry();
        return;
      }
      this.sendJson(MsgType.PING, {});
    }, 10000);
  }

  private stopHeartbeat(): void {
    if (this.hbTimer !== -1) {
      clearInterval(this.hbTimer);
      this.hbTimer = -1;
    }
  }

  private clearRetry(): void {
    if (this.retryTimer !== -1) {
      clearTimeout(this.retryTimer);
      this.retryTimer = -1;
    }
  }

  private connect(): void {
    if (!this.wantConnect || this.connecting) {
      return;
    }
    this.connecting = true;
    this.onState(LinkState.CONNECTING);
    const tcp = socket.constructTCPSocketInstance();
    this.tcp = tcp;
    this.acc = buffer.alloc(0);

    tcp.on('message', (info: socket.SocketMessageInfo) => {
      this.lastRxAt = Date.now();
      const chunk = buffer.from(info.message);
      this.acc = this.acc.length === 0 ? chunk : buffer.concat([this.acc, chunk]);
      this.drain();
    });

    tcp.on('close', () => {
      if (this.connecting) {
        return;
      }
      this.onState(LinkState.FAILED, 'peer closed');
      this.dropSocket();
      this.scheduleRetry();
    });

    tcp.connect({
      address: { address: '192.168.4.1', port: 8080, family: 1 },
      timeout: 3000
    }).then(() => {
      this.connecting = false;
      this.retryMs = 500;
      this.onState(LinkState.READY);
      this.startHeartbeat();
      this.sendTimeSync();
      const h = this.healthProvider();
      if (h) {
        this.sendJson(MsgType.HEALTH_PUSH, h);
      }
      // 断链期间按下的「② 下发」不能丢：连上就先补发
      const pending = this.pendingRoute;
      if (pending !== null) {
        console.info('[EVA] 链路恢复，补发已缓存的路线');
        this.sendRaw(MsgType.ROUTE_PUSH, buffer.from(pending, 'utf-8'));
      }
    }).catch((e: BusinessError) => {
      this.connecting = false;
      // 2301115(EINPROGRESS)：手机不在 192.168.4.0/24 网段（即没连板端 AP）时的必然结果
      const note = e.code === 2301115
        ? '板端未在线（请先连 EVA-NAV-xxxx 热点）'
        : `${e.code}: ${e.message}`;
      this.onState(LinkState.FAILED, note);
      this.dropSocket();
      this.scheduleRetry();
    });
  }

  private scheduleRetry(): void {
    if (!this.wantConnect) {
      return;
    }
    this.clearRetry();
    const delay = this.retryMs;
    this.retryMs = Math.min(4000, this.retryMs * 2);
    this.retryTimer = setTimeout(() => {
      this.retryTimer = -1;
      this.connect();
    }, delay);
  }

  private drain(): void {
    let consumed = 0;
    try {
      while (true) {
        const rest = this.acc.subarray(consumed);
        const f: EvaFrame | null = decode(rest);
        if (f === null) {
          break;
        }
        consumed += headerLen() + f.payload.length;
        this.handle(f);
      }
    } catch (e) {
      // 帧头 magic 不对说明流已失步：整条连接不可信，直接弃掉重连
      console.error(`[EVA] 解析失步: ${(e as Error).message}`);
      this.onState(LinkState.FAILED, '协议失步，重连中');
      this.dropSocket();
      this.retryMs = 500;
      this.scheduleRetry();
      return;
    }
    this.acc = consumed > 0 ? this.acc.subarray(consumed) : this.acc;
  }

  private handle(f: EvaFrame): void {
    switch (f.type) {
      case MsgType.GPS_REQ: {
        const now = Date.now();
        if (now - this.lastGpsReqAt < 3000) {
          return;
        }
        this.lastGpsReqAt = now;
        const fix = this.locationProvider();
        if (fix === null || !fix.fresh) {
          this.sendJson(MsgType.STATUS, { 'ok': false, 'note': 'no-fix' });
          return;
        }
        this.sendJson(MsgType.GPS_RSP, {
          'lat': fix.lat,
          'lon': fix.lon,
          'acc': fix.acc,
          'course': fix.course,
          'course_acc': fix.courseAcc,
          'speed': fix.speed,
          'ts': Math.floor(now / 1000)
        });
        break;
      }
      case MsgType.ROUTE_ACK: {
        const o = JSON.parse(f.payload.toString('utf-8')) as Record<string, Object>;
        this.pendingRoute = null;
        this.onRouteAck(o['ok'] as boolean ?? false,
                       o['pts'] as number ?? 0,
                       o['dist_m'] as number ?? 0);
        break;
      }
      case MsgType.HEALTH_ACK:
        console.info(`[EVA] base_step_m = ${f.payload.toString('utf-8')}`);
        break;
      case MsgType.PING:
        this.sendJson(MsgType.PONG, {});
        break;
      default:
        break;
    }
  }

  private sendJson(type: MsgType, obj: Record<string, Object>): void {
    this.sendRaw(type, buffer.from(JSON.stringify(obj), 'utf-8'));
  }

  pushRouteJson(json: string): void {
    // 先记下来：即使此刻 send 失败，重连成功后也会自动补发（板子重启场景必备）
    this.pendingRoute = json;
    this.sendRaw(MsgType.ROUTE_PUSH, buffer.from(json, 'utf-8'));
  }

  pushHealth(obj: Record<string, Object>): void {
    this.sendJson(MsgType.HEALTH_PUSH, obj);
  }

  private sendTimeSync(): void {
    this.sendJson(MsgType.TIME_SYNC, { 'unix': Math.floor(Date.now() / 1000) });
  }

  private sendRaw(type: MsgType, payload: buffer.Buffer): void {
    const tcp = this.tcp;
    if (tcp === null) {
      return;
    }
    this.seq = (this.seq + 1) >>> 0;
    const frame = encode(type, this.seq, payload);
    // [实测坑] 不能直接传 frame.buffer：Buffer 底层 ArrayBuffer 可能带偏移/多余容量，
    // 发送出去会多字节，板端 CRC 校验不过。必须按 byteOffset/长度精确切片
    // （ArkTS 的 Buffer 有 byteOffset，但没有 byteLength，长度用 length）。
    const ab: ArrayBuffer = frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.length);
    tcp.send({ data: ab })
      .catch((e: BusinessError) => {
        // 2301107 = ENOTCONN：板子重启后手机侧不会收到 close，只会在发送时暴露。
        // 这里必须按"断链"处理（关 socket + 重连），否则会永远卡在假 READY。
        console.error(`[EVA] send failed ${e.code}: ${e.message}（按断链处理）`);
        this.onState(LinkState.FAILED, `发送失败 ${e.code}，重连中`);
        this.dropSocket();
        this.retryMs = 500;
        this.scheduleRetry();
      });
  }
}

/** 定位快照（与 iOS 端字段一一对应） */
export class GpsSnapshot {
  lat: number = 0;
  lon: number = 0;
  acc: number = 999;
  course: number = -1;
  courseAcc: number = 999;
  speed: number = 0;
  timestamp: number = 0;

  get fresh(): boolean {
    return (Date.now() - this.timestamp) < 20000 && this.acc > 0 && this.acc < 65;
  }
}
