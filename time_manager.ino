#include "config.h"
#include <WiFi.h>

// ===================== NTP 非阻塞状态机 =====================
// 不依赖 NTPClient 库（其 update() 内部用 delay 循环等待 UDP 响应，会阻塞主循环）。
// 这里自实现 UDP NTP 客户端，每次 loop() 只推进一步：发送 → 非阻塞等待 → 超时切换。

enum NtpState { NTP_IDLE, NTP_SENDING, NTP_WAITING, NTP_DONE_OK, NTP_DONE_FAIL };
static NtpState ntpState = NTP_IDLE;

static int ntpServerIdx = 0;        // 当前服务器在列表中的索引
static int ntpRetryCount = 0;       // 当前服务器已重试次数
static unsigned long ntpSendTime = 0;

static bool ntpManual = false;      // 本次同步是否为用户手动触发
static bool manualSyncFinished = false;
static bool manualSyncSuccess = false;
static const char* manualSyncError = "";

static uint8_t ntpBuffer[NTP_PACKET_SIZE];

// 运行时组装：主服务器 + 备用服务器
#define MAX_NTP_SERVERS 8
static const char* ntpServerList[MAX_NTP_SERVERS];

static unsigned long currentEpoch = 0;      // 当前推算的 Unix 秒（updateTime 维护）
static unsigned long lastAutoSyncDay = 0;   // 上次自动同步所在的绝对天数
static unsigned long lastAutoTryDay = 0;    // 当天是否已尝试过自动同步

// 运行期时区偏移（秒）。默认 0 = UTC（见 config.h）：定位结果到达前不假装知道时区。
// 一律为"冬令时"基准，不随夏令时变化（见 config.h 说明）。
int32_t tzOffsetSec = TZ_OFFSET_DEFAULT;

// ===== 本地时区换算 =====
// 负偏移下不能写 (epoch + tzOffsetSec) % 86400：tzOffsetSec 为负时无符号算术会绕过 0，
// 而 2^32 不是 86400 的整数倍，结果会完全错。统一先取"当天已过秒数"再叠加有符号偏移。

// 该 epoch 在本地时区下的"当天秒数"（0~86399）
static unsigned long tzSecOfDay(unsigned long epoch) {
  long v = (long)(epoch % 86400UL) + (long)tzOffsetSec;
  v %= 86400L;
  if (v < 0) v += 86400L;
  return (unsigned long)v;
}

// 该 epoch 在本地时区下的日号（日界判定：每日 NTP 校准、序列器脉冲去重）
uint32_t timeLocalDayOf(unsigned long epoch) {
  long v = (long)(epoch % 86400UL) + (long)tzOffsetSec;
  long carry = 0;
  if (v < 0)            carry = -1;
  else if (v >= 86400L) carry =  1;
  return (uint32_t)((long)(epoch / 86400UL) + carry);
}

// 更新时区偏移。只改 tzOffsetSec 本身，不动"每日校准"的两个日槽 ——
// 本函数会被天气任务（定位刷新）和 HTTP 任务（改时区/导入）调用，若在这里写日槽，
// 那两个变量就成了跨任务共享（loop 侧不加锁读、别的任务持锁写）。
// 日界重建改由 checkDailyNtpSync() 在 loop 任务内检测到偏移变化后自行完成。
void timeSetTzOffset(int32_t tz) {
  if (tz == tzOffsetSec) return;
  StateLockGuard _lg;   // tzOffsetSec 会被 loop 与 HTTP 任务读
  tzOffsetSec = tz;
  // 这里不通知 PWM 做任何过渡：原始占空比是时间的连续函数，时区一改，它就该变成新时刻对应的值，
  // 直接到位即可（亮度跳一下是用户主动操作的结果，比"慢慢爬 25 分钟"更符合直觉）。
  int32_t a = (tz < 0) ? -tz : tz;
  logf("[时区] 偏移更新为 UTC%s%d:%02d\n",
       tz < 0 ? "-" : "+", (int)(a / 3600), (int)((a % 3600) / 60));
}

void ntpSetup() {
  ntpServerList[0] = primaryNtpServer;
  for (int i = 0; i < numBackupServers; i++) {
    ntpServerList[i + 1] = backupNtpServers[i];
  }
  ntpUdp.begin(123);
  String list;
  list.reserve(128);
  list += primaryNtpServer;
  for (int i = 0; i < numBackupServers; i++) {
    list += ", ";
    list += backupNtpServers[i];
  }
  logf("[NTP] 服务器列表: %s\n", list.c_str());
  logRaw("[NTP] UDP 端口 123 已就绪\n");
}

bool ntpIsBusy() {
  return ntpState == NTP_SENDING || ntpState == NTP_WAITING;
}
bool ntpManualFinished() { return manualSyncFinished; }
bool ntpManualSuccess() { return manualSyncSuccess; }
const char* ntpManualError() { return manualSyncError; }

// 发送 NTP 请求（UDP 123 端口）
static bool ntpSendRequest(const char* server) {
  memset(ntpBuffer, 0, NTP_PACKET_SIZE);
  ntpBuffer[0] = 0b11100011;   // LI=0, VN=3, Mode=3(客户端)
  ntpBuffer[1] = 0;            // stratum
  ntpBuffer[2] = 6;            // polling interval
  ntpBuffer[3] = 0xEC;         // precision
  ntpUdp.beginPacket(server, 123);
  ntpUdp.write(ntpBuffer, NTP_PACKET_SIZE);
  return ntpUdp.endPacket();
}

// 解析 NTP 响应中的秒数（1900 基准 → 1970 基准）
static unsigned long ntpParseEpoch() {
  unsigned long secs =
      ((unsigned long)ntpBuffer[40] << 24) |
      ((unsigned long)ntpBuffer[41] << 16) |
      ((unsigned long)ntpBuffer[42] << 8) |
      ((unsigned long)ntpBuffer[43]);
  return secs - 2208988800UL;
}

static void ntpApplyEpoch(unsigned long epoch) {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：currentTime / 同步状态会被 /gettime、/getstatus 读
  unsigned long localMs = millis() % 1000;
  if (localMs > 200) epoch += 1;
  unsigned long secOfDay = tzSecOfDay(epoch);   // 换算到本地时区
  currentTime.hour = secOfDay / 3600;
  currentTime.minute = (secOfDay % 3600) / 60;
  currentTime.second = secOfDay % 60;
  lastValidEpochTime = epoch;
  lastAutoSyncDay = timeLocalDayOf(epoch);
  lastSyncMillis = millis();
  lastSyncEpoch = epoch;
  // 时间平移不会触发任何 PWM 过渡：原始占空比直接按新时间到位。
  // （首次同步是"从开机自增时钟切到真实时间"，若做过渡会变成"开机后慢慢爬 25 分钟"；
  //   每日校准有漂移补偿，偏差通常只有几秒，本来也无需缓和。）
  // 只有"天气档位跳变"才走过渡（见 pwm_manager.ino 的 wxFadeS）。
}

// 当前服务器失败：优先重试，否则切换下一台；全部失败则结束
static void ntpAdvanceServer() {
  if (ntpRetryCount < NTP_MAX_RETRY) {
    ntpRetryCount++;
    ntpState = NTP_SENDING;
    return;
  }
  ntpRetryCount = 0;
  ntpServerIdx++;
  if (ntpServerIdx < numBackupServers + 1) {
    ntpState = NTP_SENDING;
    return;
  }
  ntpState = NTP_DONE_FAIL;
  ntpSyncInProgress = false;
  isManualSync = false;
  if (ntpManual) {
    manualSyncFinished = true;
    manualSyncSuccess = false;
    manualSyncError = "NTP服务器连接失败";
  }
  logRaw("[NTP] 同步失败，使用原有时间\n");
}

// 发起一次同步（非阻塞，立即返回）。manual=true 表示用户手动触发
void ntpStartSync(bool manual) {
  if (isAPMode) {   // 热点模式无外网，跳过 NTP
    if (manual) {
      manualSyncFinished = true;
      manualSyncSuccess = false;
      manualSyncError = "当前为热点模式，无法连接NTP";
    }
    ntpSyncInProgress = false;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (manual) {
      manualSyncFinished = true;
      manualSyncSuccess = false;
      manualSyncError = "WiFi未连接";
    }
    logRaw("[NTP] WiFi未连接，跳过同步\n");
    ntpSyncInProgress = false;
    return;
  }
  ntpManual = manual;
  isManualSync = manual;
  manualSyncFinished = false;
  ntpServerIdx = 0;
  ntpRetryCount = 0;
  ntpSyncInProgress = true;
  ntpState = NTP_SENDING;
  if (manual) logRaw("[NTP] 开始同步网络时间...\n");
}

// 每次 loop() 调用：非阻塞推进同步状态机
void ntpTick() {
  if (ntpState == NTP_IDLE || ntpState == NTP_DONE_OK || ntpState == NTP_DONE_FAIL) {
    return;
  }

  // 非阻塞抢网络锁：天气后台任务正在抓取时，本 tick 直接跳过 UDP 操作，
  // 避免与天气任务的网络访问并发（LwIP 断言崩溃）。NTP 是状态机，跳几次 tick 无影响。
  NetLockTryGuard netlock;
  if (!netlock.locked) return;


  if (ntpState == NTP_SENDING) {
    const char* server = ntpServerList[ntpServerIdx];
    if (ntpSendRequest(server)) {
      ntpSendTime = millis();
      ntpState = NTP_WAITING;
      logf("[NTP] 尝试服务器: %s\n", server);
    } else {
      ntpAdvanceServer();   // 发送失败，直接切下一台
    }
    return;
  }

  // NTP_WAITING：非阻塞检查 UDP 响应
  int packetSize = ntpUdp.parsePacket();
  if (packetSize >= NTP_PACKET_SIZE) {
    ntpUdp.read(ntpBuffer, NTP_PACKET_SIZE);
    bool validReply =
        (ntpBuffer[0] & 0x07) == 4 &&
        ntpBuffer[1] >= 1 && ntpBuffer[1] < 16 &&
        (ntpBuffer[40] | ntpBuffer[41] | ntpBuffer[42] | ntpBuffer[43]) != 0;
    if (validReply) {
      unsigned long epoch = ntpParseEpoch();

      // 自适应时钟漂移补偿
      if (lastSyncEpoch != 0 && lastSyncMillis != 0) {
        unsigned long localMs = millis() - lastSyncMillis;
        if (localMs >= 10000UL && localMs < 86400000UL) {
          unsigned long long realMs = (unsigned long long)(epoch - lastSyncEpoch) * 1000ULL;
          float measured = (float)realMs / (float)localMs;
          if (measured > 0.90f && measured < 1.10f) {
            float weight = (float)localMs / 3600000.0f * 0.2f;
            if (weight < 0.001f) weight = 0.001f;
            if (weight > 0.2f) weight = 0.2f;
            driftCompensation = driftCompensation * (1.0f - weight) + measured * weight;
            logf("[NTP] 时钟漂移补偿更新: %.6f (间隔%lus, 权重%.2f%%)\n",
                          driftCompensation, localMs / 1000UL, weight * 100.0f);
          }
        }
      }

      ntpApplyEpoch(epoch);
      ntpState = NTP_DONE_OK;
      ntpSyncInProgress = false;
      isManualSync = false;
      if (ntpManual) {
        manualSyncFinished = true;
        manualSyncSuccess = true;
      }
      logf("[NTP] 同步成功：%d:%s%d:%s%d\n",
           currentTime.hour,
           currentTime.minute < 10 ? "0" : "", currentTime.minute,
           currentTime.second < 10 ? "0" : "", currentTime.second);
      return;
    }
  }

  if (millis() - ntpSendTime >= NTP_TIMEOUT_MS) {
    logRaw("[NTP] 响应超时，切换服务器\n");
    ntpUdp.flush();
    ntpAdvanceServer();
  }
}

// ===================== 本地时间推算 =====================
void updateTime() {
  StateLockGuard _lg;   // 与 HTTP 任务互斥：currentTime 会被 /settime、NTP 同步写，handler 会读
  if (lastSyncMillis == 0 || lastSyncEpoch == 0) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      currentTime.second++;
      if (currentTime.second >= 60) {
        currentTime.second = 0;
        currentTime.minute++;
        if (currentTime.minute >= 60) {
          currentTime.minute = 0;
          currentTime.hour++;
          if (currentTime.hour >= 24) {
            currentTime.hour = 0;
          }
        }
      }
    }
    return;
  }
  unsigned long nowMs = millis();
  unsigned long elapsed = nowMs - lastSyncMillis;
  // 长时间未同步时滚动前移锚点，让时间用本地晶振持续推算，绝不冻结。
  // 原实现把 elapsed 夹紧到 24h，会使 currentEpoch 钉死在 lastSyncEpoch+86400，时间卡住；
  // 且若卡在 00:00:0x，checkDailyNtpSync 的 "second<10" 判断恒真，每日 NTP 校准被永久跳过，
  // 形成"时间冻结 → 永不校准"的死锁（挂机实测：时间卡在 00:00:07 数小时）。
  while (elapsed > 86400000UL) {
    lastSyncEpoch  += 86400UL;
    lastSyncMillis += 86400000UL;
    elapsed        -= 86400000UL;
  }
  currentEpoch = lastSyncEpoch + (unsigned long)((double)elapsed * (double)driftCompensation / 1000.0);
  unsigned long secOfDay = tzSecOfDay(currentEpoch);   // 换算到本地时区
  currentTime.hour = secOfDay / 3600;
  currentTime.minute = (secOfDay % 3600) / 60;
  currentTime.second = secOfDay % 60;
}

// 每日自动校准（非阻塞触发）：每天本地 00:00:10 之后、且当天尚未同步/尝试过时触发一次。
//
// 本地日界的重建刻意放在这里、而不是 timeSetTzOffset()：后者会被天气任务与 HTTP 任务调用，
// 若由它写 lastAutoSyncDay/lastAutoTryDay，这两个日槽就成了跨任务共享变量（本函数在 loop 里
// 不加锁读、别的任务持锁写）。改成"本函数检测到偏移变化后自行重建"之后，
// 两个日槽的全部读写都只发生在 loop 任务内，竞态从根上消失，也不必给热路径加锁。
void checkDailyNtpSync() {
  // 时区变了（天气定位刷新 / 用户改时区 / 导入配置）：本地日界整体平移，重建日槽。
  // 不能清成 0 —— 0 是"从未同步过"的哨兵，会让本函数永久跳过（改一次时区就废掉每日校准）。
  // 用新偏移重算"此刻属于哪个本地日"：日界没跨过就维持"今日已同步"，
  // 跨过了则 day != lastAutoSyncDay，下一轮自然补一次同步（lastAutoTryDay 也已放开）。
  static int32_t seenTz = INT32_MIN;   // 非法偏移，保证首次调用必定进入重建分支
  if (seenTz != tzOffsetSec) {
    seenTz = tzOffsetSec;
    lastAutoSyncDay = (lastSyncEpoch != 0) ? (unsigned long)timeLocalDayOf(lastSyncEpoch) : 0;
    lastAutoTryDay  = 0;
  }

  if (WiFi.status() != WL_CONNECTED || ntpIsBusy()) return;
  if (lastSyncEpoch == 0 || lastAutoSyncDay == 0) return;   // 从未同步过则跳过
  unsigned long day = (unsigned long)timeLocalDayOf(currentEpoch);
  if (day == lastAutoSyncDay) return;        // 今天已同步成功
  if (lastAutoTryDay == day) return;         // 今天已尝试过（无论成败），避免失败后无限重试
  if (tzSecOfDay(currentEpoch) < 10) return; // 等过了本地 00:00:10（用 epoch 而非 currentTime，防时间字段冻结时恒真死锁）
  lastAutoTryDay = day;
  logRaw("[NTP] 每日自动校准...\n");
  ntpStartSync(false);
}