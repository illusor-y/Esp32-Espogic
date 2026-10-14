#include "config.h"

// ===================== 序列器（原 IO 调度 / 定时器） =====================
// 模型：一个"组" = 一个 GPIO 引脚（命名 = 组名）；组内多条"时:分 + 动作"条目。
// 与 PWM 无关，输出为普通 GPIO 电平。条目有三种模式：
//   定时开关(SCHEDULE) ：到点输出高/低并保持 —— 决定该引脚的"基础电平"；
//   瞬时脉冲(PULSE)    ：到点拉高/拉低 pulseMs 毫秒，之后回到基础电平；
//   持久心跳(HEARTBEAT)：无时间维度，序列启用期间永久循环，高 hbHighMs / 低 hbLowMs；在序列内独占。
//
// 两层模型（最终电平优先级：脉冲 > 心跳 > 基础；组关闭恒低）：
//   基础电平层（纯函数，跨重启可推导）：任意时刻 = 该引脚"时间 <= 当前"里最晚那条
//     定时开关条目的动作；当天没有到点的定时开关条目，则跨午夜环回取昨天最后一条。
//     脉冲/心跳条目不参与基础电平，避免污染环回结果。
//   暂态层（用 millis()/墙钟毫秒，重启即清空，不持久化）：脉冲到期时刻与心跳相位。
//
// 暂态电平在"读取时"即时算出（ioLevelAt），而不是只在写引脚时算：比较器 / 计数器 /
// 中心面板 / HTTP 都通过 gpioGetCurrentLevel() 读取，若只在写引脚时算，写入与读取之间
// 会跨过相位边界，出现"面板显示高、实际引脚是低"的错位。
//
// 冲突规则（同序列内条目互相判定，标红的条目不参与执行）：
//   1) 两条"有时间"的条目（定时开关/瞬时脉冲）"时:分"完全相同 → 双向冲突（不分模式）；
//   2) 持久心跳与任何其他条目都不能共存 —— 正常路径由 ioCanSetMode() 在添加/编辑时直接拒绝
//      （设为持久心跳要求序列除此之外为空；序列已有持久心跳时不能再添加/改成其他模式）。
//      ioRecalcConflicts() 里保留的心跳相关标记只作为兜底：NVS 读入 / 全局导入不走 ioCanSetMode，
//      老配置或手工导入仍可能带进"心跳 + 其他条目"的组合，此时标红提示用户清理；
//   3) 同序列多条持久心跳 → 双向冲突（只允许一条；两条都不执行以便用户删除一条）。

IoGroup ioGroups[IO_GROUP_MAX];
int ioGroupCount = 0;

// 输出状态缓存：ioInited 标记是否已 pinMode；ioLastState/ioLastKind 记录上次写入的电平与驱动来源
static bool ioInited[IO_GROUP_MAX];
static int  ioLastState[IO_GROUP_MAX];
static int  ioLastKind[IO_GROUP_MAX];

// 按 GPIO 号索引：该引脚最近两次"由序列器写入"的来源（0=基础 1=脉冲 2=心跳）。
// 供中心面板判定某条边沿的**成因** —— 见 ioGetEdgeCauseKind()。
static uint8_t ioWriteKindNow[SOC_GPIO_PIN_COUNT];
static uint8_t ioWriteKindPrev[SOC_GPIO_PIN_COUNT];

// ===== 暂态运行态（不持久化，重启即清空）=====
// 用扁平索引 IO_TRANSIENT_INDEX(groupIndex, entryIndex)。
// 刻意放在独立数组而不是 IoGroup/IoEntry 内部：组/条目删除是整块 memmove，
// 状态若在结构体内会跟着搬移，导致新条目"继承"旧脉冲 deadline（counter.ino 手工搬
// autoOffUntil 就是在补这个坑）。独立数组的收益是结构变化时整体清零即可。
#define IO_TRANSIENT_INDEX(g, e) ((g) * IO_ENTRY_MAX + (e))

static uint32_t ioPulseUntilMs[IO_GROUP_MAX * IO_ENTRY_MAX];   // 脉冲到期时刻（millis），0=无
static uint32_t ioPulseFiredKey[IO_GROUP_MAX * IO_ENTRY_MAX];  // 脉冲去重键 = dayIdx*1440 + 当天分钟
static uint16_t ioLastMinOfDay[IO_GROUP_MAX];                  // 上次扫描到的当天分钟，0xFFFF=未播种

// 结构变化后强制下一轮全量重算输出，并清空暂态运行态。
// 被 ioAdd/Remove/UpdateEntry、ioAdd/Remove/UpdateGroup、manualClearPin、latchSet/latchClear
// 以及 handleIoToggle / releasePinsForReset 调用 —— 这些路径因此自动获得暂态清理。
void ioResetOutput() {
  for (int i = 0; i < IO_GROUP_MAX * IO_ENTRY_MAX; i++) {
    ioPulseUntilMs[i] = 0;
    ioPulseFiredKey[i] = 0;
  }
  for (int g = 0; g < IO_GROUP_MAX; g++) {
    ioInited[g] = false;
    ioLastKind[g] = IO_MODE_SCHEDULE;
    ioLastMinOfDay[g] = 0xFFFF;   // 下次扫描以当前时刻重新播种，避免补发过期脉冲
  }
}

// 模式白名单归一：非法值一律退回定时开关（避免脏配置意外开启暂态模式）
int ioNormalizeMode(int mode) {
  return (mode == IO_MODE_PULSE || mode == IO_MODE_HEARTBEAT) ? mode : IO_MODE_SCHEDULE;
}

// 时长夹紧到 [IO_DUR_MIN_MS, IO_DUR_MAX_MS]
uint32_t ioClampDurationMs(uint32_t ms) {
  if (ms < IO_DUR_MIN_MS) return IO_DUR_MIN_MS;
  if (ms > IO_DUR_MAX_MS) return IO_DUR_MAX_MS;
  return ms;
}

// 组名规范化：去首尾空格、截断到 IO_NAME_MAX-1，空则用"未命名"
static void ioSetName(IoGroup& grp, const char* name) {
  String n = name ? name : "";
  n.trim();
  if (n.length() > IO_NAME_MAX - 1) n = n.substring(0, IO_NAME_MAX - 1);
  if (n.length() == 0) n = "未命名";
  strncpy(grp.name, n.c_str(), IO_NAME_MAX - 1);
  grp.name[IO_NAME_MAX - 1] = '\0';
}

// 重算组内冲突（规则见文件头注释）。心跳相关的两条是兜底：正常路径已被 ioCanSetMode() 拦住。
// 注意第 2 条是单向标记：只标被心跳压制的定时开关条目，让心跳本身继续运行。
void ioRecalcConflicts() {
  for (int g = 0; g < ioGroupCount; g++) {
    IoGroup& grp = ioGroups[g];
    for (int i = 0; i < grp.entryCount; i++) grp.entries[i].conflict = false;
    for (int i = 0; i < grp.entryCount; i++) {
      for (int j = i + 1; j < grp.entryCount; j++) {
        const IoEntry& a = grp.entries[i];
        const IoEntry& b = grp.entries[j];
        bool aHb = (a.mode == IO_MODE_HEARTBEAT);
        bool bHb = (b.mode == IO_MODE_HEARTBEAT);

        if (!aHb && !bHb) {
          // 规则1：都不是心跳 → 同时分即双向冲突（含脉冲之间、脉冲与开关）
          if (a.hour == b.hour && a.minute == b.minute) {
            grp.entries[i].conflict = true;
            grp.entries[j].conflict = true;
          }
        } else if (aHb && bHb) {
          // 规则3：同序列多条心跳 → 双向冲突（两条都不执行，提示用户删掉一条）
          grp.entries[i].conflict = true;
          grp.entries[j].conflict = true;
        } else {
          // 规则2：心跳 vs 定时开关 → 只标定时开关那一条；心跳照常运行。
          // 心跳 vs 瞬时脉冲 → 不标（脉冲优先级更高，仍能短暂覆盖）
          if (aHb && b.mode == IO_MODE_SCHEDULE) grp.entries[j].conflict = true;
          else if (bHb && a.mode == IO_MODE_SCHEDULE) grp.entries[i].conflict = true;
        }
      }
    }
  }
}

// 计算某组当前的"基础电平"（0=低/关, 1=高/开）：只认定时开关条目，脉冲/心跳不参与。
// 这是纯函数，跨重启可推导；跨午夜环回也只取定时开关条目，避免被昨天的脉冲污染。
int ioComputeState(const IoGroup& grp, int hour, int minute) {
  int now = hour * 60 + minute;
  bool anyValid = false;
  int  latestMin = -1, latestAction = 0;   // 全天最晚（用于环回）
  bool foundToday = false;
  int  bestMin = -1, bestAction = 0;

  for (int i = 0; i < grp.entryCount; i++) {
    const IoEntry& e = grp.entries[i];
    if (e.mode != IO_MODE_SCHEDULE) continue;   // 暂态条目不提供基础电平
    if (e.conflict) continue;                   // 冲突条目不参与执行
    int m = (int)e.hour * 60 + (int)e.minute;
    if (!anyValid || m > latestMin) { latestMin = m; latestAction = e.action ? 1 : 0; }
    anyValid = true;
    if (m <= now && (!foundToday || m > bestMin)) {
      bestMin = m; bestAction = e.action ? 1 : 0; foundToday = true;
    }
  }

  if (!anyValid) return 0;                  // 无非冲突的定时开关条目 → 低电平
  if (foundToday) return bestAction;        // 当天已有到点任务 → 取最晚一条
  return latestAction;                      // 尚未到点 → 取昨天最后一条（环回）
}

// 当天序号，仅供脉冲去重键使用（需跨午夜变化）。优先用 NTP 纪元（本地时区日界）；
// 未同步时退回开机毫秒推算（降级模式，日界可能不精确，但同一天内仍能阻止重复触发）。
static uint32_t ioDayIndex() {
  if (lastSyncEpoch != 0) return timeLocalDayOf(lastSyncEpoch);
  return millis() / 86400000UL;
}

// 某组此刻应有电平(0/1)。kind 出参（可为 nullptr）：0=基础 1=脉冲 2=心跳。
// 优先级：脉冲 > 心跳 > 基础；组关闭恒低。是 (基础电平, 触发时刻, millis()) 的纯函数，
// 因此输出层 / 比较器 / 计数器 / 中心面板 读到的值必然一致。
int ioLevelAt(int groupIndex, int hour, int minute, int* kind) {
  if (kind) *kind = IO_MODE_SCHEDULE;
  if (groupIndex < 0 || groupIndex >= ioGroupCount) return 0;
  const IoGroup& grp = ioGroups[groupIndex];

  // 基础电平：被手动切换过的引脚，手动值就是"基准点"（与组是否启用无关 —— 手动切换是用户
  // 显式操作，不该被组开关重置）。脉冲/心跳叠加在这个基准之上、结束之后回到它，
  // 这就是"手动切换 = 更换基准点"的含义：把低切成高，脉冲拉低 500ms 后必须回到高。
  int base;
  if (manualIsOverridden(grp.pin)) {
    base = manualLevel[grp.pin] ? 1 : 0;
  } else {
    if (!grp.enabled) return 0;               // 组关闭 → 恒低，且不产生暂态
    base = ioComputeState(grp, hour, minute);
  }
  if (!grp.enabled) return base;              // 组关闭：保留手动基准，但脉冲/心跳都不跑

  // 1) 脉冲（优先级最高）：未过期的脉冲以 action 覆盖基础电平，多条取到期最晚者
  unsigned long nowMs = millis();
  uint32_t pulseBest = 0;
  int pulseLevel = 0;
  for (int i = 0; i < grp.entryCount; i++) {
    const IoEntry& e = grp.entries[i];
    if (e.mode != IO_MODE_PULSE || e.conflict) continue;
    uint32_t until = ioPulseUntilMs[IO_TRANSIENT_INDEX(groupIndex, i)];
    if (until == 0) continue;
    if ((int32_t)(nowMs - until) >= 0) continue;   // 已过期
    if (until >= pulseBest) { pulseBest = until; pulseLevel = e.action ? 1 : 0; }
  }
  if (pulseBest != 0) { if (kind) *kind = IO_MODE_PULSE; return pulseLevel; }

  // 2) 心跳：无时间维度，序列启用期间永久循环。相位直接取 millis() —— 它单调、连续，
  //    1 个计数就是真实 1 毫秒，这是"高 x / 低 y"精确的前提。
  //    ⚠ 绝不能用"墙钟的秒 × 1000 + millis()%1000"拼出毫秒：墙钟秒的进位时刻与
  //    millis()%1000 的回零时刻并不同相，两者相加每秒会产生一次约 ±1 秒的锯齿跳变。
  //    period 越接近 1000 的整数倍、相位被踢偏得越少（所以 x==y 时看起来正常），
  //    一旦 x≠y（如 500/1000，period=1500）相位就被打乱，高低时长完全不对。
  //    正常配置下同序列最多一条非冲突心跳。
  for (int i = 0; i < grp.entryCount; i++) {
    const IoEntry& e = grp.entries[i];
    if (e.mode != IO_MODE_HEARTBEAT || e.conflict) continue;
    uint32_t period = e.hbHighMs + e.hbLowMs;
    if (period == 0) continue;
    if (kind) *kind = IO_MODE_HEARTBEAT;
    return (millis() % period) < e.hbHighMs ? 1 : 0;
  }

  return base;
}

// 某引脚当前是否处于暂态：0=非暂态 1=脉冲 2=心跳（供中心面板 / HTTP 归因使用）
int ioGetTransientKind(int pin) {
  for (int g = 0; g < ioGroupCount; g++) {
    if (ioGroups[g].pin != pin) continue;
    int kind = IO_MODE_SCHEDULE;
    ioLevelAt(g, currentTime.hour, currentTime.minute, &kind);
    return kind;
  }
  return IO_MODE_SCHEDULE;
}

// 某引脚"本次电平变化"的成因来源（0=基础 1=脉冲 2=心跳），仅供中心面板的边沿归因使用。
// 与 ioGetTransientKind() 的区别：这里优先取"变化前那个电平"的来源。
// 原因：脉冲到期时 updateIoOutput 是把引脚当基础电平写回去的（ioWriteKindNow 已变成 SCHEDULE），
// 但那条下降沿的成因其实是脉冲；若按采样时的当前状态判定就会被误标成"序列器"。
// 判断"当前是否暂态"（心跳边沿过滤、详情面板的当前电平）仍须用 ioGetTransientKind()。
int ioGetEdgeCauseKind(int pin) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return IO_MODE_SCHEDULE;
  return (ioWriteKindPrev[pin] != IO_MODE_SCHEDULE) ? ioWriteKindPrev[pin] : ioWriteKindNow[pin];
}

// 扫描脉冲触发：分钟推进时，对本组该分钟内所有脉冲条目点火一次。
// 去重键用 (当天序号, 当天分钟)：NTP 校时会改变纪元，若用 epoch 分钟会被误判为新分钟而重复触发。
static void ioScanPulseTriggers() {
  int nowMin = currentTime.hour * 60 + currentTime.minute;
  uint32_t day = ioDayIndex();
  for (int g = 0; g < ioGroupCount; g++) {
    IoGroup& grp = ioGroups[g];
    if (ioLastMinOfDay[g] == 0xFFFF) {        // 首次播种：只记游标，不触发（过期脉冲不补发）
      ioLastMinOfDay[g] = (uint16_t)nowMin;
      continue;
    }
    if (ioLastMinOfDay[g] == nowMin) continue;
    int delta = (nowMin - (int)ioLastMinOfDay[g] + 1440) % 1440;
    if (delta == 0 || delta > 5) {
      // 时间回跳或大幅前跳：只推进游标，不补发，避免吐出一串过期脉冲
      if (delta > 5) logf("[序列器] %s 时间跳变 %d 分钟，跳过脉冲补发\n", grp.name, delta);
      delta = 0;
    }
    // 被锁存/已关闭的组不处理。手动切换不算"锁"，所以不阻止脉冲点火与定时开关交还基准。
    bool canRun = grp.enabled && !latchIsEnabled(grp.pin);
    if (canRun) {
      for (int k = 1; k <= delta; k++) {
        int tMin = ((int)ioLastMinOfDay[g] + k) % 1440;
        for (int i = 0; i < grp.entryCount; i++) {
          const IoEntry& e = grp.entries[i];
          if (e.conflict) continue;
          if ((int)e.hour * 60 + (int)e.minute != tMin) continue;

          if (e.mode == IO_MODE_PULSE) {
            uint32_t key = day * 1440UL + (uint32_t)tMin;
            int fi = IO_TRANSIENT_INDEX(g, i);
            if (ioPulseFiredKey[fi] == key) continue;   // 本分钟已触发过
            ioPulseFiredKey[fi] = key;
            ioPulseUntilMs[fi] = millis() + e.pulseMs;
            logf("[序列器] %s 脉冲触发：GPIO%d -> %s %lums\n", grp.name, grp.pin,
                 e.action ? "高电平" : "低电平", (unsigned long)e.pulseMs);
          } else if (e.mode == IO_MODE_SCHEDULE) {
            // 定时开关到点：把"手动基准"交还给调度。手动切换不是锁 —— 序列器可以覆盖它，
            // 这样"最后一次设定基准的操作"才真正生效；否则手动值会永久压住该引脚的定时开关。
            if (manualIsOverridden(grp.pin)) {
              // 只清标志，不调 manualClearPin() —— 后者会 ioResetOutput() 一并清掉所有组的暂态
              // 和"已扫描到哪一分钟"的游标，正在进行的本轮分钟扫描会被破坏。
              manualOverride[grp.pin] = false;
              manualLevel[grp.pin] = 0;
              ioInited[g] = false;              // 强制该组下一轮按调度重算
              requestIoConfigSave();   // 基准交还也要落盘，否则重启后旧的手动基准会回来
              logf("[序列器] %s 定时开关到点，交还手动设定的基准：GPIO%d\n", grp.name, grp.pin);
            }
          }
        }
      }
    }
    ioLastMinOfDay[g] = (uint16_t)nowMin;
  }
}

// loop 调用：按当前时间输出各引脚，仅状态变化时写。
// 日志策略：只记"基础电平"的变化、持久心跳的启动（一条）、以及暂态结束回到基础电平那一刻。
// 暂态期间的逐次翻转一律不打印 —— 心跳是毫秒级持续翻转，逐条或按秒聚合都会长期刷屏，
// 且持续翻转本身没有信息量；脉冲另有"脉冲触发"一次性日志。
void updateIoOutput() {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：ioGroups 会被 handler 改（增删组/条目会整段前移数组）
  int hour = currentTime.hour, minute = currentTime.minute;
  ioScanPulseTriggers();                        // 先点火，再输出（同一轮内即可读到新脉冲）
  for (int g = 0; g < ioGroupCount; g++) {
    IoGroup& grp = ioGroups[g];
    if (latchIsEnabled(grp.pin)) continue;   // 被锁存，最高权限，跳过自动调度
    // 被手动切换的引脚不再跳过：手动值只是"基准点"，脉冲/心跳仍要叠加上去（见 ioLevelAt）
    int kind = IO_MODE_SCHEDULE;
    int state = ioLevelAt(g, hour, minute, &kind);
    if (!ioInited[g] || ioLastState[g] != state) {
      if (kind == IO_MODE_SCHEDULE) {
        if (ioLastKind[g] != IO_MODE_SCHEDULE)
          logf("[序列器] %s 暂态结束：GPIO%d 回到 %s\n", grp.name, grp.pin,
               state ? "高电平" : "低电平");
        else
          logf("[序列器] 输出 GPIO%d -> %s\n", grp.pin, state ? "高电平(开)" : "低电平(关)");
      } else if (kind == IO_MODE_HEARTBEAT && kind != ioLastKind[g]) {
        // 持久心跳启动：只打这一条。之后毫秒级的持续翻转一律不打印，否则会长期刷屏。
        // 脉冲不需要这一条，它在点火处已有"脉冲触发"日志。
        logf("[序列器] %s 持久心跳启动：GPIO%d\n", grp.name, grp.pin);
      }
      // 记录这次写入的来源与"上一次写入的来源"，供中心面板判定边沿成因（见 ioGetEdgeCauseKind）。
      // 首次写入没有上一次，两侧都记成当前 kind。ioInited 必须在此判断，因为下面就会置 true。
      ioWriteKindPrev[grp.pin] = ioInited[g] ? ioWriteKindNow[grp.pin] : (uint8_t)kind;
      ioWriteKindNow[grp.pin] = (uint8_t)kind;
      pinMode(grp.pin, OUTPUT);
      digitalWrite(grp.pin, state ? HIGH : LOW);
      ioInited[g] = true;
      ioLastState[g] = state;
      ioLastKind[g] = kind;
    }
  }
}

// 芯片禁用引脚表已抽离至 pin_bus_map.h（isIoFlashPin 现由该头文件提供，全局引用）。

bool ioIsPinUsedByPwm(int pin) {
  for (int i = 0; i < pwmChannelCount; i++) if (pwmPins[i] == pin) return true;
  return false;
}

bool ioIsPinUsedByIo(int pin, int excludeGroup) {
  for (int g = 0; g < ioGroupCount; g++) {
    if (g == excludeGroup) continue;
    if (ioGroups[g].pin == pin) return true;
  }
  return false;
}

bool ioAddGroup(const char* name, int pin, bool devMode, bool allowPwm, bool allowDup) {
  stateAssertHeld();   // 新组插到最前、整段后移：必须在锁内，否则 loop 会读到正在搬移的数组
  if (ioGroupCount >= IO_GROUP_MAX) { logf("[序列器] 添加失败：组数已达上限 %d\n", IO_GROUP_MAX); return false; }
  if (!isPinAvailable(pin)) { logf("[序列器] 添加失败：GPIO%d 不在可用列表\n", pin); return false; }
  if (!devMode && isIoFlashPin(pin)) { logf("[序列器] 添加失败：GPIO%d 为 Flash/PSRAM 总线，物理上不可用作 IO 输出\n", pin); return false; }
  if (!allowPwm && ioIsPinUsedByPwm(pin)) { logf("[序列器] 添加失败：GPIO%d 被 PWM 占用\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByIo(pin, -1)) { logf("[序列器] 添加失败：GPIO%d 被其他序列器占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByOutput(pin, -1)) { logf("[序列器] 添加失败：GPIO%d 被比较器输出占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByExternalInput(pin, -1)) { logf("[序列器] 添加失败：GPIO%d 被比较器外部输入占用\n", pin); return false; }
  if (!allowDup && counterIsPinUsedByAction(pin, -1)) { logf("[序列器] 添加失败：GPIO%d 被计数器动作占用\n", pin); return false; }
  // 新组插入到最前面：新的在上，旧的顺延（编辑更新时位置不变）
  for (int g = ioGroupCount; g > 0; g--) ioGroups[g] = ioGroups[g - 1];
  IoGroup& grp = ioGroups[0];
  ioSetName(grp, name);
  grp.pin = pin;
  grp.enabled = true;
  grp.entryCount = 0;
  ioGroupCount++;
  comparatorOnIoGroupInserted();   // 同步后移比较器内部输入引用
  ioResetOutput();
  logf("[序列器] 添加成功：%s (GPIO%d)，当前共 %d 组\n", grp.name, pin, ioGroupCount);
  return true;
}

bool ioRemoveGroup(int index) {
  stateAssertHeld();   // 整段前移 + 释放引脚：必须在锁内
  if (index < 0 || index >= ioGroupCount) return false;
  // 释放引脚：置为高阻输入，避免悬浮电平。被锁存的引脚跳过——锁存是引脚级属性，
  // 删序列不该让固定电平掉电（连续性保证，见 PIN-4）
  if (latchIsEnabled(ioGroups[index].pin)) {
    logf("[序列器] GPIO%d 已锁存，保留其电平不释放\n", ioGroups[index].pin);
  } else {
    pinMode(ioGroups[index].pin, INPUT);
    digitalWrite(ioGroups[index].pin, LOW);
  }
  for (int g = index; g < ioGroupCount - 1; g++) ioGroups[g] = ioGroups[g + 1];
  ioGroupCount--;
  comparatorOnIoGroupRemoved(index);   // 同步修正比较器内部引用
  ioResetOutput();
  return true;
}

bool ioUpdateGroup(int index, const char* name, int pin, bool devMode, bool allowPwm, bool allowDup) {
  if (index < 0 || index >= ioGroupCount) return false;
  if (!isPinAvailable(pin)) return false;
  if (!devMode && isIoFlashPin(pin)) { logf("[序列器] 编辑失败：GPIO%d 为 Flash/PSRAM 总线\n", pin); return false; }
  if (!allowPwm && ioIsPinUsedByPwm(pin)) { logf("[序列器] 编辑失败：GPIO%d 被 PWM 占用\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByIo(pin, index)) { logf("[序列器] 编辑失败：GPIO%d 被其他序列器占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByOutput(pin, -1)) { logf("[序列器] 编辑失败：GPIO%d 被比较器输出占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByExternalInput(pin, -1)) { logf("[序列器] 编辑失败：GPIO%d 被比较器外部输入占用\n", pin); return false; }
  if (!allowDup && counterIsPinUsedByAction(pin, -1)) { logf("[序列器] 编辑失败：GPIO%d 被计数器动作占用\n", pin); return false; }
  // 引脚变化：释放旧引脚（被锁存的同上，保留电平）
  if (ioGroups[index].pin != pin) {
    if (latchIsEnabled(ioGroups[index].pin)) {
      logf("[序列器] GPIO%d 已锁存，保留其电平不释放\n", ioGroups[index].pin);
    } else {
      pinMode(ioGroups[index].pin, INPUT);
      digitalWrite(ioGroups[index].pin, LOW);
    }
  }
  ioSetName(ioGroups[index], name);
  ioGroups[index].pin = pin;
  ioResetOutput();
  return true;
}

// 该序列是否允许把第 entryIndex 条（-1 = 追加新条目）设为 mode：持久心跳在序列内独占。
//   要设为持久心跳 → 除"正在改的这一条"外序列必须为空（多条心跳也无意义）；
//   要设为其他模式  → 序列里不能已有持久心跳。
// 理由：持久心跳无时间维度且优先级恒高于基础电平，同序列的定时开关/瞬时脉冲永远不会生效，
// 与其让用户配出一个"配了但不工作"的序列，不如在添加/编辑时就拦掉。
// 注：ioRecalcConflicts() 里仍保留了心跳与他人共存的冲突标记，作为 NVS 读入 / 全局导入
//     绕过本函数时的兜底（那些路径不经过 ioAddEntry/ioUpdateEntry）。
bool ioCanSetMode(int groupIndex, int entryIndex, int mode) {
  if (groupIndex < 0 || groupIndex >= ioGroupCount) return false;
  const IoGroup& grp = ioGroups[groupIndex];
  if (ioNormalizeMode(mode) == IO_MODE_HEARTBEAT) {
    for (int i = 0; i < grp.entryCount; i++) if (i != entryIndex) return false;
    return true;
  }
  for (int i = 0; i < grp.entryCount; i++) {
    if (i == entryIndex) continue;
    if (grp.entries[i].mode == IO_MODE_HEARTBEAT) return false;
  }
  return true;
}

bool ioAddEntry(int groupIndex, int hour, int minute, bool action, int mode,
                uint32_t pulseMs, uint32_t hbHighMs, uint32_t hbLowMs) {
  stateAssertHeld();   // 条目数组会被 loop 每轮读取：必须在锁内
  if (groupIndex < 0 || groupIndex >= ioGroupCount) return false;
  IoGroup& grp = ioGroups[groupIndex];
  if (grp.entryCount >= IO_ENTRY_MAX) return false;
  if (!ioCanSetMode(groupIndex, -1, mode)) {
    logf("[序列器] 添加失败：%s\n",
         ioNormalizeMode(mode) == IO_MODE_HEARTBEAT ? "持久心跳必须独占序列"
                                                    : "该序列已有持久心跳，不能再添加条目");
    return false;
  }
  IoEntry& e = grp.entries[grp.entryCount++];
  e.mode = (uint8_t)ioNormalizeMode(mode);
  // 持久心跳无时间维度：统一存 00:00，避免把上一个模式的残留时间写进配置
  bool isHb = (e.mode == IO_MODE_HEARTBEAT);
  e.hour = isHb ? 0 : (uint8_t)safeConstrain(hour, 0, 23);
  e.minute = isHb ? 0 : (uint8_t)safeConstrain(minute, 0, 59);
  e.action = action ? 1 : 0;
  e.pulseMs = ioClampDurationMs(pulseMs);
  e.hbHighMs = ioClampDurationMs(hbHighMs);
  e.hbLowMs = ioClampDurationMs(hbLowMs);
  e.conflict = false;
  ioRecalcConflicts();
  ioResetOutput();
  return true;
}

bool ioRemoveEntry(int groupIndex, int entryIndex) {
  stateAssertHeld();   // 条目前移 + 重算冲突：必须在锁内
  if (groupIndex < 0 || groupIndex >= ioGroupCount) return false;
  IoGroup& grp = ioGroups[groupIndex];
  if (entryIndex < 0 || entryIndex >= grp.entryCount) return false;
  for (int j = entryIndex; j < grp.entryCount - 1; j++) grp.entries[j] = grp.entries[j + 1];
  grp.entryCount--;
  ioRecalcConflicts();
  ioResetOutput();
  return true;
}

bool ioUpdateEntry(int groupIndex, int entryIndex, int hour, int minute, bool action, int mode,
                   uint32_t pulseMs, uint32_t hbHighMs, uint32_t hbLowMs) {
  if (groupIndex < 0 || groupIndex >= ioGroupCount) return false;
  IoGroup& grp = ioGroups[groupIndex];
  if (entryIndex < 0 || entryIndex >= grp.entryCount) return false;
  IoEntry& e = grp.entries[entryIndex];
  if (!ioCanSetMode(groupIndex, entryIndex, mode)) {
    logf("[序列器] 编辑失败：%s\n",
         ioNormalizeMode(mode) == IO_MODE_HEARTBEAT ? "持久心跳必须独占序列"
                                                    : "该序列已有持久心跳，不能再改为其他模式");
    return false;
  }
  e.mode = (uint8_t)ioNormalizeMode(mode);
  // 持久心跳无时间维度：统一存 00:00（同 ioAddEntry）
  bool isHb = (e.mode == IO_MODE_HEARTBEAT);
  e.hour = isHb ? 0 : (uint8_t)safeConstrain(hour, 0, 23);
  e.minute = isHb ? 0 : (uint8_t)safeConstrain(minute, 0, 59);
  e.action = action ? 1 : 0;
  e.pulseMs = ioClampDurationMs(pulseMs);
  e.hbHighMs = ioClampDurationMs(hbHighMs);
  e.hbLowMs = ioClampDurationMs(hbLowMs);
  ioRecalcConflicts();
  ioResetOutput();
  return true;
}

// ===== 手动切换（调度面板：更换该引脚的"基准电平"，不是锁；锁存才是绝对锁）=====

bool manualIsOverridden(int pin) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return false;
  return manualOverride[pin];
}

// 返回某引脚当前实际电平（0=低/关, 1=高/开）：锁存 > 序列器(脉冲/心跳 > 基准) > 手动值 > 实际引脚读值
// 不再对手动切换做短路：手动值是"基准点"，脉冲/心跳仍可能盖在它上面，必须由 ioLevelAt 统一算。
int gpioGetCurrentLevel(int pin) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return 0;
  if (latchEnabled[pin]) return latchLevel[pin];
  for (int g = 0; g < ioGroupCount; g++) {
    if (ioGroups[g].pin == pin) {
      return ioLevelAt(g, currentTime.hour, currentTime.minute, nullptr);
    }
  }
  if (manualOverride[pin]) return manualLevel[pin];   // 无序列器组的引脚：手动值直接生效
  return digitalRead(pin) ? 1 : 0;
}

// 只读输入引脚：比较器外部输入 + 未被任何输出模块占用的计数器触发源。
// 这类引脚必须保持输入 —— 一旦被手动切换/锁存接管，pinMode 变输出（对外部器件对驱），
// 且 gpioGetCurrentLevel() 永远返回手动/锁存值，外部真实电平再也读不到。
bool pinIsReadOnlyInput(int pin) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return false;
  if (comparatorIsPinUsedByExternalInput(pin, -1)) return true;
  return counterIsPinUsedAsReadOnlyTrigger(pin);
}

// 找到占用该引脚的 PWM 通道，无则 -1
static int pwmChannelOfPin(int pin) {
  for (int i = 0; i < pwmChannelCount; i++) if (pwmPins[i] == pin) return i;
  return -1;
}

// 设定某引脚的"基准电平"并立即输出。基准会一直保留，直到：
//   manualSetLevel/manualClearPin/manualClearAll 或定时开关到点交还、或重置流程清除。
// 会跨重启保持（写入 ioconfig 的 manual 段），但**总线引脚除外** —— 它们不写入 NVS，
// 重启后失效；需要留档请用「包含开发引脚的配置导出」。
// 它不阻止其他模块：脉冲/心跳叠加在其上，比较器/计数器输出也能覆盖它；锁存才能绝对固定。
// devMode：放开 PSRAM/Flash 总线引脚的禁用（用于芯片识别错误等场景）；
// allowPwm：放开 PWM 通道引脚的禁用（切换会暂停该通道 PWM 输出）。
bool manualSetLevel(int pin, int level, bool devMode, bool allowPwm) {
  stateAssertHeld();   // manualOverride/manualLevel 会被 loop 每轮读：必须在锁内
  if (!isPinAvailable(pin)) return false;
  if (pinIsReadOnlyInput(pin)) {
    logf("[手动] GPIO%d 为只读输入引脚（比较器外部输入/计数器触发源），禁止手动切换\n", pin);
    return false;
  }
  if (latchEnabled[pin]) {
    logf("[手动] GPIO%d 已锁存，禁止手动切换（请先解除锁存）\n", pin);
    return false;
  }
  if (!devMode && isIoFlashPin(pin)) {
    logf("[手动] GPIO%d 为 PSRAM/Flash 总线，非开发模式禁止覆盖\n", pin);
    return false;
  }
  int ch = pwmChannelOfPin(pin);
  if (!allowPwm && ch >= 0) {
    logf("[手动] GPIO%d 为 PWM 通道，未开启 PWM 开关，禁止覆盖\n", pin);
    return false;
  }
  if (ch >= 0) pwmReleaseForOverride(ch);  // 允许 PWM 时覆盖：让出 LEDC
  manualOverride[pin] = true;
  manualLevel[pin] = level ? 1 : 0;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, manualLevel[pin] ? HIGH : LOW);
  // 基准变了，让序列器下一轮重算这个引脚（只重置该组：不动其他组，也不清脉冲状态）
  for (int g = 0; g < ioGroupCount; g++) if (ioGroups[g].pin == pin) ioInited[g] = false;
  requestIoConfigSave();   // 手动基准随 ioconfig 入库（总线引脚会被 saveIoConfig 过滤掉）
  logf("[手动] 设定基准 GPIO%d -> %s\n", pin, manualLevel[pin] ? "高电平" : "低电平");
  return true;
}

// 取消某引脚的手动基准
bool manualClearPin(int pin) {
  stateAssertHeld();   // manualOverride/manualLevel 会被 loop 每轮读：必须在锁内
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return false;
  if (!manualOverride[pin]) return true;
  manualOverride[pin] = false;
  int ch = pwmChannelOfPin(pin);
  if (ch >= 0) {
    pwmRestoreChannel(ch);                 // 重新挂载 PWM
  } else {
    pinMode(pin, INPUT);                   // 交还 序列器/比较器/输入
  }
  ioResetOutput();                         // 强制 序列器 下一轮按调度重算
  requestIoConfigSave();                   // 同步落盘（manualClearAll 逐引脚调本函数，无需另发请求）
  logf("[手动] 取消基准 GPIO%d\n", pin);
  return true;
}

// 取消全部手动基准
void manualClearAll() {
  for (int i = 0; i < getAvailablePinCount(); i++) {
    int pin = getAvailablePin(i);
    if (manualOverride[pin]) manualClearPin(pin);
  }
  logRaw("[手动] 已清除全部手动基准\n");
}

// ===== 锁存（绝对锁，覆盖手动切换 / PWM / 序列器 / 比较器 / 计数器）=====

bool latchIsEnabled(int pin) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return false;
  return latchEnabled[pin];
}

// 锁存某引脚电平并立即输出；持续生效直到 latchClear/latchClearAll 或重启。
// devMode：放开 Flash/PSRAM 总线引脚；allowPwm：放开 PWM 通道引脚。
bool latchSet(int pin, int level, bool devMode, bool allowPwm) {
  stateAssertHeld();   // latchEnabled/latchLevel 会被 loop 每轮读：必须在锁内
  if (!isPinAvailable(pin)) return false;
  if (pinIsReadOnlyInput(pin)) {
    logf("[锁存] GPIO%d 为只读输入引脚（比较器外部输入/计数器触发源），禁止锁存\n", pin);
    return false;
  }
  if (!devMode && isIoFlashPin(pin)) {
    logf("[锁存] GPIO%d 为 PSRAM/Flash 总线，非开发模式禁止锁存\n", pin);
    return false;
  }
  int ch = pwmChannelOfPin(pin);
  if (!allowPwm && ch >= 0) {
    logf("[锁存] GPIO%d 为 PWM 通道，未开启 PWM 开关，禁止锁存\n", pin);
    return false;
  }
  if (ch >= 0) pwmReleaseForOverride(ch);  // 允许 PWM 时锁存：让出 LEDC
  latchEnabled[pin] = true;
  latchLevel[pin] = level ? 1 : 0;
  manualOverride[pin] = false;             // 锁存优先于手动基准
  manualLevel[pin] = 0;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, latchLevel[pin] ? HIGH : LOW);
  ioResetOutput();                         // 强制 序列器 下一轮按锁存跳过
  logf("[锁存] 锁存 GPIO%d -> %s（最高权限）\n", pin, latchLevel[pin] ? "高电平" : "低电平");
  return true;
}

// 解除某引脚锁存
bool latchClear(int pin) {
  stateAssertHeld();   // latchEnabled/latchLevel 会被 loop 每轮读：必须在锁内
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return false;
  if (!latchEnabled[pin]) return true;
  latchEnabled[pin] = false;
  int ch = pwmChannelOfPin(pin);
  if (ch >= 0) {
    pwmRestoreChannel(ch);                 // 重新挂载 PWM
  } else {
    pinMode(pin, INPUT);                   // 交还 序列器/比较器/计数器/输入
  }
  ioResetOutput();                         // 强制 序列器 下一轮按调度重算
  logf("[锁存] 解除 GPIO%d 锁存\n", pin);
  return true;
}

// 解除全部锁存
void latchClearAll() {
  for (int i = 0; i < getAvailablePinCount(); i++) {
    int pin = getAvailablePin(i);
    if (latchEnabled[pin]) latchClear(pin);
  }
  logRaw("[锁存] 已解除全部锁存\n");
}

// loop 调用：锁存「自持」——每轮把锁存脚重新驱动到锁存电平（幂等）。
// 为什么需要：全项目对锁存脚只有"各模块跳过写"，没有任何一处持续驱动；一旦某条释放路径
// （删序列/任务/门、PWM 改绑）把引脚 pinMode(INPUT)，锁存就名存实亡，而面板仍显示"锁存高"。
// 有了它，锁存成为"引脚自己的属性"：配置怎么改，被固定的电平都不掉（带电维修的连续性保证）。
void updateLatchHold() {
  StateLockGuard _lg;   // latchEnabled/latchLevel 会被 HTTP 改（递归锁，可安全嵌套）
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
    if (!latchEnabled[p]) continue;
    pinMode(p, OUTPUT);
    digitalWrite(p, latchLevel[p] ? HIGH : LOW);
  }
}
