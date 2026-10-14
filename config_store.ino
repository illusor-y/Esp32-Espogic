#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <driver/gpio.h>
#include <nvs.h>            // nvs_get_stats：Preferences 不暴露容量信息，容量条要用它

// ===================== 配置存储（NVS / Preferences）=====================
// 说明：原实现使用 EEPROM（EEPROM.commit() 整块擦写 flash），commit 期间
// 中断被长时间关闭，是 ESP32 随机 INT_WDT 复位的重要诱因。
// 现改用 NVS：小块增量写入 + 磨损均衡，commit 临界区通常 <10ms。
// S3 沿用 NVS，额外收益是 Flash 更大（16MB），寿命预算更宽裕。

static const char* NVS_NS = "smartglass";   // 命名空间，<=15 字符
static const char* NVS_KEY_JSON = "config";

// ===================== 跨任务状态锁实现 =====================
// 设计说明见 config.h 的同名段落。要点：递归锁（handler -> 模块变更函数可嵌套）、
// 手动记录持有者与深度（比 xSemaphoreGetMutexHolder 跨版本更稳，也让断言是纯读操作）。
SemaphoreHandle_t stateMutex = NULL;
static TaskHandle_t stateLockOwner = NULL;   // 最外层持有者（递归时只有它会被记录）
static int  stateLockDepth = 0;              // 递归深度，归零时清空持有者
static bool stateLockArmed = false;          // 断言开关：HTTP 任务创建前不校验（setup 单线程阶段）

void stateLockInit() {
  if (stateMutex == NULL) stateMutex = xSemaphoreCreateRecursiveMutex();
}

void stateLock() {
  if (stateMutex == NULL) return;   // 锁创建失败：退化为单线程行为，宁可慢也不要卡死
  xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
  stateLockOwner = xTaskGetCurrentTaskHandle();
  stateLockDepth++;
}

void stateUnlock() {
  if (stateMutex == NULL) return;
  if (stateLockDepth > 0) stateLockDepth--;
  if (stateLockDepth == 0) stateLockOwner = NULL;
  xSemaphoreGiveRecursive(stateMutex);
}

// 兜底解锁：只在本任务确实持锁时生效，一次退到最外层。
// 用途：handler 因异常路径没走到 httpSend 就返回时，由 runLocked 调用，避免锁永久泄漏。
void stateUnlockIfHeld() {
  if (stateMutex == NULL) return;
  if (stateLockOwner != xTaskGetCurrentTaskHandle()) return;
  while (stateLockDepth > 0) stateUnlock();
}

bool stateIsHeldByCurrentTask() {
  return stateMutex != NULL && stateLockOwner == xTaskGetCurrentTaskHandle();
}

void stateLockArm(bool armed) { stateLockArmed = armed; }

// 断言告警：限流打印（前 20 次逐条，之后每 100 次一条），避免一旦踩中就把日志刷爆
static void stateWarn(const char* what) {
  static uint32_t count = 0;
  count++;
  if (count <= 20 || (count % 100) == 0) {
    logf("[锁] 断言失败：%s（累计 %lu 次）\n", what, (unsigned long)count);
  }
}

void stateAssertHeld() {
  if (!stateLockArmed) return;
  if (stateIsHeldByCurrentTask()) return;
  stateWarn("未持状态锁就修改共享状态（该函数应由 handler 或 loop 在锁内调用）");
}

void stateAssertNotHeld() {
  if (!stateLockArmed) return;
  if (!stateIsHeldByCurrentTask()) return;
  stateWarn("持状态锁发送响应（会把 loop 卡住）");
}

// ===== NVS 存 JSON 必须用 blob 接口（不要改回 putString）=====
// Preferences::putString 走的是 nvs_set_str，ESP-IDF 对字符串单条长度的限制是
// **4000 字节**（见 nvs.h：值的最大长度含结尾符 4000 字节），配置一超过就会保存失败。
// 改用 putBytes（nvs_set_blob）后，单条上限变为 min(508000, 97.6%×分区大小−4000)，
// 默认 20KB 分区下约 15.6KB；而且这是分区表不必改动的纯软件方案，不影响多板适配。
// 所以写入点统一写 prefs.putBytes(key, json.c_str(), json.length())。
//
// 注意：helper 的形参里**不要出现 Preferences&**。Arduino 自动生成函数原型时解析不了
// 这个库类型，会把原型错写成变量声明（"size_t nvsWriteJsonTo;"），导致整个文件编译失败。
// 因此读取 helper 自己开关句柄，只接 key。

// 读取 JSON：先按 blob 读，读不到（key 不存在，或该 key 是升级前的字符串类型）再回退按字符串读。
// 回退分支保证升级不会丢配置：老数据照常读得到，下次保存时自动转成 blob。
static String nvsReadJsonFrom(const char* key) {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return String();
  String out;
  size_t len = prefs.getBytesLength(key);
  if (len > 0) {
    // 是 blob：只能按 blob 读。读失败（如堆不足）就当无数据返回，不要回退到 getString
    // —— 对 blob 调 nvs_get_str 必然类型不匹配，只会多打一条错误日志、还掩盖真实原因。
    char* buf = (char*)malloc(len + 1);
    if (buf) {
      size_t got = prefs.getBytes(key, buf, len);
      if (got == len) {
        buf[len] = '\0';        // blob 不含结尾符，读取后自行补上
        out = buf;
      }
      free(buf);
    }
  } else {
    out = prefs.getString(key, "");   // 回退：兼容由 putString(nvs_set_str) 写入的旧数据
  }
  prefs.end();
  return out;
}

void initConfigStore() {
  stateLockInit();   // 跨任务状态锁：必须早于任何任务创建（setup 单线程阶段，无竞争）
  Preferences prefs;
  bool present = prefs.begin(NVS_NS, false) && prefs.isKey(NVS_KEY_JSON);
  prefs.end();
  logf("[NVS] 配置存储已初始化，命名空间: %s%s\n", NVS_NS, present ? "（已有配置）" : "（首次使用）");
}

// ===== 保存失败可见 =====
// 各模块的实际落盘都发生在 loop() 的防抖回调里（checkPendingXxxSave），HTTP 请求早就返回
// 成功了；NVS 一旦写满或损坏，用户只会在重启后发现配置退回旧值。这里让每个模块记住自己
// 最近一次落盘的结果，经前端每秒轮询的 /getstatus 的 saveFail 字段带回，显示为常驻横幅。
enum {
  SAVE_MOD_PWM = 0, SAVE_MOD_IO, SAVE_MOD_COMPARATOR, SAVE_MOD_COUNTER, SAVE_MOD_CLOCK,
  SAVE_MOD_COUNT
};
static bool saveFailFlag[SAVE_MOD_COUNT] = {false};
static const char* const saveModuleNames[SAVE_MOD_COUNT] = { "PWM 通道", "序列器", "比较器", "计数器", "时钟" };

static void noteSaveResult(int module, bool ok) {
  saveFailFlag[module] = !ok;
}

// 汇总失败的模块名；全部正常时返回空串
String saveFailSummary() {
  String s;
  for (int i = 0; i < SAVE_MOD_COUNT; i++) {
    if (!saveFailFlag[i]) continue;
    if (s.length()) s += "、";
    s += saveModuleNames[i];
  }
  return s;
}

bool saveConfig() {
  DynamicJsonDocument doc(16384);   // 堆上分配，避免栈溢出（8通道×6条目满配约需14KB）
  JsonArray pinsArray = doc.createNestedArray("pins");
  for (int i = 0; i < pwmChannelCount; i++) {
    // 总线引脚不写入 NVS：pins[i] 与 pwmConfigs[i] 按下标一一对应，
    // 必须两个循环用同一个判断整条通道一起跳过，否则下标错位会让配置串到别的通道上。
    if (isIoFlashPin(pwmPins[i])) continue;
    pinsArray.add(pwmPins[i]);
  }
  doc["wifi"]["ssid"] = wifiSsid;
  doc["wifi"]["password"] = wifiPassword;
  JsonArray pwmArray = doc.createNestedArray("pwmConfigs");
  int defClock = clockGetFirstEnabled();   // 条目未指定时钟时对应的槽位（读侧同样取它）
  for (int i = 0; i < pwmChannelCount; i++) {
    if (isIoFlashPin(pwmPins[i])) continue;   // 与上面 pins 数组保持完全相同的过滤
    JsonObject pwmObj = pwmArray.createNestedObject();
    // 与序列器同一套压缩方案：只写"与读侧默认值不同"的字段。
    // applyPwmConfigs 对缺失字段的默认值是：enabled=false、weatherInteract=false、
    // sunEnabled=false、sunMode=SUN_MODE_SUNRISE(0)、sunPeakDuty=100、
    // taskCount=tasks 数组长度、条目级默认值见下面各行的判断。
    // 注意：改动这里的 100/defClock 等必须同步改 applyPwmConfigs，否则会静默改变语义。
    // 键名必须与「配置导出」完全一致（sunEnabled/sunMode/sunPeakDuty）——
    // 导出文件也是走 applyPwmConfigs 导入的，键名不一致会让导入静默丢字段。
    if (pwmConfigs[i].enabled)         pwmObj["enabled"] = true;
    if (pwmConfigs[i].weatherInteract) pwmObj["weatherInteract"] = true;
    if (pwmConfigs[i].sunEnabled)      pwmObj["sunEnabled"] = true;
    if (pwmConfigs[i].sunMode)         pwmObj["sunMode"] = pwmConfigs[i].sunMode;
    if (pwmConfigs[i].sunPeakDuty != 100) pwmObj["sunPeakDuty"] = pwmConfigs[i].sunPeakDuty;
    JsonArray tasksArray = pwmObj.createNestedArray("tasks");
    for (int j = 0; j < pwmConfigs[i].taskCount; j++) {
      const PwmTask& t = pwmConfigs[i].tasks[j];
      JsonObject taskObj = tasksArray.createNestedObject();
      if (t.enabled)        taskObj["enabled"] = true;
      if (t.startHour)      taskObj["startHour"] = t.startHour;
      if (t.startMinute)    taskObj["startMinute"] = t.startMinute;
      if (t.endHour)        taskObj["endHour"] = t.endHour;
      if (t.endMinute)      taskObj["endMinute"] = t.endMinute;
      if (t.startDuty)      taskObj["startDuty"] = t.startDuty;
      if (t.endDuty != 100) taskObj["endDuty"] = t.endDuty;
      if (t.clockIndex != defClock) taskObj["clock"] = t.clockIndex;
      // overnight 是 isTaskOvernight() 的派生值（读侧从不读取），不再写入
    }
  }
  // 合并成一行输出：原来每通道一行，8 通道满配 7 行约 434 字节。串口写在持状态锁期间进行，
  // 会同时阻塞 loop 与 HTTP 任务（HWCDC 单次写超时默认 100ms），实测 loop 卡顿
  // 51ms ≈ commit 8ms + 日志约 40ms。
  // 硬约束：含时间戳必须 ≤ LOG_MAX_LEN-1(159)，否则会被切块输出又变回多次写。
  // 8 通道约 104 字节，余量充足；PWM_CHANNEL_MAX 若将来调大，这里会自动截断。
  String pwmLog = "[NVS] 保存PWM:";
  for (int i = 0; i < pwmChannelCount; i++) {
    char one[20];
    snprintf(one, sizeof(one), " CH%d:e%dw%dt%ds%d", i + 1,   // 1-based，与其它日志一致
             pwmConfigs[i].enabled ? 1 : 0, pwmConfigs[i].weatherInteract ? 1 : 0,
             pwmConfigs[i].taskCount, pwmConfigs[i].sunEnabled ? 1 : 0);
    if (pwmLog.length() + strlen(one) > 125) { pwmLog += " ..."; break; }
    pwmLog += one;
  }
  logf("%s\n", pwmLog.c_str());
  String jsonStr;
  serializeJson(doc, jsonStr);

  Preferences prefs;
  unsigned long t0 = millis();
  bool ok = prefs.begin(NVS_NS, false);
  // 写入走 blob 接口，内部已调用 nvs_commit；返回 0 表示写入或提交失败
  size_t written = ok ? prefs.putBytes(NVS_KEY_JSON, jsonStr.c_str(), jsonStr.length()) : 0;
  prefs.end();
  unsigned long dt = millis() - t0;

  if (written > 0) {
    logf("[NVS] 已保存, 数据=%dB, commit耗时=%lums\n", jsonStr.length(), dt);
    noteSaveResult(SAVE_MOD_PWM, true);
    return true;
  }

  logf("[NVS] 保存失败! begin=%s, written=%lu（NVS 分区损坏或已满，建议 Erase All Flash 后重烧）\n",
       ok ? "ok" : "fail", (unsigned long)written);
  noteSaveResult(SAVE_MOD_PWM, false);
  return false;
}

// 校验并应用引脚配置：非法或重复则整体忽略，保持原值
static void applyPins(JsonArray arr) {
  int n = arr.size();
  if (n > PWM_CHANNEL_MAX) n = PWM_CHANNEL_MAX;
  int tmp[PWM_CHANNEL_MAX];
  for (int i = 0; i < n; i++) {
    tmp[i] = arr[i] | pwmPins[i];
  }
  for (int i = 0; i < n; i++) {
    if (!isPinAvailable(tmp[i])) return;
    for (int k = 0; k < i; k++) if (tmp[k] == tmp[i]) return;
  }
  for (int i = 0; i < n; i++) pwmPins[i] = tmp[i];
  pwmChannelCount = n;
}

// 应用 PWM 通道配置（先按默认值初始化全部通道，再用保存值覆盖）
static void applyPwmConfigs(JsonArray pwmArray) {
  int defClock = clockGetFirstEnabled();
  for (int i = 0; i < pwmChannelCount; i++) {
    pwmConfigs[i].enabled = false;
    pwmConfigs[i].weatherInteract = false;
    pwmConfigs[i].taskCount = 0;
    pwmConfigs[i].currentDuty = 0.0;
    pwmConfigs[i].targetDuty = 0.0;
    pwmConfigs[i].weatherDuty = 0.0;
    pwmConfigs[i].activeTask = 0;
    pwmConfigs[i].sunEnabled = false;
    pwmConfigs[i].sunMode = SUN_MODE_SUNRISE;
    pwmConfigs[i].sunPeakDuty = 100;
    pwmConfigs[i].sunActive = false;
    pwmConfigs[i].sunStartMin = -1;
    pwmConfigs[i].sunEndMin = -1;
    pwmConfigs[i].sunDuty = 0.0f;
    for (int j = 0; j < PWM_TASK_MAX; j++) pwmConfigs[i].tasks[j] = {false, 0, 0, 0, 0, 0, 100, defClock, false};
  }

  for (int i = 0; i < pwmChannelCount && i < pwmArray.size(); i++) {
    JsonObject pwmObj = pwmArray[i];
    pwmConfigs[i].enabled = pwmObj["enabled"] | false;
    pwmConfigs[i].weatherInteract = pwmObj["weatherInteract"] | false;
    // 阳光交互：默认关闭 + 日出日落模式 + 峰值 100%（写侧只在非默认时落盘，见 saveConfig）
    pwmConfigs[i].sunEnabled = pwmObj["sunEnabled"] | false;
    pwmConfigs[i].sunMode = (uint8_t)safeConstrain(pwmObj["sunMode"] | SUN_MODE_SUNRISE, SUN_MODE_SUNRISE, SUN_MODE_TWILIGHT);
    pwmConfigs[i].sunPeakDuty = safeConstrain(pwmObj["sunPeakDuty"] | 100, 0, 100);
    // 条目数以 tasks 数组长度为准（写侧已不再写 taskCount 字段，它本就是派生值）。
    // 对老配置等价：老写入侧的 taskCount 与 tasks 长度始终一致。
    JsonArray tasksArray = pwmObj["tasks"];
    pwmConfigs[i].taskCount = safeConstrain((int)tasksArray.size(), 0, PWM_TASK_MAX);
    for (int j = 0; j < pwmConfigs[i].taskCount; j++) {
      JsonObject taskObj = tasksArray[j];
      int sh = taskObj["startHour"] | 0;
      int sm = taskObj["startMinute"] | 0;
      int eh = taskObj["endHour"] | 0;
      int em = taskObj["endMinute"] | 0;
      int sd = taskObj["startDuty"] | 0;
      int ed = taskObj["endDuty"] | 100;
      int ck = taskObj["clock"] | defClock;   // 兼容旧数据：无 clock 字段缺省默认时钟
      pwmConfigs[i].tasks[j].enabled = taskObj["enabled"] | false;
      pwmConfigs[i].tasks[j].startHour = safeConstrain(sh, 0, 23);
      pwmConfigs[i].tasks[j].startMinute = safeConstrain(sm, 0, 59);
      pwmConfigs[i].tasks[j].endHour = safeConstrain(eh, 0, 23);
      pwmConfigs[i].tasks[j].endMinute = safeConstrain(em, 0, 59);
      pwmConfigs[i].tasks[j].startDuty = safeConstrain(sd, 0, 100);
      pwmConfigs[i].tasks[j].endDuty = safeConstrain(ed, 0, 100);
      pwmConfigs[i].tasks[j].clockIndex = safeConstrain(ck, 0, CLOCK_MAX - 1);
      pwmConfigs[i].tasks[j].conflict = false;
    }
  }
  for (int i = 0; i < pwmChannelCount; i++) {
    logf("[NVS] loadConfig CH%d enabled=%d weatherInteract=%d\n",
         i + 1, pwmConfigs[i].enabled ? 1 : 0, pwmConfigs[i].weatherInteract ? 1 : 0);
  }
  pwmRecalcConflicts();
}

// 应用时钟配置（导入用）：校验至少一个 + 频率去重
static void applyClockConfig(JsonArray arr) {
  for (int i = 0; i < CLOCK_MAX; i++) { clocks[i].enabled = false; clocks[i].freq = 0; clocks[i].maxVal = 0; }
  int n = arr.size();
  if (n > CLOCK_MAX) n = CLOCK_MAX;
  for (int i = 0; i < n; i++) {
    bool en = arr[i]["enabled"] | false;
    uint32_t f = (uint32_t)safeConstrain(arr[i]["freq"] | PWM_FREQ_DEFAULT, 1, 40000);
    clocks[i].enabled = en;
    clocks[i].freq = f;
  }
  if (clockGetFirstEnabled() < 0) { clocks[0].enabled = true; clocks[0].freq = PWM_FREQ_DEFAULT; }
  for (int i = 0; i < CLOCK_MAX; i++) {
    if (!clocks[i].enabled) continue;
    for (int k = 0; k < i; k++) if (clocks[k].enabled && clocks[k].freq == clocks[i].freq) { clocks[i].enabled = false; break; }
  }
  if (clockGetFirstEnabled() < 0) { clocks[0].enabled = true; clocks[0].freq = PWM_FREQ_DEFAULT; }
}

void loadConfig() {
  String jsonStr = nvsReadJsonFrom(NVS_KEY_JSON);

  if (jsonStr.length() == 0) {
    // 首次启动：写入默认配置
    logRaw("[NVS] 首次启动：无已有配置，写入默认配置\n");
    currentTime = {0, 0, 0};
    wifiSsid = "";
    wifiPassword = "";
    for (int i = 0; i < pwmChannelCount; i++) {
      pwmConfigs[i].enabled = false;
      pwmConfigs[i].taskCount = 0;
      pwmConfigs[i].currentDuty = 0.0;
      pwmConfigs[i].targetDuty = 0.0;
      pwmConfigs[i].activeTask = 0;
      pwmConfigs[i].sunEnabled = false;
      pwmConfigs[i].sunMode = SUN_MODE_SUNRISE;
      pwmConfigs[i].sunPeakDuty = 100;
      pwmConfigs[i].sunActive = false;
      pwmConfigs[i].sunStartMin = -1;
      pwmConfigs[i].sunEndMin = -1;
      pwmConfigs[i].sunDuty = 0.0f;
      for (int j = 0; j < PWM_TASK_MAX; j++) {
        pwmConfigs[i].tasks[j] = {false, 0, 0, 0, 0, 0, 100, clockGetFirstEnabled(), false};
      }
    }
    saveConfig();
    return;
  }

  DynamicJsonDocument doc(16384);   // 堆上分配，避免栈溢出（8通道×6条目满配约需14KB）
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    // 数据损坏：按默认配置初始化
    logf("[NVS] 警告：配置解析失败 (%s)，使用默认配置\n", error.c_str());
    wifiSsid = "";
    wifiPassword = "";
    for (int i = 0; i < pwmChannelCount; i++) {
      pwmConfigs[i].enabled = false;
      pwmConfigs[i].taskCount = 0;
      pwmConfigs[i].currentDuty = 0.0;
      pwmConfigs[i].targetDuty = 0.0;
      pwmConfigs[i].activeTask = 0;
      pwmConfigs[i].sunEnabled = false;
      pwmConfigs[i].sunMode = SUN_MODE_SUNRISE;
      pwmConfigs[i].sunPeakDuty = 100;
      pwmConfigs[i].sunActive = false;
      pwmConfigs[i].sunStartMin = -1;
      pwmConfigs[i].sunEndMin = -1;
      pwmConfigs[i].sunDuty = 0.0f;
      for (int j = 0; j < PWM_TASK_MAX; j++) {
        pwmConfigs[i].tasks[j] = {false, 0, 0, 0, 0, 0, 100, clockGetFirstEnabled(), false};
      }
    }
    saveConfig();
    return;
  }

  wifiSsid = doc["wifi"]["ssid"] | "";
  wifiPassword = doc["wifi"]["password"] | "";
  applyPins(doc["pins"]);   // 先确定 pwmChannelCount（依据保存的 pins 数量）
  applyPwmConfigs(doc["pwmConfigs"]);

  // 打印加载结果摘要
  logf("[NVS] 配置加载成功: WiFi SSID=%s, 密码长度=%d\n",
       wifiSsid.length() ? wifiSsid.c_str() : "(未配置)",
       wifiPassword.length());
  String chStatus;
  chStatus.reserve(64);
  for (int i = 0; i < pwmChannelCount; i++) {
    if (i > 0) chStatus += ", ";
    chStatus += "CH";
    chStatus += String(i + 1);
    chStatus += "=";
    chStatus += pwmConfigs[i].enabled ? "开" : "关";
  }
  logf("[NVS] 通道状态: %s\n", chStatus.c_str());
}

// ===== 防抖合并写入：避免频繁保存造成 Flash 磨损 =====
static bool configDirty = false;
static unsigned long lastConfigWriteMs = 0;

void requestConfigSave() {
  configDirty = true;
}

void checkPendingConfigSave() {
  if (!configDirty) return;
  if (millis() - lastConfigWriteMs < 500) return;   // 500ms 内的多次操作合并为一次写入
  configDirty = false;
  lastConfigWriteMs = millis();
  StateLockGuard _lg;   // 序列化/落盘期间持锁：HTTP 任务不能同时改 pwmConfigs（dirty 标记是 loop 私有，无需锁）
  saveConfig();
}

// ===================== 序列器配置（独立 NVS key，与 PWM 配置分离）=====================
// 文档容量 IO_CONFIG_DOC_SIZE 定义在 config.h（满配含模式与毫秒时长后需 49152，原 16384 会溢出）
static const char* NVS_KEY_IO = "ioconfig";

bool saveIoConfig() {
  DynamicJsonDocument doc(IO_CONFIG_DOC_SIZE);   // 极端 20 组 × 20 条（含模式与毫秒时长）
  JsonArray groups = doc.createNestedArray("ioGroups");
  for (int g = 0; g < ioGroupCount; g++) {
    // 被标注为 Flash/PSRAM 总线的引脚不写入 NVS（整组连同其条目一起跳过）：
    // 这类引脚一旦在启动时被自动驱动，就会打断 flash 总线导致崩溃重启。
    // 配置仍保留在 RAM 里当场可用，需要留档请用「包含开发引脚的配置导出」。
    if (isIoFlashPin(ioGroups[g].pin)) continue;
    JsonObject gobj = groups.createNestedObject();
    gobj["name"] = ioGroups[g].name;
    gobj["pin"] = ioGroups[g].pin;
    gobj["enabled"] = ioGroups[g].enabled;
    JsonArray arr = gobj.createNestedArray("entries");
    for (int j = 0; j < ioGroups[g].entryCount; j++) {
      const IoEntry& e = ioGroups[g].entries[j];
      JsonObject eobj = arr.createNestedObject();
      // 只写"与读侧默认值不同"的字段。读侧 (applyIoConfig) 对缺失字段的默认值是：
      //   h=0, m=0, a=0, mo=定时开关, pm=1000, hh=500, hl=500
      // 所以这里省略掉的字段，读回来与省略前**完全一致**（无损），而每条能省约 40 字节：
      // 满配 20 组 × 20 条从约 22.7KB 降到约 6.6KB。
      // 注意：改动下面的 1000/500 必须同步改 applyIoConfig 里的默认值，否则会静默改变语义。
      if (e.hour)            eobj["h"]  = e.hour;
      if (e.minute)          eobj["m"]  = e.minute;
      if (e.action)          eobj["a"]  = 1;
      if (e.mode)            eobj["mo"] = e.mode;
      if (e.pulseMs  != 1000) eobj["pm"] = e.pulseMs;
      if (e.hbHighMs != 500)  eobj["hh"] = e.hbHighMs;
      if (e.hbLowMs  != 500)  eobj["hl"] = e.hbLowMs;
    }
  }
  // 手动基准（调度面板设定，跨重启保持）。与序列器同属"调度"配置，所以放在同一个 key 里，
  // 复用同一套防抖写入链路。p=引脚号, l=电平(0/1)。总线引脚不写入，理由同上面的组过滤。
  JsonArray manual = doc.createNestedArray("manual");
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
    if (!manualOverride[p]) continue;
    if (isIoFlashPin(p)) continue;
    JsonObject o = manual.createNestedObject();
    o["p"] = p;
    o["l"] = manualLevel[p] ? 1 : 0;
  }
  String jsonStr;
  serializeJson(doc, jsonStr);

  // 48KB 堆分配失败时 doc 为空，serializeJson 只会产出 "null"。此时绝不能写回 NVS，
  // 否则会把已有配置覆盖成空文档（下次开机整份配置丢失）。用首字符判断比 isNull() 跨版本更稳。
  if (jsonStr.length() == 0 || jsonStr[0] != '{') {
    logf("[NVS] IO配置保存失败：JSON 文档分配/序列化异常（数据=%dB），已保留原配置\n", jsonStr.length());
    noteSaveResult(SAVE_MOD_IO, false);
    return false;
  }

  Preferences prefs;
  unsigned long t0 = millis();
  bool ok = prefs.begin(NVS_NS, false);
  size_t written = ok ? prefs.putBytes(NVS_KEY_IO, jsonStr.c_str(), jsonStr.length()) : 0;
  prefs.end();
  unsigned long dt = millis() - t0;

  if (written > 0) {
    logf("[NVS] IO配置已保存, 数据=%dB, commit耗时=%lums\n", jsonStr.length(), dt);
    noteSaveResult(SAVE_MOD_IO, true);
    return true;
  }
  logf("[NVS] IO配置保存失败! begin=%s, written=%lu\n", ok ? "ok" : "fail", (unsigned long)written);
  noteSaveResult(SAVE_MOD_IO, false);
  return false;
}

void loadIoConfig() {
  String jsonStr = nvsReadJsonFrom(NVS_KEY_IO);

  if (jsonStr.length() == 0) {
    ioGroupCount = 0;
    logRaw("[序列器] 无已有配置\n");
    return;
  }

  DynamicJsonDocument doc(IO_CONFIG_DOC_SIZE);
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    ioGroupCount = 0;
    logf("[序列器] 警告：解析失败 (%s)，使用空配置\n", error.c_str());
    return;
  }

  ioGroupCount = 0;
  applyIoConfig(doc["ioGroups"]);
  applyIoManual(doc["manual"]);     // 手动基准（与组同在 ioconfig 内）
  logf("[序列器] 加载成功：%d 组\n", ioGroupCount);
}

// 应用序列器组配置（先清空计数，再按校验逐组加载；引脚冲突即跳过）
static void applyIoConfig(JsonArray groups) {
  ioGroupCount = 0;
  for (int g = 0; g < groups.size() && ioGroupCount < IO_GROUP_MAX; g++) {
    JsonObject gobj = groups[g];
    const char* name = gobj["name"] | "";
    if (name[0] == '\0') name = "未命名";
    int pin = gobj["pin"] | -1;
    if (!isPinAvailable(pin) || ioIsPinUsedByPwm(pin) || ioIsPinUsedByIo(pin, -1)) {
      continue;   // 引脚非法/冲突（可能引脚映射已变），跳过该组
    }
    IoGroup& grp = ioGroups[ioGroupCount];
    strncpy(grp.name, name, IO_NAME_MAX - 1);
    grp.name[IO_NAME_MAX - 1] = '\0';
    grp.pin = pin;
    grp.enabled = gobj["enabled"] | true;   // 缺省 true，兼容旧配置
    grp.entryCount = 0;
    JsonArray arr = gobj["entries"];
    for (int j = 0; j < arr.size() && grp.entryCount < IO_ENTRY_MAX; j++) {
      JsonObject eobj = arr[j];
      IoEntry& e = grp.entries[grp.entryCount++];
      e.hour = (uint8_t)safeConstrain(eobj["h"] | 0, 0, 23);
      e.minute = (uint8_t)safeConstrain(eobj["m"] | 0, 0, 59);
      e.action = (eobj["a"] | 0) ? 1 : 0;
      // 模式：白名单映射。缺字段 → 定时开关，即改造前的纯"时:分 + 开/关"行为
      e.mode = (uint8_t)ioNormalizeMode(eobj["mo"] | IO_MODE_SCHEDULE);
      // 时长：缺字段给有意义的默认值（避免误切模式后得到 0ms 疯狂闪烁）；
      // 读侧同样夹紧上下限，不依赖写入侧（counter 的锁存时长读侧此前缺上限保护，已按同口径补齐）
      e.pulseMs = ioClampDurationMs((uint32_t)(eobj["pm"] | 1000));
      e.hbHighMs = ioClampDurationMs((uint32_t)(eobj["hh"] | 500));
      e.hbLowMs = ioClampDurationMs((uint32_t)(eobj["hl"] | 500));
      e.conflict = false;
    }
    ioGroupCount++;
  }
  ioRecalcConflicts();
  // 必须清一次暂态：静态数组是零初始化的，若不在这里把 ioLastMinOfDay 置回 0xFFFF 哨兵，
  // 开机会走"分钟已推进 nowMin 分钟"的分支 —— nowMin<=5 时会补发当天已过时间的脉冲，
  // nowMin>5 时每个组都会误报一条"时间跳变"日志。这里同时让下一轮全量重算输出。
  ioResetOutput();
}

// 恢复手动基准（ioconfig 的 manual 段）。只恢复非总线引脚 —— 与写入侧的过滤形成双保险，
// 防止换型号或导入外部文件把总线引脚的手动值带回来（那会在启动时打断 flash 总线）。
static void applyIoManual(JsonArray manual) {
  // 先清旧值。注意不能用 manualClearPin/manualClearAll：它们会对引脚做 pinMode / pwmRestore，
  // 若旧值里含有总线引脚就会当场崩。这里只动标志位。
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
    manualOverride[p] = false;
    manualLevel[p] = 0;
  }
  for (int i = 0; i < manual.size(); i++) {
    int p = manual[i]["p"] | -1;
    if (!isPinAvailable(p) || isIoFlashPin(p)) continue;
    if (latchEnabled[p]) continue;      // 已锁存：锁存优先级更高，不覆盖
    int lv = (manual[i]["l"] | 0) ? 1 : 0;
    manualOverride[p] = true;
    manualLevel[p] = lv;
    // 立即驱动：没有序列器组的引脚不会有人替它写这个电平（组引脚由 updateIoOutput 负责）
    pinMode(p, OUTPUT);
    digitalWrite(p, lv ? HIGH : LOW);
  }
}

// IO 配置防抖合并写入
static bool ioConfigDirty = false;
static unsigned long lastIoConfigWriteMs = 0;

void requestIoConfigSave() {
  ioConfigDirty = true;
}

void checkPendingIoConfigSave() {
  if (!ioConfigDirty) return;
  if (millis() - lastIoConfigWriteMs < 500) return;
  ioConfigDirty = false;
  lastIoConfigWriteMs = millis();
  StateLockGuard _lg;   // 序列化/落盘期间持锁：HTTP 任务不能同时改 ioGroups / manual
  saveIoConfig();
}

// ===================== 比较器配置（独立 NVS key，与 PWM/IO 配置分离）=====================
static const char* NVS_KEY_COMPARATOR = "compconfig";

// ===================== 计数器配置（独立 NVS key）=====================
static const char* NVS_KEY_COUNTER = "counterconfig";

// 天气定位配置 key（独立 NVS key，中文名按 UTF-8 存储；「清除配置」需一并清除，故定义前移）
static const char* NVS_KEY_WEATHER = "weather";

// 接口转发地址 key（独立 NVS key，天气抓取第三级兜底；「清除配置」需一并清除，故定义前移）
static const char* NVS_KEY_RELAY = "relay";

// 时区配置 key（独立 NVS key，存 mode + offset；「清除配置」需一并清除，故定义前移）
static const char* NVS_KEY_TZ = "tz";

// 芯片型号固定 key（独立 NVS key；「清除配置」需一并清除，故定义前移）
static const char* NVS_KEY_CHIP = "chipmodel";

// 时钟配置 key（独立 NVS key；「清除配置」需一并清除）
static const char* NVS_KEY_CLOCK = "clocks";

// 天气系数过渡时长 key（独立 NVS key；「清除配置」需一并清除，故定义前移）
static const char* NVS_KEY_WX_FADE = "wxfade";

bool saveComparatorConfig() {
  DynamicJsonDocument doc(8192);
  JsonArray gates = doc.createNestedArray("comparatorGates");
  for (int g = 0; g < comparatorGateCount; g++) {
    JsonObject gobj = gates.createNestedObject();
    // 与序列器同一套压缩方案：只写"与读侧默认值不同"的字段。读侧默认值见 applyComparatorConfig：
    //   enabled=true、mode=AND(0)、输入 src=0(IO组)/act=0/exp=1、动作 slv=0/lvl=0。
    // 改动这里的默认值必须同步改 applyComparatorConfig。
    gobj["name"] = comparatorGates[g].name;
    if (!comparatorGates[g].enabled) gobj["enabled"] = false;
    if (comparatorGates[g].mode)     gobj["mode"] = comparatorGates[g].mode;
    JsonArray inputs = gobj.createNestedArray("inputs");
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      // 只有"外部引脚"类型的输入，refIndex 才是真实 GPIO（IO/计数器类型是下标，不能过滤）。
      // 总线引脚启动时会被配成输入而打断 flash 总线，所以必须跳过。
      if (comparatorGates[g].inputs[i].source == COMPARATOR_SRC_EXTERNAL &&
          isIoFlashPin(comparatorGates[g].inputs[i].refIndex)) continue;
      JsonObject iobj = inputs.createNestedObject();
      if (comparatorGates[g].inputs[i].source)          iobj["src"] = comparatorGates[g].inputs[i].source;
      iobj["ref"] = comparatorGates[g].inputs[i].refIndex;   // 默认 -1 是无效哨兵，必须始终写
      if (comparatorGates[g].inputs[i].actionIndex)     iobj["act"] = comparatorGates[g].inputs[i].actionIndex;
      if (!comparatorGates[g].inputs[i].expectedLevel)  iobj["exp"] = 0;
    }
    JsonArray actions = gobj.createNestedArray("actions");
    for (int a = 0; a < comparatorGates[g].actionCount; a++) {
      if (isIoFlashPin(comparatorGates[g].actions[a].pin)) continue;   // 总线引脚不写入 NVS
      JsonObject aobj = actions.createNestedObject();
      aobj["pin"] = comparatorGates[g].actions[a].pin;   // 默认 -1 必须始终写
      if (comparatorGates[g].actions[a].sourceLevel) aobj["slv"] = 1;
      if (comparatorGates[g].actions[a].actionLevel) aobj["lvl"] = 1;
    }
  }
  String jsonStr;
  serializeJson(doc, jsonStr);

  Preferences prefs;
  unsigned long t0 = millis();
  bool ok = prefs.begin(NVS_NS, false);
  size_t written = ok ? prefs.putBytes(NVS_KEY_COMPARATOR, jsonStr.c_str(), jsonStr.length()) : 0;
  prefs.end();
  unsigned long dt = millis() - t0;

  if (written > 0) {
    logf("[NVS] 比较器配置已保存, 数据=%dB, commit耗时=%lums\n", jsonStr.length(), dt);
    noteSaveResult(SAVE_MOD_COMPARATOR, true);
    return true;
  }
  logf("[NVS] 比较器配置保存失败! begin=%s, written=%lu\n", ok ? "ok" : "fail", (unsigned long)written);
  noteSaveResult(SAVE_MOD_COMPARATOR, false);
  return false;
}

void loadComparatorConfig() {
  String jsonStr = nvsReadJsonFrom(NVS_KEY_COMPARATOR);

  if (jsonStr.length() == 0) {
    comparatorGateCount = 0;
    logRaw("[比较器] 无已有配置\n");
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    comparatorGateCount = 0;
    logf("[比较器] 警告：解析失败 (%s)，使用空配置\n", error.c_str());
    return;
  }

  applyComparatorConfig(doc["comparatorGates"]);
  logf("[比较器] 加载成功：%d 个\n", comparatorGateCount);
}

// 应用比较器配置（先清空计数，再按校验逐门加载；引用失效/引脚冲突即跳过）
static void applyComparatorConfig(JsonArray gates) {
  comparatorGateCount = 0;
  for (int g = 0; g < gates.size() && comparatorGateCount < COMPARATOR_GATE_MAX; g++) {
    JsonObject gobj = gates[g];
    ComparatorGate& gate = comparatorGates[comparatorGateCount];
    const char* name = gobj["name"] | "";
    if (name[0] == '\0') name = "未命名";
    strncpy(gate.name, name, IO_NAME_MAX - 1);
    gate.name[IO_NAME_MAX - 1] = '\0';
    gate.enabled = gobj["enabled"] | true;   // 缺省 true，兼容旧配置
    gate.mode = gobj["mode"] | COMPARATOR_MODE_AND;   // 缺省 AND，兼容旧配置
    gate.inputCount = 0;
    gate.actionCount = 0;

    JsonArray inputs = gobj["inputs"];
    for (int i = 0; i < inputs.size() && gate.inputCount < COMPARATOR_INPUT_MAX; i++) {
      JsonObject iobj = inputs[i];
      // 兼容旧数据：旧版用 ext(0=IO组,1=外部输入)；新版用 src(0=IO组,1=计数器动作,2=外部输入)
      int src;
      if (iobj.containsKey("src")) {
        src = iobj["src"] | 0;
      } else {
        src = (iobj["ext"] | 0) != 0 ? COMPARATOR_SRC_EXTERNAL : COMPARATOR_SRC_IO;
      }
      int ref = iobj["ref"] | -1;
      int act = iobj["act"] | 0;
      int exp = iobj["exp"] | 1;   // 兼容旧数据：缺省默认高电平
      if (src == COMPARATOR_SRC_EXTERNAL) {
        // 外部输入：校验可用性，避免脏数据
        if (!isPinAvailable(ref) || isIoFlashPin(ref) ||
            ioIsPinUsedByPwm(ref) || ioIsPinUsedByIo(ref, -1) ||
            comparatorIsPinUsedByOutput(ref, -1)) continue;
        // 只读输入优先：旧版本/手工导入的脏数据里若残留手动基准，会让读数被手动值接管，这里清掉
        if (manualOverride[ref]) { manualOverride[ref] = false; manualLevel[ref] = 0; logf("[比较器] GPIO%d 恢复为外部输入：清除残留手动基准\n", ref); }
        pinMode(ref, INPUT);
      } else if (src == COMPARATOR_SRC_IO) {
        if (ref < 0 || ref >= ioGroupCount) continue;   // 引用失效则丢弃
      } else if (src == COMPARATOR_SRC_COUNTER) {
        if (ref < 0 || ref >= counterTaskCount || act < 0 || act >= counterTasks[ref].actionCount) continue;
      } else {
        continue;
      }
      ComparatorInput& in = gate.inputs[gate.inputCount++];
      in.source = src;
      in.refIndex = ref;
      in.actionIndex = (src == COMPARATOR_SRC_COUNTER) ? act : 0;
      in.expectedLevel = exp ? 1 : 0;
    }

    JsonArray actions = gobj["actions"];
    for (int a = 0; a < actions.size() && gate.actionCount < COMPARATOR_ACTION_MAX; a++) {
      JsonObject aobj = actions[a];
      int pin = aobj["pin"] | -1;
      int slv = aobj["slv"] | 0;   // 源IO电平，旧数据缺省 0（低）
      int lvl = aobj["lvl"] | 0;   // 行为电平
      if (!isPinAvailable(pin) || isIoFlashPin(pin) ||
          ioIsPinUsedByPwm(pin) || ioIsPinUsedByIo(pin, -1) ||
          comparatorIsPinUsedByOutput(pin, -1) || comparatorIsPinUsedByExternalInput(pin, -1)) continue;
      ComparatorAction& act = gate.actions[gate.actionCount++];
      act.pin = pin;
      act.sourceLevel = slv ? 1 : 0;
      act.actionLevel = lvl ? 1 : 0;
    }

    comparatorGateCount++;
  }
}

// 比较器配置防抖合并写入
static bool comparatorConfigDirty = false;
static unsigned long lastComparatorConfigWriteMs = 0;

void requestComparatorConfigSave() {
  comparatorConfigDirty = true;
}

void checkPendingComparatorConfigSave() {
  if (!comparatorConfigDirty) return;
  if (millis() - lastComparatorConfigWriteMs < 500) return;
  comparatorConfigDirty = false;
  lastComparatorConfigWriteMs = millis();
  StateLockGuard _lg;   // 序列化/落盘期间持锁：HTTP 任务不能同时改 comparatorGates
  saveComparatorConfig();
}

// ===================== 计数器配置（独立 NVS key）=====================

bool saveCounterConfig() {
  DynamicJsonDocument doc(8192);
  JsonArray tasks = doc.createNestedArray("counterTasks");
  for (int t = 0; t < counterTaskCount; t++) {
    JsonObject tobj = tasks.createNestedObject();
    // 与序列器同一套压缩方案：只写"与读侧默认值不同"的字段。读侧默认值见 applyCounterConfig：
    //   enabled=true、threshold=1、动作 level=1/holdMode=KEEP(0)/holdSec=0。
    // 改动这里的默认值必须同步改 applyCounterConfig。
    tobj["name"] = counterTasks[t].name;
    if (!counterTasks[t].enabled) tobj["enabled"] = false;
    // 总线引脚不能作为触发源写入：启动时会被配成输入而打断 flash 总线。-1 表示无源。
    // 默认值就是 -1（无效哨兵），所以必须始终写。
    tobj["triggerPin"] = isIoFlashPin(counterTasks[t].triggerPin) ? -1 : counterTasks[t].triggerPin;
    if (counterTasks[t].threshold != 1) tobj["threshold"] = counterTasks[t].threshold;
    JsonArray actions = tobj.createNestedArray("actions");
    for (int a = 0; a < counterTasks[t].actionCount; a++) {
      if (isIoFlashPin(counterTasks[t].actions[a].pin)) continue;   // 总线引脚不写入 NVS
      JsonObject aobj = actions.createNestedObject();
      aobj["pin"] = counterTasks[t].actions[a].pin;                 // 默认 -1 必须始终写
      if (!counterTasks[t].actions[a].level)    aobj["level"] = 0;
      if (counterTasks[t].actions[a].holdMode)  aobj["holdMode"] = counterTasks[t].actions[a].holdMode;
      if (counterTasks[t].actions[a].holdSec)   aobj["holdSec"] = counterTasks[t].actions[a].holdSec;
    }
  }
  String jsonStr;
  serializeJson(doc, jsonStr);

  Preferences prefs;
  unsigned long t0 = millis();
  bool ok = prefs.begin(NVS_NS, false);
  size_t written = ok ? prefs.putBytes(NVS_KEY_COUNTER, jsonStr.c_str(), jsonStr.length()) : 0;
  prefs.end();
  unsigned long dt = millis() - t0;

  if (written > 0) {
    logf("[NVS] 计数器配置已保存, 数据=%dB, commit耗时=%lums\n", jsonStr.length(), dt);
    noteSaveResult(SAVE_MOD_COUNTER, true);
    return true;
  }
  logf("[NVS] 计数器配置保存失败! begin=%s, written=%lu\n", ok ? "ok" : "fail", (unsigned long)written);
  noteSaveResult(SAVE_MOD_COUNTER, false);
  return false;
}

void loadCounterConfig() {
  String jsonStr = nvsReadJsonFrom(NVS_KEY_COUNTER);

  if (jsonStr.length() == 0) {
    counterTaskCount = 0;
    logRaw("[计数器] 无已有配置\n");
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    counterTaskCount = 0;
    logf("[计数器] 警告：解析失败 (%s)，使用空配置\n", error.c_str());
    return;
  }

  applyCounterConfig(doc["counterTasks"]);
  logf("[计数器] 加载成功：%d 个\n", counterTaskCount);
}

// 应用计数器配置（先清空计数，再按校验逐任务加载；触发源/动作引脚冲突即跳过）
static void applyCounterConfig(JsonArray tasks) {
  counterTaskCount = 0;
  for (int t = 0; t < tasks.size() && counterTaskCount < COUNTER_TASK_MAX; t++) {
    JsonObject tobj = tasks[t];
    CounterTask& task = counterTasks[counterTaskCount];
    const char* name = tobj["name"] | "";
    if (name[0] == '\0') name = "未命名";
    strncpy(task.name, name, IO_NAME_MAX - 1);
    task.name[IO_NAME_MAX - 1] = '\0';
    task.enabled = tobj["enabled"] | true;   // 缺省 true，兼容旧配置

    int triggerPin = tobj["triggerPin"] | -1;
    int threshold = safeConstrain(tobj["threshold"] | 1, 1, 1000000);
    if (triggerPin >= 0) {
      // 触发源校验：可用 + 非 Flash/PSRAM + 非 PWM（脏数据则失效）
      if (!isPinAvailable(triggerPin) || isIoFlashPin(triggerPin) || ioIsPinUsedByPwm(triggerPin)) {
        triggerPin = -1;
      } else {
        // 触发源（读）可与序列器/比较器输出共享：被驱动时不改输入，避免打断输出
        if (!ioIsPinUsedByIo(triggerPin, -1) && !comparatorIsPinUsedByOutput(triggerPin, -1)) {
          // 只读输入优先：清掉脏数据里残留的手动基准，否则读数被手动值接管、外部信号读不到
          if (manualOverride[triggerPin]) { manualOverride[triggerPin] = false; manualLevel[triggerPin] = 0; logf("[计数器] GPIO%d 恢复为触发源：清除残留手动基准\n", triggerPin); }
          pinMode(triggerPin, INPUT);
        }
      }
    }
    task.triggerPin = triggerPin;
    task.threshold = threshold;
    task.actionCount = 0;
    task.triggerCount = 0;
    task.fired = false;
    for (int a = 0; a < COUNTER_ACTION_MAX; a++) task.autoOffUntil[a] = 0;

    JsonArray actions = tobj["actions"];
    for (int a = 0; a < actions.size() && task.actionCount < COUNTER_ACTION_MAX; a++) {
      JsonObject aobj = actions[a];
      int pin = aobj["pin"] | -1;
      int level = (aobj["level"] | 1) ? 1 : 0;
      int holdMode = (aobj["holdMode"] | 0) == COUNTER_HOLD_AUTO_OFF ? COUNTER_HOLD_AUTO_OFF : COUNTER_HOLD_KEEP;
      uint32_t holdSec = (uint32_t)(aobj["holdSec"] | 0);
      // 读侧与写入侧同口径：超过上限（10079 分 60 秒 = 604800 秒 = 168 小时）一律取上限值
      if (holdSec > COUNTER_HOLD_MAX_SEC) holdSec = COUNTER_HOLD_MAX_SEC;
      if (pin < 0 || pin == task.triggerPin) continue;
      // 同一任务内重复动作脚：脏数据跳过（两条动作写同一脚会互相覆盖，删一条会废掉另一条，PIN-8）
      if (counterTaskHasActionPin(counterTaskCount, pin, -1)) { logf("[计数器] GPIO%d 已在任务动作中，跳过重复项\n", pin); continue; }
      // 动作引脚写者互斥校验（脏数据跳过）
      if (!isPinAvailable(pin) || isIoFlashPin(pin) ||
          ioIsPinUsedByPwm(pin) || ioIsPinUsedByIo(pin, -1) ||
          comparatorIsPinUsedByOutput(pin, -1) || comparatorIsPinUsedByExternalInput(pin, -1) ||
          counterIsPinUsedByAction(pin, -1)) continue;
      CounterAction& act = task.actions[task.actionCount];
      act.pin = pin;
      act.level = level;
      act.holdMode = holdMode;
      act.holdSec = holdSec;
      task.actionCount++;
    }

    counterTaskCount++;
  }
}

// 计数器配置防抖合并写入
static bool counterConfigDirty = false;
static unsigned long lastCounterConfigWriteMs = 0;

void requestCounterConfigSave() {
  counterConfigDirty = true;
}

void checkPendingCounterConfigSave() {
  if (!counterConfigDirty) return;
  if (millis() - lastCounterConfigWriteMs < 500) return;
  counterConfigDirty = false;
  lastCounterConfigWriteMs = millis();
  StateLockGuard _lg;   // 序列化/落盘期间持锁：HTTP 任务不能同时改 counterTasks
  saveCounterConfig();
}

// ===================== 全局配置导入（跨模块）=====================

// 应用导入的时区配置（字段名与 NVS 存储格式一致）。校验与 handleSaveTz 同一套：
// 范围 UTC-12~+14、粒度 15 分钟；非法值整个忽略，绝不写进 NVS。
static void applyTzConfig(JsonObject o) {
  bool autoMode = (strcmp(o["mode"] | "auto", "auto") == 0);
  long offset = o["offset"] | (long)tzOffsetSec;
  if (!autoMode) {
    if (offset < -12L * 3600L || offset > 14L * 3600L) {
      logf("[导入] 时区偏移越界(%ld)，忽略\n", offset);
      return;
    }
    if (offset % 900L != 0) {
      logf("[导入] 时区分钟非 15 的倍数(%ld)，忽略\n", offset);
      return;
    }
  }
  saveTzConfig(autoMode, (int32_t)offset);
  timeSetTzOffset((int32_t)offset);
}

// 应用导入的天气定位配置（字段名与 NVS 存储格式一致，全字段可读）
static void applyWeatherConfig(JsonObject o) {
  bool manual = (strcmp(o["mode"] | "auto", "manual") == 0);
  saveWeatherConfig(manual,
                    o["provCode"] | "", o["cityCode"] | "", o["distCode"] | "",
                    o["prov"] | "", o["city"] | "", o["dist"] | "");
}

// 应用导入的接口转发地址（字段名与 NVS 存储格式一致）。
// 必须重新校验：备份文件是可以被手工编辑的，不能因为"是自己刚导出的"就免检 ——
// 一旦塞进超长地址，weatherDoForecast 里的 URL 拼接会静默截断（不报错、只是查不到）。
static void applyRelayConfig(JsonObject o) {
  String url = o["url"] | "";
  url.trim();
  String err = relayUrlCheck(url);
  if (err.length() > 0) {
    logf("[导入] 接口转发地址非法（%s），忽略\n", err.c_str());
    return;
  }
  saveRelayConfig(url.c_str());
}

// 应用导入的天气系数过渡时长（字段名与 NVS 存储格式一致）。范围由 saveWxFadeConfig 内部夹紧。
static void applyWxFadeConfig(JsonObject o) {
  int sec = o["sec"] | wxFadeSec();
  saveWxFadeConfig(sec);
}

// 导入全局配置：解析 JSON 后按开机加载顺序应用 PWM -> IO -> 计数器 -> 比较器，
// 最后逐模块写回 NVS。WiFi 凭据不在导出文件中，导入时保持不变。
// 为兼容旧导出文件（仅含 PWM），各模块仅在 JSON 中存在对应段时才覆盖。
bool importGlobalConfig(const char* json, String& err) {
  DynamicJsonDocument doc(49152);
  DeserializationError error = deserializeJson(doc, json);
  if (error) { err = String("配置解析失败: ") + error.c_str(); return false; }

  // 各 apply* 均带合法性/冲突校验，非法项会自动跳过；均不触碰 wifiSsid/wifiPassword。
  bool ok = true;
  bool pwmTouched = false;

  // 时钟优先应用（PWM 条目引用 clockIndex 需先有时钟）
  if (doc.containsKey("clocks")) { applyClockConfig(doc["clocks"]); ok = saveClockConfig() && ok; }

  // PWM：显式提供 pins 时才重设通道集合（旧导出文件不含 pins，保持现有通道不动）。
  if (doc.containsKey("pins"))     { applyPins(doc["pins"]);             pwmTouched = true; }
  if (doc.containsKey("pwmConfigs")) { applyPwmConfigs(doc["pwmConfigs"]); pwmTouched = true; }
  if (pwmTouched) ok = saveConfig() && ok;

  // 其余模块按开机顺序应用：IO -> 计数器 -> 比较器（比较器可引用计数器动作）。
  // 手动基准与序列器同在 ioconfig key 内，必须在 saveIoConfig() 之前恢复，否则不会被写回。
  if (doc.containsKey("manual"))   { applyIoManual(doc["manual"]); }
  if (doc.containsKey("ioGroups")) { applyIoConfig(doc["ioGroups"]); }
  if (doc.containsKey("manual") || doc.containsKey("ioGroups")) ok = saveIoConfig() && ok;
  if (doc.containsKey("counterTasks")) { applyCounterConfig(doc["counterTasks"]); ok = saveCounterConfig() && ok; }
  if (doc.containsKey("comparatorGates")) { applyComparatorConfig(doc["comparatorGates"]); ok = saveComparatorConfig() && ok; }

  // 时区、天气定位、接口转发：三者均无 RAM 镜像，直接在 apply 内落 NVS。
  // 老导出文件不含这几段，靠 containsKey 自动跳过，保持向后兼容。
  if (doc.containsKey("tz"))      { applyTzConfig(doc["tz"]); }
  if (doc.containsKey("weather")) { applyWeatherConfig(doc["weather"]); }
  if (doc.containsKey("relay"))   { applyRelayConfig(doc["relay"]); }
  if (doc.containsKey("wxFade"))  { applyWxFadeConfig(doc["wxFade"]); }

  if (!ok) { err = "配置保存失败(NVS 损坏或已满)"; return false; }
  return true;
}

// ===================== 恢复出厂 / 重置配置 =====================

// 一次性清单 key：重置时记录"当时被固件占用的引脚"，重启后由 applyResetPinDefaults()
// 读出来主动驱动为低电平，读完立即删除。清单只收被固件驱动为输出的引脚（只读输入脚不采集）。
static const char* NVS_KEY_RESET_PINS = "resetpins";

// 该引脚是否正被固件驱动为输出（PWM / 序列器 / 比较器输出动作 / 计数器输出动作 / 手动 / 锁存）
static bool pinDrivenByConfig(int pin) {
  for (int i = 0; i < pwmChannelCount; i++) if (pwmPins[i] == pin) return true;
  for (int g = 0; g < ioGroupCount; g++) if (ioGroups[g].pin == pin) return true;
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int a = 0; a < comparatorGates[g].actionCount; a++) {
      if (comparatorGates[g].actions[a].pin == pin) return true;
    }
  }
  for (int t = 0; t < counterTaskCount; t++) {
    for (int a = 0; a < counterTasks[t].actionCount; a++) {
      if (counterTasks[t].actions[a].pin == pin) return true;
    }
  }
  return manualOverride[pin] || latchEnabled[pin];
}

// 该引脚在本次启动的配置里是否仍被占用（含只读输入型计数器触发源）；仍占用就不动它，避免与模块抢电平。
static bool pinUsedByConfig(int pin) {
  if (pinDrivenByConfig(pin)) return true;
  for (int t = 0; t < counterTaskCount; t++) if (counterTasks[t].triggerPin == pin) return true;
  return false;
}

// 采集「重置前被固件驱动为输出」的引脚清单（逗号分隔，已去重并剔除 Flash 总线引脚）。
// 只读输入脚（比较器外部输入、纯输入型触发源）不采集：固件从未驱动它们，软复位后本就是输入态，
// 拉成输出低会与外部器件对驱（PIN-3）。
static String collectUsedPins() {
  String list;
  for (int i = 0; i < getAvailablePinCount(); i++) {
    int pin = getAvailablePin(i);
    if (pin < 0 || !pinDrivenByConfig(pin)) continue;
    if (isIoFlashPin(pin)) continue;
    if (list.length()) list += ",";
    list += String(pin);
  }
  return list;
}

// 释放单个引脚到芯片复位态，并记录到 released（用于串口核对到底放了哪些脚）
static void releasePinDefault(int pin, String& released) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return;
  if (isIoFlashPin(pin)) {
    logf("[重置] 跳过 Flash 总线引脚 GPIO%d\n", pin);
    return;
  }
  gpio_reset_pin((gpio_num_t)pin);
  if (released.length()) released += ", ";
  released += "GPIO" + String(pin);
}

// 重置收尾：把「即将被清除的配置所占用的引脚」逐个恢复为芯片复位态（输入、高阻、无上下拉）。
//
// 调用位置有硬性要求：必须在 NVS 已经清除成功之后。这样即使这里某个引脚操作崩溃，
// 重置结果也已经落盘，重启后依然是空配置 —— 重置永远不会失败。旧实现把同样的动作放在
// 清 NVS 之前，一旦崩溃就前功尽弃，配置永远删不掉。
//
// 所有引脚一律先过 isIoFlashPin()：Flash/PSRAM 总线引脚被 gpio_reset_pin 从 SPI 功能切回
// GPIO 后，片内 Flash 立即失联 panic。
static void releasePinsForReset() {
  pwmReleaseAllForReset();   // PWM：停 LEDC + 复位 pad（内部已过滤 Flash 引脚并打印）

  String released;
  // 序列器组引脚
  for (int g = 0; g < ioGroupCount; g++) releasePinDefault(ioGroups[g].pin, released);

  // 比较器输出动作引脚
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int a = 0; a < comparatorGates[g].actionCount; a++) {
      releasePinDefault(comparatorGates[g].actions[a].pin, released);
    }
  }

  // 计数器输出动作引脚
  for (int t = 0; t < counterTaskCount; t++) {
    for (int a = 0; a < counterTasks[t].actionCount; a++) {
      releasePinDefault(counterTasks[t].actions[a].pin, released);
    }
  }

  // 手动覆盖 / 锁存：连同 RAM 状态一起清掉，避免重启前 loop 再按旧状态驱动引脚
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
    if (!manualOverride[p] && !latchEnabled[p]) continue;
    manualOverride[p] = false;
    manualLevel[p] = 0;
    latchEnabled[p] = false;
    latchLevel[p] = 0;
    releasePinDefault(p, released);
  }

  // 序列器暂态（脉冲 deadline / 心跳相位）同样要清：NVS 已清空但 loop 仍会按旧暂态驱动引脚
  ioResetOutput();

  logf("[重置] 引脚收尾：已释放 %s\n", released.length() ? released.c_str() : "(无)");
}

// 重置后的引脚收尾（setup() 中、initPWM() 之前调用）：把重置前占用的引脚主动驱动为低。
//
// 为什么必须放在重启之后：ESP32-S3 的 CPU 软复位（ESP.restart()）不会复位 IO 焊盘，
// 重置前被 LEDC/GPIO 拉高的引脚会跨复位继续保持高电平。实测可证：冷启动（POWERON）
// 时该脚为默认态（LED 灭），而软件复位后仍为高 —— 说明重启前把引脚放回高阻
// （releasePinsForReset）不足以让引脚回到默认电平，只有重启后显式拉低才能确定性地复位。
//
// 清单由重置函数在清 NVS 之前采集、落盘到 NVS_KEY_RESET_PINS，本函数读完即删（一次性）。
void applyResetPinDefaults() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return;
  String list = prefs.getString(NVS_KEY_RESET_PINS, "");
  prefs.end();
  if (!list.length()) return;

  Preferences wprefs;
  bool hasW = wprefs.begin(NVS_NS, false);

  int applied = 0;
  int from = 0;
  while (from <= (int)list.length()) {
    int comma = list.indexOf(',', from);
    String token = (comma < 0) ? list.substring(from) : list.substring(from, comma);
    from = (comma < 0) ? list.length() + 1 : comma + 1;
    token.trim();
    if (!token.length()) continue;
    int pin = token.toInt();
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) continue;
    if (isIoFlashPin(pin)) {
      logf("[重置] 默认电平跳过 Flash 总线引脚 GPIO%d\n", pin);
      continue;
    }
    if (pinUsedByConfig(pin)) {
      logf("[重置] 默认电平跳过已占用引脚 GPIO%d\n", pin);
      continue;
    }
    gpio_hold_dis((gpio_num_t)pin);   // 清掉可能遗留的焊盘保持
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    applied++;
    logf("[重置] 默认电平: GPIO%d -> 低\n", pin);
  }
  if (hasW) {
    // 全部应用完才删除清单：中途若异常，下次启动仍会重新拉低，保证结果确定
    wprefs.remove(NVS_KEY_RESET_PINS);
    wprefs.end();
  }
  logf("[重置] 引脚默认电平已应用：%d 个（清单 %s）\n", applied, list.c_str());
}

// 恢复出厂设置：清除整个命名空间的所有配置（含 WiFi），随后由调用方重启。
//
// 两步顺序不能颠倒：
//   1) 清 NVS —— 决定重置结果，必须先落盘（先把占用引脚清单写进 NVS_KEY_RESET_PINS）；
//   2) releasePinsForReset() —— 把旧配置占用的引脚放回芯片复位态。
// 第 2 步放在第 1 步之后，所以即便某个引脚操作崩溃，也只是这次收尾没做完，
// 重启后配置依然是空的，重置不会失效。
// 引脚电平的最终复位由重启后的 applyResetPinDefaults() 完成（软复位不复位 IO 焊盘）。
bool factoryReset() {
  String usedPins = collectUsedPins();   // 必须在清 NVS 之前采集（清完就无从得知了）
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) {
    logf("[重置] 恢复出厂失败：NVS 打开失败\n");
    return false;
  }
  prefs.clear();   // 清除 smartglass 命名空间下所有 key（含 WiFi）
  bool cleared = !prefs.isKey(NVS_KEY_JSON);   // 读回校验，确认配置确实已清掉
  if (cleared && usedPins.length()) {
    prefs.putString(NVS_KEY_RESET_PINS, usedPins);   // 落盘清单，供重启后驱动为默认低
  }
  prefs.end();
  if (!cleared) {
    logf("[重置] 恢复出厂失败：配置未被清除\n");
    return false;
  }
  logRaw("[重置] 已清除全部用户配置（含 WiFi）\n");
  releasePinsForReset();   // NVS 已落盘，再做引脚收尾
  return true;
}

// 重置配置（除 WiFi 外）：清除 IO/比较器/计数器/天气/时区/芯片/时钟配置，PWM 清空，保留 WiFi 凭据。
//
// 顺序与 factoryReset() 一致：先清 NVS（决定结果），再 releasePinsForReset() 放引脚。
// 引脚收尾内部会跳过 Flash/PSRAM 总线引脚，所以不会再出现"操作总线引脚导致
// 在清 NVS 之前 panic、重置永远失败"的老问题。
bool resetConfigKeepWifi() {
  String usedPins = collectUsedPins();   // 必须在清 NVS 之前采集
  // 1) 清除各模块独立 NVS key（IO/比较器/计数器/天气/芯片/时钟）
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) {
    logf("[重置] 重置配置失败：NVS 打开失败\n");
    return false;
  }
  prefs.remove(NVS_KEY_IO);
  prefs.remove(NVS_KEY_COMPARATOR);
  prefs.remove(NVS_KEY_COUNTER);
  prefs.remove(NVS_KEY_WEATHER);
  prefs.remove(NVS_KEY_RELAY);
  prefs.remove(NVS_KEY_TZ);
  prefs.remove(NVS_KEY_CHIP);
  prefs.remove(NVS_KEY_CLOCK);
  prefs.remove(NVS_KEY_WX_FADE);

  // 2) 重写 config key：保留 WiFi，PWM 引脚与通道清空（空数组 = 0 通道）
  DynamicJsonDocument doc(2048);
  doc.createNestedArray("pins");          // 空数组
  doc["wifi"]["ssid"] = wifiSsid;
  doc["wifi"]["password"] = wifiPassword;
  doc.createNestedArray("pwmConfigs");    // 空数组
  String jsonStr;
  serializeJson(doc, jsonStr);

  // 3) 写入并读回校验（复用同一个 NVS 句柄，避免同命名空间重复打开）
  size_t written = prefs.putBytes(NVS_KEY_JSON, jsonStr.c_str(), jsonStr.length());
  bool clearedOk = written > 0 && !prefs.isKey(NVS_KEY_IO);   // 顺带确认模块 key 已删净
  if (written > 0 && clearedOk && usedPins.length()) {
    prefs.putString(NVS_KEY_RESET_PINS, usedPins);   // 落盘清单，供重启后驱动为默认低
  }
  prefs.end();

  if (written > 0 && clearedOk) {
    logRaw("[重置] 已重置配置（保留 WiFi）\n");
    releasePinsForReset();   // NVS 已落盘，再做引脚收尾
    return true;
  }
  logf("[重置] 重置配置失败：写入=%lu, 模块key已清除=%d\n",
       (unsigned long)written, clearedOk ? 1 : 0);
  return false;
}

// ===================== 芯片型号固定 ======================
const char* const CHIP_MODELS[] = {
  "ESP32", "ESP32-H4", "ESP32-C2", "ESP32-C3", "ESP32-C5", "ESP32-C6",
  "ESP32-C61", "ESP32-H2", "ESP32-S2", "ESP32-S3", "ESP32-P4", "ESP32-S31"
};
const int CHIP_MODEL_COUNT = sizeof(CHIP_MODELS) / sizeof(CHIP_MODELS[0]);

bool isValidChipModel(const char* model) {
  if (!model || model[0] == '\0') return false;
  for (int i = 0; i < CHIP_MODEL_COUNT; i++) {
    if (strcmp(CHIP_MODELS[i], model) == 0) return true;
  }
  return false;
}

// 生效型号：固定值优先，否则返回芯片自动识别结果
const char* getEffectiveChipModel() {
  if (forcedChipModel.length() > 0) return forcedChipModel.c_str();
  return ESP.getChipModel();
}

void loadChipConfig() {
  Preferences prefs;
  prefs.begin(NVS_NS, true);
  String saved = prefs.getString(NVS_KEY_CHIP, "");
  prefs.end();
  if (saved.length() > 0 && isValidChipModel(saved.c_str())) {
    forcedChipModel = saved;
    logf("[核心] 型号固定：%s（自动识别为 %s）\n", saved.c_str(), ESP.getChipModel());
  } else {
    forcedChipModel = "";
  }
}

bool setForcedChipModel(const char* model) {
  if (!isValidChipModel(model)) {
    logf("[核心] 固定失败：型号 %s 不在支持列表\n", model ? model : "(空)");
    return false;
  }
  Preferences prefs;
  bool ok = prefs.begin(NVS_NS, false);
  size_t written = ok ? prefs.putString(NVS_KEY_CHIP, model) : 0;
  prefs.end();
  if (written == 0) {
    logf("[核心] 固定失败：NVS 写入失败\n");
    return false;
  }
  forcedChipModel = model;
  logf("[核心] 已固定到 %s（下次重启生效；清除配置/重置系统均可清除）\n", model);
  return true;
}

bool clearForcedChipModel() {
  Preferences prefs;
  bool ok = prefs.begin(NVS_NS, false);
  if (ok) prefs.remove(NVS_KEY_CHIP);
  prefs.end();
  forcedChipModel = "";
  logf("[核心] 已恢复自动识别（%s）\n", ESP.getChipModel());
  return true;
}

// ===================== 天气定位配置（保存/读取，中文名按 UTF-8 存储）=====================
void saveWeatherConfig(bool manual,
                       const char* provCode, const char* cityCode, const char* distCode,
                       const char* prov, const char* city, const char* dist) {
  stateAssertHeld();   // HTTP handler 直接调用的 NVS 写入：Preferences 非线程安全，必须在锁内
  DynamicJsonDocument doc(512);
  doc["mode"] = manual ? "manual" : "auto";
  if (manual) {
    doc["provCode"] = provCode ? provCode : "";
    doc["cityCode"] = cityCode ? cityCode : "";
    doc["distCode"] = distCode ? distCode : "";
    doc["prov"] = prov ? prov : "";
    doc["city"] = city ? city : "";
    doc["dist"] = dist ? dist : "";
  }
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putBytes(NVS_KEY_WEATHER, json.c_str(), json.length());
  prefs.end();
}

void loadWeatherConfigRaw(String& out) {
  out = nvsReadJsonFrom(NVS_KEY_WEATHER);
}

// ===================== 接口转发地址（独立 NVS key）=====================
// 存 JSON {"url":"..."} 而非裸字符串：与本工程其它配置一致，日后要加字段（如开关）也不用改格式。
// url 为空 = 关闭该级，行为与从未配置过完全相同。
void saveRelayConfig(const char* url) {
  stateAssertHeld();   // HTTP handler 直接调用的 NVS 写入：Preferences 非线程安全，必须在锁内
  DynamicJsonDocument doc(320);
  doc["url"] = url ? url : "";
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  size_t written = prefs.putBytes(NVS_KEY_RELAY, json.c_str(), json.length());
  prefs.end();
  logf("[NVS] 接口转发已保存: %s (%luB)\n",
       (url && *url) ? url : "(空 = 关闭该级)", (unsigned long)written);
}

void loadRelayConfigRaw(String& out) {
  out = nvsReadJsonFrom(NVS_KEY_RELAY);
}

// ===================== 时区配置（独立 NVS key）=====================
// mode=auto：偏移由网络定位结果填充（国内恒 +8，国外按 ipwho.is 折算冬令时）；
// mode=manual：偏移由用户在网页指定，自动定位结果不许覆盖。
// offset 一律是"冬令时"基准秒数，不随夏令时变化。
void saveTzConfig(bool autoMode, int32_t offset) {
  stateAssertHeld();   // HTTP handler 直接调用的 NVS 写入：Preferences 非线程安全，必须在锁内
  DynamicJsonDocument doc(128);
  doc["mode"] = autoMode ? "auto" : "manual";
  doc["offset"] = offset;
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  size_t written = prefs.putBytes(NVS_KEY_TZ, json.c_str(), json.length());
  prefs.end();
  int32_t a = (offset < 0) ? -offset : offset;
  logf("[NVS] 时区配置已保存: mode=%s offset=UTC%s%d:%02d (%luB)\n",
       autoMode ? "auto" : "manual", offset < 0 ? "-" : "+",
       (int)(a / 3600), (int)((a % 3600) / 60), (unsigned long)written);
}

void loadTzConfigRaw(String& out) {
  out = nvsReadJsonFrom(NVS_KEY_TZ);
}

// 开机加载时区配置：把保存的模式与偏移写入运行时 tzOffsetSec。
// 必须做这一步——运行时默认是 TZ_OFFSET_DEFAULT（0 = UTC），若不载入，用户手动设的时区
// 重启后就丢了；而 AP 模式下没有外网、定位链根本不会跑，全靠这个手动值撑着。
// 两种模式都应用保存的偏移：auto 模式下它相当于"上次已知时区"，
// 比开机先停在 UTC 再等定位跳变要好；定位成功后会被定位结果覆盖。
void loadTzConfig() {
  String raw;
  loadTzConfigRaw(raw);
  if (raw.length() == 0) return;   // 未配置 → 保持默认（auto + UTC）
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  bool autoMode = (strcmp(doc["mode"] | "auto", "auto") == 0);
  long offset = doc["offset"] | (long)TZ_OFFSET_DEFAULT;
  timeSetTzOffset((int32_t)offset);
  logf("[时区] 载入配置: mode=%s offset=%ld\n", autoMode ? "auto" : "manual", offset);
}

// ===================== 时钟配置（独立 NVS key，固定 4 槽位 = timer0~3）=====================

bool saveClockConfig() {
  DynamicJsonDocument doc(512);
  JsonArray arr = doc.createNestedArray("clocks");
  for (int i = 0; i < CLOCK_MAX; i++) {
    // 槽位必须逐个占位（空对象也要写），否则读侧按数组下标对槽，后面的时钟会串位。
    // 只写"与读侧默认值不同"的字段：enabled 默认 false、freq 默认 PWM_FREQ_DEFAULT。
    // 改动这两处默认值必须同步改 loadClockConfig 的 `| false` / `| PWM_FREQ_DEFAULT`。
    JsonObject o = arr.createNestedObject();
    if (clocks[i].enabled)                    o["enabled"] = true;
    if (clocks[i].freq != PWM_FREQ_DEFAULT)   o["freq"] = clocks[i].freq;
  }
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  bool ok = prefs.begin(NVS_NS, false);
  size_t written = ok ? prefs.putBytes(NVS_KEY_CLOCK, json.c_str(), json.length()) : 0;
  prefs.end();
  if (written > 0) {
    logf("[NVS] 时钟配置已保存, 数据=%dB\n", json.length());
    noteSaveResult(SAVE_MOD_CLOCK, true);
    return true;
  }
  logf("[NVS] 时钟配置保存失败!\n");
  noteSaveResult(SAVE_MOD_CLOCK, false);
  return false;
}

void loadClockConfig() {
  for (int i = 0; i < CLOCK_MAX; i++) { clocks[i].enabled = false; clocks[i].freq = 0; clocks[i].maxVal = 0; }

  String json = nvsReadJsonFrom(NVS_KEY_CLOCK);

  if (json.length() == 0) {
    clocks[0].enabled = true;
    clocks[0].freq = PWM_FREQ_DEFAULT;
    saveClockConfig();
    logRaw("[时钟] 无已有配置，默认一个 20000Hz 时钟\n");
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    clocks[0].enabled = true;
    clocks[0].freq = PWM_FREQ_DEFAULT;
    saveClockConfig();
    logf("[时钟] 警告：解析失败 (%s)，使用默认\n", err.c_str());
    return;
  }

  JsonArray arr = doc["clocks"];
  int n = arr.size();
  if (n > CLOCK_MAX) n = CLOCK_MAX;
  for (int i = 0; i < n; i++) {
    bool en = arr[i]["enabled"] | false;
    uint32_t f = (uint32_t)safeConstrain(arr[i]["freq"] | PWM_FREQ_DEFAULT, 1, 40000);
    clocks[i].enabled = en;
    clocks[i].freq = f;
  }

  // 确保至少一个时钟
  if (clockGetFirstEnabled() < 0) { clocks[0].enabled = true; clocks[0].freq = PWM_FREQ_DEFAULT; }

  // 频率去重：数据异常时重复的后续时钟禁用
  for (int i = 0; i < CLOCK_MAX; i++) {
    if (!clocks[i].enabled) continue;
    for (int k = 0; k < i; k++) {
      if (clocks[k].enabled && clocks[k].freq == clocks[i].freq) { clocks[i].enabled = false; break; }
    }
  }
  if (clockGetFirstEnabled() < 0) { clocks[0].enabled = true; clocks[0].freq = PWM_FREQ_DEFAULT; }

  String s;
  s.reserve(64);
  for (int i = 0; i < CLOCK_MAX; i++) {
    if (!clocks[i].enabled) continue;
    if (s.length()) s += ", ";
    s += "时钟" + String(i + 1) + "=" + String(clocks[i].freq) + "Hz";
  }
  logf("[时钟] 加载成功：%s\n", s.c_str());
}

// 时钟配置防抖合并写入
static bool clockConfigDirty = false;
static unsigned long lastClockConfigWriteMs = 0;

void requestClockConfigSave() {
  clockConfigDirty = true;
}

void checkPendingClockConfigSave() {
  if (!clockConfigDirty) return;
  if (millis() - lastClockConfigWriteMs < 500) return;
  clockConfigDirty = false;
  lastClockConfigWriteMs = millis();
  StateLockGuard _lg;   // 序列化/落盘期间持锁：HTTP 任务不能同时改 clocks
  saveClockConfig();
}

// 生成 NVS 命名空间下所有已保存参数的只读文本（供网页「NVS 数据」弹窗展示）
void buildNvsDump(String& out) {
  out = "";
  out.reserve(4096);
  // 每个 key 按自身实际大小分配解析缓冲：只有 ioconfig 需要 IO_CONFIG_DOC_SIZE（满配约 23KB），
  // 其余 key 远小于此。统一用大容量会让循环里连续 7 次申请/释放 48KB，白白制造堆碎片。
  struct NvsEntry { const char* key; const char* title; uint32_t size; };
  const NvsEntry entries[] = {
    { NVS_KEY_JSON,       "config (PWM + WiFi)",    16384 },
    { NVS_KEY_IO,         "ioconfig (序列器)",      IO_CONFIG_DOC_SIZE },
    { NVS_KEY_COMPARATOR, "compconfig (比较器)",     8192 },
    { NVS_KEY_COUNTER,    "counterconfig (计数器)",  8192 },
    { NVS_KEY_WEATHER,    "weather (天气)",          2048 },
    { NVS_KEY_RELAY,      "relay (接口转发)",          512 },
    { NVS_KEY_TZ,         "tz (时区)",                 512 },
    { NVS_KEY_CHIP,       "chipmodel (芯片型号)",     512 },
    { NVS_KEY_CLOCK,      "clocks (时钟)",           2048 },
    { NVS_KEY_WX_FADE,    "wxfade (天气过渡)",        512 },
  };
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    // 统一走兼容读取：JSON 大配置是 blob，芯片型号是普通字符串，helper 自带字符串回退分支。
    // （对字符串 key 会先试探 blob 接口、在库内打一条 log_e，默认 Core Debug Level 为 None 时不可见。）
    String raw = nvsReadJsonFrom(entries[i].key);
    out += "[" + String(entries[i].title) + "]\n";
    if (raw.length() == 0) {
      out += "(未保存)\n\n";
      continue;
    }
    DynamicJsonDocument sub(entries[i].size);
    if (deserializeJson(sub, raw) == DeserializationError::Ok) {
      String pretty;
      serializeJsonPretty(sub, pretty);
      out += pretty;
    } else {
      out += raw;   // 非 JSON（如芯片型号字符串）
    }
    out += "\n\n";
  }
}

// NVS 容量统计（供网页容量条）。返回 false 表示取统计失败。
// used=已用条目数；avail=NVS 认为还能写入的条目数（已扣掉垃圾回收要保留的页）；
// total=分区总条目数（5 页 × 126）。
// 注意：used+avail 才是"真正可分配的空间"，两者的比例才有意义（NVS 会留页给回收，
// 直接用 used/total 会得出一个偏小、且加不到 100% 的占用率）。
bool readNvsStats(uint32_t& usedEntries, uint32_t& availEntries, uint32_t& totalEntries) {
  nvs_stats_t st;
  if (nvs_get_stats(NULL, &st) != ESP_OK) return false;   // NULL = 默认 "nvs" 分区
  usedEntries = (uint32_t)st.used_entries;
  availEntries = (uint32_t)st.available_entries;
  totalEntries = (uint32_t)st.total_entries;
  return true;
}

// ===================== 天气系数过渡时长（独立 NVS key）=====================
// 天气现象切换时，天气系数在该时长内线性走到新档位（0 = 关闭过渡 = 立即切换）。
// 必须有 RAM 缓存：updatePwmDirect() 每 loop 都要取，不能每次都读 NVS。
static int wxFadeSecCache = WX_FADE_DEFAULT_SEC;

int wxFadeSec() { return wxFadeSecCache; }

void loadWxFadeConfig() {
  wxFadeSecCache = WX_FADE_DEFAULT_SEC;
  String json = nvsReadJsonFrom(NVS_KEY_WX_FADE);
  if (json.length() == 0) {
    logf("[天气过渡] 未配置，默认 %d 秒\n", wxFadeSecCache);
    return;
  }
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    logf("[天气过渡] 配置解析失败，用默认 %d 秒\n", wxFadeSecCache);
    return;
  }
  wxFadeSecCache = safeConstrain((int)(doc["sec"] | WX_FADE_DEFAULT_SEC), 0, WX_FADE_MAX_SEC);
  logf("[天气过渡] 载入：%d 秒%s\n", wxFadeSecCache, wxFadeSecCache == 0 ? "（立即切换）" : "");
}

bool saveWxFadeConfig(int sec) {
  stateAssertHeld();   // HTTP handler 直接调用的 NVS 写入：Preferences 非线程安全，必须在锁内
  sec = safeConstrain(sec, 0, WX_FADE_MAX_SEC);
  DynamicJsonDocument doc(128);
  doc["sec"] = sec;
  String json;
  serializeJson(doc, json);
  Preferences prefs;
  bool ok = prefs.begin(NVS_NS, false);
  size_t written = ok ? prefs.putBytes(NVS_KEY_WX_FADE, json.c_str(), json.length()) : 0;
  prefs.end();
  if (written == 0) {
    logf("[NVS] 天气过渡时长保存失败! begin=%s, written=%lu\n", ok ? "ok" : "fail", (unsigned long)written);
    return false;
  }
  wxFadeSecCache = sec;   // 立即生效（下一轮 updatePwmDirect 就用新时长）
  logf("[NVS] 天气过渡时长已保存: %d 秒 (%luB)\n", sec, (unsigned long)written);
  return true;
}