#include "config.h"
#include <ArduinoJson.h>
#include "webpage.h"

// ===================== HTTP 独立任务 + 状态锁接入 =====================
// 为什么要有这个任务：在主 loop 里调 server.handleClient() 时，一次 290KB 的整页响应会阻塞在
// socket 写上（NetworkClient::write 用 select 等可写，单次 write 就是整包），最长可到
// HTTP_MAX_SEND_WAIT = 5s —— 浏览器刷新中途 abort 掉旧请求时正是这种情况。那期间序列器到点
// 翻转、计数器采样、比较器求值、NVS 防抖落盘全部停摆。把 HTTP 挪到独立任务后，这些阻塞只
// 影响 HTTP 任务自己，主 loop 继续跑控制逻辑。
//
// 代价是两个任务并发读写同一批配置结构，必须靠状态锁互斥（设计说明见 config.h「跨任务状态锁」）。
// 这里的三条接入规则：
//   1) handler 统一在注册处用 onLocked/onLockedPost 包装（处理期间持锁）；
//   2) 所有发送走 httpSend（发送前解锁，"持锁发送"因此在结构上不可能发生）；
//   3) handler 返回时 runLocked 兜底解锁（异常路径漏发响应也不会永久死锁）。
static TaskHandle_t httpTaskHandle = NULL;

bool httpTaskIsRunning() { return httpTaskHandle != NULL; }

// 发送响应：先释放状态锁再发。发送是秒级长阻塞，持锁发送会把 loop 卡死。
static void httpSend(int code, const char* contentType, const String& body) {
  stateUnlockIfHeld();
  server.send(code, contentType, body);
}
static void httpSend(int code, const char* contentType, const char* body) {
  stateUnlockIfHeld();
  server.send(code, contentType, body);
}

// 请求处理包装：处理期间持锁，返回时兜底解锁（正常路径的锁已由 httpSend 释放）
static void runLocked(void (*fn)()) {
  stateLock();
  fn();
  stateUnlockIfHeld();
}
static void onLocked(const char* uri, void (*fn)()) {
  server.on(uri, [fn]() { runLocked(fn); });
}
static void onLockedPost(const char* uri, void (*fn)()) {
  server.on(uri, HTTP_POST, [fn]() { runLocked(fn); });
}

// HTTP 任务：handleClient() 本身非阻塞（无客户端时立即返回），靠 vTaskDelay(1) 让出 CPU，
// 避免空转饿死同核 IDLE 任务触发 task_wdt（天气任务当初踩过这个坑）。
static void httpTaskLoop(void*) {
  for (;;) {
    server.handleClient();
    vTaskDelay(1);
  }
}

// 启动 HTTP 任务。pin core 0 的理由：计数器与中心面板都是软件轮询（counter.ino 的 updateCounter、
// central.ino 的 updateCentralMonitor），依赖 loop 周期的稳定性；放 core 0 就不会偷 loop 的 CPU，
// 而 core 0 上的 WiFi/lwIP 任务优先级更高，也不会被这个任务饿到。
static void httpTaskStart() {
  stateLockArm(true);   // 此后才真正存在并发，断言开始生效
  BaseType_t rc = xTaskCreatePinnedToCore(httpTaskLoop, "http", 10240, NULL, 1, &httpTaskHandle, 0);
  if (rc != pdPASS) {
    httpTaskHandle = NULL;   // 回退：主 loop 继续自己 handleClient（功能不变，只是仍会被阻塞）
    logf("[HTTP] 独立任务创建失败(%d)，回退主循环处理\n", (int)rc);
    return;
  }
  logf("[HTTP] 独立任务已启动（core 0, prio 1, 栈 10KB）\n");
}

String getHTML() {
  String html;
  // 网页总内容约 247KB（CSS+HTML+6 个 JS 模块），预留充足容量避免 String 反复扩容拷贝拖慢刷新
  html.reserve(300000);

  // 拼接顺序：HEAD_A -> CSS -> HEAD_B -> [动态时间] -> FOOT_A -> JS*4 -> FOOT_B
  html.concat(PAGE_HTML_HEAD_A);
  html.concat(PAGE_CSS);
  html.concat(PAGE_HTML_HEAD_B);

  // 动态：初始时间（后续由前端 /getstatus 每秒刷新）
  html += String(currentTime.hour) + ":" +
          (currentTime.minute < 10 ? "0" : "") + String(currentTime.minute) + ":" +
          (currentTime.second < 10 ? "0" : "") + String(currentTime.second);

  html.concat(PAGE_HTML_FOOT_A);
  html.concat(PAGE_JS_CORE);
  html.concat(PAGE_JS_IO);
  html.concat(PAGE_JS_COMPARATOR);
  html.concat(PAGE_JS_COUNTER);
  html.concat(PAGE_JS_CENTRAL);
  html.concat(PAGE_JS_WEATHER);
  html.concat(PAGE_HTML_FOOT_B);

  return html;
}

// 提供给前端的完整配置（通道 + 任务初始值），前端据此渲染通道卡片
void handleGetConfig() {
  DynamicJsonDocument doc(16384);
  doc["chipModel"] = getEffectiveChipModel();
  // 天气系数（目标档位）：供「大气环境」弹窗左栏摘要显示"原始 × 系数 = 输出"
  doc["weatherFactor"] = weatherGetDutyFactor();

  JsonArray pinsArray = doc.createNestedArray("pwmPins");
  for (int i = 0; i < pwmChannelCount; i++) {
    pinsArray.add(pwmPins[i]);
  }

  JsonArray availArray = doc.createNestedArray("availablePins");
  for (int i = 0; i < getAvailablePinCount(); i++) {
    availArray.add(getAvailablePin(i));
  }

  JsonArray clocksArray = doc.createNestedArray("clocks");
  for (int i = 0; i < CLOCK_MAX; i++) {
    JsonObject c = clocksArray.createNestedObject();
    c["enabled"] = clocks[i].enabled;
    c["freq"] = clocks[i].freq;
  }

  JsonArray channels = doc.createNestedArray("channels");
  for (int i = 0; i < pwmChannelCount; i++) {
    JsonObject ch = channels.createNestedObject();
    ch["enabled"] = pwmConfigs[i].enabled;
    ch["taskCount"] = pwmConfigs[i].taskCount;
    ch["weatherInteract"] = pwmConfigs[i].weatherInteract;
    ch["currentDuty"] = pwmConfigs[i].currentDuty;
    ch["activeTask"] = pwmConfigs[i].activeTask;
    // 阳光交互（配置 + 运行时接管状态）：前端渲染开关、设置回填与「阳光覆盖」标签用
    ch["sunEnabled"] = pwmConfigs[i].sunEnabled;
    ch["sunMode"] = pwmConfigs[i].sunMode;
    ch["sunPeakDuty"] = pwmConfigs[i].sunPeakDuty;
    ch["sunActive"] = pwmConfigs[i].sunActive;
    ch["sunStartMin"] = pwmConfigs[i].sunStartMin;
    ch["sunEndMin"] = pwmConfigs[i].sunEndMin;

    JsonArray tasks = ch.createNestedArray("tasks");
    for (int j = 0; j < pwmConfigs[i].taskCount; j++) {
      JsonObject t = tasks.createNestedObject();
      t["enabled"] = pwmConfigs[i].tasks[j].enabled;
      t["startHour"] = pwmConfigs[i].tasks[j].startHour;
      t["startMinute"] = pwmConfigs[i].tasks[j].startMinute;
      t["endHour"] = pwmConfigs[i].tasks[j].endHour;
      t["endMinute"] = pwmConfigs[i].tasks[j].endMinute;
      t["startDuty"] = pwmConfigs[i].tasks[j].startDuty;
      t["endDuty"] = pwmConfigs[i].tasks[j].endDuty;
      t["clock"] = pwmConfigs[i].tasks[j].clockIndex;
      t["conflict"] = pwmConfigs[i].tasks[j].conflict;
    }
  }

  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleRoot() {
  logf("[HTTP] GET / (uptime=%lums, heap=%u)\n", millis(), ESP.getFreeHeap());
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  httpSend(200, "text/html", getHTML());
}

void handleGetTime() {
  StaticJsonDocument<128> doc;
  doc["hour"] = currentTime.hour;
  doc["minute"] = currentTime.minute;
  doc["second"] = currentTime.second;
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleGetChannelStatus() {
  StaticJsonDocument<1024> doc;
  JsonArray channels = doc.createNestedArray("channels");
  for (int i = 0; i < pwmChannelCount; i++) {
    JsonObject channel = channels.createNestedObject();
    channel["index"] = i;
    channel["currentDuty"] = pwmConfigs[i].currentDuty;
    channel["activeTask"] = pwmConfigs[i].activeTask;
    channel["enabled"] = pwmConfigs[i].enabled;
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 非阻塞：仅返回当前同步状态
void handleGetNtpSyncStatus() {
  StaticJsonDocument<128> doc;
  doc["syncInProgress"] = ntpSyncInProgress;
  doc["manualFinished"] = ntpManualFinished();
  doc["manualSuccess"] = ntpManualSuccess();
  doc["manualError"] = ntpManualError();
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleSetTime() {
  DynamicJsonDocument doc(128);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) { httpSend(400, "application/json", "{\"error\":\"无效数据\"}"); return; }
  int hour = doc["hour"] | 0;
  int minute = doc["minute"] | 0;
  int second = doc["second"] | 0;

  currentTime.hour = safeConstrain(hour, 0, 23);
  currentTime.minute = safeConstrain(minute, 0, 59);
  currentTime.second = safeConstrain(second, 0, 59);

  if (lastSyncEpoch != 0 && lastSyncMillis != 0) {
    // 用户输入的时分秒是"本地钟点"，需换算成 UTC epoch 存入 lastSyncEpoch。
    // 必须用运行期偏移 tzOffsetSec（而非写死的北京时间），否则非 UTC+8 时区拨钟会算错。
    // 本地日号 × 86400 得到本地日界，减去偏移即该日界对应的 UTC epoch（负数偏移按模运算成立）。
    unsigned long secOfDay = currentTime.hour * 3600UL + currentTime.minute * 60UL + currentTime.second;
    unsigned long dayBase = (unsigned long)timeLocalDayOf(lastValidEpochTime) * 86400UL;
    lastSyncEpoch = dayBase + secOfDay - (unsigned long)tzOffsetSec;
    lastSyncMillis = millis();
  }

  requestConfigSave();
  // 拨钟会让 currentTime 跳变，原始占空比随之直接跳到新时刻对应的值，不做过渡。
  httpSend(200, "application/json", "{\"success\":true}");
}

// 不在 Web 回调中阻塞同步，只触发后台编排，立即返回。
// 走与开机相同的「定位(求时区) → NTP(拿 UTC) → 天气」顺序：先定位能顺带刷新时区。
void handleSyncNTPTime() {
  StaticJsonDocument<128> doc;
  if (WiFi.status() != WL_CONNECTED) {
    doc["success"] = false;
    doc["error"] = "WiFi未连接";
    String jsonStr;
    serializeJson(doc, jsonStr);
    httpSend(200, "application/json", jsonStr);
    return;
  }
  weatherArmStartup(true);   // true = 按手动同步上报 NTP 结果（前端据此弹提示）
  doc["success"] = true;
  doc["syncing"] = true;
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleToggleChannel() {
  int channel = server.arg("channel").toInt();
  if (channel < 0 || channel >= pwmChannelCount) { httpSend(400, "application/json", "{\"error\":\"无效通道\"}"); return; }
  pwmConfigs[channel].enabled = !pwmConfigs[channel].enabled;
  requestConfigSave();
  StaticJsonDocument<64> doc;
  doc["enabled"] = pwmConfigs[channel].enabled;
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleSaveChannel() {
  int channel = server.arg("channel").toInt();
  if (channel < 0 || channel >= pwmChannelCount) { httpSend(400, "application/json", "{\"error\":\"无效通道\"}"); return; }
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) { httpSend(400, "application/json", "{\"error\":\"无效数据\"}"); return; }
  JsonArray tasks = doc["tasks"];
  int n = pwmConfigs[channel].taskCount;
  if (n > tasks.size()) n = tasks.size();
  for (int j = 0; j < n; j++) {
    JsonObject task = tasks[j];
    pwmConfigs[channel].tasks[j].enabled = task["enabled"];
    int startHour = task["startHour"] | 0;
    int startMinute = task["startMinute"] | 0;
    int endHour = task["endHour"] | 0;
    int endMinute = task["endMinute"] | 0;
    int startDuty = task["startDuty"] | 0;
    int endDuty = task["endDuty"] | 100;
    int ck = task["clock"] | pwmConfigs[channel].tasks[j].clockIndex;
    pwmConfigs[channel].tasks[j].startHour = safeConstrain(startHour, 0, 23);
    pwmConfigs[channel].tasks[j].startMinute = safeConstrain(startMinute, 0, 59);
    pwmConfigs[channel].tasks[j].endHour = safeConstrain(endHour, 0, 23);
    pwmConfigs[channel].tasks[j].endMinute = safeConstrain(endMinute, 0, 59);
    pwmConfigs[channel].tasks[j].startDuty = safeConstrain(startDuty, 0, 100);
    pwmConfigs[channel].tasks[j].endDuty = safeConstrain(endDuty, 0, 100);
    pwmConfigs[channel].tasks[j].clockIndex = safeConstrain(ck, 0, CLOCK_MAX - 1);
  }
  pwmRecalcConflicts();
  requestConfigSave();

  StaticJsonDocument<128> resp;
  resp["success"] = true;
  JsonArray conflicts = resp.createNestedArray("conflicts");
  for (int j = 0; j < pwmConfigs[channel].taskCount; j++) conflicts.add(pwmConfigs[channel].tasks[j].conflict);
  String jsonStr;
  serializeJson(resp, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 引脚自定义：接收完整 6 路引脚数组，校验后重绑并保存到 NVS
void handleSavePins() {
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return; }
  JsonArray arr = doc["pins"];
  bool devMode = doc["devMode"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (arr.size() < pwmChannelCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"引脚数量不足\"}");
    return;
  }

  // 整体校验：每个都在可用集合内；非开发模式禁用 Flash/PSRAM；
  // 非允许重复时禁止与 IO/比较器/计数器及本组其他通道冲突
  int newPins[PWM_CHANNEL_MAX];
  for (int i = 0; i < pwmChannelCount; i++) {
    int p = arr[i] | -1;
    if (!isPinAvailable(p)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"包含不可用引脚\"}");
      return;
    }
    if (!devMode && isIoFlashPin(p)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"包含 Flash/PSRAM 总线引脚\"}");
      return;
    }
    if (!allowDup && ioIsPinUsedByIo(p, -1)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"引脚被序列器占用\"}");
      return;
    }
    if (!allowDup && comparatorIsPinUsedByOutput(p, -1)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"引脚被比较器输出占用\"}");
      return;
    }
    if (!allowDup && comparatorIsPinUsedByExternalInput(p, -1)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"引脚被比较器外部输入占用\"}");
      return;
    }
    if (!allowDup && counterIsPinUsedByAction(p, -1)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"引脚被计数器动作占用\"}");
      return;
    }
    for (int k = 0; k < i; k++) {
      if (!allowDup && newPins[k] == p) {
        httpSend(400, "application/json", "{\"success\":false,\"error\":\"引脚重复\"}");
        return;
      }
    }
    newPins[i] = p;
  }

  // 逐个重绑：仅对引脚发生变化的通道执行，避免未变更通道被重新绑定
  // 而中断其 PWM 输出（或与锁存/手动覆盖状态冲突）。
  for (int i = 0; i < pwmChannelCount; i++) {
    if (newPins[i] == pwmPins[i]) continue;   // 未变更，跳过
    if (!setChannelPin(i, newPins[i])) {
      httpSend(500, "application/json", "{\"success\":false,\"error\":\"引脚绑定失败\"}");
      return;
    }
  }

  requestConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 添加 PWM 通道（末尾追加，最多 PWM_CHANNEL_MAX 个）
void handlePwmAdd() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int pin = doc["pin"] | -1;
  bool devMode = doc["devMode"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (!pwmAddChannel(pin, devMode, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：引脚不可用/已被占用/已达上限\"}");
    return;
  }
  requestConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 删除 PWM 通道（释放引脚给 IO/比较器/计数器）
void handlePwmDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (!pwmRemoveChannel(index)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效通道或引脚被锁存/手动切换\"}");
    return;
  }
  requestConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 添加 PWM 条目（末尾追加，最多 PWM_TASK_MAX 条）
void handlePwmTaskAdd() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int channel = doc["channel"] | -1;
  if (!pwmAddTask(channel)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：通道无效或条目已达上限 6 条\"}");
    return;
  }
  requestConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 删除 PWM 条目
void handlePwmTaskDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int channel = doc["channel"] | -1;
  int index = doc["index"] | -1;
  if (!pwmRemoveTask(channel, index)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效条目\"}");
    return;
  }
  requestConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 切换 PWM 通道的天气交互开关（持久化到 NVS）
void handlePwmWeatherToggle() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int channel = doc["channel"] | -1;
  // 注意：ArduinoJson 的 operator| 用 is<T>() 做严格类型判断，不做隐式转换，
  // 用 (doc["enabled"] | 0) != 0 解析布尔值会永远拿到默认值 0。必须用 as<bool>()。
  bool enabled = doc["enabled"].as<bool>();
  if (channel < 0 || channel >= pwmChannelCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效通道\"}"); return;
  }
  pwmConfigs[channel].weatherInteract = enabled;
  requestConfigSave();
  StaticJsonDocument<64> r;
  r["success"] = true;
  r["weatherInteract"] = pwmConfigs[channel].weatherInteract;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 阳光交互设置（一次保存全部字段：开关 + 基准 + 最高原始占空比）。
// 与气象交互的开关分开：阳光带参数，必须整体保存才不会出现"开了但参数没生效"的中间态。
void handlePwmSunSet() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int channel = doc["channel"] | -1;
  if (channel < 0 || channel >= pwmChannelCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效通道\"}"); return;
  }
  PwmChannelConfig& c = pwmConfigs[channel];
  // 布尔值必须用 as<bool>()（operator| 的 is<T>() 判定不做隐式转换，见 handlePwmWeatherToggle 注释）
  bool enabled = doc["enabled"].as<bool>();
  int mode = doc["mode"] | (int)c.sunMode;
  int peak = doc["peakDuty"] | c.sunPeakDuty;
  if (mode != SUN_MODE_SUNRISE && mode != SUN_MODE_TWILIGHT) mode = SUN_MODE_SUNRISE;
  peak = safeConstrain(peak, 0, 100);

  c.sunMode = (uint8_t)mode;
  c.sunPeakDuty = peak;
  c.sunEnabled = enabled;
  requestConfigSave();

  // 回带数据可用性：前端据此提示"暂无太阳时刻数据，将按条目执行"
  int s = -1, e = -1;
  bool dataOk = sunWindowOfChannel(channel, s, e);
  StaticJsonDocument<256> r;
  r["success"] = true;
  r["sunEnabled"] = c.sunEnabled;
  r["sunMode"] = (int)c.sunMode;
  r["sunPeakDuty"] = c.sunPeakDuty;
  r["dataOk"] = dataOk;
  r["startMin"] = s;
  r["endMin"] = e;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// ===== 时钟接口 =====

void handleClockGet() {
  DynamicJsonDocument doc(512);
  JsonArray arr = doc.createNestedArray("clocks");
  for (int i = 0; i < CLOCK_MAX; i++) {
    JsonObject o = arr.createNestedObject();
    o["enabled"] = clocks[i].enabled;
    o["freq"] = clocks[i].freq;
    o["used"] = clockIsUsed(i);
  }
  String json; serializeJson(doc, json);
  httpSend(200, "application/json", json);
}

void handleClockAdd() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return; }
  uint32_t freq = (uint32_t)(doc["freq"] | PWM_FREQ_DEFAULT);
  if (!clockAdd(freq)) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：已达上限或频率重复\"}"); return; }
  requestClockConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleClockRemove() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return; }
  int slot = doc["slot"] | -1;
  if (slot >= 0 && slot < CLOCK_MAX && clocks[slot].enabled && clockIsUsed(slot)) {
    // 收集引用该时钟的 GPIO
    String pins;
    for (int i = 0; i < pwmChannelCount; i++) {
      for (int j = 0; j < pwmConfigs[i].taskCount; j++) {
        if (pwmConfigs[i].tasks[j].clockIndex == slot) {
          if (pins.length()) pins += ", ";
          pins += String(pwmPins[i]);
          break;
        }
      }
    }
    String resp = "{\"success\":false,\"error\":\"此时钟应用于 GPIO ";
    resp += pins;
    resp += "\"}";
    httpSend(400, "application/json", resp);
    return;
  }
  if (!clockRemove(slot)) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：至少保留一个时钟\"}"); return; }
  requestClockConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleClockSetFreq() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return; }
  int slot = doc["slot"] | -1;
  uint32_t freq = (uint32_t)(doc["freq"] | PWM_FREQ_DEFAULT);
  if (!clockSetFreq(slot, freq)) { httpSend(400, "application/json", "{\"success\":false,\"error\":\"修改失败：频率重复或时钟无效\"}"); return; }
  requestClockConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleNvsData() {
  String out;
  buildNvsDump(out);
  httpSend(200, "text/plain; charset=utf-8", out);
}

void handleExportConfig() {
  // 全局导出：时钟 + PWM（引脚+通道）+ 序列器组 + 比较器门 + 计数器任务 + 手动基准 + 时区 + 天气定位 + 接口转发。
  // 不含 WiFi 凭据，避免敏感信息落入备份文件；也不含芯片型号固定（设备相关，换机导入反而有害）。
  //
  // 两个通道（前端会先弹窗让用户选），**都从实时 RAM 重扫**，区别只在是否过滤总线引脚：
  //   缺省   → 标准导出：与"写入 NVS 的内容"一致（滤掉总线/开发模式引脚的配置）
  //   dev=1  → 含开发引脚导出：不过滤，总线引脚上的配置一并导出
  //
  // 不要改成"读 NVS 再导出"：NVS 满或某次保存失败时读出来会缺内容，
  // 用户会拿到一份不完整的备份。扫实时配置才能保证导出完整。
  // 代价：下面的过滤条件必须与 config_store.ino 各 save*() 里的过滤保持一致（两处注释互指）。
  bool includeDev = server.hasArg("dev") && server.arg("dev") == "1";
  logf("[导出] %s（实时重扫配置）\n", includeDev ? "含开发引脚导出" : "标准导出");
  DynamicJsonDocument doc(49152);   // 堆上分配，避免占用 Web 回调栈空间

  // 1) 时钟
  JsonArray clocksArr = doc.createNestedArray("clocks");
  for (int i = 0; i < CLOCK_MAX; i++) {
    JsonObject c = clocksArr.createNestedObject();
    c["enabled"] = clocks[i].enabled;
    c["freq"] = clocks[i].freq;
  }

  // 2) PWM 引脚
  JsonArray pinsArr = doc.createNestedArray("pins");
  for (int i = 0; i < pwmChannelCount; i++) {
    if (!includeDev && isIoFlashPin(pwmPins[i])) continue;   // 与 saveConfig() 的过滤一致
    pinsArr.add(pwmPins[i]);
  }

  // 3) PWM 通道
  JsonArray pwmArray = doc.createNestedArray("pwmConfigs");
  for (int i = 0; i < pwmChannelCount; i++) {
    if (!includeDev && isIoFlashPin(pwmPins[i])) continue;   // 必须与上面 pins 用同一判断，否则下标错位
    JsonObject pwmObj = pwmArray.createNestedObject();
    pwmObj["enabled"] = pwmConfigs[i].enabled;
    pwmObj["taskCount"] = pwmConfigs[i].taskCount;
    // 必须导出：applyPwmConfigs 对缺失字段按默认 false 处理，漏掉它会让"导出→导入"把
    // 所有通道的天气交互静默关掉（导出侧保持全字段可读，压缩只发生在写 NVS 时）
    pwmObj["weatherInteract"] = pwmConfigs[i].weatherInteract;
    // 阳光交互必须导出（与 weatherInteract 同理：导入侧缺失字段按默认 false 处理，
    // 漏掉会让"导出→导入"把所有通道的阳光交互静默关掉）
    pwmObj["sunEnabled"] = pwmConfigs[i].sunEnabled;
    pwmObj["sunMode"] = pwmConfigs[i].sunMode;
    pwmObj["sunPeakDuty"] = pwmConfigs[i].sunPeakDuty;
    JsonArray tasksArray = pwmObj.createNestedArray("tasks");
    for (int j = 0; j < pwmConfigs[i].taskCount; j++) {
      JsonObject taskObj = tasksArray.createNestedObject();
      taskObj["enabled"] = pwmConfigs[i].tasks[j].enabled;
      taskObj["startHour"] = pwmConfigs[i].tasks[j].startHour;
      taskObj["startMinute"] = pwmConfigs[i].tasks[j].startMinute;
      taskObj["endHour"] = pwmConfigs[i].tasks[j].endHour;
      taskObj["endMinute"] = pwmConfigs[i].tasks[j].endMinute;
      taskObj["startDuty"] = pwmConfigs[i].tasks[j].startDuty;
      taskObj["endDuty"] = pwmConfigs[i].tasks[j].endDuty;
      taskObj["clock"] = pwmConfigs[i].tasks[j].clockIndex;
      taskObj["overnight"] = isTaskOvernight(pwmConfigs[i].tasks[j]);
    }
  }

  // 3) 序列器组
  JsonArray ioArr = doc.createNestedArray("ioGroups");
  for (int g = 0; g < ioGroupCount; g++) {
    if (!includeDev && isIoFlashPin(ioGroups[g].pin)) continue;   // 与 saveIoConfig() 的过滤一致
    JsonObject gobj = ioArr.createNestedObject();
    gobj["name"] = ioGroups[g].name;
    gobj["pin"] = ioGroups[g].pin;
    gobj["enabled"] = ioGroups[g].enabled;
    JsonArray entries = gobj.createNestedArray("entries");
    for (int j = 0; j < ioGroups[g].entryCount; j++) {
      const IoEntry& e = ioGroups[g].entries[j];
      JsonObject eobj = entries.createNestedObject();
      eobj["h"] = e.hour;
      eobj["m"] = e.minute;
      eobj["a"] = e.action ? 1 : 0;
      eobj["mo"] = e.mode;
      eobj["pm"] = e.pulseMs;
      eobj["hh"] = e.hbHighMs;
      eobj["hl"] = e.hbLowMs;
    }
  }

  // 4) 比较器门
  JsonArray cmpArr = doc.createNestedArray("comparatorGates");
  for (int g = 0; g < comparatorGateCount; g++) {
    JsonObject gobj = cmpArr.createNestedObject();
    gobj["name"] = comparatorGates[g].name;
    gobj["enabled"] = comparatorGates[g].enabled;
    gobj["mode"] = comparatorGates[g].mode;
    JsonArray inputs = gobj.createNestedArray("inputs");
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      // 只有"外部引脚"类型的输入 refIndex 才是真实 GPIO（与 saveComparatorConfig 一致）
      if (!includeDev && comparatorGates[g].inputs[i].source == COMPARATOR_SRC_EXTERNAL &&
          isIoFlashPin(comparatorGates[g].inputs[i].refIndex)) continue;
      JsonObject iobj = inputs.createNestedObject();
      iobj["src"] = comparatorGates[g].inputs[i].source;
      iobj["ref"] = comparatorGates[g].inputs[i].refIndex;
      iobj["act"] = comparatorGates[g].inputs[i].actionIndex;
      iobj["exp"] = comparatorGates[g].inputs[i].expectedLevel;
    }
    JsonArray actions = gobj.createNestedArray("actions");
    for (int a = 0; a < comparatorGates[g].actionCount; a++) {
      if (!includeDev && isIoFlashPin(comparatorGates[g].actions[a].pin)) continue;
      JsonObject aobj = actions.createNestedObject();
      aobj["pin"] = comparatorGates[g].actions[a].pin;
      aobj["slv"] = comparatorGates[g].actions[a].sourceLevel;
      aobj["lvl"] = comparatorGates[g].actions[a].actionLevel;
    }
  }

  // 5) 计数器任务
  JsonArray ctrArr = doc.createNestedArray("counterTasks");
  for (int t = 0; t < counterTaskCount; t++) {
    JsonObject tobj = ctrArr.createNestedObject();
    tobj["name"] = counterTasks[t].name;
    tobj["enabled"] = counterTasks[t].enabled;
    // 总线引脚不能作为触发源（与 saveCounterConfig 一致）：标准导出里写成 -1 = 无源
    tobj["triggerPin"] = (!includeDev && isIoFlashPin(counterTasks[t].triggerPin))
                             ? -1 : counterTasks[t].triggerPin;
    tobj["threshold"] = counterTasks[t].threshold;
    JsonArray actions = tobj.createNestedArray("actions");
    for (int a = 0; a < counterTasks[t].actionCount; a++) {
      if (!includeDev && isIoFlashPin(counterTasks[t].actions[a].pin)) continue;   // 与 saveCounterConfig 一致
      JsonObject aobj = actions.createNestedObject();
      aobj["pin"] = counterTasks[t].actions[a].pin;
      aobj["level"] = counterTasks[t].actions[a].level;
      aobj["holdMode"] = counterTasks[t].actions[a].holdMode;
      aobj["holdSec"] = counterTasks[t].actions[a].holdSec;
    }
  }

  // 6) 手动基准（dev=1 时含总线引脚上的 —— 这正是两个导出通道的差别所在）
  JsonArray manualArr = doc.createNestedArray("manual");
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
    if (!manualOverride[p]) continue;
    if (!includeDev && isIoFlashPin(p)) continue;   // 与 saveIoConfig() 的 manual 段一致
    JsonObject o = manualArr.createNestedObject();
    o["p"] = p;
    o["l"] = manualLevel[p] ? 1 : 0;
  }

  // 7) 时区 + 天气定位。
  // 这两个模块在设备端没有 RAM 镜像（每次抓取时直接读 NVS），只能从 NVS 取。
  // 破例读 NVS 是可以的：它们体积都很小，且每次保存都是整体原子写入，
  // 不存在上面那种"大配置写不下→读出残缺"的问题。
  {
    String tzRaw;
    loadTzConfigRaw(tzRaw);
    bool tzAuto = true;
    long tzOff = (long)tzOffsetSec;
    if (tzRaw.length() > 0) {
      DynamicJsonDocument tzDoc(128);
      if (deserializeJson(tzDoc, tzRaw) == DeserializationError::Ok) {
        tzAuto = (strcmp(tzDoc["mode"] | "auto", "auto") == 0);
        tzOff  = tzDoc["offset"] | tzOff;
      }
    }
    JsonObject tzObj = doc.createNestedObject("tz");
    tzObj["mode"]   = tzAuto ? "auto" : "manual";
    tzObj["offset"] = tzOff;
  }
  {
    String wRaw;
    loadWeatherConfigRaw(wRaw);
    bool wManual = false;
    String pC, cC, dC, p, c, d;
    if (wRaw.length() > 0) {
      DynamicJsonDocument wDoc(512);
      if (deserializeJson(wDoc, wRaw) == DeserializationError::Ok) {
        wManual = (strcmp(wDoc["mode"] | "auto", "manual") == 0);
        pC = wDoc["provCode"] | ""; cC = wDoc["cityCode"] | ""; dC = wDoc["distCode"] | "";
        p  = wDoc["prov"]     | ""; c  = wDoc["city"]     | ""; d  = wDoc["dist"]     | "";
      }
    }
    JsonObject wObj = doc.createNestedObject("weather");
    wObj["mode"]     = wManual ? "manual" : "auto";
    wObj["provCode"] = pC;
    wObj["cityCode"] = cC;
    wObj["distCode"] = dC;
    wObj["prov"]     = p;
    wObj["city"]     = c;
    wObj["dist"]     = d;
  }
  {
    // 接口转发地址：本项是全工程唯一【没有 RAM 镜像】的配置，只能读 NVS。
    // 这是对上面"不要改成读 NVS 再导出"那条规则的有意例外 ——
    // 那条规则的目的是防止"某次保存失败让用户拿到不完整备份"，
    // 而本项是单值、无过滤条件、不存在部分写入，读 NVS 与读实时值完全等价。
    String rRaw;
    loadRelayConfigRaw(rRaw);
    String rUrl;
    if (rRaw.length() > 0) {
      DynamicJsonDocument rDoc(320);
      if (deserializeJson(rDoc, rRaw) == DeserializationError::Ok) {
        rUrl = (const char*)(rDoc["url"] | "");
      }
    }
    JsonObject rObj = doc.createNestedObject("relay");
    rObj["url"] = rUrl;                 // 字段名与 NVS 存储格式一致，导入时同构还原
  }
  {
    // 8) 天气系数过渡时长：设备端有 RAM 缓存（wxFadeSec()），直接取缓存值即可，无需读 NVS
    JsonObject fadeObj = doc.createNestedObject("wxFade");
    fadeObj["sec"] = wxFadeSec();
  }

  String jsonStr;
  serializeJsonPretty(doc, jsonStr);
  server.sendHeader("Content-Disposition", "attachment; filename=Espogic_Config.json");
  httpSend(200, "application/json", jsonStr);
}

// NVS 容量（供网页「NVS 数据」弹窗标题栏的容量条）。
// percent = 占用百分比（进度条填充）；剩余 = 100 - percent。
void handleNvsStats() {
  uint32_t used = 0, avail = 0, total = 0;
  DynamicJsonDocument doc(192);
  if (readNvsStats(used, avail, total)) {
    uint32_t denom = used + avail;
    doc["used"] = used;
    doc["available"] = avail;
    doc["total"] = total;
    doc["percent"] = denom ? (int)((used * 100UL + denom / 2) / denom) : 0;   // 四舍五入
  } else {
    doc["error"] = "nvs_get_stats failed";
  }
  String out;
  serializeJson(doc, out);
  httpSend(200, "application/json", out);
}

// 当前 RAM 里是否含"落在总线引脚上"的配置项（导入后给用户提示用）
static bool configHasBusPin() {
  for (int i = 0; i < pwmChannelCount; i++) if (isIoFlashPin(pwmPins[i])) return true;
  for (int g = 0; g < ioGroupCount; g++) if (isIoFlashPin(ioGroups[g].pin)) return true;
  for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) if (manualOverride[p] && isIoFlashPin(p)) return true;
  for (int g = 0; g < comparatorGateCount; g++) {
    for (int a = 0; a < comparatorGates[g].actionCount; a++) if (isIoFlashPin(comparatorGates[g].actions[a].pin)) return true;
    for (int i = 0; i < comparatorGates[g].inputCount; i++) {
      if (comparatorGates[g].inputs[i].source == COMPARATOR_SRC_EXTERNAL &&
          isIoFlashPin(comparatorGates[g].inputs[i].refIndex)) return true;
    }
  }
  for (int t = 0; t < counterTaskCount; t++) {
    if (isIoFlashPin(counterTasks[t].triggerPin)) return true;
    for (int a = 0; a < counterTasks[t].actionCount; a++) if (isIoFlashPin(counterTasks[t].actions[a].pin)) return true;
  }
  return false;
}

void handleImportConfig() {
  String err;
  if (!importGlobalConfig(server.arg("plain").c_str(), err)) {
    String resp;
    resp += "{\"success\":false,\"error\":\"";
    resp += err;
    resp += "\"}";
    httpSend(400, "application/json", resp);
    return;
  }
  // 若配置含总线（开发模式）引脚项，提醒用户：这些项不会进 NVS，重启后失效
  if (configHasBusPin()) {
    logRaw("[导入] 注意：配置含 Flash/PSRAM 总线引脚项 —— 它们不会保存到 NVS，重启后失效\n");
    httpSend(200, "application/json",
                "{\"success\":true,\"warn\":\"配置中 Flash/PSRAM 总线引脚的项不会保存，重启后失效\"}");
  } else {
    httpSend(200, "application/json", "{\"success\":true}");
  }
  // 重启前重新持锁：确保主 loop 不在 NVS 写入中途被重启打断（发送时锁已被 httpSend 释放）
  stateLock();
  delay(200);
  ESP.restart();
}

// ===== WiFi 配置接口 =====

// 异步扫描 WiFi（后台进行，不阻塞主循环）
void handleWifiScan() {
  DynamicJsonDocument doc(2048);
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED) {
    if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);   // 失败时重启扫描
    doc["scanning"] = true;
    String jsonStr;
    serializeJson(doc, jsonStr);
    httpSend(200, "application/json", jsonStr);
    return;
  }
  if (n < 0) {
    WiFi.scanNetworks(true);   // 首次调用启动异步扫描
    doc["scanning"] = true;
    String jsonStr;
    serializeJson(doc, jsonStr);
    httpSend(200, "application/json", jsonStr);
    return;
  }
  // n >= 0：扫描完成，返回结果（忽略信号弱于 -75dBm 的网络）
  JsonArray nets = doc.createNestedArray("networks");
  for (int i = 0; i < n; i++) {
    int rssi = WiFi.RSSI(i);
    if (rssi < -75) continue;
    JsonObject net = nets.createNestedObject();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = rssi;
    net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
  doc["scanning"] = false;
  WiFi.scanDelete();
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 保存 WiFi 并立即重启：重启后进入 STA 模式连接新 WiFi
void handleWifiSave() {
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) { httpSend(400, "application/json", "{\"error\":\"无效数据\"}"); return; }
  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  if (ssid.length() == 0) { httpSend(400, "application/json", "{\"error\":\"SSID不能为空\"}"); return; }
  wifiSsid = ssid;
  wifiPassword = password;
  if (!saveConfig()) {   // 写入失败则不重启，否则重启后配置必然丢失
    httpSend(500, "application/json", "{\"success\":false,\"reboot\":false,\"error\":\"配置保存失败(NVS损坏或已满)\"}");
    return;
  }
  httpSend(200, "application/json", "{\"success\":true,\"reboot\":true}");
  // 重启前重新持锁：确保主 loop 不在 NVS 写入中途被重启打断（发送时锁已被 httpSend 释放）
  stateLock();
  delay(200);
  ESP.restart();
}

// 合并状态接口：时间 + NTP + WiFi + 全部通道（前端轮询只需 1 个请求）
void handleGetStatus() {
  DynamicJsonDocument doc(4096);
  doc["hour"] = currentTime.hour;
  doc["minute"] = currentTime.minute;
  doc["second"] = currentTime.second;
  doc["syncInProgress"] = ntpSyncInProgress;
  doc["manualFinished"] = ntpManualFinished();
  doc["manualSuccess"] = ntpManualSuccess();
  doc["manualError"] = ntpManualError();
  if (isAPMode) {
    doc["wifiMode"] = "AP";
    doc["wifiSsid"] = String(AP_SSID);
    doc["ip"] = WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    doc["wifiMode"] = "STA";
    doc["wifiSsid"] = wifiSsid;
    doc["ip"] = WiFi.localIP().toString();
  } else {
    doc["wifiMode"] = "STA_CONNECTING";
    doc["wifiSsid"] = wifiSsid;
    doc["ip"] = "";
  }
  doc["hostname"] = String(MDNS_HOSTNAME) + ".local";
  doc["chipModel"] = getEffectiveChipModel();
  doc["chipTemp"] = temperatureRead();
  doc["weatherFactor"] = weatherGetDutyFactor();   // 天气占空比系数(%)，供通道卡片显示「大气交互70%(阴 🌧)」
  // 落盘失败提示：保存发生在 loop 的防抖回调里，HTTP 早已返回成功，只能靠这个字段让前端看见
  doc["saveFail"] = saveFailSummary();
  JsonArray channels = doc.createNestedArray("channels");
  for (int i = 0; i < pwmChannelCount; i++) {
    JsonObject channel = channels.createNestedObject();
    channel["index"] = i;
    channel["currentDuty"] = pwmConfigs[i].currentDuty;
    channel["activeTask"] = pwmConfigs[i].activeTask;
    channel["enabled"] = pwmConfigs[i].enabled;
    channel["weatherInteract"] = pwmConfigs[i].weatherInteract;
    channel["weatherDuty"] = pwmConfigs[i].weatherDuty;
    // 阳光交互运行态：sunActive=当前由阳光曲线接管；sunDuty=阳光算出的原始占空比
    channel["sunEnabled"] = pwmConfigs[i].sunEnabled;
    channel["sunMode"] = pwmConfigs[i].sunMode;
    channel["sunPeakDuty"] = pwmConfigs[i].sunPeakDuty;
    channel["sunActive"] = pwmConfigs[i].sunActive;
    channel["sunDuty"] = pwmConfigs[i].sunDuty;
    // 变轨中：该通道输出正在做天气系数过渡（多久结束由 rampRemain 秒给出）
    channel["ramping"] = pwmIsRamping(i);
    channel["rampRemain"] = pwmRampRemainSec();
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 增量拉取串口日志：GET /getlogs?seq=N，返回 seq>N 的日志（文本，逐行 "seq|内容"）
void handleGetLogs() {
  uint32_t fromSeq = 0;
  if (server.hasArg("seq")) {
    fromSeq = (uint32_t)strtoul(server.arg("seq").c_str(), nullptr, 10);
  }
  String out;
  logGetSince(fromSeq, out);
  // 追加当前最新 seq 供前端续传
  out += "END|";
  out += String(logGetSeq());
  httpSend(200, "text/plain", out);
}

// 记录未匹配路径：手机连入热点后的 captive portal 探测
void handleNotFound() {
  logf("[HTTP] 404 %s %s (uptime=%lums)\n",
                (server.method() == HTTP_GET ? "GET" : "POST"),
                server.uri().c_str(), millis());
  httpSend(404, "application/json", "{\"error\":\"not found\"}");
}

// ===== 序列器接口 =====

void handleIoGetConfig() {
  logf("[HTTP] 序列器配置查询: 当前 %d 组\n", ioGroupCount);
  DynamicJsonDocument doc(IO_CONFIG_DOC_SIZE);   // 满配 20 组 × 20 条（含模式与毫秒时长）约 23KB
  JsonArray groups = doc.createNestedArray("groups");
  // 分配失败时 groups 为空数组，若照常返回会让前端显示成"暂无序列"（用户会误以为配置丢了）。
  // 明确报错，前端 loadIoConfig 的 catch 会保留上一份配置，不误导。
  if (groups.isNull()) {
    logRaw("[HTTP] 序列器配置查询失败：JSON 文档分配异常\n");
    httpSend(500, "application/json", "{\"success\":false,\"error\":\"内存不足，请稍后重试\"}");
    return;
  }
  for (int g = 0; g < ioGroupCount; g++) {
    IoGroup& grp = ioGroups[g];
    JsonObject gobj = groups.createNestedObject();
    gobj["name"] = grp.name;
    gobj["pin"] = grp.pin;
    gobj["enabled"] = grp.enabled;
    bool grpLocked = latchEnabled[grp.pin];
    JsonArray arr = gobj.createNestedArray("entries");
    for (int j = 0; j < grp.entryCount; j++) {
      const IoEntry& e = grp.entries[j];
      JsonObject eobj = arr.createNestedObject();
      eobj["hour"] = e.hour;
      eobj["minute"] = e.minute;
      eobj["action"] = e.action ? 1 : 0;
      eobj["mode"] = e.mode;
      eobj["pulseMs"] = e.pulseMs;
      eobj["hbHighMs"] = e.hbHighMs;
      eobj["hbLowMs"] = e.hbLowMs;
      eobj["conflict"] = e.conflict;
      eobj["noPerm"] = grpLocked;   // 整组被锁存 → 所有条目无权限
    }
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleIoAddGroup() {
  String plain = server.arg("plain");
  logf("[HTTP] 序列器添加序列: plain=[%s] len=%d\n", plain.c_str(), plain.length());
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, plain)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  String name = doc["name"] | "";
  int pin = doc["pin"] | -1;
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (!ioAddGroup(name.c_str(), pin, devMode, allowPwm, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：引脚不可用/已被占用/组数已满\"}");
    return;
  }
  requestIoConfigSave();
  requestComparatorConfigSave();   // 比较器内部输入引用已后移
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleIoRemoveGroup() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (!ioRemoveGroup(index)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效序列\"}");
    return;
  }
  requestIoConfigSave();
  requestComparatorConfigSave();   // ioRemoveGroup 内部已修正比较器内部输入引用，需一并落盘
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleIoUpdateGroup() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  String name = doc["name"] | "";
  int pin = doc["pin"] | -1;
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (!ioUpdateGroup(index, name.c_str(), pin, devMode, allowPwm, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"编辑失败：引脚不可用/已被占用\"}");
    return;
  }
  requestIoConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleIoToggle() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (index < 0 || index >= ioGroupCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效序列\"}"); return;
  }
  ioGroups[index].enabled = !ioGroups[index].enabled;
  // 暂态一并清掉：否则关组再开组时若仍在脉冲 deadline 内，会立刻吐出一次脉冲尾巴
  ioResetOutput();
  requestIoConfigSave();
  StaticJsonDocument<64> r;
  r["success"] = true;
  r["enabled"] = ioGroups[index].enabled;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleIoAddEntry() {
  DynamicJsonDocument doc(384);   // 请求体含 8 个字段（原 128 装不下，会解析失败导致新字段存不进去）
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int groupIndex = doc["groupIndex"] | -1;
  int hour = doc["hour"] | 0;
  int minute = doc["minute"] | 0;
  bool action = (doc["action"] | 0) != 0;
  int mode = doc["mode"] | IO_MODE_SCHEDULE;
  uint32_t pulseMs = (uint32_t)(doc["pulseMs"] | 1000);
  uint32_t hbHighMs = (uint32_t)(doc["hbHighMs"] | 500);
  uint32_t hbLowMs = (uint32_t)(doc["hbLowMs"] | 500);
  // 先校验索引：否则 ioCanSetMode 对非法索引也返回 false，会弹出"已有条目/已有心跳"这种无关原因
  if (groupIndex < 0 || groupIndex >= ioGroupCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效序列\"}");
    return;
  }
  // 持久心跳在序列内独占：提前判断以便给出确切原因（ioAddEntry 内部也会再挡一次）
  if (!ioCanSetMode(groupIndex, -1, mode)) {
    httpSend(400, "application/json",
                ioNormalizeMode(mode) == IO_MODE_HEARTBEAT
                  ? "{\"success\":false,\"error\":\"该序列已有条目，持久心跳必须独占序列\"}"
                  : "{\"success\":false,\"error\":\"该序列已有持久心跳，不能再添加其他条目\"}");
    return;
  }
  if (!ioAddEntry(groupIndex, hour, minute, action, mode, pulseMs, hbHighMs, hbLowMs)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：条目数已满或序列无效\"}");
    return;
  }
  requestIoConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleIoRemoveEntry() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int groupIndex = doc["groupIndex"] | -1;
  int entryIndex = doc["entryIndex"] | -1;
  if (!ioRemoveEntry(groupIndex, entryIndex)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效条目\"}");
    return;
  }
  requestIoConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleIoUpdateEntry() {
  DynamicJsonDocument doc(384);   // 请求体含 8 个字段（原 128 装不下）
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int groupIndex = doc["groupIndex"] | -1;
  int entryIndex = doc["entryIndex"] | -1;
  int hour = doc["hour"] | 0;
  int minute = doc["minute"] | 0;
  bool action = (doc["action"] | 0) != 0;
  int mode = doc["mode"] | IO_MODE_SCHEDULE;
  uint32_t pulseMs = (uint32_t)(doc["pulseMs"] | 1000);
  uint32_t hbHighMs = (uint32_t)(doc["hbHighMs"] | 500);
  uint32_t hbLowMs = (uint32_t)(doc["hbLowMs"] | 500);
  // 先校验索引：否则 ioCanSetMode 对非法索引也返回 false，会弹出"已有条目/已有心跳"这种无关原因
  if (groupIndex < 0 || groupIndex >= ioGroupCount ||
      entryIndex < 0 || entryIndex >= ioGroups[groupIndex].entryCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效条目\"}");
    return;
  }
  // 持久心跳在序列内独占：提前判断以便给出确切原因（ioUpdateEntry 内部也会再挡一次）
  if (!ioCanSetMode(groupIndex, entryIndex, mode)) {
    httpSend(400, "application/json",
                ioNormalizeMode(mode) == IO_MODE_HEARTBEAT
                  ? "{\"success\":false,\"error\":\"该序列还有其他条目，持久心跳必须独占序列\"}"
                  : "{\"success\":false,\"error\":\"该序列已有持久心跳，不能再改为其他模式\"}");
    return;
  }
  if (!ioUpdateEntry(groupIndex, entryIndex, hour, minute, action, mode, pulseMs, hbHighMs, hbLowMs)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"编辑失败：无效条目\"}");
    return;
  }
  requestIoConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 调度面板：返回所有可用 GPIO 引脚的电平与手动覆盖状态
void handleIoStatus() {
  DynamicJsonDocument doc(8192);
  JsonArray pins = doc.createNestedArray("pins");
  for (int i = 0; i < getAvailablePinCount(); i++) {
    int pin = getAvailablePin(i);
    JsonObject o = pins.createNestedObject();
    o["pin"] = pin;
    bool isPwm = ioIsPinUsedByPwm(pin);
    bool isFlash = isIoFlashPin(pin);
    o["manual"] = manualIsOverridden(pin);
    o["latch"] = latchIsEnabled(pin);
    o["latchLevel"] = latchLevel[pin];
    if (isPwm && !manualOverride[pin] && !latchEnabled[pin]) {
      o["level"] = -1;   // PWM 正常输出时无高低电平概念，显示 PWM 标签
    } else {
      o["level"] = gpioGetCurrentLevel(pin);
    }
    o["pwm"] = isPwm;
    o["flash"] = isFlash;
    o["input"] = pinIsReadOnlyInput(pin);   // 只读输入（比较器外部输入/纯输入型触发源）：面板置灰
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 调度面板：手动切换某引脚电平（进入手动覆盖）
void handleIoSetLevel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int pin = doc["pin"] | -1;
  int level = (doc["level"] | 0) ? 1 : 0;
  bool devMode = doc["devMode"].as<bool>();
  bool allowPwm = doc["allowPwm"].as<bool>();
  if (latchIsEnabled(pin)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚已锁存，请先解除锁存\"}");
    return;
  }
  if (pinIsReadOnlyInput(pin)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚为只读输入（比较器外部输入/计数器触发源），不能手动切换\"}");
    return;
  }
  if (!manualSetLevel(pin, level, devMode, allowPwm)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"切换失败：无效引脚或需开发模式/PWM 开关\"}");
    return;
  }
  StaticJsonDocument<64> r;
  r["success"] = true;
  r["level"] = manualLevel[pin];
  r["manual"] = true;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 调度面板：恢复自动。带 pin 只恢复该引脚，不带则恢复全部。
// 锁存的引脚拒绝：锁存优先级最高，必须先解除锁存（前端也会禁用该按钮）。
void handleIoAutoReset() {
  DynamicJsonDocument doc(128);
  bool parsed = !deserializeJson(doc, server.arg("plain"));
  int pin = parsed ? (doc["pin"] | -1) : -1;
  if (pin >= 0) {
    if (!isPinAvailable(pin)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效引脚\"}");
      return;
    }
    if (latchIsEnabled(pin)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚已锁存，请先解除锁存\"}");
      return;
    }
    manualClearPin(pin);
    logf("[手动] GPIO%d 恢复自动\n", pin);
  } else {
    manualClearAll();
  }
  httpSend(200, "application/json", "{\"success\":true}");
}

// ===== 锁存接口（最高权限）=====

// 调度面板：锁存某引脚电平（覆盖手动覆盖/PWM/IO/比较器/计数器）
void handleLatchSet() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int pin = doc["pin"] | -1;
  int level = (doc["level"] | 0) ? 1 : 0;
  bool devMode = doc["devMode"].as<bool>();
  bool allowPwm = doc["allowPwm"].as<bool>();
  if (pinIsReadOnlyInput(pin)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚为只读输入（比较器外部输入/计数器触发源），不能锁存\"}");
    return;
  }
  if (!latchSet(pin, level, devMode, allowPwm)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"锁存失败：无效引脚或需开发模式/PWM 开关\"}");
    return;
  }
  StaticJsonDocument<64> r;
  r["success"] = true;
  r["level"] = latchLevel[pin];
  r["latch"] = true;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 调度面板：解除某引脚锁存
void handleLatchClear() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int pin = doc["pin"] | -1;
  if (!latchClear(pin)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"解除失败：无效引脚\"}");
    return;
  }
  httpSend(200, "application/json", "{\"success\":true}");
}

// ===== 比较器接口 =====

void handleComparatorGetConfig() {
  logf("[HTTP] 比较器配置查询: 当前 %d 个\n", comparatorGateCount);
  DynamicJsonDocument doc(8192);
  JsonArray gates = doc.createNestedArray("gates");
  for (int g = 0; g < comparatorGateCount; g++) {
    ComparatorGate& gate = comparatorGates[g];
    JsonObject gobj = gates.createNestedObject();
    gobj["name"] = gate.name;
    gobj["enabled"] = gate.enabled;
    gobj["mode"] = gate.mode;
    JsonArray inputs = gobj.createNestedArray("inputs");
    for (int i = 0; i < gate.inputCount; i++) {
      JsonObject iobj = inputs.createNestedObject();
      iobj["source"] = gate.inputs[i].source;
      iobj["refIndex"] = gate.inputs[i].refIndex;
      iobj["actionIndex"] = gate.inputs[i].actionIndex;
      iobj["expectedLevel"] = gate.inputs[i].expectedLevel;
    }
    JsonArray actions = gobj.createNestedArray("actions");
    for (int a = 0; a < gate.actionCount; a++) {
      JsonObject aobj = actions.createNestedObject();
      aobj["pin"] = gate.actions[a].pin;
      aobj["sourceLevel"] = gate.actions[a].sourceLevel;
      aobj["actionLevel"] = gate.actions[a].actionLevel;
      aobj["noPerm"] = latchEnabled[gate.actions[a].pin] && (gate.actions[a].actionLevel != latchLevel[gate.actions[a].pin]);
    }
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleComparatorGateAdd() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  String name = doc["name"] | "";
  int mode = doc["mode"] | COMPARATOR_MODE_AND;
  if (!comparatorAddGate(name.c_str(), mode)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：数量已达上限\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorGateDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (!comparatorRemoveGate(index)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效任务\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorGateUpdate() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  String name = doc["name"] | "";
  if (!comparatorUpdateGate(index, name.c_str())) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"编辑失败：无效任务\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorGateMode() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  int mode = doc["mode"] | COMPARATOR_MODE_AND;
  if (!comparatorSetGateMode(index, mode)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"修改失败：无效任务或模式\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorToggle() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (index < 0 || index >= comparatorGateCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效任务\"}"); return;
  }
  comparatorGates[index].enabled = !comparatorGates[index].enabled;
  requestComparatorConfigSave();
  StaticJsonDocument<64> r;
  r["success"] = true;
  r["enabled"] = comparatorGates[index].enabled;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleComparatorInputAdd() {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int gateIndex = doc["gateIndex"] | -1;
  int source = doc["source"] | 0;
  int refIndex = doc["refIndex"] | -1;
  int actionIndex = doc["actionIndex"] | 0;
  int expectedLevel = doc["expectedLevel"] | 1;
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  bool allowDup = doc["allowDup"] | false;
  // 只读输入引脚：外部输入脚不能被锁存/手动接管（模块层同样拒绝，这里给出可读原因）
  if (source == COMPARATOR_SRC_EXTERNAL) {
    if (latchIsEnabled(refIndex)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚已锁存，请前往调度面板解除锁存\"}");
      return;
    }
    if (manualIsOverridden(refIndex)) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚正被手动切换，请先在调度面板恢复自动\"}");
      return;
    }
  }
  if (!comparatorAddInput(gateIndex, source, refIndex, actionIndex, expectedLevel, devMode, allowPwm, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：来源无效或引脚被占用\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorInputDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int gateIndex = doc["gateIndex"] | -1;
  int inputIndex = doc["inputIndex"] | -1;
  if (!comparatorRemoveInput(gateIndex, inputIndex)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效输入\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorOutputAdd() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int gateIndex = doc["gateIndex"] | -1;
  int pin = doc["pin"] | -1;
  int sourceLevel = doc["sourceLevel"] | 0;
  int actionLevel = doc["actionLevel"] | 1;
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (!comparatorAddAction(gateIndex, pin, sourceLevel, actionLevel, devMode, allowPwm, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：引脚不可用或已占用\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleComparatorOutputDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int gateIndex = doc["gateIndex"] | -1;
  int actionIndex = doc["actionIndex"] | -1;
  if (!comparatorRemoveAction(gateIndex, actionIndex)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效输出\"}");
    return;
  }
  requestComparatorConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// ===== 计数器接口 =====

void handleCounterGetConfig() {
  DynamicJsonDocument doc(8192);
  JsonArray tasks = doc.createNestedArray("tasks");
  for (int t = 0; t < counterTaskCount; t++) {
    CounterTask& task = counterTasks[t];
    JsonObject tobj = tasks.createNestedObject();
    tobj["name"] = task.name;
    tobj["enabled"] = task.enabled;
    tobj["triggerPin"] = task.triggerPin;
    tobj["threshold"] = task.threshold;
    tobj["triggerCount"] = task.triggerCount;
    tobj["fired"] = task.fired;
    JsonArray actions = tobj.createNestedArray("actions");
    for (int a = 0; a < task.actionCount; a++) {
      JsonObject aobj = actions.createNestedObject();
      aobj["pin"] = task.actions[a].pin;
      aobj["level"] = task.actions[a].level;
      aobj["holdMode"] = task.actions[a].holdMode;
      aobj["holdSec"] = task.actions[a].holdSec;
      aobj["noPerm"] = latchEnabled[task.actions[a].pin] && (task.actions[a].level != latchLevel[task.actions[a].pin]);
      long remain = -1;
      if (task.actions[a].holdMode == COUNTER_HOLD_AUTO_OFF && task.enabled && task.fired) {
        if (millis() < task.autoOffUntil[a]) {
          remain = (long)((task.autoOffUntil[a] - millis() + 999UL) / 1000UL);   // 锁存剩余秒数（向上取整）
        } else {
          remain = 0;
        }
      }
      aobj["remain"] = remain;
    }
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// 计量面板：返回所有可用 GPIO 的切变计数 + 当前电平 + 占用标记
void handleCounterStatus() {
  DynamicJsonDocument doc(8192);
  doc["countPwm"] = counterCountPwm;
  doc["countFlash"] = counterCountFlash;
  JsonArray pins = doc.createNestedArray("pins");
  for (int i = 0; i < getAvailablePinCount(); i++) {
    int pin = getAvailablePin(i);
    JsonObject o = pins.createNestedObject();
    o["pin"] = pin;
    o["count"] = ioToggleCount[pin];
    bool isPwm = ioIsPinUsedByPwm(pin);
    bool isFlash = isIoFlashPin(pin);
    if (isPwm) {
      // 默认不统计 PWM 时电平无意义，固定 -1；开发模式开启统计时恢复真实电平
      o["level"] = counterCountPwm ? gpioGetCurrentLevel(pin) : -1;
    } else if (isFlash) {
      // 默认不统计 Flash 时固定 -1；开发模式开启统计时恢复真实电平
      o["level"] = counterCountFlash ? gpioGetCurrentLevel(pin) : -1;
    } else {
      o["level"] = gpioGetCurrentLevel(pin);
    }
    o["pwm"] = isPwm;
    o["flash"] = isFlash;
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleCounterClear() {
  counterClearAllCounts();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 清除单个引脚的切变计数
void handleCounterClearPin() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int pin = doc["pin"] | -1;
  counterClearPinCount(pin);
  httpSend(200, "application/json", "{\"success\":true}");
}

// 计量面板开发模式：设置是否统计 PWM / Flash 引脚的切变
void handleCounterDevMode() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  counterCountPwm = doc["countPwm"].as<bool>();
  counterCountFlash = doc["countFlash"].as<bool>();
  logf("[计数器] 开发模式：统计PWM=%d, 统计Flash=%d\n", counterCountPwm, counterCountFlash);
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterTaskAdd() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  String name = doc["name"] | "";
  if (!counterAddTask(name.c_str())) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：数量已达上限\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterTaskDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (!counterRemoveTask(index)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效任务\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterTaskUpdate() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  String name = doc["name"] | "";
  if (!counterUpdateTask(index, name.c_str())) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"编辑失败：无效任务\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterToggle() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  if (index < 0 || index >= counterTaskCount) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效任务\"}"); return;
  }
  counterTasks[index].enabled = !counterTasks[index].enabled;
  if (!counterTasks[index].enabled) {
    // 关闭时清零触发状态，重开从零开始
    counterTasks[index].triggerCount = 0;
    counterTasks[index].fired = false;
    for (int a = 0; a < COUNTER_ACTION_MAX; a++) counterTasks[index].autoOffUntil[a] = 0;
  }
  requestCounterConfigSave();
  StaticJsonDocument<64> r;
  r["success"] = true;
  r["enabled"] = counterTasks[index].enabled;
  String jsonStr;
  serializeJson(r, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleCounterTriggerSet() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int index = doc["index"] | -1;
  int pin = doc["pin"] | -1;
  int threshold = doc["threshold"] | 1;
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  // 只读输入引脚：触发源脚不能被锁存/手动接管（模块层同样拒绝，这里给出可读原因）
  if (latchIsEnabled(pin)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚已锁存，请前往调度面板解除锁存\"}");
    return;
  }
  if (manualIsOverridden(pin)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"该引脚正被手动切换，请先在调度面板恢复自动\"}");
    return;
  }
  if (!counterSetTrigger(index, pin, threshold, devMode, allowPwm)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"设置失败：引脚不可用或被占用\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterActionAdd() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int taskIndex = doc["taskIndex"] | -1;
  int pin = doc["pin"] | -1;
  int level = doc["level"] | 1;
  int holdMode = doc["holdMode"] | 0;
  uint32_t holdSec = (uint32_t)(doc["holdSec"] | 0);
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (!counterAddAction(taskIndex, pin, level, holdMode, holdSec, devMode, allowPwm, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"添加失败：引脚不可用或已占用\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterActionDel() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int taskIndex = doc["taskIndex"] | -1;
  int actionIndex = doc["actionIndex"] | -1;
  if (!counterRemoveAction(taskIndex, actionIndex)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"删除失败：无效输出\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleCounterActionUpdate() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  int taskIndex = doc["taskIndex"] | -1;
  int actionIndex = doc["actionIndex"] | -1;
  int pin = doc["pin"] | -1;
  int level = doc["level"] | 1;
  int holdMode = doc["holdMode"] | 0;
  uint32_t holdSec = (uint32_t)(doc["holdSec"] | 0);
  bool devMode = doc["devMode"] | false;
  bool allowPwm = doc["allowPwm"] | false;
  bool allowDup = doc["allowDup"] | false;
  if (!counterUpdateAction(taskIndex, actionIndex, pin, level, holdMode, holdSec, devMode, allowPwm, allowDup)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"编辑失败：引脚不可用或已占用\"}");
    return;
  }
  requestCounterConfigSave();
  httpSend(200, "application/json", "{\"success\":true}");
}

// ===== 中心面板（只读监控 + 触发归因）=====

void handleCentralStatus() {
  DynamicJsonDocument doc(16384);
  centralBuildStatus(doc);
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleCentralDetail() {
  int pin = server.arg("pin").toInt();
  DynamicJsonDocument doc(8192);
  centralBuildDetail(doc, pin);
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

// ===== 恢复出厂 / 重置配置 =====

void handleFactoryReset() {
  logf("[HTTP] 恢复出厂设置\n");
  if (!factoryReset()) {
    httpSend(500, "application/json", "{\"success\":false,\"error\":\"恢复出厂失败\"}");
    return;
  }
  httpSend(200, "application/json", "{\"success\":true,\"reboot\":true}");
  // 重启前重新持锁：确保主 loop 不在 NVS 写入中途被重启打断（发送时锁已被 httpSend 释放）
  stateLock();
  delay(200);
  ESP.restart();
}

void handleResetConfig() {
  logf("[HTTP] 重置配置（保留 WiFi）\n");
  if (!resetConfigKeepWifi()) {
    httpSend(500, "application/json", "{\"success\":false,\"error\":\"重置配置失败\"}");
    return;
  }
  httpSend(200, "application/json", "{\"success\":true,\"reboot\":true}");
  // 重启前重新持锁：确保主 loop 不在 NVS 写入中途被重启打断（发送时锁已被 httpSend 释放）
  stateLock();
  delay(200);
  ESP.restart();
}

// ===== 芯片信息与版本固定接口 =====

void handleChipInfo() {
  // 2048：原 1024 放不下新增的资源字段。按 14 个新数字字段 + 型号列表估算，
  // 1024 那点余量刚好被吃干净，直接给足 —— 本函数只在打开弹窗时调用，堆开销可忽略。
  DynamicJsonDocument doc(2048);
  doc["model"] = getEffectiveChipModel();          // 生效型号
  doc["realModel"] = ESP.getChipModel();           // 自动识别型号
  doc["forced"] = forcedChipModel.length() > 0;    // 是否已手动固定
  doc["revision"] = (int)ESP.getChipRevision();
  doc["cores"] = ESP.getChipCores();
  doc["cpuFreq"] = ESP.getCpuFreqMHz();
  doc["flashSize"] = ESP.getFlashChipSize();
  doc["psramSize"] = ESP.getPsramSize();
  doc["temperature"] = temperatureRead();

  // ===== 资源占用 =====
  // 与芯片信息同属"看一眼设备状态"，合并进本接口：前端打开弹窗一次就能全拿到，
  // 也便于弹窗开着时按秒轮询（不必再多开一个接口）。
  doc["heapTotal"] = ESP.getHeapSize();
  doc["heapFree"]  = ESP.getFreeHeap();
  doc["heapMin"]   = ESP.getMinFreeHeap();    // 开机至今最低水位：判断有没有曾经濒临耗尽
  doc["psramTotal"] = ESP.getPsramSize();
  doc["psramFree"]  = ESP.getFreePsram();
  uint32_t nvsUsed = 0, nvsAvail = 0, nvsTotal = 0;
  if (readNvsStats(nvsUsed, nvsAvail, nvsTotal)) {
    // 单位是【条目】不是字节，每条 32 字节（前端按此换算成 KB 显示）。
    // 百分比的分母必须是 used+avail，不能用 total —— NVS 会留页给垃圾回收，
    // 用 total 会得出一个偏小、且加不到 100% 的占用率（理由见 readNvsStats 注释）。
    uint32_t denom = nvsUsed + nvsAvail;
    doc["nvsUsed"]    = nvsUsed;
    doc["nvsAvail"]   = nvsAvail;
    doc["nvsTotal"]   = nvsTotal;
    doc["nvsPercent"] = denom ? (int)((nvsUsed * 100UL + denom / 2) / denom) : 0;   // 已用百分比
  }
  doc["loopRate"]   = loopLoadRate();     // 主循环迭代率（次/秒）
  doc["loopLoad"]   = loopLoadPercent();  // 负载率(%)：相对本机最空闲时的被抢占占比
  doc["loopMaxMs"]  = loopLoadMaxMs();    // 最近 1 秒最大间隔
  doc["loopStalls"] = loopLoadStalls();   // 最近 1 秒 >50ms 次数

  JsonArray models = doc.createNestedArray("models");
  for (int i = 0; i < CHIP_MODEL_COUNT; i++) models.add(CHIP_MODELS[i]);
  String jsonStr;
  serializeJson(doc, jsonStr);
  httpSend(200, "application/json", jsonStr);
}

void handleChipSet() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}"); return;
  }
  const char* model = doc["model"] | "";
  if (!setForcedChipModel(model)) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"固定失败：型号无效或 NVS 写入失败\"}");
    return;
  }
  httpSend(200, "application/json", "{\"success\":true}");
}

void handleChipClear() {
  clearForcedChipModel();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 读取接口转发地址（天气第三级兜底用；空 = 关闭该级）
void handleRelayGet() {
  String json;
  loadRelayConfigRaw(json);
  if (json.length() == 0) {
    httpSend(200, "application/json", "{\"url\":\"\"}");
    return;
  }
  httpSend(200, "application/json", json);
}

// 接口转发地址校验。实现落在 web_server 侧（这里），但声明在 config.h ——
// 「保存」与「测试联通」在下面调用它，导入备份文件时 config_store.ino 也调用同一份实现。
// 三条路径必须共用一套规则：漏检会造成"能存进去却用不通"，反向则"测过了却存不了"。
String relayUrlCheck(const String& url) {
  // 长度上限 192 不是随便定的：weatherDoForecast 里的 url 缓冲区按 512 设计，
  // 按"转发地址 + 查询串(约 251)"算出来的可用上限。这里必须卡住，否则拼接会静默截断。
  if (url.length() > 192) return "地址过长（最多 192 字符）";
  if (url.length() > 0 && !url.startsWith("http://") && !url.startsWith("https://"))
    return "地址需以 http:// 或 https:// 开头";
  return String();
}

// 保存接口转发地址。空串 = 关闭该级。
void handleRelaySave() {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false}");
    return;
  }
  String url = doc["url"] | "";
  url.trim();
  String err = relayUrlCheck(url);
  if (err.length() > 0) {
    StaticJsonDocument<192> r;
    r["success"] = false;
    r["msg"] = err;
    String js;
    serializeJson(r, js);
    httpSend(400, "application/json", js);
    return;
  }
  saveRelayConfig(url.c_str());
  httpSend(200, "application/json", "{\"success\":true}");
}

// 「测试联通」：只触发、不同步发请求，结果由 /relayteststatus 轮询。
// 绝不能在这里同步发请求 —— 那会持着 stateMutex 阻塞十几秒，把主循环一起冻住
//（与 handleSyncNTPTime 只发起 weatherArmStartup 而不阻塞同步，是同一个道理）。
void handleRelayTest() {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"msg\":\"请求格式错误\"}");
    return;
  }
  String url = doc["url"] | "";
  url.trim();

  String err = relayUrlCheck(url);        // 与保存共用同一份规则，避免"能测不能存"
  if (err.length() > 0 || url.length() == 0) {
    StaticJsonDocument<192> r;
    r["success"] = false;
    r["msg"] = err.length() > 0 ? err : String("请先填写地址");
    String js;
    serializeJson(r, js);
    httpSend(200, "application/json", js);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    httpSend(200, "application/json", "{\"success\":false,\"msg\":\"WiFi未连接\"}");
    return;
  }
  // 测的是输入框里这份地址（可能还没保存），所以走独立的 wTestUrl，不碰 NVS 里已存的值
  if (!weatherRelayTestStart(url)) {
    httpSend(200, "application/json", "{\"success\":false,\"msg\":\"上一次测试还在进行中\"}");
    return;
  }
  httpSend(200, "application/json", "{\"success\":true,\"testing\":true}");
}

// 轮询测试结果：state 0=未测 1=进行中 2=成功 3=失败
void handleRelayTestStatus() {
  int st = weatherRelayTestState();
  int32_t ms = (int32_t)weatherRelayTestMs();
  float tp = weatherRelayTestTemp();
  const char* msg = weatherRelayTestMsg();
  StaticJsonDocument<256> doc;
  doc["state"]   = st;
  doc["running"] = (st == 1);
  doc["done"]    = (st == 2 || st == 3);
  doc["ok"]      = (st == 2);
  doc["code"]    = weatherRelayTestCode();
  doc["ms"]      = ms;
  doc["temp"]    = (st == 2) ? tp : -999.0f;
  doc["msg"]     = msg ? msg : "";
  String js;
  serializeJson(doc, js);
  httpSend(200, "application/json", js);
}

// 读取天气定位配置（模式 + 手动位置）
void handleWeatherGet() {
  String json;
  loadWeatherConfigRaw(json);
  if (json.length() == 0) {
    httpSend(200, "application/json", "{\"mode\":\"auto\"}");
    return;
  }
  httpSend(200, "application/json", json);
}

// 保存天气定位配置。mode=manual 时保存手动位置（省市区 code + 中文名）；
// mode=auto 时仅保存模式（同时清空手动位置，实现「切换网络定位清除手动记录」）。
void handleWeatherSave() {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false}");
    return;
  }
  bool manual = (strcmp(doc["mode"] | "auto", "manual") == 0);
  saveWeatherConfig(manual,
                    doc["provCode"] | "", doc["cityCode"] | "", doc["distCode"] | "",
                    doc["prov"] | "", doc["city"] | "", doc["dist"] | "");
  weatherStart();   // 配置已更新，重新抓取天气
  httpSend(200, "application/json", "{\"success\":true}");
}

// 返回后端天气缓存状态（位置 + 天气），前端只读
void handleWeatherStatus() {
  DynamicJsonDocument doc(1024);
  weatherBuildStatus(doc);
  String json; serializeJson(doc, json);
  httpSend(200, "application/json", json);
}

// 「变轨中」弹窗的「立即完成」：把正在进行的天气系数过渡直接落到目标档位。
// 系数全局共用，所以没有通道参数 —— 一按就是所有通道一起到位。
void handlePwmFadeComplete() {
  pwmCompleteFade();
  httpSend(200, "application/json", "{\"success\":true}");
}

// 读取天气系数过渡时长：0 = 关闭过渡（天气档位切换时立即切换）。
// 存储单位是秒（NVS），maxMin 给前端的分钟输入框设上限（单位换算只在前端做）。
void handleWxFadeGet() {
  StaticJsonDocument<128> doc;
  doc["sec"] = wxFadeSec();
  doc["min"] = wxFadeSec() / 60;
  doc["maxMin"] = WX_FADE_MAX_SEC / 60;
  String out;
  serializeJson(doc, out);
  httpSend(200, "application/json", out);
}

// 保存天气系数过渡时长（秒）。范围由 saveWxFadeConfig 内部夹紧到 [0, WX_FADE_MAX_SEC]，
// 保存后立即生效（RAM 缓存同步更新，无需重启）。
void handleWxFadeSave() {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}");
    return;
  }
  int sec = doc["sec"] | wxFadeSec();
  int clamped = safeConstrain(sec, 0, WX_FADE_MAX_SEC);
  if (!saveWxFadeConfig(clamped)) {
    httpSend(500, "application/json", "{\"success\":false,\"error\":\"配置保存失败(NVS损坏或已满)\"}");
    return;
  }
  StaticJsonDocument<128> r;
  r["success"] = true;
  r["sec"] = wxFadeSec();
  String out;
  serializeJson(r, out);
  httpSend(200, "application/json", out);
}

// 读取时区配置：mode(auto/manual) + 已保存偏移 + 当前生效偏移
void handleGetTz() {
  DynamicJsonDocument doc(256);
  bool autoMode = true;
  long saved = (long)tzOffsetSec;
  String raw;
  loadTzConfigRaw(raw);
  if (raw.length() > 0) {
    DynamicJsonDocument cfg(128);
    if (deserializeJson(cfg, raw) == DeserializationError::Ok) {
      autoMode = (strcmp(cfg["mode"] | "auto", "auto") == 0);
      saved    = cfg["offset"] | (long)tzOffsetSec;
    }
  }
  doc["mode"]    = autoMode ? "auto" : "manual";
  doc["offset"]  = saved;               // 已保存偏移（手动模式下拉回填用）
  doc["current"] = (long)tzOffsetSec;   // 当前生效偏移（自动模式下可能刚被定位更新）
  String out;
  serializeJson(doc, out);
  httpSend(200, "application/json", out);
}

// 保存时区配置。auto 时偏移由定位链填充（只落盘当前值作为兜底），manual 时校验后立即生效。
void handleSaveTz() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"无效数据\"}");
    return;
  }
  // AP 模式无外网、NTP 永不执行 → lastSyncEpoch 恒为 0 → updateTime 走"直接自增时分秒"
  // 的分支，时区偏移根本不参与计算。所以时区设置在 AP 下毫无意义，直接拒绝。
  if (isAPMode) {
    httpSend(400, "application/json", "{\"success\":false,\"error\":\"AP 模式不支持时区设置\"}");
    return;
  }
  // 先读旧模式：只有"从手动切回自动"才需要重新定位（自动模式的偏移由定位刷新）。
  // 已经处于自动模式时再保存一次，模式与偏移都不会变，重抓整条链路纯属浪费
  // （一条完整链路约 5 秒 HTTPS，还会让堆内存掉一截）。
  bool oldAuto = true;
  {
    String raw;
    loadTzConfigRaw(raw);
    if (raw.length() > 0) {
      DynamicJsonDocument cfg(128);
      if (deserializeJson(cfg, raw) == DeserializationError::Ok) {
        oldAuto = (strcmp(cfg["mode"] | "auto", "auto") == 0);
      }
    }
  }

  bool autoMode = (strcmp(doc["mode"] | "auto", "auto") == 0);
  long offset = (long)tzOffsetSec;
  if (!autoMode) {
    offset = doc["offset"] | 0L;
    // 真实时区边界为 UTC-12 ~ UTC+14；粒度 15 分钟（对应网页：时 -12~+14，分 00/15/30/45）
    if (offset < -12L * 3600L || offset > 14L * 3600L) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"时区超出 -12 ~ +14\"}");
      return;
    }
    if (offset % 900L != 0) {
      httpSend(400, "application/json", "{\"success\":false,\"error\":\"分钟只能是 00/15/30/45\"}");
      return;
    }
  }
  saveTzConfig(autoMode, (int32_t)offset);
  timeSetTzOffset((int32_t)offset);   // 立即生效（本地日界由 loop 侧的每日校准检测后重建）
  if (autoMode && !oldAuto) weatherStart();   // 仅"切回自动"需要重新定位，用定位结果刷新偏移
  httpSend(200, "application/json", "{\"success\":true}");
}

void initWebServer() {
  // 注册规则：除 "/" 外一律用 onLocked/onLockedPost 包装（见文件头说明）。
  // "/" 例外：handleRoot 只读 currentTime 的 3 个字段，不碰任何配置结构；
  // 若也包进来，290KB 页面的拼接（十几毫秒）会占着锁，反而让 loop 等它。
  server.on("/", handleRoot);
  onLocked("/getconfig", handleGetConfig);
  onLocked("/getstatus", handleGetStatus);
  onLocked("/gettime", handleGetTime);
  onLocked("/getchannelstatus", handleGetChannelStatus);
  onLocked("/getntpsyncstatus", handleGetNtpSyncStatus);
  onLocked("/wifiscan", handleWifiScan);
  onLockedPost("/wifisave", handleWifiSave);
  onLockedPost("/settime", handleSetTime);
  onLocked("/syncntptime", handleSyncNTPTime);
  onLocked("/togglechannel", handleToggleChannel);
  onLockedPost("/savechannel", handleSaveChannel);
  onLockedPost("/savepins", handleSavePins);
  onLockedPost("/pwmadd", handlePwmAdd);
  onLockedPost("/pwmdel", handlePwmDel);
  onLockedPost("/pwmtaskadd", handlePwmTaskAdd);
  onLockedPost("/pwmtaskdel", handlePwmTaskDel);
  onLockedPost("/pwmweathertoggle", handlePwmWeatherToggle);
  onLockedPost("/pwmsun", handlePwmSunSet);
  onLocked("/clockget", handleClockGet);
  onLockedPost("/clockadd", handleClockAdd);
  onLockedPost("/clockremove", handleClockRemove);
  onLockedPost("/clocksetfreq", handleClockSetFreq);
  onLocked("/exportconfig", handleExportConfig);
  onLocked("/nvsdata", handleNvsData);
  onLocked("/nvsstats", handleNvsStats);
  onLockedPost("/importconfig", handleImportConfig);
  onLocked("/getlogs", handleGetLogs);
  onLocked("/iogetconfig", handleIoGetConfig);
  onLockedPost("/iogroupadd", handleIoAddGroup);
  onLockedPost("/iogroupdel", handleIoRemoveGroup);
  onLockedPost("/iogroupupdate", handleIoUpdateGroup);
  onLockedPost("/iotoggle", handleIoToggle);
  onLockedPost("/ioentryadd", handleIoAddEntry);
  onLockedPost("/ioentrydel", handleIoRemoveEntry);
  onLockedPost("/ioentryupdate", handleIoUpdateEntry);
  onLocked("/iostatus", handleIoStatus);
  onLockedPost("/iosetlevel", handleIoSetLevel);
  onLockedPost("/ioautoreset", handleIoAutoReset);
  onLockedPost("/latchset", handleLatchSet);
  onLockedPost("/latchclear", handleLatchClear);
  onLocked("/comparatorgetconfig", handleComparatorGetConfig);
  onLockedPost("/comparatorgateadd", handleComparatorGateAdd);
  onLockedPost("/comparatorgatedel", handleComparatorGateDel);
  onLockedPost("/comparatorgateupdate", handleComparatorGateUpdate);
  onLockedPost("/comparatorgatemode", handleComparatorGateMode);
  onLockedPost("/comparatortoggle", handleComparatorToggle);
  onLockedPost("/comparatorinputadd", handleComparatorInputAdd);
  onLockedPost("/comparatorinputdel", handleComparatorInputDel);
  onLockedPost("/comparatoroutputadd", handleComparatorOutputAdd);
  onLockedPost("/comparatoroutputdel", handleComparatorOutputDel);
  onLocked("/countergetconfig", handleCounterGetConfig);
  onLocked("/counterstatus", handleCounterStatus);
  onLockedPost("/counterclear", handleCounterClear);
  onLockedPost("/counterclearpin", handleCounterClearPin);
  onLockedPost("/counterdevmode", handleCounterDevMode);
  onLockedPost("/countertaskadd", handleCounterTaskAdd);
  onLockedPost("/countertaskdel", handleCounterTaskDel);
  onLockedPost("/countertaskupdate", handleCounterTaskUpdate);
  onLockedPost("/countertoggle", handleCounterToggle);
  onLockedPost("/countertriggerset", handleCounterTriggerSet);
  onLockedPost("/counteractionadd", handleCounterActionAdd);
  onLockedPost("/counteractiondel", handleCounterActionDel);
  onLockedPost("/counteractionupdate", handleCounterActionUpdate);
  onLocked("/centralstatus", handleCentralStatus);
  onLocked("/centraldetail", handleCentralDetail);
  onLockedPost("/factoryreset", handleFactoryReset);
  onLockedPost("/resetconfig", handleResetConfig);
  onLocked("/chipinfo", handleChipInfo);
  onLockedPost("/chipset", handleChipSet);
  onLockedPost("/chipclear", handleChipClear);
  onLocked("/weatherget", handleWeatherGet);
  onLockedPost("/weathersave", handleWeatherSave);
  onLocked("/relayget", handleRelayGet);
  onLockedPost("/relaysave", handleRelaySave);
  onLockedPost("/relaytest", handleRelayTest);
  onLocked("/relayteststatus", handleRelayTestStatus);
  onLocked("/weatherstatus", handleWeatherStatus);
  onLocked("/gettz", handleGetTz);
  onLockedPost("/savetz", handleSaveTz);
  onLocked("/wxfadeget", handleWxFadeGet);
  onLockedPost("/wxfadesave", handleWxFadeSave);
  onLockedPost("/pwmfadecomplete", handlePwmFadeComplete);
  server.onNotFound([]() { runLocked(handleNotFound); });
  server.begin();
  logRaw("[WEB] 服务器已启动 (端口 80)\n");
  httpTaskStart();   // HTTP 交给独立任务；创建失败时主 loop 会自动兜底
}