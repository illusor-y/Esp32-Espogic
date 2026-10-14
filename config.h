#ifndef ESPOGIC_S3_CONFIG_H
#define ESPOGIC_S3_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <freertos/semphr.h>
#include <freertos/task.h>   // xTaskGetCurrentTaskHandle / xTaskCreatePinnedToCore（状态锁与 HTTP 任务用）
#include "pin_bus_map.h"

// ===================== 目标平台 =====================
// 本工程为 ESP32-S3（N16R8：16MB Flash + 8MB PSRAM）全新适配。
//
// Arduino IDE 板卡配置（工具 -> 开发板 -> ESP32S3 Dev Module）：
//   Flash Size          : 16MB
//   PSRAM               : OPI PSRAM  (8MB)
//   USB CDC On Boot     : Enabled    （串口日志走原生 USB CDC，波特率可任意）
//   Partition Scheme    : 16M Flash (3MB APP / 9.9MB FATFS) 或 Huge APP
//   USB Mode            : Hardware CDC and JTAG
//
// 说明：S3 拥有更大 SRAM/PSRAM、更新的 WiFi 驱动与可调的中断看门狗，
// 原 ESP32 上偶发的 INT_WDT 随机复位问题在本平台得到根本性缓解。

// ===================== PWM 引脚映射 =====================
// 注意（S3 引脚冲突，务必读写换到安全 GPIO）：
//   GPIO19/20 = 原生 USB D-/D+（使用 USB CDC 时不可占用）
//   GPIO33~37 = OPI Flash / PSRAM（不可占用）
//   GPIO43/44 = UART0（未启用 USB CDC 时用于串口）
//   GPIO0/3/45/46 = Strapping 引脚（尽量避免）
// 下方 6 路默认选用了与旧版尽量接近、且全程安全的引脚。
// PWM 通道可动态增删（最多 PWM_CHANNEL_MAX 个），删除通道即释放其引脚给 IO/比较器/计数器使用。
#define PWM_CHANNEL_MAX 8
#define PWM_TASK_MAX 6      // 每通道最多条目数（可变条目，可运行时增删）
#define CLOCK_MAX 4         // 时钟（= 硬件 timer）最多 4 个，固定对应 timer0~3
extern int pwmPins[PWM_CHANNEL_MAX];
extern int pwmChannelCount;   // 当前实际通道数（0..PWM_CHANNEL_MAX）

// ===== PWM 分辨率（运行时按时钟频率自适应计算）=====
// S3 LEDC 时钟源约 40MHz（XTAL）。为兼顾 1Hz~40kHz 的频率范围和占空比精度，
// 分辨率按 freq 动态选取，最大不超过 14 位。占空比满量程值存于各时钟 maxVal。
#define PWM_BIT_PRECISION_MAX 14
#define PWM_FREQ_DEFAULT 20000   // 新增时钟默认频率 Hz
// =====================================================================

// ===== 可用 GPIO 引脚枚举（运行时获取当前芯片存在的引脚）=====
void enumAvailablePins();          // 枚举可安全用于 PWM 的 GPIO（排除 USB/PSRAM/Strapping 等）
bool isPinAvailable(int gpio);     // 判断 GPIO 是否在可用集合内
int getAvailablePinCount();        // 可用 GPIO 数量
int getAvailablePin(int index);    // 按索引取可用 GPIO

// ===== NTP 常量 =====
extern const char* primaryNtpServer;
extern const char* backupNtpServers[];
extern const int numBackupServers;
extern const unsigned long NTP_TIMEOUT_MS;   // 单次 NTP 等待超时(ms)
extern const int NTP_MAX_RETRY;              // 每台服务器重试次数
#define NTP_PACKET_SIZE 48
// 时区偏移（单位秒，运行期可变）。NTP 返回的永远是 UTC 时间戳，
// 所有需要显示/调度用的时分秒都必须加上该偏移后再取模。
//
// 策略：一律使用"冬令时"（标准时）基准 —— 设备时钟绝不随夏令时跳变。
// 因此本值不会因夏令时变化，只由「网络定位」（自动）或「用户手动」设定。
//
// 默认 0 = UTC：定位结果到达前不假装知道时区。故开机后到定位完成之间墙上时间显示 UTC，
// 定位成功（或用户手动设定）后立即跳到本地钟点。国内定位结果恒为 +8。
#define TZ_OFFSET_DEFAULT 0L
extern int32_t tzOffsetSec;

// 本地时区换算入口。负偏移（美洲）下 (epoch + tzOffsetSec) 的无符号算术会绕回，
// 而 2^32 不是 86400 的整数倍，直接取模结果完全错误，故统一走这两个函数。
uint32_t timeLocalDayOf(unsigned long epoch);   // 该 epoch 在本地时区下的日号（日界判定用）
void     timeSetTzOffset(int32_t tz);           // 更新时区偏移（本地日界由 checkDailyNtpSync 在 loop 内重建）

// ===== NVS 数据导出（只读展示，供网页「NVS 数据」弹窗使用）=====
void buildNvsDump(String& out);   // 生成 NVS 命名空间下所有已保存参数的文本
bool readNvsStats(uint32_t& usedEntries, uint32_t& availEntries, uint32_t& totalEntries);  // NVS 容量统计

// ===== WiFi 非阻塞常量 =====
extern const unsigned long WIFI_CONNECT_TIMEOUT;

// ===== WiFi 配网状态 =====
extern String wifiSsid;
extern String wifiPassword;
extern bool isAPMode;   // true=热点模式, false=STA 模式
extern const char* AP_SSID;      // 热点模式 SSID
extern const char* AP_PASSWORD;  // 热点模式密码

// mDNS 默认开启：S3 驱动已完善，STA 模式用 http://espogic.local 访问；
// 保持与旧版统一的对外域名。
#define ENABLE_MDNS 1
// AP 模式 DNS 劫持（captive portal）：任意域名都解析到 192.168.4.1。
#define ENABLE_DNS_HIJACK 1
#define MDNS_HOSTNAME "espogic"

// ===== 结构体 =====
struct TimeData {
  int hour;
  int minute;
  int second;
};

struct ClockConfig {
  bool enabled;      // 是否启用（固定槽位，删除=禁用）
  uint32_t freq;     // 时钟频率 Hz（1~40000，全局唯一不重复）
  uint32_t maxVal;   // 运行时：2^分辨率 - 1（占空比满量程）
};

struct PwmTask {
  bool enabled;
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  int startDuty;
  int endDuty;
  int clockIndex;    // 引用的时钟槽位（0..CLOCK_MAX-1）
  bool conflict;     // 运行时冲突标记：同通道内条目时间窗口重叠（左闭右开区间）
};

// ===== 阳光交互基准（曲线两端取哪两个太阳时刻）=====
#define SUN_MODE_SUNRISE  0   // 基于日出 -> 日落
#define SUN_MODE_TWILIGHT 1   // 基于晨光 -> 昏光（天文晨昏蒙影 -18°）

struct PwmChannelConfig {
  bool enabled;
  bool weatherInteract;   // 天气交互开关：开启后占空比 = 原始 × 天气系数
  PwmTask tasks[PWM_TASK_MAX];
  int taskCount;      // 当前条目数（0..PWM_TASK_MAX，可运行时增删）
  float currentDuty;  // 原始占空比（用户配置算出，仅显示）
  float targetDuty;
  float weatherDuty;  // 天气交互后占空比（实际输出，运行时计算不持久化）
  int activeTask;     // 当前生效条目序号（1..taskCount，0=无条目生效）
  // ===== 阳光交互（基于太阳时刻生成"原始占空比"曲线）=====
  // 开启且当日对应太阳时刻有效时：targetDuty 由曲线给出（0 → 中点=峰值 → 0，两段线性），
  // 条目计算被忽略；时刻缺失（-1）/非法时完全回退条目逻辑（"妥协处理"）。
  bool    sunEnabled;    // 开关（持久化）
  uint8_t sunMode;       // SUN_MODE_*（持久化）
  int     sunPeakDuty;   // 最高原始占空比 0..100，落在曲线中点（持久化）
  // 运行时状态（不持久化）
  bool  sunActive;       // 当前是否由阳光曲线接管（有有效时刻）
  int   sunStartMin;     // 本次使用的起点（自当地 00:00 的分钟数），-1=无效
  int   sunEndMin;       // 本次使用的终点
  float sunDuty;         // 阳光曲线算出的原始占空比（供 UI 显示）
};

// ===== 序列器结构体（原「IO 调度 / 定时器」）=====
// 一个组 = 一个 GPIO 引脚；组内多条"时:分 + 动作"条目。上限各 20。
#define IO_GROUP_MAX 20
#define IO_ENTRY_MAX 20
#define IO_NAME_MAX  17   // 组名最多 16 字符（8 个汉字）+ 结尾 '\0'

// 条目模式
#define IO_MODE_SCHEDULE  0   // 定时开关：到点输出高/低并保持（决定该引脚的基础电平）
#define IO_MODE_PULSE     1   // 瞬时脉冲：到点拉高/拉低 x 毫秒后回到基础电平
#define IO_MODE_HEARTBEAT 2   // 持久心跳：无时间维度，序列启用期间永久循环；在序列内独占，不能与其他条目共存

// 时长范围（毫秒）。下限 10ms 是硬下限：更短的周期会受 loop() 周期（含 HTTP 处理）影响而抖动严重。
#define IO_DUR_MIN_MS       10UL
#define IO_DUR_MAX_MS  3600000UL        // 1 小时

// 序列器配置 JSON 文档容量（NVS 读写 / HTTP 查询共用）。
// 满配 20 组 × 20 条、含模式与三个毫秒时长字段后约 23KB，原来的 16384 会溢出
// （ArduinoJson 静默丢弃超出部分 → 保存不完整；读回则解析失败 → 整份配置丢失）。
#define IO_CONFIG_DOC_SIZE 49152

struct IoEntry {
  uint32_t pulseMs;    // 脉冲时长（毫秒），mode=IO_MODE_PULSE 有效
  uint32_t hbHighMs;   // 心跳高电平时长（毫秒），mode=IO_MODE_HEARTBEAT 有效
  uint32_t hbLowMs;    // 心跳低电平时长（毫秒），mode=IO_MODE_HEARTBEAT 有效
  uint8_t mode;        // IO_MODE_*
  uint8_t hour;
  uint8_t minute;
  uint8_t action;      // 定时开关=开启/关闭；脉冲=拉高/拉低；心跳忽略（固定先高后低）
  uint8_t conflict;    // 运行时冲突标记（同组同时分，且两条均为定时开关）
};

struct IoGroup {
  char name[IO_NAME_MAX];
  int pin;               // GPIO
  bool enabled;          // 组主开关（关闭后不参与调度，输出低电平）
  int entryCount;        // 当前条目数
  IoEntry entries[IO_ENTRY_MAX];
};

// ===== 比较器调度结构体 =====
// 一个门 = 逻辑类型（AND/OR/NOT/NAND）+ 左半输入条件 + 右半输出动作。
#define COMPARATOR_GATE_MAX   10
#define COMPARATOR_INPUT_MAX  8
#define COMPARATOR_ACTION_MAX 8

// 比较器逻辑类型
#define COMPARATOR_MODE_AND 0   // 与：全部条件达成才触发
#define COMPARATOR_MODE_OR  1   // 或：任一条件达成即触发
#define COMPARATOR_MODE_NOR 2   // 或非：无任何条件达成才触发
#define COMPARATOR_MODE_NAND 3  // 与非：非全部满足才触发

// 比较器输入来源类型
#define COMPARATOR_SRC_IO       0   // 引用序列器组
#define COMPARATOR_SRC_COUNTER  1   // 引用计数器动作
#define COMPARATOR_SRC_EXTERNAL 2   // 外部输入引脚

struct ComparatorInput {
  uint8_t source;      // 0=引用序列器组, 1=引用计数器动作, 2=外部输入引脚
  int  refIndex;       // source=0: IO组索引; source=1: 计数器任务索引; source=2: GPIO号
  int  actionIndex;    // source=1: 计数器动作索引（其他来源忽略）
  int  expectedLevel;  // 满足条件所需电平：0=低, 1=高
};

struct ComparatorAction {
  int pin;
  int sourceLevel;    // 源IO电平：条件不满足时复位至此电平（0=低, 1=高）
  int actionLevel;    // 行为电平：条件满足时输出此电平（0=低, 1=高）
};

struct ComparatorGate {
  char name[IO_NAME_MAX];
  bool enabled;          // 门主开关（关闭后不评估，输出复位源IO电平）
  uint8_t mode;          // 逻辑类型：0=AND, 1=OR, 2=NOR, 3=NAND
  int  inputCount;
  ComparatorInput inputs[COMPARATOR_INPUT_MAX];
  int  actionCount;
  ComparatorAction actions[COMPARATOR_ACTION_MAX];
};

// ===== 计数器结构体 =====
// 一个计数器任务 = 1 个触发源引脚（监控其电平切变）+ 1 个阈值 X + 最多 10 个输出动作。
// 触发源电平每反转一次（高->低 或 低->高）触发计数 +1，达到阈值 X 触发动作并清零重计。
#define COUNTER_TASK_MAX   10
#define COUNTER_ACTION_MAX 10

// 动作保持模式
#define COUNTER_HOLD_KEEP     0   // 修改：触发后输出电平并一直保持，不夺权（跟随更高优先级的锁存）
#define COUNTER_HOLD_AUTO_OFF 1   // 锁存：触发后锁存电平一段时间，优先级低于调度面板锁存，不可永久
#define COUNTER_HOLD_MAX_SEC 604800UL  // 锁存最大时长：168 小时（7 天）

struct CounterAction {
  int  pin;           // 输出引脚
  int  level;         // 0=低电平, 1=高电平
  int  holdMode;      // COUNTER_HOLD_KEEP / COUNTER_HOLD_AUTO_OFF
  uint32_t holdSec;   // 延时关闭时长（秒，holdMode=AUTO_OFF 时有效），UI 按「分:秒」输入
};

struct CounterTask {
  char name[IO_NAME_MAX];
  bool enabled;       // 任务主开关（关闭后不计数、不触发，输出复位低电平）
  int  triggerPin;    // 触发源引脚
  int  threshold;     // 反转阈值 X（>=1）
  int  actionCount;
  CounterAction actions[COUNTER_ACTION_MAX];
  // 运行时状态（不持久化）
  uint32_t triggerCount;                       // 任务私有触发计数（达到 X 清零）
  bool     fired;                              // 是否已触发（一直保持模式的锁存标记）
  unsigned long autoOffUntil[COUNTER_ACTION_MAX]; // 延时关闭到期时间（millis）
};

// ===== 全局变量（定义见 espogic.ino）=====
extern WebServer server;
extern TimeData currentTime;
extern PwmChannelConfig pwmConfigs[];
extern ClockConfig clocks[];      // 时钟数组（定义见 pwm_manager.ino，固定 4 槽位 = timer0~3）
extern unsigned long previousMillis;
extern const long interval;
extern float driftCompensation;   // 自适应时钟漂移补偿系数（由 NTP 校准更新）
extern unsigned long lastSyncMillis;
extern unsigned long lastSyncEpoch;
extern WiFiUDP ntpUdp;
extern unsigned long lastValidEpochTime;
extern bool ntpSyncInProgress;
extern bool isManualSync;

// 序列器全局
extern IoGroup ioGroups[];
extern int ioGroupCount;

// 比较器全局
extern ComparatorGate comparatorGates[];
extern int comparatorGateCount;

// 计数器全局
extern CounterTask counterTasks[];
extern int counterTaskCount;
extern uint32_t ioToggleCount[];   // 按 GPIO 号索引的全局切变累计计数（计量面板，只增）
extern uint8_t  ioLastLevel[];     // 边沿检测快照（按 GPIO 号索引，0=低/1=高）
extern bool counterCountPwm;       // 开发模式：是否统计 PWM 引脚切变
extern bool counterCountFlash;     // 开发模式：是否统计 Flash/PSRAM 引脚切变

// ===== 工具函数 =====
int safeConstrain(int value, int minVal, int maxVal);
uint16_t safeConstrain(uint16_t value, uint16_t minVal, uint16_t maxVal);

// ===== 公共 DNS：治"解析器把不可达节点也返回给你"的根因 =====
// 背景：天气/定位要连的域名（myip.ipip.net、api.open-meteo.com）都挂在多节点 CDN 上。
// CDN 会看解析器有没有带 ECS（EDNS Client Subnet）来决定返回哪个子集：
//   · 公共 DNS（阿里/腾讯/百度）带上你的地址段 → 返回本线路可达的子集（实测太平洋 13/13 可达）
//   · 路由器转发时丢掉了 ECS → 返回全局全集，掺着给别的运营商/地区的节点（实测 21 个里 8 个不可达）
// 设备默认用 DHCP 下发（= 路由器）的 DNS，于是每次解析都在抽签 —— 抽到不可达节点就是超时。
// 改为直连公共 DNS 后，解析结果就是"这条线路能用的那批"。
// 想关掉：把 ENABLE_PUBLIC_DNS 置 0 即可，其余逻辑不受影响（本项与业务代码完全解耦）。
#define ENABLE_PUBLIC_DNS 1
#define DNS_PRIMARY_IP    IPAddress(223, 5, 5, 5)     // 阿里公共 DNS
#define DNS_SECONDARY_IP  IPAddress(223, 6, 6, 6)     // 阿里公共 DNS 备用

// ===== wifi 管理 =====
void initWiFi();
void wifiTick();
void startAPMode();

// ===== time_manager =====
void updateTime();
void ntpSetup();
void ntpTick();
void ntpStartSync(bool manual);
bool ntpIsBusy();
bool ntpManualFinished();
bool ntpManualSuccess();
const char* ntpManualError();
void checkDailyNtpSync();

// ===== pwm_manager =====
void initPWM();
void pwmReleaseForOverride(int channel);                // 停止并释放某通道 PWM（手动覆盖/锁存接管）
void pwmReleaseAllForReset();                           // 重置用：停止全部 PWM 并释放引脚（过滤 Flash 总线）
void pwmRestoreChannel(int channel);                    // 恢复某通道 PWM（手动覆盖取消后）
bool setChannelPin(int channel, int newPin);   // 将某通道重绑到新 GPIO
bool pwmAddChannel(int pin, bool devMode, bool allowDup);   // 新增 PWM 通道（末尾追加）
bool pwmRemoveChannel(int index);                           // 删除 PWM 通道（释放引脚）
bool pwmAddTask(int channel);                               // 新增 PWM 条目（末尾追加，最多 PWM_TASK_MAX 条）
bool pwmRemoveTask(int channel, int index);                 // 删除 PWM 条目（前移后续条目）
void updatePWMOutput();               // 重新计算各通道目标占空比
void updatePwmDirect();               // 按目标占空比直接输出（无渐变）
float calculateDuty(PwmChannelConfig& config, int& activeTask);
bool isTaskOvernight(const PwmTask& task);   // 判断任务是否跨日
void pwmRecalcConflicts();                   // 重算每通道内任务时间重叠冲突标记

// ===== 变轨（只有"天气系数过渡"这一条，见 pwm_manager.ino）=====
// 天气档位跳变（晴→多云等）时，输出系数在 wxFadeSec 秒内匀速走过去；0 = 关闭（立即跳变）。
// 时间被修改（拨钟 / 改时区 / NTP 校时）不触发过渡：原始占空比直接按新时间到位。
bool pwmIsRamping(int channel);    // 该通道当前是否正在过渡（网页「变轨中」标签显示用）
int  pwmRampRemainSec();           // 变轨剩余秒数（0 = 无过渡）。严格正计时：只认 millis()，改时间不影响
void pwmCompleteFade();            // 立即完成：系数直接落到目标档位（网页按钮；系数全局共用，故全部通道一起到位）

// ===== 阳光交互（sun_interact.ino）=====
// 取某通道当前应使用的太阳时刻窗口（按 sunMode 选 日出→日落 / 晨光→昏光）。
// 返回 false = 未开启 / 时刻数据缺失或非法（此时通道完全回退条目逻辑）。
bool  sunWindowOfChannel(int channel, int& startMin, int& endMin);
// 三角波：0 → 峰值 → 0。secOfDay = 当天已过秒数（0..86399），peakDuty = 最高原始占空比。
float sunDutyCurve(int startMin, int endMin, int secOfDay, int peakDuty);

// ===== 时钟（Clock，硬件 timer 抽象）=====
int  clockGetFirstEnabled();                 // 第一个启用的时钟槽位，无则 -1
bool clockIsUsed(int slot);                  // 时钟是否被任意 PWM 条目引用
bool clockAdd(uint32_t freq);                // 添加时钟（找空闲槽位，频率全局唯一）
bool clockRemove(int slot);                  // 删除时钟（被引用/仅剩一个则拒绝）
bool clockSetFreq(int slot, uint32_t freq);  // 修改时钟频率（全局唯一）

// ===== 序列器（原 io_scheduler）=====
void ioRecalcConflicts();                       // 重算组内冲突（同分即冲突；心跳排斥同序列的定时开关）
int  ioComputeState(const IoGroup& grp, int hour, int minute); // 计算某组基础电平(0/1)，只认定时开关条目
int  ioLevelAt(int groupIndex, int hour, int minute, int* kind); // 某组此刻应有电平(0/1)；kind出参：0=基础 1=脉冲 2=心跳
int  ioGetTransientKind(int pin);               // 某引脚当前是否处于暂态：0=非暂态 1=脉冲 2=心跳
int  ioGetEdgeCauseKind(int pin);               // 某引脚本次电平变化的成因来源（中心面板边沿归因用）
int  ioNormalizeMode(int mode);                 // 模式白名单归一（非法值→定时开关）
bool ioCanSetMode(int groupIndex, int entryIndex, int mode); // 该序列是否允许把某条设为该模式（持久心跳独占序列）
uint32_t ioClampDurationMs(uint32_t ms);        // 时长夹紧到 [IO_DUR_MIN_MS, IO_DUR_MAX_MS]
void ioResetOutput();                           // 结构变化后强制全量重算并清空暂态运行态
void updateIoOutput();                          // loop 调用：按当前时间输出各引脚
bool ioIsPinUsedByPwm(int pin);                 // 引脚是否被 PWM 占用
bool ioIsPinUsedByIo(int pin, int excludeGroup);// 引脚是否被其他序列器组占用
bool ioAddGroup(const char* name, int pin, bool devMode, bool allowPwm, bool allowDup);     // 添加组（devMode 放开 Flash，allowPwm 放开 PWM 占用，allowDup 放开重复）
bool ioRemoveGroup(int index);                  // 删除组
bool ioUpdateGroup(int index, const char* name, int pin, bool devMode, bool allowPwm, bool allowDup); // 编辑组
bool ioAddEntry(int groupIndex, int hour, int minute, bool action, int mode,
                uint32_t pulseMs, uint32_t hbHighMs, uint32_t hbLowMs);     // 添加条目
bool ioRemoveEntry(int groupIndex, int entryIndex);                          // 删除条目
bool ioUpdateEntry(int groupIndex, int entryIndex, int hour, int minute, bool action, int mode,
                   uint32_t pulseMs, uint32_t hbHighMs, uint32_t hbLowMs);   // 编辑条目

// ===== 手动切换（调度面板）=====
// 语义：手动切换 = 更换该引脚的"基准电平"，**不是锁**。
//   脉冲/心跳叠加在这个基准之上、结束之后回到它（所以把低切成高后，脉冲拉低若干毫秒会回到高）；
//   定时开关到点、比较器/计数器输出也都能覆盖它（定时开关到点时手动基准交还调度）。
//   只有"锁存"才是绝对不可改的。手动状态只存 RAM，重启即失效，且不参与配置持久化。
extern bool manualOverride[];   // 按 GPIO 号索引：是否处于手动基准
extern int  manualLevel[];      // 手动基准电平 0=低/关, 1=高/开
bool manualIsOverridden(int pin);           // 引脚是否处于手动基准
int  gpioGetCurrentLevel(int pin);          // 引脚当前电平（锁存 > 脉冲/心跳 > 手动基准/调度基础 > 实际读值）
bool manualSetLevel(int pin, int level, bool devMode, bool allowPwm);    // 设定手动基准（devMode 放开 PSRAM，allowPwm 放开 PWM）
bool manualClearPin(int pin);               // 取消某引脚的手动基准
void manualClearAll();                      // 取消全部手动基准

// ===== 锁存（绝对锁：覆盖手动切换 / PWM / 序列器 / 比较器 / 计数器）=====
// 锁存后电平被固定，仅可在调度面板锁存弹窗中解除；其余模块对被锁存引脚的
// 任何电平写入都会被跳过，并在 Web 前端将该条目标黄、提示「无权限」。
extern bool latchEnabled[];   // 按 GPIO 号索引：是否锁存
extern int  latchLevel[];     // 锁存电平 0=低/关, 1=高/开
bool latchIsEnabled(int pin);                                  // 引脚是否被锁存
bool latchSet(int pin, int level, bool devMode, bool allowPwm);  // 锁存（devMode 放开 Flash，allowPwm 放开 PWM）
bool latchClear(int pin);                                      // 解除某引脚锁存
void latchClearAll();                                          // 解除全部锁存
void updateLatchHold();                                       // loop 调用：锁存脚自持（每轮幂等驱动，保证配置变更不打断固定电平）

// ===== 芯片型号（自动识别 + 手动固定）=====
// 手动固定芯片型号用于自动识别错误时的人工纠正。固定值存独立 NVS key，
// 「清除配置」与「重置系统」（清空整个命名空间）均会清除。
extern String forcedChipModel;                // 手动固定的芯片型号（空 = 自动识别）
extern const char* const CHIP_MODELS[];       // 受支持的芯片型号列表
extern const int CHIP_MODEL_COUNT;
const char* getEffectiveChipModel();          // 生效型号：固定值优先，否则自动识别
bool isValidChipModel(const char* model);     // 校验型号是否受支持
void loadChipConfig();                        // 启动时从 NVS 读取固定值
bool setForcedChipModel(const char* model);   // 固定型号并写入 NVS
bool clearForcedChipModel();                  // 清除固定，恢复自动识别

// ===== config_store =====
void initConfigStore();
bool saveConfig();            // 返回是否成功写入 NVS
void loadConfig();
void requestConfigSave();      // 防抖：标记需要保存（由 loop 合并写入）
void checkPendingConfigSave(); // 由 loop 调用，执行防抖后的实际写入
String saveFailSummary();      // 落盘失败的模块名汇总（空串=全部正常），供 /getstatus 显示常驻提示
void applyResetPinDefaults();  // 重置后引脚收尾：把重置前占用的引脚主动驱动为低（setup 中、initPWM 前调用）

// 全局配置导入：校验并应用 PWM/IO/比较器/计数器全部模块后写回 NVS。
// WiFi 凭据不随导出文件传输，导入时保持不变。成功返回 true；失败 err 为可读原因。
bool importGlobalConfig(const char* json, String& err);

// IO 配置独立读写与防抖保存
bool saveIoConfig();
void loadIoConfig();
void requestIoConfigSave();
void checkPendingIoConfigSave();

// ===== digital_comparator =====
void updateComparatorOutput();                                        // loop 调用：评估并输出比较器
bool comparatorIsPinUsedByOutput(int pin, int excludeGate);           // 引脚是否被比较器输出占用
bool comparatorIsPinUsedByExternalInput(int pin, int excludeGate);    // 引脚是否被比较器外部输入占用
bool pinIsReadOnlyInput(int pin);                                     // 只读输入引脚：比较器外部输入 + 未被输出占用的计数器触发源（禁止手动切换/锁存）
void comparatorOnIoGroupRemoved(int removedIndex);                    // IO 组删除后修正内部引用
void comparatorOnIoGroupInserted();                                   // IO 组插入到最前时修正内部引用
void comparatorOnCounterTaskInserted();                               // 计数器任务插入到最前时修正引用
void comparatorOnCounterTaskRemoved(int removedIndex);                // 计数器任务删除后修正引用
void comparatorOnCounterActionRemoved(int taskIndex, int actionIndex);// 计数器动作删除后修正引用
bool comparatorAddGate(const char* name, int mode);
bool comparatorRemoveGate(int index);
bool comparatorUpdateGate(int index, const char* name);
bool comparatorSetGateMode(int index, int mode);
bool comparatorGateMet(const ComparatorGate& gate);   // 门当前是否触发（按 mode 评估）
bool comparatorAddInput(int gateIndex, int source, int refIndex, int actionIndex, int expectedLevel, bool devMode, bool allowPwm, bool allowDup);
bool comparatorRemoveInput(int gateIndex, int inputIndex);
bool comparatorUpdateInput(int gateIndex, int inputIndex, int source, int refIndex, int actionIndex, int expectedLevel, bool devMode, bool allowPwm, bool allowDup);
bool comparatorAddAction(int gateIndex, int pin, int sourceLevel, int actionLevel, bool devMode, bool allowPwm, bool allowDup);
bool comparatorRemoveAction(int gateIndex, int actionIndex);
bool comparatorUpdateAction(int gateIndex, int actionIndex, int pin, int sourceLevel, int actionLevel, bool devMode, bool allowPwm, bool allowDup);

// 比较器配置独立读写与防抖保存
bool saveComparatorConfig();
void loadComparatorConfig();
void requestComparatorConfigSave();
void checkPendingComparatorConfigSave();

// ===== counter =====
void updateCounter();                                    // loop 调用：边沿检测 + 触发判定 + 动作输出
bool counterIsPinUsedByAction(int pin, int excludeTask); // 引脚是否被计数器输出动作占用
bool counterTaskHasActionPin(int taskIndex, int pin, int excludeAction); // 引脚是否已是该任务的动作脚（excludeAction 跳过正在编辑的那条）
bool counterIsPinUsedAsReadOnlyTrigger(int pin);         // 引脚是否为"只读输入"型触发源（是触发源且无输出模块占用）
int  counterGetActionLevel(int taskIndex, int actionIndex); // 计数器动作应有输出电平（供比较器引用）
bool counterAddTask(const char* name);
bool counterRemoveTask(int index);
bool counterUpdateTask(int index, const char* name);
bool counterSetTrigger(int index, int pin, int threshold, bool devMode, bool allowPwm);
bool counterAddAction(int taskIndex, int pin, int level, int holdMode, uint32_t holdSec, bool devMode, bool allowPwm, bool allowDup);
bool counterRemoveAction(int taskIndex, int actionIndex);
bool counterUpdateAction(int taskIndex, int actionIndex, int pin, int level, int holdMode, uint32_t holdSec, bool devMode, bool allowPwm, bool allowDup);
void counterClearAllCounts();                            // 计量面板清零（全局切变计数 + 任务触发计数/锁存）
void counterClearPinCount(int pin);                      // 清除单个引脚的切变计数

// 计数器配置独立读写与防抖保存
bool saveCounterConfig();
void loadCounterConfig();
void requestCounterConfigSave();
void checkPendingCounterConfigSave();

// 时钟配置独立读写与防抖保存
bool saveClockConfig();
void loadClockConfig();
void requestClockConfigSave();
void checkPendingClockConfigSave();

// ===== 主循环负载（espogic.ino 每秒结算；供「芯片信息」显示）=====
// 它不是"CPU 占用率"：Arduino 下的 FreeRTOS 预编译库关闭了运行时间统计，拿不到任务耗时占比。
// 本工程所有控制逻辑都在 loop 里顺序跑，所以迭代率与卡顿间隔是等价的负载证据。
uint32_t loopLoadRate();     // 约每秒迭代次数（骤降 = CPU 被 WiFi/HTTP/天气抓取占走）
uint32_t loopLoadPercent();  // 负载率(%)：相对"本机最空闲时"被高优先级任务抢走的时间占比
uint32_t loopLoadMaxMs();    // 最近 1 秒窗口内最大单次间隔（ms）
uint32_t loopLoadStalls();   // 最近 1 秒窗口内 >50ms 的卡顿次数

// 恢复出厂 / 重置配置
bool factoryReset();            // 清除所有用户配置（含 WiFi）
bool resetConfigKeepWifi();     // 清除除 WiFi 外的所有配置（PWM/IO/比较器恢复默认）

// 天气定位配置（模式 + 手动位置），独立 NVS key，中文名按 UTF-8 存储
void saveWeatherConfig(bool manual,
                       const char* provCode, const char* cityCode, const char* distCode,
                       const char* prov, const char* city, const char* dist);
void loadWeatherConfigRaw(String& out);   // 返回 JSON 字符串（可能为空，表示未配置=auto）

// 接口转发地址校验：空串 = 关闭该级（合法）；否则必须 ≤192 字符且带 http(s):// 前缀。
// 网页保存、测试联通、导入备份文件三条路径共用这一份规则 —— 任何一条漏检都会造成
// "能存进去却用不通"，或反向的"测试通过、保存被拒"。
// 返回空串 = 通过；否则返回可直接展示给用户的错误文案。
String relayUrlCheck(const String& url);

// 接口转发地址（独立 NVS key，天气抓取的第三级兜底）。空 = 关闭该级，行为与未配置完全相同。
// 由用户在网页「天气 → 接口转发」自填（Cloudflare Worker 等），因此对外分发时不依赖任何人的私有服务。
void saveRelayConfig(const char* url);
void loadRelayConfigRaw(String& out);     // 返回 JSON 字符串（可能为空，表示未配置）

// 接口转发「测试联通」：非阻塞触发 + 轮询结果（与手动 NTP 同步 ntpsynctime/手动同步同一套模式）。
// 真正的网络请求在天气任务里、持 netMutex 执行 —— 绝不能在 HTTP handler 里同步做，
// 那会持着 stateMutex 阻塞十几秒，把主循环（PWM/序列器/计数器）一起冻住。
bool weatherRelayTestStart(const String& url);   // false = 上一次测试还在跑
int  weatherRelayTestState();      // 0=未测 1=进行中 2=成功 3=失败
int  weatherRelayTestCode();       // 失败时的 HTTP 码；连接阶段就失败为 0
unsigned long weatherRelayTestMs();
float weatherRelayTestTemp();      // 成功时返回的温度；失败为 -999
const char* weatherRelayTestMsg();

// 时区配置（模式 + 手动偏移），独立 NVS key。空/缺省 = auto（偏移由网络定位填充）。
void saveTzConfig(bool autoMode, int32_t offset);
void loadTzConfigRaw(String& out);
void loadTzConfig();   // 开机加载：把保存的时区写回运行时（AP 模式下唯一来源）

// ===== 网络互斥锁 =====
// ESP32 Arduino 的网络库（WiFiUDP / HTTPClient / WiFiClientSecure）不支持多任务/多核并发访问，
// 否则会触发 LwIP 断言 "Required to lock TCPIP core functionality!" 崩溃。
// netMutex 串行化「天气后台抓取任务」与「主 loop 的 NTP UDP」，保证同一时刻只有一处访问网络协议栈。
extern SemaphoreHandle_t netMutex;

// 阻塞式网络锁（天气抓取任务用）：构造即持锁，离开作用域自动释放
class NetLockGuard {
public:
  NetLockGuard()  { if (netMutex) xSemaphoreTake(netMutex, portMAX_DELAY); }
  ~NetLockGuard() { if (netMutex) xSemaphoreGive(netMutex); }
};

// 非阻塞网络锁（主 loop 的 NTP tick 用）：抢不到锁立即放弃本次 tick，避免阻塞主循环
class NetLockTryGuard {
public:
  bool locked;
  NetLockTryGuard() : locked(false) {
    if (netMutex) locked = (xSemaphoreTake(netMutex, 0) == pdTRUE);
  }
  ~NetLockTryGuard() { if (locked && netMutex) xSemaphoreGive(netMutex); }
};

// ===== 跨任务状态锁（HTTP 独立任务 与 主 loop 互斥）=====
// HTTP 服务已移到独立任务（web_server.ino 的 httpTaskLoop）：主 loop 只跑控制逻辑，
// 浏览器刷新/卡死/中途 abort 都只影响那个任务，不会再冻住序列器、计数器、比较器。
// 但两个任务会并发访问同一批运行时状态（pwmConfigs / ioGroups / comparatorGates /
// counterTasks / clocks / manualOverride / latch / centralEvents / ioLevelInit 等），
// 没有这把锁就会出现"loop 正按旧下标读数组、HTTP 正在删除并前移该数组"这类偶发错误输出
// （例：ioRemoveGroup 整组前移时被读到 → 错误的引脚被驱动）。
//
// 只有两条规则：
//   1) 锁内只碰状态，绝不发送响应。发送是秒级长阻塞（浏览器 abort 时最长 5s），必须锁外做。
//      所有发送都走 web_server.ino 的 httpSend()，它在发送前自动解锁，因此"持锁发送"
//      在结构上不可能发生。
//   2) 只短暂持锁，不要整轮 loop 持锁，否则网页会被饿住。
//
// 锁顺序（防死锁）：stateMutex -> weatherMutex / logMutex / netMutex，反向永不成立。
extern SemaphoreHandle_t stateMutex;

void stateLockInit();          // setup 早期调用：创建锁
void stateLock();              // 持锁（递归锁，允许嵌套：handler -> 模块变更函数）
void stateUnlock();            // 解锁（与 stateLock 配对，同一任务内成对）
void stateUnlockIfHeld();      // 兜底解锁：handler 走完却没发送时由 runLocked 调用，防永久死锁
void stateLockArm(bool armed); // 打开断言：HTTP 任务创建后才置 true（此前 setup 单线程无需校验）
void stateAssertHeld();        // 断言：当前任务持有状态锁（模块变更函数入口用）
void stateAssertNotHeld();     // 断言：当前任务未持锁（发送前用）
bool stateIsHeldByCurrentTask();

// 作用域锁：构造即持锁，离开作用域自动释放。loop 侧 update* 用它，天然覆盖提前 return。
class StateLockGuard {
public:
  StateLockGuard()  { stateLock(); }
  ~StateLockGuard() { stateUnlock(); }
};

// ===== solar（太阳位置：日出/日落/天文晨昏光，纯本地计算，不联网）=====
// 见 solar.ino。作为"天文量"与天气解耦：只依赖 纬度/经度/日期/时区偏移 四个入参。
struct SolarTimes {
  int sunriseMin;   // 自当地 00:00 起的分钟数（0..1439）；-1 = 当日无此事件（极昼/极夜）
  int sunsetMin;
  int dawnMin;      // 天文晨光始（太阳高度角 -18°：天空刚开始泛光的"第一缕光"）
  int duskMin;      // 天文昏光终（太阳高度角 -18°：天空完全无光的"最后一缕光"）
};

// lat/lon 均为角度（北纬/东经为正）；tzOffsetSec 为该地时区偏移（秒）。
// 返回 false 表示入参非法（纬度接近极点、经度或日期越界）；事件缺失以 -1 表示、不影响返回 true。
bool solarComputeTimes(double lat, double lon, int year, int month, int day,
                       int32_t tzOffsetSec, SolarTimes& out);

// ===== weather（后端天气抓取：启动最后一步自动执行，全程串口日志）=====
void weatherStart();                        // 开始一次天气抓取（非阻塞：惰性建后台任务 + 发通知；网络就绪 / 手动保存后调用）
// 启动/手动同步统一入口：按「定位(求时区) → NTP(拿 UTC) → 天气」编排。
// ntpManual=true 表示用户点的是「同步网络时间」（NTP 结果按手动上报，前端弹提示）。
void weatherArmStartup(bool ntpManual);
void weatherTick();                         // loop 调用：整点定时触发 + 启动编排最后一棒（抓取体在后台任务）
void weatherBuildStatus(JsonDocument& doc); // 组装天气状态 JSON（位置 + 天气缓存，/weatherstatus 用）
void weatherGetSunTimes(int& sunriseMin, int& sunsetMin, int& dawnMin, int& duskMin); // 当日太阳时刻快照（-1=无数据）
int  weatherGetDutyFactor();                // 当前天气对应的占空比系数（百分比）；天气未抓到返回 100（不缩放）
bool weatherHasData();                      // 是否已获得过真实天气（false 时上面的 100 只是占位值，不是"晴"）

// ===== 天气系数过渡（天气现象切换时占空比匀速过渡）=====
// 只作用于"天气系数"这一层（输出侧）：档位切换时系数在 N 秒内线性走到新值，
// 避免"晴 100% → 多云 80%"那一下瞬间跳 20 个点。
// 阳光曲线与条目插值本身是连续量（速率比过渡慢 2~3 个数量级），不受本机制影响；
// 手动切换 / 锁存 / 通道启停等有意跳变同样不受影响。
// 默认 25 分钟；上限 50 分钟（< 天气刷新间隔 60 分钟，仍留 10 分钟稳态期）。
// 网页按分钟输入，秒/分换算只在前端做（见 wxFadeLoad / wxFadeSave）。
#define WX_FADE_DEFAULT_SEC 1500   // 默认 25 分钟
#define WX_FADE_MAX_SEC     3000   // 上限 50 分钟
int  wxFadeSec();                  // 当前过渡时长（秒）；0 = 关闭过渡（立即切换）
void loadWxFadeConfig();           // 开机加载（结果缓存在 RAM，供每 loop 的输出使用）
bool saveWxFadeConfig(int sec);    // 保存（同时更新 RAM 缓存）

// ===== central（中心面板：只读监控 + 电平反转归因）=====
void updateCentralMonitor();                        // loop 调用：重建引脚集合并采样记录边沿事件
void centralBuildStatus(JsonDocument& doc);         // 组装中心面板整体状态 JSON（pins/events/pwmRunning）
void centralBuildDetail(JsonDocument& doc, int pin);// 组装某引脚触发归因 JSON（模块-组-条目）

// ===== web_server =====
void initWebServer();
String getHTML();
bool httpTaskIsRunning();  // HTTP 独立任务是否已接管 handleClient（创建失败时主 loop 兜底）
void handleWeatherGet();   // 读取天气定位配置（模式 + 手动位置）
void handleWeatherSave();  // 保存天气定位配置（模式 + 手动位置）
void handleWeatherStatus();  // 返回后端天气缓存状态（位置 + 天气）

// ===== logger（环形缓冲日志：串口 + 网页回看）=====
void logf(const char* fmt, ...);                 // 统一日志出口：写串口 + 写环形缓冲区
void logRaw(const char* text);                   // 写入无格式化纯文本日志
uint32_t logGetSeq();                            // 当前最新日志序号（含空洞的全局递增序号）
uint32_t logGetCount();                          // 缓冲区内实际已存日志条数
void logGetSince(uint32_t fromSeq, String& out); // 拉取 seq > fromSeq 的日志（换行分隔，含各自 seq 前缀由调用方处理）

#endif
