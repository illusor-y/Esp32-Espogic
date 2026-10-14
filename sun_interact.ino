#include "config.h"

// ===================== 阳光交互 =====================
// 模型：一个 PWM 通道可开启"阳光交互"。开启且当日太阳时刻有效时，该通道的"原始占空比"
// （= 天气系数之前的 targetDuty）不再由条目计算，改由太阳时刻生成：
//     起点 0%  →  两时刻中点 = 用户设定的最高原始占空比  →  终点 0%
// 两段线性（三角波）。窗口之外（含夜间）恒 0%。
//
// 时刻来源：天气模块每轮抓取时写入 RAM 的四个值（日出/日落来自 Open-Meteo，
// 晨光/昏光由 solar.ino 在抓取时算出）。对应模式的时刻缺失（-1）或非法（终点<=起点）
// 时判定为"无数据" → 通道完全回退条目逻辑（不干预）—— 这就是约定好的"妥协处理"。
//
// 与气象交互的关系：两者可同时开启。阳光只负责给出"原始占空比"，之后仍按原链路
// 乘天气系数（updatePwmDirect），所以"最高原始占空比"正是天气现象之前的那个值。
//
// 与条目的关系：阳光生效时忽略条目计算（updatePwmTargetDuty 里 activeTask 记 0）；
// 无数据时条目照常。前端据此把条目输入置灰并显示"阳光覆盖"标签。

// 取某通道当前应使用的时刻窗口。返回 false = 未开启 / 无数据 / 数据非法。
bool sunWindowOfChannel(int channel, int& startMin, int& endMin) {
  startMin = -1;
  endMin = -1;
  if (channel < 0 || channel >= pwmChannelCount) return false;
  const PwmChannelConfig& c = pwmConfigs[channel];
  if (!c.sunEnabled) return false;

  int sunrise = -1, sunset = -1, dawn = -1, dusk = -1;
  weatherGetSunTimes(sunrise, sunset, dawn, dusk);

  if (c.sunMode == SUN_MODE_TWILIGHT) { startMin = dawn; endMin = dusk; }
  else                                { startMin = sunrise; endMin = sunset; }

  // 有效性：两端都要有（-1 = 无数据/当日无此事件），且起点严格早于终点。
  // 不成立即视为"无数据" → 回退条目，绝不猜一个窗口出来。
  if (startMin < 0 || endMin < 0) return false;
  if (endMin <= startMin) return false;
  return true;
}

// 三角波：0 → 峰值 → 0。secOfDay 为"当天已过秒数"（0..86399）。
// 起点之前、终点之后（含夜间）一律 0%。
float sunDutyCurve(int startMin, int endMin, int secOfDay, int peakDuty) {
  if (startMin < 0 || endMin <= startMin) return 0.0f;
  long s   = (long)startMin * 60;
  long e   = (long)endMin * 60;
  long mid = (s + e) / 2;          // 两时刻中点：曲线峰值所在
  long t   = (long)secOfDay;
  if (t <= s || t >= e) return 0.0f;

  float peak = (float)safeConstrain(peakDuty, 0, 100);
  if (t <= mid) {
    if (mid <= s) return peak;                          // 极端窄窗口：避免除零
    return peak * (float)(t - s) / (float)(mid - s);    // 上升段
  }
  if (e <= mid) return peak;                            // 极端窄窗口：避免除零
  return peak * (float)(e - t) / (float)(e - mid);      // 下降段
}
