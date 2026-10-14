#include "config.h"

// ===================== 计数器 =====================
// 模型：一个计数器任务 = 1 个触发源引脚（监控其电平切变）+ 1 个阈值 X + 最多 10 个输出动作。
//   触发源电平每反转一次（高->低 或 低->高）计 1 次，达到阈值 X 的整数倍触发动作。
//   动作保持模式：一直保持（触发后永久输出 level）或 延时关闭（输出 level 保持 holdSec 秒后复位低）。
//
// 全局切变计量：ioToggleCount[pin] 持续累计（只增），供「计量面板」展示，不参与任务触发清零。
// 任务触发计数：task.triggerCount 持续累计（不清零），每达到阈值整数倍触发一次，多个任务共享同一触发源互不干扰。
//
// 引脚互斥：
//   触发源（读）—— 仅排除 PWM，不排除 序列器/比较器占用（读可共享）；
//   动作（写）—— 全局唯一写者，与 PWM / 序列器 / 比较器输出与外部输入 / 其他计数器动作互斥。

CounterTask counterTasks[COUNTER_TASK_MAX];
int counterTaskCount = 0;

uint32_t ioToggleCount[SOC_GPIO_PIN_COUNT];   // 全局切变累计计数（计量面板）
uint8_t  ioLastLevel[SOC_GPIO_PIN_COUNT];     // 边沿检测快照
static bool ioLevelInit[SOC_GPIO_PIN_COUNT];  // 快照是否已初始化（首次采样不计数）

bool counterCountPwm = false;      // 开发模式：是否统计 PWM 引脚切变
bool counterCountFlash = false;    // 开发模式：是否统计 Flash/PSRAM 引脚切变

// ===== 组名规范化（复用 IO/比较器的命名规则）=====
static void counterSetName(CounterTask& task, const char* name) {
  String n = name ? name : "";
  n.trim();
  if (n.length() > IO_NAME_MAX - 1) n = n.substring(0, IO_NAME_MAX - 1);
  if (n.length() == 0) n = "未命名";
  strncpy(task.name, n.c_str(), IO_NAME_MAX - 1);
  task.name[IO_NAME_MAX - 1] = '\0';
}

// ===== 引脚占用查询 =====

// 引脚是否被某个计数器任务的输出动作占用（excludeTask 跳过自身）
bool counterIsPinUsedByAction(int pin, int excludeTask) {
  for (int t = 0; t < counterTaskCount; t++) {
    if (t == excludeTask) continue;
    for (int a = 0; a < counterTasks[t].actionCount; a++) {
      if (counterTasks[t].actions[a].pin == pin) return true;
    }
  }
  return false;
}

// 计数器动作的应有输出电平（供比较器引用）：按触发状态与保持模式计算，只跟随锁存
// （手动切换只是引脚的基准电平，不阻止计数器动作覆盖它）
int counterGetActionLevel(int taskIndex, int actionIndex) {
  if (taskIndex < 0 || taskIndex >= counterTaskCount) return 0;
  CounterTask& task = counterTasks[taskIndex];
  if (actionIndex < 0 || actionIndex >= task.actionCount) return 0;
  CounterAction& act = task.actions[actionIndex];
  if (latchIsEnabled(act.pin)) return latchLevel[act.pin];         // 被锁存 → 跟随锁存
  // 手动切换不再短路：手动值只是引脚的"基准电平"，计数器动作会覆盖它（见 updateCounter）
  if (task.enabled && task.fired) {
    if (act.holdMode == COUNTER_HOLD_KEEP) return act.level;
    if (millis() < task.autoOffUntil[actionIndex]) return act.level;
  }
  return 0;
}

// 校验触发源引脚（读）：仅排除 Flash/PSRAM 与 PWM
static bool counterCheckTriggerPin(int pin, bool devMode, bool allowPwm) {
  if (!isPinAvailable(pin)) { logf("[计数器] 触发源失败：GPIO%d 不在可用列表\n", pin); return false; }
  if (!devMode && isIoFlashPin(pin)) { logf("[计数器] 触发源失败：GPIO%d 为 Flash/PSRAM 总线\n", pin); return false; }
  if (!allowPwm && ioIsPinUsedByPwm(pin)) { logf("[计数器] 触发源失败：GPIO%d 被 PWM 占用\n", pin); return false; }
  // 只读输入反向守卫：已手动切换/锁存的引脚不能作触发源，否则 pinMode 是输出、读数是手动值，外部信号读不到
  if (manualIsOverridden(pin) || latchIsEnabled(pin)) { logf("[计数器] 触发源失败：GPIO%d 正被手动切换/锁存，请先恢复自动\n", pin); return false; }
  return true;
}

// 是否为该任务已有的动作脚（excludeAction 跳过正在编辑的那一条）。
// 同一任务内重复配置同一引脚没有意义：两条动作由同一次触发同时执行、互相覆盖，
// 且删除其中一条会把引脚置为输入、连另一条一起废掉。**无条件拒绝**（不受 devMode/allowDup 影响）。
bool counterTaskHasActionPin(int taskIndex, int pin, int excludeAction) {
  if (taskIndex < 0 || taskIndex >= counterTaskCount) return false;
  const CounterTask& task = counterTasks[taskIndex];
  for (int a = 0; a < task.actionCount; a++) {
    if (a == excludeAction) continue;
    if (task.actions[a].pin == pin) return true;
  }
  return false;
}

// 是否为"只读输入"型触发源：该引脚被某任务用作触发源，且没有被任何输出模块驱动。
// 被序列器/比较器输出占用时读到的是调度状态（既定用法：可用来数自己的脉冲），不算只读输入。
bool counterIsPinUsedAsReadOnlyTrigger(int pin) {
  bool isTrigger = false;
  for (int t = 0; t < counterTaskCount; t++) {
    if (counterTasks[t].triggerPin == pin) { isTrigger = true; break; }
  }
  if (!isTrigger) return false;
  if (ioIsPinUsedByPwm(pin)) return false;
  if (ioIsPinUsedByIo(pin, -1)) return false;
  if (comparatorIsPinUsedByOutput(pin, -1)) return false;
  if (counterIsPinUsedByAction(pin, -1)) return false;
  return true;
}

// 校验动作引脚（写）：全局唯一写者，与各模块输出/外部输入互斥
static bool counterCheckActionPin(int pin, int excludeTask, bool devMode, bool allowPwm, bool allowDup) {
  if (!isPinAvailable(pin)) { logf("[计数器] 输出失败：GPIO%d 不在可用列表\n", pin); return false; }
  if (!devMode && isIoFlashPin(pin)) { logf("[计数器] 输出失败：GPIO%d 为 Flash/PSRAM 总线\n", pin); return false; }
  if (!allowPwm && ioIsPinUsedByPwm(pin)) { logf("[计数器] 输出失败：GPIO%d 被 PWM 占用\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByIo(pin, -1)) { logf("[计数器] 输出失败：GPIO%d 被序列器占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByOutput(pin, -1)) { logf("[计数器] 输出失败：GPIO%d 被比较器输出占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByExternalInput(pin, -1)) { logf("[计数器] 输出失败：GPIO%d 被比较器外部输入占用\n", pin); return false; }
  if (!allowDup && counterIsPinUsedByAction(pin, excludeTask)) { logf("[计数器] 输出失败：GPIO%d 被其他计数器动作占用\n", pin); return false; }
  return true;
}

// ===== 任务 CRUD =====

bool counterAddTask(const char* name) {
  stateAssertHeld();   // 新任务插到最前、整段后移：必须在锁内，否则 loop 会读到正在搬移的数组
  if (counterTaskCount >= COUNTER_TASK_MAX) { logf("[计数器] 添加失败：数量已达上限 %d\n", COUNTER_TASK_MAX); return false; }
  // 新任务插入到最前面：新的在上，旧的顺延
  for (int t = counterTaskCount; t > 0; t--) counterTasks[t] = counterTasks[t - 1];
  CounterTask& task = counterTasks[0];
  counterSetName(task, name);
  task.enabled = true;
  task.triggerPin = -1;
  task.threshold = 0;
  task.actionCount = 0;
  task.triggerCount = 0;
  task.fired = false;
  for (int a = 0; a < COUNTER_ACTION_MAX; a++) task.autoOffUntil[a] = 0;
  counterTaskCount++;
  comparatorOnCounterTaskInserted();   // 比较器引用的计数器 taskIndex 后移
  logf("[计数器] 添加成功：%s，当前共 %d 个\n", task.name, counterTaskCount);
  return true;
}

bool counterRemoveTask(int index) {
  stateAssertHeld();   // 整段前移 + 释放引脚：必须在锁内
  if (index < 0 || index >= counterTaskCount) return false;
  // 释放输出引脚：置为输入，避免悬浮电平。被锁存的跳过——锁存是引脚级属性（连续性，见 PIN-4）
  for (int a = 0; a < counterTasks[index].actionCount; a++) {
    int p = counterTasks[index].actions[a].pin;
    if (latchIsEnabled(p)) {
      logf("[计数器] GPIO%d 已锁存，保留其电平不释放\n", p);
      continue;
    }
    pinMode(p, INPUT);
    digitalWrite(p, LOW);
  }
  for (int t = index; t < counterTaskCount - 1; t++) counterTasks[t] = counterTasks[t + 1];
  counterTaskCount--;
  comparatorOnCounterTaskRemoved(index);   // 修正比较器引用的计数器 taskIndex
  return true;
}

bool counterUpdateTask(int index, const char* name) {
  if (index < 0 || index >= counterTaskCount) return false;
  counterSetName(counterTasks[index], name);
  return true;
}

// 设置触发源引脚与阈值 X（重置触发计数与锁存）
bool counterSetTrigger(int index, int pin, int threshold, bool devMode, bool allowPwm) {
  if (index < 0 || index >= counterTaskCount) return false;
  if (threshold < 1) threshold = 1;
  if (!counterCheckTriggerPin(pin, devMode, allowPwm)) return false;
  CounterTask& task = counterTasks[index];
  for (int a = 0; a < task.actionCount; a++) {
    if (task.actions[a].pin == pin) { logf("[计数器] 触发源失败：GPIO%d 已被本任务动作占用\n", pin); return false; }
  }
  task.triggerPin = pin;
  task.threshold = threshold;
  task.triggerCount = 0;
  task.fired = false;
  // 触发源（读）可与 序列器/比较器输出共享：若引脚正被这些输出模块驱动，
  // 不能改成输入，否则会打断其输出、造成面板显示电平与实际电平不一致。
  // 电平变化由 gpioGetCurrentLevel 反映（序列器返回调度状态，其余返回 digitalRead）。
  if (!ioIsPinUsedByIo(pin, -1) && !comparatorIsPinUsedByOutput(pin, -1)) {
    pinMode(pin, INPUT);   // 仅纯外部输入引脚才设为输入读取真实电平
  }
  return true;
}

// ===== 动作 CRUD =====

bool counterAddAction(int taskIndex, int pin, int level, int holdMode, uint32_t holdSec, bool devMode, bool allowPwm, bool allowDup) {
  if (taskIndex < 0 || taskIndex >= counterTaskCount) return false;
  CounterTask& task = counterTasks[taskIndex];
  if (task.actionCount >= COUNTER_ACTION_MAX) { logf("[计数器] 输出失败：已达上限 %d\n", COUNTER_ACTION_MAX); return false; }
  if (pin == task.triggerPin) { logf("[计数器] 输出失败：GPIO%d 为当前任务触发源，不能同时作为动作\n", pin); return false; }
  if (counterTaskHasActionPin(taskIndex, pin, -1)) { logf("[计数器] 输出失败：GPIO%d 已是本任务的动作脚，不能重复添加\n", pin); return false; }
  if (!counterCheckActionPin(pin, taskIndex, devMode, allowPwm, allowDup)) return false;
  CounterAction& act = task.actions[task.actionCount];
  act.pin = pin;
  act.level = level ? 1 : 0;
  act.holdMode = (holdMode == COUNTER_HOLD_AUTO_OFF) ? COUNTER_HOLD_AUTO_OFF : COUNTER_HOLD_KEEP;
  if (holdSec > COUNTER_HOLD_MAX_SEC) holdSec = COUNTER_HOLD_MAX_SEC;
  act.holdSec = holdSec;
  task.autoOffUntil[task.actionCount] = 0;
  task.actionCount++;
  return true;
}

bool counterRemoveAction(int taskIndex, int actionIndex) {
  stateAssertHeld();   // 动作前移 + 释放引脚：必须在锁内
  if (taskIndex < 0 || taskIndex >= counterTaskCount) return false;
  CounterTask& task = counterTasks[taskIndex];
  if (actionIndex < 0 || actionIndex >= task.actionCount) return false;
  int p = task.actions[actionIndex].pin;
  if (latchIsEnabled(p)) {
    logf("[计数器] GPIO%d 已锁存，保留其电平不释放\n", p);
  } else {
    pinMode(p, INPUT);
    digitalWrite(p, LOW);
  }
  for (int a = actionIndex; a < task.actionCount - 1; a++) {
    task.actions[a] = task.actions[a + 1];
    task.autoOffUntil[a] = task.autoOffUntil[a + 1];
  }
  task.actionCount--;
  comparatorOnCounterActionRemoved(taskIndex, actionIndex);   // 修正比较器引用的计数器 actionIndex
  return true;
}

bool counterUpdateAction(int taskIndex, int actionIndex, int pin, int level, int holdMode, uint32_t holdSec, bool devMode, bool allowPwm, bool allowDup) {
  if (taskIndex < 0 || taskIndex >= counterTaskCount) return false;
  CounterTask& task = counterTasks[taskIndex];
  if (actionIndex < 0 || actionIndex >= task.actionCount) return false;
  if (pin == task.triggerPin) { logf("[计数器] 输出失败：GPIO%d 为当前任务触发源\n", pin); return false; }
  if (counterTaskHasActionPin(taskIndex, pin, actionIndex)) { logf("[计数器] 输出失败：GPIO%d 已是本任务的动作脚，不能重复添加\n", pin); return false; }
  if (task.actions[actionIndex].pin != pin) {
    if (!counterCheckActionPin(pin, taskIndex, devMode, allowPwm, allowDup)) return false;
    pinMode(task.actions[actionIndex].pin, INPUT);
    digitalWrite(task.actions[actionIndex].pin, LOW);
    task.actions[actionIndex].pin = pin;
  }
  task.actions[actionIndex].level = level ? 1 : 0;
  task.actions[actionIndex].holdMode = (holdMode == COUNTER_HOLD_AUTO_OFF) ? COUNTER_HOLD_AUTO_OFF : COUNTER_HOLD_KEEP;
  if (holdSec > COUNTER_HOLD_MAX_SEC) holdSec = COUNTER_HOLD_MAX_SEC;
  task.actions[actionIndex].holdSec = holdSec;
  return true;
}

// ===== 计量清零 =====
void counterClearAllCounts() {
  for (int i = 0; i < SOC_GPIO_PIN_COUNT; i++) ioToggleCount[i] = 0;
  for (int t = 0; t < counterTaskCount; t++) {
    counterTasks[t].triggerCount = 0;
    counterTasks[t].fired = false;
    for (int a = 0; a < counterTasks[t].actionCount; a++) counterTasks[t].autoOffUntil[a] = 0;
  }
  logRaw("[计数器] 已清零全部切变计数与触发状态\n");
}

// 清除单个引脚的切变计数
void counterClearPinCount(int pin) {
  stateAssertHeld();   // ioToggleCount 会被 loop 每轮累加：必须在锁内
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return;
  ioToggleCount[pin] = 0;
}

// ===== 执行引擎（loop 调用）=====
void updateCounter() {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：counterTasks / ioLastLevel / ioToggleCount 会被 handler 改
  // 1) 边沿检测：遍历所有可用 GPIO，检测电平切变
  //    默认跳过 PWM 与 Flash/PSRAM 引脚（开发模式可放开）
  for (int i = 0; i < getAvailablePinCount(); i++) {
    int pin = getAvailablePin(i);
    if (!counterCountPwm && ioIsPinUsedByPwm(pin)) continue;
    if (!counterCountFlash && isIoFlashPin(pin)) continue;
    int level = gpioGetCurrentLevel(pin);
    if (!ioLevelInit[pin]) {
      ioLevelInit[pin] = true;
      ioLastLevel[pin] = level;
      continue;   // 首次采样只记录，不计数
    }
    if (level != ioLastLevel[pin]) {
      ioLastLevel[pin] = level;
      ioToggleCount[pin]++;   // 全局累计（计量面板）
      // 同步触发以该引脚为触发源的任务
      for (int t = 0; t < counterTaskCount; t++) {
        CounterTask& task = counterTasks[t];
        if (task.triggerPin != pin || task.threshold < 1 || !task.enabled) continue;
        task.triggerCount++;   // 累计不清零，触发用"达到阈值整数倍"判断
        if (task.threshold >= 1 && (task.triggerCount % (uint32_t)task.threshold) == 0) {
          task.fired = true;
          for (int a = 0; a < task.actionCount; a++) {
            task.autoOffUntil[a] = millis() + (unsigned long)task.actions[a].holdSec * 1000UL;
          }
          // 不打印触发日志：触发频率跟随引脚切变，会持续刷屏（触发状态在网页计量面板可见）
        }
      }
    }
  }

  // 2) 动作输出：未触发=低，触发后按保持模式输出 level（只有被锁存的引脚跳过）
  for (int t = 0; t < counterTaskCount; t++) {
    CounterTask& task = counterTasks[t];
    for (int a = 0; a < task.actionCount; a++) {
      CounterAction& act = task.actions[a];
      if (latchIsEnabled(act.pin)) continue;   // 被锁存，最高权限，跳过输出
      // 手动切换不跳过：手动值只是该引脚的"基准电平"，计数器动作可以覆盖它
      int out = 0;
      if (task.enabled && task.fired) {
        if (act.holdMode == COUNTER_HOLD_KEEP) {
          out = act.level;
        } else { // COUNTER_HOLD_AUTO_OFF
          if (millis() < task.autoOffUntil[a]) out = act.level;
        }
      }
      pinMode(act.pin, OUTPUT);
      digitalWrite(act.pin, out ? HIGH : LOW);
    }
  }
}
