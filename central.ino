#include "config.h"

// ===================== 中心面板（只读监控 + 电平反转归因）=====================
// 本模块不产生任何 GPIO 输出，仅做两件事：
//   1) 采样：重建"配置中正在使用"的引脚集合，对普通 GPIO 做边沿检测，记录电平反转事件；
//   2) 归因：对任意引脚，逐模块排查（锁存 > 序列器暂态(脉冲/心跳) > 手动基准 > PWM > 序列器基础 >
//            比较器 > 计数器 > 触发源/外部输入），整理成「模块 - 组 - 条目」三级结构，供网页只读查看。
// PWM 引脚由 LEDC 硬件驱动，digitalRead 无法反映真实占空比，故不参与边沿检测，
// 单独在"行为区"以"运行中的 PWM"形式展示。
// 心跳器引脚（序列器持久心跳驱动）同样不参与边沿事件记录 —— 毫秒级持续翻转会把
// CENTRAL_EVENT_MAX 的环形缓冲瞬间冲光，行为区只剩它一个。它仍出现在监控区（带「持久心跳器」
// 标签、显示实时电平），精确边沿数由计数器的计量面板统计，两处职责不重叠。

#define CENTRAL_EVENT_MAX 50

struct CentralEvent {
  uint8_t  pin;
  uint8_t  fromLevel;
  uint8_t  toLevel;
  uint32_t seq;
  char     clock[9];    // 触发时刻 "HH:MM:SS"
  char     reason[24];  // 驱动模块简短名（锁存/脉冲器/持久心跳器/手动/序列器/比较器/计数器…）
};

static CentralEvent centralEvents[CENTRAL_EVENT_MAX];
static uint32_t centralEventCount = 0;   // 已记录事件总数（环形写满后仍递增）
static uint32_t centralEventSeq = 0;     // 事件序号（唯一游标）
static uint8_t  centralLastLevel[SOC_GPIO_PIN_COUNT];
static bool     centralLevelInit[SOC_GPIO_PIN_COUNT];
static bool     centralPinInUse[SOC_GPIO_PIN_COUNT];

static void centralMarkPin(int pin) {
  if (pin >= 0 && pin < SOC_GPIO_PIN_COUNT) centralPinInUse[pin] = true;
}

// 重建"配置中使用中"的引脚集合（IO 组 / 比较器输入输出 / 计数器触发源与动作 /
// PWM / 手动覆盖 / 锁存），去重后用于监控区展示与边沿检测。
void centralRebuildPinSet() {
  memset(centralPinInUse, 0, sizeof(centralPinInUse));
  for (int g = 0; g < ioGroupCount; g++) centralMarkPin(ioGroups[g].pin);
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++)
      if (comparatorGates[g].inputs[i].source == COMPARATOR_SRC_EXTERNAL)
        centralMarkPin(comparatorGates[g].inputs[i].refIndex);
    for (int a = 0; a < comparatorGates[g].actionCount; a++)
      centralMarkPin(comparatorGates[g].actions[a].pin);
  }
  for (int t = 0; t < counterTaskCount; t++) {
    centralMarkPin(counterTasks[t].triggerPin);
    for (int a = 0; a < counterTasks[t].actionCount; a++)
      centralMarkPin(counterTasks[t].actions[a].pin);
  }
  for (int i = 0; i < pwmChannelCount; i++) centralMarkPin(pwmPins[i]);
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
    if (manualOverride[p] || latchEnabled[p]) centralMarkPin(p);
  }
}

// 当前实际决定该引脚电平的模块（简短中文名），优先级与全局仲裁一致。
// causeByTransition=true：序列器分支按"边沿成因"判定（变化前那个电平的来源）——
//   脉冲到期时引脚是被当基础电平写回去的，若按当前状态判定，那条下降沿会被误标成"序列器"；
// causeByTransition=false：按"当前状态"判定，供详情面板显示此刻的驱动者。
static const char* centralDriverOf(int pin, bool causeByTransition) {
  if (latchEnabled[pin]) return "锁存";
  // 序列器的暂态（脉冲/心跳）优先于手动切换：手动值只是"基准电平"，暂态叠加在它之上，
  // 所以脉冲期间该引脚的真实驱动者是脉冲器，而不是手动。
  for (int g = 0; g < ioGroupCount; g++) {
    if (ioGroups[g].pin != pin) continue;
    int k = causeByTransition ? ioGetEdgeCauseKind(pin) : ioGetTransientKind(pin);
    if (k == IO_MODE_PULSE) return "脉冲器";
    if (k == IO_MODE_HEARTBEAT) return "持久心跳器";
    break;   // 有组但无暂态 → 落到下面的 手动 / PWM / 序列器 判定
  }
  if (manualOverride[pin]) return "手动";
  for (int i = 0; i < pwmChannelCount; i++) if (pwmPins[i] == pin) return "PWM";
  for (int g = 0; g < ioGroupCount; g++) if (ioGroups[g].pin == pin) return "序列器";
  for (int g = 0; g < comparatorGateCount; g++)
    for (int a = 0; a < comparatorGates[g].actionCount; a++)
      if (comparatorGates[g].actions[a].pin == pin) return "比较器";
  for (int t = 0; t < counterTaskCount; t++)
    for (int a = 0; a < counterTasks[t].actionCount; a++)
      if (counterTasks[t].actions[a].pin == pin) return "计数器";
  for (int t = 0; t < counterTaskCount; t++)
    if (counterTasks[t].triggerPin == pin) return "触发源";
  for (int g = 0; g < comparatorGateCount; g++)
    for (int i = 0; i < comparatorGates[g].inputCount; i++)
      if (comparatorGates[g].inputs[i].source == COMPARATOR_SRC_EXTERNAL && comparatorGates[g].inputs[i].refIndex == pin)
        return "外部输入";
  return "输入";
}

// loop 末尾调用：采样并记录边沿事件（不触碰 PWM 引脚与心跳器引脚）
void updateCentralMonitor() {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：centralEvents 环形缓冲会被 /centralstatus 读取
  centralRebuildPinSet();
  for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
    if (!centralPinInUse[pin]) continue;
    if (ioIsPinUsedByPwm(pin)) continue;   // PWM 由硬件输出，不做边沿检测
    int level = digitalRead(pin) ? 1 : 0;
    if (!centralLevelInit[pin]) {
      centralLevelInit[pin] = true;
      centralLastLevel[pin] = level;
      continue;                            // 首次采样只记录，不报事件
    }
    if (level != centralLastLevel[pin]) {
      uint8_t oldLevel = centralLastLevel[pin];
      centralLastLevel[pin] = level;   // 快照始终更新，保证后续边沿判定正确
      // 心跳器驱动的引脚与 PWM 同等对待：完全不参与边沿事件记录。
      // 心跳是毫秒级持续翻转，逐条落事件会在 1 秒内把 CENTRAL_EVENT_MAX(50) 的环形缓冲冲光，
      // 行为区就只剩这一个引脚。要看心跳是否在跑，监控区的实时电平 + 「心跳器」标签已经足够；
      // 精确边沿数由计数器（计量面板）负责，那边不做任何排除。
      // 此处不能改成"限流"：脉冲的"拉高"和"回到基础电平"两个边沿可能间隔不到 1 秒，限流会吃掉后者。
      if (ioGetTransientKind(pin) == IO_MODE_HEARTBEAT) continue;
      CentralEvent& e = centralEvents[centralEventCount % CENTRAL_EVENT_MAX];
      e.pin = (uint8_t)pin;
      e.fromLevel = oldLevel;
      e.toLevel = (uint8_t)level;
      e.seq = ++centralEventSeq;
      snprintf(e.clock, sizeof(e.clock), "%02d:%02d:%02d",
               currentTime.hour, currentTime.minute, currentTime.second);
      snprintf(e.reason, sizeof(e.reason), "%s", centralDriverOf(pin, true));   // 边沿成因
      centralEventCount++;
    }
  }
}

// 组内"当前应生效"的条目索引（含跨午夜环回），与 ioComputeState 语义一致。
// 只认定时开关条目：脉冲/心跳不提供基础电平，否则会顶掉基础条目的高亮。
static int centralIoActiveEntry(const IoGroup& grp, int hour, int minute) {
  int now = hour * 60 + minute;
  int latestIdx = -1, latestMin = -1;   // 全天最晚（环回用）
  int bestIdx = -1, bestMin = -1;       // 当天 <= now 的最晚
  for (int i = 0; i < grp.entryCount; i++) {
    if (grp.entries[i].mode != IO_MODE_SCHEDULE) continue;
    if (grp.entries[i].conflict) continue;
    int m = grp.entries[i].hour * 60 + grp.entries[i].minute;
    if (latestIdx < 0 || m > latestMin) { latestMin = m; latestIdx = i; }
    if (m <= now && (bestIdx < 0 || m > bestMin)) { bestMin = m; bestIdx = i; }
  }
  return bestIdx >= 0 ? bestIdx : latestIdx;
}

static void centralItem(JsonArray& arr, const char* module, const char* group, const char* entry, bool active) {
  JsonObject o = arr.createNestedObject();
  o["module"] = module;
  o["group"] = group;
  o["entry"] = entry;
  o["active"] = active;
}

// 组装某引脚的完整触发归因（模块-组-条目），写入 doc 的 "items" 数组。
void centralBuildDetail(JsonDocument& doc, int pin) {
  bool isPwm = ioIsPinUsedByPwm(pin);
  doc["pin"] = pin;
  doc["level"] = isPwm ? -1 : (digitalRead(pin) ? 1 : 0);
  doc["driver"] = centralDriverOf(pin, false);   // 详情面板：显示此刻的驱动者，不取边沿成因
  JsonArray items = doc.createNestedArray("items");

  bool overridden = latchEnabled[pin] || manualOverride[pin];   // 基础电平层被压过（锁存 或 手动基准）
  bool drivable = !latchEnabled[pin];                           // 是否还能被 脉冲/比较器/计数器 驱动
  char grp[32];
  char buf[128];

  // 1) 锁存（最高权限）
  if (latchEnabled[pin]) {
    snprintf(grp, sizeof(grp), "GPIO%d", pin);
    centralItem(items, "锁存", grp, latchLevel[pin] ? "锁存为高电平" : "锁存为低电平", true);
  }

  // 2) 手动切换（设定基准电平，不是锁）
  if (manualOverride[pin]) {
    snprintf(grp, sizeof(grp), "GPIO%d", pin);
    centralItem(items, "手动", grp, manualLevel[pin] ? "基准为高电平" : "基准为低电平", !latchEnabled[pin]);
  }

  // 3) PWM 通道
  for (int i = 0; i < pwmChannelCount; i++) {
    if (pwmPins[i] != pin) continue;
    snprintf(grp, sizeof(grp), "CH%d (GPIO%d)", i + 1, pin);
    bool run = pwmConfigs[i].enabled && pwmConfigs[i].activeTask > 0;
    if (pwmConfigs[i].sunEnabled) {
      // 阳光接管状态优先展示（它决定原始占空比，条目此时不参与计算）
      if (!pwmConfigs[i].enabled) {
        centralItem(items, "PWM", grp, "阳光交互已开启，但通道已关闭", false);
      } else if (pwmConfigs[i].sunActive) {
        snprintf(buf, sizeof(buf), "阳光交互：%s %02d:%02d→%02d:%02d，峰值%d%%，当前%.1f%%",
                 pwmConfigs[i].sunMode == SUN_MODE_TWILIGHT ? "晨光→昏光" : "日出→日落",
                 pwmConfigs[i].sunStartMin / 60, pwmConfigs[i].sunStartMin % 60,
                 pwmConfigs[i].sunEndMin / 60, pwmConfigs[i].sunEndMin % 60,
                 pwmConfigs[i].sunPeakDuty, (double)pwmConfigs[i].sunDuty);
        centralItem(items, "PWM", grp, buf, !overridden);
      } else {
        centralItem(items, "PWM", grp, "阳光交互已开启，无数据退回条目", false);
      }
    }
    if (run) {
      int t = pwmConfigs[i].activeTask - 1;
      const PwmTask& task = pwmConfigs[i].tasks[t];
      snprintf(buf, sizeof(buf), "运行任务%d：%02d:%02d→%02d:%02d 占空比%d%%→%d%%",
               pwmConfigs[i].activeTask,
               task.startHour, task.startMinute, task.endHour, task.endMinute,
               task.startDuty, task.endDuty);
      centralItem(items, "PWM", grp, buf, !overridden);
    } else if (pwmConfigs[i].enabled) {
      centralItem(items, "PWM", grp, pwmConfigs[i].sunEnabled ? "阳光交互生效中，条目被接管" : "通道已启用，无运行任务", false);
    } else {
      centralItem(items, "PWM", grp, "通道已关闭", false);
    }
  }

  // 4) 序列器组
  for (int g = 0; g < ioGroupCount; g++) {
    if (ioGroups[g].pin != pin) continue;
    IoGroup& gr = ioGroups[g];
    int active = centralIoActiveEntry(gr, currentTime.hour, currentTime.minute);
    int kind = IO_MODE_SCHEDULE;
    ioLevelAt(g, currentTime.hour, currentTime.minute, &kind);
    if (gr.entryCount == 0) {
      centralItem(items, "序列器", gr.name, "无条目", false);
    }
    if (!gr.enabled) {
      centralItem(items, "序列器", gr.name, "组已关闭（不参与调度）", false);
    }
    for (int j = 0; j < gr.entryCount; j++) {
      const IoEntry& e = gr.entries[j];
      if (e.mode == IO_MODE_PULSE)
        snprintf(buf, sizeof(buf), "%02d:%02d 瞬时脉冲 %s %lums%s", e.hour, e.minute,
                 e.action ? "拉高" : "拉低", (unsigned long)e.pulseMs,
                 e.conflict ? "（冲突）" : "");
      else if (e.mode == IO_MODE_HEARTBEAT)
        snprintf(buf, sizeof(buf), "持久心跳 高%lu/低%lums%s",
                 (unsigned long)e.hbHighMs, (unsigned long)e.hbLowMs,
                 e.conflict ? "（冲突）" : "");
      else
        snprintf(buf, sizeof(buf), "%02d:%02d %s%s", e.hour, e.minute,
                 e.action ? "开启(高)" : "关闭(低)", e.conflict ? "（冲突）" : "");
      centralItem(items, "序列器", gr.name, buf, (j == active) && gr.enabled && !overridden);
    }
    // 暂态归因：该引脚此刻由脉冲/心跳驱动（独立成 脉冲器 / 持久心跳器 两条，便于行为区区分）。
    // 只有锁存能挡住暂态；手动切换只是换基准点，脉冲/心跳仍叠加在它之上并会盖过它。
    if (drivable && (kind == IO_MODE_PULSE || kind == IO_MODE_HEARTBEAT)) {
      snprintf(buf, sizeof(buf), "当前暂态电平=%s", gpioGetCurrentLevel(pin) ? "高" : "低");
      centralItem(items, kind == IO_MODE_PULSE ? "脉冲器" : "持久心跳器", gr.name, buf, true);
    }
  }

  // 5) 比较器输出（含该门输入条件）
  for (int g = 0; g < comparatorGateCount; g++) {
    ComparatorGate& gate = comparatorGates[g];
    bool hasOutput = false;
    for (int a = 0; a < gate.actionCount; a++) {
      if (gate.actions[a].pin != pin) continue;
      hasOutput = true;
      bool met = gate.enabled && comparatorGateMet(gate);
      snprintf(buf, sizeof(buf), "输出 GPIO%d：满足→%s，不满足→%s（条件%s）", pin,
               gate.actions[a].actionLevel ? "高电平" : "低电平",
               gate.actions[a].sourceLevel ? "高电平" : "低电平",
               met ? "满足" : "不满足");
      centralItem(items, "比较器", gate.name, buf, met && drivable);
    }
    if (hasOutput) {
      if (!gate.enabled) {
        centralItem(items, "比较器", gate.name, "门已关闭（输出复位源IO电平）", false);
      }
      for (int i = 0; i < gate.inputCount; i++) {
        const ComparatorInput& in = gate.inputs[i];
        char refName[32];
        int lvl;
        if (in.source == COMPARATOR_SRC_EXTERNAL) {
          snprintf(refName, sizeof(refName), "GPIO%d", in.refIndex);
          lvl = gpioGetCurrentLevel(in.refIndex);
        } else if (in.source == COMPARATOR_SRC_IO) {
          if (in.refIndex >= 0 && in.refIndex < ioGroupCount) {
            snprintf(refName, sizeof(refName), "%s", ioGroups[in.refIndex].name);
            lvl = gpioGetCurrentLevel(ioGroups[in.refIndex].pin);
          } else {
            snprintf(refName, sizeof(refName), "无效引用");
            lvl = 0;
          }
        } else if (in.source == COMPARATOR_SRC_COUNTER) {
          if (in.refIndex >= 0 && in.refIndex < counterTaskCount &&
              in.actionIndex >= 0 && in.actionIndex < counterTasks[in.refIndex].actionCount) {
            snprintf(refName, sizeof(refName), "%s·动作%d", counterTasks[in.refIndex].name, in.actionIndex + 1);
            lvl = counterGetActionLevel(in.refIndex, in.actionIndex);
          } else {
            snprintf(refName, sizeof(refName), "无效引用");
            lvl = 0;
          }
        } else {
          snprintf(refName, sizeof(refName), "无效引用");
          lvl = 0;
        }
        snprintf(buf, sizeof(buf), "输入条件：%s 期望%s（当前%s）%s", refName,
                 in.expectedLevel ? "高" : "低", lvl ? "高" : "低",
                 (lvl == in.expectedLevel) ? "✓" : "✗");
        centralItem(items, "比较器", gate.name, buf, false);
      }
    }
  }

  // 6) 比较器外部输入（该引脚作为某门的输入源）
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      const ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source != COMPARATOR_SRC_EXTERNAL || in.refIndex != pin) continue;
      int lvl = gpioGetCurrentLevel(pin);
      snprintf(buf, sizeof(buf), "作为外部输入：期望%s（当前%s）%s",
               in.expectedLevel ? "高" : "低", lvl ? "高" : "低",
               (lvl == in.expectedLevel) ? "✓" : "✗");
      centralItem(items, "比较器", comparatorGates[g].name, buf, false);
    }
  }

  // 7) 计数器输出动作（含触发状态）
  for (int t = 0; t < counterTaskCount; t++) {
    CounterTask& task = counterTasks[t];
    for (int a = 0; a < task.actionCount; a++) {
      if (task.actions[a].pin != pin) continue;
      const CounterAction& act = task.actions[a];
      char holdBuf[24];
      if (act.holdMode == COUNTER_HOLD_AUTO_OFF)
        snprintf(holdBuf, sizeof(holdBuf), "延时%lus关闭", (unsigned long)act.holdSec);
      else
        snprintf(holdBuf, sizeof(holdBuf), "一直保持");
      if (task.enabled)
        snprintf(buf, sizeof(buf), "动作：GPIO%d=%s（%s）", pin,
                 act.level ? "高电平" : "低电平", holdBuf);
      else
        snprintf(buf, sizeof(buf), "动作：GPIO%d=%s（%s）——任务已关闭", pin,
                 act.level ? "高电平" : "低电平", holdBuf);
      centralItem(items, "计数器", task.name, buf, task.enabled && task.fired && drivable);

      if (task.triggerPin >= 0)
        snprintf(buf, sizeof(buf), "触发源 GPIO%d 反转 %u/%d 次", task.triggerPin, task.triggerCount, task.threshold);
      else
        snprintf(buf, sizeof(buf), "未设置触发源");
      centralItem(items, "计数器", task.name, buf, false);
    }
  }

  // 8) 计数器触发源（该引脚作为某任务的触发源）
  for (int t = 0; t < counterTaskCount; t++) {
    if (counterTasks[t].triggerPin != pin) continue;
    CounterTask& task = counterTasks[t];
    snprintf(buf, sizeof(buf), "作为触发源：反转 %u/%d 次，%s",
             task.triggerCount, task.threshold, task.fired ? "已触发" : "未触发");
    centralItem(items, "计数器", task.name, buf, task.enabled && task.fired);
  }
}

// 组装中心面板整体状态 JSON：pins（监控区）/ events（行为区-反转事件）/ pwmRunning（行为区-运行中的PWM）
void centralBuildStatus(JsonDocument& doc) {
  centralRebuildPinSet();

  JsonArray pins = doc.createNestedArray("pins");
  for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; pin++) {
    if (!centralPinInUse[pin]) continue;
    bool isPwm = ioIsPinUsedByPwm(pin);
    JsonObject o = pins.createNestedObject();
    o["pin"] = pin;
    if (isPwm) {
      o["level"] = -1;
      float duty = 0.0f;
      for (int i = 0; i < pwmChannelCount; i++) if (pwmPins[i] == pin) duty = pwmConfigs[i].currentDuty;
      o["duty"] = duty;
    } else {
      o["level"] = digitalRead(pin) ? 1 : 0;
    }
    o["driver"] = centralDriverOf(pin, false);   // 监控区：显示此刻的驱动者，不取边沿成因
    o["pwm"] = isPwm;
    o["flash"] = isIoFlashPin(pin);
    o["latch"] = latchEnabled[pin];
    o["manual"] = manualOverride[pin];
  }

  // 事件：最新在前
  uint32_t total = centralEventCount;
  uint32_t cnt = total < CENTRAL_EVENT_MAX ? total : (uint32_t)CENTRAL_EVENT_MAX;
  JsonArray events = doc.createNestedArray("events");
  for (uint32_t k = 0; k < cnt; k++) {
    uint32_t idx = (total - 1 - k) % CENTRAL_EVENT_MAX;
    CentralEvent& e = centralEvents[idx];
    JsonObject o = events.createNestedObject();
    o["seq"] = e.seq;
    o["pin"] = e.pin;
    o["from"] = e.fromLevel;
    o["to"] = e.toLevel;
    o["reason"] = e.reason;
    o["clock"] = e.clock;
  }

  // 运行中的 PWM（启用且有生效内容：条目渐变区间内，或阳光曲线接管中）
  JsonArray pr = doc.createNestedArray("pwmRunning");
  for (int i = 0; i < pwmChannelCount; i++) {
    if (!pwmConfigs[i].enabled) continue;
    bool sun = pwmConfigs[i].sunEnabled && pwmConfigs[i].sunActive;
    if (pwmConfigs[i].activeTask <= 0 && !sun) continue;
    JsonObject o = pr.createNestedObject();
    o["index"] = i;
    o["pin"] = pwmPins[i];
    o["duty"] = pwmConfigs[i].currentDuty;
    o["task"] = pwmConfigs[i].activeTask;
    o["sun"] = sun;   // true = 本通道由阳光曲线接管（task 恒为 0）
  }
}
