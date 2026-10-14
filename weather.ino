#include "config.h"
#include <HTTPClient.h>
#include <WiFiClient.h>          // 诊断用的明文 TCP 探测（WiFiClient = NetworkClient）
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ===================== 天气模块（后端抓取）=====================
// 自动模式：iPIP IP 定位（国内优先，UTF-8 JSON）优先；判定国外 -> 回退 ipwho.is（全球经纬度）。
// 注：原用的太平洋 pconline 接口已退役，连带删掉了 gb2312_to_utf8.h（省约 16KB flash）。
//           国内 = 省市区 -> Open-Meteo 地理编码 -> 查天气；国外 = ipwho.is 经纬度 -> 查天气。
// 手动模式：省市区中文名 -> Open-Meteo 地理编码 -> Open-Meteo 查天气。
// 天气现象直接使用 Open-Meteo 返回的 WMO 天气码（WMO 4680 子集，共 28 个码），
// 经 wmoText/wmoIcon 输出中文文案与 emoji 图标（前端不再维护码表）。
// 每小时整点自动刷新。每一步都通过 logf() 打印到串口（同时写入环形日志缓冲区，可网页回看）。

// ===== 天气状态缓存（前端只读，通过 /weatherstatus 获取）=====
static String   wMode     = "auto";    // "auto" 网络定位 / "manual" 手动定位
static String   wProCode    = "";      // 数字行政区划码 · 省（纯 ASCII，无需转码）
static String   wCityCode   = "";      // 数字行政区划码 · 市
static String   wRegionCode = "";      // 数字行政区划码 · 区/县
static String   wSource   = "";        // "ipwho.is/Open-Meteo" / "手动"
static float    wLat = 0, wLon = 0;
static bool     wLocated  = false;
static bool     wWeatherOk = false;
static int      wCode = 0;
static float    wTemp = 0, wFeels = 0, wWind = 0;
static int      wHumidity = 0;
static float    wTmax = 0, wTmin = 0;
static bool     wBusy = false;
static String   wIp   = "";      // 定位 IP（ipwho.is 返回；手动为空）
static String   wCity = "";      // 用于显示的城市名（自动=ipwho.is city，手动=所选城市）
static String   wUpdated = "";   // 天气数据最后一次成功获取的时刻 "HH:MM:SS"（前端「更新」显示；空=暂无有效数据）

// ===== 当日太阳时刻（自当地 00:00 起的分钟数，0..1439；-1 = 暂无数据/当日无此事件）=====
//   日出/日落：取自 Open-Meteo forecast 的 daily.sunrise / daily.sunset（分钟精度）
//   晨光/昏光：由本地 NOAA 算法算出（太阳高度角 -18° 的天文晨昏蒙影，见 solar.ino），不联网
// 与天气数据同样"抓取失败保留旧值"，避免刷新期间面板闪成空（理由见 weatherRunOnce 注释）。
static int      wSunriseMin = -1;
static int      wSunsetMin  = -1;
static int      wDawnMin    = -1;
static int      wDuskMin    = -1;

// ===== 抓取状态机 =====
// 0=空闲 1=iPIP定位(自动,国内优先) 2=地理编码(手动/国内) 3=ipwho.is定位(国外回退) 4=查天气(Open-Meteo)
static int    wStage = 0;
static String wGeoName = "";      // 地理编码的城市名（UTF-8，手动/国内自动共用）
static bool   wTriedIpwho = false;// 自动模式是否已尝试 ipwho.is（地理编码失败时兜底回退用）
static bool   wTzAuto = true;     // 时区是否为自动模式（manual 时定位结果不许覆盖用户设定；每轮抓取开头刷新）
static volatile int lastWeatherAutoSlot = -1;   // 上次整点自动刷新的小时槽（仅主 loop 的 weatherTick 读写）
static volatile bool wArmAfterNtp = false;  // NTP 已发起，待其结束后跑一次完整链路（定位+天气）
                                            // 只在 NTP 发起那一刻置位 —— 若在定位阶段就置位，
                                            // 此时 ntpIsBusy() 仍为 false，weatherTick 会抢跑天气、跳过定位与对时。
// 启动时序：定位(求时区) -> NTP(拿 UTC) -> 天气。本值只描述"下一轮跑哪种"：
//   0 = 无（正常完整链路：定位 + 天气）
//   1 = 仅定位，跑完自动发起 NTP
//   2 = 仅定位，跑完发起 NTP 且按"手动同步"上报（前端「同步网络时间」走这条）
// 由 weatherRunOnce 读走即清，随后由它自己调用 ntpStartSync 交棒（不放在 loop 里判定，
// 否则 weatherStart 刚发通知、wBusy 还没置起来时会被误判成"定位已完成"）。
static volatile int wLocatePhase = 0;

// ===== 后台抓取任务（低优先级 + 通知驱动）=====
// 天气抓取在专用低优先级任务 weatherTaskLoop（prio=1，与 loopTask 同级，pin CPU1）里同步执行，
// 用 ulTaskNotifyTake 阻塞等待触发通知：无请求时任务处于 blocked 态（0% CPU），idle 可正常喂狗，
// 故不会饿死空闲任务触发 task_wdt（上次崩溃根因）；抓取期间仅阻塞该任务自身，主 loop 与 UI 不受影响。
// 与主 loop 的 NTP 通过 netMutex（NetLockGuard）串行，避免 LwIP 并发崩溃。
// weatherMutex 保护 w* 状态"更新的瞬间"（与 weatherBuildStatus 读取互斥，微秒级）。
static SemaphoreHandle_t weatherMutex = NULL;
static TaskHandle_t     weatherTaskHandle = NULL;

static void weatherLock()   { if (weatherMutex) xSemaphoreTake(weatherMutex, portMAX_DELAY); }
static void weatherUnlock() { if (weatherMutex) xSemaphoreGive(weatherMutex); }

// UTF-8 字节做百分号编码（用于拼 Open-Meteo 的 name 参数）
static String weatherUrlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// HTTPS 连接的"必定关闭"守卫（与 NetLockGuard / StateLockGuard 同一套作用域写法）。
//
// 为什么必须显式关闭：HTTPClient::end() 并不可靠 —— disconnect() 先要求 connected() 为真，
// 里面还分了 keep-alive 分支（_reuse && _canReuse → 只打日志、不关闭）。而关闭 TLS 的
// socket fd 与释放 mbedTLS 上下文，唯一入口是 NetworkClientSecure::stop() → stop_ssl_socket()。
// NetworkClientSecure 的析构函数只 free 证书内存、基类析构是空的，二者都不关。
// 本工程每次请求都用全新的 WiFiClientSecure，keep-alive 根本无从复用，一旦漏掉 stop()
// 就每请求泄漏「1 个 socket + 1 份 mbedTLS 上下文」：
//   - lwIP 的 socket 数量有限，十来个请求后耗尽 → 所有后续连接失败（GET=-1）；
//   - mbedTLS 上下文（数十 KB）不释放 → 堆持续下降并碎片化。
// 症状正是"重启后能查一两次、之后手动更新永远失败"——只有重启才归还这些资源。
class SslStopGuard {
public:
  NetworkClientSecure& _c;
  explicit SslStopGuard(NetworkClientSecure& c) : _c(c) {}
  ~SslStopGuard() { _c.stop(); }
};

// 明文 TCP 连接的"必定关闭"守卫（与 SslStopGuard 同一套作用域写法）。
// 明文没有 mbedTLS 上下文要释放，但 socket fd 同样必须显式关闭 —— 理由与 SslStopGuard 完全相同。
class ClientStopGuard {
public:
  NetworkClient* _c;
  explicit ClientStopGuard(NetworkClient* c) : _c(c) {}
  ~ClientStopGuard() { if (_c) _c->stop(); }
};

// 发起 GET 并解析 JSON；失败返回 false。
//   plain = false（默认）：HTTPS，常规通道（跳过证书校验，个人设备可接受）
//   plain = true          ：明文 HTTP（端口 80），只给"查天气失败后的降级重试"用
// 两条通道共用同一段请求代码，保证降级只换传输层、不换请求内容（URL 参数逐字相同）。
static bool weatherHttpGet(const String& url, int timeoutMs, DynamicJsonDocument& doc, bool plain = false) {
  WiFiClientSecure ssl;
  NetworkClient    tcp;
  NetworkClient*   cli = plain ? static_cast<NetworkClient*>(&tcp) : static_cast<NetworkClient*>(&ssl);
  if (!plain) ssl.setInsecure();
  ClientStopGuard stopGuard(cli);   // 出作用域必定关闭（见上方说明）
  HTTPClient http;
  http.setReuse(false);   // 全新 client，keep-alive 用不上；关掉它才会走 end() 里的 stop() 分支
  // HTTPClient 里"连接超时"与"读超时"是两个独立变量，必须分别设置：
  //   setTimeout()        -> _tcpTimeout     ，只管连上之后的读写
  //   setConnectTimeout() -> _connectTimeout ，管建立连接（TCP + TLS 握手）
  // 只设前者时，连接阶段会一直用默认的 5000ms —— 日志里"GET=-1 且耗时恰好 5.0s"
  // 就是它到点，表现为"URL 与参数都没问题却连不上"。两处都设，timeoutMs 才是整个请求的预算。
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  // 日志里标明本次走的是哪条通道（HTTPS / 明文）—— 降级时同一轮会有两条 [HTTP] 行，必须能区分
  const char* ch = plain ? "明文" : "HTTPS";
  if (!http.begin(*cli, url)) { http.end(); logf("[HTTP] %s begin失败 heap=%u\n", ch, (unsigned)ESP.getFreeHeap()); return false; }
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
  http.addHeader("Accept", "application/json, text/plain, */*");
  http.addHeader("Accept-Language", "zh-CN,zh;q=0.9");
  int code = http.GET();
  if (code != 200) { http.end(); logf("[HTTP] %s GET=%d heap=%u\n", ch, code, (unsigned)ESP.getFreeHeap()); return false; }
  String body = http.getString();
  http.end();
  if (body.length() == 0) { logf("[HTTP] %s body为空 heap=%u\n", ch, (unsigned)ESP.getFreeHeap()); return false; }
  DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    // NoMemory 即堆碎片/容量不足，是“运行一整天后步骤2失败”的最直接证据
    logf("[HTTP] %s JSON解析失败:%s body=%u字节 heap=%u\n", ch, err.c_str(), (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
    return false;
  }
  return true;
}

// 天气现象直接使用 Open-Meteo 返回的 WMO 天气码（WMO 4680 子集，共 28 个码）。
// 码表来源：Open-Meteo 官方 weather_code（WMO Weather interpretation codes）。
// WMO 天气码 -> 中文文案
static const char* wmoText(int c) {
  switch (c) {
    case 0: return "晴";
    case 1: return "大部晴朗";
    case 2: return "局部多云";
    case 3: return "阴";
    case 45: return "雾";
    case 48: return "雾凇";
    case 51: return "小毛毛雨";
    case 53: return "毛毛雨";
    case 55: return "大毛毛雨";
    case 56: return "冻毛毛雨";
    case 57: return "强冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: return "冻雨";
    case 67: return "强冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80: return "小阵雨";
    case 81: return "阵雨";
    case 82: return "强阵雨";
    case 85: return "阵雪";
    case 86: return "强阵雪";
    case 95: return "雷暴";
    case 96: return "雷暴伴小冰雹";
    case 99: return "雷暴伴大冰雹";
    default: return "未知";
  }
}

// WMO 天气码 -> 图标（emoji）
static const char* wmoIcon(int c) {
  switch (c) {
    case 0: return "☀️";   // 晴
    case 1: return "🌤️";   // 大部晴朗
    case 2: return "⛅";   // 局部多云
    case 3: return "☁️";   // 阴
    case 45: return "🌫️";  // 雾
    case 48: return "🌫️";  // 雾凇
    case 51: return "🌦️";  // 小毛毛雨
    case 53: return "🌦️";  // 毛毛雨
    case 55: return "🌧️";  // 大毛毛雨
    case 56: return "🌧️";  // 冻毛毛雨
    case 57: return "🌧️";  // 强冻毛毛雨
    case 61: return "🌧️";  // 小雨
    case 63: return "🌧️";  // 中雨
    case 65: return "🌧️";  // 大雨
    case 66: return "🌧️";  // 冻雨
    case 67: return "🌧️";  // 强冻雨
    case 71: return "🌨️";  // 小雪
    case 73: return "🌨️";  // 中雪
    case 75: return "❄️";  // 大雪
    case 77: return "🌨️";  // 雪粒
    case 80: return "🌦️";  // 小阵雨
    case 81: return "🌧️";  // 阵雨
    case 82: return "🌧️";  // 强阵雨
    case 85: return "🌨️";  // 阵雪
    case 86: return "🌨️";  // 强阵雪
    case 95: return "⛈️";  // 雷暴
    case 96: return "⛈️";  // 雷暴伴小冰雹
    case 99: return "⛈️";  // 雷暴伴大冰雹
    default: return "🌡️";  // 未知
  }
}

// 步骤1：IP 定位（iPIP，国内优先）。
// 成功且是国内 -> 转地理编码（拿经纬度）；失败或国外 IP -> 回退 ipwho.is（直接给经纬度+时区）。
//
// 为什么从太平洋（whois.pconline.com.cn）换成 iPIP：
//   1) 太平洋那个免费域名挂在一个多节点 CDN 上，解析结果会在 13~21 个节点之间浮动，
//      其中掺着从本线路完全不可达的节点（实测 21 个里 8 个 3 秒无响应）；而且它返回的是
//      GBK 网页里的 JS 字面量，必须靠 gb2312_to_utf8.h 那张表转码 ——
//      表覆盖 0xA1A1~0xF7FE，94 列 × 87 行 × 2 字节 ≈ 16KB flash，为一句话的定位付这个代价不值。
//   2) iPIP 返回 UTF-8 JSON，字段直接可用，且支持明文 HTTP —— 省掉一次 TLS 握手
//      （本机 CPU 只有 80MHz，这笔开销不小）。接口：GET http://myip.ipip.net/json
//          {"ret":"ok","data":{"ip":"1.2.3.4","location":["中国","辽宁","大连","","联通"]}}
//   3) 两个域名都有"解析器给不给可达子集"的问题（ECS 相关），所以配套在 wifiTick 里
//      指定了公共 DNS，见 config.h 的 WEATHER_PUBLIC_DNS 说明。
//
// 自动定位只精确到【市】：iPIP 不返回区县（location[3] 恒为空），而地理编码只要有城市名就够了。
// 手动定位仍是网页上选的省/市/区三级，走的是完全不同的分支（wStage=2），与本函数无关。
static void weatherDoIpipLocate() {
  logRaw("[天气] 步骤1：IP定位（iPIP，国内优先）…\n");
  DynamicJsonDocument doc(2048);
  // 5s：国内访问 <1s 足够，连不上时快速失败回退 ipwho.is（与原来太平洋那级的预算一致）
  if (!weatherHttpGet("http://myip.ipip.net/json", 5000, doc, /*plain=*/true)) {
    logRaw("[天气] iPIP 无响应，回退 ipwho.is…\n");
    wStage = 3; return;
  }

  // ret 是显式成功标志：缺失或非 "ok" 一律当失败，不去猜内容
  const char* ret = doc["ret"] | "";
  if (strcmp(ret, "ok") != 0) {
    logf("[天气] iPIP 返回 ret=%s，回退 ipwho.is…\n", ret);
    wStage = 3; return;
  }

  JsonArray loc = doc["data"]["location"];
  String country = (loc && loc.size() > 0) ? (const char*)(loc[0] | "") : "";
  String prov    = (loc && loc.size() > 1) ? (const char*)(loc[1] | "") : "";
  String city    = (loc && loc.size() > 2) ? (const char*)(loc[2] | "") : "";

  // 判据：只有"国家字段命中中国"才算国内。
  // 用 startsWith 而不是 == ：万一带了空格或返回"中国大陆"，也能命中；而不会有别的
  // 国家名以"中国"开头，所以不会误判。字段缺失时 country 为空串，startsWith 返回 false，
  // 同样落到回退分支 —— 即"任何非中国的情况一律退让"。
  //
  // 后半个条件（省市都空）是防御：国家是"中国"但省市都没给出来（数据异常，或免费库
  // 精度不足），地理编码就没有名字可用，与其拿空串去查，不如直接交给 ipwho.is ——
  // 它本来就直接返回经纬度，不需要名字。
  //
  // 注意：**不能再用"省市是否为空"判国外** —— 那是原太平洋接口的判法，它只覆盖中国大陆；
  // iPIP 是覆盖全球的库，国外 IP 会返回"美国/加利福尼亚/洛杉矶"这类非空值，套旧判据会把
  // 国外误判成国内，并错误地设成 UTC+8。
  if (!country.startsWith("中国") || (prov.length() == 0 && city.length() == 0)) {
    logf("[天气] iPIP 未命中中国（%s %s %s），回退 ipwho.is…\n",
         country.c_str(), prov.c_str(), city.c_str());
    wStage = 3; return;
  }

  weatherLock();
  wIp      = doc["data"]["ip"] | "";
  wGeoName = city.length() ? city : prov;   // 地理编码优先城市，其次省份（iPIP 没有区县）
  wCity    = wGeoName;
  wProCode = ""; wCityCode = ""; wRegionCode = "";   // iPIP 不提供行政编码，清空避免残留上一次的值
  wSource  = "iPIP/Open-Meteo";
  weatherUnlock();

  // 国内无夏令时，恒 UTC+8（与原来太平洋那级一致）
  if (wTzAuto) timeSetTzOffset(28800);

  logf("[天气] iPIP 定位成功：%s（ip=%s 省=%s 市=%s）\n",
       wGeoName.c_str(), wIp.c_str(), prov.c_str(), city.c_str());
  wStage = 2;   // 国内：城市名 -> 地理编码 -> 查天气（wStage 为任务内部流程变量，无需锁）
}

// 步骤（回退）：IP 定位（ipwho.is，全球，直接返回经纬度）
static void weatherDoIpLocate() {
  wTriedIpwho = true;
  logRaw("[天气] IP定位（ipwho.is，全球）…\n");
  DynamicJsonDocument doc(2048);
  if (!weatherHttpGet("https://ipwho.is/", 8000, doc)) {
    logRaw("[天气] IP定位失败（ipwho.is 无响应）\n");
    weatherLock(); wBusy = false; wStage = 0; weatherUnlock(); return;
  }
  bool ok = doc["success"] | false;
  if (!ok) {
    logf("[天气] IP定位失败：%s\n", (const char*)(doc["message"] | "unknown"));
    weatherLock(); wBusy = false; wStage = 0; weatherUnlock(); return;
  }
  weatherLock();
  wIp      = doc["ip"] | "";
  wCity    = doc["city"] | "";
  wLat     = doc["latitude"] | 0.0f;
  wLon     = doc["longitude"] | 0.0f;
  wLocated = true;
  wSource  = "ipwho.is/Open-Meteo";
  weatherUnlock();

  // 时区：一律取"冬令时"基准。ipwho.is 的 offset 是当前实际偏移（夏令时期间含 +1h），
  // 故夏令时生效时减去 1 小时得到标准时偏移 —— 与"设备时钟全年不跳变"的策略一致。
  if (wTzAuto) {
    long tzOff = doc["timezone"]["offset"] | 0L;
    bool isDst = doc["timezone"]["is_dst"] | false;
    timeSetTzOffset((int32_t)(isDst ? tzOff - 3600L : tzOff));
  }

  logf("[天气] IP定位成功：%s（%.4f, %.4f，国家 %s）\n",
       wCity.c_str(), wLat, wLon, (const char*)(doc["country"] | ""));
  wStage = 4;
}

// 地理编码（Open-Meteo geocoding，城市名 -> 经纬度；手动模式 + 国内自动共用）
static void weatherDoGeocode() {
  logf("[天气] 地理编码（%s）…\n", wGeoName.c_str());
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
               weatherUrlEncode(wGeoName) + "&count=1&language=zh&format=json";
  DynamicJsonDocument doc(2048);
  if (!weatherHttpGet(url, 8000, doc)) {
    // 自动模式（国内 iPIP）地理编码失败时兜底回退 ipwho.is
    if (wMode == "auto" && !wTriedIpwho) { logRaw("[天气] 地理编码失败，回退 ipwho.is…\n"); wStage = 3; return; }
    logRaw("[天气] 地理编码失败\n");
    weatherLock(); wBusy = false; wStage = 0; weatherUnlock(); return;
  }
  JsonArray results = doc["results"];
  if (!results || results.size() == 0) {
    if (wMode == "auto" && !wTriedIpwho) { logRaw("[天气] 地理编码无结果，回退 ipwho.is…\n"); wStage = 3; return; }
    logRaw("[天气] 地理编码失败（无结果）\n");
    weatherLock(); wBusy = false; wStage = 0; weatherUnlock(); return;
  }
  weatherLock();
  wLat = results[0]["latitude"] | 0.0f;
  wLon = results[0]["longitude"] | 0.0f;
  wLocated = true;
  weatherUnlock();
  logf("[天气] 坐标：%.4f, %.4f\n", wLat, wLon);
  wStage = 4;
}

// 从 ISO8601 时间串（"2026-09-23T06:02"）取出 "HH:MM" 的分钟数；失败返回 -1。
// Open-Meteo 的 sunrise/sunset 固定是 "YYYY-MM-DDTHH:MM"（不带时区后缀，时区由响应顶层的
// utc_offset_seconds 统一约定），所以只需截取 'T' 之后的部分。
static int weatherParseHm(const char* iso) {
  if (!iso || !iso[0]) return -1;
  const char* t = strchr(iso, 'T');
  if (!t) return -1;
  int hh = -1, mm = -1;
  if (sscanf(t + 1, "%d:%d", &hh, &mm) != 2) return -1;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

// 分钟数 -> "HH:MM"；-1（无数据/当日无事件）返回空串，由前端回落成占位符
static String weatherFmtHm(int minOfDay) {
  if (minOfDay < 0) return String();
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", minOfDay / 60, minOfDay % 60);
  return String(b);
}

// ===== 主机可达性诊断（仅在"查询天气"失败时调用，只读、不改任何行为）=====
// 目的：把"连不上 api.open-meteo.com"拆成三层来定位，避免继续停留在猜测：
//   ① 解析失败 / 极慢      → DNS 被污染或不可用
//   ② 解析成功但 TCP 不通   → IP 层被封锁（DNS 拿到了黑洞/被墙 IP）
//   ③ 解析通、TCP 也通      → 问题在 TLS/SNI 层（主机名被针对），或服务端异常
// 内置两组【对照组】——地理编码与定位源是同一轮里必然成功的主机；
// 若对照组通、api 不通，就能把结论锁死在"针对该主机"，与 URL 参数/代码逻辑无关。
static void weatherDiagnoseHosts() {
  static const char* const HOSTS[] = {
    "api.open-meteo.com",            // 查询天气：每轮必失败
    "geocoding-api.open-meteo.com",  // 地理编码：每轮必成功（对照）
    "myip.ipip.net",                 // 定位源：每轮必成功（对照）
  };
  logRaw("[诊断] 主机可达性（解析 + TCP 直连 443）：\n");
  for (size_t i = 0; i < sizeof(HOSTS) / sizeof(HOSTS[0]); i++) {
    IPAddress ip;
    unsigned long t0 = millis();
    int ok = WiFi.hostByName(HOSTS[i], ip);
    unsigned long dtR = millis() - t0;
    if (!ok) {
      logf("[诊断] %s 解析失败（%lums）\n", HOSTS[i], dtR);
      continue;
    }
    WiFiClient plain;
    unsigned long t1 = millis();
    bool tcpOk = plain.connect(ip, 443, 3000);
    unsigned long dtC = millis() - t1;
    if (tcpOk) plain.stop();
    logf("[诊断] %s -> %s 解析%lums，TCP %s（%lums）\n",
         HOSTS[i], ip.toString().c_str(), dtR, tcpOk ? "通" : "不通", dtC);
  }
}

// 构建 forecast 查询串。三级降级与「测试联通」共用同一份 ——
// 保证"测的"就是"真的会发的那条 URL"，而不是另写一套简化版（那样测通了也可能真跑不通）。
static void weatherBuildForecastQuery(char* q, size_t n, float lat, float lon) {
  snprintf(q, n,
           "latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&timezone=auto&forecast_days=1&wind_speed_unit=ms",
           lat, lon);
}

// 查天气（Open-Meteo forecast）
// 三级降级：逐级只在上一级失败时才走，任一级成功即停；三级全失败则放弃，等下一个整点触发。
//   ① HTTPS 直连 api.open-meteo.com   —— 正常通道
//   ② 明文 HTTP 直连（端口 80）        —— 少一层 TLS 握手，少一层死法
//   ③ 接口转发（用户自填，留空则整级跳过）—— 换的是路由，不是车道
static void weatherDoForecast() {
  logRaw("[天气] 查询天气（Open-Meteo forecast）…\n");

  // 查询串只构建一次，三级共用 —— 保证降级只换"往哪发"、不换"发什么"，
  // 出问题时可以干净地排除"参数写错"这类干扰。
  char q[320];
  weatherBuildForecastQuery(q, sizeof(q), wLat, wLon);

  // 512 字节，比原来（384）放大是按第③级算的：
  //   转发地址(≤192，保存时已卡上限) + 分隔符(1) + 查询串(实测约 251) ≈ 444。
  // 前两级只用 290 字节左右。缓冲区不足时会【静默截断】而不是报错 —— 放大时务必留足余量。
  char url[512];

  DynamicJsonDocument doc(4096);
  int tier = 0;   // 0=都失败；>0 = 由第几级取到（仅用于日志统计）

  // ① HTTPS 直连
  snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?%s", q);
  bool got = weatherHttpGet(String(url), 8000, doc);
  if (got) tier = 1;

  // ② 明文 HTTP 直连
  if (!got) {
    // 为什么值得做：天气是编排的最后一棒，走到这里失败，本轮已经注定失败
    //   （回退由下一个整点触发兜底）。所以多掷一次骰子是纯上行期权：成了白赚、不成不亏，
    //   最坏只是前端「更新中」多显示几秒。
    // 为什么换传输而不是重试 HTTPS：同一个采样点再问一次，在路径劣化面前没有意义；
    //   明文少一层 TLS 握手（多轮往返的脆弱交换，丢包时每一轮都是一个死点），少一层死法。
    // 实测依据（2026-09-23 22:36，同一网络、同一时刻）：HTTPS 0/3 成功，HTTP 1/3 成功。
    // 超时给 8000ms 而非更短：实测唯一成功那次耗时 13s，给太短等于自己把机会掐掉。
    //   （不会牵连别人：NTP 走 NetLockTryGuard 非阻塞抢锁，抢不到只跳几拍。）
    // 不作用于 iPIP 定位 / 地理编码：它们历来稳定，无谓增加失败面。
    snprintf(url, sizeof(url), "http://api.open-meteo.com/v1/forecast?%s", q);
    logRaw("[天气] HTTPS 失败，降级明文 HTTP 重试…\n");
    got = weatherHttpGet(String(url), 8000, doc, true);
    if (got) tier = 2;
  }

  // ③ 接口转发（用户自填的中转地址；留空 = 关闭该级，与未配置时行为完全一致）
  if (!got) {
    String raw;
    loadRelayConfigRaw(raw);
    String relay;
    if (raw.length() > 0) {
      DynamicJsonDocument rdoc(512);
      if (deserializeJson(rdoc, raw) == DeserializationError::Ok) {
        relay = (const char*)(rdoc["url"] | "");
      }
    }
    relay.trim();
    if (relay.length() > 0) {
      // 转发地址可能自带查询串（例如 .../api/weather?key=xxx），所以要按情况选 ? 还是 &
      const char* sep = (relay.indexOf('?') >= 0) ? "&" : "?";
      snprintf(url, sizeof(url), "%s%s%s", relay.c_str(), sep, q);
      // 转发地址也可能是明文 http（用户自填，不受我们控制），此时客户端必须跟着走明文通道
      bool relayPlain = relay.startsWith("http://");
      logRaw("[天气] 直连均失败，改用接口转发…\n");
      got = weatherHttpGet(String(url), 8000, doc, relayPlain);
      if (got) tier = 3;
    }
  }

  if (!got) {
    logRaw("[天气] 天气查询失败（三级均未取到）\n");
    weatherDiagnoseHosts();   // 拆开 DNS / TCP / TLS 三层定位（详见该函数注释）
    weatherLock(); wBusy = false; wStage = 0; weatherUnlock(); return;
  }
  // 留痕：便于日后统计各级救场率（正常网络下这两行不会出现）
  if (tier == 2) logRaw("[天气] 降级生效：本轮由明文 HTTP 取到数据\n");
  if (tier == 3) logRaw("[天气] 中转生效：本轮由接口转发取到数据\n");
  weatherLock();
  JsonObject cur = doc["current"];
  wCode     = cur["weather_code"] | 0;                 // 直接存 WMO 天气码
  wTemp     = cur["temperature_2m"] | 0.0f;
  wHumidity = (int)cur["relative_humidity_2m"].as<float>();   // 湿度字段按 float 读取再转 int
  wFeels    = cur["apparent_temperature"] | 0.0f;
  wWind     = cur["wind_speed_10m"] | 0.0f;   // wind_speed_unit=ms，单位 m/s

  JsonArray dmax = doc["daily"]["temperature_2m_max"];
  JsonArray dmin = doc["daily"]["temperature_2m_min"];
  wTmax = (dmax && dmax.size() > 0) ? (float)dmax[0] : wTemp;
  wTmin = (dmin && dmin.size() > 0) ? (float)dmin[0] : wTemp;

  // ===== 当日太阳时刻 =====
  // 日出/日落：直接取 Open-Meteo 的 daily.sunrise / daily.sunset；
  // 晨光/昏光：由本地 NOAA 算法算出（见 solar.ino）。
  // 两者统一使用响应顶层的 utc_offset_seconds（timezone=auto 解析出的当地偏移）作为时区基，
  // 保证四项互相对齐 —— 否则会出现"日出按 auto 时区、晨光按设备时区"的错位（国外夏令时差 1 小时）。
  // 该字段缺失时才退回设备时区 tzOffsetSec。
  {
    int32_t sunTz = (int32_t)(doc["utc_offset_seconds"] | (long)tzOffsetSec);

    JsonArray dsr = doc["daily"]["sunrise"];
    JsonArray dss = doc["daily"]["sunset"];
    wSunriseMin = weatherParseHm((dsr && dsr.size() > 0) ? (const char*)(dsr[0] | "") : "");
    wSunsetMin  = weatherParseHm((dss && dss.size() > 0) ? (const char*)(dss[0] | "") : "");

    // 日期取响应里的 daily.time[0]（"YYYY-MM-DD"），保证与上面的日出/日落是同一天
    int ys = 0, ms = 0, ds = 0;
    JsonArray dt = doc["daily"]["time"];
    if (dt && dt.size() > 0) sscanf((const char*)(dt[0] | ""), "%d-%d-%d", &ys, &ms, &ds);
    if (ys > 0) {
      SolarTimes st;
      if (solarComputeTimes(wLat, wLon, ys, ms, ds, sunTz, st)) {
        wDawnMin = st.dawnMin;
        wDuskMin = st.duskMin;
      }
    }
    logf("[天气] 太阳时刻：日出=%s 日落=%s 晨光=%s 昏光=%s（时区 %+ld 分钟）\n",
         weatherFmtHm(wSunriseMin).c_str(), weatherFmtHm(wSunsetMin).c_str(),
         weatherFmtHm(wDawnMin).c_str(),   weatherFmtHm(wDuskMin).c_str(),
         (long)sunTz / 60);
  }

  wWeatherOk = true;
  wBusy = false;
  wStage = 0;
  char clock[16];
  snprintf(clock, sizeof(clock), "%02d:%02d:%02d", currentTime.hour, currentTime.minute, currentTime.second);
  wUpdated = clock;      // 记录数据落地时刻，前端显示为「更新：HH:MM:SS」
  weatherUnlock();
  logf("[天气] 获取成功：%s %.0f°C（体感 %.0f°C，湿度 %d%%，风 %.0f m/s）\n",
       wCity.c_str(), wTemp, wFeels, wHumidity, wWind);
}

// 读时区模式：未配置 / 解析失败一律按 auto（保持跟随定位的旧行为）
static bool tzConfigIsAuto() {
  String tzJson;
  loadTzConfigRaw(tzJson);
  if (tzJson.length() == 0) return true;
  DynamicJsonDocument tzDoc(128);
  if (deserializeJson(tzDoc, tzJson) != DeserializationError::Ok) return true;
  return strcmp(tzDoc["mode"] | "auto", "auto") == 0;
}

// 直辖市省码（北京/天津/上海/重庆）。这四个的"市级"在国标行政区划数据里叫「市辖区」
// （重庆另有一枝「县」），不是地名 —— 显示与地理编码一律改用省名，即"北京市-北京市"。
static bool weatherIsMuniProv(const String& code) {
  return code == "110000" || code == "120000" || code == "310000" || code == "500000";
}

// 解析天气定位配置，返回本轮的起始阶段：手动模式且位置有效 → 2（地理编码）；
// 其余情况（自动 / 未配置 / 手动但无位置）→ 1（IP 定位）。
// 手动模式命中时顺带把位置写入 w* 状态。
static int weatherResolveStartStage(const String& json) {
  DynamicJsonDocument doc(512);
  if (json.length() == 0 || deserializeJson(doc, json) != DeserializationError::Ok) return 1;
  if (strcmp(doc["mode"] | "auto", "manual") != 0) return 1;

  String provCode = doc["provCode"] | "";
  String prov     = doc["prov"] | "";
  // 手动模式下若 NVS 中没有手动位置信息（省码/省名均为空），回退网络定位
  if (provCode.length() == 0 && prov.length() == 0) {
    logRaw("[天气] NVS 无手动位置信息，回退网络定位…\n");
    return 1;
  }

  String city     = doc["city"] | "";
  String dist     = doc["dist"] | "";
  String cityCode = doc["cityCode"] | "";
  String distCode = doc["distCode"] | "";
  String geoName;
  // 地理编码名选择（与前端逻辑一致：优先城市，其次城市+区县，最后省份）
  // 直辖市例外：市级是「市辖区」/「县」，不是地名，直接取省名（"北京市"）——
  //   按省码判断而不是看名字，NVS 里已存的旧「市辖区」配置也一并被修正；
  //   不拼区名（"东城区"全国重名），与普通省份"粒度只到市"的既有行为保持一致。
  if (weatherIsMuniProv(provCode))                              geoName = prov;
  else if (city.length() && city != "市辖区" && city != "县")   geoName = city;
  else if (city.length() && dist.length())                     geoName = city + dist;
  else if (dist.length())                                      geoName = dist;
  else                                                         geoName = prov;

  weatherLock();
  wMode = "manual";
  wSource = "手动";
  wProCode = provCode;
  wCityCode = cityCode;
  wRegionCode = distCode;
  wGeoName = geoName;
  wCity = geoName;   // 手动模式城市名（无 IP）
  weatherUnlock();

  logf("[天气] 手动模式位置：prov=%s city=%s dist=%s（地理编码名=%s）\n",
       provCode.c_str(), cityCode.c_str(), distCode.c_str(), geoName.c_str());
  return 2;   // 手动模式：地理编码 -> 查天气
}

// ===== 接口转发「测试联通」=====
// 异步触发 + 轮询结果：HTTP handler 只置标志就返回，真正的请求在这里做
//（与手动 NTP 同步"只发起、不阻塞"是同一套思路）。
// 为什么必须异步：请求要在天气任务内、持 netMutex 执行。若放进 HTTP handler 同步做，
// 就得持着 stateMutex 阻塞 8 秒以上，主循环（PWM/序列器/计数器）会被一起冻住。
// 状态：0=未测 1=进行中 2=成功 3=失败。写者只有天气任务一个，HTTP 任务只读。
static volatile int           wTestState = 0;
static volatile int           wTestCode  = 0;       // 失败时的 HTTP 码；连接阶段就失败为 0
static volatile unsigned long wTestMs    = 0;
static volatile float         wTestTemp  = -999.0f;
static volatile bool          wTestReq   = false;
static const char*            wTestMsg   = "";      // 只用字符串字面量：跨任务读 String 会撕裂
static String                 wTestUrl;             // 交接一次，任务侧随即本地化

bool weatherRelayTestStart(const String& url) {
  if (wTestState == 1) return false;   // 已有测试在跑：不排队（用户正看着界面，排队只会更迷惑）
  wTestUrl   = url;
  wTestCode  = 0;
  wTestMs    = 0;
  wTestTemp  = -999.0f;
  wTestMsg   = "";
  wTestState = 1;
  wTestReq   = true;
  weatherStart();                      // 只发通知，立即返回
  return true;
}
int  weatherRelayTestState() { return wTestState; }
int  weatherRelayTestCode()  { return wTestCode; }
unsigned long weatherRelayTestMs() { return wTestMs; }
float weatherRelayTestTemp() { return wTestTemp; }
const char* weatherRelayTestMsg() { return wTestMsg; }

// 真正执行测试（由 weatherRunOnce 调用，此时 netMutex 已持有）
static void weatherRunRelayTest() {
  String relay = wTestUrl;             // 先本地化：wTestState==1 期间不会被下一次 start 覆盖
  relay.trim();
  if (relay.length() == 0) { wTestMsg = "地址为空"; wTestState = 3; return; }

  // 与第③级完全同一套拼接与请求代码 —— 测的就是真实链路，不是简化版
  char q[320];
  weatherBuildForecastQuery(q, sizeof(q), wLat, wLon);
  char url[512];
  const char* sep = (relay.indexOf('?') >= 0) ? "&" : "?";
  snprintf(url, sizeof(url), "%s%s%s", relay.c_str(), sep, q);
  bool plain = relay.startsWith("http://");

  logf("[天气] 测试接口转发：%s\n", url);
  unsigned long t0 = millis();
  DynamicJsonDocument doc(4096);
  bool ok = weatherHttpGet(String(url), 8000, doc, plain);
  wTestMs = millis() - t0;

  if (!ok) {
    wTestCode = 0;   // 具体错误码已由 weatherHttpGet 打进串口日志的 [HTTP] 行
    wTestMsg  = "连接失败或被拒绝（串口 [HTTP] 那行有具体错误码）";
    wTestState = 3;
    logf("[天气] 测试失败（%lu ms）\n", wTestMs);
    return;
  }
  // 光有 200 不够：地址写错时也可能返回某个 JSON。必须确认拿到的是天气数据结构，
  // 否则用户会以为"通了"，而第③级实际永远取不到数据 —— 这是最容易误判的一种情况。
  if (!doc.containsKey("current") && !doc.containsKey("daily")) {
    wTestMsg   = "能连通，但返回的不是天气数据（检查地址路径）";
    wTestState = 3;
    logf("[天气] 测试失败：响应里没有 current/daily 字段\n");
    return;
  }
  wTestTemp  = doc["current"]["temperature_2m"] | 0.0f;
  wTestMsg   = "接口可用";
  wTestState = 2;
  logf("[天气] 测试成功（%lu ms，返回温度 %.1f°C）\n", wTestMs, (float)wTestTemp);
}

// 抓取执行体：读配置 -> 重置状态 -> 顺序跑完 4 步抓取（同步 HTTP，在 weatherTaskLoop 任务内阻塞）
static void weatherRunOnce() {
  // 全程持有网络互斥锁：抓取过程（DNS+TCP+TLS+HTTP）与主 loop 的 NTP UDP 串行，
  // 避免两个上下文并发访问 LwIP 协议栈（另一个崩溃隐患）。
  NetLockGuard netlock;

  // 「测试联通」优先：这是用户正盯着界面等结果的前台操作，插到抓取流水线之前。
  // 清标志要在读走 wTestUrl 之后（weatherRunRelayTest 自己会先本地化）。
  if (wTestReq) {
    wTestReq = false;
    weatherRunRelayTest();
    return;
  }

  // 本次运行类型：仅定位（启动求时区 / 手动同步）还是完整链路（含天气）
  int locatePhase = wLocatePhase;
  wLocatePhase = 0;
  bool locateOnly = (locatePhase != 0);

  String json;
  loadWeatherConfigRaw(json);

  // 时区模式：manual 时偏移由用户指定，自动定位结果不许覆盖。
  wTzAuto = tzConfigIsAuto();

  // 只重置"每轮必须重算"的控制字段，天气数据与定位结果一律保留到新数据落地：
  //   1) 刷新期间占空比系数不会突变 —— 否则 wWeatherOk=false 会让 weatherGetDutyFactor()
  //      短路返回 100%，weatherInteract 通道的输出会被抬到原始目标值约 5 秒（非晴天可见）。
  //   2) 刷新失败时维持上一次的天气，而不是清空成 "--"：天气没抓到不等于"不缩放"。
  // wMode 仅在非定位阶段重置为默认 auto（命中手动位置时由 weatherResolveStartStage 改成 manual）；
  // 定位阶段是内部临时阶段，不该改变对外报告的位置模式。
  weatherLock();
  wBusy = true;
  wTriedIpwho = false;
  if (!locateOnly) wMode = "auto";
  weatherUnlock();

  int startStage = 1;   // 默认：自动模式（IP 定位）
  if (locateOnly) {
    // 仅定位：不读手动位置配置，直接走 IP 定位（手动位置给不出时区，没有意义）
    logRaw("[天气] 仅定位模式：获取时区后交棒给 NTP…\n");
  } else {
    startStage = weatherResolveStartStage(json);
  }

  weatherLock();
  wStage = startStage;
  weatherUnlock();

  if (startStage == 1) logRaw("[天气] 启动自动获取（iPIP 定位优先，国外回退 ipwho.is）…\n");

  // 顺序跑完状态机直到空闲。每步在任务内同步阻塞（HTTP），主 loop 完全不受影响。
  while (true) {
    int s;
    weatherLock();
    s = wStage;
    weatherUnlock();
    if (s == 0) break;
    // 仅定位：stage 2(地理编码)/4(查天气) 属于天气阶段，交给 NTP 之后的完整链路，
    // 这里停下并交棒 —— 此刻时区已由 stage1/stage3 写好。
    if (locateOnly && (s == 2 || s == 4)) break;
    switch (s) {
      case 1: weatherDoIpipLocate();     break;
      case 2: weatherDoGeocode();        break;
      case 3: weatherDoIpLocate();       break;
      case 4: weatherDoForecast();       break;
      default:
        // 未知阶段值（正常流程不应出现，属兜底）。绝不能直接 return ——
        //   1) wBusy 若留在 true，前端会一直显示「更新中…」，且下次抓取仍撞进本分支，只有重启能恢复；
        //   2) wStage 是 static，不在此复位会一直停在非法值上，本任务永久无法恢复工作。
        // 故与各失败分支同样收尾：清 busy、回空闲阶段。
        logf("[天气] 未知抓取阶段 %d，中止本轮并复位状态\n", s);
        weatherLock();
        wBusy = false;
        wStage = 0;
        weatherUnlock();
        return;
    }
    vTaskDelay(1);   // 让出 CPU，避免长时间独占
  }

  if (locateOnly) {
    // 交棒给 NTP。必须由本任务自己发起，不能留给 loop 判定"wBusy 已转 false"——
    // 本任务末尾才把 wBusy 置 false，而 weatherStart 发出通知后 wBusy 仍是上一轮的 false，
    // loop 会在这段空窗里误判成"定位已完成"。
    // 定位失败也照样交棒（见各失败分支的 wBusy=false, wStage=0），否则 NTP 永远等不到。
    weatherLock();
    wBusy = false;
    wStage = 0;
    weatherUnlock();
    logRaw("[天气] 定位阶段结束，开始同步网络时间…\n");
    ntpStartSync(locatePhase == 2);   // phase 2 = 前端「同步网络时间」触发，按手动上报结果
    wArmAfterNtp = true;              // NTP 已发起，结束后由 weatherTick 跑完整链路取天气
  }
}

// 后台抓取任务：阻塞等待触发通知，收到后同步执行一次完整抓取。
// 无请求时任务处于 blocked 态（0% CPU），idle 可正常喂狗，不会触发 task_wdt。
static void weatherTaskLoop(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // 阻塞等待；pdTRUE 清零累积通知，多次请求合并为一次
    weatherRunOnce();
  }
}

// 请求一次天气抓取（非阻塞，立即返回）：首次调用时惰性创建后台任务，随后仅发送触发通知。
// 保留非阻塞入口，避免在 WiFi 事件回调 / Web 回调里直接阻塞做 HTTP。
void weatherStart() {
  if (weatherMutex == NULL) {
    weatherMutex = xSemaphoreCreateMutex();
    if (weatherMutex == NULL) return;   // 极端情况：互斥锁创建失败则放弃
  }
  if (weatherTaskHandle == NULL) {
    // 栈 10KB（TLS+JSON+String 需要）；prio=1 与 loopTask 同级；pin CPU1 与主 loop 同核，
    // 让 CPU0 的 IDLE0 完全空闲，彻底避免饿死空闲任务触发 task_wdt（上次崩溃根因）。
    BaseType_t rc = xTaskCreatePinnedToCore(weatherTaskLoop, "weather", 10000, NULL, 1, &weatherTaskHandle, 1);
    if (rc != pdPASS) {
      logf("[天气] 任务创建失败(%d)，回退主循环同步抓取\n", (int)rc);
      weatherRunOnce();   // 极端兜底：任务创建失败时同步抓取（阻塞当前上下文，但保证功能）
      return;
    }
  }
  xTaskNotifyGive(weatherTaskHandle);
}

// 启动/手动同步的统一入口：按「定位(求时区) → NTP(拿 UTC) → 天气」编排。
// 必须这个顺序 —— 时区先到手，NTP 一落地显示的就是正确本地钟点，不会先显示 UTC 再跳变。
// 时区为手动模式时定位给不出新信息，跳过定位直接对时。
// ntpManual=true 表示用户点的是「同步网络时间」，NTP 结果按手动上报（前端弹提示）。
void weatherArmStartup(bool ntpManual) {
  if (isAPMode || !tzConfigIsAuto()) {
    // 无外网 / 时区由用户指定：无定位需求，直接对时
    logRaw("[天气] 无需定位（AP 模式或手动时区），直接同步网络时间…\n");
    ntpStartSync(ntpManual);
    wArmAfterNtp = true;   // 紧接 NTP 发起之后置位（此刻 ntpIsBusy() 已为 true）
    return;
  }
  // 仅定位模式：跑完 stage1/stage3 由任务自己交棒给 NTP（phase 1=自动，2=手动）。
  // 这里不能置 wArmAfterNtp —— NTP 尚未发起，weatherTick 会误判成"已就绪"而抢跑天气。
  wLocatePhase = ntpManual ? 2 : 1;
  logRaw("[天气] 启动编排：先定位取时区，再同步网络时间…\n");
  weatherStart();
}

// loop 调用：启动编排的最后一棒（NTP 结束 → 抓天气）+ 整点定时刷新。
// 抓取体在后台任务里执行，不阻塞主循环。
void weatherTick() {
  if (WiFi.status() != WL_CONNECTED) return;   // 网络未就绪则等待

  // NTP 结束后跑完整链路（定位 + 天气）。启动与手动同步都汇到这里。
  if (wArmAfterNtp && !ntpIsBusy()) {
    wArmAfterNtp = false;
    logRaw("[天气] 时间已就绪，开始抓取天气…\n");
    weatherStart();
  }

  if (lastSyncEpoch == 0) return;              // 时间尚未同步，跳过整点刷新（避免开机 00:00:00 误触发）

  // 每小时整点定时刷新（:00）：到点自动重抓，每个整点槽只触发一次。
  if (currentTime.minute == 0) {
    int slot = currentTime.hour;
    if (slot != lastWeatherAutoSlot) {
      lastWeatherAutoSlot = slot;
      logRaw("[天气] 定时刷新（整点）…\n");
      weatherStart();
    }
  }
}

// 当日太阳时刻快照（阳光交互取用）：四个值均为"自当地 00:00 起的分钟数"，-1 = 无数据/当日无此事件。
// 持 weatherMutex 读取，避免与后台抓取任务的写入并发（锁内只读 4 个 int，微秒级）。
void weatherGetSunTimes(int& sunriseMin, int& sunsetMin, int& dawnMin, int& duskMin) {
  weatherLock();
  sunriseMin = wSunriseMin;
  sunsetMin  = wSunsetMin;
  dawnMin    = wDawnMin;
  duskMin    = wDuskMin;
  weatherUnlock();
}

// 组装天气状态 JSON（前端 /weatherstatus 读取）
void weatherBuildStatus(JsonDocument& doc) {
  // 持锁读取全部对外状态，避免与后台任务的瞬间写并发（锁内仅内存读取 + JSON 填充，微秒级）
  weatherLock();
  doc["mode"]      = wMode;
  doc["busy"]      = wBusy;
  doc["located"]   = wLocated;
  doc["source"]    = wSource;
  doc["name"]      = wCity;      // 显示城市名（自动=iPIP/ipwho.is 城市，手动=所选城市；前端优先显示）
  doc["ip"]        = wIp;        // 定位 IP（自动定位返回，手动为空）
  doc["city"]      = wCity;      // 用于显示的城市名
  doc["updated"]   = wUpdated;   // 天气最后更新时刻 "HH:MM:SS"（空=暂无有效数据）
  doc["proCode"]   = wProCode;
  doc["cityCode"]  = wCityCode;
  doc["regionCode"] = wRegionCode;
  doc["lat"]       = wLat;
  doc["lon"]       = wLon;
  doc["weatherOk"] = wWeatherOk;
  doc["code"]      = wCode;
  doc["text"]      = wmoText(wCode);   // 前端直接显示，不再维护码表
  doc["icon"]      = wmoIcon(wCode);
  doc["temperature"] = wTemp;
  doc["feels"]     = wFeels;
  doc["humidity"]  = wHumidity;
  doc["wind"]      = wWind;
  doc["tmax"]      = wTmax;
  doc["tmin"]      = wTmin;
  // 当日太阳时刻（"HH:MM"；空串=暂无有效数据/当日无此事件，前端回落成占位符）
  doc["sunrise"]   = weatherFmtHm(wSunriseMin);
  doc["sunset"]    = weatherFmtHm(wSunsetMin);
  doc["dawn"]      = weatherFmtHm(wDawnMin);
  doc["dusk"]      = weatherFmtHm(wDuskMin);

  weatherUnlock();
}

// 是否已获得过真实天气数据。false 时 weatherGetDutyFactor() 返回的 100% 只是"尚未抓到"的占位值
// （不代表"晴"）—— 天气系数过渡不能把它当作真实档位，否则首次抓到天气时会从 100% 慢慢爬过去
// （见 pwm_manager.ino 天气系数过渡的说明）。
bool weatherHasData() {
  weatherLock();
  bool ok = wWeatherOk;
  weatherUnlock();
  return ok;
}

// 当前天气对应的占空比系数（百分比）：晴100% 大部晴朗90% 局部多云80% 阴70% 其他已知现象60%。
// 开机至今从未抓到（wWeatherOk=false）或码未知（不在 WMO 28 码内）时返回 100，等效于不缩放。
// 注意：抓取失败不会把 wWeatherOk 清回 false（见 weatherRunOnce 的重置说明），
// 所以这里的 100 只代表"还没有任何天气数据"，不代表"这次没抓到"——后者沿用上一次的系数。
int weatherGetDutyFactor() {
  weatherLock();
  bool ok = wWeatherOk;
  int code = wCode;
  weatherUnlock();
  int factor;
  if (!ok) factor = 100;   // 天气没抓到 → 原始值
  else {
    switch (code) {
      case 0: factor = 100; break;   // 晴
      case 1: factor = 90;  break;   // 大部晴朗
      case 2: factor = 80;  break;   // 局部多云
      case 3: factor = 70;  break;   // 阴
      // 其余已知现象一律 60%
      case 45: case 48:
      case 51: case 53: case 55: case 56: case 57:
      case 61: case 63: case 65: case 66: case 67:
      case 71: case 73: case 75: case 77:
      case 80: case 81: case 82: case 85: case 86:
      case 95: case 96: case 99:
        factor = 60; break;
      default: factor = 100; break;   // 未知码 → 原始值（不缩放）
    }
  }
  // 仅当天气状态变化时打印一次，避免每 loop 刷屏
  static int lastCode = -999;
  static bool lastOk = false;
  if (code != lastCode || ok != lastOk) {
    lastCode = code;
    lastOk = ok;
    logf("[天气系数] wWeatherOk=%d wCode=%d -> factor=%d%%\n", ok ? 1 : 0, code, factor);
  }
  return factor;
}