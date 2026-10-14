#include "config.h"

// ===================== 日志环形缓冲区 =====================
// 统一日志出口 logf()：每条日志同时写串口（实时调试）与环形缓冲区（网页回看）。
// 环形缓冲区保留最近 LOG_MAX_ENTRIES 条，超出后顶掉最旧记录。
// 每条日志最长 LOG_MAX_LEN 字节（含时间戳前缀），超长按固定块切分存储，
// 避免截断丢内容。静态分配避免动态内存碎片。
// 每条日志在写入缓冲区时统一前缀 "[HH:MM:SS] "（来自 currentTime，随 NTP 校准）。

#define LOG_MAX_ENTRIES 200
#define LOG_MAX_LEN     160   // 单条日志（含时间戳）最大字节，超出切分

static char   logBuf[LOG_MAX_ENTRIES][LOG_MAX_LEN];  // 静态环形槽位（约 32KB）
static uint32_t logSeqNext  = 1;    // 全局递增序号（写入一块 +1，作唯一游标；从 1 起，避免首条 seq=0 被前端游标跳过）
static uint32_t logCount    = 0;    // 当前缓冲区内有效块数（0..LOG_MAX_ENTRIES）
static uint32_t logHead     = 0;    // 最旧一块在环形数组中的下标
static SemaphoreHandle_t logMutex = NULL;  // 日志互斥锁：后台天气任务与主 loop 并发写日志时串行

// 当前时间戳字符串（含前后空格与方括号，如 "[12:03:45] "）
static void logTimestamp(char* buf, size_t cap) {
  snprintf(buf, cap, "[%02d:%02d:%02d] ", currentTime.hour, currentTime.minute, currentTime.second);
}

// 追加一块日志到环形缓冲区（调用方保证 text 已含时间戳）
static void logPushBlock(const char* text) {
  uint32_t idx;
  if (logCount < LOG_MAX_ENTRIES) {
    idx = logCount;                 // 尚未满：顺序填入
    logCount++;
  } else {
    idx = logHead;                  // 已满：顶掉最旧
    logHead = (logHead + 1) % LOG_MAX_ENTRIES;
  }
  size_t n = strlen(text);
  if (n > LOG_MAX_LEN - 1) n = LOG_MAX_LEN - 1;
  memcpy(logBuf[idx], text, n);
  logBuf[idx][n] = '\0';
  logSeqNext++;
}

// 追加一条日志：先拼时间戳，再写串口 + 写环形缓冲区。
// 超长日志按块切分（每块 LOG_MAX_LEN），保证内容不丢失。
// 无锁内层（由 logPush 外壳持锁后调用）
static void logPushInner(const char* text) {
  char ts[12];
  logTimestamp(ts, sizeof(ts));
  size_t textLen = strlen(text);
  size_t tsLen = strlen(ts);

  if (textLen + tsLen <= (size_t)(LOG_MAX_LEN - 1)) {
    // 正常长度：单块存储
    char line[LOG_MAX_LEN];
    memcpy(line, ts, tsLen);
    memcpy(line + tsLen, text, textLen);
    line[tsLen + textLen] = '\0';
    logPushBlock(line);
    Serial.print(ts);
    Serial.print(text);
    return;
  }

  // 超长日志：按块切分。首块带时间戳，后续块带缩进延续。
  const size_t blockDataMax = LOG_MAX_LEN - 1 - tsLen;
  size_t offset = 0;
  bool first = true;
  while (offset < textLen) {
    size_t chunk = textLen - offset;
    if (chunk > blockDataMax) chunk = blockDataMax;
    char line[LOG_MAX_LEN];
    size_t p = 0;
    if (first) {
      memcpy(line, ts, tsLen);
      p += tsLen;
      first = false;
    }
    memcpy(line + p, text + offset, chunk);
    p += chunk;
    line[p] = '\0';
    logPushBlock(line);
    Serial.print(line);   // 串口同样切分输出，保持与缓冲区一致
    offset += chunk;
  }
}

// 持锁外壳：后台天气任务与主 loop 会并发写日志（环形缓冲 + 串口），须互斥串行
static void logPush(const char* text) {
  if (logMutex == NULL) logMutex = xSemaphoreCreateMutex();   // 惰性初始化：首次日志必在 setup 单线程阶段，早于天气任务创建
  if (logMutex) xSemaphoreTake(logMutex, portMAX_DELAY);
  logPushInner(text);
  if (logMutex) xSemaphoreGive(logMutex);
}

// 统一日志出口：格式化 -> logPush（自动加时间戳 + 写串口/缓冲）
void logf(const char* fmt, ...) {
  char line[512];   // 格式化缓冲放大，容纳超长日志后再切分
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  logPush(line);
}

// 写入无格式化纯文本日志（用于无需格式化、可能含 % 的固定串）
void logRaw(const char* text) {
  logPush(text);
}

uint32_t logGetSeq()   { return logSeqNext; }
uint32_t logGetCount() { return logCount; }

// 拉取 seq > fromSeq 的日志块。每块附加其全局 seq（前端据此续传游标）。
// 返回格式：每行 "seq|内容\n"。fromSeq 为 0 时返回缓冲区全部内容。
void logGetSince(uint32_t fromSeq, String& out) {
  if (logMutex) xSemaphoreTake(logMutex, portMAX_DELAY);   // 读环形缓冲须与写互斥（后台任务可能在写）
  out.reserve(LOG_MAX_ENTRIES * (LOG_MAX_LEN + 16));
  uint32_t n = logCount;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t idx = (logHead + i) % LOG_MAX_ENTRIES;
    uint32_t seq = logSeqNext - n + i;
    if (seq > fromSeq) {
      out += String(seq);
      out += '|';
      out += logBuf[idx];
      out += '\n';
    }
  }
  if (logMutex) xSemaphoreGive(logMutex);
}
