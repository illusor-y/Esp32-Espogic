// webpage_html.h - 从 webpage.h 拆分而来
#ifndef ESPOGIC_WEBPAGE_HTML_H
#define ESPOGIC_WEBPAGE_HTML_H

static const char PAGE_HTML_HEAD_A[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, viewport-fit=cover">
    <title>Espogic 控制台</title>
    <style>)HTML";
static const char PAGE_HTML_HEAD_B[] PROGMEM = R"HTML(</style>
</head>
<body>
    <div class="container">
        <header class="header">
            <div class="brand" onclick="openLogs()" title="查看串口日志"><span class="brand-dot"></span>Espogic</div>
            <div style="display:flex; align-items:center; gap:12px;">
                <button class="weather-fab" id="weatherBtn" onclick="openWeather()" title="天气">
                    <span id="weatherIcon">🌡️</span>
                    <span id="weatherTemp">--°</span>
                </button>
                <button class="chip-info" id="chipInfoBtn" onclick="openChipPanel()" title="芯片信息与版本固定">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>
                    <span id="chipInfoText">芯片 —</span>
                </button>
                <button class="wifi-fab" onclick="openWifi()" title="网络设置">
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
                    <span id="wifiInfo">连接中…</span>
                </button>
            </div>
        </header>

        <div class="save-fail" id="saveFailBanner">
            <span class="save-fail-dot"></span>
            <div>
                <div class="save-fail-title">配置未能保存</div>
                <div class="save-fail-desc">以下模块写入 NVS 失败：<span id="saveFailText">—</span>。这些改动重启后会丢失，请检查 NVS 是否已满或损坏（可在「配置备份与恢复」中查看剩余空间）。</div>
            </div>
        </div>

        <section class="clock">
            <div class="time-display"><span id="clock">)HTML";
static const char PAGE_HTML_FOOT_A[] PROGMEM = R"HTML(</span></div>
            <div class="clock-sub" id="clockSub">
                <span id="netMode">连接中…</span>
                <span class="net-dot">·</span>
                <span class="copy-chip" id="ipChip" onclick="copyNetInfo('ipChip')">—</span>
                <span class="net-dot">·</span>
                <span class="copy-chip" id="hostChip" onclick="copyNetInfo('hostChip')">—</span>
            </div>
        </section>

        <div class="two-col">
            <section class="card">
                <div class="section-label">全局配置</div>
                <div class="row">
                    <button class="btn filled" style="flex:1;" onclick="openClockPanel()">时钟面板</button>
                    <button class="btn filled" style="flex:1;" onclick="openCentralPanel()">中心面板</button>
                    <button class="btn filled" style="flex:1;" onclick="openIoPanel()">调度面板</button>
                </div>
                <div class="divider"></div>
                <button class="btn tonal" style="width:100%;" onclick="openBackupModal()">配置备份与恢复</button>
            </section>

            <section class="card">
                <div class="section-label">时间设置</div>
                <div class="time-actions">
                    <button class="btn tonal" onclick="openManualTime()">手动时间</button>
                    <button class="btn filled" id="syncNtpBtn" onclick="syncNTP()">同步网络时间</button>
                </div>
            </section>
        </div>

        <div class="section-tabs">
            <button class="tab active" id="tabPwm" onclick="switchSection('pwm')">PWM 信号</button>
            <button class="tab" id="tabIo" onclick="switchSection('io')">序列器</button>
            <button class="tab" id="tabComparator" onclick="switchSection('comparator')">比较器</button>
            <button class="tab" id="tabCounter" onclick="switchSection('counter')">计数器</button>
        </div>
        <div id="pwmSection">
            <div class="card">
                <div class="row">
                    <span class="row-label">每个通道对应一个 GPIO 引脚。（通道最多 6 条目，最多 8 通道）。</span>
                    <button class="btn filled" onclick="pwmAddOpen()" style="margin-left:auto;">添加通道</button>
                </div>
            </div>
            <div id="channels"></div>
        </div>
        <div id="ioSection" style="display:none;">
            <div class="card">
                <div class="row">
                    <span class="row-label">每个序列对应一个 GPIO 引脚，组内可添加多条条目：定时开关 / 瞬时脉冲 / 持久心跳。</span>
                    <button class="btn filled" onclick="ioGroupAddOpen()" style="margin-left:auto;">添加序列</button>
                </div>
            </div>
            <div id="ioGroups"></div>
        </div>
        <div id="comparatorSection" style="display:none;">
            <div class="card">
                <div class="row">
                    <span class="row-label">左半满足所选门逻辑时，右半 IO 输出行为电平；否则复位至源IO电平。</span>
                    <button class="btn filled" onclick="comparatorGateAddOpen()" style="margin-left:auto;">添加任务</button>
                </div>
            </div>
            <div id="comparatorGates"></div>
        </div>
        <div id="counterSection" style="display:none;">
            <div class="card">
                <div class="row">
                    <span class="row-label">监控引脚电平切变，达到阈值后触发其他引脚输出。</span>
                    <div style="margin-left:auto;display:flex;gap:8px;">
                        <button class="btn tonal" onclick="openCounterPanel()">计量面板</button>
                        <button class="btn filled" onclick="counterTaskAddOpen()">添加任务</button>
                    </div>
                </div>
            </div>
            <div id="counterTasks"></div>
        </div>
    </div>

    <!-- 天气弹窗 -->
    <div class="modal-mask" id="weatherModal" onclick="closeWeather()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>天气</h3>
                <button class="icon-btn" onclick="closeWeather()">✕</button>
            </div>
            <div class="weather-mode-row">
                <span class="row-label">网络定位</span>
                <input type="checkbox" class="switch" id="weatherModeSwitch" onchange="weatherModeToggle(this)">
                <span class="row-label">手动定位</span>
                <!-- 「接口转发」贴在这一行最右侧（靠 CSS margin-left:auto）：
                     它属于传输层兜底，与怎么定位无关，两种模式下都该在同一位置够得着 -->
                <button class="btn tonal" onclick="openRelay()">接口转发</button>
            </div>
            <!-- 这行只剩「手动更新」：手动定位下整行隐藏（见 weatherRenderMode） -->
            <div class="weather-loc" id="weatherAutoRow">
                <button class="btn filled" id="weatherRefreshBtn" onclick="weatherReAuto()">手动更新</button>
            </div>
            <div class="weather-selects" id="weatherSelects" style="display:none;">
                <div class="w-select" data-ws="prov">
                    <button type="button" class="w-select-btn"><span class="w-select-val">选择省份</span><span class="w-select-arrow">▾</span></button>
                    <div class="w-select-panel"></div>
                </div>
                <div class="w-select" data-ws="city">
                    <button type="button" class="w-select-btn"><span class="w-select-val">选择城市</span><span class="w-select-arrow">▾</span></button>
                    <div class="w-select-panel"></div>
                </div>
                <div class="w-select" data-ws="dist">
                    <button type="button" class="w-select-btn"><span class="w-select-val">选择区县</span><span class="w-select-arrow">▾</span></button>
                    <div class="w-select-panel"></div>
                </div>
            </div>
            <div class="weather-loc-text" id="weatherLocText">未设置</div>
            <div class="weather-interact-entry">
                <span class="weather-loc-debug" id="weatherLocDebug" style="display:none;">
                    <span id="weatherLocSource"></span>
                    <span id="weatherUpdatedAt"></span>
                </span>
                <button class="btn filled wi-btn" onclick="openWeatherInteract()">气象 / 阳光</button>
            </div>
            <div class="weather-now">
                <span class="weather-now-icon" id="weatherNowIcon">🌡️</span>
                <div class="weather-now-main">
                    <div class="weather-now-temp" id="weatherNowTemp">--°</div>
                    <div class="weather-now-text" id="weatherNowText">--</div>
                </div>
            </div>
            <div class="weather-meta">
                <div class="weather-meta-item"><span>体感</span><b id="weatherFeels">--</b></div>
                <div class="weather-meta-item"><span>湿度</span><b id="weatherHumidity">--</b></div>
                <div class="weather-meta-item"><span>风速</span><b id="weatherWind">--</b></div>
                <div class="weather-meta-item"><span>气温</span><b id="weatherToday">--/--</b></div>
            </div>
            <div class="weather-meta">
                <div class="weather-meta-item"><span>日出</span><b id="weatherSunrise">--:--</b></div>
                <div class="weather-meta-item"><span>日落</span><b id="weatherSunset">--:--</b></div>
                <div class="weather-meta-item"><span>晨光</span><b id="weatherDawn">--:--</b></div>
                <div class="weather-meta-item"><span>昏光</span><b id="weatherDusk">--:--</b></div>
            </div>
            <div class="modal-actions">
                <button class="btn filled" id="weatherSaveBtn" onclick="weatherManualSave()" style="display:none;">保存</button>
                <button class="btn tonal" onclick="closeWeather()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 大气环境二级弹窗：顶部公共设置（横跨两栏，避免左右不等高）+ 左右两栏通道列表 -->
    <div class="modal-mask" id="weatherInteractModal" onclick="closeWeatherInteract()">
        <div class="modal modal-wide" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>大气环境</h3>
                <button class="icon-btn" onclick="closeWeatherInteract()">✕</button>
            </div>

            <!-- 公共设置：跨两栏的参数都放这里，两栏本身只留对称的通道列表 -->
            <div class="wi-topbar">
                <span class="wi-topbar-label">过渡时长</span>
                <input type="number" id="wxFadeMin" min="0" max="50" step="1" value="25" onchange="wxFadeSave()">
                <span class="wi-topbar-unit">分钟</span>
                <span class="wi-topbar-now" id="wiNow">—</span>
            </div>
            <div class="wi-topbar-hint">天气现象变化时，占空比系数在该时长内匀速过渡（0 = 立即切换）。</div>

            <div class="wi-cols">
                <div class="wi-col">
                    <div class="wi-col-title">气象交互</div>
                    <div class="wi-col-sub">按天气现象缩放占空比</div>
                    <div class="wi-list" id="weatherInteractList"></div>
                </div>
                <div class="wi-col">
                    <div class="wi-col-title">阳光交互</div>
                    <div class="wi-col-sub">按太阳时刻生成曲线</div>
                    <div class="wi-list" id="sunInteractList"></div>
                </div>
            </div>

            <div class="modal-actions">
                <button class="btn tonal" onclick="closeWeatherInteract()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 阳光交互设置三级弹窗：基准（日出→日落 / 晨光→昏光）+ 最高原始占空比 -->
    <div class="modal-mask" id="sunSetupModal" onclick="closeSunSetup()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>阳光交互</h3>
                <button class="icon-btn" onclick="closeSunSetup()">✕</button>
            </div>
            <div class="modal-desc" id="sunSetupPin">GPIO —</div>
            <div class="sun-warn">⚠ 若无气象数据（太阳时刻不可用），该通道将按你的条目设置执行。</div>
            <div class="section-label">基准</div>
            <div class="sun-mode-row">
                <button type="button" class="btn sun-mode-btn" id="sunModeBtn0" onclick="sunSetMode(0)">基于日出 → 日落</button>
                <button type="button" class="btn sun-mode-btn" id="sunModeBtn1" onclick="sunSetMode(1)">基于晨光 → 昏光</button>
            </div>
            <div class="sun-times" id="sunSetupTimes">—</div>
            <div class="section-label">最高原始占空比（%）</div>
            <div class="sun-peak-row">
                <input type="number" id="sunPeakInput" min="0" max="100" value="100">
                <span class="sun-peak-hint">曲线中点达到该值（天气现象之前的原始占空比）</span>
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeSunSetup()">取消</button>
                <button class="btn filled" onclick="sunSetupConfirm()">开启并保存</button>
            </div>
        </div>
    </div>

    <!-- 变轨中说明弹窗（通道卡片「变轨中」标签点开）-->
    <div class="modal-mask" id="rampModal" onclick="closeRampHelp()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>变轨中</h3>
                <button class="icon-btn" onclick="closeRampHelp()">✕</button>
            </div>
            <div class="modal-desc">
                天气现象变化（如晴转多云）时，天气系数是跳变的，直接切换会让亮度一跳一跳。<br>
                所以这里让系数在过渡时长内平滑地走到新档位 —— 亮度因此在缓慢变化，属正常现象，不是故障。<br>
                过渡时长由「大气环境 → 过渡时长」统一控制（当前 <b id="rampDurMin">--</b> 分钟），设为 0 则立即跳变。
            </div>
            <div class="modal-desc">
                注：<b>修改时间（手动拨钟 / 改时区）不会产生过渡</b>，占空比会直接变成新时刻对应的值。<br>
                标签上的倒计时是过渡自己的时间轴，严格正计时：<b>拨钟 / 改时区不会让它变快或变慢</b>，
                它只随设备运行时长递减，走到 00:00 即过渡结束。<br>
                「立即完成」是把这次天气过渡提前结束：天气系数是所有通道共用的一份值，所以一按就全部通道一起到位。
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeRampHelp()">关闭</button>
                <button class="btn filled" onclick="rampComplete()">立即完成</button>
            </div>
        </div>
    </div>

    <!-- 标签说明弹窗（通道卡片上的状态标签 / 跨日条目标签点开）-->
    <div class="modal-mask" id="statModal" onclick="closeStatHelp()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>标签说明</h3>
                <button class="icon-btn" onclick="closeStatHelp()">✕</button>
            </div>
            <div class="modal-desc">
                通道卡片上的标签分三类：<b>状态标签</b>（谁在驱动这一路的输出，看颜色也能分辨）、
                <b>提示标签</b>（时间与过渡相关）、<b>条目标签</b>（单个条目自身的情况）。
            </div>

            <div class="tag-help-group">通道状态标签</div>
            <div class="tag-help-row">
                <span class="badge-pill accent">运行条目X</span>
                <span class="tag-help-text">条目正在按时段运行，输出由该条目的起止占空比插值决定。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-pill warn">阳光交互</span>
                <span class="tag-help-text">阳光正在接管这一路：占空比由日出→日落（或晨光→昏光）曲线给出，
                    天气系数照旧叠乘。此时条目计算被暂停，条目上会显示「阳光覆盖」。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-pill accent">无阳光数据·运行条目X</span>
                <span class="tag-help-text">阳光交互开着，但设备此刻没有当天太阳时刻数据
                    （未定位成功 / 尚未抓到天气 / 当日无该事件如极昼极夜），已按妥协处理回退到你的条目设置。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-pill warn">无条目运行</span>
                <span class="tag-help-text">通道开着、且挂着阳光配置，但此刻没有任何东西在驱动输出
                    （阳光在等太阳时刻数据，或没有启用中的条目）。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-pill danger">无条目运行</span>
                <span class="tag-help-text">通道开着，但没有启用中的条目，输出为 0。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-pill warn">已关闭</span>
                <span class="tag-help-text">通道被关闭，但这一路还挂着阳光交互配置（重新打开通道后阳光依然生效）。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-pill danger">已关闭</span>
                <span class="tag-help-text">通道被关闭，且未配置阳光交互。</span>
            </div>

            <div class="tag-help-group">通道上的提示标签</div>
            <div class="tag-help-row">
                <span class="badge-overnight">跨日条目</span>
                <span class="tag-help-text">该通道里有条目的时间段跨过午夜，例如 22:00→06:00（结束时间属于第二天）；
                    首尾相同如 00:00→00:00 表示<b>全天 24 小时</b>生效。它是"开始→午夜 + 午夜→结束"的连续窗口，
                    占空比按跨日总时长线性插值，容易与当天凌晨的其它条目重叠（重叠会标「冲突」）。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-ramp">变轨中 12:34</span>
                <span class="tag-help-text">天气现象变化时，天气系数正在按「大气环境 → 过渡时长」平滑走到新档位，
                    括号内是剩余时间（严格正计时，拨钟 / 改时区都不会影响它）。点它可查看说明并立即完成。</span>
            </div>

            <div class="tag-help-group">条目上的标签</div>
            <div class="tag-help-row">
                <span class="task-state task-state-working">工作中</span>
                <span class="tag-help-text">该条目当前正在生效（同一通道同时只会有一条工作中）。</span>
            </div>
            <div class="tag-help-row">
                <span class="task-state task-state-idle">待命中</span>
                <span class="tag-help-text">条目已启用，但当前不在它的时间段内。</span>
            </div>
            <div class="tag-help-row">
                <span class="task-state task-state-disabled">未启用</span>
                <span class="tag-help-text">该条目的启用开关是关的，不参与调度。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-danger">冲突</span>
                <span class="tag-help-text">同一通道内有两条以上启用条目的时间段重叠，请调整时间。</span>
            </div>
            <div class="tag-help-row">
                <span class="badge-sunoff">阳光覆盖</span>
                <span class="tag-help-text">阳光正在接管该通道，此条目的时间与占空比暂时不生效
                    （仅置灰显示，配置不会被改动）。</span>
            </div>

            <div class="modal-actions">
                <button class="btn filled" onclick="closeStatHelp()">知道了</button>
            </div>
        </div>
    </div>

    <!-- 接口转发二级弹窗（天气第三级兜底；地址由用户自填，留空即关闭）-->
    <div class="modal-mask" id="relayModal" onclick="closeRelay()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>接口转发</h3>
                <button class="icon-btn" onclick="closeRelay()">✕</button>
            </div>
            <div class="modal-desc">天气直连取不到时，才用这个地址兜底（最后一级）。填你自己的中转地址，例如 Cloudflare Worker 的 <code>https://xxx.workers.dev/api/weather</code>；<b>留空 = 关闭该级</b>。<br>若你的中转开了密钥校验，把 <code>?key=xxx</code> 一起带上即可。</div>
            <input type="text" id="relayUrl" maxlength="192" placeholder="https://xxx.workers.dev/api/weather" autocomplete="off" spellcheck="false" style="width:100%;margin-bottom:12px;">
            <div class="modal-desc" id="relayTestResult" style="min-height:18px;margin-bottom:6px;"></div>
            <div class="modal-actions">
                <button class="btn tonal" id="relayTestBtn" onclick="relayTest()">测试联通</button>
                <button class="btn filled" onclick="relaySave()">保存</button>
                <button class="btn tonal" onclick="closeRelay()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 手动时间二级弹窗（时区 + 手动拨钟）-->
    <div class="modal-mask" id="manualTimeModal" onclick="closeManualTime()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>手动时间</h3>
                <button class="icon-btn" onclick="closeManualTime()">✕</button>
            </div>

            <div class="section-label">时区</div>
            <div class="tz-mode-list">
                <div class="tz-mode-row">
                    <span class="row-label">自动</span>
                    <input type="checkbox" class="switch" id="tzAutoSwitch" onchange="tzModeToggle(this)">
                </div>
                <div class="tz-mode-row">
                    <span class="row-label">手动</span>
                    <input type="checkbox" class="switch" id="tzManualSwitch" onchange="tzModeToggle(this)">
                </div>
            </div>
            <div class="tz-now" id="tzNowText">当前：--</div>
            <div class="tz-ap-hint" id="tzApHint" style="display:none;">AP 模式无网络时间，时区设置不适用；请用下方「手动拨钟」直接设定本地时间</div>
            <div id="tzManualRow" style="display:none;">
                <div class="tz-pick">
                    <span class="tz-sep">UTC</span>
                    <div class="w-select" data-ws="tzHour">
                        <button type="button" class="w-select-btn"><span class="w-select-val">+8</span><span class="w-select-arrow">▾</span></button>
                        <div class="w-select-panel"></div>
                    </div>
                    <span class="tz-sep">时</span>
                    <div class="w-select" data-ws="tzMin">
                        <button type="button" class="w-select-btn"><span class="w-select-val">00</span><span class="w-select-arrow">▾</span></button>
                        <div class="w-select-panel"></div>
                    </div>
                    <span class="tz-sep">分</span>
                </div>
                <button class="btn filled" style="width:100%;" onclick="tzSaveManual()">应用</button>
            </div>

            <div class="divider"></div>

            <div class="section-label">手动拨钟</div>
            <div class="modal-desc">直接设定时分秒。AP模式下使用它校准时间。</div>
            <div class="time-inputs">
                <input type="number" id="setHour" min="0" max="23" inputmode="numeric" placeholder="00"><span class="sep">:</span>
                <input type="number" id="setMinute" min="0" max="59" inputmode="numeric" placeholder="00"><span class="sep">:</span>
                <input type="number" id="setSecond" min="0" max="59" inputmode="numeric" placeholder="00">
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeManualTime()">关闭</button>
                <button class="btn filled" onclick="setTime()">应用时间</button>
            </div>
        </div>
    </div>

    <!-- 芯片信息与版本固定弹窗 -->
    <div class="modal-mask" id="chipModal" onclick="closeChipPanel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>芯片信息与版本固定</h3>
                <button class="icon-btn" onclick="closeChipPanel()">✕</button>
            </div>
            <div class="modal-desc">自动识别与实际硬件不符时，可手动固定芯片型号。固定后下次重启生效，「清除配置」与「重置系统」均会将其清除。</div>
            <!-- 左右两栏：左=信息卡，右=固定型号列表。窄屏由 .chip-cols 的媒体查询改回上下堆叠 -->
            <div class="chip-cols">
                <div class="chip-info-card">
                    <div class="chip-row"><span>自动识别型号</span><b id="chipRealModel">—</b></div>
                    <div class="chip-row"><span>生效型号</span><b id="chipEffModel">—</b></div>
                    <div class="chip-row"><span>版本号（修订）</span><b id="chipRevision">—</b></div>
                    <div class="chip-row"><span>内部存储</span><b id="chipStorage">—</b></div>
                    <div class="chip-row"><span>核心温度</span><b id="chipTemp">—</b></div>
                    <div class="chip-row"><span>堆内存已用</span><b id="chipHeap">—</b></div>
                    <div class="chip-row"><span>PSRAM 已用</span><b id="chipPsram">—</b></div>
                    <div class="chip-row"><span>NVS 剩余</span><b id="chipNvs">—</b></div>
                    <div class="chip-row"><span>主循环负载</span><b id="chipLoopRate">—</b></div>
                    <div class="chip-row"><span>主循环卡顿</span><b id="chipLoopStall">—</b></div>
                    <div class="chip-row"><span>状态</span><b id="chipForcedTag">自动识别</b></div>
                </div>
                <div class="chip-model-pane">
                    <div class="modal-desc" style="margin:0 0 8px;">选择要固定的芯片型号（当前生效型号带「当前」标记）：</div>
                    <div class="chip-model-list" id="chipModelList"></div>
                </div>
            </div>
            <div class="modal-actions">
                <button class="btn tonal" id="chipAutoBtn" onclick="chipRestoreAuto()">恢复自动识别</button>
                <button class="btn tonal" onclick="closeChipPanel()">取消</button>
                <button class="btn filled" id="chipFixBtn" onclick="chipFix()" disabled>固定到此版本</button>
            </div>
        </div>
    </div>

    <!-- WiFi 弹窗 -->
    <div class="modal-mask" id="wifiModal" onclick="closeWifi()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>网络设置</h3>
                <button class="icon-btn" onclick="closeWifi()">✕</button>
            </div>
            <div class="modal-desc">选择要连接的 WiFi，保存后设备将重启。</div>
            <div class="row" style="margin-bottom:10px;">
                <button class="btn tonal" id="scanBtn" onclick="scanWifi()">扫描网络</button>
                <button class="btn tonal" onclick="refreshWifiInfo()">刷新状态</button>
            </div>
            <div class="wifi-list" id="wifiList"></div>
            <input type="password" id="wifiPassword" placeholder="WiFi 密码（开放网络可留空）" style="width:100%;">
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeWifi()">取消</button>
                <button class="btn filled" onclick="saveWifi()">保存并连接</button>
            </div>
        </div>
    </div>

    <!-- GPIO 引脚选择弹窗 -->
    <div class="modal-mask" id="pinModal" onclick="closePinModal()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="pinModalTitle">设置 GPIO 引脚</h3>
                <button class="icon-btn" onclick="closePinModal()">✕</button>
            </div>
            <div class="modal-desc">点击选择一个可用引脚，保存后立即生效并写入 NVS。</div>
            <div class="pin-warning" id="pinWarning"></div>
            <div class="pin-grid" id="pinGrid"></div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="pwmDevModeSwitch" onchange="pwmDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="closePinModal()">取消</button>
                <button class="btn filled" id="pwmSaveBtn" onclick="savePins()">保存引脚</button>
            </div>
        </div>
    </div>

    <!-- PWM 引脚开发模式确认弹窗 -->
    <div class="modal-mask" id="pwmDevModeModal" onclick="pwmDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="pwmDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许重复选中引脚</span>
                <input type="checkbox" class="switch" id="pwmAllowDupSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="pwmDevModeCancel()">取消</button>
                <button class="btn filled" onclick="pwmDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- PWM 添加通道弹窗 -->
    <div class="modal-mask" id="pwmAddModal" onclick="pwmAddClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>添加 PWM 通道</h3>
                <button class="icon-btn" onclick="pwmAddClose()">✕</button>
            </div>
            <div class="modal-desc">点击选择一个可用引脚，添加后立即生效并写入 NVS。</div>
            <div class="pin-warning" id="pwmAddPinWarning"></div>
            <div class="pin-grid" id="pwmAddPinGrid"></div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="pwmAddDevModeSwitch" onchange="pwmAddDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="pwmAddClose()">取消</button>
                <button class="btn filled" id="pwmAddSaveBtn" onclick="pwmAddConfirm()">添加通道</button>
            </div>
        </div>
    </div>

    <!-- PWM 添加通道开发模式确认弹窗 -->
    <div class="modal-mask" id="pwmAddDevModeModal" onclick="pwmAddDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="pwmAddDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许重复选中引脚</span>
                <input type="checkbox" class="switch" id="pwmAddAllowDupSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="pwmAddDevModeCancel()">取消</button>
                <button class="btn filled" onclick="pwmAddDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- PWM 删除确认弹窗 -->
    <div class="modal-mask" id="pwmDelModal" onclick="pwmDelCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>确认删除</h3>
                <button class="icon-btn" onclick="pwmDelCancel()">✕</button>
            </div>
            <div class="modal-desc" id="pwmDelDesc"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="pwmDelCancel()">取消</button>
                <button class="btn danger" onclick="pwmDelConfirm()">确认删除</button>
            </div>
        </div>
    </div>

    <!-- PWM 删除条目确认弹窗 -->
    <div class="modal-mask" id="pwmTaskDelModal" onclick="pwmTaskDelCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>确认删除条目</h3>
                <button class="icon-btn" onclick="pwmTaskDelCancel()">✕</button>
            </div>
            <div class="modal-desc" id="pwmTaskDelDesc"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="pwmTaskDelCancel()">取消</button>
                <button class="btn danger" onclick="pwmTaskDelConfirm()">确认删除</button>
            </div>
        </div>
    </div>

    <!-- 时钟面板弹窗 -->
    <div class="modal-mask" id="clockPanelModal" onclick="closeClockPanel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>时钟面板</h3>
                <button class="icon-btn" onclick="closeClockPanel()">✕</button>
            </div>
            <div class="modal-desc">管理全局时钟（对应 4 个硬件 timer）。被条目引用的时钟不可删除；频率全局唯一。</div>
            <div class="clk-list" id="clockList"></div>
            <div class="modal-actions">
                <button class="btn filled" onclick="clockAddOpen()">添加时钟</button>
                <button class="btn tonal" onclick="closeClockPanel()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 添加时钟弹窗 -->
    <div class="modal-mask" id="clockAddModal" onclick="clockAddClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>添加时钟</h3>
                <button class="icon-btn" onclick="clockAddClose()">✕</button>
            </div>
            <div class="modal-desc">输入频率（1~40000 Hz），频率不可与已有时钟重复。</div>
            <input type="number" id="clockFreqInput" min="1" max="40000" inputmode="numeric" placeholder="频率 Hz" style="width:100%;">
            <div class="modal-actions">
                <button class="btn tonal" onclick="clockAddClose()">取消</button>
                <button class="btn filled" onclick="clockAddConfirm()">添加</button>
            </div>
        </div>
    </div>

    <!-- 修改时钟频率弹窗 -->
    <div class="modal-mask" id="clockEditModal" onclick="clockEditClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>修改频率</h3>
                <button class="icon-btn" onclick="clockEditClose()">✕</button>
            </div>
            <div class="modal-desc">输入新频率（1~40000 Hz），频率不可与已有时钟重复。</div>
            <input type="hidden" id="clockEditSlot">
            <input type="number" id="clockEditFreqInput" min="1" max="40000" inputmode="numeric" placeholder="频率 Hz" style="width:100%;">
            <div class="modal-actions">
                <button class="btn tonal" onclick="clockEditClose()">取消</button>
                <button class="btn filled" onclick="clockEditConfirm()">保存</button>
            </div>
        </div>
    </div>

    <!-- 条目时钟选择弹窗 -->
    <div class="modal-mask" id="taskClockModal" onclick="closeTaskClock()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>选择时钟</h3>
                <button class="icon-btn" onclick="closeTaskClock()">✕</button>
            </div>
            <div class="modal-desc">点击选择一个时钟应用到该条目。</div>
            <div class="clk-list" id="taskClockList"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeTaskClock()">取消</button>
            </div>
        </div>
    </div>

    <!-- 配置备份与恢复弹窗 -->
    <div class="modal-mask" id="backupModal" onclick="closeBackupModal()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>配置备份与恢复</h3>
                <button class="icon-btn" onclick="closeBackupModal()">✕</button>
            </div>
            <div class="modal-desc">导出 / 导入设备全部配置，或查看当前 NVS 中已保存的所有参数。</div>
            <input type="file" id="fileInput" accept=".json" style="display:none;" onchange="importConfig(this)">
            <div class="backup-actions">
                <button class="btn tonal" onclick="exportConfig()">导出配置</button>
                <button class="btn tonal" onclick="document.getElementById('fileInput').click()">导入配置</button>
                <button class="btn tonal" onclick="openNvsModal()">NVS 数据</button>
            </div>
        </div>
    </div>

    <!-- 导出配置确认弹窗 -->
    <div class="modal-mask" id="exportModal" onclick="closeExportModal()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>导出配置</h3>
                <button class="icon-btn" onclick="closeExportModal()">✕</button>
            </div>
            <div class="modal-desc">标准导出仅包含已保存到 NVS 的配置；若使用了开发模式中的引脚，这些引脚不会写入 NVS，请选择包含开发引脚的导出。</div>
            <!-- 两个按钮文案较长，照父弹窗的 .backup-actions 竖排满宽，避免横排换行导致参差 -->
            <div class="backup-actions">
                <button class="btn filled" onclick="doExport(false)">标准配置导出</button>
                <button class="btn tonal" onclick="doExport(true)">包含开发引脚的配置导出</button>
            </div>
            <div class="modal-desc" style="color:var(--warn); margin-top:10px;">如果您复用的引脚包含开发模式中的请选择（包含开发引脚的配置导出），否则不要选择此选项。</div>
        </div>
    </div>

    <!-- NVS 数据弹窗（只读） -->
    <div class="modal-mask" id="nvsModal" onclick="closeNvsModal()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>NVS 数据</h3>
                <div class="nvs-usage">
                    <div class="progress-track"><div class="progress-bar" id="nvsUsageBar"></div></div>
                    <span id="nvsUsageText">剩余空间 --</span>
                </div>
                <button class="icon-btn" onclick="closeNvsModal()">✕</button>
            </div>
            <div class="modal-desc">当前 NVS 中已保存的全部参数。</div>
            <pre class="nvs-box" id="nvsBox">加载中…</pre>
        </div>
    </div>

    <!-- 串口日志弹窗 -->
    <div class="modal-mask log-modal" id="logModal" onclick="closeLogs()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>串口日志</h3>
                <div style="display:flex; align-items:center; gap:8px;">
                    <button class="btn danger" style="padding:7px 14px;font-size:13px;" onclick="confirmFactoryReset()">重置系统</button>
                    <button class="btn tonal" style="padding:7px 14px;font-size:13px;" onclick="confirmResetConfig()">清除配置</button>
                    <button class="icon-btn" onclick="closeLogs()">✕</button>
                </div>
            </div>
            <div class="modal-desc">实时显示设备串口输出（保留最近 200 条）。</div>
            <div class="log-box" id="logBox"></div>
        </div>
    </div>

    <!-- 恢复出厂确认弹窗 -->
    <div class="modal-mask" id="factoryResetModal" onclick="closeFactoryResetConfirm()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>恢复出厂设置</h3>
                <button class="icon-btn" onclick="closeFactoryResetConfirm()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">将清除所有用户配置数据（包括已保存的 WiFi），恢复出厂状态。此操作不可恢复，确认后系统将立刻重启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeFactoryResetConfirm()">取消</button>
                <button class="btn danger" onclick="doFactoryReset()">确认恢复出厂</button>
            </div>
        </div>
    </div>

    <!-- 清除配置确认弹窗 -->
    <div class="modal-mask" id="resetConfigModal" onclick="closeResetConfigConfirm()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>清除配置</h3>
                <button class="icon-btn" onclick="closeResetConfigConfirm()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--warn);">将清除除 WiFi 以外的全部配置，保留已保存的 WiFi 以便下次正常联网。确认后系统将立刻重启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeResetConfigConfirm()">取消</button>
                <button class="btn danger" onclick="doResetConfig()">确认清除</button>
            </div>
        </div>
    </div>

    <!-- 重置进度弹窗（不可手动关闭） -->
    <div class="modal-mask" id="resetProgressModal">
        <div class="modal" style="text-align:center;">
            <div class="modal-head" style="justify-content:center;">
                <h3 id="resetProgressTitle">正在重置…</h3>
            </div>
            <div class="modal-desc" id="resetProgressDesc">请稍候…</div>
            <div class="progress-track"><div class="progress-bar" id="resetProgressBar"></div></div>
            <div class="modal-actions" style="justify-content:center;">
                <button class="btn filled" id="resetProgressRefreshBtn" style="display:none;" onclick="location.reload()">刷新页面</button>
            </div>
        </div>
    </div>

    <div class="toast" id="toast"></div>

    <!-- IO 任务（组）添加/编辑弹窗 -->
    <div class="modal-mask" id="ioGroupModal" onclick="ioGroupClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="ioGroupTitle">添加 IO 任务</h3>
                <button class="icon-btn" onclick="ioGroupClose()">✕</button>
            </div>
            <div class="modal-desc">为任务命名，并选择一个未被 PWM 与其他任务占用的 GPIO 引脚。</div>
            <input type="text" id="ioGroupName" placeholder="任务名（≤16 字符）" maxlength="16" style="width:100%;margin-bottom:12px;">
            <div class="pin-warning" id="ioPinWarning"></div>
            <div class="pin-grid" id="ioPinGrid"></div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="ioDevModeSwitch" onchange="ioDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="ioGroupClose()">取消</button>
                <button class="btn filled" id="ioGroupSaveBtn" onclick="ioGroupSave()">添加</button>
            </div>
        </div>
    </div>

    <!-- 序列器 条目（时间 + 模式）添加/编辑弹窗 -->
    <div class="modal-mask" id="ioEntryModal" onclick="ioEntryClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="ioEntryTitle">添加条目</h3>
                <button class="icon-btn" onclick="ioEntryClose()">✕</button>
            </div>
            <div class="modal-desc" id="ioEntryDesc">定时开关：到点输出并保持。</div>
            <div class="row" style="justify-content:center;margin-top:14px;">
                <button class="btn filled" id="ioModeSchedule" onclick="ioEntrySetMode(0)">定时开关</button>
                <button class="btn tonal" id="ioModePulse" onclick="ioEntrySetMode(1)">瞬时脉冲</button>
                <button class="btn tonal" id="ioModeHb" onclick="ioEntrySetMode(2)">持久心跳</button>
            </div>
            <div class="modal-desc" id="ioModeHint" style="display:none;margin-top:8px;color:var(--warn);"></div>
            <div class="time-inputs" id="ioEntryTimeRow" style="margin-top:14px;">
                <input type="number" id="ioEntryHour" min="0" max="23" inputmode="numeric" placeholder="时"><span class="sep">:</span>
                <input type="number" id="ioEntryMinute" min="0" max="59" inputmode="numeric" placeholder="分">
            </div>
            <div class="row" style="justify-content:center;margin-top:14px;" id="ioActRow">
                <button class="btn tonal" id="ioActOn" onclick="ioEntrySetAction(true)">开启</button>
                <button class="btn tonal" id="ioActOff" onclick="ioEntrySetAction(false)">关闭</button>
            </div>
            <div id="ioPulseRow" style="display:none;">
                <div class="row" style="justify-content:center;margin-top:14px;">
                    <span class="row-label">脉冲时长</span>
                    <input type="number" id="ioPulseMs" min="10" max="3600000" inputmode="numeric" style="width:120px;text-align:center;" placeholder="毫秒">
                    <span class="row-label">毫秒</span>
                </div>
                <div class="modal-desc" style="margin-top:6px;">范围 10 ~ 3600000 毫秒（1 小时），到时后回到该序列的当前基础电平。</div>
            </div>
            <div id="ioHbRow" style="display:none;">
                <div class="row" style="justify-content:center;margin-top:14px;">
                    <span class="row-label">高电平</span>
                    <input type="number" id="ioHbHighMs" min="10" max="3600000" inputmode="numeric" style="width:120px;text-align:center;" placeholder="毫秒">
                    <span class="row-label">低电平</span>
                    <input type="number" id="ioHbLowMs" min="10" max="3600000" inputmode="numeric" style="width:120px;text-align:center;" placeholder="毫秒">
                </div>
                <div class="modal-desc" style="margin-top:6px;">各 10 ~ 3600000 毫秒（1 小时）。不需要设置时间：只要该序列启用就永久循环。持久心跳<b>在该序列内独占</b> —— 序列已有其他条目时不能添加它；添加之后该序列也不能再添加其他条目。</div>
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="ioEntryClose()">取消</button>
                <button class="btn filled" onclick="ioEntrySave()">保存</button>
            </div>
        </div>
    </div>

    <!-- IO 开发模式确认弹窗 -->
    <div class="modal-mask" id="ioDevModeModal" onclick="ioDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="ioDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许选中PWM占用引脚</span>
                <input type="checkbox" class="switch" id="ioAllowPwmSwitch">
            </div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许重复选中引脚</span>
                <input type="checkbox" class="switch" id="ioAllowDupSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="ioDevModeCancel()">取消</button>
                <button class="btn filled" onclick="ioDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- IO 删除确认弹窗 -->
    <div class="modal-mask" id="ioDelModal" onclick="ioDelCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>确认删除</h3>
                <button class="icon-btn" onclick="ioDelCancel()">✕</button>
            </div>
            <div class="modal-desc" id="ioDelDesc"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="ioDelCancel()">取消</button>
                <button class="btn danger" onclick="ioDelConfirm()">确认删除</button>
            </div>
        </div>
    </div>

    <!-- 中心面板弹窗（只读监控） -->
    <div class="modal-mask" id="centralModal" onclick="closeCentralPanel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>中心面板</h3>
                <button class="icon-btn" onclick="closeCentralPanel()">✕</button>
            </div>
            <div class="modal-desc">只读监控：上为 IO 引脚监控区，下为运行中的 PWM 与电平反转事件（保留近 50 条）。点击任意条目查看触发归因（模块 - 组 - 条目）。</div>
            <div class="central-section-label">IO 引脚监控区</div>
            <div class="central-grid" id="centralPins"></div>
            <div class="central-section-label">IO 引脚行为区</div>
            <div class="central-sub-label">正在运行的 PWM</div>
            <div class="central-events" id="centralPwm"></div>
            <div class="central-sub-label" style="display:flex;align-items:center;justify-content:space-between;">
                <span>电平反转事件</span>
                <button class="btn tonal" id="centralExpandBtn" style="display:none;padding:5px 12px;font-size:12px;" onclick="openCentralEvents()">展开</button>
            </div>
            <div class="central-events" id="centralEvents"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeCentralPanel()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 中心面板 触发归因详情弹窗 -->
    <div class="modal-mask" id="centralDetailModal" onclick="closeCentralDetail()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="centralDetailTitle">引脚触发归因</h3>
                <button class="icon-btn" onclick="closeCentralDetail()">✕</button>
            </div>
            <div class="modal-desc" id="centralDetailDesc"></div>
            <div class="central-detail-list" id="centralDetailList"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeCentralDetail()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 中心面板 全部反转事件弹窗 -->
    <div class="modal-mask" id="centralEventsModal" onclick="closeCentralEvents()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>电平反转事件（全部）</h3>
                <button class="icon-btn" onclick="closeCentralEvents()">✕</button>
            </div>
            <div class="modal-desc">保留近 50 条，最新在前。点击条目查看触发归因。</div>
            <div class="central-events central-events-all" id="centralEventsAll"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="closeCentralEvents()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 调度面板弹窗 -->
    <div class="modal-mask" id="ioPanelModal" onclick="closeIoPanel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>调度面板</h3>
            </div>
            <div class="modal-desc">点击引脚切换电平；PSRAM 与 PWM 通道引脚需开启开发模式。</div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="row" style="margin-bottom:10px;">
                <label class="switch-row" style="cursor:pointer;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="ioPanelDevModeSwitch" onchange="ioPanelDevModeToggle(this)">
                </label>
            </div>
            <div class="io-panel-grid" id="ioPanelList"></div>
            <div class="modal-actions">
                <button class="btn tonal" id="ioPanelAutoBtn" onclick="ioPanelAutoReset()">恢复全部自动</button>
                <button class="btn tonal" onclick="closeIoPanel()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 调度面板 二次确认弹窗 -->
    <div class="modal-mask" id="ioPanelConfirmModal" onclick="ioPanelConfirmCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>切换电平</h3>
                <button class="icon-btn" onclick="ioPanelConfirmCancel()">✕</button>
            </div>
            <div class="modal-desc" id="ioPanelConfirmDesc" style="color:#fff;"></div>
            <div class="modal-desc" style="margin-top:8px;">这是设定该引脚的「基准电平」，不是锁：脉冲/心跳会叠加在它之上、结束后回到它；定时开关到点、比较器/计数器输出也能覆盖它。要绝对固定请用「锁存」。此基准电平会跨重启保持（但总线引脚的不会）。</div>
            <div class="pin-warning" id="ioPanelConfirmWarning"></div>
            <div class="pin-warning" id="ioPanelLatchInfo" style="display:none;"></div>
            <div class="modal-actions">
                <button class="btn tonal" id="ioPanelLatchBtn" onclick="ioPanelLatchAction()">锁存</button>
                <!-- 单引脚恢复自动（替代原来的"取消"；关闭弹窗用右上角 ✕ 或点击遮罩） -->
                <button class="btn tonal" id="ioPanelConfirmAutoBtn" onclick="ioPanelConfirmAuto()">自动</button>
                <button class="btn filled" id="ioPanelConfirmDoBtn" onclick="ioPanelConfirmDo()">确认切换</button>
            </div>
        </div>
    </div>

    <!-- 锁存弹窗（最高权限） -->
    <div class="modal-mask" id="latchModal" onclick="latchClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>锁存电平</h3>
                <button class="icon-btn" onclick="latchClose()">✕</button>
            </div>
            <div class="modal-desc">锁存后该引脚电平被固定，序列器 / 比较器 / 计数器 / PWM 均无法修改，仅可回到此处解除。锁存不跨重启保存，重启后自动解除。</div>
            <div class="pin-warning" id="latchWarning"></div>
            <div class="row" style="justify-content:center;margin-top:12px;">
                <button class="btn filled" id="latchLvlHigh" onclick="latchSetLevel(1)">高电平</button>
                <button class="btn tonal" id="latchLvlLow" onclick="latchSetLevel(0)">低电平</button>
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="latchClose()">取消</button>
                <button class="btn filled" id="latchSaveBtn" onclick="latchConfirm()">确认锁存</button>
            </div>
        </div>
    </div>

    <!-- 调度面板 开发模式确认弹窗 -->
    <div class="modal-mask" id="ioPanelDevModeModal" onclick="ioPanelDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="ioPanelDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作异常或崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许切换 PWM 通道引脚</span>
                <input type="checkbox" class="switch" id="ioPanelAllowPwmSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">PWM 引脚切换会暂停该通道 PWM 输出，属高风险操作，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="ioPanelDevModeCancel()">取消</button>
                <button class="btn filled" onclick="ioPanelDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- 调度面板 恢复自动确认弹窗 -->
    <div class="modal-mask" id="ioPanelAutoResetModal" onclick="ioPanelAutoResetCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>恢复自动调度</h3>
                <button class="icon-btn" onclick="ioPanelAutoResetCancel()">✕</button>
            </div>
            <div class="modal-desc">确认取消所有引脚的手动基准，恢复为自动调度？</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="ioPanelAutoResetCancel()">取消</button>
                <button class="btn filled" onclick="ioPanelAutoResetConfirm()">确认恢复</button>
            </div>
        </div>
    </div>

    <!-- 比较器 添加/重命名弹窗 -->
    <div class="modal-mask" id="comparatorGateModal" onclick="comparatorGateClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="comparatorGateTitle">添加比较器任务</h3>
                <button class="icon-btn" onclick="comparatorGateClose()">✕</button>
            </div>
            <input type="text" id="comparatorGateName" placeholder="任务名（≤16 字符）" maxlength="16" style="width:100%;margin-bottom:12px;">
            <div class="row" style="flex-direction:column;align-items:stretch;gap:8px;margin-bottom:12px;">
                <span class="modal-desc" id="comparatorModeDesc"></span>
                <button class="btn filled" id="comparatorModeAnd" onclick="comparatorGateSetMode(0)">AND 与</button>
                <button class="btn tonal" id="comparatorModeOr" onclick="comparatorGateSetMode(1)">OR 或</button>
                <button class="btn tonal" id="comparatorModeNor" onclick="comparatorGateSetMode(2)">NOR 或非</button>
                <button class="btn tonal" id="comparatorModeNand" onclick="comparatorGateSetMode(3)">NAND 与非</button>
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="comparatorGateClose()">取消</button>
                <button class="btn filled" id="comparatorGateSaveBtn" onclick="comparatorGateSave()">添加</button>
            </div>
        </div>
    </div>

    <!-- 比较器 输入弹窗 -->
    <div class="modal-mask" id="comparatorInputModal" onclick="comparatorInputClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>添加输入条件</h3>
                <button class="icon-btn" onclick="comparatorInputClose()">✕</button>
            </div>
            <div class="modal-desc">选择输入来源，并设定满足条件所需达到的电平。</div>
            <div class="row" style="margin-bottom:10px;">
                <button class="btn filled" id="comparatorSrcIo" onclick="comparatorInputSetSource(0)">引用 序列器</button>
                <button class="btn tonal" id="comparatorSrcCounter" onclick="comparatorInputSetSource(1)">引用 计数器</button>
                <button class="btn tonal" id="comparatorSrcExternal" onclick="comparatorInputSetSource(2)">外部输入</button>
            </div>
            <div class="pin-warning" id="comparatorInputWarning"></div>
            <div class="pin-grid" id="comparatorInputGrid"></div>
            <div class="row" style="justify-content:center;margin-top:12px;">
                <span class="row-label">满足条件：</span>
                <button class="btn filled" id="comparatorInputLvlHigh" onclick="comparatorInputSetLevel(1)">高电平</button>
                <button class="btn tonal" id="comparatorInputLvlLow" onclick="comparatorInputSetLevel(0)">低电平</button>
            </div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="comparatorDevModeSwitch" onchange="comparatorDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="comparatorInputClose()">取消</button>
                <button class="btn filled" id="comparatorInputSaveBtn" onclick="comparatorInputSave()">保存</button>
            </div>
        </div>
    </div>

    <!-- 比较器 开发模式确认弹窗 -->
    <div class="modal-mask" id="comparatorDevModeModal" onclick="comparatorDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="comparatorDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许选中PWM占用引脚</span>
                <input type="checkbox" class="switch" id="comparatorAllowPwmSwitch">
            </div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许重复选中引脚</span>
                <input type="checkbox" class="switch" id="comparatorAllowDupSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="comparatorDevModeCancel()">取消</button>
                <button class="btn filled" onclick="comparatorDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- 比较器 输出弹窗 -->
    <div class="modal-mask" id="comparatorOutputModal" onclick="comparatorOutputClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>添加输出动作</h3>
                <button class="icon-btn" onclick="comparatorOutputClose()">✕</button>
            </div>
            <div class="modal-desc">选择输出引脚、源IO电平（复位电平）与行为电平（门逻辑满足时输出）。</div>
            <div class="pin-warning" id="comparatorOutputWarning"></div>
            <div class="pin-grid" id="comparatorOutputGrid"></div>
            <div class="row" style="justify-content:center;margin-top:12px;">
                <span class="row-label">源IO电平</span>
                <button class="btn tonal" id="comparatorSrcLvlLow" onclick="comparatorOutputSetSourceLevel(0)">低电平</button>
                <button class="btn tonal" id="comparatorSrcLvlHigh" onclick="comparatorOutputSetSourceLevel(1)">高电平</button>
            </div>
            <div class="row" style="justify-content:center;margin-top:8px;">
                <span class="row-label">行为电平</span>
                <button class="btn tonal" id="comparatorActLvlLow" onclick="comparatorOutputSetActionLevel(0)">低电平</button>
                <button class="btn filled" id="comparatorActLvlHigh" onclick="comparatorOutputSetActionLevel(1)">高电平</button>
            </div>
            <div class="row" style="margin-top:10px;">
                <button class="btn tonal" style="padding:5px 12px;font-size:12px;" onclick="comparatorOutputHelpOpen()">详细说明</button>
            </div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="comparatorOutputDevModeSwitch" onchange="comparatorOutputDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="comparatorOutputClose()">取消</button>
                <button class="btn filled" id="comparatorOutputSaveBtn" onclick="comparatorOutputSave()">保存</button>
            </div>
        </div>
    </div>

    <!-- 比较器 输出电平详细说明弹窗 -->
    <div class="modal-mask" id="comparatorOutputHelpModal" onclick="comparatorOutputHelpClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>输出电平说明</h3>
                <button class="icon-btn" onclick="comparatorOutputHelpClose()">✕</button>
            </div>
            <div class="modal-desc" style="line-height:1.9;">
                <div style="margin-bottom:12px;"><b style="color:var(--primary);">源IO电平</b>：门逻辑不满足时引脚恢复到的电平（如保持默认，请查看调度面板中该引脚的现有状态）。</div>
                <div style="margin-bottom:12px;"><b style="color:var(--primary);">行为电平</b>：门逻辑满足时切换到的电平；左半条件不足时视为不满足，输出源IO电平。</div>
                <div><b style="color:var(--primary);">门逻辑</b>决定“满足”的判定：与 = 条件全部满足，或 = 任一满足，或非 = 全部不满足，与非 = 任一不满足（或 / 与非至少需 2 个条件）。</div>
            </div>
            <div class="modal-actions">
                <button class="btn filled" onclick="comparatorOutputHelpClose()">我知道了</button>
            </div>
        </div>
    </div>

    <!-- 比较器 输出开发模式确认弹窗 -->
    <div class="modal-mask" id="comparatorOutputDevModeModal" onclick="comparatorOutputDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="comparatorOutputDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许选中PWM占用引脚</span>
                <input type="checkbox" class="switch" id="comparatorOutputAllowPwmSwitch">
            </div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许重复选中引脚</span>
                <input type="checkbox" class="switch" id="comparatorOutputAllowDupSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="comparatorOutputDevModeCancel()">取消</button>
                <button class="btn filled" onclick="comparatorOutputDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- 比较器 删除确认弹窗 -->
    <div class="modal-mask" id="comparatorDelModal" onclick="comparatorDelCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>确认删除</h3>
                <button class="icon-btn" onclick="comparatorDelCancel()">✕</button>
            </div>
            <div class="modal-desc" id="comparatorDelDesc"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="comparatorDelCancel()">取消</button>
                <button class="btn danger" onclick="comparatorDelConfirm()">确认删除</button>
            </div>
        </div>
    </div>

    <!-- 计数器 计量面板弹窗 -->
    <div class="modal-mask" id="counterPanelModal" onclick="closeCounterPanel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>计量面板</h3>
                <button class="icon-btn" onclick="closeCounterPanel()">✕</button>
            </div>
            <div class="modal-desc">各 GPIO 引脚电平切变累计次数（高↔低 每次反转计 1 次）。</div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="row" style="margin-bottom:10px;">
                <label class="switch-row" style="cursor:pointer;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="counterPanelDevModeSwitch" onchange="counterPanelDevModeToggle(this)">
                </label>
            </div>
            <div class="io-panel-grid" id="counterPanelList"></div>
            <div class="modal-actions">
                <button class="btn danger" onclick="counterClear()">全部清零</button>
                <button class="btn tonal" onclick="closeCounterPanel()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 计数器 添加/重命名任务弹窗 -->
    <div class="modal-mask" id="counterTaskModal" onclick="counterTaskClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="counterTaskTitle">添加计数器任务</h3>
                <button class="icon-btn" onclick="counterTaskClose()">✕</button>
            </div>
            <input type="text" id="counterTaskName" placeholder="任务名（≤16 字符）" maxlength="16" style="width:100%;margin-bottom:12px;">
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterTaskClose()">取消</button>
                <button class="btn filled" id="counterTaskSaveBtn" onclick="counterTaskSave()">添加</button>
            </div>
        </div>
    </div>

    <!-- 计数器 触发源选择弹窗 -->
    <div class="modal-mask" id="counterTriggerModal" onclick="counterTriggerClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>设置触发源</h3>
                <button class="icon-btn" onclick="counterTriggerClose()">✕</button>
            </div>
            <div class="modal-desc">选择一个引脚作为触发源，并设置反转次数阈值。</div>
            <div class="pin-warning" id="counterTriggerWarning"></div>
            <div class="pin-grid" id="counterTriggerGrid"></div>
            <div class="row" style="justify-content:center;margin-top:12px;">
                <span class="row-label">反转</span>
                <input type="number" id="counterThreshold" min="1" max="1000000" value="1" style="width:90px;">
                <span class="row-label">次触发</span>
            </div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="counterTriggerDevModeSwitch" onchange="counterTriggerDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="counterTriggerClose()">取消</button>
                <button class="btn filled" id="counterTriggerSaveBtn" onclick="counterTriggerSave()">保存</button>
            </div>
        </div>
    </div>

    <!-- 计数器 触发源开发模式确认弹窗 -->
    <div class="modal-mask" id="counterTriggerDevModeModal" onclick="counterTriggerDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="counterTriggerDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许选中PWM占用引脚</span>
                <input type="checkbox" class="switch" id="counterTriggerAllowPwmSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterTriggerDevModeCancel()">取消</button>
                <button class="btn filled" onclick="counterTriggerDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- 计数器 输出动作弹窗 -->
    <div class="modal-mask" id="counterActionModal" onclick="counterActionClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3 id="counterActionTitle">添加输出动作</h3>
                <button class="icon-btn" onclick="counterActionClose()">✕</button>
            </div>
            <div class="modal-desc">选择输出引脚、电平与保持方式。</div>
            <div class="pin-warning" id="counterActionWarning"></div>
            <div class="pin-grid" id="counterActionGrid"></div>
            <div class="row" style="justify-content:center;margin-top:12px;">
                <button class="btn filled" id="counterLvlHigh" onclick="counterActionSetLevel(1)">高电平</button>
                <button class="btn tonal" id="counterLvlLow" onclick="counterActionSetLevel(0)">低电平</button>
            </div>
            <div class="row" style="justify-content:center;margin-top:12px;">
                <button class="btn filled" id="counterHoldKeep" onclick="counterActionSetHold(0)">修改</button>
                <button class="btn tonal" id="counterHoldAutoOff" onclick="counterActionSetHold(1)">锁存</button>
            </div>
            <div class="row" id="counterHoldTimeRow" style="justify-content:center;margin-top:12px;display:none;">
                <span class="row-label">锁存时长</span>
                <input type="number" id="counterHoldMin" min="0" max="10079" value="0" style="width:80px;"><span class="sep">分</span>
                <input type="number" id="counterHoldSec" min="0" max="60" value="0" style="width:70px;"><span class="sep">秒</span>
            </div>
            <div class="row" style="margin-top:10px;">
                <button class="btn tonal" style="padding:5px 12px;font-size:12px;" onclick="counterHoldHelp()">详细说明</button>
            </div>
            <div class="modal-desc" style="color:var(--warn);">开发模式里的引脚不会保存到 nvs，如果您成功复用，请前往（配置备份与恢复）独立导出</div>
            <div class="modal-actions">
                <label class="switch-row" style="cursor:pointer;margin-right:auto;">
                    <span class="row-label">开发模式</span>
                    <input type="checkbox" class="switch" id="counterActionDevModeSwitch" onchange="counterActionDevModeToggle(this)">
                </label>
                <button class="btn tonal" onclick="counterActionClose()">取消</button>
                <button class="btn filled" id="counterActionSaveBtn" onclick="counterActionSave()">保存</button>
            </div>
        </div>
    </div>

    <!-- 计数器 保持方式详细说明弹窗 -->
    <div class="modal-mask" id="counterHoldHelpModal" onclick="counterHoldHelpClose()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>保持方式说明</h3>
                <button class="icon-btn" onclick="counterHoldHelpClose()">✕</button>
            </div>
            <div class="modal-desc" style="line-height:1.9;">
                <div style="margin-bottom:12px;"><b style="color:var(--primary);">修改</b>：触发后输出设定的电平并一直保持（不夺权），若该引脚被其他行为覆盖/修改，则跟随。</div>
                <div style="margin-bottom:12px;"><b style="color:var(--primary);">锁存</b>：触发后把电平锁存一段时间（需填写时长，最长 10079 分 60 秒）。该锁存<b>优先级低于调度面板的锁存</b>（调度面板锁存优先），且<b>不可永久锁存</b>。</div>
                <div style="color:var(--warn);">如需永久锁存某个引脚，请前往「调度面板」使用锁存功能。</div>
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterHoldHelpClose()">关闭</button>
            </div>
        </div>
    </div>

    <!-- 计数器 动作开发模式确认弹窗 -->
    <div class="modal-mask" id="counterActionDevModeModal" onclick="counterActionDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="counterActionDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">开发模式将开放被禁用的 Flash/PSRAM 引脚（适用于芯片识别错误等场景），可能导致工作崩溃。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许选中PWM占用引脚</span>
                <input type="checkbox" class="switch" id="counterActionAllowPwmSwitch">
            </div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">允许重复选中引脚</span>
                <input type="checkbox" class="switch" id="counterActionAllowDupSwitch">
            </div>
            <div class="modal-desc" style="color:var(--warn);">专业选项：开启上述功能风险较高，请按需开启。</div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterActionDevModeCancel()">取消</button>
                <button class="btn filled" onclick="counterActionDevModeConfirm()">我已知晓</button>
            </div>
        </div>
    </div>

    <!-- 计数器 删除确认弹窗 -->
    <div class="modal-mask" id="counterDelModal" onclick="counterDelCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>确认删除</h3>
                <button class="icon-btn" onclick="counterDelCancel()">✕</button>
            </div>
            <div class="modal-desc" id="counterDelDesc"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterDelCancel()">取消</button>
                <button class="btn danger" onclick="counterDelConfirm()">确认删除</button>
            </div>
        </div>
    </div>

    <!-- 计数器 计量面板开发模式确认弹窗 -->
    <div class="modal-mask" id="counterPanelDevModeModal" onclick="counterPanelDevModeCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>开发模式</h3>
                <button class="icon-btn" onclick="counterPanelDevModeCancel()">✕</button>
            </div>
            <div class="modal-desc" style="color:var(--danger);">默认不统计 PWM 与 Flash/PSRAM 引脚的切变，开启后可能产生噪声计数。</div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">统计 PWM 引脚</span>
                <input type="checkbox" class="switch" id="counterPanelCountPwmSwitch">
            </div>
            <div class="row" style="justify-content:space-between;margin:10px 0;">
                <span class="row-label">统计 Flash/PSRAM 引脚</span>
                <input type="checkbox" class="switch" id="counterPanelCountFlashSwitch">
            </div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterPanelDevModeCancel()">取消</button>
                <button class="btn filled" onclick="counterPanelDevModeConfirm()">确定</button>
            </div>
        </div>
    </div>

    <!-- 计数器 计量面板 清除单引脚计数确认弹窗 -->
    <div class="modal-mask" id="counterClearPinModal" onclick="counterClearPinCancel()">
        <div class="modal" onclick="event.stopPropagation()">
            <div class="modal-head">
                <h3>清除计数</h3>
                <button class="icon-btn" onclick="counterClearPinCancel()">✕</button>
            </div>
            <div class="modal-desc" id="counterClearPinDesc"></div>
            <div class="modal-actions">
                <button class="btn tonal" onclick="counterClearPinCancel()">取消</button>
                <button class="btn danger" onclick="counterClearPinConfirm()">确认清除</button>
            </div>
        </div>
    </div>

    <script>)HTML";
static const char PAGE_HTML_FOOT_B[] PROGMEM = R"HTML(</script>
</body>
</html>
)HTML";

#endif
