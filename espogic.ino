#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "esp_system.h"

// ===================== 配置区（请根据实际接线修改）=====================
// 6 路 PWM 输出，默认选用与旧版尽量接近且全程安全的 GPIO。
// 引脚可自定义（运行时可枚举、网页上点击修改，保存到 NVS）。
int pwmPins[PWM_CHANNEL_MAX] = {14, 15, 16, 17, 18, 21};
int pwmChannelCount = 0;   // 默认 0 通道（出厂为空，用户自行添加，最多 PWM_CHANNEL_MAX 个）
// =====================================================================

const char* primaryNtpServer = "ntp.aliyun.com";
const char* backupNtpServers[] = {"time1.aliyun.com", "time.qq.com", "cn.pool.ntp.org"};
const int numBackupServers = sizeof(backupNtpServers) / sizeof(backupNtpServers[0]);
const unsigned long NTP_TIMEOUT_MS = 3000;
const int NTP_MAX_RETRY = 2;

const unsigned long WIFI_CONNECT_TIMEOUT = 15000;

// ===== 全局变量定义 =====
WebServer server(80);
TimeData currentTime = {0, 0, 0};
PwmChannelConfig pwmConfigs[PWM_CHANNEL_MAX];
unsigned long previousMillis = 0;
const long interval = 1000;
float driftCompensation = 1.0f;
unsigned long lastSyncMillis = 0;
unsigned long lastSyncEpoch = 0;
WiFiUDP ntpUdp;
unsigned long lastValidEpochTime = 0;
bool ntpSyncInProgress = false;
bool isManualSync = false;
SemaphoreHandle_t netMutex = NULL;   // 网络互斥锁：串行化天气任务与 NTP，避免 LwIP 并发崩溃

// ===== 芯片型号手动固定（自动识别错误时的人工纠正，存 NVS，空 = 自动识别）=====
String forcedChipModel = "";

// ===== GPIO 手动覆盖（调度面板，按 GPIO 号索引）=====
bool manualOverride[SOC_GPIO_PIN_COUNT] = {false};
int  manualLevel[SOC_GPIO_PIN_COUNT] = {0};

// ===== 锁存（最高权限，按 GPIO 号索引）=====
bool latchEnabled[SOC_GPIO_PIN_COUNT] = {false};
int  latchLevel[SOC_GPIO_PIN_COUNT] = {0};

// ===== WiFi 配网与连接状态 =====
String wifiSsid = "";
String wifiPassword = "";
bool isAPMode = false;                 // true=热点模式, false=STA 模式
static bool wifiConnecting = false;
static unsigned long wifiAttemptStart = 0;
static bool networkServicesReady = false;
static bool wifiWasConnected = false;
static int wifiReconnectCount = 0;
static unsigned long wifiLastReconnectTry = 0;

// 热点模式默认配置（未配置 WiFi 时的兜底入口）
const char* AP_SSID = "Espogic";
const char* AP_PASSWORD = "12345678";

// AP 模式 DNS 劫持：任意域名(含 espogic.local)都解析到 192.168.4.1
DNSServer dnsServer;

int safeConstrain(int value, int minVal, int maxVal) {
  if (value < minVal) return minVal;
  if (value > maxVal) return maxVal;
  return value;
}
uint16_t safeConstrain(uint16_t value, uint16_t minVal, uint16_t maxVal) {
  if (value < minVal) return minVal;
  if (value > maxVal) return maxVal;
  return value;
}

// 启动热点模式（未配置 WiFi 或 STA 连接失败时的兜底），不阻塞
void startAPMode() {
  isAPMode = true;
  wifiConnecting = false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  logf("[WIFI] === 热点模式 ===\n");
  logf("[WIFI] SSID: %s\n", AP_SSID);
  logf("[WIFI] 密码: %s\n", AP_PASSWORD);
  logf("[WIFI] 访问地址: http://%s\n", WiFi.softAPIP().toString().c_str());
#if ENABLE_DNS_HIJACK
  dnsServer.start(53, "*", WiFi.softAPIP());
  logf("[DNS] 劫持已启动: 任意域名 -> %s\n", WiFi.softAPIP().toString().c_str());
#endif
  networkServicesReady = true;   // 触发 mDNS + NTP
  logf("[WIFI] 热点模式就绪，网络服务即将初始化\n");
}

// WiFi 连接改为非阻塞：不等待连接，结果由 loop() 中的 wifiTick() 处理。
void initWiFi() {
  if (wifiSsid.length() > 0) {
    isAPMode = false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    logf("[WIFI] 正在连接: %s\n", wifiSsid.c_str());
    wifiConnecting = true;
    wifiAttemptStart = millis();
  } else {
    logf("[WIFI] 未配置 WiFi 凭据，直接进入热点模式\n");
    startAPMode();
  }
}

// 指定公共 DNS（理由见 config.h 的 ENABLE_PUBLIC_DNS 说明）。
// 只在连接状态发生变化时应用一次：DHCP 续约与重连都可能把 DNS 覆盖回路由器，
// 所以断线时要把标志清掉，重连后重新应用 —— 这里用的是"当前是否连着"来判断，天然覆盖两种情况。
static void applyPublicDnsIfNeeded() {
#if ENABLE_PUBLIC_DNS
  static bool applied = false;
  if (WiFi.status() != WL_CONNECTED) { applied = false; return; }
  if (applied) return;
  WiFi.setDNS(DNS_PRIMARY_IP, DNS_SECONDARY_IP);
  applied = true;
  logf("[WIFI] 已指定公共 DNS: %s / %s\n",
       DNS_PRIMARY_IP.toString().c_str(), DNS_SECONDARY_IP.toString().c_str());
#endif
}

void wifiTick() {
  applyPublicDnsIfNeeded();

  // 1) STA 连接中
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting = false;
      wifiReconnectCount = 0;
      wifiWasConnected = true;
      networkServicesReady = true;
      logf("[WIFI] 连接成功！\n");
      logf("[WIFI] IP 地址: %s\n", WiFi.localIP().toString().c_str());
    } else if (millis() - wifiAttemptStart >= WIFI_CONNECT_TIMEOUT) {
      wifiConnecting = false;
      logf("[WIFI] 连接失败（超时 %lums）！切换到热点模式\n", (unsigned long)WIFI_CONNECT_TIMEOUT);
      startAPMode();
    }
    return;
  }

  // 2) STA 已连接 → 监控断线并自动重连，多次失败后转热点模式
  if (!isAPMode) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiWasConnected = false;
      unsigned long now = millis();
      if (now - wifiLastReconnectTry >= 10000) {
        wifiLastReconnectTry = now;
        wifiReconnectCount++;
        if (wifiReconnectCount > 5) {
          wifiReconnectCount = 0;
          logf("[WIFI] 重连多次失败，切换到热点模式\n");
          startAPMode();
        } else {
          logf("[WIFI] 断线，正在重连...（第 %d 次）\n", wifiReconnectCount);
          WiFi.disconnect();
          WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        }
      }
      return;
    } else {
      wifiReconnectCount = 0;
      if (!wifiWasConnected) {
        wifiWasConnected = true;
        networkServicesReady = true;
      }
    }
  }

  // 3) 一次性初始化网络服务（不阻塞主循环）
  if (networkServicesReady) {
    networkServicesReady = false;
    logf("[NET] 网络服务初始化完成\n");

#if ENABLE_MDNS
    if (MDNS.begin(MDNS_HOSTNAME)) {
      logf("[mDNS] http://%s.local\n", MDNS_HOSTNAME);
    } else {
      logf("[mDNS] 启动失败\n");
    }
#else
    logf("[mDNS] 已禁用（STA 模式请用 IP 访问）\n");
#endif
    if (isAPMode) {
      logf("[WIFI] 热点模式访问地址: http://%s\n", WiFi.softAPIP().toString().c_str());
    }
    // 启动编排交给 weatherTick：定位(求时区) → NTP(拿 UTC) → 天气。
    // 时区先到手，NTP 一落地显示的就是正确本地钟点，不会先显示 UTC 再跳变。
    // AP 模式无外网，两者都无意义，故不启动（NTP 内部也会自行跳过）。
    if (!isAPMode) weatherArmStartup(false);
  }
}

// 打印本次重启原因（用于定位是否仍偶发复位）
void printResetReason() {
  const char* reason = "未知";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   reason = "上电复位(POWERON)"; break;
    case ESP_RST_EXT:       reason = "外部复位引脚(EXT)"; break;
    case ESP_RST_SW:        reason = "软件复位(SW)"; break;
    case ESP_RST_PANIC:     reason = "异常崩溃(PANIC)"; break;
    case ESP_RST_INT_WDT:   reason = "中断看门狗(INT_WDT)"; break;
    case ESP_RST_TASK_WDT:  reason = "任务看门狗(TASK_WDT)"; break;
    case ESP_RST_WDT:       reason = "其他看门狗(WDT)"; break;
    case ESP_RST_DEEPSLEEP: reason = "深度睡眠唤醒(DEEPSLEEP)"; break;
    case ESP_RST_BROWNOUT:  reason = "电压跌落(BROWNOUT)"; break;
    case ESP_RST_SDIO:      reason = "SDIO复位(SDIO)"; break;
    case ESP_RST_USB:       reason = "USB复位(USB)"; break;
    default:                reason = "未知"; break;
  }
  logf("[BOOT] 本次重启原因: %s\n", reason);
}

void setup() {
  Serial.begin(115200);   // USB CDC On Boot 时波特率可任意；走 UART 则需 115200

  logRaw("==============================================\n");
  logRaw("[BOOT] 信号调度模块 - 启动日志 (ESP32-S3)\n");
  logRaw("==============================================\n");
  printResetReason();
  logRaw("[BOOT] setup() 开始执行\n");
  logf("[核心] 型号: %s\n", ESP.getChipModel());
  logf("[核心] 核心数: %d\n", ESP.getChipCores());
  logf("[核心] CPU 频率: %d MHz\n", ESP.getCpuFreqMHz());
  logf("[核心] Flash 容量: %d KB\n", ESP.getFlashChipSize() / 1024);
  logf("[核心] PSRAM 容量: %d KB\n", ESP.getPsramSize() / 1024);
  logf("[核心] 可用堆内存: %d 字节\n", ESP.getFreeHeap());
  if (ESP.getPsramSize() > 0) {
    logf("[核心] 可用 PSRAM: %d 字节\n", ESP.getFreePsram());
  }
  logRaw("==============================================\n");

  logRaw("[step1] 初始化 NVS 配置存储\n");
  initConfigStore();

  logRaw("[step2] 枚举可用 GPIO 引脚\n");
  enumAvailablePins();   // 运行时获取当前芯片可安全用于 PWM 的引脚

  logRaw("[step2b] 从 NVS 加载芯片型号固定配置\n");
  loadChipConfig();

  logRaw("[step2c] 从 NVS 加载时钟配置\n");
  loadClockConfig();

  logRaw("[step3] 从 NVS 加载配置\n");
  loadConfig();

  logRaw("[step3b] 从 NVS 加载序列器配置\n");
  loadIoConfig();

  logRaw("[step3c] 从 NVS 加载计数器配置\n");
  loadCounterConfig();

  logRaw("[step3d] 从 NVS 加载比较器配置\n");
  loadComparatorConfig();

  logRaw("[step3e] 从 NVS 加载时区配置\n");
  loadTzConfig();   // 必须早于 WiFi/NTP：手动时区在重启后要立即生效（AP 模式无定位可依赖）

  logRaw("[step3e-2] 从 NVS 加载天气过渡时长\n");
  loadWxFadeConfig();   // 天气系数过渡时长（updatePwmDirect 每 loop 用，须有 RAM 缓存）

  logRaw("[step3f] 应用重置后的引脚默认电平\n");
  applyResetPinDefaults();   // 须在配置加载之后（才能判断引脚是否仍被占用）、initPWM 之前

  logRaw("[step4] 初始化 PWM 输出（先于 WiFi，避免与射频校准并发）\n");
  initPWM();

  logRaw("[step5] 初始化 WiFi（先初始化协议栈）\n");
  netMutex = xSemaphoreCreateMutex();   // 网络互斥锁须在首次 weatherStart/NTP 之前就绪
  initWiFi();          // 非阻塞，仅发起连接（会初始化 WiFi 协议栈）

  logRaw("[step6] 初始化 NTP 客户端（依赖 WiFi 协议栈）\n");
  ntpSetup();          // 依赖 WiFi 协议栈已初始化，须放在 initWiFi 之后

  logRaw("[step7] 启动 Web 服务器\n");
  initWebServer();

  logRaw("==============================================\n");
  logRaw("[BOOT] 系统初始化完成，进入主循环\n");
  logRaw("==============================================\n");
}

// ===== 主循环负载统计（供「芯片信息」显示）=====
// 【先说清这不是什么】硬件 CPU 占用率拿不到：Arduino 用的 FreeRTOS 是预编译库，
// 关闭了运行时间统计（CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS），
// uxTaskGetSystemState 的 ulRunTimeCounter 恒为 0，各任务耗时占比无从取得。
//
// 【给的是什么】一个自校准的"负载率"：本工程的主循环是"裸机式"调度器，loop 每轮做的事
// 完全固定（一串非阻塞检查），所以迭代率与"能拿到多少 CPU 时间"成反比。取开机至今的
// 峰值迭代率当基线（= 本机最空闲时的能力），落差就是被高优先级任务抢走的时间占比：
//         负载率 = 100 - 当前迭代率 × 100 / 峰值迭代率
// 基线用运行期峰值而不是固定常量，是为了自动适配不同板子 / 主频 / 编译选项，无需手工标定。
//
// 【语义边界，别当真理用】
//   · 它相对的是"本机最空闲时刻"，不是硬件 CPU 利用率 —— 设备空转时恒为 0%
//   · 峰值若在极有利的瞬间采到，之后的百分比会略微偏保守（宁高勿漏）
//   · WiFi 驱动的抢占是突发性的，读数会跳动；这里已是 1 秒平均
//   · 最大间隔 >50ms 表示某一刻被整体阻塞（与 [LOOP] 卡顿日志同源，只是这里是计数）
// 1 秒结算一次，只留最近一组快照给 HTTP 侧读，运行开销就是几个自增。
static volatile uint32_t lpIterCount  = 0;   // 本窗口内迭代次数
static volatile uint32_t lpRate       = 0;   // 上次结算：次/秒
static volatile uint32_t lpRatePeak   = 0;   // 开机至今最高迭代率 = 负载率基线
static volatile uint32_t lpLoadPct    = 0;   // 上次结算：负载率(%)
static volatile uint32_t lpMaxGapMs   = 0;   // 本窗口内最大单次间隔
static volatile uint32_t lpMaxGapLast = 0;   // 上次结算的最大间隔
static volatile uint32_t lpStalls     = 0;   // 本窗口内 >50ms 的次数
static volatile uint32_t lpStallsLast = 0;   // 上次结算的卡顿次数
static unsigned long     lpWindowMs   = 0;

static void loopLoadTick(unsigned long nowMs, unsigned long gapMs) {
  lpIterCount++;
  // gapMs==0 表示首次调用（lastLoopMs 还是 0），无意义不计
  if (gapMs > 0 && gapMs < 60000UL) {
    if (gapMs > lpMaxGapMs) lpMaxGapMs = (uint32_t)gapMs;
    if (gapMs > 50) lpStalls++;
  }
  if (lpWindowMs == 0) { lpWindowMs = nowMs; return; }
  uint32_t elapsed = (uint32_t)(nowMs - lpWindowMs);
  if (elapsed < 900UL) return;              // 未满 1 秒（900 是容差）；同时兼作除零保护

  lpRate = (uint32_t)((uint64_t)lpIterCount * 1000ULL / elapsed);

  // 基线自校准：必须先更新峰值、再算负载率 —— 这样 lpRate <= lpRatePeak 恒成立，
  // 不会出现无符号下溢（把 100-负数 算成一个天文数字）。
  if (lpRate > lpRatePeak) lpRatePeak = lpRate;
  lpLoadPct = lpRatePeak ? (100UL - (lpRate * 100UL) / lpRatePeak) : 0;
  if (lpLoadPct > 100UL) lpLoadPct = 100UL;   // 防御：按上面的顺序已经到不了这里

  lpMaxGapLast = lpMaxGapMs;
  lpStallsLast = lpStalls;
  lpIterCount  = 0;
  lpMaxGapMs   = 0;
  lpStalls     = 0;
  lpWindowMs   = nowMs;
}

uint32_t loopLoadRate()    { return lpRate; }
uint32_t loopLoadPercent() { return lpLoadPct; }
uint32_t loopLoadMaxMs()   { return lpMaxGapLast; }
uint32_t loopLoadStalls()  { return lpStallsLast; }

// 主循环喂狗，彻底防崩溃；NTP/WiFi 均为非阻塞状态机
void loop() {
  // 卡顿检测：loop 周期超过 50ms 就说明有东西阻塞了主循环（HTTP 已移出，正常应远小于此）。
  // 已知会短暂超标的场景：loop 内的 NVS 防抖落盘（commit 十几~几十 ms），属预期。
  static unsigned long lastLoopMs = 0;
  unsigned long loopNowMs = millis();
  unsigned long gapMs = lastLoopMs ? (loopNowMs - lastLoopMs) : 0;
  if (gapMs > 50) {
    logf("[LOOP] 卡顿 %lums（heap=%u）\n", gapMs, ESP.getFreeHeap());
  }
  lastLoopMs = loopNowMs;
  loopLoadTick(loopNowMs, gapMs);   // 同一份间隔数据，顺带喂给负载统计

  // HTTP 已由独立任务处理（web_server.ino 的 httpTaskStart）。仅当任务创建失败时在此兜底，
  // 保证功能不丢——但那种情况下主循环仍会被大响应阻塞。
  if (!httpTaskIsRunning()) server.handleClient();

  if (isAPMode) {
#if ENABLE_DNS_HIJACK
    dnsServer.processNextRequest();   // AP 模式处理 DNS 劫持
#endif
    // 监测客户端连接变化（节流到每 1 秒一次，避免高频调用与 WiFi 驱动竞争）
    static unsigned long lastStaCheck = 0;
    static int lastStaNum = -1;
    if (millis() - lastStaCheck >= 1000UL) {
      lastStaCheck = millis();
      int staNum = WiFi.softAPgetStationNum();
      if (staNum != lastStaNum) {
        lastStaNum = staNum;
        logf("[STATION] 已连接客户端数量=%d (uptime=%lums)\n", staNum, millis());
      }
    }
  }

  wifiTick();
  ntpTick();
  updateTime();
  updatePWMOutput();
  updatePwmDirect();
  updateIoOutput();
  updateComparatorOutput();
  updateCounter();
  updateLatchHold();   // 锁存自持：每轮把锁存脚重新驱动到锁存电平（结构变更不掉电平）
  checkDailyNtpSync();
  checkPendingConfigSave();
  checkPendingIoConfigSave();
  checkPendingComparatorConfigSave();
  checkPendingCounterConfigSave();
  checkPendingClockConfigSave();
  updateCentralMonitor();
  weatherTick();

  yield();
}