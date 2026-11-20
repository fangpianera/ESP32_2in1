/**
 * BleLink.ets —— 鸿蒙端 BLE 中央设备（手机侧）主动连接板端 GATT 服务器
 *
 * 为什么换 BLE（真实踩坑驱动，不是偏好）：
 *   Wi-Fi SoftAP 方案要求手机先切到板端热点，切过去就没有外网，
 *   高德取路线必然失败（2300006 DNS），于是被迫"家里取路线→现场切热点→下发"，
 *   而且每次切换要重连、板子一复位旧 TCP 还会变僵尸连接。
 *   走 BLE：手机自己的 Wi-Fi/蜂窝不动，取路线与下发可以连续完成，功耗也低。
 *
 * 协议完全不变（common/EvaProtocol 同一套帧）：
 *   手机 → 板：按 MTU 切片写 RX 特征（6E400002…），每片用 WRITE（带响应）保证不丢
 *   板 → 手机：TX 特征（6E400003…）notify，回调里重组帧
 *
 * [实测坑清单]
 *   1) 默认 MTU 23 → 每片只剩 20 字节，路线几十 KB 会传到天荒地老；
 *      连上后必须 setBLEMtuSize(517)，并按实际 MTU 重新算分片大小。
 *   2) setBLEMtuSize 在部分机型/固件上会抛，必须 try/catch 后按 23 继续跑（降级而不是失败）。
 *   3) 必须先 setCharacteristicChangeNotification(tx, true) 再写数据，否则板端 ACK 永远收不到。
 *   4) 断链回调里不能立刻重连同一个 GattClientDevice，必须 close() 后重新 create，否则报重复设备。
 */
import { ble } from '@kit.ConnectivityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { buffer } from '@kit.ArkTS';
import { MsgType, encode, decode, EvaFrame, headerLen } from '../common/EvaProtocol';
import { LinkState, GpsSnapshot } from './EvaLink';

const SVC_UUID: string = '6E400001-B5A3-F393-E0A9-E50E24DCCA9E';
const RX_UUID: string = '6E400002-B5A3-F393-E0A9-E50E24DCCA9E';   // 手机 → 板（write）
const TX_UUID: string = '6E400003-B5A3-F393-E0A9-E50E24DCCA9E';   // 板 → 手机（notify）
const DEV_NAME_PREFIX: string = 'EVA';

export class BleLink {
  private dev: ble.GattClientDevice | null = null;
  private rxChar: ble.BLECharacteristic | null = null;
  private txChar: ble.BLECharacteristic | null = null;
  private acc: buffer.Buffer = buffer.alloc(0);
  private seq: number = 1;
  private wantConnect: boolean = false;
  private scanning: boolean = false;
  private retryTimer: number = -1;
  private retryMs: number = 500;
  private hbTimer: number = -1;
  private lastRxAt: number = 0;
  private lastGpsReqAt: number = 0;
  private mtu: number = 23;
  private pendingRoute: string | null = null;
  private busy: boolean = false;          // 发送串行化：BLE 写必须一片等一片
  private gen: number = 0;                // 连接代号：过滤旧设备迟到的回调

  onState: (s: LinkState, note?: string) => void = () => {};
  onRouteAck: (ok: boolean, pts: number, dist: number) => void = () => {};
  locationProvider: () => GpsSnapshot | null = () => null;
  healthProvider: () => Record<string, Object> | null = () => null;
  /** 板端界面读数：{dest, hi, lo, code}；null 表示暂无（板端保持上次显示） */
  statusProvider: () => Record<string, Object> | null = () => null;

  start(): void {
    if (this.wantConnect) {
      return;
    }
    this.wantConnect = true;
    this.scan();
  }

  stop(): void {
    this.wantConnect = false;
    this.clearRetry();
    this.stopHeartbeat();
    this.closeDev();
    if (this.scanning) {
      this.scanning = false;
      try {
        ble.stopBLEScan();
      } catch (e) {
      }
    }
    this.onState(LinkState.IDLE, '蓝牙已断开');
  }

  isReady(): boolean {
    return this.dev !== null && this.rxChar !== null && this.txChar !== null;
  }

  /** 扫描 → 命中板端就停下开始连接。
   *  鸿蒙 startBLEScan 不接受空数组（实测 401 Invalid parameter），必须带过滤条件：
   *  按服务 UUID 过滤，板端把 128bit UUID 放在广播包里；名字可能被截短成 EVA-01A5，只做弱校验。 */
  private scan(): void {
    if (this.scanning) {
      return;
    }
    this.scanning = true;
    this.onState(LinkState.CONNECTING, '正在搜索板端蓝牙…');
    try {
      const filter: ble.ScanFilter = { serviceUuid: SVC_UUID };
      ble.startBLEScan([filter]);
      ble.on('BLEDeviceFind', this.onFound);
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 蓝牙扫描失败 ${err.code}: ${err.message}（检查蓝牙开关/权限）`);
      this.scanning = false;
      this.retryLater();
    }
  }

  private onFound = (results: Array<ble.ScanResult>): void => {
    if (!this.wantConnect) {
      return;
    }
    for (const r of results) {
      const name: string = r.deviceName ?? '';
      // 已有 serviceUuid 过滤，名字只做兜底校验：板端广播名是 EVA-01A5（31 字节装不下完整前缀）
      if (name.length > 0 && !name.startsWith(DEV_NAME_PREFIX)) {
        continue;
      }
      console.info(`[EVA] 发现板端 ${name.length > 0 ? name : '(无名)'} rssi=${r.rssi}`);
      this.scanning = false;
      try {
        ble.stopBLEScan();
        ble.off('BLEDeviceFind', this.onFound);
      } catch (e) {
      }
      this.connect(r.deviceId, name);
      return;
    }
  };

  private connect(deviceId: string, name: string): void {
    this.closeDev();
    // 每次连接都带代号：旧 GattClientDevice 的断开/连接回调会晚到，
    // 若不校验代号，READY 之后 0.5s 就被旧设备的 state=0 打断重连（实测反复出现）
    const myGen = ++this.gen;
    this.onState(LinkState.CONNECTING, `连接 ${name}…`);
    try {
      const dev = ble.createGattClientDevice(deviceId);
      dev.on('BLEConnectionStateChange', (st: ble.BLEConnectionChangeState) => {
        if (myGen !== this.gen) {
          return;
        }
        // 直接比数值：不同 API 版本把这个枚举挂在 ble 或 constant 下，数值语义稳定（2=已连接,0=已断开）
        if (st.state as number === 2) {
          this.setup();
        } else if (st.state as number === 0) {
          console.warn('[EVA] 蓝牙断开，准备重连');
          this.closeDev();
          this.onState(LinkState.FAILED, '蓝牙断开，重连中');
          this.retryLater();
        }
      });
      dev.on('BLECharacteristicChange', (c: ble.BLECharacteristic) => {
        if (myGen !== this.gen) {
          return;
        }
        this.onChars(c.characteristicValue);
      });
      this.dev = dev;
      dev.connect();
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 蓝牙连接失败 ${err.code}: ${err.message}`);
      this.retryLater();
    }
  }

  /**
   * 连接建立后按华为官方最佳实践的顺序：先服务发现(getServices)，再协商 MTU，
   * 最后订阅 notify。顺序颠倒会让 MTU 请求抢占 ATT 通道，服务发现 10s 超时返回空。
   */
  private setup(): void {
    this.fetchServices(0);
  }

  private fetchServices(tryIdx: number): void {
    const dev = this.dev;
    const g = this.gen;
    if (dev === null) {
      return;
    }
    dev.getServices().then((svcs: Array<ble.GattService>) => {
      if (g !== this.gen) {
        return;                       // 期间已重连，本轮结果作废
      }
      console.info(`[EVA] getServices 第${tryIdx + 1}次：${svcs.length} 个服务`);
      if (svcs.length === 0 && tryIdx < 5) {
        setTimeout(() => this.fetchServices(tryIdx + 1), 600);
        return;
      }
      let found = false;
      for (const s of svcs) {
        // 鸿蒙返回的 128bit UUID 大小写/连字符格式与常量不一定一致，统一归一化后再比
        const su: string = BleLink.norm(s.serviceUuid);
        console.info(`[EVA] 服务 ${s.serviceUuid} chars=${s.characteristics.length} 命中=${su === BleLink.norm(SVC_UUID)}`);
        if (su !== BleLink.norm(SVC_UUID)) {
          continue;
        }
        for (const ch of s.characteristics) {
          const u: string = BleLink.norm(ch.characteristicUuid);
          if (u === BleLink.norm(RX_UUID)) {
            this.rxChar = ch;
          } else if (u === BleLink.norm(TX_UUID)) {
            this.txChar = ch;
          }
        }
        found = true;
      }
      if (!found || this.rxChar === null || this.txChar === null) {
        console.error('[EVA] 板端 GATT 服务不完整（服务/特征没找到）');
        this.onState(LinkState.FAILED, '板端服务缺失');
        this.closeDev();
        this.retryLater();
        return;
      }
      // 华为最佳实践顺序：服务发现 → 订阅 → 协商 MTU → 写数据
      dev.setCharacteristicChangeNotification(this.txChar, true).then(() => {
        void this.onNotifyReady('已订阅');
      }).catch((e: BusinessError) => {
        console.warn(`[EVA] 订阅 notify 失败 ${e.code}，改写字 CCCD`);
        const cccd: ble.BLEDescriptor = {
          serviceUuid: SVC_UUID,
          characteristicUuid: TX_UUID,
          descriptorUuid: '00002902-0000-1000-8000-00805F9B34FB',
          descriptorValue: new Uint8Array([0x01, 0x00]).buffer as ArrayBuffer
        };
        dev.writeDescriptorValue(cccd).then(() => {
          void this.onNotifyReady('CCCD 直写');
        }).catch((e2: BusinessError) => {
          console.error(`[EVA] 写 CCCD 也失败 ${e2.code}: ${e2.message}`);
          this.onState(LinkState.FAILED, '订阅失败');
          this.retryLater();
        });
      });
    }).catch((e: BusinessError) => {
      console.error(`[EVA] getServices 失败 ${e.code}: ${e.message}`);
      this.retryLater();
    });
  }

  private async onNotifyReady(how: string): Promise<void> {
    this.lastRxAt = Date.now();
    /* 订阅成功后再协商 MTU，**必须等它真正完成**：
     * 原来的写法是 setBLEMtuSize(517) 不 await，紧接着把 mtu 当 244 用，
     * 第一帧就按 241 字节分片发出去 —— 此时实际 MTU 还是 23，写必然超长
     * （华为侧回 2900099 Operation failed），然后整条链路被判为断链。
     * 这也是"有时能发、有时第一帧就断"的真正原因。 */
    const d = this.dev;
    if (d !== null) {
      try {
        await d.setBLEMtuSize(517);
        this.mtu = 244;                 // 服务端上限 517，取 244 稳妥（片长 241）
        await this.delay(60);           // 给协议栈一点落地时间再发第一帧
      } catch (e) {
        const err = e as BusinessError;
        console.warn(`[EVA] MTU 协商失败 ${err.code}，改用 20 字节片`);
        this.mtu = 23;
      }
    }
    this.startHeartbeat();
    this.onState(LinkState.READY, '蓝牙已连上板端');
    console.info(`[EVA] BLE READY（notify ${how}，chunk=${this.mtu - 3}）`);
    // 断链期间压着的路线，连上自动补发
    if (this.pendingRoute !== null) {
      const r = this.pendingRoute;
      this.pendingRoute = null;
      this.pushRouteJson(r);
    }
  }

  /** UUID 归一化：去掉连字符、统一大写，避免 128bit 字符串大小写/格式差异导致比较失败 */
  static norm(u: string): string {
    return u.replace(/-/g, '').toUpperCase();
  }


  private closeDev(): void {
    this.stopHeartbeat();
    this.gen++;                       // 作废旧连接的所有迟到回调
    const d = this.dev;
    this.dev = null;
    this.rxChar = null;
    this.txChar = null;
    this.acc = buffer.alloc(0);
    if (d !== null) {
      try {
        d.off('BLEConnectionStateChange');
        d.off('BLECharacteristicChange');
        d.disconnect();
        d.close();
      } catch (e) {
        // 已断开的设备再 close 会抛，忽略
      }
    }
  }

  private clearRetry(): void {
    if (this.retryTimer !== -1) {
      clearTimeout(this.retryTimer);
      this.retryTimer = -1;
    }
  }

  private retryLater(): void {
    this.clearRetry();
    if (!this.wantConnect) {
      return;
    }
    const ms = this.retryMs;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = -1;
      this.retryMs = Math.min(this.retryMs * 2, 4000);
      if (this.wantConnect) {
        this.scan();
      }
    }, ms);
  }

  /**
   * 心跳：BLE 断链一般有回调，但板子复位/掉电这类场景偶发收不到，
   * 所以仍然保留"25s 没收到任何帧就重建"这条保险。
   */
  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.hbTimer = setInterval(() => {
      if (!this.isReady()) {
        return;
      }
      const silent: number = Date.now() - this.lastRxAt;
      if (silent > 25000) {
        console.warn(`[EVA] 蓝牙链路静默 ${Math.round(silent / 1000)}s，重建`);
        this.closeDev();
        this.onState(LinkState.FAILED, '链路静默，重连中');
        this.retryLater();
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

  /* ---------------- 收 ---------------------------- */

  private onChars(ab: ArrayBuffer): void {
    if (ab === null || ab.byteLength === 0) {
      return;
    }
    this.lastRxAt = Date.now();
    const chunk = buffer.from(ab);
    this.acc = buffer.concat([this.acc, chunk]);
    this.drain();
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
      console.error(`[EVA] 蓝牙帧解析失步: ${(e as Error).message}`);
      this.acc = buffer.alloc(0);
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
          /* 没有定位不等于"什么都不用发"：目的地名和今日天气跟定位无关，
           * 板端圆屏的目标地一行就靠这帧。早先这里只发 {ok:false}，
           * 结果室内没定位时板端目的地一直是"未设终点"。 */
          const st = this.statusProvider();
          if (st !== null) {
            const m = st as Record<string, Object>;
            m['ok'] = false;
            m['note'] = 'no-fix';
            this.sendJson(MsgType.STATUS, m);
          } else {
            this.sendJson(MsgType.STATUS, { 'ok': false, 'note': 'no-fix' });
          }
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
        // 借着这次 30s 通联，把界面读数（目标地 + 今日天气）一并刷新
        this.pushStatus();
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

  /* ---------------- 发 ---------------- */

  private sendJson(type: MsgType, obj: Record<string, Object>): void {
    this.sendRaw(type, buffer.from(JSON.stringify(obj), 'utf-8'));
  }

  pushRouteJson(json: string): void {
    this.pendingRoute = json;      // 断链/重连后自动补发
    this.sendRaw(MsgType.ROUTE_PUSH, buffer.from(json, 'utf-8'));
  }

  pushHealth(obj: Record<string, Object>): void {
    this.sendJson(MsgType.HEALTH_PUSH, obj);
  }

  /** 把界面读数（目标地/今日天气/电量）推给板端；板端 handle_status 里逐项可选更新 */
  pushStatus(): void {
    if (!this.isReady()) {
      return;
    }
    const o = this.statusProvider();
    if (o === null) {
      return;
    }
    this.sendJson(MsgType.STATUS, o);
  }

  private delay(ms: number): Promise<void> {
    return new Promise<void>((resolve: () => void) => {
      setTimeout(() => resolve(), ms);
    });
  }

  private async sendRaw(type: MsgType, payload: buffer.Buffer): Promise<void> {
    const dev = this.dev;
    if (dev === null || this.rxChar === null) {
      return;
    }

    // 重帧（路线/健康）不能丢：等上一帧发完再发；轻帧（PING）宁可丢一帧也不打断大包。
    // GPS_RSP / STATUS 例外：板端 30s 才开一次校准窗口，丢了这一轮就得再等 30s，
    // 而 STATUS 里带着目的地和最近一次 GPS，丢了界面就停在旧数据上（实测 14:12:50 被丢过），
    // 所以这两个也排队（只等 2.5s）。
    const heavy: boolean = (type === MsgType.ROUTE_PUSH) || (type === MsgType.HEALTH_PUSH);
    const mustDeliver: boolean = heavy || (type === MsgType.GPS_RSP) || (type === MsgType.STATUS);
    if (this.busy) {
      if (!mustDeliver) {
        console.warn('[EVA] 上一帧还在发，丢弃本帧 type=' + type);
        return;
      }
      const waitCap: number = heavy ? 8000 : 2500;
      let waited: number = 0;
      while (this.busy && waited < waitCap) {
        await this.delay(20);
        waited += 20;
      }
      if (this.busy) {
        console.warn('[EVA] 等发送队列超时，丢弃本帧 type=' + type);
        return;
      }
    }
    this.busy = true;

    try {
      this.seq = (this.seq + 1) >>> 0;
      const frame = encode(type, this.seq, payload);
      const ab: ArrayBuffer = frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.length);

      // 鸿蒙侧拿不到"MTU 真协商值"，244 只是乐观假设；写失败就降到 20 字节片。
      // 关键改动：失败只重发"当前这一片"，不整帧从头来 —— 整帧重来会让板端重复拼接、CRC 必错。
      let chunk: number = (this.mtu > 4) ? this.mtu - 3 : 20;
      let off: number = 0;
      let chunks: number = 0;

      while (off < ab.byteLength) {
        let ok: boolean = false;
        let lastCode: number = 0;
        for (let attempt = 0; attempt < 3 && !ok; attempt++) {
          const n: number = Math.min(chunk, ab.byteLength - off);
          const piece: ArrayBuffer = ab.slice(off, off + n);
          const ch: ble.BLECharacteristic = {
            serviceUuid: SVC_UUID,
            characteristicUuid: RX_UUID,
            characteristicValue: piece,
            descriptors: []
          };
          try {
            await dev.writeCharacteristicValue(ch, ble.GattWriteType.WRITE);
            ok = true;
            off += n;
          } catch (e) {
            const be = e as BusinessError;
            lastCode = be.code;
            if (chunk > 20) {
              chunk = 20;
              this.mtu = 23;
              console.warn(`[EVA] 第 ${chunks} 片写失败 ${be.code}，降到 20 字节片重试`);
            }
            await this.delay(10 + attempt * 10);
          }
        }
        if (!ok) {
          throw new Error(`BLE 写失败 ${lastCode}`);
        }
        chunks++;
        // 片间留呼吸：大分片 3ms、20 字节片 6ms（板端 NimBLE 单缓冲，灌太快会回 2900099）
        await this.delay(chunk >= 100 ? 3 : 6);
      }

      console.info(`[EVA] BLE 发帧完成 type=${type} len=${ab.byteLength} 片=${chunks} chunk=${chunk}`);
    } catch (e) {
      const err = e as Error;
      console.error(`[EVA] BLE 写失败：${err.message}（按断链处理）`);
      this.closeDev();
      this.onState(LinkState.FAILED, `蓝牙写失败 ${err.message}`);
      this.retryLater();
    } finally {
      this.busy = false;
    }
  }

  /** MTU 变了要重新算分片；由外部（连接回调里）调用 */
  setMtu(m: number): void {
    if (m > 23) {
      this.mtu = m;
    }
  }
}
