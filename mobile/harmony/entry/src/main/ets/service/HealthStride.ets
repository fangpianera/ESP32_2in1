/**
 * HealthStride.ets —— 鸿蒙端健康数据 → 身高 / 步长基准
 *
 * 【本轮按 SDK 真常量改写】
 *   之前以为"SDK 没给数据类型常量"，翻 d.ts 后确认有：
 *     healthStore.healthDataTypes.DAILY_ACTIVITIES（步数 step / 距离 distance，单位 m）
 *     healthStore.healthDataTypes.HEIGHT（height，单位 cm）
 *   字段名同样来自 d.ts 的 healthFields：DailyActivities{step,calorie,distance,...} / Height{height}
 *   → 不再需要往 Config 里手填数字 ID。
 *
 * 【真实限制（不要掩盖）】
 *   READ_HEALTH_DATA 属受限权限（ACL），module.json5 里没声明、个人调试签名也拿不到，
 *   所以 requestAuthorizations 会失败，此时自动回落到"手动身高 + 经验步长"，不影响导航。
 *   以 C 列说明的路径：AGC 申请 ACL → module.json5 放开权限 → 这里即自动生效。
 */
import { common } from '@kit.AbilityKit';
import { healthStore } from '@kit.HealthServiceKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { AppCfg } from '../common/Config';

export interface HealthPayload {
  height_cm?: number;
  sex: string;          // "m" / "f"
  avg_step_m?: number;
  steps_7d: number;
  dist_7d_m: number;
  conf: number;
  src: string;
}

export class HealthStride {
  private cached: HealthPayload | null = null;
  private manualHeightCm: number = 0;
  private manualSex: string = 'm';
  private inited: boolean = false;
  private authorized: boolean = false;

  setManualProfile(heightCm: number, sex: string): void {
    this.manualHeightCm = heightCm;
    this.manualSex = sex;
    this.buildFallback();
  }

  /** 当前身高（来自华为健康，或手动兜底） */
  heightCm(): number {
    if (this.cached !== null && this.cached.height_cm !== undefined) {
      return this.cached.height_cm;
    }
    return this.manualHeightCm;
  }

  /** 数据来源的一句话描述，给 UI 显示 */
  source(): string {
    if (this.cached === null) {
      return '未初始化';
    }
    switch (this.cached.src) {
      case 'healthservice-harmony': return '华为健康';
      case 'height-fallback': return '手动身高';
      default: return this.cached.src;
    }
  }

  /** 路径 A-1：初始化 + 申请「日常活动 / 身高」读权限 */
  async requestPermission(ctx: common.UIAbilityContext): Promise<boolean> {
    if (!AppCfg.HEALTH_ENABLE) {
      console.info('[EVA] HEALTH_ENABLE=false，走身高经验式');
      return false;
    }
    try {
      if (!this.inited) {
        await healthStore.init(ctx);
        this.inited = true;
      }
      const req: healthStore.AuthorizationRequest = {
        readDataTypes: [healthStore.healthDataTypes.DAILY_ACTIVITIES, healthStore.healthDataTypes.HEIGHT],
        writeDataTypes: []
      };
      const res: healthStore.AuthorizationResponse = await healthStore.requestAuthorizations(ctx, req);
      this.authorized = res.readDataTypes.length > 0;
      return this.authorized;
    } catch (e) {
      const err = e as BusinessError;
      // 201 = 权限未声明/未授予（READ_HEALTH_DATA 是 ACL 受限权限，个人签名拿不到）
      console.warn(`[EVA] 健康数据授权不可用 ${err.code}: ${err.message}，改用手动身高`);
      return false;
    }
  }

  /** 路径 A-2：拉近 7 天步数与步行距离，算平均步长；并取最近一次身高 */
  async refresh(): Promise<void> {
    if (!this.authorized) {
      this.buildFallback();
      return;
    }
    try {
      const end: number = Date.now();
      const start: number = end - 7 * 24 * 3600 * 1000;

      const dailyReq: healthStore.SamplePointReadRequest = {
        startTime: start,
        endTime: end,
        samplePointDataType: healthStore.healthDataTypes.DAILY_ACTIVITIES
      };
      const daily = await healthStore.readData<healthStore.SamplePoint>(dailyReq);

      let stepSum: number = 0;
      let distSum: number = 0;
      for (const row of daily) {
        const f = row.fields as Record<string, number | string | boolean>;
        if (f === undefined) {
          continue;
        }
        const st = f['step'];
        if (typeof st === 'number' && isFinite(st)) {
          stepSum += st;
        }
        const ds = f['distance'];
        if (typeof ds === 'number' && isFinite(ds)) {
          distSum += ds;
        }
      }

      const p: HealthPayload = {
        sex: this.manualSex,
        steps_7d: stepSum,
        dist_7d_m: distSum,
        conf: 0,
        src: 'healthservice-harmony'
      };

      const hReq: healthStore.SamplePointReadRequest = {
        startTime: start,
        endTime: end,
        samplePointDataType: healthStore.healthDataTypes.HEIGHT,
        sortOrder: healthStore.SortOrder.DESC,
        count: 1
      };
      const h = await healthStore.readData<healthStore.SamplePoint>(hReq);
      if (h.length > 0) {
        const hf = h[0].fields as Record<string, number | string | boolean>;
        const hv = hf['height'];
        if (typeof hv === 'number' && isFinite(hv) && hv > 100 && hv < 230) {
          p.height_cm = hv;
        }
      }
      if (p.height_cm === undefined && this.manualHeightCm > 0) {
        p.height_cm = this.manualHeightCm;
      }
      if (stepSum > 0 && distSum > 0) {
        p.avg_step_m = Math.round((distSum / stepSum) * 1000) / 1000;
        p.conf = Math.min(1, stepSum / 10000);
      }
      this.cached = p;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[EVA] 健康数据读取失败 ${err.code}: ${err.message}，回落身高经验式`);
      this.buildFallback();
    }
  }

  /** 路径 B：只用身高推步长（保证永远有一个可用的基准） */
  private buildFallback(): void {
    if (this.manualHeightCm <= 0) {
      return;
    }
    const h = this.manualHeightCm;
    const lh = (this.manualSex === 'f' ? 0.413 : 0.415) * h;
    const p: HealthPayload = {
      height_cm: h,
      sex: this.manualSex,
      avg_step_m: Math.round(lh * 1000) / 1000,
      steps_7d: 0,
      dist_7d_m: 0,
      conf: 0.35,           // 低置信：板端把主权重交给 Weinberg 实时项
      src: 'height-fallback'
    };
    this.cached = p;
  }

  payload(): Record<string, Object> | null {
    if (this.cached === null) {
      return null;
    }
    const obj: Record<string, Object> = {};
    obj['sex'] = this.cached.sex;
    obj['steps_7d'] = this.cached.steps_7d;
    obj['dist_7d_m'] = this.cached.dist_7d_m;
    obj['conf'] = this.cached.conf;
    obj['src'] = this.cached.src;
    if (this.cached.height_cm !== undefined) { obj['height_cm'] = this.cached.height_cm; }
    if (this.cached.avg_step_m !== undefined) { obj['avg_step_m'] = this.cached.avg_step_m; }
    return obj;
  }
}
