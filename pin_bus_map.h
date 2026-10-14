// ===================== 芯片禁用引脚表（全局唯一，仅此一处维护）=====================
// 不同芯片的 SPI Flash / PSRAM 等专用总线引脚不同，这些引脚被物理占用，
// 用作普通 IO 输出会立即崩溃，必须全局拒绝。
// 所有模块（PWM / 序列器 / 比较器 / 计数器 / 手动覆盖 / 锁存 / 配置导入）
// 统一经 isIoFlashPin() 引用此表；换芯片或修正禁用范围，只改下面这张表即可。
// 数据源：各芯片数据手册。表中未列出的型号不限制（返回 false）。
#ifndef SMARTGLASS_PIN_BUS_MAP_H
#define SMARTGLASS_PIN_BUS_MAP_H

#include <Arduino.h>

// 生效芯片型号（config_store.ino 实现，config.h 声明）
const char* getEffectiveChipModel();

// 一段连续禁用区间 [lo, hi]；lo < 0 表示该段无效
struct PinBusRange { int lo; int hi; };

// 一行 = 一个芯片型号 + 最多 2 段禁用区间
struct ChipPinBusRow {
  const char* model;
  PinBusRange ranges[2];
};

static const ChipPinBusRow CHIP_PIN_BUS[] = {
  { "ESP32",     { { 6, 11 }, { 16, 17 } } },   // SPI Flash 6~11 + PSRAM 16~17
  { "ESP32-H4",  { { 6, 12 }, { -1, -1 } } },   // SPI Flash/PSRAM 6~12
  { "ESP32-C2",  { { 11, 17 }, { -1, -1 } } },  // 11=VDD_SPI，12~17=SPI Flash
  { "ESP32-C3",  { { 11, 17 }, { -1, -1 } } },  // 11=VDD_SPI，12~17=SPI Flash
  { "ESP32-C5",  { { 16, 22 }, { -1, -1 } } },  // SPI Flash/PSRAM 16~22
  { "ESP32-C6",  { { 24, 30 }, { -1, -1 } } },  // SPI Flash/PSRAM 24~30
  { "ESP32-C61", { { 14, 17 }, { 19, 21 } } },  // SPI Flash/PSRAM 14~17 + 19~21
  { "ESP32-H2",  { { 15, 21 }, { -1, -1 } } },  // SPI Flash/PSRAM 15~21
  { "ESP32-S2",  { { 26, 32 }, { -1, -1 } } },  // SPI Flash/PSRAM 26~32
  { "ESP32-S3",  { { 26, 32 }, { 33, 37 } } },  // SPI Flash 26~32 + OPI Flash/PSRAM 33~37
  { "ESP32-P4",  { { -1, -1 }, { -1, -1 } } },  // 无内置 Flash，无固定总线引脚，不限制
  { "ESP32-S31", { { -1, -1 }, { -1, -1 } } },  // 外部 Flash，无固定总线引脚，不限制
};
static const int CHIP_PIN_BUS_COUNT = sizeof(CHIP_PIN_BUS) / sizeof(CHIP_PIN_BUS[0]);

static inline bool isIoFlashPin(int pin) {
  const char* model = getEffectiveChipModel();
  for (int i = 0; i < CHIP_PIN_BUS_COUNT; i++) {
    if (strcmp(model, CHIP_PIN_BUS[i].model) != 0) continue;
    for (int r = 0; r < 2; r++) {
      int lo = CHIP_PIN_BUS[i].ranges[r].lo;
      int hi = CHIP_PIN_BUS[i].ranges[r].hi;
      if (lo < 0) continue;
      if (pin >= lo && pin <= hi) return true;
    }
    return false;   // 型号命中但引脚不在禁用范围
  }
  return false;     // 型号未命中：不限制
}

#endif