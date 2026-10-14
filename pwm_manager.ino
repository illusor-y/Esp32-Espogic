#include "config.h"
#include <driver/ledc.h>
#include <driver/gpio.h>

// ===================== LEDC 驱动（S3 底层 timer/channel 精确映射）=====================
// 时钟（Clock）= 硬件 timer 的抽象：CLOCK_MAX=4 个时钟固定对应 timer0~3。
// 每个 PWM 通道 = 一个 LEDC channel（0~7），其输出频率由「活动条目所选时钟」决定，
// 活动条目切换时动态重绑到对应 timer。用 ESP-IDF 底层 API 精确控制，避免
// Arduino 高层 API 的"同频自动共享 timer"带来的改频互相牵连问题。

// 时钟全局（固定 4 槽位 = timer0~3）
ClockConfig clocks[CLOCK_MAX];

// 每通道当前绑定的时钟槽位（运行时缓存，-1 表示未绑定）
static int pwmChannelClock[PWM_CHANNEL_MAX];

// ===================== 可用 GPIO 枚举与引脚自定义 =====================
static int availPins[SOC_GPIO_PIN_COUNT];
static int availPinCount = 0;

void enumAvailablePins() {
  availPinCount = 0;
  for (int gpio = 0; gpio < SOC_GPIO_PIN_COUNT; gpio++) {
    if (!digitalPinIsValid(gpio)) continue;
    availPins[availPinCount++] = gpio;
  }
  String pins;
  pins.reserve(availPinCount * 4);
  for (int i = 0; i < availPinCount; i++) {
    if (i > 0) pins += ", ";
    pins += String(availPins[i]);
  }
  logf("[GPIO] 可用引脚（共 %d 个）: %s\n", availPinCount, pins.c_str());
}

bool isPinAvailable(int gpio) {
  for (int i = 0; i < availPinCount; i++) {
    if (availPins[i] == gpio) return true;
  }
  return false;
}

int getAvailablePinCount() { return availPinCount; }

int getAvailablePin(int index) {
  if (index < 0 || index >= availPinCount) return -1;
  return availPins[index];
}

// ===================== 分辨率计算 =====================
// S3 LEDC 时钟源 40MHz（XTAL）。可表示最大频率 = 40MHz / 2^res，res 须满足 2^res <= 40MHz/freq。
static uint8_t pwmResolutionForFreq(uint32_t freq) {
  if (freq == 0) return 1;
  unsigned long maxCount = 40000000UL / freq;
  uint8_t res = PWM_BIT_PRECISION_MAX;
  while (res > 1 && (1UL << res) > maxCount) res--;
  return res;
}

static ledc_timer_bit_t ledcBitFromRes(uint8_t res) {
  if (res < 1) res = 1;
  if (res > PWM_BIT_PRECISION_MAX) res = PWM_BIT_PRECISION_MAX;
  return (ledc_timer_bit_t)(LEDC_TIMER_1_BIT + (res - 1));
}

// ===================== LEDC 底层操作 =====================

// 配置某时钟对应的 timer（频率 + 分辨率）
static void clockApplyTimer(int slot) {
  if (slot < 0 || slot >= CLOCK_MAX || !clocks[slot].enabled) return;
  uint8_t res = pwmResolutionForFreq(clocks[slot].freq);
  clocks[slot].maxVal = (1UL << res) - 1;
  ledc_timer_config_t tc = {};
  tc.speed_mode = LEDC_LOW_SPEED_MODE;
  tc.timer_num = (ledc_timer_t)slot;
  tc.duty_resolution = ledcBitFromRes(res);
  tc.freq_hz = clocks[slot].freq;
  tc.clk_cfg = LEDC_AUTO_CLK;
  esp_err_t err = ledc_timer_config(&tc);
  if (err != ESP_OK) logf("[时钟] timer%d 配置失败 (err=%d)\n", slot, err);
}

// 将某通道绑定到某时钟（timer）
static void pwmBindChannelToClock(int channel, int slot) {
  if (channel < 0 || channel >= pwmChannelCount) return;
  if (slot < 0 || slot >= CLOCK_MAX || !clocks[slot].enabled) return;
  ledc_channel_config_t cc = {};
  cc.gpio_num = pwmPins[channel];
  cc.speed_mode = LEDC_LOW_SPEED_MODE;
  cc.channel = (ledc_channel_t)channel;
  cc.timer_sel = (ledc_timer_t)slot;
  cc.duty = 0;
  cc.hpoint = 0;
  cc.intr_type = LEDC_INTR_DISABLE;
  esp_err_t err = ledc_channel_config(&cc);
  if (err != ESP_OK) logf("[PWM] CH%d 绑定 timer%d 失败 (err=%d)\n", channel + 1, slot, err);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
}

// 输出占空比（duty 为计数满量程值：0 ~ 2^res-1）
static void pwmWriteDuty(int channel, uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
}

// 停止某通道输出（输出低电平）
static void pwmStopChannel(int channel) {
  if (channel < 0 || channel >= PWM_CHANNEL_MAX) return;
  ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, 0);
}

// 停止并释放某通道 PWM，供手动覆盖/锁存接管（GPIO 回到普通 IO）
void pwmReleaseForOverride(int channel) {
  if (channel < 0 || channel >= pwmChannelCount) return;
  ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, 0);
  pwmChannelClock[channel] = -1;
  gpio_reset_pin((gpio_num_t)pwmPins[channel]);
}

// 重置用：停止全部 LEDC 输出，并把这些引脚恢复为芯片复位态（输入、高阻、无上下拉）。
// 与 pwmReleaseForOverride 的唯一区别是逐个过滤 Flash/PSRAM 总线引脚：对这些引脚做
// gpio_reset_pin 会把 pad 从 SPI 功能切回 GPIO，片内 Flash 立即失联 panic。
void pwmReleaseAllForReset() {
  String released;
  for (int i = 0; i < pwmChannelCount; i++) {
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, 0);
    pwmChannelClock[i] = -1;
    if (isIoFlashPin(pwmPins[i])) {
      logf("[重置] PWM 跳过 Flash 总线引脚 GPIO%d\n", pwmPins[i]);
      continue;
    }
    gpio_reset_pin((gpio_num_t)pwmPins[i]);
    if (released.length()) released += ", ";
    released += "GPIO" + String(pwmPins[i]);
  }
  logf("[重置] PWM 引脚已释放: %s\n", released.length() ? released.c_str() : "(无)");
}

// ===================== 时钟管理 =====================

int clockGetFirstEnabled() {
  for (int i = 0; i < CLOCK_MAX; i++) if (clocks[i].enabled) return i;
  return -1;
}

bool clockIsUsed(int slot) {
  for (int i = 0; i < pwmChannelCount; i++) {
    for (int j = 0; j < pwmConfigs[i].taskCount; j++) {
      if (pwmConfigs[i].tasks[j].clockIndex == slot) return true;
    }
  }
  return false;
}

bool clockAdd(uint32_t freq) {
  stateAssertHeld();   // clocks 会被 loop 每轮读（条目绑定时钟）：必须在锁内
  int slot = -1;
  for (int i = 0; i < CLOCK_MAX; i++) if (!clocks[i].enabled) { slot = i; break; }
  if (slot < 0) { logf("[时钟] 添加失败：已达上限 %d 个\n", CLOCK_MAX); return false; }
  uint32_t f = (uint32_t)safeConstrain((int)freq, 1, 40000);
  for (int i = 0; i < CLOCK_MAX; i++) if (clocks[i].enabled && clocks[i].freq == f) {
    logf("[时钟] 添加失败：频率 %luHz 已存在\n", (unsigned long)f); return false;
  }
  clocks[slot].enabled = true;
  clocks[slot].freq = f;
  clockApplyTimer(slot);
  logf("[时钟] 已添加时钟%d (%luHz)\n", slot + 1, (unsigned long)f);
  return true;
}

bool clockRemove(int slot) {
  stateAssertHeld();   // clocks 会被 loop 每轮读：必须在锁内
  if (slot < 0 || slot >= CLOCK_MAX || !clocks[slot].enabled) return false;
  int cnt = 0;
  for (int i = 0; i < CLOCK_MAX; i++) if (clocks[i].enabled) cnt++;
  if (cnt <= 1) { logf("[时钟] 删除失败：至少保留一个时钟\n"); return false; }
  if (clockIsUsed(slot)) { logf("[时钟] 删除失败：此时钟被 PWM 条目引用\n"); return false; }
  clocks[slot].enabled = false;
  ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)slot);
  logf("[时钟] 已删除时钟%d\n", slot + 1);
  return true;
}

bool clockSetFreq(int slot, uint32_t freq) {
  if (slot < 0 || slot >= CLOCK_MAX || !clocks[slot].enabled) return false;
  uint32_t f = (uint32_t)safeConstrain((int)freq, 1, 40000);
  for (int i = 0; i < CLOCK_MAX; i++) if (i != slot && clocks[i].enabled && clocks[i].freq == f) {
    logf("[时钟] 修改失败：频率 %luHz 已存在\n", (unsigned long)f); return false;
  }
  // 先停止绑定到该时钟的所有通道，再重配 timer，最后重新绑定
  for (int i = 0; i < pwmChannelCount; i++) if (pwmChannelClock[i] == slot) pwmStopChannel(i);
  clocks[slot].freq = f;
  clockApplyTimer(slot);
  for (int i = 0; i < pwmChannelCount; i++) if (pwmChannelClock[i] == slot) pwmBindChannelToClock(i, slot);
  logf("[时钟] 时钟%d 频率已改为 %luHz\n", slot + 1, (unsigned long)f);
  return true;
}

// ===================== PWM 通道/条目管理 =====================

void initPWM() {
  for (int i = 0; i < PWM_CHANNEL_MAX; i++) pwmChannelClock[i] = -1;
  // 配置所有启用的时钟（timer）
  for (int i = 0; i < CLOCK_MAX; i++) if (clocks[i].enabled) clockApplyTimer(i);
  // 通道绑定由 updatePwmDirect 按活动条目动态完成

  String map;
  map.reserve(64);
  for (int i = 0; i < pwmChannelCount; i++) {
    if (i > 0) map += ", ";
    map += "CH";
    map += String(i + 1);
    map += "=GPIO";
    map += String(pwmPins[i]);
  }
  logf("[PWM] 引脚映射: %s\n", map.c_str());

  String cmap;
  cmap.reserve(64);
  for (int i = 0; i < CLOCK_MAX; i++) {
    if (!clocks[i].enabled) continue;
    if (cmap.length()) cmap += ", ";
    cmap += "时钟" + String(i + 1) + "=" + String(clocks[i].freq) + "Hz";
  }
  logf("[时钟] 时钟: %s\n", cmap.c_str());
}

// 将某通道重绑到新 GPIO（用于网页自定义引脚）
bool setChannelPin(int channel, int newPin) {
  if (channel < 0 || channel >= pwmChannelCount) return false;
  if (!isPinAvailable(newPin)) return false;
  int oldPin = pwmPins[channel];
  if (oldPin != newPin) {
    if (pwmChannelClock[channel] >= 0) pwmStopChannel(channel);
    if (latchIsEnabled(oldPin)) {   // 锁存脚不释放：改绑不该打断固定电平（连续性，见 PIN-5）
      logf("[PWM] GPIO%d 已锁存，保留其电平不释放\n", oldPin);
    } else {
      pinMode(oldPin, INPUT);
      digitalWrite(oldPin, LOW);
    }
  }
  pwmPins[channel] = newPin;
  pwmChannelClock[channel] = -1;   // 强制下一轮重新绑定
  logf("[PWM] CH%d 引脚已改为 GPIO%d\n", channel + 1, newPin);
  return true;
}

// 新增 PWM 通道（末尾追加），最多 PWM_CHANNEL_MAX 个
bool pwmAddChannel(int pin, bool devMode, bool allowDup) {
  if (pwmChannelCount >= PWM_CHANNEL_MAX) { logf("[PWM] 添加失败：通道数已达上限 %d\n", PWM_CHANNEL_MAX); return false; }
  if (!isPinAvailable(pin)) { logf("[PWM] 添加失败：GPIO%d 不在可用列表\n", pin); return false; }
  if (!devMode && isIoFlashPin(pin)) { logf("[PWM] 添加失败：GPIO%d 为 Flash/PSRAM 总线\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByPwm(pin)) { logf("[PWM] 添加失败：GPIO%d 已被其他 PWM 通道占用\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByIo(pin, -1)) { logf("[PWM] 添加失败：GPIO%d 被序列器占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByOutput(pin, -1)) { logf("[PWM] 添加失败：GPIO%d 被比较器输出占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByExternalInput(pin, -1)) { logf("[PWM] 添加失败：GPIO%d 被比较器外部输入占用\n", pin); return false; }
  if (!allowDup && counterIsPinUsedByAction(pin, -1)) { logf("[PWM] 添加失败：GPIO%d 被计数器动作占用\n", pin); return false; }

  int idx = pwmChannelCount;
  int defClock = clockGetFirstEnabled();
  pwmPins[idx] = pin;
  pwmConfigs[idx].enabled = false;
  pwmConfigs[idx].weatherInteract = false;
  pwmConfigs[idx].taskCount = 0;
  pwmConfigs[idx].currentDuty = 0.0;
  pwmConfigs[idx].targetDuty = 0.0;
  pwmConfigs[idx].weatherDuty = 0.0;
  pwmConfigs[idx].activeTask = 0;
  pwmConfigs[idx].sunEnabled = false;
  pwmConfigs[idx].sunMode = SUN_MODE_SUNRISE;
  pwmConfigs[idx].sunPeakDuty = 100;
  pwmConfigs[idx].sunActive = false;
  pwmConfigs[idx].sunStartMin = -1;
  pwmConfigs[idx].sunEndMin = -1;
  pwmConfigs[idx].sunDuty = 0.0f;
  for (int j = 0; j < PWM_TASK_MAX; j++) pwmConfigs[idx].tasks[j] = {false, 0, 0, 0, 0, 0, 100, defClock, false};
  pwmChannelClock[idx] = -1;
  pwmChannelCount++;
  logf("[PWM] 添加通道 CH%d -> GPIO%d（当前 %d 个）\n", idx + 1, pin, pwmChannelCount);
  return true;
}

// 删除 PWM 通道：释放引脚给 IO/比较器/计数器使用，后续通道前移
bool pwmRemoveChannel(int index) {
  if (index < 0 || index >= pwmChannelCount) return false;
  int pin = pwmPins[index];
  // 前移后 channel 索引会变，停止全部通道并强制重新绑定
  for (int i = 0; i < pwmChannelCount; i++) pwmStopChannel(i);
  if (latchIsEnabled(pin)) {
    // 锁存脚不释放：删通道不该打断固定电平（连续性，见 PIN-5）；手动基准不保护属产品决策
    logf("[PWM] GPIO%d 已锁存，删除通道但保留其电平\n", pin);
  } else {
    pinMode(pin, INPUT);
    digitalWrite(pin, LOW);
  }
  for (int i = index; i < pwmChannelCount - 1; i++) {
    pwmPins[i] = pwmPins[i + 1];
    pwmConfigs[i] = pwmConfigs[i + 1];
  }
  pwmChannelCount--;
  for (int i = 0; i < pwmChannelCount; i++) pwmChannelClock[i] = -1;
  logf("[PWM] 删除通道 CH%d (GPIO%d %s)，当前 %d 个\n", index + 1, pin,
       latchIsEnabled(pin) ? "保留锁存电平" : "已释放", pwmChannelCount);
  return true;
}

// 新增 PWM 条目（末尾追加），最多 PWM_TASK_MAX 条
bool pwmAddTask(int channel) {
  stateAssertHeld();   // pwmConfigs 会被 loop 每轮读：必须在锁内
  if (channel < 0 || channel >= pwmChannelCount) { logf("[PWM] 添加条目失败：无效通道\n"); return false; }
  PwmChannelConfig& c = pwmConfigs[channel];
  if (c.taskCount >= PWM_TASK_MAX) { logf("[PWM] 添加条目失败：CH%d 已达上限 %d 条\n", channel + 1, PWM_TASK_MAX); return false; }
  int defClock = clockGetFirstEnabled();
  c.tasks[c.taskCount] = {false, 0, 0, 0, 0, 0, 100, defClock, false};   // 默认：未启用，00:00→00:00，0%→100%，默认时钟
  c.taskCount++;
  pwmRecalcConflicts();
  logf("[PWM] CH%d 已添加条目 %d（当前 %d 条）\n", channel + 1, c.taskCount, c.taskCount);
  return true;
}

// 删除 PWM 条目：后续条目前移，重算冲突
bool pwmRemoveTask(int channel, int index) {
  stateAssertHeld();   // 条目前移 + 重算冲突：必须在锁内
  if (channel < 0 || channel >= pwmChannelCount) return false;
  PwmChannelConfig& c = pwmConfigs[channel];
  if (index < 0 || index >= c.taskCount) return false;
  for (int j = index; j < c.taskCount - 1; j++) c.tasks[j] = c.tasks[j + 1];
  c.taskCount--;
  pwmRecalcConflicts();
  logf("[PWM] CH%d 已删除条目 %d（当前 %d 条）\n", channel + 1, index + 1, c.taskCount);
  return true;
}

// 恢复某通道 PWM 输出（手动覆盖/锁存取消后重新挂载）
void pwmRestoreChannel(int channel) {
  if (channel < 0 || channel >= pwmChannelCount) return;
  pwmChannelClock[channel] = -1;   // 强制下一轮重新绑定
}

// ===================== 冲突计算 =====================

// 判断任务是否跨日（结束时间 <= 开始时间，含整 24 小时任务）
bool isTaskOvernight(const PwmTask& task) {
  if (!task.enabled) return false;
  int startSeconds = task.startHour * 3600 + task.startMinute * 60;
  int endSeconds = task.endHour * 3600 + task.endMinute * 60;
  return endSeconds <= startSeconds;
}

// 判断某时刻 t（秒，0..86399）是否落在任务窗口内（左闭右开 [start, end)）
static bool pwmTimeInTask(const PwmTask& task, int t) {
  int s = task.startHour * 3600 + task.startMinute * 60;
  int e = task.endHour * 3600 + task.endMinute * 60;
  if (isTaskOvernight(task)) return (t >= s || t < e);   // 跨午夜；S==E 视为全天
  return (t >= s && t < e);
}

// 两条任务的时间窗口是否重叠（左闭右开区间）
static bool pwmTasksOverlap(const PwmTask& a, const PwmTask& b) {
  int sa = a.startHour * 3600 + a.startMinute * 60;
  int sb = b.startHour * 3600 + b.startMinute * 60;
  return pwmTimeInTask(a, sb) || pwmTimeInTask(b, sa);
}

// 重算每个通道内所有条目的冲突标记：时间窗口重叠即标记（仅限同一通道/引脚）
void pwmRecalcConflicts() {
  for (int i = 0; i < pwmChannelCount; i++) {
    int n = pwmConfigs[i].taskCount;
    for (int j = 0; j < n; j++) pwmConfigs[i].tasks[j].conflict = false;
    for (int j = 0; j < n; j++) {
      PwmTask& a = pwmConfigs[i].tasks[j];
      if (!a.enabled) continue;
      for (int k = j + 1; k < n; k++) {
        PwmTask& b = pwmConfigs[i].tasks[k];
        if (!b.enabled) continue;
        if (pwmTasksOverlap(a, b)) {
          a.conflict = true;
          b.conflict = true;
        }
      }
    }
  }
}

// 计算目标占空比；支持跨午夜任务（如 22:00 → 次日02:00）
float calculateDuty(PwmChannelConfig& config, int& activeTask) {
  int currentSeconds = currentTime.hour * 3600 + currentTime.minute * 60 + currentTime.second;
  activeTask = 0;
  for (int j = 0; j < config.taskCount; j++) {
    PwmTask& task = config.tasks[j];
    if (!task.enabled || task.conflict) continue;   // 冲突任务不参与执行
    int startSeconds = task.startHour * 3600 + task.startMinute * 60;
    int endSeconds = task.endHour * 3600 + task.endMinute * 60;

    bool overnight = isTaskOvernight(task);
    int totalSeconds = overnight
      ? (86400 - startSeconds + endSeconds)
      : (endSeconds - startSeconds);
    bool inWindow = overnight
      ? (currentSeconds >= startSeconds || currentSeconds < endSeconds)   // 左闭右开 [start, end)
      : (currentSeconds >= startSeconds && currentSeconds < endSeconds);

    if (inWindow) {
      if (totalSeconds == 0) {
        activeTask = j + 1;
        return (float)task.endDuty;
      }
      int elapsedSeconds = overnight
        ? (currentSeconds >= startSeconds
             ? (currentSeconds - startSeconds)
             : (86400 - startSeconds + currentSeconds))
        : (currentSeconds - startSeconds);
      float ratio = (float)elapsedSeconds / totalSeconds;
      float duty = task.startDuty + (task.endDuty - task.startDuty) * ratio;
      float finalDuty = duty < 0 ? 0 : (duty > 100 ? 100 : duty);
      activeTask = j + 1;
      return finalDuty;
    }
  }
  return 0.0f;
}

void updatePwmTargetDuty() {
  int nowSec = currentTime.hour * 3600 + currentTime.minute * 60 + currentTime.second;
  // 接管状态的上一次快照：只在"进入/退出接管"时打一条日志，避免每 loop 刷屏
  static bool sunActivePrev[PWM_CHANNEL_MAX] = {false};
  for (int i = 0; i < pwmChannelCount; i++) {
    // ===== 阳光交互优先：开启且当日太阳时刻有效时，原始占空比由阳光曲线给出 =====
    // 时钟/频率不受影响（activeTask 记 0 → updatePwmDirect 用默认时钟输出）。
    // 天气系数照旧在 updatePwmDirect 里乘，所以这里的值是"天气现象之前的原始占空比"。
    if (pwmConfigs[i].enabled && pwmConfigs[i].sunEnabled) {
      int s = -1, e = -1;
      if (sunWindowOfChannel(i, s, e)) {
        if (!sunActivePrev[i]) {
          sunActivePrev[i] = true;
          logf("[阳光] CH%d 接管原始占空比：%s %02d:%02d→%02d:%02d，峰值%d%%\n", i + 1,
               pwmConfigs[i].sunMode == SUN_MODE_TWILIGHT ? "晨光→昏光" : "日出→日落",
               s / 60, s % 60, e / 60, e % 60, pwmConfigs[i].sunPeakDuty);
        }
        pwmConfigs[i].sunActive = true;
        pwmConfigs[i].sunStartMin = s;
        pwmConfigs[i].sunEndMin = e;
        pwmConfigs[i].sunDuty = sunDutyCurve(s, e, nowSec, pwmConfigs[i].sunPeakDuty);
        pwmConfigs[i].targetDuty = pwmConfigs[i].sunDuty;
        pwmConfigs[i].activeTask = 0;   // 条目计算被接管
        continue;
      }
    }
    // 无阳光接管（未开启 / 通道关闭 / 无时刻数据）：清运行态，走原条目逻辑（妥协处理）
    if (sunActivePrev[i]) {
      sunActivePrev[i] = false;
      logf("[阳光] CH%d 退出接管（无太阳时刻数据或通道关闭），按条目执行\n", i + 1);
    }
    pwmConfigs[i].sunActive = false;
    pwmConfigs[i].sunStartMin = -1;
    pwmConfigs[i].sunEndMin = -1;
    pwmConfigs[i].sunDuty = 0.0f;

    if (!pwmConfigs[i].enabled) {
      pwmConfigs[i].targetDuty = 0.0f;
      pwmConfigs[i].activeTask = 0;
      continue;
    }
    int activeTask;
    float duty = calculateDuty(pwmConfigs[i], activeTask);
    pwmConfigs[i].targetDuty = duty;
    pwmConfigs[i].activeTask = activeTask;
  }
}

// ===================== 天气系数过渡（线性斜坡）=====================
// 为什么只做在"天气系数"这一层，而不是最终输出：
//   跳变的唯一来源是"离散档位的天气系数"（100/90/80/70/60）。阳光曲线与条目插值本身是连续量
//   （实测速率 0.005~0.17 %/s，比过渡速率 0.027 %/s 慢 1~2 个数量级），限速器对它们完全透明；
//   而手动切换 / 锁存 / 通道启停属"有意跳变"，也不该被平滑。若做在最终输出层，这些会被一起拉长
//   —— 那就是"全局 wave"的味道。
// 为什么用线性斜坡而不是指数：25 分钟这种长过渡下，指数（时间常数 1500s）20 分钟才走 63%、
//   近 1 小时才收敛（拖尾巴）；若把时间常数调小凑够 25 分钟走完，前期速率又要快 4 倍。
//   线性保证"不管差 10 点还是 40 点，都在配置时长内匀速走完，且到点即停"。
// 状态：文件级 static，只有 loop 访问（HTTP 侧读的是 weatherGetDutyFactor() 的目标档位），无竞争。
struct WxFadeState {
  bool          inited;   // 是否已建立过渡状态
  bool          everReal; // 是否曾经拿到过【真实】天气档位（占位值 100% 不算）
  float         from;     // 过渡起点（%，天气系数）
  float         to;       // 过渡目标（%）
  unsigned long t0;       // 过渡起点时刻（millis）
  unsigned long durMs;    // 过渡时长（ms）；0 = 关闭过渡
};
static WxFadeState wxFadeS = { false, false, 100.0f, 100.0f, 0, 0 };
static bool wxFadeDoneLogged = false;   // "完成"日志每次过渡只打一次

// 把过渡状态直接对齐到某值（不产生过渡动画）
static void wxFadeSetValue(float v, unsigned long durMs) {
  wxFadeS.from   = v;
  wxFadeS.to     = v;
  wxFadeS.t0     = millis();
  wxFadeS.durMs  = durMs;
  wxFadeS.inited = true;
  wxFadeDoneLogged = false;
}

// 当前可见的系数（线性插值；未初始化或已到期即返回目标值）
static float wxFadeValue() {
  if (!wxFadeS.inited) return wxFadeS.to;
  if (wxFadeS.durMs == 0) return wxFadeS.to;          // 关闭过渡 → 立即切换
  unsigned long el = millis() - wxFadeS.t0;
  if (el >= wxFadeS.durMs) {
    // 只打一次：本函数每 loop 调用，不加闸会刷屏
    if (!wxFadeDoneLogged) {
      wxFadeDoneLogged = true;
      logf("[天气过渡] 完成 -> %.0f%%\n", (double)wxFadeS.to);
    }
    return wxFadeS.to;         // 到期精确落位，不留余差
  }
  wxFadeDoneLogged = false;
  float k = (float)el / (float)wxFadeS.durMs;
  return wxFadeS.from + (wxFadeS.to - wxFadeS.from) * k;
}

// 目标值或时长变化时重新起一段过渡：起点取"当前可见值"，保证衔接处不跳变。
// 起段/完成各打一条日志：用于区分"过渡正常起停"与"目标被反复改动"（后者才会显得像抖动）。
static void wxFadeRetarget(float target, unsigned long durMs) {
  float cur = wxFadeS.inited ? wxFadeValue() : target;
  if (wxFadeS.inited && cur != target) {
    if (durMs == 0) logf("[天气过渡] %.1f%% -> %.0f%%（立即）\n", (double)cur, (double)target);
    else logf("[天气过渡] %.1f%% -> %.0f%%（%lu 分钟）\n",
              (double)cur, (double)target, durMs / 60000UL);
  }
  wxFadeS.from   = cur;
  wxFadeS.to     = target;
  wxFadeS.t0     = millis();
  wxFadeS.durMs  = durMs;
  wxFadeS.inited = true;
  wxFadeDoneLogged = false;
}

// ===================== 变轨（只有天气系数过渡这一条）=====================
// 「变轨」= 天气档位跳变时，不让系数瞬间切换，而是在配置时长内匀速走过去（即上面那段 wxFadeS）。
//
// 曾经还有第二条轨道：时间被修改（拨钟 / 改时区）时，原始占空比也做一段同样的斜坡。
// 已按需求移除，理由：
//   原始占空比本身就是时间的连续函数（条目插值 / 阳光曲线，实测 0.005~0.17 %/s，比过渡速率
//   0.027 %/s 慢 1~2 个数量级），时间一改，它就"应当"立刻是新时间该有的值；
//   再套一段 25 分钟斜坡，等于把"设备在追时间"伪装成"过渡中"，用户还得点「立即完成」去跳过它。
// 代价：拨钟一瞬间亮度会跳一下 —— 那是用户主动操作的结果，直接体现反而更符合直觉。

// 某通道当前是否"正在过渡"（网页「变轨中」标签显隐用）。只有天气系数一条来源，
// 且只对启用了天气交互的通道有意义（没开的话系数根本不参与该通道输出）。
bool pwmIsRamping(int ch) {
  if (ch < 0 || ch >= pwmChannelCount) return false;
  if (!pwmConfigs[ch].weatherInteract) return false;
  return (wxFadeS.inited && wxFadeS.durMs > 0 &&
          wxFadeS.from != wxFadeS.to && (millis() - wxFadeS.t0) < wxFadeS.durMs);
}

// 变轨剩余时间（秒，向上取整；0 = 当前没有过渡），供网页「变轨中 mm:ss」显示。
//
// 计时基准是 millis()，不是墙上时间 —— 这一点是刻意的，也是"严格正计时"的落地方式：
//   · millis() 自开机单调递增（uint32 回绕时无符号减法仍然正确，只要单段过渡远短于 49.7 天）；
//   · 过渡途中用户把时间往回拨、往前跳、反复来回改，都动不了 t0，倒计时照旧走到 0；
//   · 反过来，过渡也不会因为"改时间"而被拉长或提前结束。
// 换言之：过渡有自己独立的一条时间轴，与 NTP / 拨钟 / 时区完全无关。
int pwmRampRemainSec() {
  if (!wxFadeS.inited || wxFadeS.durMs == 0) return 0;
  if (wxFadeS.from == wxFadeS.to) return 0;              // from==to → 本就无需过渡
  unsigned long el = millis() - wxFadeS.t0;
  if (el >= wxFadeS.durMs) return 0;
  return (int)((wxFadeS.durMs - el + 999UL) / 1000UL);   // 向上取整：还剩 0.2s 也显示 1 秒
}

// 「立即完成」：把正在进行的天气系数过渡直接落到目标档位（网页「变轨中」弹窗的按钮）。
// 天气系数是所有通道共用的一份值，所以这个动作天然作用于全部通道 —— 没有"只完成某一路"的说法。
void pwmCompleteFade() {
  stateAssertHeld();   // 与 loop 的 updatePwmDirect 争 wxFadeS
  wxFadeS.from = wxFadeS.to;        // from=to → 立即到位
  wxFadeDoneLogged = false;
  logf("[天气过渡] 立即完成：系数直接落到 %.0f%%\n", (double)wxFadeS.to);
}

// 直接输出占空比，无渐变；按活动条目的时钟动态绑定 timer
void updatePwmDirect() {
  // 天气系数先取（weatherGetDutyFactor 内部会拿 weatherMutex）：避免在持 stateMutex 时再去拿天气锁，
  // 让锁顺序恒为 stateMutex -> weatherMutex，杜绝反向嵌套
  int weatherFactor = weatherGetDutyFactor();   // 每 loop 取一次天气系数（目标档位：显示与日志用）
  // 实际参与输出的是"过渡值"：档位切换时在 wxFadeSec() 秒内匀速走过去（0 = 立即切换）
  unsigned long fadeMs = (unsigned long)wxFadeSec() * 1000UL;
  float factorNow;
  if (!weatherHasData()) {
    // ===== 还没有任何真实天气数据 =====
    // 此时 weatherGetDutyFactor() 返回的 100% 只是"没抓到"的占位值，**不能当作过渡起点**：
    // 否则首次抓到天气（例如 90%）时会从 100% 慢慢爬满一个过渡时长，而设备刚开机、输出本来
    // 还没起来，那段爬升纯属噪声（现象：开机后占空比一直在缓慢漂、很久到不了稳态）。
    // 因此这里把过渡状态直接按住不动，等真实档位到手再决定要不要过渡。
    wxFadeSetValue((float)weatherFactor, fadeMs);
    factorNow = (float)weatherFactor;
  } else if (!wxFadeS.everReal) {
    // ===== 首次拿到真实天气档位：直接到位，不做过渡 =====
    // （设备刚开机，输出尚未稳定；从"占位 100%"过渡过去是无意义的等待）
    wxFadeSetValue((float)weatherFactor, fadeMs);
    wxFadeS.everReal = true;
    factorNow = (float)weatherFactor;
    logf("[天气过渡] 首次获得真实天气，系数直接到位 %.0f%%\n", (double)factorNow);
  } else {
    // ===== 正常运行：只有档位真正变化（或过渡时长被改）时才起一段过渡 =====
    if ((float)weatherFactor != wxFadeS.to || wxFadeS.durMs != fadeMs) {
      wxFadeRetarget((float)weatherFactor, fadeMs);
    }
    factorNow = wxFadeValue();
  }
  StateLockGuard _lg;   // 与 HTTP 任务互斥：pwmConfigs / pwmPins / 手动覆盖 / 锁存都会被 handler 改
  static int lastFactor = -1;
  static bool lastInteract[PWM_CHANNEL_MAX];
  static bool wiInit = false;
  if (!wiInit) { wiInit = true; for (int k = 0; k < PWM_CHANNEL_MAX; k++) lastInteract[k] = false; }
  int defClock = clockGetFirstEnabled();
  // 天气交互日志合并成一行：原来每通道一行，8 通道约 640 字节，实测 updatePwmDirect 阶段
  // 耗时 37ms 全是串口写（且写在持状态锁期间，会同时阻塞 loop 与 HTTP 任务）。
  // 记录条件不变：天气系数变化，或该通道交互开关变化。未启用通道不占篇幅。
  // 硬约束：含时间戳必须 ≤ LOG_MAX_LEN-1(159)，故累计部分限制在 115 字节内，超出即截断。
  String weatherLog;
  bool weatherLogCut = false;
  for (int i = 0; i < pwmChannelCount; i++) {
    if (!pwmConfigs[i].enabled) {
      pwmConfigs[i].currentDuty = 0;
      pwmConfigs[i].weatherDuty = 0;
      if (pwmChannelClock[i] >= 0) pwmStopChannel(i);
      pwmChannelClock[i] = -1;
      continue;
    }
    if (latchIsEnabled(pwmPins[i])) continue;       // 被锁存，最高权限，暂停 PWM 输出
    if (manualIsOverridden(pwmPins[i])) continue;   // 被手动覆盖，暂停 PWM 输出
    if (defClock < 0) continue;                      // 无时钟，无法输出

    // 确定活动条目所选时钟
    int slot = defClock;
    if (pwmConfigs[i].activeTask > 0) {
      int t = pwmConfigs[i].activeTask - 1;
      int c = pwmConfigs[i].tasks[t].clockIndex;
      if (c >= 0 && c < CLOCK_MAX && clocks[c].enabled) slot = c;
    }

    // 时钟变化 → 先停旧绑定，再绑定新 timer
    if (pwmChannelClock[i] != slot) {
      if (pwmChannelClock[i] >= 0) pwmStopChannel(i);
      pwmBindChannelToClock(i, slot);
      pwmChannelClock[i] = slot;
    }

    // 原始占空比：直接采用本轮算出的目标值。
    // 时间被修改时不做任何斜坡 —— 原始占空比是时间的连续函数，时间一变它就该是新值。
    float rawDuty = pwmConfigs[i].targetDuty;
    pwmConfigs[i].currentDuty = rawDuty;   // 原始占空比（网页显示的"当前占空比"）

    // 天气交互：原始 × 天气系数（天气未抓到/未知系数为100，等效原始输出）。
    // 系数用 factorNow（过渡值）而非 weatherFactor（档位）：档位切换时占空比匀速过渡；
    // 档位不变时 factorNow 恒等于 weatherFactor，行为与改造前完全一致。
    float finalDuty = rawDuty;
    if (pwmConfigs[i].weatherInteract) {
      finalDuty = rawDuty * factorNow / 100.0f;
    }
    pwmConfigs[i].weatherDuty = finalDuty;   // 最终输出占空比

    if (weatherFactor != lastFactor || pwmConfigs[i].weatherInteract != lastInteract[i]) {
      if (!weatherLogCut) {
        char one[28];
        // 通道号用 1-based（与 [PWM] 引脚映射 / [NVS] 通道状态 / [阳光] 等日志一致），
        // 否则同一路通道在不同日志里编号相差 1，排查时会看错通道
        snprintf(one, sizeof(one), " CH%d:w%d=%.1f", i + 1, pwmConfigs[i].weatherInteract ? 1 : 0, (double)finalDuty);
        if (weatherLog.length() + strlen(one) <= 115) weatherLog += one;
        else { weatherLog += " ..."; weatherLogCut = true; }
      }
    }
    lastInteract[i] = pwmConfigs[i].weatherInteract;

    uint32_t dutyVal = (uint32_t)(finalDuty / 100.0f * (float)clocks[slot].maxVal);
    pwmWriteDuty(i, dutyVal);
  }
  // 日志区分"目标档位"与"过渡中的当前值"：否则过渡刚起时会出现"系数=90% 但输出按 100% 算"的怪现象，
  // 让人误以为钳制失效。头部长度受 LOG_MAX_LEN(160) 约束，故不写"过渡中"三字，只用箭头表示。
  if (weatherLog.length()) {
    if (fadeMs == 0 || factorNow == (float)weatherFactor) {
      logf("[PWM天气] 系数=%d%%%s\n", weatherFactor, weatherLog.c_str());
    } else {
      logf("[PWM天气] 系数=%d%%→%.1f%%%s\n", weatherFactor, (double)factorNow, weatherLog.c_str());
    }
  }
  lastFactor = weatherFactor;
}

void updatePWMOutput() {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：pwmConfigs 会被 handler 改（增删通道/条目、开关、天气交互）
  updatePwmTargetDuty();
}
