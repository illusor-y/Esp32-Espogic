#include "config.h"

// ===================== 比较器调度 =====================
// 模型：一个"比较器" = 左半输入条件 + 右半输出动作。
//   左半输入（AND 逻辑）：全部达到期望电平时，右半按"行为电平"输出；
//   任一输入不满足，右半复位到"源IO电平"（电平持续生效，非锁存）。
//   输入来源三种（由用户在网页选择）：
//     - 引用序列器（source=IO）：引用某个序列器，状态 = ioLevelAt() 算出的实时电平（含脉冲/心跳暂态）；
//     - 引用计数器动作（source=COUNTER）：引用某个计数器动作，状态 = counterGetActionLevel() 的应有输出电平；
//     - 外部输入（source=EXTERNAL）：独占一个 GPIO，pinMode(INPUT) 后 digitalRead 读取真实电平。
//   右半动作：{ pin, level(0/1) }，独占该 GPIO 并 digitalWrite 输出。
//
// 引脚互斥（全局唯一"写者"）：
//   PWM 输出、序列器输出、比较器输出 —— 三者两两互斥；
//   比较器外部输入 —— 不得与任何"写者"重叠；多个门的外部输入可共享同一引脚（多读不冲突）。

ComparatorGate comparatorGates[COMPARATOR_GATE_MAX];
int comparatorGateCount = 0;

// ===== 引脚占用查询（供 序列器 / 比较器互查）=====

// 引脚是否被某个比较器的"输出动作"占用（excludeGate 跳过自身）
bool comparatorIsPinUsedByOutput(int pin, int excludeGate) {
  for (int g = 0; g < comparatorGateCount; g++) {
    if (g == excludeGate) continue;
    for (int a = 0; a < comparatorGates[g].actionCount; a++) {
      if (comparatorGates[g].actions[a].pin == pin) return true;
    }
  }
  return false;
}

// 引脚是否被某个比较器的"外部输入"占用（excludeGate 跳过自身）
bool comparatorIsPinUsedByExternalInput(int pin, int excludeGate) {
  for (int g = 0; g < comparatorGateCount; g++) {
    if (g == excludeGate) continue;
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      const ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source == COMPARATOR_SRC_EXTERNAL && in.refIndex == pin) return true;
    }
  }
  return false;
}

// 组名规范化（复用 IO 的命名规则）
static void comparatorSetName(ComparatorGate& gate, const char* name) {
  String n = name ? name : "";
  n.trim();
  if (n.length() > IO_NAME_MAX - 1) n = n.substring(0, IO_NAME_MAX - 1);
  if (n.length() == 0) n = "未命名";
  strncpy(gate.name, n.c_str(), IO_NAME_MAX - 1);
  gate.name[IO_NAME_MAX - 1] = '\0';
}

// ===== 门 CRUD =====

bool comparatorAddGate(const char* name, int mode) {
  stateAssertHeld();   // 新门插到最前、整段后移：必须在锁内
  if (comparatorGateCount >= COMPARATOR_GATE_MAX) { logf("[比较器] 添加失败：数量已达上限 %d\n", COMPARATOR_GATE_MAX); return false; }
  // 新门插入到最前面：新的在上，旧的顺延（编辑更新时位置不变）
  for (int g = comparatorGateCount; g > 0; g--) comparatorGates[g] = comparatorGates[g - 1];
  ComparatorGate& gate = comparatorGates[0];
  comparatorSetName(gate, name);
  gate.enabled = true;
  gate.mode = (mode >= COMPARATOR_MODE_AND && mode <= COMPARATOR_MODE_NAND) ? mode : COMPARATOR_MODE_AND;
  gate.inputCount = 0;
  gate.actionCount = 0;
  comparatorGateCount++;
  logf("[比较器] 添加成功：%s，当前共 %d 个\n", gate.name, comparatorGateCount);
  return true;
}

bool comparatorRemoveGate(int index) {
  stateAssertHeld();   // 整段前移 + 释放引脚：必须在锁内
  if (index < 0 || index >= comparatorGateCount) return false;
  // 释放输出引脚：置为输入，避免悬浮电平。被锁存的跳过——锁存是引脚级属性（连续性，见 PIN-4）
  for (int a = 0; a < comparatorGates[index].actionCount; a++) {
    int p = comparatorGates[index].actions[a].pin;
    if (latchIsEnabled(p)) {
      logf("[比较器] GPIO%d 已锁存，保留其电平不释放\n", p);
      continue;
    }
    pinMode(p, INPUT);
    digitalWrite(p, LOW);
  }
  for (int g = index; g < comparatorGateCount - 1; g++) comparatorGates[g] = comparatorGates[g + 1];
  comparatorGateCount--;
  return true;
}

bool comparatorUpdateGate(int index, const char* name) {
  if (index < 0 || index >= comparatorGateCount) return false;
  comparatorSetName(comparatorGates[index], name);
  return true;
}

bool comparatorSetGateMode(int index, int mode) {
  if (index < 0 || index >= comparatorGateCount) return false;
  if (mode < COMPARATOR_MODE_AND || mode > COMPARATOR_MODE_NAND) return false;
  comparatorGates[index].mode = mode;
  return true;
}

// ===== 输入 CRUD =====

// 校验一个外部输入引脚是否可用（devMode 放开 Flash，allowPwm 放开 PWM 占用，allowDup 放开重复占用）
static bool comparatorCheckExternalInputPin(int pin, int excludeGate, bool devMode, bool allowPwm, bool allowDup) {
  if (!isPinAvailable(pin)) { logf("[比较器] 外部输入失败：GPIO%d 不在可用列表\n", pin); return false; }
  // 只读输入反向守卫：已手动切换/锁存的引脚不能作外部输入，否则 pinMode 被改成输出、读数是手动值
  if (manualIsOverridden(pin) || latchIsEnabled(pin)) { logf("[比较器] 外部输入失败：GPIO%d 正被手动切换/锁存，请先恢复自动\n", pin); return false; }
  if (!devMode && isIoFlashPin(pin)) { logf("[比较器] 外部输入失败：GPIO%d 为 Flash/PSRAM 总线\n", pin); return false; }
  if (!allowPwm && ioIsPinUsedByPwm(pin)) { logf("[比较器] 外部输入失败：GPIO%d 被 PWM 占用\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByIo(pin, -1)) { logf("[比较器] 外部输入失败：GPIO%d 被序列器占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByOutput(pin, excludeGate)) { logf("[比较器] 外部输入失败：GPIO%d 被比较器输出占用\n", pin); return false; }
  if (!allowDup && counterIsPinUsedByAction(pin, -1)) { logf("[比较器] 外部输入失败：GPIO%d 被计数器动作占用\n", pin); return false; }
  return true;
}

bool comparatorAddInput(int gateIndex, int source, int refIndex, int actionIndex, int expectedLevel, bool devMode, bool allowPwm, bool allowDup) {
  if (gateIndex < 0 || gateIndex >= comparatorGateCount) return false;
  ComparatorGate& gate = comparatorGates[gateIndex];
  if (gate.inputCount >= COMPARATOR_INPUT_MAX) { logf("[比较器] 输入失败：已达上限 %d\n", COMPARATOR_INPUT_MAX); return false; }
  if (source == COMPARATOR_SRC_EXTERNAL) {
    if (!comparatorCheckExternalInputPin(refIndex, gateIndex, devMode, allowPwm, allowDup)) return false;
    pinMode(refIndex, INPUT);
  } else if (source == COMPARATOR_SRC_IO) {
    if (refIndex < 0 || refIndex >= ioGroupCount) { logf("[比较器] 输入失败：序列器 %d 无效\n", refIndex); return false; }
  } else if (source == COMPARATOR_SRC_COUNTER) {
    if (refIndex < 0 || refIndex >= counterTaskCount || actionIndex < 0 || actionIndex >= counterTasks[refIndex].actionCount) {
      logf("[比较器] 输入失败：计数器动作 %d/%d 无效\n", refIndex, actionIndex); return false;
    }
  } else {
    logf("[比较器] 输入失败：未知来源 %d\n", source); return false;
  }
  ComparatorInput& in = gate.inputs[gate.inputCount++];
  in.source = source;
  in.refIndex = refIndex;
  in.actionIndex = (source == COMPARATOR_SRC_COUNTER) ? actionIndex : 0;
  in.expectedLevel = expectedLevel ? 1 : 0;
  return true;
}

bool comparatorRemoveInput(int gateIndex, int inputIndex) {
  stateAssertHeld();   // 输入前移：必须在锁内（loop 每轮按 inputCount 遍历）
  if (gateIndex < 0 || gateIndex >= comparatorGateCount) return false;
  ComparatorGate& gate = comparatorGates[gateIndex];
  if (inputIndex < 0 || inputIndex >= gate.inputCount) return false;
  for (int i = inputIndex; i < gate.inputCount - 1; i++) gate.inputs[i] = gate.inputs[i + 1];
  gate.inputCount--;
  return true;
}

bool comparatorUpdateInput(int gateIndex, int inputIndex, int source, int refIndex, int actionIndex, int expectedLevel, bool devMode, bool allowPwm, bool allowDup) {
  if (gateIndex < 0 || gateIndex >= comparatorGateCount) return false;
  ComparatorGate& gate = comparatorGates[gateIndex];
  if (inputIndex < 0 || inputIndex >= gate.inputCount) return false;
  if (source == COMPARATOR_SRC_EXTERNAL) {
    if (!comparatorCheckExternalInputPin(refIndex, gateIndex, devMode, allowPwm, allowDup)) return false;
    pinMode(refIndex, INPUT);
  } else if (source == COMPARATOR_SRC_IO) {
    if (refIndex < 0 || refIndex >= ioGroupCount) return false;
  } else if (source == COMPARATOR_SRC_COUNTER) {
    if (refIndex < 0 || refIndex >= counterTaskCount || actionIndex < 0 || actionIndex >= counterTasks[refIndex].actionCount) return false;
  } else {
    return false;
  }
  gate.inputs[inputIndex].source = source;
  gate.inputs[inputIndex].refIndex = refIndex;
  gate.inputs[inputIndex].actionIndex = (source == COMPARATOR_SRC_COUNTER) ? actionIndex : 0;
  gate.inputs[inputIndex].expectedLevel = expectedLevel ? 1 : 0;
  return true;
}

// ===== 输出动作 CRUD =====

// 校验一个输出引脚是否可用（不与任何写者/外部输入冲突）
static bool comparatorCheckOutputPin(int pin, int excludeGate, bool devMode, bool allowPwm, bool allowDup) {
  if (!isPinAvailable(pin)) { logf("[比较器] 输出失败：GPIO%d 不在可用列表\n", pin); return false; }
  if (!devMode && isIoFlashPin(pin)) { logf("[比较器] 输出失败：GPIO%d 为 Flash/PSRAM 总线\n", pin); return false; }
  if (!allowPwm && ioIsPinUsedByPwm(pin)) { logf("[比较器] 输出失败：GPIO%d 被 PWM 占用\n", pin); return false; }
  if (!allowDup && ioIsPinUsedByIo(pin, -1)) { logf("[比较器] 输出失败：GPIO%d 被序列器占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByOutput(pin, excludeGate)) { logf("[比较器] 输出失败：GPIO%d 被其他比较器输出占用\n", pin); return false; }
  if (!allowDup && comparatorIsPinUsedByExternalInput(pin, excludeGate)) { logf("[比较器] 输出失败：GPIO%d 被比较器外部输入占用\n", pin); return false; }
  if (!allowDup && counterIsPinUsedByAction(pin, -1)) { logf("[比较器] 输出失败：GPIO%d 被计数器动作占用\n", pin); return false; }
  return true;
}

bool comparatorAddAction(int gateIndex, int pin, int sourceLevel, int actionLevel, bool devMode, bool allowPwm, bool allowDup) {
  if (gateIndex < 0 || gateIndex >= comparatorGateCount) return false;
  ComparatorGate& gate = comparatorGates[gateIndex];
  if (gate.actionCount >= COMPARATOR_ACTION_MAX) { logf("[比较器] 输出失败：已达上限 %d\n", COMPARATOR_ACTION_MAX); return false; }
  if (!comparatorCheckOutputPin(pin, gateIndex, devMode, allowPwm, allowDup)) return false;
  ComparatorAction& act = gate.actions[gate.actionCount++];
  act.pin = pin;
  act.sourceLevel = sourceLevel ? 1 : 0;
  act.actionLevel = actionLevel ? 1 : 0;
  return true;
}

bool comparatorRemoveAction(int gateIndex, int actionIndex) {
  if (gateIndex < 0 || gateIndex >= comparatorGateCount) return false;
  ComparatorGate& gate = comparatorGates[gateIndex];
  if (actionIndex < 0 || actionIndex >= gate.actionCount) return false;
  int p = gate.actions[actionIndex].pin;
  if (latchIsEnabled(p)) {
    logf("[比较器] GPIO%d 已锁存，保留其电平不释放\n", p);
  } else {
    pinMode(p, INPUT);
    digitalWrite(p, LOW);
  }
  for (int a = actionIndex; a < gate.actionCount - 1; a++) gate.actions[a] = gate.actions[a + 1];
  gate.actionCount--;
  return true;
}

bool comparatorUpdateAction(int gateIndex, int actionIndex, int pin, int sourceLevel, int actionLevel, bool devMode, bool allowPwm, bool allowDup) {
  if (gateIndex < 0 || gateIndex >= comparatorGateCount) return false;
  ComparatorGate& gate = comparatorGates[gateIndex];
  if (actionIndex < 0 || actionIndex >= gate.actionCount) return false;
  if (gate.actions[actionIndex].pin != pin) {
    if (!comparatorCheckOutputPin(pin, gateIndex, devMode, allowPwm, allowDup)) return false;
    // 释放旧引脚
    pinMode(gate.actions[actionIndex].pin, INPUT);
    digitalWrite(gate.actions[actionIndex].pin, LOW);
    gate.actions[actionIndex].pin = pin;
  }
  gate.actions[actionIndex].sourceLevel = sourceLevel ? 1 : 0;
  gate.actions[actionIndex].actionLevel = actionLevel ? 1 : 0;
  return true;
}

// IO 组被删除后：修正比较器内部输入的组索引引用
void comparatorOnIoGroupRemoved(int removedIndex) {
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source != COMPARATOR_SRC_IO) continue;
      if (in.refIndex == removedIndex) {
        in.refIndex = -1;   // 标记失效（评估时按低电平处理）
        logf("[比较器] %s 的输入引用的序列器已删除，标记失效\n", comparatorGates[g].name);
      } else if (in.refIndex > removedIndex) {
        in.refIndex--;
      }
    }
  }
}

// IO 组插入到最前面后：所有有效的内部输入组索引统一后移一位
void comparatorOnIoGroupInserted() {
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source != COMPARATOR_SRC_IO) continue;
      if (in.refIndex >= 0) in.refIndex++;
    }
  }
}

// 计数器任务插入到最前时：引用计数器动作的输入 taskIndex 统一后移一位
void comparatorOnCounterTaskInserted() {
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source != COMPARATOR_SRC_COUNTER) continue;
      if (in.refIndex >= 0) in.refIndex++;
    }
  }
}

// 计数器任务删除后：引用该任务的输入失效，>removedIndex 的 -1
void comparatorOnCounterTaskRemoved(int removedIndex) {
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source != COMPARATOR_SRC_COUNTER) continue;
      if (in.refIndex == removedIndex) {
        in.refIndex = -1;
        logf("[比较器] %s 的输入引用的计数器任务已删除，标记失效\n", comparatorGates[g].name);
      } else if (in.refIndex > removedIndex) {
        in.refIndex--;
      }
    }
  }
}

// 计数器动作删除后：引用该动作的输入失效，同任务 >actionIndex 的 -1
void comparatorOnCounterActionRemoved(int taskIndex, int actionIndex) {
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      ComparatorInput& in = comparatorGates[g].inputs[i];
      if (in.source != COMPARATOR_SRC_COUNTER) continue;
      if (in.refIndex != taskIndex) continue;
      if (in.actionIndex == actionIndex) {
        in.refIndex = -1;
        logf("[比较器] %s 的输入引用的计数器动作已删除，标记失效\n", comparatorGates[g].name);
      } else if (in.actionIndex > actionIndex) {
        in.actionIndex--;
      }
    }
  }
}

// ===== 执行引擎（loop 调用）=====
// 读比较器输入的电平，引用失效返回 -1（不参与满足计数）
static int comparatorReadInputLevel(const ComparatorInput& in) {
  if (in.source == COMPARATOR_SRC_IO) {
    if (in.refIndex < 0 || in.refIndex >= ioGroupCount) return -1;
    return gpioGetCurrentLevel(ioGroups[in.refIndex].pin);
  } else if (in.source == COMPARATOR_SRC_COUNTER) {
    if (in.refIndex < 0 || in.refIndex >= counterTaskCount ||
        in.actionIndex < 0 || in.actionIndex >= counterTasks[in.refIndex].actionCount) return -1;
    return counterGetActionLevel(in.refIndex, in.actionIndex);
  } else { // COMPARATOR_SRC_EXTERNAL
    return gpioGetCurrentLevel(in.refIndex);
  }
}

// 门当前是否触发（按 mode 评估，含最小条件数约束）
bool comparatorGateMet(const ComparatorGate& gate) {
  int total = gate.inputCount;
  int met = 0;
  for (int i = 0; i < total; i++) {
    int lvl = comparatorReadInputLevel(gate.inputs[i]);
    if (lvl >= 0 && lvl == gate.inputs[i].expectedLevel) met++;
  }
  switch (gate.mode) {
    case COMPARATOR_MODE_OR:   return total >= 2 && met >= 1;      // 或：任一达成（需 >=2 条件）
    case COMPARATOR_MODE_NOR:  return total >= 1 && met == 0;      // 或非：无一达成（需 >=1 条件）
    case COMPARATOR_MODE_NAND: return total >= 2 && met < total;   // 与非：非全部满足（需 >=2 条件）
    case COMPARATOR_MODE_AND:
    default:                   return total >= 1 && met == total;  // 与：全部达成
  }
}

void updateComparatorOutput() {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：comparatorGates 会被 handler 改（增删门/输入/输出会前移数组）
  for (int g = 0; g < comparatorGateCount; g++) {
    ComparatorGate& gate = comparatorGates[g];
    // 评估左半：按门逻辑类型（AND/OR/NOT/NAND）判断是否触发
    bool allMet = gate.enabled && comparatorGateMet(gate);
    // 输出右半：满足 = 行为电平，不满足 = 源IO电平（被锁存的引脚跳过）
    for (int a = 0; a < gate.actionCount; a++) {
      ComparatorAction& act = gate.actions[a];
      if (latchIsEnabled(act.pin)) continue;   // 被锁存，最高权限，跳过输出
      // 手动切换不跳过：手动值只是该引脚的"基准电平"，比较器输出可以覆盖它
      int out = allMet ? act.actionLevel : act.sourceLevel;
      pinMode(act.pin, OUTPUT);
      digitalWrite(act.pin, out ? HIGH : LOW);
    }
  }
}
